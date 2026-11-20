/**
 * M10 home: today's-task rules (Beijing dates, rollover), role-appropriate pending work, bounded
 * cursor scans that never present partial results as totals or as zero, independent card states,
 * session isolation of late results, and home refreshes that leave task/reward actions and keys alone.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/avatar', async original => ({ ...(await original<typeof import('@/api/avatar')>()), getAvatarAppearance: vi.fn() }))
vi.mock('@/api/tasks', () => ({
  listTasks: vi.fn(), completeTask: vi.fn(), createTask: vi.fn(), decideTaskChange: vi.fn(), getTask: vi.fn(),
  progressTask: vi.fn(), proposeTaskChange: vi.fn(), newIdempotencyKey: vi.fn(),
}))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn() }))
vi.mock('@/api/anniversaries', () => ({
  createAnniversary: vi.fn(), listAnniversaries: vi.fn(), getAnniversary: vi.fn(), editAnniversary: vi.fn(),
  proposeAnniversaryEdit: vi.fn(), decideAnniversaryProposal: vi.fn(), getHomeAnniversary: vi.fn(),
  setHomeAnniversaryPin: vi.fn(),
}))
vi.mock('@/api/rewards', () => ({
  listRedemptions: vi.fn(), listRewardProducts: vi.fn(), createRewardProduct: vi.fn(), editRewardProduct: vi.fn(),
  archiveRewardProduct: vi.fn(), redeemRewardProduct: vi.fn(), fulfillRedemption: vi.fn(), transitionRedemption: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))

import { getHomeAnniversary, listAnniversaries } from '@/api/anniversaries'
import { ApiError, setCsrfToken } from '@/api/client'
import * as authApi from '@/api/auth'
import { listMessages } from '@/api/messages'
import { listRedemptions, redeemRewardProduct } from '@/api/rewards'
import { completeTask, listTasks, newIdempotencyKey } from '@/api/tasks'
import type { Redemption, RedemptionListResponse, Task, TaskListResponse } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { getAvatarAppearance } from '@/api/avatar'
import { appearance as avatarAppearance } from './avatar-fixtures'
import { HOME_MAX_PAGES, beijingDate, isTodayTask, pendingSummary, useHomeStore } from '@/stores/home'
import { useRewardsStore } from '@/stores/rewards'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'
import HomeView from '@/views/HomeView.vue'

// 2026-10-26 12:00 Beijing = 04:00 UTC.
const NOON = Date.UTC(2026, 9, 26, 4, 0, 0)
const BEFORE_MIDNIGHT = Date.UTC(2026, 9, 26, 15, 59, 0)  // 23:59 Beijing
const AFTER_MIDNIGHT = Date.UTC(2026, 9, 26, 16, 0, 1)   // 2026-10-27 00:00:01 Beijing

function task(id: string, overrides: Partial<Task> = {}): Task {
  return {
    id, title: `任务 ${id}`, description: '', creator_id: 'partner', assignee_id: 'self', reward_flowers: 1,
    reward_sunshine: 0, deadline: null, status: 'open', completed_at: null, created_at: '2026-10-20T00:00:00Z',
    schedule_type: 'one_off', ...overrides,
  }
}

function deadline(date: string, at: string) {
  return { local_date: date, local_time: null, at }
}

function redemption(id: string, overrides: Partial<Redemption> = {}): Redemption {
  return {
    id, product_id: 'p', product_version: 1, creator_id: 'partner', redeemer_id: 'self', title: `兑换 ${id}`,
    description: '', price_flowers: 1, redemption_mode: 'direct', operation_id: 'op', status: 'fulfilled',
    created_at: '2026-10-26T00:00:00Z', fulfilled_at: null, fulfilled_by: null, ...overrides,
  }
}

function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: `昵称${id}`, created_at: '2026-10-26T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-26T00:00:00Z', members: [] },
  })
  setCsrfToken(`csrf-${id}`)
}

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

/** Serves view=open and view=history separately (cursor-aware). */
function serveTasks(open: (cursor?: string | null) => TaskListResponse,
  history: (cursor?: string | null) => TaskListResponse = () => ({ tasks: [], next_cursor: null })): void {
  vi.mocked(listTasks).mockImplementation(async (view, cursor) => (view === 'open' ? open(cursor) : history(cursor)))
}

function callsFor(view: 'open' | 'history') {
  return vi.mocked(listTasks).mock.calls.filter((call) => call[0] === view)
}

