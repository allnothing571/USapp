/**
 * One delivery: strict request validation, the expiry check, encryption and VAPID through web-push,
 * then the controlled transport. No state, no queue, no business decision: C++ owns all of that.
 * web-push is used only through generateRequestDetails; it never sends.
 */

import { ECDH } from 'node:crypto';

import webPush from 'web-push';

import { parseEndpoint, providerOf } from './endpoint.js';
import type { ProviderResult, ProviderTransport } from './transport.js';

export interface DeliverJob {
  readonly notification_id: string;
  readonly generation_id: string;
  readonly attempt_no: number;
}

export interface DeliverRequest {
  readonly job: DeliverJob;
  readonly subscription: { readonly endpoint: string; readonly keys: { readonly p256dh: string; readonly auth: string } };
  readonly payload: Readonly<Record<string, string | number>>;
  readonly ttl_seconds: number;
  readonly expires_at_ms: number;
}

export type DeliverOutcome = ProviderResult['outcome'] | 'ttl_skipped';

export interface DeliverResult {
  readonly outcome: DeliverOutcome;
  readonly httpStatus: number | null;
  readonly retryAfterSeconds: number | null;
}

export interface VapidDetails {
  readonly subject: string;
  readonly publicKey: string;
  readonly privateKey: string;
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
// D20 (0043): memory, shared_plan and album are the three Phase 3 targets. Every kind stays closed; nothing else passes.
const TARGET_KINDS: ReadonlySet<string> = new Set(['task', 'reward', 'anniversary', 'message', 'memory', 'shared_plan', 'album', 'account']);
export const MAX_PAYLOAD_BYTES = 1024;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function exactKeys(value: Record<string, unknown>, keys: readonly string[]): boolean {
  const present = Object.keys(value);
  return present.length === keys.length && keys.every((k) => Object.hasOwn(value, k));
}
/** Uncompressed and on the P-256 curve: OpenSSL decides, through Node. */
function isP256Point(base64url: string): boolean {
  try {
    const raw = Buffer.from(base64url, 'base64url');
    if (raw.length !== 65 || raw[0] !== 0x04) return false;
    ECDH.convertKey(raw, 'prime256v1');
    return true;
  } catch {
    return false;
  }
}

/** The request exactly, or null. A null never says which field failed: nothing is echoed. */
export function parseDeliverRequest(value: unknown): DeliverRequest | null {
  if (!isObject(value) || !exactKeys(value, ['job', 'subscription', 'payload', 'ttl_seconds', 'expires_at_ms'])) return null;
  const { job, subscription, payload, ttl_seconds: ttl, expires_at_ms: expires } = value;
  if (!isObject(job) || !exactKeys(job, ['notification_id', 'generation_id', 'attempt_no'])) return null;
  if (typeof job['notification_id'] !== 'string' || !UUID.test(job['notification_id']) ||
    typeof job['generation_id'] !== 'string' || !UUID.test(job['generation_id']) ||
    !Number.isInteger(job['attempt_no']) || (job['attempt_no'] as number) < 1 || (job['attempt_no'] as number) > 100000) return null;
  if (!isObject(subscription) || !exactKeys(subscription, ['endpoint', 'keys'])) return null;
  const keys = subscription['keys'];
  if (typeof subscription['endpoint'] !== 'string' || parseEndpoint(subscription['endpoint']) === null) return null;
  if (!isObject(keys) || !exactKeys(keys, ['p256dh', 'auth'])) return null;
  if (typeof keys['p256dh'] !== 'string' || !/^[A-Za-z0-9_-]{87}$/.test(keys['p256dh']) ||
    typeof keys['auth'] !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(keys['auth']) || !isP256Point(keys['p256dh'])) return null;
  if (!isObject(payload) || !exactKeys(payload, ['v', 'nid', 'event_type', 'target_kind', 'target_id', 'marker'])) return null;
  if (payload['v'] !== 1 || payload['nid'] !== job['notification_id'] ||
    typeof payload['event_type'] !== 'string' || !/^[a-z_]{1,32}\.[a-z_]{1,32}$/.test(payload['event_type']) ||
    typeof payload['target_kind'] !== 'string' || !TARGET_KINDS.has(payload['target_kind']) ||
    typeof payload['target_id'] !== 'string' || !UUID.test(payload['target_id']) ||
    typeof payload['marker'] !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(payload['marker'])) return null;
  if (!Number.isInteger(ttl) || (ttl as number) < 0 || (ttl as number) > 86400) return null;
  if (Buffer.byteLength(JSON.stringify(payload)) > MAX_PAYLOAD_BYTES) return null;
  if (!Number.isSafeInteger(expires) || (expires as number) <= 0) return null;
  return value as unknown as DeliverRequest;
}

export interface DeliverDeps {
  readonly transport: ProviderTransport;
  readonly vapid: VapidDetails;
  readonly now: () => number;
}

export interface DeliverInfo {
  readonly provider: string;
}

/**
 * Never continues an already-expired delivery. The provider TTL is what remains now, so a retry or a
 * queueing delay can never extend it to a fresh 24 hours.
 */
export async function deliver(request: DeliverRequest, deps: DeliverDeps, signal: AbortSignal): Promise<DeliverResult> {
  const remaining = Math.floor((request.expires_at_ms - deps.now()) / 1000);
  const ttl = Math.min(request.ttl_seconds, remaining);
  if (ttl < 1 || signal.aborted) {
    return { outcome: signal.aborted ? 'transport_unavailable' : 'ttl_skipped', httpStatus: null, retryAfterSeconds: null };
  }
  let details: webPush.RequestDetails;
  try {
    details = webPush.generateRequestDetails(request.subscription, JSON.stringify(request.payload), {
      vapidDetails: deps.vapid,
      TTL: ttl,
      urgency: 'normal',
      contentEncoding: 'aes128gcm',
    });
  } catch {
    // Bad key material: nothing was sent, and the library's message is not kept.
    return { outcome: 'transport_unavailable', httpStatus: null, retryAfterSeconds: null };
  }
  return deps.transport.post({ endpoint: request.subscription.endpoint, headers: details.headers, body: details.body }, signal);
}

export function infoOf(request: DeliverRequest): DeliverInfo {
  const parsed = parseEndpoint(request.subscription.endpoint);
  return { provider: parsed === null ? 'other' : providerOf(parsed.hostname) };
}
