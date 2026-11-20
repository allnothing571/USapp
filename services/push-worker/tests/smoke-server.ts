/**
 * Isolated-network wiring only: the real worker server and real provider transport, with the resolver,
 * address policy, port and CA injected to reach an in-process stub provider. Never in the production
 * image (dist/src only) and not reachable from the production entry. Nothing here can reach a provider.
 *
 * The endpoint path chooses the stub provider's behaviour:
 *   verify-<max ttl>-   decrypts with RFC 8291 keys, checks payload, VAPID scheme and TTL; 201 or 400
 *   accept-             201       gone-      410       auth-   403
 *   retry-              429 with Retry-After: 120      redirect-   302 to another host
 *   reset-              the connection is dropped      slow-       never answers
 */

import type { IncomingMessage, ServerResponse } from 'node:http';

import { createLogger } from '../src/logger.js';
import { createWorkerServer } from '../src/server.js';
import { createProviderTransport } from '../src/transport.js';
import { decryptWithRfcKeys, startStubPeer, TEST_CERT } from './support.js';

const logger = createLogger('push-worker-smoke', '0.0.0');

function verified(req: IncomingMessage, body: Buffer, maxTtl: number): boolean {
  const ttl = Number(req.headers['ttl']);
  if (!Number.isInteger(ttl) || ttl > maxTtl || ttl < maxTtl - 60) return false;
  if (req.headers['content-encoding'] !== 'aes128gcm') return false;
  if (!/^vapid t=[^,]+, k=[A-Za-z0-9_-]{87}$/.test(String(req.headers['authorization']))) return false;
  try {
    const payload = JSON.parse(decryptWithRfcKeys(body)) as Record<string, unknown>;
    return payload['v'] === 1 && Object.keys(payload).sort().join() === 'event_type,marker,nid,target_id,target_kind,v' &&
      payload['event_type'] === 'message.created' && payload['target_kind'] === 'message';
  } catch {
    return false;
  }
}

function provider(req: IncomingMessage, res: ServerResponse): void {
  const leaf = (req.url ?? '').split('/').pop() ?? '';
  const behaviour = leaf.split('-')[0];
  const chunks: Buffer[] = [];
  req.on('data', (c: Buffer) => chunks.push(c));
  req.on('end', () => {
    if (behaviour === 'verify') {
      res.writeHead(verified(req, Buffer.concat(chunks), Number(leaf.split('-')[1])) ? 201 : 400);
      res.end();
    } else if (behaviour === 'gone') {
      res.writeHead(410);
      res.end();
    } else if (behaviour === 'auth') {
      res.writeHead(403);
      res.end();
    } else if (behaviour === 'retry') {
      res.writeHead(429, { 'Retry-After': '120' });
      res.end();
    } else if (behaviour === 'redirect') {
      res.writeHead(302, { Location: 'https://updates.push.services.mozilla.com/other' });
      res.end();
    } else if (behaviour === 'reset') {
      req.socket.destroy();
    } else if (behaviour !== 'slow') {
      res.writeHead(201);
      res.end();
    }
  });
}

const stub = await startStubPeer(provider);
const token = process.env['PUSH_WORKER_TOKEN'] ?? '';
const server = createWorkerServer({
  token,
  version: 'smoke',
  deps: {
    transport: createProviderTransport({
      resolve: () => Promise.resolve([{ address: '127.0.0.1', family: 4 }]),
      isAddressAllowed: () => true,
      port: stub.port,
      ca: TEST_CERT,
    }),
    vapid: {
      subject: 'mailto:test-only@example.com',
      publicKey: process.env['VAPID_PUBLIC_KEY'] ?? '',
      privateKey: process.env['VAPID_PRIVATE_KEY'] ?? '',
    },
    now: Date.now,
  },
}, logger);
server.listen(8090, '0.0.0.0', () => logger.info('smoke worker listening', {}));
