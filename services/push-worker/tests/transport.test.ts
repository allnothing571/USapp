/**
 * The connection boundary against local TLS peers. The production address policy is replaced only
 * through createProviderTransport's argument; a separate test proves the default refuses loopback.
 */

import assert from 'node:assert/strict';
import { after, describe, it } from 'node:test';
import type https from 'node:https';

import { classifyStatus, createProviderTransport, parseRetryAfter, type TransportOptions } from '../src/transport.js';
import { startStubPeer, TEST_CERT, TEST_ENDPOINT, type StubPeer } from './support.js';

const request = { endpoint: TEST_ENDPOINT, headers: { TTL: 60, 'Content-Length': 2 }, body: Buffer.from('hi') };
const never = (): AbortSignal => new AbortController().signal;
const peers: StubPeer[] = [];
after(async () => { await Promise.all(peers.map((p) => p.close())); });

async function peer(handler: Parameters<typeof startStubPeer>[0]): Promise<StubPeer> {
  const p = await startStubPeer(handler);
  peers.push(p);
  return p;
}
const via = (p: StubPeer, extra: TransportOptions = {}): ReturnType<typeof createProviderTransport> => createProviderTransport({
  resolve: () => Promise.resolve([{ address: '127.0.0.1', family: 4 }]),
  isAddressAllowed: () => true,
  port: p.port,
  ca: TEST_CERT,
  ...extra,
});

describe('status mapping', () => {
  it('maps the documented classes', () => {
    const table: Array<[number, string]> = [
      [200, 'accepted'], [201, 'accepted'], [202, 'accepted'], [404, 'gone'], [410, 'gone'], [401, 'provider_auth_failed'],
      [403, 'provider_auth_failed'], [408, 'transient'], [425, 'transient'], [429, 'transient'], [500, 'transient'],
      [503, 'transient'], [301, 'rejected'], [302, 'rejected'], [307, 'rejected'], [400, 'rejected'], [413, 'rejected'],
      [422, 'rejected'], [100, 'unknown'],
    ];
    for (const [status, outcome] of table) assert.equal(classifyStatus(status), outcome, String(status));
  });

  it('parses Retry-After as seconds or a date, bounded to a day', () => {
    const now = Date.parse('2026-11-02T00:00:00Z');
    assert.equal(parseRetryAfter(undefined, now), null);
    assert.equal(parseRetryAfter('120', now), 120);
    assert.equal(parseRetryAfter('9999999', now), 86400);
    assert.equal(parseRetryAfter('Mon, 02 Nov 2026 00:01:00 GMT', now), 60);
    assert.equal(parseRetryAfter('Sun, 01 Nov 2026 00:00:00 GMT', now), 0);
    assert.equal(parseRetryAfter('soon', now), null);
    assert.equal(parseRetryAfter('-5', now), null);
  });
});

