/** The HTTP surface, on loopback with a fake provider transport. */

import assert from 'node:assert/strict';
import { request as httpRequest } from 'node:http';
import type { AddressInfo } from 'node:net';
import { after, before, describe, it } from 'node:test';
import type { Server } from 'node:http';

import { createLogger } from '../src/logger.js';
import { createWorkerServer, tokenMatches } from '../src/server.js';
import type { ProviderRequest, ProviderResult, ProviderTransport } from '../src/transport.js';
import { deliverBody, GENERATION_ID, NOTIFICATION_ID, RFC8291_AUTH, RFC8291_UA_PUBLIC, TEST_ENDPOINT, testVapid, WORKER_TOKEN } from './support.js';

const accepted: ProviderResult = { outcome: 'accepted', httpStatus: 201, retryAfterSeconds: null };

interface Reply { status: number; text: string; json: Record<string, unknown> }

function call(port: number, path: string, options: { method?: string; token?: string | null; body?: string } = {}): Promise<Reply> {
  return new Promise((resolve, reject) => {
    const body = options.body ?? '';
    const headers: Record<string, string | number> = { 'Content-Length': Buffer.byteLength(body) };
    const token = options.token === undefined ? WORKER_TOKEN : options.token;
    if (token !== null) headers['Authorization'] = `Bearer ${token}`;
    const req = httpRequest({ host: '127.0.0.1', port, path, method: options.method ?? 'POST', headers, agent: false }, (res) => {
      let text = '';
      res.on('data', (c: Buffer) => { text += c.toString(); });
      res.on('end', () => {
        let json: Record<string, unknown> = {};
        try { json = JSON.parse(text) as Record<string, unknown>; } catch { /* asserted by the caller */ }
        resolve({ status: res.statusCode ?? 0, text, json });
      });
    });
    req.on('error', reject);
    req.end(body);
  });
}

interface Harness { server: Server; port: number; logs: () => string; close: () => Promise<void> }

async function start(transport: ProviderTransport, options: { deadlineMs?: number } = {}): Promise<Harness> {
  const lines: string[] = [];
  const logger = createLogger('push-worker-test', '0.0.0', (line) => { lines.push(line); });
  const server = createWorkerServer({
    token: WORKER_TOKEN,
    version: '0.0.0',
    deps: { transport, vapid: testVapid(), now: Date.now },
    ...options,
  }, logger);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    server,
    port: (server.address() as AddressInfo).port,
    logs: () => lines.join('\n'),
    close: () => new Promise((resolve) => { server.closeAllConnections(); server.close(() => resolve()); }),
  };
}

const okBody = (): string => JSON.stringify(deliverBody());

describe('authentication', () => {
  it('compares tokens exactly', () => {
    assert.equal(tokenMatches(`Bearer ${WORKER_TOKEN}`, WORKER_TOKEN), true);
    for (const header of [undefined, '', WORKER_TOKEN, `Basic ${WORKER_TOKEN}`, `Bearer ${WORKER_TOKEN}x`, `bearer ${WORKER_TOKEN}`, 'Bearer ']) {
      assert.equal(tokenMatches(header, WORKER_TOKEN), false, String(header));
    }
    assert.equal(tokenMatches('Bearer ', ''), false);
  });
});

describe('POST /v1/deliver', () => {
  let h: Harness;
  const sent: ProviderRequest[] = [];
  before(async () => {
    h = await start({ post: (request) => { sent.push(request); return Promise.resolve({ outcome: 'transient', httpStatus: 503, retryAfterSeconds: 7 }); } });
  });
  after(async () => { await h.close(); });

  it('returns the job and the result', async () => {
    const reply = await call(h.port, '/v1/deliver', { body: okBody() });
    assert.equal(reply.status, 200);
    assert.deepEqual(reply.json, {
      job: { notification_id: NOTIFICATION_ID, generation_id: GENERATION_ID, attempt_no: 1 },
      outcome: 'transient',
      http_status: 503,
      retry_after_s: 7,
    });
  });

  it('refuses a missing or wrong token before anything else, without echoing', async () => {
    const count = sent.length;
    for (const token of [null, 'wrong-token']) {
      const reply = await call(h.port, '/v1/deliver', { token, body: okBody() });
      assert.equal(reply.status, 401);
      assert.deepEqual(reply.json, { error: { code: 'WORKER_AUTH_FAILED' } });
    }
    assert.equal(sent.length, count);
  });

  it('bounds the body by declaration and by actual size', async () => {
    const big = await call(h.port, '/v1/deliver', { body: 'x'.repeat(4097) });
    assert.equal(big.status, 413);
    assert.deepEqual(big.json, { error: { code: 'BODY_TOO_LARGE' } });
    const chunked = await new Promise<number>((resolve, reject) => {
      const req = httpRequest({ host: '127.0.0.1', port: h.port, path: '/v1/deliver', method: 'POST', agent: false,
        headers: { Authorization: `Bearer ${WORKER_TOKEN}`, 'Transfer-Encoding': 'chunked' } }, (res) => { res.resume(); resolve(res.statusCode ?? 0); });
      req.on('error', reject);
      req.write('x'.repeat(3000));
      setTimeout(() => { req.write('x'.repeat(3000)); req.end(); }, 20);
    });
    assert.equal(chunked, 413);
  });

  it('answers invalid input with a fixed body that echoes nothing', async () => {
    const secret = 'https://evil.example/LEAK-MARKER';
    for (const body of ['not json LEAK-MARKER', JSON.stringify({ ...deliverBody(), subscription: { endpoint: secret, keys: { p256dh: RFC8291_UA_PUBLIC, auth: RFC8291_AUTH } } })]) {
      const reply = await call(h.port, '/v1/deliver', { body });
      assert.equal(reply.status, 400);
      assert.equal(reply.text, JSON.stringify({ error: { code: 'INVALID_REQUEST' } }));
    }
  });

  it('serves only its routes', async () => {
    assert.equal((await call(h.port, '/deliver', { body: okBody() })).status, 404);
    assert.equal((await call(h.port, '/v1/deliver', { method: 'GET' })).status, 404);
    const health = await call(h.port, '/health', { method: 'GET', token: null });
    assert.equal(health.status, 200);
    assert.equal(health.json['status'], 'ok');
    assert.ok(!health.text.includes(WORKER_TOKEN));
  });
});

