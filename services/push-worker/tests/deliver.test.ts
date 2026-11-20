/** Request validation and crypto construction, with RFC 8291 keys and a VAPID pair made at test time. */

import assert from 'node:assert/strict';
import { createPublicKey, verify } from 'node:crypto';
import { describe, it } from 'node:test';

import { deliver, parseDeliverRequest, type DeliverDeps } from '../src/deliver.js';
import type { ProviderRequest, ProviderResult } from '../src/transport.js';
import { decryptWithRfcKeys, deliverBody, NOTIFICATION_ID, RFC8291_AUTH, RFC8291_UA_PUBLIC, TARGET_ID, TEST_ENDPOINT, testVapid } from './support.js';

const accepted: ProviderResult = { outcome: 'accepted', httpStatus: 201, retryAfterSeconds: null };

function capture(now = 1_800_000_000_000): { deps: DeliverDeps; sent: ProviderRequest[]; vapid: ReturnType<typeof testVapid>; now: number } {
  const sent: ProviderRequest[] = [];
  const vapid = testVapid();
  return {
    sent,
    vapid,
    now,
    deps: {
      vapid,
      now: () => now,
      transport: { post: (request) => { sent.push(request); return Promise.resolve(accepted); } },
    },
  };
}
const signal = (): AbortSignal => new AbortController().signal;

describe('parseDeliverRequest', () => {
  const base = (): Record<string, unknown> => JSON.parse(JSON.stringify(deliverBody())) as Record<string, unknown>;

  it('accepts the exact shape', () => {
    assert.notEqual(parseDeliverRequest(base()), null);
  });
  it('accepts the personal account target but rejects private financial or session fields', () => {
    for (const event of ['security.wallet_adjusted', 'security.redemption_cancelled', 'security.session_created']) {
      const original = deliverBody();
      const body = { ...original, payload: { ...original.payload, event_type: event, target_kind: 'account' } };
      assert.notEqual(parseDeliverRequest(body), null);
      for (const field of ['reason', 'admin_id', 'session_id', 'token_hash']) {
        const extra = { ...body, payload: { ...body.payload, [field]: 'private' } };
        assert.equal(parseDeliverRequest(extra), null);
      }
    }
  });

  it('refuses extra fields, wrong types and a mismatched notification', () => {
    const cases: Array<(b: any) => void> = [
      (b) => { b.extra = 1; },
      (b) => { delete b.job; },
      (b) => { b.job.attempt_no = 0; },
      (b) => { b.job.attempt_no = 1.5; },
      (b) => { b.job.generation_id = 'not-a-uuid'; },
      (b) => { b.job.extra = 1; },
      (b) => { b.subscription.endpoint = 'https://evil.example/a'; },
      (b) => { b.subscription.keys.extra = 'x'; },
      (b) => { b.subscription.keys.auth = 'short'; },
      (b) => { b.payload.v = 2; },
      (b) => { b.payload.nid = TARGET_ID; },
      (b) => { b.payload.message = 'text must never travel'; },
      (b) => { b.payload.title = 'x'; },
      (b) => { b.payload.event_type = 'Message Created'; },
      (b) => { b.payload.target_kind = 'wallet'; },
      (b) => { b.payload.target_id = '../../evil'; },
      (b) => { b.payload.marker = 'short'; },
      (b) => { b.ttl_seconds = 86401; },
      (b) => { b.ttl_seconds = '60'; },
      (b) => { b.expires_at_ms = 0; },
      (b) => { delete b.expires_at_ms; },
    ];
    for (const mutate of cases) {
      const body = base();
      mutate(body);
      assert.equal(parseDeliverRequest(body), null, mutate.toString());
    }
    for (const value of [null, [], 'x', 1]) assert.equal(parseDeliverRequest(value), null);
  });

  it('accepts every closed target kind, including the three D20 kinds, and nothing near them', () => {
    for (const kind of ['task', 'reward', 'anniversary', 'message', 'memory', 'shared_plan', 'album']) {
      const body = base();
      (body.payload as Record<string, unknown>).target_kind = kind;
      assert.notEqual(parseDeliverRequest(body), null, kind);
    }
    for (const kind of ['memories', 'plan', 'album_photo', 'Album', 'media', '']) {
      const body = base();
      (body.payload as Record<string, unknown>).target_kind = kind;
      assert.equal(parseDeliverRequest(body), null, kind);
    }
    for (const event of ['memory.created', 'shared_plan.created', 'album.photos_added']) {
      const body = base();
      (body.payload as Record<string, unknown>).event_type = event;
      assert.notEqual(parseDeliverRequest(body), null, event);
    }
  });

  it('holds the TTL to 0-86400 whole seconds: both ends parse, nothing beyond them does', () => {
    for (const ttl of [0, 1, 86400]) {
      const body = JSON.parse(JSON.stringify(deliverBody({ ttl_seconds: ttl })));
      assert.notEqual(parseDeliverRequest(body), null, String(ttl));
    }
    for (const ttl of [-1, 86401, 1.5, 90000000000]) {
      const body = JSON.parse(JSON.stringify(deliverBody({ ttl_seconds: ttl })));
      assert.equal(parseDeliverRequest(body), null, String(ttl));
    }
  });

  it('refuses a p256dh that is not on the curve, even with the right shape', () => {
    const body = deliverBody();
    const offCurve = Buffer.concat([Buffer.from([4]), Buffer.alloc(64, 1)]).toString('base64url');
    assert.equal(offCurve.length, 87);
    const bad = { ...body, subscription: { endpoint: TEST_ENDPOINT, keys: { p256dh: offCurve, auth: RFC8291_AUTH } } };
    assert.equal(parseDeliverRequest(JSON.parse(JSON.stringify(bad))), null);
    const compressed = Buffer.concat([Buffer.from([2]), Buffer.alloc(64, 1)]).toString('base64url');
    const bad2 = { ...body, subscription: { endpoint: TEST_ENDPOINT, keys: { p256dh: compressed, auth: RFC8291_AUTH } } };
    assert.equal(parseDeliverRequest(JSON.parse(JSON.stringify(bad2))), null);
  });
});

