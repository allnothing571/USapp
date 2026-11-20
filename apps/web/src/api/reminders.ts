/**
 * 提醒规则（阶段 2 P2-6 R3、R4，契约 0.23.0，docs/phase2-reminders.md）：只读写调用者自己的规则。
 *
 * 全部经共享的 `request()`：同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里。写请求必须带
 * `Idempotency-Key`；键由 reminders store 为每次动作生成，只在显式重试同一次动作时复用。
 * 关闭提醒的请求体必须恰好是 {"enabled": false}。
 */
import { request } from './client'

export type ReminderTargetKind = 'task' | 'anniversary'
export type ReminderMode = 'before_deadline' | 'local_time' | 'absolute' | 'anniversary_offsets'

export type TaskReminderRequest =
  | { enabled: false }
  | { enabled: true; mode: 'before_deadline'; before_minutes: number }
  | { enabled: true; mode: 'local_time'; days_before: number; at_time: string }
  | { enabled: true; mode: 'absolute'; at_local_date: string; at_time: string }

export type AnniversaryReminderRequest =
  | { enabled: false }
  | { enabled: true; days_before: number[]; at_time: string }

export type ReminderRequest = TaskReminderRequest | AnniversaryReminderRequest

/** 一次修订的不可变事实。同键重放原样返回。 */
export interface ReminderRuleFacts {
  action: 'reminder_rule'
  rule_id: string
  seq: number
  target_kind: ReminderTargetKind
  target_id: string
  enabled: boolean
  mode: ReminderMode | null
  before_minutes: number | null
  days_before: number | null
  at_time: string | null
  at_local_date: string | null
  offsets: number[] | null
  created_at: string
}

/** 目标当前的规则与派生状态，与任何一次写入的事实分开。 */
export interface ReminderRuleCurrent extends ReminderRuleFacts {
  /** successor: copied from the predecessor when an edit was approved. */
  origin: 'user' | 'successor'
  /** An inherited rule that no longer fits: the choice is kept, nothing fires until it is set again. */
  needs_reset: boolean
  effective_at: string | null
  offset_effective_at: { days_before: number; effective_at: string }[] | null
  next_fire_at: string | null
  fired_this_period: boolean | null
}

export interface ReminderRuleResponse {
  mutation: ReminderRuleFacts
  current: ReminderRuleCurrent
}

export interface ReminderRuleList {
  reminders: ReminderRuleCurrent[]
  next_cursor: string | null
  /** Configuration status, not proof that anything ran or was delivered. Optional here: an older server omits it. */
  generation_enabled?: boolean
}

export const REMINDER_PAGE_SIZE = 100

/** `GET /api/v1/reminders?cursor=&limit=`：只有调用者自己的规则，含已关闭的；游标原样回传。 */
export function listReminderRules(cursor?: string | null, limit = REMINDER_PAGE_SIZE): Promise<ReminderRuleList> {
  return request<ReminderRuleList>('/api/v1/reminders', { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

/** `POST /api/v1/tasks/{id}/reminder`：201 新修订，200 同键重放。`id` 是任务响应里的根任务 id。 */
export function setTaskReminder(taskId: string, body: TaskReminderRequest, idempotencyKey: string): Promise<ReminderRuleResponse> {
  return request<ReminderRuleResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}/reminder`, {
    method: 'POST', body, headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `POST /api/v1/anniversaries/{id}/reminder`：201 新修订，200 同键重放。 */
export function setAnniversaryReminder(anniversaryId: string, body: AnniversaryReminderRequest, idempotencyKey: string): Promise<ReminderRuleResponse> {
  return request<ReminderRuleResponse>(`/api/v1/anniversaries/${encodeURIComponent(anniversaryId)}/reminder`, {
    method: 'POST', body, headers: { 'Idempotency-Key': idempotencyKey },
  })
}
