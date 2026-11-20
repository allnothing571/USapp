/**
 * Endpoint and address policy (docs/phase2-web-push.md Section 8.3). Pure; applied again at the
 * connection boundary by transport.ts.
 */

import { BlockList, isIP } from 'node:net';

export const MAX_ENDPOINT_BYTES = 2048;

const EXACT_HOSTS: ReadonlySet<string> = new Set(['fcm.googleapis.com', 'updates.push.services.mozilla.com']);
// Label boundary: the host must have at least one label before the leading dot of the suffix.
const HOST_SUFFIXES: readonly string[] = ['.push.apple.com', '.notify.windows.com'];
const HOST_SHAPE = /^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$/;

export interface ParsedEndpoint {
  readonly hostname: string;
  readonly path: string;
}

/** Strict parse, or null. The raw text is checked first: the URL parser would normalize it away. */
export function parseEndpoint(endpoint: string): ParsedEndpoint | null {
  if (Buffer.byteLength(endpoint) > MAX_ENDPOINT_BYTES) return null;
  if (!endpoint.startsWith('https://') || /[\s\\#]/.test(endpoint)) return null;
  const rawHost = endpoint.slice('https://'.length).split(/[/?]/, 1)[0] ?? '';
  // No userinfo or port, and no case or escape form the parser would quietly normalize.
  if (!HOST_SHAPE.test(rawHost)) return null;
  let url: URL;
  try {
    url = new URL(endpoint);
  } catch {
    return null;
  }
  if (url.protocol !== 'https:' || url.username !== '' || url.password !== '' || url.port !== '' || url.hash !== '') {
    return null;
  }
  const hostname = url.hostname;
  if (hostname !== rawHost || isIP(hostname) !== 0) return null;
  const allowed = EXACT_HOSTS.has(hostname) ||
    HOST_SUFFIXES.some((suffix) => hostname.length > suffix.length && hostname.endsWith(suffix));
  if (!allowed) return null;
  return { hostname, path: `${url.pathname}${url.search}` };
}

/** For logs: the provider family only, never the endpoint. */
export function providerOf(hostname: string): string {
  if (hostname === 'fcm.googleapis.com') return 'fcm';
  if (hostname === 'updates.push.services.mozilla.com') return 'mozilla';
  if (hostname.endsWith('.push.apple.com')) return 'apple';
  if (hostname.endsWith('.notify.windows.com')) return 'windows';
  return 'other';
}

const blocked = new BlockList();
for (const [network, prefix] of [
  ['0.0.0.0', 8], ['10.0.0.0', 8], ['100.64.0.0', 10], ['127.0.0.0', 8], ['169.254.0.0', 16], ['172.16.0.0', 12],
  ['192.0.0.0', 24], ['192.0.2.0', 24], ['192.88.99.0', 24], ['192.168.0.0', 16], ['198.18.0.0', 15],
  ['198.51.100.0', 24], ['203.0.113.0', 24], ['224.0.0.0', 4], ['240.0.0.0', 4],
] as const) {
  blocked.addSubnet(network, prefix, 'ipv4');
}
for (const [network, prefix] of [
  ['::', 128], ['::1', 128], ['64:ff9b::', 96], ['64:ff9b:1::', 48], ['100::', 64], ['2001::', 32], ['2001:db8::', 32],
  ['2002::', 16], ['fc00::', 7], ['fe80::', 10], ['fec0::', 10], ['ff00::', 8],
] as const) {
  blocked.addSubnet(network, prefix, 'ipv6');
}

/** Expands a textual IPv6 address to its sixteen bytes. Input already passed net.isIP. */
function ipv6Bytes(address: string): number[] {
  let text = address.split('%')[0] ?? '';
  const dotted = /(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(text);
  if (dotted) {
    const [a, b, c, d] = dotted.slice(1).map(Number) as [number, number, number, number];
    text = `${text.slice(0, dotted.index)}${((a << 8) | b).toString(16)}:${((c << 8) | d).toString(16)}`;
  }
  const [head = '', tail] = text.split('::');
  const left = head === '' ? [] : head.split(':');
  const right = tail === undefined || tail === '' ? [] : tail.split(':');
  const fill = tail === undefined ? 0 : 8 - left.length - right.length;
  const groups = [...left, ...new Array<string>(fill).fill('0'), ...right];
  return groups.flatMap((g) => {
    const v = parseInt(g, 16);
    return [v >> 8, v & 0xff];
  });
}

/**
 * The IPv4 address an IPv6 form actually reaches: mapped (::ffff:a.b.c.d, in any spelling) and the
 * deprecated compatible form (::a.b.c.d). Null otherwise.
 */
function embeddedIpv4(address: string): string | null {
  const b = ipv6Bytes(address);
  if (b.length !== 16 || b.slice(0, 10).some((x) => x !== 0)) return null;
  const mapped = b[10] === 0xff && b[11] === 0xff;
  const compatible = b[10] === 0 && b[11] === 0;
  if (!mapped && !compatible) return null;
  return b.slice(12).join('.');
}

/** True only for an address that may be a provider's: public unicast. Unparseable text is false. */
export function isPublicAddress(address: string): boolean {
  const family = isIP(address.split('%')[0] ?? '');
  if (family === 4) return !blocked.check(address, 'ipv4');
  if (family !== 6) return false;
  if (address.includes('%')) return false;  // a zone id means link-local scope
  const embedded = embeddedIpv4(address);
  if (embedded !== null) return !blocked.check(embedded, 'ipv4');
  return !blocked.check(address, 'ipv6');
}