describe('deliver', () => {
  it('encrypts so the RFC 8291 user agent keys can decrypt the exact payload', async () => {
    const c = capture();
    const body = deliverBody({}, c.now);
    assert.deepEqual(await deliver(body, c.deps, signal()), accepted);
    const sent = c.sent[0];
    assert.ok(sent);
    assert.equal(sent.headers['Content-Encoding'], 'aes128gcm');
    assert.equal(sent.headers['Content-Type'], 'application/octet-stream');
    assert.equal(sent.endpoint, TEST_ENDPOINT);
    assert.ok(!sent.body.toString('latin1').includes(NOTIFICATION_ID), 'payload is not in clear');
    assert.deepEqual(JSON.parse(decryptWithRfcKeys(sent.body)), body.payload);
  });

  it('never produces the same ciphertext twice', async () => {
    const c = capture();
    await deliver(deliverBody({}, c.now), c.deps, signal());
    await deliver(deliverBody({}, c.now), c.deps, signal());
    assert.ok(c.sent[0] && c.sent[1] && !c.sent[0].body.equals(c.sent[1].body));
  });

  it('signs a VAPID JWT for the provider origin with the configured key', async () => {
    const realNow = Date.now();
    const c = capture(realNow);
    await deliver(deliverBody({}, c.now), c.deps, signal());
    const auth = String(c.sent[0]?.headers['Authorization']);
    const match = /^vapid t=([^,]+), k=(.+)$/.exec(auth);
    assert.ok(match, 'vapid scheme');
    const [, jwt = '', k = ''] = match;
    assert.equal(k, c.vapid.publicKey);
    const [header = '', claims = '', signature = ''] = jwt.split('.');
    const decoded = JSON.parse(Buffer.from(claims, 'base64url').toString()) as { aud: string; exp: number; sub: string };
    assert.equal(decoded.aud, 'https://fcm.googleapis.com');
    assert.equal(decoded.sub, c.vapid.subject);
    assert.ok(decoded.exp * 1000 <= realNow + 24 * 3600 * 1000 + 1000);
    const x = c.vapid.publicRaw.subarray(1, 33).toString('base64url');
    const y = c.vapid.publicRaw.subarray(33, 65).toString('base64url');
    const key = createPublicKey({ key: { kty: 'EC', crv: 'P-256', x, y }, format: 'jwk' });
    assert.equal(
      verify('sha256', Buffer.from(`${header}.${claims}`), { key, dsaEncoding: 'ieee-p1363' }, Buffer.from(signature, 'base64url')),
      true,
    );
  });

  it('sends the remaining TTL, never a fresh one', async () => {
    const c = capture();
    await deliver(deliverBody({ ttl_seconds: 86400, expires_at_ms: c.now + 90_500 }, c.now), c.deps, signal());
    assert.equal(c.sent[0]?.headers['TTL'], 90);
    await deliver(deliverBody({ ttl_seconds: 30, expires_at_ms: c.now + 90_500 }, c.now), c.deps, signal());
    assert.equal(c.sent[1]?.headers['TTL'], 30);
  });

  it('sends the largest valid TTL without extending the expiry it was given', async () => {
    const c = capture();
    await deliver(deliverBody({ ttl_seconds: 86400, expires_at_ms: c.now + 86_400_000 }, c.now), c.deps, signal());
    assert.equal(c.sent[0]?.headers['TTL'], 86400);
    await deliver(deliverBody({ ttl_seconds: 86400, expires_at_ms: c.now + 1_000 }, c.now), c.deps, signal());
    assert.equal(c.sent[1]?.headers['TTL'], 1);
  });

  it('does not continue an expired delivery that arrived late', async () => {
    const c = capture();
    for (const expires of [c.now - 1, c.now, c.now + 999]) {
      assert.deepEqual(
        await deliver(deliverBody({ ttl_seconds: 86400, expires_at_ms: expires }, c.now), c.deps, signal()),
        { outcome: 'ttl_skipped', httpStatus: null, retryAfterSeconds: null },
      );
    }
    assert.deepEqual(
      await deliver(deliverBody({ ttl_seconds: 0, expires_at_ms: c.now + 60_000 }, c.now), c.deps, signal()),
      { outcome: 'ttl_skipped', httpStatus: null, retryAfterSeconds: null },
    );
    assert.equal(c.sent.length, 0);
  });

  it('reports bad key material as not sent, with no library text', async () => {
    const c = capture();
    const badVapid = { ...c.deps, vapid: { ...c.vapid, privateKey: 'short' } };
    assert.deepEqual(await deliver(deliverBody({}, c.now), badVapid, signal()),
      { outcome: 'transport_unavailable', httpStatus: null, retryAfterSeconds: null });
    assert.equal(c.sent.length, 0);
  });

  it('does not begin after the deadline has passed', async () => {
    const c = capture();
    const controller = new AbortController();
    controller.abort();
    assert.equal((await deliver(deliverBody({}, c.now), c.deps, controller.signal)).outcome, 'transport_unavailable');
    assert.equal(c.sent.length, 0);
  });

  it('keeps the RFC fixture public key on the curve', () => {
    assert.equal(Buffer.from(RFC8291_UA_PUBLIC, 'base64url').length, 65);
  });
});
