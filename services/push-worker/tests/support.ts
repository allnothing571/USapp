/**
 * Test-only material. The certificate and key were generated offline for these tests, are trusted
 * only through an explicit `ca` option in a test, and never reach the production image (the Dockerfile
 * copies dist/src only). The certificate names fcm.googleapis.com and
 * updates.push.services.mozilla.com, so a test can exercise hostname verification against a local peer.
 */

import { createECDH } from 'node:crypto';
import { createRequire } from 'node:module';
import type { IncomingMessage, ServerResponse } from 'node:http';
import https from 'node:https';
import type { AddressInfo } from 'node:net';

import type { DeliverRequest, VapidDetails } from '../src/deliver.js';

export const TEST_KEY = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgLvHjn7lxo+s1SPIz
oQ5bJS1UfgTg/4UdP+0fuLTEdz6hRANCAAQWUWAlN39LNE1fQ7dk1cFDbdyDUyPk
Vrk5hVH3ZXLkgqFl4BoJ27IjDUYhTtxEOQtTtnJvBhKHBQi39lpL5XkQ
-----END PRIVATE KEY-----
`;
export const TEST_CERT = `-----BEGIN CERTIFICATE-----
MIIBzzCCAXWgAwIBAgIUGUaOJJb3J+bCA3SfhXU1R2udnzwwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPdXNhcHAtdGVzdC1vbmx5MCAXDTI2MTEwMjA5NTU1OFoYDzIx
MjYxMDA5MDk1NTU4WjAaMRgwFgYDVQQDDA91c2FwcC10ZXN0LW9ubHkwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAAQWUWAlN39LNE1fQ7dk1cFDbdyDUyPkVrk5hVH3
ZXLkgqFl4BoJ27IjDUYhTtxEOQtTtnJvBhKHBQi39lpL5XkQo4GWMIGTMB0GA1Ud
DgQWBBQZHZ/+2clImqdS9ZqK3PtSqghnVDAfBgNVHSMEGDAWgBQZHZ/+2clImqdS
9ZqK3PtSqghnVDAPBgNVHRMBAf8EBTADAQH/MEAGA1UdEQQ5MDeCEmZjbS5nb29n
bGVhcGlzLmNvbYIhdXBkYXRlcy5wdXNoLnNlcnZpY2VzLm1vemlsbGEuY29tMAoG
CCqGSM49BAMCA0gAMEUCIQDIa44JCUZS7oJH7Fkgb6Tl905GijxTRYbJLjg27+dg
RgIgVwdiiGxlubTiXbbMknfjkIHKuKaRilCdMUKWa3y5tAI=
-----END CERTIFICATE-----
`;

// RFC 8291 Section 5: the user agent's key pair and auth secret.
export const RFC8291_UA_PUBLIC = 'BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4';
export const RFC8291_UA_PRIVATE = 'q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94';
export const RFC8291_AUTH = 'BTBZMqHH6r4Tts7J_aSIgg';

export const NOTIFICATION_ID = '11111111-1111-4111-8111-111111111111';
export const GENERATION_ID = '22222222-2222-4222-8222-222222222222';
export const TARGET_ID = '33333333-3333-4333-8333-333333333333';
export const TEST_ENDPOINT = 'https://fcm.googleapis.com/fcm/send/test-only-token';
export const WORKER_TOKEN = 'test-only-worker-token-with-enough-length';

/** A key pair made here and now: never written anywhere, gone with the process. */
export function testVapid(): VapidDetails & { readonly publicRaw: Buffer } {
  const ecdh = createECDH('prime256v1');
  ecdh.generateKeys();
  return {
    subject: 'mailto:test-only@example.com',
    publicKey: ecdh.getPublicKey().toString('base64url'),
    privateKey: ecdh.getPrivateKey().toString('base64url'),
    publicRaw: ecdh.getPublicKey(),
  };
}

export function deliverBody(overrides: Partial<DeliverRequest> = {}, nowMs = Date.now()): DeliverRequest {
  return {
    job: { notification_id: NOTIFICATION_ID, generation_id: GENERATION_ID, attempt_no: 1 },
    subscription: { endpoint: TEST_ENDPOINT, keys: { p256dh: RFC8291_UA_PUBLIC, auth: RFC8291_AUTH } },
    payload: { v: 1, nid: NOTIFICATION_ID, event_type: 'message.created', target_kind: 'message', target_id: TARGET_ID, marker: 'm'.repeat(22) },
    ttl_seconds: 3600,
    expires_at_ms: nowMs + 3_600_000,
    ...overrides,
  };
}

/** The aes128gcm decrypter that web-push itself depends on, reached through its own resolution path. */
export function decryptWithRfcKeys(body: Buffer): string {
  const require = createRequire(import.meta.url);
  const fromWebPush = createRequire(require.resolve('web-push'));
  const ece = fromWebPush('http_ece') as {
    decrypt(buffer: Buffer, params: Record<string, unknown>): Buffer;
  };
  const ua = createECDH('prime256v1');
  ua.setPrivateKey(Buffer.from(RFC8291_UA_PRIVATE, 'base64url'));
  return ece.decrypt(body, {
    version: 'aes128gcm',
    privateKey: ua,
    authSecret: Buffer.from(RFC8291_AUTH, 'base64url').toString('base64url'),
  }).toString('utf8');
}

export interface StubPeer {
  readonly port: number;
  readonly connections: () => number;
  readonly requests: () => number;
  readonly close: () => Promise<void>;
}

/** A local TLS peer presenting the test certificate. The handler decides the provider's behaviour. */
export async function startStubPeer(handler: (req: IncomingMessage, res: ServerResponse) => void): Promise<StubPeer> {
  let connections = 0;
  let requests = 0;
  const server = https.createServer({ key: TEST_KEY, cert: TEST_CERT }, (req, res) => {
    requests += 1;
    handler(req, res);
  });
  server.on('secureConnection', () => { connections += 1; });
  server.on('tlsClientError', () => undefined);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    port: (server.address() as AddressInfo).port,
    connections: () => connections,
    requests: () => requests,
    close: () => new Promise((resolve) => {
      server.closeAllConnections();
      server.close(() => resolve());
    }),
  };
}
