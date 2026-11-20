/**
 * P2-5a Web Push device subscriptions (contract 0.19.0; docs/phase2-web-push.md Section 9).
 *
 * Lists never carry endpoints or keys. Registration responses separate the immutable command
 * facts (the key's ORIGINAL result) from the generation's CURRENT state; the validators below
 * check both against what the client captured before submitting.
 */
import { request } from './client'

export interface PushConfig {
  subscribe_available: boolean
  vapid_public_key: string | null
  device_limit: number
}

export type PushGenerationState = 'active' | 'inactive' | 'revoked'
/** `gone`: the push provider reported the subscription dead; never a user action (P2-5b). */
export type PushRevokedReason = 'user' | 'superseded' | 'replaced' | 'reowned' | 'gone'

export interface PushGeneration {
  id: string
  state: PushGenerationState
  activated_at: string
  revoked_reason: PushRevokedReason | null
  revoked_at: string | null
  current_session: boolean
  provider: 'fcm' | 'mozilla' | 'apple' | 'windows'
  user_agent_family: string | null
  endpoint_hash?: string
  vapid_key_id?: string
}

/**
 * `devices` is complete: every generation occupying a cap slot (unrevoked, session valid, any key
 * id), so absence from it is authoritative. `history` is bounded (newest revoked or
 * session-ended generations) and never displaces a device.
 */
export interface PushSubscriptionList {
  devices: PushGeneration[]
  history: PushGeneration[]
  occupied_slots: number
  device_limit: number
}

export interface PushRegistrationBody {
  endpoint: string
  keys: { p256dh: string; auth: string }
}

export interface PushCommand {
  result: 'created' | 'already_active'
  generation_id: string
  recorded_at: string
}

export interface PushRegistrationResponse {
  command: PushCommand
  generation: PushGeneration
  marker: string | null
}

export interface PushRevocation {
  generation_id: string
  reason: PushRevokedReason
  /** Null exactly when the reason is `gone` (provider attribution, no human actor). */
  revoked_by: string | null
  revoked_at: string
}

export interface PushRevocationResponse {
  revocation: PushRevocation
  generation: PushGeneration
}

export function getPushConfig(): Promise<PushConfig> {
  return request('/api/v1/push/config', { method: 'GET' })
}

export function listPushSubscriptions(): Promise<PushSubscriptionList> {
  return request('/api/v1/push/subscriptions', { method: 'GET' })
}

/** The key and body are captured once; a retry resends exactly the same pair. */
export function registerPushSubscription(body: PushRegistrationBody, key: string): Promise<PushRegistrationResponse> {
  return request('/api/v1/push/subscriptions', { method: 'POST', body, headers: { 'Idempotency-Key': key } })
}

/** The generation id is the idempotency target; no client key. */
export function revokePushSubscription(generationId: string): Promise<PushRevocationResponse> {
  return request(`/api/v1/push/subscriptions/${encodeURIComponent(generationId)}/revoke`, { method: 'POST', body: {} })
}

// ---------------------------------------------------------------- validation
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
const HEX64 = /^[0-9a-f]{64}$/
const MARKER = /^[A-Za-z0-9_-]{22}$/
const REASONS: readonly string[] = ['user', 'superseded', 'replaced', 'reowned', 'gone']
const STATES: readonly string[] = ['active', 'inactive', 'revoked']
const PROVIDERS: readonly string[] = ['fcm', 'mozilla', 'apple', 'windows']

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}
function nonEmpty(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0
}

/** Current state of one generation: revoked exactly when a reason and time are present. */
export function isValidGeneration(value: unknown): value is PushGeneration {
  if (!isObject(value)) return false
  const g = value
  if (typeof g.id !== 'string' || !UUID.test(g.id) || typeof g.state !== 'string' || !STATES.includes(g.state) ||
    !nonEmpty(g.activated_at) || typeof g.current_session !== 'boolean' ||
    typeof g.provider !== 'string' || !PROVIDERS.includes(g.provider) ||
    (g.user_agent_family !== null && typeof g.user_agent_family !== 'string')) return false
  if (g.state === 'revoked') {
    return typeof g.revoked_reason === 'string' && REASONS.includes(g.revoked_reason) && nonEmpty(g.revoked_at)
  }
  return g.revoked_reason === null && g.revoked_at === null
}

export interface CapturedRegistration {
  endpointHash: string
  vapidKeyId: string
}

/**
 * A registration response is accepted only when the immutable command facts and the current
 * generation agree with each other and with the captured request; anything else stays unknown.
 */
export function isValidRegistrationResponse(value: unknown, captured: CapturedRegistration): value is PushRegistrationResponse {
  if (!isObject(value) || !isObject(value.command) || !isValidGeneration(value.generation)) return false
  const c = value.command, g = value.generation as PushGeneration
  if ((c.result !== 'created' && c.result !== 'already_active') || typeof c.generation_id !== 'string' ||
    !UUID.test(c.generation_id) || !nonEmpty(c.recorded_at)) return false
  if (g.id !== c.generation_id || !g.current_session || g.endpoint_hash !== captured.endpointHash ||
    g.vapid_key_id !== captured.vapidKeyId) return false
  // The marker accompanies exactly an active generation.
  return g.state === 'active' ? typeof value.marker === 'string' && MARKER.test(value.marker) : value.marker === null
}

export function isValidRevocationResponse(value: unknown, generationId: string): value is PushRevocationResponse {
  if (!isObject(value) || !isObject(value.revocation) || !isValidGeneration(value.generation)) return false
  const r = value.revocation, g = value.generation as PushGeneration
  return r.generation_id === generationId && typeof r.reason === 'string' && REASONS.includes(r.reason) &&
    (r.reason === 'gone' ? r.revoked_by === null : typeof r.revoked_by === 'string' && UUID.test(r.revoked_by)) &&
    nonEmpty(r.revoked_at) &&
    g.id === generationId && g.state === 'revoked' && g.revoked_reason === r.reason && g.revoked_at === r.revoked_at
}

export function isValidSubscriptionList(value: unknown): value is PushSubscriptionList {
  if (!isObject(value) || !Array.isArray(value.devices) || !Array.isArray(value.history) ||
    !Number.isInteger(value.device_limit) || value.occupied_slots !== value.devices.length) return false
  const limit = value.device_limit as number
  const devices = value.devices as unknown[], history = value.history as unknown[]
  if (devices.length > limit || history.length > 20 || !devices.every(isValidGeneration) ||
    !history.every(isValidGeneration)) return false
  const ids = new Set([...devices, ...history].map((g) => (g as PushGeneration).id))
  // A device occupies a slot, so it is never revoked; no generation appears twice.
  return ids.size === devices.length + history.length &&
    (devices as PushGeneration[]).every((g) => g.state !== 'revoked')
}

export function isValidConfig(value: unknown): value is PushConfig {
  return isObject(value) && typeof value.subscribe_available === 'boolean' &&
    (value.vapid_public_key === null || (typeof value.vapid_public_key === 'string' && /^[A-Za-z0-9_-]{87}$/.test(value.vapid_public_key))) &&
    Number.isInteger(value.device_limit) && (value.subscribe_available === false || value.vapid_public_key !== null)
}

/** SHA-256 hex of the UTF-8 text (the server's endpoint_hash and vapid_key_id). */
export async function sha256Hex(text: string): Promise<string> {
  const digest = await globalThis.crypto.subtle.digest('SHA-256', new TextEncoder().encode(text))
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, '0')).join('')
}

export { HEX64 as PUSH_HEX64 }