describe('limits', () => {
  it('allows four at a time and answers the fifth busy, then recovers', async () => {
    const release: Array<() => void> = [];
    let begun = 0;
    const h = await start({ post: () => new Promise((resolve) => { begun += 1; release.push(() => resolve(accepted)); }) });
    try {
      const held = [1, 2, 3, 4].map(() => call(h.port, '/v1/deliver', { body: okBody() }));
      while (begun < 4) await new Promise((resolve) => setTimeout(resolve, 5));
      const fifth = await call(h.port, '/v1/deliver', { body: okBody() });
      assert.equal(fifth.status, 503);
      assert.deepEqual(fifth.json, { error: { code: 'BUSY' } });
      assert.equal(begun, 4);
      for (const r of release) r();
      for (const reply of await Promise.all(held)) assert.equal(reply.status, 200);
      assert.equal((await new Promise<Reply>((resolve, reject) => {
        const next = call(h.port, '/v1/deliver', { body: okBody() });
        const pump = setInterval(() => { for (const r of release) r(); }, 5);
        next.then((v) => { clearInterval(pump); resolve(v); }, reject);
      })).status, 200);
    } finally {
      for (const r of release) r();
      await h.close();
    }
  });

  it('checks authentication before the semaphore', async () => {
    const release: Array<() => void> = [];
    let begun = 0;
    const h = await start({ post: () => new Promise((resolve) => { begun += 1; release.push(() => resolve(accepted)); }) });
    try {
      const held = [1, 2, 3, 4].map(() => call(h.port, '/v1/deliver', { body: okBody() }));
      while (begun < 4) await new Promise((resolve) => setTimeout(resolve, 5));
      assert.equal((await call(h.port, '/v1/deliver', { token: 'wrong', body: okBody() })).status, 401);
      for (const r of release) r();
      await Promise.all(held);
    } finally {
      await h.close();
    }
  });

  it('gives one absolute deadline to the whole operation, and answers once', async () => {
    let aborted = 0;
    const h = await start({
      post: (_request, signal) => new Promise((resolve) => {
        signal.addEventListener('abort', () => {
          aborted += 1;
          resolve({ outcome: 'unknown', httpStatus: null, retryAfterSeconds: null });
          resolve(accepted);
        });
      }),
    }, { deadlineMs: 100 });
    try {
      const started = Date.now();
      const reply = await call(h.port, '/v1/deliver', { body: okBody() });
      assert.equal(reply.status, 200);
      assert.equal(reply.json['outcome'], 'unknown');
      assert.equal(aborted, 1);
      assert.ok(Date.now() - started < 2000);
    } finally {
      await h.close();
    }
  });

  it('releases its slot when the transport throws, answering with fixed text', async () => {
    let calls = 0;
    const h = await start({ post: () => { calls += 1; return Promise.reject(new Error(`LEAK ${TEST_ENDPOINT}`)); } });
    try {
      for (let i = 0; i < 6; i += 1) {
        const reply = await call(h.port, '/v1/deliver', { body: okBody() });
        assert.equal(reply.status, 500);
        assert.equal(reply.text, JSON.stringify({ error: { code: 'INTERNAL_ERROR' } }));
      }
      assert.equal(calls, 6);
      assert.ok(!h.logs().includes('LEAK'));
    } finally {
      await h.close();
    }
  });
});

describe('logging', () => {
  it('holds no endpoint, key, token or payload', async () => {
    const h = await start({ post: () => Promise.resolve(accepted) });
    try {
      const body = deliverBody();
      await call(h.port, '/v1/deliver', { body: JSON.stringify(body) });
      await call(h.port, '/v1/deliver', { token: 'wrong-token-value', body: JSON.stringify(body) });
      const logs = h.logs();
      assert.ok(logs.includes('delivery finished'));
      assert.ok(logs.includes(GENERATION_ID));
      for (const secret of [TEST_ENDPOINT, 'fcm/send', RFC8291_UA_PUBLIC, RFC8291_AUTH, WORKER_TOKEN, 'wrong-token-value', 'm'.repeat(22), 'message.created']) {
        assert.ok(!logs.includes(secret), secret);
      }
    } finally {
      await h.close();
    }
  });
});