function change(id: string, proposer: string) {
  return { id, kind: 'cancel' as const, proposer_id: proposer, created_at: '', terms: null, blocked_reason: 'expired' as const }
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 5, sunshine: 6 } })
  vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: avatarAppearance() })
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
  vi.mocked(listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-29', anniversaries: [], next_cursor: null })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.useRealTimers()
})

function mountHome() {
  wrapper = mount(HomeView, { global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } })
  return wrapper
}

describe('home character integration', () => {
  it('loads the caller appearance once, keeps the own balances and refreshes explicitly', async () => {
    const w = mountHome(); await flushPromises()
    expect(getAvatarAppearance).toHaveBeenCalledTimes(1)
    expect(w.get('[data-test="avatar-home"]').text()).toContain('当前形象')
    expect(w.get('[data-test="home-wallet"]').text()).toContain('阳光值')
    await w.get('[data-test="home-refresh"]').trigger('click'); await flushPromises()
    expect(getAvatarAppearance).toHaveBeenCalledTimes(2)
  })
})

describe("today's tasks", () => {
  it('uses the Beijing date', () => {
    expect(beijingDate(BEFORE_MIDNIGHT)).toBe('2026-10-26')
    expect(beijingDate(AFTER_MIDNIGHT)).toBe('2026-10-27')
  })

  it('includes my open, applicable tasks and excludes everything else', () => {
    const include = [
      task('undated'),
      task('due-later', { deadline: deadline('2026-10-28', '2026-10-28T16:00:00Z') }),
      task('daily', { schedule_type: 'daily', period_date: '2026-10-26', deadline: deadline('2026-10-26', '2026-10-26T16:00:00Z') }),
      task('weekly', { schedule_type: 'weekly', period_date: '2026-10-26', deadline: deadline('2026-11-01', '2026-11-01T16:00:00Z') }),
      task('custom', { schedule_type: 'custom', start_date: '2026-10-25', deadline: deadline('2026-10-27', '2026-10-27T16:00:00Z') }),
    ]
    const exclude = [
      task('partner-assigned', { assignee_id: 'partner', creator_id: 'self' }),
      task('upcoming', { schedule_type: 'custom', status: 'upcoming', start_date: '2026-10-27', deadline: deadline('2026-10-28', '2026-10-28T16:00:00Z') }),
      task('expired', { status: 'expired', deadline: deadline('2026-10-25', '2026-10-25T16:00:00Z') }),
      task('completed', { status: 'completed' }),
      task('cancelled', { status: 'cancelled' }),
      task('superseded', { status: 'superseded' }),
      task('past-due-open', { deadline: deadline('2026-10-26', '2026-10-26T03:00:00Z') }),
      task('stale-daily', { schedule_type: 'daily', period_date: '2026-10-25', deadline: deadline('2026-10-25', '2026-10-25T16:00:00Z') }),
      task('future-custom', { schedule_type: 'custom', start_date: '2026-10-27', deadline: deadline('2026-10-28', '2026-10-28T16:00:00Z') }),
      task('old-week', { schedule_type: 'weekly', period_date: '2026-10-19', deadline: deadline('2026-10-25', '2026-10-25T16:00:00Z') }),
    ]
    for (const t of include) expect([t.id, isTodayTask(t, 'self', NOON)]).toEqual([t.id, true])
    for (const t of exclude) expect([t.id, isTodayTask(t, 'self', NOON)]).toEqual([t.id, false])
  })

  it('re-reads at the Beijing midnight and shows the new period only', async () => {
    vi.useFakeTimers({ toFake: ['Date', 'setTimeout', 'clearTimeout'] })
    vi.setSystemTime(BEFORE_MIDNIGHT)
    const day1 = task('d', { schedule_type: 'daily', period_date: '2026-10-26', title: '第一天' })
    const day2 = task('d', { schedule_type: 'daily', period_date: '2026-10-27', title: '第二天' })
    let served = day1
    serveTasks(() => ({ tasks: [served], next_cursor: null }))
    const w = mountHome()
    await flushPromises()
    expect(w.find('[data-test="home-today"]').text()).toContain('第一天')
    served = day2
    await vi.advanceTimersByTimeAsync(2 * 60 * 1000)
    await flushPromises()
    expect(callsFor('open')).toHaveLength(2)
    const text = w.find('[data-test="home-today"]').text()
    expect(text).toContain('第二天')
    expect(text).not.toContain('第一天')
  })

  it('re-reads when the app returns to the foreground', async () => {
    mountHome()
    await flushPromises()
    expect(callsFor('open')).toHaveLength(1)
    expect(callsFor('history')).toHaveLength(1)
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expect(callsFor('open')).toHaveLength(2)
    expect(callsFor('history')).toHaveLength(2)
    expect(listRedemptions).toHaveBeenCalledTimes(2)
  })
})

