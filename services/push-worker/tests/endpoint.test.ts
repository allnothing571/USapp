import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { isPublicAddress, parseEndpoint, providerOf } from '../src/endpoint.js';

describe('endpoint policy', () => {
  it('accepts exact hosts and label-boundary suffixes', () => {
    for (const url of [
      'https://fcm.googleapis.com/fcm/send/abc',
      'https://updates.push.services.mozilla.com/wpush/v2/abc?x=1',
      'https://web.push.apple.com/abc',
      'https://wns2-par02p.notify.windows.com/w/?token=abc',
    ]) {
      assert.notEqual(parseEndpoint(url), null, url);
    }
    assert.deepEqual(parseEndpoint('https://fcm.googleapis.com/a?b=c'), { hostname: 'fcm.googleapis.com', path: '/a?b=c' });
  });

  it('refuses malicious hostnames and forms', () => {
    for (const url of [
      'http://fcm.googleapis.com/a',
      'https://fcm.googleapis.com:8443/a',
      'https://fcm.googleapis.com:443/a',
      'https://user:pw@fcm.googleapis.com/a',
      'https://fcm.googleapis.com@evil.example/a',
      'https://evil.example/fcm.googleapis.com',
      'https://fcm.googleapis.com.evil.example/a',
      'https://evilfcm.googleapis.com/a',
      'https://sub.fcm.googleapis.com/a',
      'https://evilpush.apple.com/a',
      'https://push.apple.com/a',
      'https://.push.apple.com/a',
      'https://notify.windows.com/a',
      'https://FCM.googleapis.com/a',
      'https://fcm.googleapis.com./a',
      'https://fcm.googleapis.com%2eevil.example/a',
      'https://xn--fcm-googleapis-com.example/a',
      'https://127.0.0.1/a',
      'https://[::1]/a',
      'https://2130706433/a',
      'https://0x7f.0.0.1/a',
      'https://fcm.googleapis.com/a#fragment',
      'https://fcm.googleapis.com\\@evil.example/a',
      'https://fcm.googleapis.com/a b',
      `https://fcm.googleapis.com/${'a'.repeat(2048)}`,
      '',
      'not a url',
    ]) {
      assert.equal(parseEndpoint(url), null, url);
    }
  });

  it('names only the provider family', () => {
    assert.equal(providerOf('web.push.apple.com'), 'apple');
    assert.equal(providerOf('fcm.googleapis.com'), 'fcm');
  });
});

describe('address policy', () => {
  it('allows public unicast', () => {
    for (const a of ['8.8.8.8', '142.250.1.1', '2607:f8b0:4005:80a::200e', '::ffff:8.8.8.8', '::ffff:808:808']) {
      assert.equal(isPublicAddress(a), true, a);
    }
  });

  it('refuses every non-public range, including mapped and compatible IPv6', () => {
    for (const a of [
      '0.0.0.0', '10.0.0.1', '100.64.0.1', '127.0.0.1', '169.254.169.254', '172.16.0.1', '172.31.255.255', '192.0.0.1',
      '192.0.2.1', '192.168.1.1', '198.18.0.1', '198.51.100.1', '203.0.113.1', '224.0.0.1', '240.0.0.1', '255.255.255.255',
      '::', '::1', 'fe80::1', 'fc00::1', 'fd12:3456::1', 'ff02::1', '2001:db8::1', '64:ff9b::7f00:1', '2002:7f00:1::',
      '::ffff:127.0.0.1', '::ffff:7f00:1', '0:0:0:0:0:ffff:7f00:1', '::FFFF:10.0.0.1', '::ffff:a00:1', '::ffff:169.254.169.254',
      '::127.0.0.1', '::7f00:1', 'fe80::1%eth0', '', 'not-an-address',
    ]) {
      assert.equal(isPublicAddress(a), false, a);
    }
  });
});
