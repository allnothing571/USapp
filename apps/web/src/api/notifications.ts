import { request } from './client'
import { eventLabel, targetPath, type NotificationTargetKind } from './notification-labels'

export interface Notification {
  id: string
  event_type: string
  /** Null for reminders and the three closed personal security events. */
  actor_id: string | null
  target_kind: NotificationTargetKind
  target_id: string
  created_at: string
  read_at: string | null
}
export interface NotificationPage { notifications: Notification[]; next_cursor: string | null }
export interface ReadReceipt { receipt: { notification_id: string; read_at: string } }

export function listNotifications(cursor?: string | null, unreadOnly = false): Promise<NotificationPage> {
  return request('/api/v1/notifications', { method: 'GET',
    query: { limit: 50, cursor: cursor ?? undefined, unread_only: unreadOnly ? 'true' : 'false' } })
}
export function getUnreadCount(): Promise<{ unread_count: number }> {
  return request('/api/v1/notifications/unread-count', { method: 'GET' })
}
// Notification identity is the idempotency target. No new client key on a retry.
export function readNotification(id: string): Promise<ReadReceipt> {
  return request(`/api/v1/notifications/${encodeURIComponent(id)}/read`, { method: 'POST', body: {} })
}
export function notificationTarget(item: Notification): string {
  return targetPath(item.target_kind, item.target_id)
}
/** Reminders and the closed security registry have no member actor. */
export function hasValidActor(item: Pick<Notification, 'event_type' | 'actor_id'>): boolean {
  return item.event_type === 'reminder.task' || item.event_type === 'reminder.anniversary' || ['security.wallet_adjusted', 'security.redemption_cancelled', 'security.session_created'].includes(item.event_type)
    ? item.actor_id === null
    : typeof item.actor_id === 'string' && item.actor_id.length > 0
}
export function isNotificationForUser(item: Notification, user: string): boolean {
  if (!hasValidActor(item)) return false
  const personal = ['security.wallet_adjusted', 'security.redemption_cancelled', 'security.session_created'].includes(item.event_type)
  return personal ? item.target_kind === 'account' && item.target_id === user : item.target_kind !== 'account'
}
export function notificationLabel(item: Notification): string { return eventLabel(item.event_type) ?? '收到一条通知' }

// ---------------------------------------------------------------- P2-3b remembered choices
export type ChoiceOperation = 'task_publish' | 'anniversary_create' | 'anniversary_edit' | 'message_create' | 'message_reply'
export const CHOICE_OPERATIONS: readonly ChoiceOperation[] =
  ['task_publish', 'anniversary_create', 'anniversary_edit', 'message_create', 'message_reply']
export interface NotificationChoice { enabled: boolean; seq: number | null }
export interface NotificationChoices { choices: Record<ChoiceOperation, NotificationChoice> }

/** `GET /api/v1/notification-choices`: the caller's remembered defaults (read only). */
export function getNotificationChoices(): Promise<NotificationChoices> {
  return request('/api/v1/notification-choices', { method: 'GET' })
}

/** Immutable facts of the original write's notification decision; null for a legacy write. */
export interface OptionalNotificationResult {
  requested: boolean
  created: boolean
  reason: 'delivered_to_inbox' | 'not_requested' | 'no_partner'
  notification_id: string | null
}

/**
 * Validates the `notification` field of a mutation response against the captured choice.
 * - null (a legacy original without a recorded decision) is valid only when the captured choice
 *   was omitted or false: true uses its own fingerprint, so it can never replay a legacy write;
 * - captured explicit boolean: the field must be present; facts' `requested` equals the choice;
 * - nothing captured (no toggle): absent, null, or facts with requested = false;
 * - `no_partner` only where a partnerless write exists (anniversary create / direct edit, and the
 *   D20 memory, shared-plan and album-batch creations): pass allowNoPartner = true there. Task publication and messages require a partner.
 * Anything else is malformed, so the caller keeps the result unknown.
 */
export function isValidNotificationFacts(value: unknown, captured: boolean | undefined,
  allowNoPartner = false): boolean {
  if (value === undefined) return captured === undefined
  if (value === null) return captured !== true
  if (typeof value !== 'object') return false
  const f = value as Partial<OptionalNotificationResult>
  if (typeof f.requested !== 'boolean' || typeof f.created !== 'boolean') return false
  if (f.requested !== (captured === true)) return false
  const id = f.notification_id
  if (!(id === null || (typeof id === 'string' && id.length > 0))) return false
  switch (f.reason) {
    case 'delivered_to_inbox': return f.requested && f.created && typeof id === 'string'
    case 'no_partner': return allowNoPartner && f.requested && !f.created && id === null
    case 'not_requested': return !f.requested && !f.created && id === null
    default: return false
  }
}