describe('pending work', () => {
  it('separates needs-me from waiting-for-partner by role', () => {
    const tasks = [
      task('a', { pending_change: { id: 'c1', kind: 'cancel', proposer_id: 'partner', created_at: '', terms: null, blocked_reason: null } }),
      task('b', { creator_id: 'self', assignee_id: 'partner',
        pending_change: { id: 'c2', kind: 'edit', proposer_id: 'self', created_at: '', terms: null, blocked_reason: null } }),
      task('c'),
    ]
    const redemptions = [
      redemption('r1', { status: 'pending_confirmation', creator_id: 'self', redeemer_id: 'partner', operation_id: null }),
      redemption('r2', { status: 'pending_confirmation' }),
      redemption('r3', { status: 'pending_fulfillment', creator_id: 'self', redeemer_id: 'partner' }),
      redemption('r4', { status: 'pending_fulfillment', pending_cancellation: { id: 'x', proposed_by: 'partner' } }),
      redemption('r5', { status: 'pending_fulfillment', creator_id: 'self', redeemer_id: 'partner',
        pending_cancellation: { id: 'y', proposed_by: 'self' } }),
      redemption('r6', { status: 'fulfilled' }),
      redemption('r7', { status: 'cancelled', cancel_reason: 'rejected' }),
    ]
    expect(pendingSummary(tasks, redemptions, 'self')).toEqual({
      needsMe: { taskDecisions: 1, redemptionConfirmations: 1, cancellationDecisions: 1, fulfillments: 2 },
      waiting: { taskDecisions: 1, redemptionConfirmations: 1, cancellationDecisions: 1, fulfillments: 1 },
    })
  })

  it('follows cursors to the end and reports exact counts', async () => {
    vi.mocked(listRedemptions)
      .mockResolvedValueOnce({ redemptions: [redemption('r1', { status: 'pending_confirmation', creator_id: 'self', redeemer_id: 'partner' })], next_cursor: 'c1' })
      .mockResolvedValueOnce({ redemptions: [redemption('r2', { status: 'pending_confirmation', creator_id: 'self', redeemer_id: 'partner' })], next_cursor: null })
    const w = mountHome()
    await flushPromises()
    expect(vi.mocked(listRedemptions).mock.calls.map((call) => call[0])).toEqual([null, 'c1'])
    expect(w.find('[data-test="needs-me-redemptionConfirmations"]').text()).toContain('2')
    expect(w.find('[data-test="needs-me-redemptionConfirmations"]').text()).not.toContain('至少')
    // Redemptions are an exhaustive source once every page is read: an exact count, no scope qualifier.
    expect(w.find('[data-test="needs-me-redemptionConfirmations"]').text()).not.toContain('已找到')
    expect(w.find('[data-test="pending-incomplete"]').exists()).toBe(false)
  })

  it('stops after the page bound and labels results as incomplete, never as zero', async () => {
    let n = 0
    serveTasks(() => ({ tasks: [task(`t${++n}`)], next_cursor: `c${n}` }))
    let m = 0
    vi.mocked(listRedemptions).mockImplementation(async (): Promise<RedemptionListResponse> => ({
      redemptions: [redemption(`r${++m}`, { status: m === 1 ? 'pending_fulfillment' : 'fulfilled', creator_id: 'self', redeemer_id: 'partner' })],
      next_cursor: `d${m}`,
    }))
    const w = mountHome()
    await flushPromises()
    expect(callsFor('open')).toHaveLength(HOME_MAX_PAGES)
    expect(callsFor('history')).toHaveLength(1)
    expect(listRedemptions).toHaveBeenCalledTimes(HOME_MAX_PAGES)
    const store = useHomeStore()
    expect(store.tasks.complete).toBe(false)
    expect(w.find('[data-test="needs-me-fulfillments"]').text()).toContain('至少 1')
    expect(w.find('[data-test="pending-incomplete"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
    expect(w.find('[data-test="today-incomplete"]').exists()).toBe(true)
    expect(w.find('[data-test="today-none"]').exists()).toBe(false)
  })

  it('shows "none" only after complete scans; a failed source shows an error, not zero', async () => {
    let w = mountHome()
    await flushPromises()
    // Cursor exhaustion is not global coverage (history is the last 30 days): only a scoped "none".
    const none = w.find('[data-test="pending-none"]')
    expect(none.text()).toContain('在可查看的范围内没有找到')
    expect(none.text()).toContain('最近 30 天')
    expect(none.text()).not.toBe('没有待处理事项。')
    const scope = w.find('[data-test="pending-task-scope"]').text()
    expect(scope).toContain('最近 30 天')
    // Older records are not reachable from any list (the Tasks page included): no redirect promise.
    expect(scope).toContain('无法通过现有列表查到')
    expect(scope).not.toContain('请到任务页查看')
    w.unmount()
    vi.mocked(listRedemptions).mockRejectedValue(new ApiError('NETWORK_ERROR', 'offline', 0))
    w = mountHome()
    await flushPromises()
    expect(w.find('[data-test="pending-redemptions-error"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
    expect(w.find('[data-test="home-wallet"]').findAll('dd').map((d) => d.text())).toEqual(['5', '6'])
    expect(w.find('[data-test="today-none"]').exists()).toBe(true)
  })

  it('a failing page discards partial results instead of showing them as totals', async () => {
    vi.mocked(listTasks).mockImplementation(async (view, cursor) => {
      if (view === 'history') return { tasks: [], next_cursor: null }
      if (!cursor) return { tasks: [task('t1')], next_cursor: 'c1' }
      throw new ApiError('INTERNAL_ERROR', 'down', 500)
    })
    const w = mountHome()
    await flushPromises()
    expect(useHomeStore().tasks.items).toEqual([])
    expect(w.find('[data-test="home-today"] [role="alert"]').exists()).toBe(true)
    expect(w.find('[data-test="today-t1"]').exists()).toBe(false)
  })
})

describe('task-change pending work across open and history', () => {
  it('counts an undecided change on a historical task with an empty open list; today ignores history', async () => {
    serveTasks(() => ({ tasks: [], next_cursor: null }), () => ({
      tasks: [
        task('expired', { status: 'expired', pending_change: change('p1', 'partner'),
          deadline: deadline('2026-10-20', '2026-10-20T16:00:00Z') }),
        task('h-open'),  // a history row that would pass the today rule must still not appear in today's tasks
      ],
      next_cursor: null,
    }))
    const w = mountHome()
    await flushPromises()
    expect(w.find('[data-test="needs-me-taskDecisions"]').text()).toContain('已找到 1')
    expect(w.find('[data-test="needs-me-taskDecisions"]').text()).not.toContain('至少')
    expect(w.find('[data-test="pending-task-scope"]').text()).toContain('最近 30 天')
    expect(w.find('[data-test="today-h-open"]').exists()).toBe(false)
    expect(w.find('[data-test="today-expired"]').exists()).toBe(false)
    expect(w.find('[data-test="today-none"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
  })

  it('counts one proposal once across open and several returned periods', async () => {
    const shared = change('p-shared', 'self')
    serveTasks(
      () => ({ tasks: [task('root', { schedule_type: 'daily', period_date: '2026-10-26', pending_change: shared })], next_cursor: null }),
      (cursor) => cursor
        ? { tasks: [task('root', { schedule_type: 'daily', status: 'expired', period_date: '2026-10-24', pending_change: shared })], next_cursor: null }
        : { tasks: [task('root', { schedule_type: 'daily', status: 'completed', period_date: '2026-10-25', pending_change: shared })], next_cursor: 'h1' },
    )
    const w = mountHome()
    await flushPromises()
    expect(useHomeStore().history.items).toHaveLength(2)  // two periods of one root are both kept as rows
    expect(w.find('[data-test="waiting-taskDecisions"]').text()).toContain('已找到 1')
    expect(w.find('[data-test="waiting-taskDecisions"]').text()).not.toContain('至少')
    expect(w.find('[data-test="needs-me-taskDecisions"]').exists()).toBe(false)
  })

  it('an incomplete history scan prevents exact counts and "none"', async () => {
    let n = 0
    serveTasks(() => ({ tasks: [], next_cursor: null }), () => {
      n += 1
      return { tasks: [task(`h${n}`, { status: 'expired', ...(n === 1 ? { pending_change: change('p1', 'partner') } : {}) })],
        next_cursor: `h${n}` }
    })
    const w = mountHome()
    await flushPromises()
    expect(callsFor('history')).toHaveLength(HOME_MAX_PAGES)
    expect(w.find('[data-test="needs-me-taskDecisions"]').text()).toContain('至少 1')
    expect(w.find('[data-test="pending-incomplete"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
    expect(w.find('[data-test="today-none"]').exists()).toBe(true)  // open was read completely
  })

  it('exhausted task cursors still give a scoped count, never an exact global total', async () => {
    serveTasks(() => ({ tasks: [task('t', { pending_change: change('p1', 'partner') })], next_cursor: null }))
    const w = mountHome()
    await flushPromises()
    expect(useHomeStore().tasks.complete && useHomeStore().history.complete).toBe(true)
    const row = w.find('[data-test="needs-me-taskDecisions"]')
    expect(row.find('.badge').text()).toBe('已找到 1')
    expect(w.find('[data-test="pending-task-scope"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
    expect(w.find('[data-test="pending-incomplete"]').exists()).toBe(false)
  })

  it('a failed history scan shows an error, never "none" or task-change counts', async () => {
    vi.mocked(listTasks).mockImplementation(async (view) => {
      if (view === 'history') throw new ApiError('NETWORK_ERROR', 'offline', 0)
      return { tasks: [task('mine', { pending_change: change('p1', 'partner') })], next_cursor: null }
    })
    const w = mountHome()
    await flushPromises()
    expect(w.find('[data-test="pending-history-error"]').exists()).toBe(true)
    expect(w.find('[data-test="needs-me-taskDecisions"]').exists()).toBe(false)
    expect(w.find('[data-test="pending-none"]').exists()).toBe(false)
    expect(w.find('[data-test="today-mine"]').exists()).toBe(true)
  })
})

describe('session isolation', () => {
  it.each(['account-switch', 'logout'])('late success and failure are discarded after %s', async (boundary) => {
    const store = useHomeStore()
    const tasks = deferred<TaskListResponse>()
    const redemptions = deferred<RedemptionListResponse>()
    vi.mocked(listTasks).mockReturnValueOnce(tasks.promise)
    vi.mocked(listRedemptions).mockReturnValueOnce(redemptions.promise)
    const pending = store.refresh()
    if (boundary === 'logout') {
      vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
      await useSessionStore().logout()
    } else {
      signIn('other')
    }
    tasks.resolve({ tasks: [task('old')], next_cursor: null })
    redemptions.reject(new ApiError('INTERNAL_ERROR', 'down', 500))
    await pending
    expect(store.tasks).toMatchObject({ status: 'idle', items: [], errorText: '' })
    expect(store.history).toMatchObject({ status: 'idle', items: [], errorText: '' })
    expect(store.redemptions).toMatchObject({ status: 'idle', items: [], errorText: '' })
  })

  it('a newer refresh wins over an older in-flight one', async () => {
    const store = useHomeStore()
    const older = deferred<TaskListResponse>()
    vi.mocked(listTasks).mockReturnValueOnce(older.promise).mockResolvedValueOnce({ tasks: [task('new')], next_cursor: null })
    const first = store.loadTasks()
    await store.loadTasks()
    older.resolve({ tasks: [task('old')], next_cursor: null })
    await first
    expect(store.tasks.items.map((t) => t.id)).toEqual(['new'])
  })

  it('shows the own nickname and clears home data on account switch', async () => {
    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [task('mine')], next_cursor: null })
    const w = mountHome()
    await flushPromises()
    expect(w.find('[data-test="home-me"]').text()).toContain('昵称self')
    expect(w.find('[data-test="today-mine"]').exists()).toBe(true)
    signIn('other')
    await flushPromises()
    expect(w.find('[data-test="home-me"]').text()).toContain('昵称other')
    expect(w.find('[data-test="today-mine"]').exists()).toBe(false)
  })
})

describe('home refresh leaves task and reward actions alone', () => {
  it('unknown completion and redemption actions keep their keys across home refreshes', async () => {
    const tasksStore = useTasksStore()
    const rewards = useRewardsStore()
    vi.mocked(completeTask).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    vi.mocked(redeemRewardProduct).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    await tasksStore.complete(task('t1'))
    await rewards.redeem({ id: 'p1', title: '奖励', version: 2 })
    const taskAction = { ...tasksStore.completionActions.t1 }
    const rewardAction = { ...rewards.actions['redeem:p1'] }
    expect(taskAction.status).toBe('unknown')
    expect(rewardAction.status).toBe('unknown')
    const views = JSON.stringify(tasksStore.views)
    mountHome()
    await flushPromises()
    await wrapper!.find('[data-test="home-refresh"]').trigger('click')
    await flushPromises()
    expect(tasksStore.completionActions.t1).toEqual(taskAction)
    expect(rewards.actions['redeem:p1']).toEqual(rewardAction)
    expect(JSON.stringify(tasksStore.views)).toBe(views)
    expect(completeTask).toHaveBeenCalledTimes(1)
    expect(redeemRewardProduct).toHaveBeenCalledTimes(1)
  })
})