describe('provider transport', () => {
  it('reports the status and Retry-After, with the body on the wire', async () => {
    let received = '';
    const p = await peer((req, res) => {
      req.on('data', (c: Buffer) => { received += c.toString(); });
      req.on('end', () => { res.writeHead(429, { 'Retry-After': '30' }); res.end(); });
    });
    const result = await via(p).post(request, never());
    assert.deepEqual(result, { outcome: 'transient', httpStatus: 429, retryAfterSeconds: 30 });
    assert.equal(received, 'hi');
  });

  it('reports only combinations the API accepts: a delay only on transient', async () => {
    for (const [status, outcome] of [[201, 'accepted'], [410, 'gone'], [403, 'provider_auth_failed'], [400, 'rejected']] as const) {
      const p = await peer((req, res) => { req.resume(); res.writeHead(status, { 'Retry-After': '30' }); res.end(); });
      assert.deepEqual(await via(p).post(request, never()), { outcome, httpStatus: status, retryAfterSeconds: null });
    }
  });

  it('uses a new connection and agent per request and destroys the agent', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    const agents: https.Agent[] = [];
    const t = via(p, { onAgent: (a) => agents.push(a) });
    const before = p.connections();
    assert.equal((await t.post(request, never())).outcome, 'accepted');
    assert.equal((await t.post(request, never())).outcome, 'accepted');
    assert.equal(p.connections() - before, 2);
    assert.equal(agents.length, 2);
    assert.notEqual(agents[0], agents[1]);
    await new Promise((resolve) => setTimeout(resolve, 50));  // an agent drops a socket on its close event
    for (const a of agents) {
      assert.deepEqual(Object.keys(a.sockets), []);
      assert.deepEqual(Object.keys(a.freeSockets), []);
    }
  });

  it('does not follow a redirect', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(307, { Location: 'https://fcm.googleapis.com/other' }); res.end(); });
    const before = p.requests();
    assert.deepEqual(await via(p).post(request, never()), { outcome: 'rejected', httpStatus: 307, retryAfterSeconds: null });
    assert.equal(p.requests() - before, 1);
  });

  it('refuses a non-public resolved address under the default policy, before connecting', async () => {
    const p = await peer((req, res) => { req.resume(); res.end(); });
    for (const address of ['127.0.0.1', '10.0.0.1', '169.254.169.254', '::1', '::ffff:127.0.0.1', '::ffff:7f00:1']) {
      const t = createProviderTransport({
        resolve: () => Promise.resolve([{ address, family: address.includes(':') ? 6 : 4 }]),
        port: p.port,
        ca: TEST_CERT,
      });
      const before = p.connections();
      assert.deepEqual(await t.post(request, never()), { outcome: 'transport_unavailable', httpStatus: null, retryAfterSeconds: null }, address);
      assert.equal(p.connections(), before, address);
    }
  });

  it('drops refused answers and connects only to what remains', async () => {
    const seen: string[] = [];
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    const t = via(p, {
      resolve: () => Promise.resolve([{ address: '10.0.0.1', family: 4 }, { address: '127.0.0.1', family: 4 }]),
      // 10.0.0.1 stands for a refused answer; 127.0.0.1 stands for the public one in this test.
      isAddressAllowed: (a) => { seen.push(a); return a === '127.0.0.1'; },
    });
    assert.equal((await t.post(request, never())).outcome, 'accepted');
    assert.ok(seen.includes('10.0.0.1'));
  });

  it('fails closed when lookup fails', async () => {
    const p = await peer((req, res) => { req.resume(); res.end(); });
    const t = via(p, { resolve: () => Promise.reject(new Error('dns')) });
    assert.equal((await t.post(request, never())).outcome, 'transport_unavailable');
  });

  it('checks the actual peer and writes nothing to a refused one', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    let calls = 0;
    // Lookup sees a permitted answer; the connected peer is then refused (DNS rebinding's shape).
    const t = via(p, { isAddressAllowed: () => { calls += 1; return calls === 1; } });
    const before = p.requests();
    assert.deepEqual(await t.post(request, never()), { outcome: 'transport_unavailable', httpStatus: null, retryAfterSeconds: null });
    assert.equal(p.requests(), before);
    assert.ok(calls >= 2);
  });

  it('verifies the certificate chain and the hostname', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    const before = p.requests();
    // Not trusted: no test CA supplied.
    const untrusted = createProviderTransport({
      resolve: () => Promise.resolve([{ address: '127.0.0.1', family: 4 }]), isAddressAllowed: () => true, port: p.port,
    });
    assert.equal((await untrusted.post(request, never())).outcome, 'transport_unavailable');
    // Trusted chain, but the certificate does not name this allowlisted host.
    const wrongName = { ...request, endpoint: 'https://web.push.apple.com/abc' };
    assert.equal((await via(p).post(wrongName, never())).outcome, 'transport_unavailable');
    assert.equal(p.requests(), before);
  });

  it('refuses an endpoint outside the allowlist at the connection boundary', async () => {
    const p = await peer((req, res) => { req.resume(); res.end(); });
    const before = p.connections();
    assert.equal((await via(p).post({ ...request, endpoint: 'https://evil.example/a' }, never())).outcome, 'transport_unavailable');
    assert.equal(p.connections(), before);
  });

  it('holds an absolute deadline against a peer that keeps trickling', async () => {
    const timers: NodeJS.Timeout[] = [];
    const p = await peer((req, res) => {
      req.resume();
      res.socket?.write('HTTP/1.1 200 OK\r\n');
      // An inactivity timeout would never fire: there is always another byte.
      timers.push(setInterval(() => { res.socket?.write('X-Pad: y\r\n'); }, 20));
    });
    const started = Date.now();
    const result = await via(p).post(request, AbortSignal.timeout(250));
    for (const timer of timers) clearInterval(timer);
    assert.deepEqual(result, { outcome: 'unknown', httpStatus: null, retryAfterSeconds: null });
    assert.ok(Date.now() - started < 2000);
  });

  it('classifies a silent peer after the send as unknown, and an abort before connecting as not sent', async () => {
    const p = await peer((req) => { req.resume(); });
    assert.equal((await via(p).post(request, AbortSignal.timeout(150))).outcome, 'unknown');
    const stalled = via(p, { resolve: () => new Promise(() => undefined) });
    assert.equal((await stalled.post(request, AbortSignal.timeout(100))).outcome, 'transport_unavailable');
    const aborted = new AbortController();
    aborted.abort();
    assert.equal((await via(p).post(request, aborted.signal)).outcome, 'transport_unavailable');
  });

  it('classifies a connection lost after the send as unknown', async () => {
    const p = await peer((req) => { req.resume(); req.socket.destroy(); });
    assert.equal((await via(p).post(request, never())).outcome, 'unknown');
  });

  it('caps oversized response headers and never reads a large body', async () => {
    const big = await peer((req, res) => {
      req.resume();
      res.socket?.write(`HTTP/1.1 200 OK\r\nX-Big: ${'a'.repeat(32 * 1024)}\r\n\r\n`);
    });
    assert.equal((await via(big).post(request, never())).outcome, 'unknown');
    let sent = 0;
    let closed = false;
    const body = await peer((req, res) => {
      req.resume();
      res.writeHead(201);
      const chunk = Buffer.alloc(64 * 1024);
      const pump = (): void => {
        while (!res.destroyed && sent < 512 * 1024 * 1024 && res.write(chunk)) sent += chunk.length;
      };
      res.on('drain', pump);
      res.on('close', () => { closed = true; });
      pump();
    });
    assert.equal((await via(body).post(request, never())).outcome, 'accepted');
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(closed, true);
    assert.ok(sent < 512 * 1024 * 1024);
  });

  it('settles exactly once with a late abort after the result', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    const controller = new AbortController();
    let settled = 0;
    const result = await via(p).post(request, controller.signal).then((r) => { settled += 1; return r; });
    controller.abort();
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(result.outcome, 'accepted');
    assert.equal(settled, 1);
  });

  it('is not routed through environment proxy variables', async () => {
    const p = await peer((req, res) => { req.resume(); res.writeHead(201); res.end(); });
    const saved = { https: process.env['HTTPS_PROXY'], lower: process.env['https_proxy'] };
    process.env['HTTPS_PROXY'] = 'http://127.0.0.1:9';
    process.env['https_proxy'] = 'http://127.0.0.1:9';
    try {
      assert.equal((await via(p).post(request, never())).outcome, 'accepted');
    } finally {
      if (saved.https === undefined) delete process.env['HTTPS_PROXY']; else process.env['HTTPS_PROXY'] = saved.https;
      if (saved.lower === undefined) delete process.env['https_proxy']; else process.env['https_proxy'] = saved.lower;
    }
  });
});
