/**
 * 提醒 store（阶段 2 P2-6 R3，docs/phase2-reminders.md）：内存状态，不做浏览器持久化，没有离线写队列，
 * 与任务、奖励的待确认动作和请求标识完全独立。
 *
 * - 读取：`refresh()` 按游标读完全部页，没有逐目标读取，只在打开详情时调用，不随任务卡片调用。
 *   读不完（出错、响应不合法、游标重复）就是错误，绝不把不完整的结果当作“没有规则”。
 * - 不可变事实与当前状态分开：只有 `current` 进入 `rules`，`mutation` 只用于核对这次动作。旧键重放
 *   的 `current` 可能更新，它决定显示内容，旧设置不会覆盖当前规则。
 * - 较旧的读取不能覆盖较新的确认：seq 更小的不接受；seq 相同时，读取开始得早于该目标最近一次
 *   确认的也不接受（其 next_fire_at 与 fired_this_period 可能已过时）。
 * - 写动作（每个目标至多一个）捕获目标、键、完整请求体、账号与会话代数。结果未确认时保留原键与原内容，
 *   只在用户点击重试时重发，从不自动重放。
 * - 一旦某次发送结果未确认，服务端在键分类之前的拒绝（参数、认证、CSRF、限流、无权限、不存在）都不能
 *   证明原请求失败，继续保留。键分类之后的业务拒绝（TARGET_CLOSED、TIME_PASSED、TIME_INVALID、
 *   IDEMPOTENCY_KEY_REUSED）说明这个键没有写入这份内容，动作结束。服务端的顺序见 ReminderService.h；
 *   TIME_INVALID 在分类前只用于周期 id，那是不可变事实，这样的请求从未写入过，而这里只发送根任务 id。
 * - 成功响应必须与捕获的动作完全一致，否则视为结果未确认。
 * - 注销或身份切换清空全部内容，晚到的响应静默丢弃。
 * - `generationStatus` 只是服务端的配置状态，不证明调度运行过或提醒已送达，也不代表手机推送。只有完整读取且
 *   每页都明确给出同一个布尔值才算已知；缺失、格式不对、页间不一致或读取失败都是 unknown，但规则本身照常读取。
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import {
  listReminderRules,
  setAnniversaryReminder,
  setTaskReminder,
  type AnniversaryReminderRequest,
  type ReminderRequest,
  type ReminderRuleCurrent,
  type ReminderRuleFacts,
  type ReminderRuleResponse,
  type ReminderTargetKind,
  type TaskReminderRequest,
} from '@/api/reminders'
import { newIdempotencyKey } from '@/api/tasks'
import { useSessionStore } from './session'

export type ReminderReadStatus = 'idle' | 'loading' | 'ready' | 'error'
export type ReminderActionStatus = 'submitting' | 'unknown' | 'not_sent'
export type ReminderGenerationStatus = 'enabled' | 'disabled' | 'unknown'

export interface ReminderAction {
  id: string
  kind: ReminderTargetKind
  targetId: string
  payload: ReminderRequest
  key: string
  status: ReminderActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved: boolean
}

export interface ReminderNotice {
  ok: boolean
  text: string
}

export type ReminderResult =
  | { outcome: 'done'; response: ReminderRuleResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

/** Guards a repeating or endless cursor: at 100 per page, far past two members' own rules. */
const MAX_PAGES = 200

/** Codes the server can only reach after classifying the key (ReminderService.h). */
const AFTER_CLASSIFICATION: ReadonlySet<string> = new Set([
  'REMINDER_TARGET_CLOSED', 'REMINDER_TIME_PASSED', 'REMINDER_TIME_INVALID', 'IDEMPOTENCY_KEY_REUSED',
])

const SUPERSEDED_TEXT = '这次保存之前已生效，但之后又被更改过；现在显示的是最新的提醒设置。'
/** What saving an enabled rule may claim depends on what is known about generation. */
const SAVED_TEXT: Record<ReminderGenerationStatus, string> = {
  enabled: '提醒设置已保存。',
  disabled: '提醒设置已保存，但提醒生成目前未开启，暂时不会按它提醒。',
  unknown: '提醒设置已保存，但暂时无法确认提醒生成是否开启，不代表一定会提醒。',
}

export function reminderId(kind: ReminderTargetKind, targetId: string): string {
  return `${kind}:${targetId}`
}

function isText(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0
}

function generationOf(value: unknown): ReminderGenerationStatus {
  return value === true ? 'enabled' : value === false ? 'disabled' : 'unknown'
}

function isCount(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0
}

