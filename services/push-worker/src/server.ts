/**
 * The worker's HTTP surface: a stateless authenticated transport.
 *
 *   GET  /health       liveness and whether transport is configured; no secrets
 *   GET  /boundary     the responsibility boundary
 *   POST /v1/deliver   one delivery already decided by the C++ process
 *
 * /v1/deliver: constant-time bearer authentication first, then the semaphore (excess gets 503 busy),
 * bounded body, strict validation, one absolute deadline over the whole operation. A provider result
 * is always HTTP 200 with the job echoed, so C++ can correlate it. Error bodies are fixed strings and
 * never echo input or exception text. Nothing about the endpoint, keys, token or payload is logged.
 */

import { createHash, timingSafeEqual } from 'node:crypto';
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';

import { deliver, infoOf, parseDeliverRequest, type DeliverDeps, type DeliverJob, type DeliverResult } from './deliver.js';
import type { Logger } from './logger.js';
import { assertAllowedOperation, describeResponsibilityBoundary } from './policy.js';

export const MAX_BODY_BYTES = 4 * 1024;
export const MAX_CONCURRENT = 4;
export const DEADLINE_MS = 10_000;

export interface WorkerOptions {
  readonly token: string;
  readonly version: string;
  readonly deps: DeliverDeps;
  readonly deadlineMs?: number;
  readonly maxConcurrent?: number;
}

function sendJson(res: ServerResponse, status: number, payload: unknown, close = false): void {
  if (res.headersSent || res.destroyed) return;
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store, no-cache, must-revalidate, private',
    'X-Content-Type-Options': 'nosniff',
    ...(close ? { Connection: 'close' } : {}),
  });
  res.end(body);
}

function fail(res: ServerResponse, status: number, code: string, close = false): void {
  sendJson(res, status, { error: { code } }, close);
}

/** Answers a request whose body will not be read. The connection ends after the response. */
function refuse(req: IncomingMessage, res: ServerResponse, status: number, code: string): void {
  fail(res, status, code, true);
  req.resume();
}

const digest = (value: string): Buffer => createHash('sha256').update(value).digest();

/** Constant time over fixed-length digests, so the token's length is not observable either. */
export function tokenMatches(header: string | undefined, token: string): boolean {
  const presented = header !== undefined && header.startsWith('Bearer ') ? header.slice('Bearer '.length) : '';
  const ok = timingSafeEqual(digest(presented), digest(token));
  return ok && token.length > 0;
}

class BodyTooLarge extends Error {}

function readBody(req: IncomingMessage, signal: AbortSignal): Promise<string> {
  return new Promise((resolve, reject) => {
    const declared = Number(req.headers['content-length'] ?? '0');
    if (declared > MAX_BODY_BYTES) {
      reject(new BodyTooLarge());
      return;
    }
    const chunks: Buffer[] = [];
    let total = 0;
    const onAbort = (): void => reject(new Error('deadline'));
    signal.addEventListener('abort', onAbort, { once: true });
    req.on('data', (chunk: Buffer) => {
      total += chunk.length;
      if (total > MAX_BODY_BYTES) {
        reject(new BodyTooLarge());
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      signal.removeEventListener('abort', onAbort);
      resolve(Buffer.concat(chunks).toString('utf8'));
    });
    req.on('error', () => reject(new Error('read')));
  });
}

export function createWorkerServer(options: WorkerOptions, logger: Logger): Server {
  const deadlineMs = options.deadlineMs ?? DEADLINE_MS;
  const maxConcurrent = options.maxConcurrent ?? MAX_CONCURRENT;
  let inFlight = 0;

  const handleDeliver = async (req: IncomingMessage, res: ServerResponse): Promise<void> => {
    assertAllowedOperation('push.deliver');
    const header = req.headers['authorization'];
    if (!tokenMatches(Array.isArray(header) ? header[0] : header, options.token)) {
      refuse(req, res, 401, 'WORKER_AUTH_FAILED');
      return;
    }
    if (inFlight >= maxConcurrent) {
      refuse(req, res, 503, 'BUSY');
      return;
    }
    inFlight += 1;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), deadlineMs);
    try {
      let raw: string;
      try {
        raw = await readBody(req, controller.signal);
      } catch (error) {
        if (error instanceof BodyTooLarge) refuse(req, res, 413, 'BODY_TOO_LARGE');
        else refuse(req, res, 408, 'BODY_NOT_READ');
        return;
      }
      let parsed: unknown;
      try {
        parsed = JSON.parse(raw);
      } catch {
        fail(res, 400, 'INVALID_REQUEST');
        return;
      }
      const request = parseDeliverRequest(parsed);
      if (request === null) {
        fail(res, 400, 'INVALID_REQUEST');
        return;
      }
      const job: DeliverJob = request.job;
      const result: DeliverResult = await deliver(request, options.deps, controller.signal);
      sendJson(res, 200, {
        job,
        outcome: result.outcome,
        http_status: result.httpStatus,
        retry_after_s: result.retryAfterSeconds,
      });
      logger.info('delivery finished', {
        generation: job.generation_id,
        attempt: job.attempt_no,
        provider: infoOf(request).provider,
        outcome: result.outcome,
        status: result.httpStatus,
      });
    } finally {
      clearTimeout(timer);
      inFlight -= 1;
    }
  };

  const handler = async (req: IncomingMessage, res: ServerResponse): Promise<void> => {
    const method = req.method ?? 'GET';
    const path = (req.url ?? '/').split('?', 1)[0];
    if (method === 'GET' && path === '/health') {
      sendJson(res, 200, { status: 'ok', component: 'push-worker', version: options.version, transport: 'configured' });
      return;
    }
    if (method === 'GET' && path === '/boundary') {
      sendJson(res, 200, describeResponsibilityBoundary());
      return;
    }
    if (method === 'POST' && path === '/v1/deliver') {
      await handleDeliver(req, res);
      return;
    }
    fail(res, 404, 'NOT_FOUND');
  };

  const server = createServer({ maxHeaderSize: 8 * 1024 }, (req, res) => {
    handler(req, res).catch(() => {
      // Fixed text only: an exception may hold request data. If the delivery may have begun, C++
      // treats this 500 as an unknown result.
      logger.error('request handling failed', {});
      fail(res, 500, 'INTERNAL_ERROR');
    });
  });
  server.headersTimeout = 5_000;
  server.requestTimeout = deadlineMs + 2_000;
  server.keepAliveTimeout = 5_000;
  return server;
}
