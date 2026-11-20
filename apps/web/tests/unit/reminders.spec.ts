/**
 * P2-6 R3 reminders (web): schedule bounds and ownership; the exact disable body; current state kept apart from
 * immutable facts, including a replay of an older save; the unknown-retry matrix; malformed responses; stale reads;
 * identity and target changes; pagination; independence from task and reward actions; null actors and labels.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/reminders', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/reminders')>(),
  listReminderRules: vi.fn(), setTaskReminder: vi.fn(), setAnniversaryReminder: vi.fn(),
}))
vi.mock('@/api/tasks', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/tasks')>(),
  newIdempotencyKey: vi.fn(), completeTask: vi.fn(), listTasks: vi.fn(), getTask: vi.fn(),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/rewards', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/rewards')>(), redeemRewardProduct: vi.fn(),
}))
vi.mock('@/api/anniversaries', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/anniversaries')>(), getAnniversary: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('@/api/notifications', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/notifications')>(),
  getUnreadCount: vi.fn(), listNotifications: vi.fn(), readNotification: vi.fn(),
}))
vi.mock('vue-router', async importOriginal => ({
  ...await importOriginal<typeof import('vue-router')>(), useRouter: () => ({ push: vi.fn() }),
}))

import { getAnniversary } from '@/api/anniversaries'
import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { eventLabel, targetPath } from '@/api/notification-labels'
import { hasValidActor, listNotifications, notificationLabel, notificationTarget, readNotification, type Notification } from '@/api/notifications'
import {
  beijingInstant,
  buildAnniversaryReminder,
  buildTaskReminder,
  nextFireText,
  reminderText,
  taskReminderAccess,
  taskReminderBounds,
  type TaskReminderDraft,
} from '@/api/reminder-rules'
import {
  listReminderRules,
  setAnniversaryReminder,
  setTaskReminder,
  type ReminderRequest,
  type ReminderRuleCurrent,
  type ReminderRuleFacts,
  type ReminderRuleResponse,
} from '@/api/reminders'
import { redeemRewardProduct } from '@/api/rewards'
import { completeTask, getTask, listTasks, newIdempotencyKey } from '@/api/tasks'
import type { Task } from '@/api/types'
import { getWallet } from '@/api/wallet'
import ReminderControls from '@/components/ReminderControls.vue'
import { destinationFor, notificationFor, parsePushPayload } from '@/pwa/push-sw'
import { useNotificationsStore } from '@/stores/notifications'
import { useRewardsStore } from '@/stores/rewards'
import { isUsableReminderResponse, reminderId, useRemindersStore } from '@/stores/reminders'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'
import AnniversaryDetailView from '@/views/AnniversaryDetailView.vue'
import NotificationsView from '@/views/NotificationsView.vue'
import TasksView from '@/views/TasksView.vue'

const NEXT = '2026-11-10T13:00:00.000000Z'

function facts(overrides: Partial<ReminderRuleFacts> = {}): ReminderRuleFacts {
  return {
    action: 'reminder_rule', rule_id: 'r1', seq: 1, target_kind: 'task', target_id: 't1', enabled: true,
    mode: 'before_deadline', before_minutes: 30, days_before: null, at_time: null, at_local_date: null, offsets: null,
    created_at: '2026-11-02T00:00:00.000000Z', ...overrides,
  }
}
function current(overrides: Partial<ReminderRuleCurrent> = {}): ReminderRuleCurrent {
  return { ...facts(), origin: 'user', needs_reset: false, effective_at: '2026-11-02T00:00:00.000000Z', offset_effective_at: null, next_fire_at: NEXT, fired_this_period: false, ...overrides }
}
const BEFORE_30: ReminderRequest = { enabled: true, mode: 'before_deadline', before_minutes: 30 }
const OFF_RULE = { enabled: false, mode: null, before_minutes: null } as const

function saved(overrides: Partial<ReminderRuleResponse> = {}): ReminderRuleResponse {
  return { mutation: facts(), current: current(), ...overrides }
}
function disabledRule(seq = 2): ReminderRuleResponse {
  const row = { ...OFF_RULE, seq, rule_id: `r${seq}` }
  return { mutation: facts(row), current: current({ ...row, effective_at: null, next_fire_at: null }) }
}
function task(overrides: Partial<Task> = {}): Task {
  return {
    id: 't1', title: '任务', description: '', creator_id: 'partner', assignee_id: 'self', reward_flowers: 1, reward_sunshine: 0,
    deadline: { local_date: '2026-11-10', local_time: null, at: '2026-11-10T16:00:00.000000Z' }, status: 'open',
    completed_at: null, created_at: '2026-11-01T00:00:00.000000Z', ...overrides,
  }
}
function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (error: unknown) => void
  const promise = new Promise<T>((a, b) => { resolve = a; reject = b })
  return { promise, resolve, reject }
}
function login(id = 'self') {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`, user: { id, email: `${id}@example.com`, display_name: id, created_at: '' },
    space: { id: 'space', name: 'test', timezone: 'Asia/Shanghai', members: [], member_count: 2, created_at: '' },
  })
  setCsrfToken(`csrf-${id}`)
}
function api(status: number, code: string): ApiError {
  return new ApiError(code as never, code, status)
}
const draft = (overrides: Partial<TaskReminderDraft>): TaskReminderDraft =>
  ({ mode: 'before_deadline', beforeMinutes: '30', daysBefore: '0', atTime: '21:00', atLocalDate: '', ...overrides })

let wrapper: VueWrapper | undefined
let keys = 0
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  keys = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${++keys}`)
  vi.mocked(listReminderRules).mockResolvedValue({ reminders: [], next_cursor: null, generation_enabled: true })
  login()
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
})

describe('rules', () => {
  const daily = taskReminderBounds({ schedule_type: 'daily', deadline: null })
  const weekly = taskReminderBounds({ schedule_type: 'weekly', deadline: null })
  const custom = taskReminderBounds({ schedule_type: 'custom', deadline: task().deadline })
  const dated = taskReminderBounds({ deadline: task().deadline })
  const open = taskReminderBounds({ deadline: null })

  it.each([
    ['daily 1440', daily, '1440', true], ['daily 1441', daily, '1441', false], ['daily 0', daily, '0', false],
    ['weekly 10080', weekly, '10080', true], ['weekly 10081', weekly, '10081', false],
    ['custom 10080', custom, '10080', true], ['custom 10081', custom, '10081', false],
    ['one-off 10080', dated, '10080', true], ['one-off 10081', dated, '10081', false],
    ['fraction', dated, '1.5', false], ['negative', dated, '-1', false],
  ])('before_minutes: %s', (_name, bounds, minutes, ok) => {
    expect(buildTaskReminder(draft({ beforeMinutes: minutes }), bounds).ok).toBe(ok)
  })
  it.each([
    ['daily 0', daily, '0', true], ['daily 1', daily, '1', false],
    ['weekly 6', weekly, '6', true], ['weekly 7', weekly, '7', false],
    ['custom 6', custom, '6', true], ['custom 7', custom, '7', false],
    ['one-off 7', dated, '7', true], ['one-off 8', dated, '8', false],
  ])('days_before: %s', (_name, bounds, days, ok) => {
    expect(buildTaskReminder(draft({ mode: 'local_time', daysBefore: days }), bounds).ok).toBe(ok)
  })
  it('a one-off with no deadline allows only an absolute date and time', () => {
    expect(open.modes).toEqual(['absolute'])
    expect(buildTaskReminder(draft({}), open).ok).toBe(false)
    expect(buildTaskReminder(draft({ mode: 'local_time' }), open).ok).toBe(false)
    expect(buildTaskReminder(draft({ mode: 'absolute', atLocalDate: '2026-13-01' }), open).ok).toBe(false)
    expect(buildTaskReminder(draft({ mode: 'absolute', atLocalDate: '2026-12-01' }), open)).toEqual({
      ok: true, request: { enabled: true, mode: 'absolute', at_local_date: '2026-12-01', at_time: '21:00' },
    })
    expect(dated.modes).not.toContain('absolute')
  })
  it('requests carry only their own mode fields, with a minute-precision time', () => {
    expect(buildTaskReminder(draft({}), dated)).toEqual({ ok: true, request: { enabled: true, mode: 'before_deadline', before_minutes: 30 } })
    expect(buildTaskReminder(draft({ mode: 'local_time', daysBefore: '2', atTime: '22:00' }), dated)).toEqual({
      ok: true, request: { enabled: true, mode: 'local_time', days_before: 2, at_time: '22:00' },
    })
    expect(buildTaskReminder(draft({ mode: 'local_time', atTime: '24:00' }), dated).ok).toBe(false)
  })
  it('anniversary offsets: one to three from the approved set, sorted and distinct', () => {
    expect(buildAnniversaryReminder([7, 1, 7], '09:00')).toEqual({ ok: true, request: { enabled: true, days_before: [1, 7], at_time: '09:00' } })
    expect(buildAnniversaryReminder([], '09:00').ok).toBe(false)
    expect(buildAnniversaryReminder([0, 1, 3, 7], '09:00').ok).toBe(false)
    expect(buildAnniversaryReminder([2], '09:00').ok).toBe(false)
    expect(buildAnniversaryReminder([1], '9:00').ok).toBe(false)
  })
  it('ownership and closed targets follow the API', () => {
    expect(taskReminderAccess(task(), 'partner').visible).toBe(false)
    expect(taskReminderAccess(task(), 'self')).toMatchObject({ visible: true, canEnable: true, canDisable: true })
    expect(taskReminderAccess(task({ status: 'completed' }), 'self')).toMatchObject({ canEnable: false, canDisable: true })
    expect(taskReminderAccess(task({ status: 'expired' }), 'self')).toMatchObject({ canEnable: false, canDisable: true })
    expect(taskReminderAccess(task({ status: 'cancelled' }), 'self')).toMatchObject({ canEnable: false, canDisable: false })
    expect(taskReminderAccess(task({ status: 'superseded' }), 'self')).toMatchObject({ canEnable: false, canDisable: false })
    expect(taskReminderAccess(task({ status: 'open', closure: { reason: 'cancelled', closed_at: '', replaced_by: null } }), 'self').canDisable).toBe(false)
    // One finished period does not end a recurring rule on its root.
    expect(taskReminderAccess(task({ schedule_type: 'daily', status: 'completed' }), 'self').canEnable).toBe(true)
    // R4: the old root points to the successor, which holds the copy.
    expect(taskReminderAccess(task({ status: 'superseded' }), 'self').closedText).toContain('原样带到新任务')
  })
  it('wording: Beijing time, and null next_fire_at invents nothing', () => {
    expect(beijingInstant('2026-11-10T13:00:00.000Z')).toContain('21:00')
    expect(beijingInstant('nonsense')).toBeNull()
    expect(nextFireText(current())).toContain('北京时间')
    expect(nextFireText(current({ next_fire_at: null }))).toBe('按当前设置，没有下一次提醒时间。')
    expect(nextFireText(undefined)).toBe('没有开启提醒。')
    expect(nextFireText(current({ origin: 'successor', needs_reset: true, next_fire_at: null }))).toBe('需要重新设置后才会提醒。')
    expect(reminderText({ enabled: true, days_before: [0, 7], at_time: '09:00' })).toBe('当天、提前 7 天，北京时间 09:00')
    expect(reminderText(facts({ target_kind: 'anniversary', mode: 'anniversary_offsets', before_minutes: null, offsets: [1], at_time: '09:00' }))).toBe('提前 1 天，北京时间 09:00')
  })
})

describe('response validation', () => {
  const action = { kind: 'task' as const, targetId: 't1', payload: BEFORE_30 }
  it('accepts exact facts', () => {
    expect(isUsableReminderResponse(saved(), action)).toBe(true)
    expect(isUsableReminderResponse(disabledRule(), { ...action, payload: { enabled: false } })).toBe(true)
    // Derived fields are not immutable facts: any valid values pass at an equal seq.
    expect(isUsableReminderResponse(saved({ current: current({ next_fire_at: null, fired_this_period: true, effective_at: null }) }), action)).toBe(true)
    // A genuinely newer revision may have any settings, the opposite enabled state included.
    expect(isUsableReminderResponse(saved({ current: current({ seq: 2, rule_id: 'r2', before_minutes: 90 }) }), action)).toBe(true)
    expect(isUsableReminderResponse(saved({ current: current({ ...OFF_RULE, seq: 2, rule_id: 'r2', effective_at: null, next_fire_at: null }) }), action)).toBe(true)
    expect(isUsableReminderResponse(
      { mutation: facts({ target_kind: 'anniversary', target_id: 'a1', mode: 'anniversary_offsets', before_minutes: null, offsets: [1, 7], at_time: '09:00' }),
        current: current({ target_kind: 'anniversary', target_id: 'a1', mode: 'anniversary_offsets', before_minutes: null, offsets: [1, 7], at_time: '09:00', fired_this_period: null }) },
      { kind: 'anniversary', targetId: 'a1', payload: { enabled: true, days_before: [1, 7], at_time: '09:00' } })).toBe(true)
  })
  it.each<[string, unknown]>([
    ['null', null], ['a string', 'ok'], ['no current', { mutation: facts() }], ['no mutation', { current: current() }],
    ['another target in facts', saved({ mutation: facts({ target_id: 't2' }) })],
    ['another target in current', saved({ current: current({ target_id: 't2' }) })],
    ['another kind', saved({ mutation: facts({ target_kind: 'anniversary' }) })],
    ['other minutes', saved({ mutation: facts({ before_minutes: 31 }) })],
    ['other mode', saved({ mutation: facts({ mode: 'local_time', before_minutes: null, days_before: 0, at_time: '09:00' }) })],
    ['disabled facts', saved({ mutation: facts(OFF_RULE) })],
    ['stray column', saved({ mutation: facts({ at_time: '09:00' }) })],
    ['wrong action', saved({ mutation: { ...facts(), action: 'other' as never } })],
    ['seq zero', saved({ mutation: facts({ seq: 0 }) })],
    ['current older than the write', saved({ mutation: facts({ seq: 3 }), current: current({ seq: 2 }) })],
    ['same seq, other rule', saved({ current: current({ rule_id: 'other' }) })],
    ['same seq, current disabled', saved({ current: current({ ...OFF_RULE, effective_at: null, next_fire_at: null }) })],
    ['same seq, other minutes', saved({ current: current({ before_minutes: 90 }) })],
    ['same seq, other mode and terms', saved({ current: current({ mode: 'local_time', before_minutes: null, days_before: 1, at_time: '21:00' }) })],
    ['same seq, other created_at', saved({ current: current({ created_at: '2026-11-03T00:00:00.000000Z' }) })],
    ['invalid next', saved({ current: current({ next_fire_at: 'soon' }) })],
    ['next on a disabled rule', { mutation: facts(), current: current({ ...OFF_RULE, seq: 2, rule_id: 'r2' }) }],
    ['fired not boolean', saved({ current: current({ fired_this_period: 'yes' as never }) })],
    ['no origin', saved({ current: { ...current(), origin: undefined as never } })],
    ['unknown origin', saved({ current: current({ origin: 'imported' as never }) })],
    ['no needs_reset', saved({ current: { ...current(), needs_reset: undefined as never } })],
    ['needs_reset on a user rule', saved({ current: current({ needs_reset: true, next_fire_at: null }) })],
    ['needs_reset with a next instant', saved({ current: current({ origin: 'successor', needs_reset: true }) })],
    ['needs_reset on a disabled rule', { mutation: facts(), current: current({ ...OFF_RULE, seq: 2, rule_id: 'r2', origin: 'successor', needs_reset: true, effective_at: null, next_fire_at: null }) }],
  ])('refuses %s', (_name, response) => {
    expect(isUsableReminderResponse(response, action)).toBe(false)
  })
})

describe('store: current state', () => {
  it('a disable sends exactly {"enabled": false}', async () => {
    vi.mocked(setTaskReminder).mockResolvedValue(disabledRule())
    const store = useRemindersStore()
    await store.save('task', 't1', { enabled: false })
    expect(vi.mocked(setTaskReminder).mock.calls[0]).toEqual(['t1', { enabled: false }, 'key-1'])
    expect(JSON.stringify(vi.mocked(setTaskReminder).mock.calls[0]?.[1])).toBe('{"enabled":false}')
    expect(store.notices[reminderId('task', 't1')]?.text).toBe('提醒已关闭。')
  })
  it('an equal seq contradiction stays unknown with its key and payload; a valid same-key retry resolves it', async () => {
    const off = { ...OFF_RULE, effective_at: null, next_fire_at: null }
    vi.mocked(setTaskReminder)
      .mockResolvedValueOnce(saved({ current: current(off) }))
      .mockResolvedValueOnce(saved({ current: current({ before_minutes: 90 }) }))
      .mockResolvedValueOnce(saved({ current: current({ next_fire_at: null, fired_this_period: true }) }))
    const store = useRemindersStore()
    expect((await store.save('task', 't1', BEFORE_30)).outcome).toBe('unknown')
    expect(store.rules['task:t1']).toBeUndefined()
    expect(store.actions['task:t1']).toMatchObject({ key: 'key-1', payload: BEFORE_30, status: 'unknown', unresolved: true })
    expect((await store.retry('task:t1')).outcome).toBe('unknown')
    expect(store.rules['task:t1']).toBeUndefined()
    expect(store.actions['task:t1']).toMatchObject({ key: 'key-1', payload: BEFORE_30, unresolved: true })
    expect((await store.retry('task:t1')).outcome).toBe('done')
    for (const args of vi.mocked(setTaskReminder).mock.calls) expect(args).toEqual(['t1', BEFORE_30, 'key-1'])
    expect(store.actions['task:t1']).toBeUndefined()
    expect(store.rules['task:t1']).toMatchObject({ enabled: true, before_minutes: 30, next_fire_at: null, fired_this_period: true })
  })
  it('a replay of an older save does not restore its settings over the newer rule', async () => {
    const newer = current({ seq: 5, rule_id: 'r5', before_minutes: 90 })
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network'))
      .mockResolvedValueOnce({ mutation: facts(), current: newer })
    const store = useRemindersStore()
    await store.save('task', 't1', BEFORE_30)
    const result = await store.retry('task:t1')
    expect(result.outcome).toBe('done')
    const rule = store.rules['task:t1']
    expect(rule?.seq).toBe(5)
    expect(rule?.before_minutes).toBe(90)
    expect(store.notices['task:t1']?.text).toContain('之后又被更改过')
    expect(store.actions['task:t1']).toBeUndefined()
  })
  it('a confirmed response older than what is known changes nothing', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current({ seq: 7, rule_id: 'r7', before_minutes: 90 })], next_cursor: null, generation_enabled: true })
    const store = useRemindersStore()
    await store.refresh()
    vi.mocked(setTaskReminder).mockResolvedValue(saved())
    await store.save('task', 't1', BEFORE_30)
    expect(store.rules['task:t1']?.seq).toBe(7)
  })
  it('a stale read cannot overwrite a newer confirmed rule, at a lower or an equal seq', async () => {
    const stale = deferred<{ reminders: ReminderRuleCurrent[]; next_cursor: null, generation_enabled: true }>()
    vi.mocked(listReminderRules).mockReturnValueOnce(stale.promise)
    const store = useRemindersStore()
    const reading = store.refresh()
    vi.mocked(setTaskReminder).mockResolvedValue(saved({ mutation: facts({ seq: 2, rule_id: 'r2' }), current: current({ seq: 2, rule_id: 'r2', fired_this_period: true, next_fire_at: null }) }))
    await store.save('task', 't1', BEFORE_30)
    // Same seq: its derived state predates the confirmation.
    stale.resolve({ reminders: [current({ seq: 2, rule_id: 'r2', fired_this_period: false, next_fire_at: NEXT })], next_cursor: null, generation_enabled: true })
    await reading
    expect(store.rules['task:t1']?.fired_this_period).toBe(true)
    expect(store.rules['task:t1']?.next_fire_at).toBeNull()
    // A read started after the confirmation does replace it.
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current({ seq: 2, rule_id: 'r2', fired_this_period: false, next_fire_at: NEXT })], next_cursor: null, generation_enabled: true })
    await store.refresh()
    expect(store.rules['task:t1']?.next_fire_at).toBe(NEXT)
  })
  it('a read superseded by a newer read is dropped', async () => {
    const old = deferred<{ reminders: ReminderRuleCurrent[]; next_cursor: null, generation_enabled: true }>()
    vi.mocked(listReminderRules).mockReturnValueOnce(old.promise).mockResolvedValueOnce({ reminders: [current({ seq: 4 })], next_cursor: null, generation_enabled: true })
    const store = useRemindersStore()
    const first = store.refresh()
    await store.refresh()
    old.resolve({ reminders: [current({ seq: 9 })], next_cursor: null, generation_enabled: true })
    await first
    expect(store.rules['task:t1']?.seq).toBe(4)
    expect(store.status).toBe('ready')
  })
})

describe('store: request safety', () => {
  it('an unconfirmed result keeps the same key and exact payload; nothing retries by itself', async () => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network')).mockRejectedValueOnce(api(503, 'RESULT_UNKNOWN')).mockResolvedValueOnce(saved())
    const store = useRemindersStore()
    const payload: ReminderRequest = { enabled: true, mode: 'before_deadline', before_minutes: 30 }
    expect((await store.save('task', 't1', payload)).outcome).toBe('unknown')
    ;(payload as { before_minutes: number }).before_minutes = 999  // later changes to the source do not reach the captured request
    await flushPromises()
    expect(setTaskReminder).toHaveBeenCalledTimes(1)
    expect(store.actions['task:t1']).toMatchObject({ status: 'unknown', unresolved: true })
    expect((await store.retry('task:t1')).outcome).toBe('unknown')
    expect((await store.retry('task:t1')).outcome).toBe('done')
    for (const args of vi.mocked(setTaskReminder).mock.calls) expect(args).toEqual(['t1', BEFORE_30, 'key-1'])
  })
  it('malformed success stays unknown with the same key', async () => {
    vi.mocked(setTaskReminder).mockResolvedValueOnce({ mutation: facts({ before_minutes: 31 }), current: current() }).mockResolvedValueOnce(saved())
    const store = useRemindersStore()
    expect((await store.save('task', 't1', BEFORE_30)).outcome).toBe('unknown')
    expect(store.rules['task:t1']).toBeUndefined()
    expect((await store.retry('task:t1')).outcome).toBe('done')
    expect(vi.mocked(setTaskReminder).mock.calls[1]?.[2]).toBe('key-1')
  })
  it.each([
    [400, 'VALIDATION_ERROR'], [401, 'UNAUTHENTICATED'], [403, 'CSRF_TOKEN_INVALID'], [403, 'ORIGIN_NOT_ALLOWED'],
    [403, 'REMINDER_NOT_ASSIGNEE'], [404, 'TASK_NOT_FOUND'], [404, 'SPACE_NOT_FOUND'], [429, 'RATE_LIMITED'],
  ])('after an unknown attempt, pre-classification %i %s does not discard the action', async (status, code) => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network')).mockRejectedValueOnce(api(status, code)).mockResolvedValueOnce(saved())
    const store = useRemindersStore()
    await store.save('task', 't1', BEFORE_30)
    expect((await store.retry('task:t1')).outcome).toBe('unknown')
    expect(store.actions['task:t1']).toMatchObject({ key: 'key-1', payload: BEFORE_30, status: 'unknown', unresolved: true })
    expect((await store.retry('task:t1')).outcome).toBe('done')
    expect(vi.mocked(setTaskReminder).mock.calls[2]).toEqual(['t1', BEFORE_30, 'key-1'])
  })
  it.each([
    [409, 'REMINDER_TARGET_CLOSED'], [409, 'REMINDER_TIME_PASSED'], [400, 'REMINDER_TIME_INVALID'], [409, 'IDEMPOTENCY_KEY_REUSED'],
  ])('after an unknown attempt, post-classification %i %s resolves it: the key wrote nothing', async (status, code) => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network')).mockRejectedValueOnce(api(status, code))
    const store = useRemindersStore()
    await store.save('task', 't1', BEFORE_30)
    expect(await store.retry('task:t1')).toEqual({ outcome: 'rejected', errorCode: code })
    expect(store.actions['task:t1']).toBeUndefined()
    expect(store.notices['task:t1']?.ok).toBe(false)
    expect(store.rules['task:t1']).toBeUndefined()
  })
  it('a first definitive rejection ends the action and re-reads', async () => {
    vi.mocked(setTaskReminder).mockRejectedValue(api(409, 'REMINDER_TIME_PASSED'))
    const store = useRemindersStore()
    expect((await store.save('task', 't1', BEFORE_30)).outcome).toBe('rejected')
    expect(store.actions['task:t1']).toBeUndefined()
    await flushPromises()
    expect(listReminderRules).toHaveBeenCalledTimes(1)
  })
  it('offline never sends, and a later offline retry does not lose an unresolved action', async () => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new OfflineMutationBlockedError())
    const store = useRemindersStore()
    expect((await store.save('task', 't1', BEFORE_30)).outcome).toBe('not_sent')
    expect(store.actions['task:t1']).toMatchObject({ status: 'not_sent', unresolved: false, key: 'key-1' })
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network')).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect((await store.retry('task:t1')).outcome).toBe('unknown')
    expect((await store.retry('task:t1')).outcome).toBe('unknown')
    expect(store.actions['task:t1']).toMatchObject({ unresolved: true, key: 'key-1' })
  })
  it('one action per target; a double save starts nothing new', async () => {
    const pending = deferred<ReminderRuleResponse>()
    vi.mocked(setTaskReminder).mockReturnValue(pending.promise)
    const store = useRemindersStore()
    const first = store.save('task', 't1', BEFORE_30)
    expect((await store.save('task', 't1', { enabled: false })).outcome).toBe('busy')
    await flushPromises()
    pending.resolve(saved())
    await first
    expect(setTaskReminder).toHaveBeenCalledTimes(1)
  })
  it('a late response after an identity reset is discarded, on either account', async () => {
    const pending = deferred<ReminderRuleResponse>()
    vi.mocked(setTaskReminder).mockReturnValue(pending.promise)
    const store = useRemindersStore()
    const write = store.save('task', 't1', BEFORE_30)
    await flushPromises()
    useSessionStore().resetBusinessState()
    login('other')
    expect(store.actions).toEqual({})
    pending.resolve(saved())
    expect((await write).outcome).toBe('discarded')
    expect(store.rules).toEqual({})
    expect(store.notices).toEqual({})
  })
  it('a late read after an account change is discarded', async () => {
    const old = deferred<{ reminders: ReminderRuleCurrent[]; next_cursor: null, generation_enabled: true }>()
    vi.mocked(listReminderRules).mockReturnValueOnce(old.promise)
    const store = useRemindersStore()
    const reading = store.refresh()
    useSessionStore().resetBusinessState()
    login('other')
    old.resolve({ reminders: [current()], next_cursor: null, generation_enabled: true })
    await reading
    expect(store.rules).toEqual({})
    expect(store.status).toBe('idle')
  })
})

describe('store: pagination', () => {
  it('reads every page with the cursor returned', async () => {
    vi.mocked(listReminderRules)
      .mockResolvedValueOnce({ reminders: [current({ target_id: 't1' })], next_cursor: 'c1', generation_enabled: true })
      .mockResolvedValueOnce({ reminders: [current({ target_id: 't2' })], next_cursor: 'c2', generation_enabled: true })
      .mockResolvedValueOnce({ reminders: [current({ target_id: 't3' })], next_cursor: null, generation_enabled: true })
    const store = useRemindersStore()
    await store.refresh()
    expect(vi.mocked(listReminderRules).mock.calls.map(args => args[0])).toEqual([null, 'c1', 'c2'])
    expect(Object.keys(store.rules).sort()).toEqual(['task:t1', 'task:t2', 'task:t3'])
    expect(store.status).toBe('ready')
  })
  it('a failed later page is an error, never a silently short list', async () => {
    vi.mocked(listReminderRules).mockResolvedValueOnce({ reminders: [current()], next_cursor: 'c1', generation_enabled: true }).mockRejectedValueOnce(api(500, 'INTERNAL_ERROR'))
    const store = useRemindersStore()
    await store.refresh()
    expect(store.status).toBe('error')
    expect(store.errorText).not.toBe('')
    expect(store.rules['task:t1']).toBeDefined()
  })
  it('a repeating cursor stops as an error', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [], next_cursor: 'same', generation_enabled: true })
    const store = useRemindersStore()
    await store.refresh()
    expect(store.status).toBe('error')
    expect(listReminderRules).toHaveBeenCalledTimes(2)
  })
  it.each<[string, unknown]>([
    ['no list', { next_cursor: null }], ['bad row', { reminders: [{ id: 'x' }], next_cursor: null }],
    ['no cursor field', { reminders: [] }], ['empty cursor', { reminders: [], next_cursor: '' }],
  ])('a malformed page is an error: %s', async (_name, page) => {
    vi.mocked(listReminderRules).mockResolvedValue(page as never)
    const store = useRemindersStore()
    await store.refresh()
    expect(store.status).toBe('error')
    expect(store.rules).toEqual({})
  })
})

describe('independence from task and reward actions', () => {
  it('pending task and reward actions survive reminder reads and saves unchanged, and no key is shared', async () => {
    vi.mocked(completeTask).mockRejectedValue(new Error('network'))
    vi.mocked(redeemRewardProduct).mockRejectedValue(new Error('network'))
    const tasks = useTasksStore()
    const rewards = useRewardsStore()
    await tasks.complete(task())
    await rewards.redeem({ id: 'p1', title: '奖励', version: 1 })
    const taskBefore = JSON.stringify(tasks.completionActions.t1)
    const rewardBefore = JSON.stringify(rewards.actions['redeem:p1'])
    expect(tasks.completionActions.t1).toMatchObject({ status: 'unknown', unresolved: true })
    expect(rewards.actions['redeem:p1']).toMatchObject({ status: 'unknown', unresolved: true })

    const store = useRemindersStore()
    await store.refresh()
    vi.mocked(listReminderRules).mockRejectedValueOnce(api(500, 'INTERNAL_ERROR'))
    await store.refresh()
    vi.mocked(setTaskReminder).mockResolvedValueOnce(saved())
    await store.save('task', 't1', BEFORE_30)
    vi.mocked(setTaskReminder).mockRejectedValueOnce(api(409, 'REMINDER_TIME_PASSED'))
    await store.save('task', 't1', BEFORE_30)
    await flushPromises()
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network'))
    await store.save('task', 't1', { enabled: false })

    expect(JSON.stringify(tasks.completionActions.t1)).toBe(taskBefore)
    expect(JSON.stringify(rewards.actions['redeem:p1'])).toBe(rewardBefore)
    expect(completeTask).toHaveBeenCalledTimes(1)
    expect(redeemRewardProduct).toHaveBeenCalledTimes(1)
    const reminderKey = store.actions['task:t1']?.key
    expect(reminderKey).toBeTruthy()
    expect([tasks.completionActions.t1?.key, rewards.actions['redeem:p1']?.key]).not.toContain(reminderKey)
    // And the other way round: retrying the task action leaves the reminder action untouched.
    const held = JSON.stringify(store.actions['task:t1'])
    await tasks.retryCompletion('t1')
    expect(JSON.stringify(store.actions['task:t1'])).toBe(held)
  })
})

describe('controls', () => {
  function mountTask(props: Partial<Task> = {}) {
    wrapper = mount(ReminderControls, { props: { task: task(props) } })
    return flushPromises()
  }
  it('is off by default: reading writes nothing, and no rule is created until asked', async () => {
    await mountTask()
    expect(listReminderRules).toHaveBeenCalledTimes(1)
    expect(setTaskReminder).not.toHaveBeenCalled()
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('未开启')
    expect(wrapper?.find('[data-test="reminder-disable"]').exists()).toBe(false)
  })
  it('does not say "off" before a complete read', async () => {
    vi.mocked(listReminderRules).mockRejectedValue(api(500, 'INTERNAL_ERROR'))
    await mountTask()
    expect(wrapper?.find('[data-test="reminder-current"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-edit"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-read-error"]').exists()).toBe(true)
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [], next_cursor: null, generation_enabled: true })
    await wrapper?.get('[data-test="reminder-read-retry"]').trigger('click')
    await flushPromises()
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('未开启')
  })
  it('assignee only; the creator sees and reads nothing', async () => {
    login('partner')
    wrapper = mount(ReminderControls, { props: { task: task() } })
    await flushPromises()
    expect(wrapper.find('[data-test="reminder-controls"]').exists()).toBe(false)
    expect(setTaskReminder).not.toHaveBeenCalled()
  })
  it('promises no phone delivery and states Beijing time', async () => {
    await mountTask()
    const text = wrapper?.get('[data-test="reminder-scope"]').text() ?? ''
    expect(text).toContain('应用内')
    expect(text).toContain('北京时间')
    expect(text).toContain('不保证手机会收到')
  })
  it('saves the root id with exactly the draft, then shows current state, not the mutation', async () => {
    vi.mocked(setTaskReminder).mockResolvedValue(saved({ current: current({ seq: 3, rule_id: 'r3', before_minutes: 90 }) }))
    await mountTask({ schedule_type: 'daily', period_date: '2026-11-02', deadline: null })
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(setTaskReminder).mock.calls[0]).toEqual(['t1', BEFORE_30, 'key-1'])
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('截止前 90 分钟')
    expect(wrapper?.find('[data-test="reminder-form"]').exists()).toBe(false)
  })
  it('a contradictory equal seq shows nothing as current; a newer rule from a replay is what is displayed', async () => {
    vi.mocked(setTaskReminder)
      .mockResolvedValueOnce(saved({ current: current({ before_minutes: 90 }) }))
      .mockResolvedValueOnce(saved({ current: current({ seq: 2, rule_id: 'r2', before_minutes: 90 }) }))
    await mountTask()
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('未开启')
    expect(wrapper?.get('[data-test="reminder-pending-terms"]').text()).toContain('截止前 30 分钟')
    await wrapper?.get('[data-test="reminder-retry"]').trigger('click')
    await flushPromises()
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('截止前 90 分钟')
    expect(wrapper?.get('[data-test="reminder-notice"]').text()).toContain('之后又被更改过')
  })
  it('refuses an out-of-bounds draft locally, keeping it', async () => {
    await mountTask({ schedule_type: 'daily', deadline: null })
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-before"]').setValue('1441')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(setTaskReminder).not.toHaveBeenCalled()
    expect(wrapper?.get('[data-test="reminder-draft-error"]').text()).toContain('1440')
  })
  it('a one-off with no deadline offers only a date and time', async () => {
    await mountTask({ deadline: null })
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    expect(wrapper?.find('[data-test="reminder-mode"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-date"]').exists()).toBe(true)
    expect(wrapper?.find('[data-test="reminder-before"]').exists()).toBe(false)
  })
  it('shows fired state honestly, and null next invents nothing', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current({ fired_this_period: true, next_fire_at: null })], next_cursor: null, generation_enabled: true })
    await mountTask()
    expect(wrapper?.get('[data-test="reminder-fired"]').text()).toContain('更改时间不会让它再提醒一次')
    expect(wrapper?.get('[data-test="reminder-next"]').text()).toBe('按当前设置，没有下一次提醒时间。')
  })
  it('disables with exactly {"enabled": false} on a completed task, and offers no enable', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current()], next_cursor: null, generation_enabled: true })
    vi.mocked(setTaskReminder).mockResolvedValue(disabledRule())
    await mountTask({ status: 'completed' })
    expect(wrapper?.find('[data-test="reminder-edit"]').exists()).toBe(false)
    await wrapper?.get('[data-test="reminder-disable"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(setTaskReminder).mock.calls[0]?.[1]).toEqual({ enabled: false })
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('未开启')
  })
  it('an inherited rule needing reset says so, keeps the choice shown, invents no time, and can be set again', async () => {
    const inherited = current({ origin: 'successor', needs_reset: true, next_fire_at: null, before_minutes: 45 })
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [inherited], next_cursor: null, generation_enabled: true })
    vi.mocked(setTaskReminder).mockResolvedValue({
      mutation: facts({ seq: 2, rule_id: 'r2', mode: 'absolute', before_minutes: null, at_local_date: '2026-12-01', at_time: '21:00' }),
      current: current({ seq: 2, rule_id: 'r2', mode: 'absolute', before_minutes: null, at_local_date: '2026-12-01', at_time: '21:00' }),
    })
    await mountTask({ deadline: null })
    expect(wrapper?.get('[data-test="reminder-needs-reset"]').text()).toContain('需要重新设置')
    expect(wrapper?.get('[data-test="reminder-inherited"]').text()).toContain('原样带过来')
    expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('截止前 45 分钟')
    expect(wrapper?.get('[data-test="reminder-next"]').text()).toBe('需要重新设置后才会提醒。')
    expect(setTaskReminder).not.toHaveBeenCalled()
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    // The inherited mode is not offered on a task with no deadline, so the draft starts from one that is.
    expect(wrapper?.find('[data-test="reminder-before"]').exists()).toBe(false)
    await wrapper?.get('[data-test="reminder-date"]').setValue('2026-12-01')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(setTaskReminder).mock.calls[0]?.[1]).toEqual({ enabled: true, mode: 'absolute', at_local_date: '2026-12-01', at_time: '21:00' })
    expect(wrapper?.find('[data-test="reminder-needs-reset"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-inherited"]').exists()).toBe(false)
  })
  it('a usable inherited rule shows its next time and no reset notice', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current({ origin: 'successor' })], next_cursor: null, generation_enabled: true })
    await mountTask()
    expect(wrapper?.find('[data-test="reminder-inherited"]').exists()).toBe(true)
    expect(wrapper?.find('[data-test="reminder-needs-reset"]').exists()).toBe(false)
    expect(wrapper?.get('[data-test="reminder-next"]').text()).toContain('北京时间')
  })
  it('a cancelled or superseded target takes no writes; the old root points to the successor', async () => {
    vi.mocked(listReminderRules).mockResolvedValue({ reminders: [current()], next_cursor: null, generation_enabled: true })
    await mountTask({ status: 'superseded', closure: { reason: 'superseded', closed_at: '', replaced_by: 't2' } })
    expect(wrapper?.find('[data-test="reminder-edit"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-disable"]').exists()).toBe(false)
    expect(wrapper?.get('[data-test="reminder-closed"]').text()).toContain('原样带到新任务')
  })
  it('an unknown result locks the form to the captured request until retried', async () => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network')).mockResolvedValueOnce(saved())
    await mountTask()
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-form"]').exists()).toBe(false)
    expect(wrapper?.get('[data-test="reminder-pending-terms"]').text()).toContain('截止前 30 分钟')
    await wrapper?.get('[data-test="reminder-retry"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(setTaskReminder).mock.calls[1]).toEqual(['t1', BEFORE_30, 'key-1'])
    expect(wrapper?.find('[data-test="reminder-retry"]').exists()).toBe(false)
  })
  it('changing target clears the draft and notice; a pending action comes back with its target', async () => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(api(409, 'REMINDER_TIME_PASSED'))
    await mountTask()
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-notice"]').exists()).toBe(true)
    await wrapper?.setProps({ task: task({ id: 't2' }) })
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-notice"]').exists()).toBe(false)
    expect(wrapper?.find('[data-test="reminder-form"]').exists()).toBe(false)
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    vi.mocked(setTaskReminder).mockRejectedValueOnce(new Error('network'))
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    await wrapper?.setProps({ task: task({ id: 't3' }) })
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-retry"]').exists()).toBe(false)
    await wrapper?.setProps({ task: task({ id: 't2' }) })
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-retry"]').exists()).toBe(true)
  })
  it('changing account clears the draft and all state, then reads for the new account', async () => {
    vi.mocked(listReminderRules).mockResolvedValueOnce({ reminders: [current({ target_kind: 'anniversary', target_id: 'a1', mode: 'anniversary_offsets', before_minutes: null, offsets: [1], at_time: '09:00', fired_this_period: null })], next_cursor: null, generation_enabled: true })
    wrapper = mount(ReminderControls, { props: { anniversaryId: 'a1' } })
    await flushPromises()
    await wrapper.get('[data-test="reminder-edit"]').trigger('click')
    expect(wrapper.find('[data-test="reminder-form"]').exists()).toBe(true)
    useSessionStore().resetBusinessState()
    await flushPromises()
    login('other')
    await flushPromises()
    expect(wrapper.find('[data-test="reminder-form"]').exists()).toBe(false)
    expect(wrapper.get('[data-test="reminder-current"]').text()).toContain('未开启')
    expect(listReminderRules).toHaveBeenCalledTimes(2)
  })
  it('returning to a target as a new instance, as the pages do by key, does not bring back its notice', async () => {
    vi.mocked(setTaskReminder).mockRejectedValueOnce(api(409, 'REMINDER_TIME_PASSED'))
    await mountTask()
    await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper?.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(wrapper?.find('[data-test="reminder-notice"]').exists()).toBe(true)
    wrapper?.unmount()
    await mountTask()
    expect(wrapper?.find('[data-test="reminder-notice"]').exists()).toBe(false)
  })
  it('anniversary: each member saves their own, offsets sorted, default 09:00, at most three', async () => {
    const row: Partial<ReminderRuleFacts> = { target_kind: 'anniversary', target_id: 'a1', mode: 'anniversary_offsets', before_minutes: null, offsets: [0, 7], at_time: '09:00' }
    vi.mocked(setAnniversaryReminder).mockResolvedValue({ mutation: facts(row), current: current({ ...row, fired_this_period: null }) })
    wrapper = mount(ReminderControls, { props: { anniversaryId: 'a1' } })
    await flushPromises()
    await wrapper.get('[data-test="reminder-edit"]').trigger('click')
    await wrapper.get('[data-test="reminder-offset-7"]').trigger('change')
    await wrapper.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(setAnniversaryReminder).mock.calls[0]).toEqual(['a1', { enabled: true, days_before: [0, 7], at_time: '09:00' }, 'key-1'])
    expect(wrapper.get('[data-test="reminder-no-repeat"]').text()).toContain('不会因为更改时间而重复')
    expect(wrapper.find('[data-test="reminder-fired"]').exists()).toBe(false)
    await wrapper.get('[data-test="reminder-edit"]').trigger('click')
    for (const day of [1, 3]) await wrapper.get(`[data-test="reminder-offset-${day}"]`).trigger('change')
    await wrapper.get('[data-test="reminder-save"]').trigger('submit')
    await flushPromises()
    expect(wrapper.get('[data-test="reminder-draft-error"]').text()).toContain('1 到 3')
    expect(setAnniversaryReminder).toHaveBeenCalledTimes(1)
  })
})

describe('generation status', () => {
  const page = (generation: unknown, overrides: Record<string, unknown> = {}) =>
    ({ reminders: [current()], next_cursor: null, generation_enabled: generation, ...overrides }) as never
  const MALFORMED: [string, unknown][] = [['null', null], ['the text true', 'true'], ['one', 1], ['an object', {}], ['missing', undefined]]

  describe('store', () => {
    it('starts unknown', () => {
      expect(useRemindersStore().generationStatus).toBe('unknown')
    })
    it.each([[true, 'enabled'], [false, 'disabled']] as const)('%s is %s', async (value, expected) => {
      vi.mocked(listReminderRules).mockResolvedValue(page(value))
      const store = useRemindersStore()
      await store.refresh()
      expect(store.generationStatus).toBe(expected)
      expect(store.status).toBe('ready')
    })
    it.each(MALFORMED)('%s is unknown, and the rules still load', async (_, value) => {
      vi.mocked(listReminderRules).mockResolvedValue(page(value))
      const store = useRemindersStore()
      await store.refresh()
      expect(store.generationStatus).toBe('unknown')
      expect(store.status).toBe('ready')
      expect(store.rules['task:t1']).toBeTruthy()
    })
    it('a missing field after a known one does not keep the old answer', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true)).mockResolvedValueOnce({ reminders: [], next_cursor: null })
      const store = useRemindersStore()
      await store.refresh()
      expect(store.generationStatus).toBe('enabled')
      await store.refresh()
      expect(store.generationStatus).toBe('unknown')
    })
    it('a failed read is unknown, keeping the rules it already had', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true)).mockRejectedValueOnce(api(500, 'INTERNAL_ERROR'))
      const store = useRemindersStore()
      await store.refresh()
      await store.refresh()
      expect(store.generationStatus).toBe('unknown')
      expect(store.status).toBe('error')
      expect(store.rules['task:t1']).toBeTruthy()
    })
    it('reads every page as before; all pages must agree', async () => {
      vi.mocked(listReminderRules)
        .mockResolvedValueOnce(page(false, { reminders: [current({ target_id: 't1' })], next_cursor: 'c1' }))
        .mockResolvedValueOnce(page(false, { reminders: [current({ target_id: 't2' })] }))
      const store = useRemindersStore()
      await store.refresh()
      expect(vi.mocked(listReminderRules).mock.calls.map(args => args[0])).toEqual([null, 'c1'])
      expect(Object.keys(store.rules).sort()).toEqual(['task:t1', 'task:t2'])
      expect(store.generationStatus).toBe('disabled')
    })
    it.each([[true, false], [true, undefined], [undefined, true]])('pages saying %s then %s are unknown', async (first, second) => {
      vi.mocked(listReminderRules)
        .mockResolvedValueOnce(page(first, { reminders: [current({ target_id: 't1' })], next_cursor: 'c1' }))
        .mockResolvedValueOnce(page(second, { reminders: [current({ target_id: 't2' })] }))
      const store = useRemindersStore()
      await store.refresh()
      expect(store.generationStatus).toBe('unknown')
      expect(store.status).toBe('ready')
    })
    it('a failure on a later page is unknown, whatever the first page said', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true, { next_cursor: 'c1' })).mockRejectedValueOnce(api(500, 'INTERNAL_ERROR'))
      const store = useRemindersStore()
      await store.refresh()
      expect(store.generationStatus).toBe('unknown')
    })
    it('a delayed response across an account switch never reaches the new account', async () => {
      const old = deferred<never>()
      const store = useRemindersStore()
      vi.mocked(listReminderRules).mockReturnValueOnce(old.promise)
      const reading = store.refresh()
      useSessionStore().resetBusinessState()
      login('other')
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(false, { reminders: [] }))
      await store.refresh()
      expect(store.generationStatus).toBe('disabled')
      old.resolve(page(true))
      await reading
      expect(store.generationStatus).toBe('disabled')
      expect(store.rules).toEqual({})
    })
    it('an account change forgets the old account\'s status at once', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true))
      const store = useRemindersStore()
      await store.refresh()
      useSessionStore().resetBusinessState()
      expect(store.generationStatus).toBe('unknown')
    })
    it('a superseded read does not decide the status', async () => {
      const old = deferred<never>()
      vi.mocked(listReminderRules).mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(false))
      const store = useRemindersStore()
      const first = store.refresh()
      await store.refresh()
      old.resolve(page(true))
      await first
      expect(store.generationStatus).toBe('disabled')
    })
    it.each([
      [true, '提醒设置已保存。'],
      [false, '提醒设置已保存，但提醒生成目前未开启，暂时不会按它提醒。'],
      [undefined, '提醒设置已保存，但暂时无法确认提醒生成是否开启，不代表一定会提醒。'],
    ])('saving under %s says only what is known', async (value, text) => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(value, { reminders: [] }))
      vi.mocked(setTaskReminder).mockResolvedValueOnce({ mutation: facts(), current: current() })
      const store = useRemindersStore()
      await store.refresh()
      const result = await store.save('task', 't1', { enabled: true, mode: 'before_deadline', before_minutes: 30 })
      expect(result.outcome).toBe('done')
      expect(store.notices['task:t1']?.text).toBe(text)
    })
    it('an unknown save keeps its key and body through a status change', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true, { reminders: [] })).mockResolvedValueOnce(page(false, { reminders: [] }))
      vi.mocked(setTaskReminder).mockRejectedValueOnce(api(500, 'RESULT_UNKNOWN')).mockResolvedValueOnce({ mutation: facts(), current: current() })
      const store = useRemindersStore()
      await store.refresh()
      const body = { enabled: true, mode: 'before_deadline', before_minutes: 30 } as const
      expect((await store.save('task', 't1', body)).outcome).toBe('unknown')
      await store.refresh()
      expect(store.generationStatus).toBe('disabled')
      expect(setTaskReminder).toHaveBeenCalledTimes(1)
      expect((await store.retry('task:t1')).outcome).toBe('done')
      const calls = vi.mocked(setTaskReminder).mock.calls
      expect(calls[1]).toEqual(calls[0])
      expect(store.notices['task:t1']?.text).toContain('未开启')
    })
  })

  describe('controls', () => {
    const anniversaryRule = () => current({ target_kind: 'anniversary', target_id: 'a1', mode: 'anniversary_offsets', before_minutes: null, offsets: [1], at_time: '09:00', fired_this_period: null })
    function mountFor(kind: 'task' | 'anniversary') {
      wrapper = kind === 'task'
        ? mount(ReminderControls, { props: { task: task() } })
        : mount(ReminderControls, { props: { anniversaryId: 'a1' } })
      return flushPromises()
    }
    const rows = (kind: 'task' | 'anniversary') => [kind === 'task' ? current() : anniversaryRule()]
    const has = (name: string) => wrapper?.find(`[data-test="${name}"]`).exists()

    describe.each(['task', 'anniversary'] as const)('%s', (kind) => {
      it('enabled: shows the computed next time and no status line', async () => {
        vi.mocked(listReminderRules).mockResolvedValue(page(true, { reminders: rows(kind) }))
        await mountFor(kind)
        expect(wrapper?.get('[data-test="reminder-next"]').text()).toContain('北京时间')
        expect(has('reminder-generation-disabled')).toBe(false)
        expect(has('reminder-generation-unknown')).toBe(false)
      })
      it('disabled: says so, hides the next time, and the rule stays visible and editable', async () => {
        vi.mocked(listReminderRules).mockResolvedValue(page(false, { reminders: rows(kind) }))
        await mountFor(kind)
        const line = wrapper?.get('[data-test="reminder-generation-disabled"]').text()
        expect(line).toContain('未开启')
        expect(line).not.toContain('推送')
        expect(has('reminder-next')).toBe(false)
        expect(has('reminder-no-repeat')).toBe(false)
        expect(wrapper?.get('[data-test="reminder-current"]').text()).not.toContain('未开启')
        expect(has('reminder-edit')).toBe(true)
        expect(has('reminder-disable')).toBe(true)
      })
      it.each(MALFORMED)('%s: says unknown and suppresses the next time', async (_, value) => {
        vi.mocked(listReminderRules).mockResolvedValue(page(value, { reminders: rows(kind) }))
        await mountFor(kind)
        expect(wrapper?.get('[data-test="reminder-generation-unknown"]').text()).not.toContain('推送')
        expect(has('reminder-next')).toBe(false)
        expect(wrapper?.text()).not.toContain('下次时间：')
        expect(has('reminder-edit')).toBe(true)
      })
    })
    it('with no rule yet, disabled still says so and setting one remains possible', async () => {
      vi.mocked(listReminderRules).mockResolvedValue(page(false, { reminders: [] }))
      await mountFor('task')
      expect(has('reminder-generation-disabled')).toBe(true)
      expect(wrapper?.get('[data-test="reminder-current"]').text()).toContain('未开启')
      await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
      expect(has('reminder-form')).toBe(true)
    })
    it('a needs-reset rule under disabled generation does not promise a reminder after resetting', async () => {
      vi.mocked(listReminderRules).mockResolvedValue(page(false, { reminders: [current({ origin: 'successor', needs_reset: true, next_fire_at: null })] }))
      await mountFor('task')
      expect(has('reminder-needs-reset')).toBe(true)
      expect(has('reminder-next')).toBe(false)
    })
    it('saving while disabled posts the rule and never implies it will fire', async () => {
      vi.mocked(listReminderRules).mockResolvedValue(page(false, { reminders: [] }))
      vi.mocked(setTaskReminder).mockResolvedValueOnce({ mutation: facts(), current: current() })
      await mountFor('task')
      await wrapper?.get('[data-test="reminder-edit"]').trigger('click')
      await wrapper?.get('[data-test="reminder-form"]').trigger('submit')
      await flushPromises()
      expect(setTaskReminder).toHaveBeenCalledTimes(1)
      expect(wrapper?.get('[data-test="reminder-notice"]').text()).toContain('暂时不会')
      expect(has('reminder-next')).toBe(false)
      expect(has('reminder-generation-disabled')).toBe(true)
    })
    it('a read error with rules on screen is unknown and shows no next time', async () => {
      vi.mocked(listReminderRules).mockResolvedValueOnce(page(true))
      await mountFor('task')
      expect(has('reminder-next')).toBe(true)
      vi.mocked(listReminderRules).mockRejectedValueOnce(api(500, 'INTERNAL_ERROR'))
      await useRemindersStore().refresh()
      await flushPromises()
      expect(has('reminder-next')).toBe(false)
      expect(has('reminder-generation-unknown')).toBe(true)
    })
    it('a delayed enabled answer for the old account never shows a next time to the new one', async () => {
      const old = deferred<never>()
      vi.mocked(listReminderRules).mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(false, { reminders: [] }))
      await mountFor('anniversary')
      useSessionStore().resetBusinessState()
      await flushPromises()
      login('other')
      await flushPromises()
      expect(has('reminder-generation-disabled')).toBe(true)
      old.resolve(page(true, { reminders: rows('anniversary') }))
      await flushPromises()
      expect(has('reminder-generation-disabled')).toBe(true)
      expect(has('reminder-next')).toBe(false)
      expect(useRemindersStore().rules).toEqual({})
    })
  })
})

describe('task page integration', () => {
  it('task cards load no reminders; opening one detail reads once and shows the control to its assignee', async () => {
    const rows = [task({ id: 't1' }), task({ id: 't2' }), task({ id: 't3' })]
    vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 0, sunshine: 0 } })
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? rows : [], next_cursor: null }))
    vi.mocked(getTask).mockResolvedValue({ task: rows[0] as Task })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(listReminderRules).not.toHaveBeenCalled()
    await useTasksStore().openDetail('t1')
    await flushPromises()
    expect(listReminderRules).toHaveBeenCalledTimes(1)
    expect(wrapper.find('[data-test="reminder-controls"]').exists()).toBe(true)
  })
  it.each(['self', 'partner'])('the anniversary detail offers its own control to member %s, whoever created it', async (member) => {
    login(member)
    vi.mocked(getAnniversary).mockResolvedValue({ as_of_date: '2026-11-02', anniversary: {
      id: 'a1', creator_id: 'self', version: 1, title: '在一起', date: '2020-05-20', kind: 'anniversary', note: '',
      created_at: '', updated_at: '', pinned_by_me: false, pending_proposal: null, last_decision: null,
      computed: { days_since: 1, next: { date: '2027-05-20', years: 7, days_until: 200 }, days_until: null },
    } } as never)
    wrapper = mount(AnniversaryDetailView, { props: { id: 'a1' }, global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="reminder-controls"]').exists()).toBe(true)
    expect(listReminderRules).toHaveBeenCalledTimes(1)
    expect(setAnniversaryReminder).not.toHaveBeenCalled()
  })
})

describe('notifications', () => {
  const row = (overrides: Partial<Notification> = {}): Notification => ({
    id: 'n1', event_type: 'reminder.task', actor_id: null, target_kind: 'task', target_id: 't1', created_at: '2026-11-02T00:00:00Z', read_at: null, ...overrides,
  })
  const UUID = '11111111-1111-4111-8111-111111111111'

  it('null actor only for the reminder family, and a reminder never has one', () => {
    expect(hasValidActor({ event_type: 'reminder.task', actor_id: null })).toBe(true)
    expect(hasValidActor({ event_type: 'reminder.anniversary', actor_id: null })).toBe(true)
    expect(hasValidActor({ event_type: 'reminder.task', actor_id: 'x' })).toBe(false)
    for (const type of ['task.published', 'message.created', 'anniversary.edited', 'redemption.created', 'unknown.event']) {
      expect(hasValidActor({ event_type: type, actor_id: null })).toBe(false)
      expect(hasValidActor({ event_type: type, actor_id: 'x' })).toBe(true)
      expect(hasValidActor({ event_type: type, actor_id: '' })).toBe(false)
    }
  })
  it('the approved fixed labels, and existing targets', () => {
    expect(eventLabel('reminder.task')).toBe('有一个任务快到截止时间了')
    expect(eventLabel('reminder.anniversary')).toBe('有一个重要的日子快到了')
    expect(notificationLabel(row())).toBe('有一个任务快到截止时间了')
    expect(notificationTarget(row())).toBe('/tasks')
    expect(notificationTarget(row({ event_type: 'reminder.anniversary', target_kind: 'anniversary', target_id: 'a1' }))).toBe('/us/anniversaries/a1')
  })
  it('the inbox renders a null actor, and opening nothing marks nothing read', async () => {
    vi.mocked(listNotifications).mockResolvedValue({ notifications: [row(), row({ id: 'n2', event_type: 'reminder.anniversary', target_kind: 'anniversary', target_id: 'a1' })], next_cursor: null })
    wrapper = mount(NotificationsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const text = wrapper.text()
    expect(text).toContain('有一个任务快到截止时间了')
    expect(text).toContain('有一个重要的日子快到了')
    expect(text).toContain('未读')
    expect(useNotificationsStore().status).toBe('ready')
    expect(readNotification).not.toHaveBeenCalled()
  })
  it('a page with a null actor outside the family is an error; a later page too, keeping what was read', async () => {
    const store = useNotificationsStore()
    vi.mocked(listNotifications).mockResolvedValueOnce({ notifications: [row({ event_type: 'task.published' })], next_cursor: null })
    await store.load()
    expect(store.status).toBe('error')
    vi.mocked(listNotifications).mockResolvedValueOnce({ notifications: [row({ actor_id: 'other', event_type: 'task.published' })], next_cursor: 'c' })
    await store.load()
    expect(store.status).toBe('ready')
    vi.mocked(listNotifications).mockResolvedValueOnce({ notifications: [row({ id: 'n9', event_type: 'message.created' })], next_cursor: null })
    await store.loadMore()
    expect(store.moreError).not.toBe('')
    expect(store.items.map(n => n.id)).toEqual(['n1'])
  })
  it('the service worker shows the labels and allowed targets; no read or new Push behaviour', () => {
    const marker = 'A'.repeat(22)
    const stored = { marker } as never
    const text = (event_type: string, target_kind: string) => JSON.stringify({ v: 1, nid: UUID, event_type, target_kind, target_id: UUID, marker })
    const task = parsePushPayload(text('reminder.task', 'task'))
    const item = parsePushPayload(text('reminder.anniversary', 'anniversary'))
    expect(task).not.toBeNull()
    expect(item).not.toBeNull()
    const taskPlan = notificationFor(task, stored)
    const itemPlan = notificationFor(item, stored)
    expect(taskPlan.options.body).toBe('有一个任务快到截止时间了')
    expect(itemPlan.options.body).toBe('有一个重要的日子快到了')
    expect(destinationFor(taskPlan.options.data, stored)).toBe('/tasks')
    expect(destinationFor(itemPlan.options.data, stored)).toBe(targetPath('anniversary', UUID))
    // An unknown family still never reaches a label.
    expect(parsePushPayload(text('reminder.other', 'task'))).toBeNull()
  })
})