/** A revision whose columns are exactly what its mode stores. */
function isFacts(value: unknown): value is ReminderRuleFacts {
  if (!value || typeof value !== 'object') return false
  const f = value as Partial<ReminderRuleFacts>
  if (f.action !== 'reminder_rule' || !isText(f.rule_id) || !isCount(f.seq) || f.seq < 1 || !isText(f.target_id) ||
    (f.target_kind !== 'task' && f.target_kind !== 'anniversary') || !isText(f.created_at)) return false
  const none = (x: unknown) => x === null
  if (f.enabled === false) {
    return none(f.mode) && none(f.before_minutes) && none(f.days_before) && none(f.at_time) && none(f.at_local_date) && none(f.offsets)
  }
  if (f.enabled !== true) return false
  if (f.target_kind === 'anniversary') {
    return f.mode === 'anniversary_offsets' && Array.isArray(f.offsets) && f.offsets.length > 0 && f.offsets.every(isCount) &&
      isText(f.at_time) && none(f.before_minutes) && none(f.days_before) && none(f.at_local_date)
  }
  if (!none(f.offsets)) return false
  switch (f.mode) {
    case 'before_deadline': return isCount(f.before_minutes) && none(f.days_before) && none(f.at_time) && none(f.at_local_date)
    case 'local_time': return isCount(f.days_before) && isText(f.at_time) && none(f.before_minutes) && none(f.at_local_date)
    case 'absolute': return isText(f.at_local_date) && isText(f.at_time) && none(f.before_minutes) && none(f.days_before)
    default: return false
  }
}

function isCurrent(value: unknown): value is ReminderRuleCurrent {
  if (!isFacts(value)) return false
  const c = value as ReminderRuleCurrent
  const next = c.next_fire_at
  if (!(next === null || (isText(next) && !Number.isNaN(new Date(next).getTime())))) return false
  // A disabled rule has no next reminder; nothing may suggest otherwise.
  if (!c.enabled && next !== null) return false
  if (c.origin !== 'user' && c.origin !== 'successor') return false
  if (typeof c.needs_reset !== 'boolean') return false
  // Only an enabled inherited rule can need a reset, and it never has a next instant.
  if (c.needs_reset && (c.origin !== 'successor' || !c.enabled || next !== null)) return false
  if (!(c.fired_this_period === null || typeof c.fired_this_period === 'boolean')) return false
  if (!(c.effective_at === null || isText(c.effective_at))) return false
  return c.offset_effective_at === null || Array.isArray(c.offset_effective_at)
}

/** The immutable facts state exactly the captured request. */
function factsMatchRequest(m: ReminderRuleFacts, payload: ReminderRequest): boolean {
  if (m.enabled !== payload.enabled) return false
  if (!payload.enabled) return true
  if (!('mode' in payload)) {
    const days = payload.days_before
    return m.at_time === payload.at_time && m.offsets?.length === days.length && days.every((day, i) => m.offsets?.[i] === day)
  }
  if (m.mode !== payload.mode) return false
  switch (payload.mode) {
    case 'before_deadline': return m.before_minutes === payload.before_minutes
    case 'local_time': return m.days_before === payload.days_before && m.at_time === payload.at_time
    default: return m.at_local_date === payload.at_local_date && m.at_time === payload.at_time
  }
}

/** One seq names one immutable revision, so these must agree. Derived fields such as next_fire_at are not facts. */
function sameRevision(a: ReminderRuleFacts, b: ReminderRuleFacts): boolean {
  const offsetsA = a.offsets
  const offsetsB = b.offsets
  const sameOffsets = offsetsA === null || offsetsB === null
    ? offsetsA === offsetsB
    : offsetsA.length === offsetsB.length && offsetsA.every((day, i) => day === offsetsB[i])
  return sameOffsets && a.action === b.action && a.rule_id === b.rule_id && a.seq === b.seq &&
    a.target_kind === b.target_kind && a.target_id === b.target_id && a.enabled === b.enabled && a.mode === b.mode &&
    a.before_minutes === b.before_minutes && a.days_before === b.days_before && a.at_time === b.at_time &&
    a.at_local_date === b.at_local_date && a.created_at === b.created_at
}

/** The response confirms exactly the captured action; anything else stays unknown. */
export function isUsableReminderResponse(response: unknown, action: Pick<ReminderAction, 'kind' | 'targetId' | 'payload'>):
  response is ReminderRuleResponse {
  if (!response || typeof response !== 'object') return false
  const { mutation, current } = response as Partial<ReminderRuleResponse>
  if (!isFacts(mutation) || !isCurrent(current)) return false
  if (mutation.target_kind !== action.kind || mutation.target_id !== action.targetId ||
    current.target_kind !== action.kind || current.target_id !== action.targetId) return false
  if (!factsMatchRequest(mutation, action.payload)) return false
  // Current is this revision, with identical facts, or a genuinely newer one, never an older one.
  if (current.seq < mutation.seq) return false
  return current.seq > mutation.seq || sameRevision(mutation, current)
}

