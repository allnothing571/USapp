/**
 * The controlled HTTPS request to a push provider (docs/phase2-web-push.md Section 8.2).
 *
 * - A NEW https.Agent per request, destroyed when the request settles: an empty pool, so no socket
 *   can be reused across sends. keepAlive:false alone is not relied on as proof. No proxyEnv.
 * - DNS through a controlled lookup returning public addresses only; the actual peer is checked
 *   again on secureConnect, and the body is not written until that check passes.
 * - Normal certificate and hostname verification with SNI.
 * - No redirects (one request; a 3xx is a result). The body is never read: the result comes from
 *   the status line, and the response is destroyed at once, so a large response costs nothing.
 * - The caller's AbortSignal is the absolute deadline. The socket timeout option is not used.
 *
 * Tests inject stubs through createProviderTransport's argument. index.ts passes none, and there is
 * no environment or runtime switch that reaches these options.
 */

import { lookup as dnsLookup } from 'node:dns';
import https from 'node:https';
import type { LookupAddress } from 'node:dns';
import type { TLSSocket } from 'node:tls';

import { isPublicAddress, parseEndpoint } from './endpoint.js';

export type ProviderOutcome =
  | 'accepted' | 'gone' | 'rejected' | 'transient' | 'unknown' | 'provider_auth_failed' | 'transport_unavailable';

export interface ProviderResult {
  readonly outcome: ProviderOutcome;
  readonly httpStatus: number | null;
  readonly retryAfterSeconds: number | null;
}

export interface ProviderRequest {
  readonly endpoint: string;
  readonly headers: Record<string, string | number>;
  readonly body: Buffer;
}

export interface ProviderTransport {
  post(request: ProviderRequest, signal: AbortSignal): Promise<ProviderResult>;
}

export type Resolver = (hostname: string) => Promise<readonly LookupAddress[]>;

export interface TransportOptions {
  readonly resolve?: Resolver;
  readonly isAddressAllowed?: (address: string) => boolean;
  readonly port?: number;
  readonly ca?: string;
  readonly onAgent?: (agent: https.Agent) => void;
}

const MAX_RESPONSE_HEADER_BYTES = 8 * 1024;

const systemResolver: Resolver = (hostname) =>
  new Promise((resolve, reject) => {
    dnsLookup(hostname, { all: true, verbatim: true }, (error, addresses) => {
      if (error) reject(error);
      else resolve(addresses);
    });
  });

/** Status mapping (Section 6.3). A 3xx is never followed. */
export function classifyStatus(status: number): ProviderOutcome {
  if (status >= 200 && status < 300) return 'accepted';
  if (status === 404 || status === 410) return 'gone';
  if (status === 401 || status === 403) return 'provider_auth_failed';
  if (status === 408 || status === 425 || status === 429 || status >= 500) return 'transient';
  if (status >= 300 && status < 500) return 'rejected';
  return 'unknown';
}

/** Delta-seconds or an HTTP-date, bounded to a day. Anything else is ignored. */
export function parseRetryAfter(value: string | undefined, nowMs: number): number | null {
  if (value === undefined) return null;
  const text = value.trim();
  let seconds: number;
  if (/^\d{1,10}$/.test(text)) seconds = Number(text);
  else {
    // Date.parse alone is too forgiving: it reads "-5" as a date.
    if (!/^[A-Za-z]{3,9},? /.test(text)) return null;
    const at = Date.parse(text);
    if (Number.isNaN(at)) return null;
    seconds = Math.ceil((at - nowMs) / 1000);
  }
  if (seconds < 0) return 0;
  return Math.min(seconds, 86400);
}

export function createProviderTransport(options: TransportOptions = {}): ProviderTransport {
  const resolve = options.resolve ?? systemResolver;
  const isAllowed = options.isAddressAllowed ?? isPublicAddress;

  return {
    post(request, signal) {
      return new Promise<ProviderResult>((done) => {
        const target = parseEndpoint(request.endpoint);
        // Nothing was sent: these are not provider facts.
        const notSent: ProviderResult = { outcome: 'transport_unavailable', httpStatus: null, retryAfterSeconds: null };
        if (target === null || signal.aborted) {
          done(notSent);
          return;
        }
        const agent = new https.Agent({ keepAlive: false, maxSockets: 1 });
        options.onAgent?.(agent);
        let settled = false;
        let bodyWritten = false;
        const settle = (result: ProviderResult): void => {
          if (settled) return;
          settled = true;
          agent.destroy();
          done(result);
        };
        const lookup = (
          hostname: string,
          lookupOptions: { all?: boolean },
          callback: (error: NodeJS.ErrnoException | null, address: string | LookupAddress[], family?: number) => void,
        ): void => {
          resolve(hostname).then(
            (addresses) => {
              const allowed = addresses.filter((a) => isAllowed(a.address));
              const first = allowed[0];
              if (first === undefined) {
                callback(Object.assign(new Error('address refused'), { code: 'EADDRREFUSED' }), '', 0);
              } else if (lookupOptions.all) {
                callback(null, allowed);
              } else {
                callback(null, first.address, first.family);
              }
            },
            () => callback(Object.assign(new Error('lookup failed'), { code: 'ENOTFOUND' }), '', 0),
          );
        };
        const req = https.request({
          protocol: 'https:',
          hostname: target.hostname,
          port: options.port ?? 443,
          path: target.path,
          method: 'POST',
          headers: request.headers,
          agent,
          lookup: lookup as never,
          servername: target.hostname,
          rejectUnauthorized: true,
          maxHeaderSize: MAX_RESPONSE_HEADER_BYTES,
          signal,
          ...(options.ca === undefined ? {} : { ca: options.ca }),
        });
        req.on('socket', (socket) => {
          const tls = socket as TLSSocket;
          tls.once('secureConnect', () => {
            const peer = tls.remoteAddress ?? '';
            if (!isAllowed(peer)) {
              req.destroy(new Error('peer refused'));
              return;
            }
            // Headers and body leave only now: nothing reached a peer that failed the check.
            bodyWritten = true;
            req.end(request.body);
          });
        });
        req.on('response', (response) => {
          const status = response.statusCode ?? 0;
          const header = response.headers['retry-after'];
          const outcome = classifyStatus(status);
          // A delay means something only for "try later"; C++ refuses any other combination.
          settle({
            outcome,
            httpStatus: outcome === 'unknown' ? null : status,
            retryAfterSeconds: outcome === 'transient' ? parseRetryAfter(Array.isArray(header) ? header[0] : header, Date.now()) : null,
          });
          response.destroy();
          req.destroy();
        });
        // Before the write nothing left this process; after it the provider may have acted.
        req.on('error', () => {
          settle(bodyWritten ? { outcome: 'unknown', httpStatus: null, retryAfterSeconds: null } : notSent);
        });
        req.on('close', () => {
          settle(bodyWritten ? { outcome: 'unknown', httpStatus: null, retryAfterSeconds: null } : notSent);
        });
      });
    },
  };
}