export const useRemindersStore = defineStore('reminders', () => {
  const rules = ref<Record<string, ReminderRuleCurrent>>({})
  const status = ref<ReminderReadStatus>('idle')
  const generationStatus = ref<ReminderGenerationStatus>('unknown')
  const errorText = ref('')
  const actions = ref<Record<string, ReminderAction>>({})
  const notices = ref<Record<string, ReminderNotice>>({})

  let sessionGeneration = 0
  let readSeq = 0
  // Orders reads against confirmed writes, per target.
  let clock = 0
  const confirmedAt = new Map<string, number>()

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    readSeq += 1
    clock = 0
    confirmedAt.clear()
    rules.value = {}
    status.value = 'idle'
    generationStatus.value = 'unknown'
    errorText.value = ''
    actions.value = {}
    notices.value = {}
  }

  function absorbRead(row: ReminderRuleCurrent, startedAt: number): void {
    const id = reminderId(row.target_kind, row.target_id)
    const known = rules.value[id]
    if (known && (row.seq < known.seq || (row.seq === known.seq && startedAt < (confirmedAt.get(id) ?? 0)))) return
    rules.value = { ...rules.value, [id]: row }
  }

  function absorbConfirmed(row: ReminderRuleCurrent): void {
    const id = reminderId(row.target_kind, row.target_id)
    const known = rules.value[id]
    if (known && row.seq < known.seq) return
    rules.value = { ...rules.value, [id]: row }
    clock += 1
    confirmedAt.set(id, clock)
  }

  /** Reads every page. A newer refresh supersedes this one. */
  async function refresh(): Promise<void> {
    const seq = ++readSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    if (!owner) return
    const startedAt = clock
    status.value = 'loading'
    errorText.value = ''
    const live = () => seq === readSeq && stillCurrent(generation, owner)
    try {
      let cursor: string | null = null
      const seen = new Set<string>()
      let generation: ReminderGenerationStatus | null = null
      for (let page = 0; ; page += 1) {
        if (page >= MAX_PAGES) throw new Error('提醒列表过长，无法读取完整。')
        const response = await listReminderRules(cursor)
        if (!live()) return
        const rows: unknown = response?.reminders
        const next: unknown = response?.next_cursor
        if (!Array.isArray(rows) || !rows.every(isCurrent) || !(next === null || isText(next))) {
          throw new Error('提醒列表响应无效，请稍后重试。')
        }
        for (const row of rows) absorbRead(row, startedAt)
        const stated = generationOf(response?.generation_enabled)
        generation = generation === null || generation === stated ? stated : 'unknown'
        if (next === null) break
        if (seen.has(next)) throw new Error('提醒列表无法读取完整。')
        seen.add(next)
        cursor = next
      }
      generationStatus.value = generation ?? 'unknown'
      status.value = 'ready'
    } catch (error) {
      if (!live()) return
      generationStatus.value = 'unknown'
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  function update(id: string, patch: Partial<ReminderAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function clearNotice(id: string): void {
    const next = { ...notices.value }
    delete next[id]
    notices.value = next
  }

  function keepUnknown(id: string, what: string): ReminderResult {
    update(id, { status: 'unknown', errorText: `${what}之前的保存结果仍未确认，请用原请求重试。` })
    return { outcome: 'unknown' }
  }

  function call(action: ReminderAction): Promise<ReminderRuleResponse> {
    return action.kind === 'task'
      ? setTaskReminder(action.targetId, action.payload as TaskReminderRequest, action.key)
      : setAnniversaryReminder(action.targetId, action.payload as AnniversaryReminderRequest, action.key)
  }

  async function send(id: string): Promise<ReminderResult> {
    const action = actions.value[id]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }
    update(id, { status: 'submitting', errorText: '' })
    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
      update(id, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }
    let response: ReminderRuleResponse
    try {
      const received: unknown = await call(action)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableReminderResponse(received, action)) throw new Error('提醒响应无法确认。')
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (error instanceof ApiError && error.status >= 400 && error.status < 500) {
        if (action.unresolved && !AFTER_CLASSIFICATION.has(error.code)) {
          return keepUnknown(id, `本次重试未确认原结果（${describeError(error)}）。`)
        }
        end(id)
        notices.value = { ...notices.value, [id]: { ok: false, text: describeError(error) } }
        void refresh()
        return { outcome: 'rejected', errorCode: error.code }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复保存。' })
      return { outcome: 'unknown' }
    }
    end(id)
    absorbConfirmed(response.current)
    const superseded = response.current.seq > response.mutation.seq
    notices.value = { ...notices.value, [id]: {
      ok: true,
      text: superseded ? SUPERSEDED_TEXT : response.mutation.enabled ? SAVED_TEXT[generationStatus.value] : '提醒已关闭。',
    } }
    return { outcome: 'done', response }
  }

  /** The payload is captured here and resent unchanged; `id` is the root task id from the API. */
  function save(kind: ReminderTargetKind, targetId: string, payload: ReminderRequest): Promise<ReminderResult> {
    const id = reminderId(kind, targetId)
    if (actions.value[id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    clearNotice(id)
    const captured = JSON.parse(JSON.stringify(payload)) as ReminderRequest
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [id]: { id, kind, targetId, payload: captured, key: newIdempotencyKey(), status: 'not_sent', errorText: '', unresolved: false } }
    return send(id)
  }

  /** Resend an unknown or unsent action with its original key and payload (user click only). */
  function retry(id: string): Promise<ReminderResult> {
    return send(id)
  }

  return { rules, status, generationStatus, errorText, actions, notices, reset, refresh, save, retry, clearNotice }
})
