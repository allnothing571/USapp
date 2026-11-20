import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', async importOriginal => {
  const actual = await importOriginal<typeof import('@/api/tasks')>()
  return { ...actual, listTasks: vi.fn(), getTask: vi.fn(), createTask: vi.fn(), newIdempotencyKey: vi.fn() }
})
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { getTask, listTasks, newIdempotencyKey } from '@/api/tasks'
import { buildCreateTaskRequest, nextDailyRefreshDelay, periodLabel } from '@/api/task-rules'
import type { ProgressTaskResponse, Task } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'

const counted: Task = { id: 'count', title: 'Count', description: '', creator_id: 'partner', assignee_id: 'self',
  reward_flowers: 2, reward_sunshine: 3, completion_mode: 'count', target_count: 2, progress_count: 0,
  deadline: null, status: 'open', completed_at: null, created_at: '2026-10-14T00:00:00.000000Z' }
const event = (n = 1): ProgressTaskResponse => ({ task_id: 'count', event_id: `event-${n}`, progress_count: n,
  target_count: 2, recorded_at: '2026-10-14T01:00:00.000000Z', completed: n === 2, ignored: false,
  settlement: n === 2 ? { operation_id: 'operation', flower: 2, sunshine: 3 } : null })
const ok = (body: unknown) => ({ ok: true, status: 200, text: async () => JSON.stringify(body) })
function signIn(id = 'self') {
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'csrf',
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-14T00:00:00Z' }, space: null })
}
let fetchMock: ReturnType<typeof vi.fn>
beforeEach(() => {
  setActivePinia(createPinia())
  fetchMock = vi.fn()
  vi.stubGlobal('fetch', fetchMock)
  vi.mocked(listTasks).mockReset().mockResolvedValue({ tasks: [{ ...counted }], next_cursor: null })
  vi.mocked(getTask).mockReset()
  vi.mocked(getWallet).mockReset().mockResolvedValue({ wallet: { flower: 2, sunshine: 3 } })
  let key = 0
  vi.mocked(newIdempotencyKey).mockReset().mockImplementation(() => `progress-key-${String(++key).padStart(16, '0')}`)
  signIn()
})
afterEach(() => { vi.unstubAllGlobals(); Reflect.deleteProperty(window.navigator, 'onLine') })
const keys = () => fetchMock.mock.calls.map(c => (c[1].headers as Record<string, string>)['Idempotency-Key'])

describe('counting progress', () => {
  it('keeps the same progress key after an unknown result and a pre-classification 4xx', async () => {
    const store = useTasksStore()
    fetchMock.mockRejectedValueOnce(new TypeError('response lost'))
      .mockResolvedValueOnce({ ok: false, status: 403, json: async () => ({ error: { code: 'CSRF_TOKEN_INVALID', message: 'csrf' } }) })
      .mockResolvedValueOnce(ok(event()))
    expect((await store.progress(counted)).outcome).toBe('unknown')
    const key = store.completionActions.count.key
    expect((await store.retryCompletion('count')).outcome).toBe('unknown')
    expect(store.completionActions.count).toMatchObject({ key, status: 'unknown', unresolved: true })
    expect((await store.progress(counted)).outcome).toBe('busy')
    expect((await store.retryCompletion('count')).outcome).toBe('progressed')
    expect(keys()).toEqual([key, key, key])
  })

  it('keeps an unknown progress action when the next retry is blocked offline', async () => {
    const store = useTasksStore()
    fetchMock.mockRejectedValueOnce(new TypeError('response lost')).mockResolvedValueOnce(ok(event()))
    expect((await store.progress(counted)).outcome).toBe('unknown')
    const key = store.completionActions.count.key
    Object.defineProperty(navigator, 'onLine', { configurable: true, value: false })
    expect((await store.retryCompletion('count')).outcome).toBe('unknown')
    expect(store.completionActions.count).toMatchObject({ key, status: 'unknown', unresolved: true })
    expect(store.completionActions.count.errorText).toContain('仍未确认')
    expect(fetchMock).toHaveBeenCalledTimes(1)
    Reflect.deleteProperty(navigator, 'onLine')
    expect((await store.retryCompletion('count')).outcome).toBe('progressed')
    expect(keys()).toEqual([key, key])
  })
  it('daily form rules and the midnight read timer keep their period boundary', () => {
    const form = { title: 'D', description: '', flowers: '1', sunshine: '0', date: '', time: '',
      scheduleType: 'daily' as const, completionMode: 'count' as const, targetCount: '2' }
    const body = buildCreateTaskRequest(form)
    expect(body).toMatchObject({ ok: true, request: { schedule_type: 'daily', completion_mode: 'count', target_count: 2 } })
    expect(buildCreateTaskRequest({ ...form, completionMode: 'single' }).ok).toBe(false)
    expect(nextDailyRefreshDelay(Date.parse('2028-02-29T15:59:59.000Z'))).toBe(1050)
    expect(nextDailyRefreshDelay(Date.parse('2028-02-29T16:00:00.000Z'))).toBe(86400050)
    // M5-B: same rules for weekly; a week crossing New Year shows Monday to Sunday.
    const weekly = buildCreateTaskRequest({ ...form, scheduleType: 'weekly' })
    expect(weekly).toMatchObject({ ok: true, request: { schedule_type: 'weekly', completion_mode: 'count', target_count: 2 } })
    expect(buildCreateTaskRequest({ ...form, scheduleType: 'weekly', completionMode: 'single' }).ok).toBe(false)
    expect(periodLabel({ schedule_type: 'weekly', period_date: '2026-12-28',
      deadline: { local_date: '2027-01-03', local_time: null, at: '2027-01-03T16:00:00.000000Z' } }))
      .toBe('每周重复 · 2026-12-28 至 2027-01-03')
    // M5-C: one bounded range; inclusive dates, same day allowed, end before start rejected locally.
    const custom = { ...form, scheduleType: 'custom' as const, startDate: '2026-12-31', endDate: '2027-01-02' }
    expect(buildCreateTaskRequest(custom)).toEqual({ ok: true, request: { title: 'D', description: '',
      reward_flowers: 1, reward_sunshine: 0, completion_mode: 'count', target_count: 2, schedule_type: 'custom',
      start_date: '2026-12-31', end_date: '2027-01-02' } })
    expect(buildCreateTaskRequest({ ...custom, endDate: '2026-12-31' }).ok).toBe(true)
    expect(buildCreateTaskRequest({ ...custom, endDate: '2026-12-30' }).ok).toBe(false)
    expect(buildCreateTaskRequest({ ...custom, startDate: '' }).ok).toBe(false)
    expect(buildCreateTaskRequest({ ...custom, completionMode: 'single' }).ok).toBe(false)
    expect(periodLabel({ schedule_type: 'custom', start_date: '2026-12-31',
      deadline: { local_date: '2027-01-02', local_time: null, at: '2027-01-02T16:00:00.000000Z' } }))
      .toBe('日期范围 · 2026-12-31 至 2027-01-02')
  })

  // Shared by daily (yesterday/today) and weekly (last Monday/this Monday) periods.
  const periods: Array<['daily' | 'weekly', string, string]> = [
    ['daily', '2026-10-15', '2026-10-16'],
    ['weekly', '2026-10-12', '2026-10-19'],
  ]

  it.each(periods)('%s: a retry keeps the old period in its body; the new period starts a new action with a new key', async (schedule, oldDate, newDate) => {
    const old: Task = { ...counted, schedule_type: schedule, period_date: oldDate }
    const today: Task = { ...old, period_date: newDate, progress_count: 0 }
    const store = useTasksStore()
    fetchMock.mockRejectedValueOnce(new TypeError('response lost'))
      .mockResolvedValueOnce(ok({ ...event(), period_date: old.period_date }))
      .mockResolvedValueOnce(ok({ ...event(), period_date: today.period_date }))
    expect((await store.progress(old)).outcome).toBe('unknown')
    const key = store.completionActions.count.key
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [today] : [], next_cursor: null }))
    await store.loadFirst('open')
    expect((await store.retryCompletion('count')).outcome).toBe('progressed')
    await vi.waitFor(() => expect(store.views.open.status).toBe('ready'))
    expect(store.views.open.items[0].progress_count).toBe(0)
    expect(store.views.open.items[0].period_date).toBe(today.period_date)
    expect((await store.progress(today)).outcome).toBe('progressed')
    expect(keys()[0]).toBe(key); expect(keys()[1]).toBe(key); expect(keys()[2]).not.toBe(key)
    const bodies = fetchMock.mock.calls.map(c => JSON.parse(c[1].body as string))
    expect(bodies).toEqual([{ period_date: old.period_date }, { period_date: old.period_date }, { period_date: today.period_date }])
    expect(getWallet).not.toHaveBeenCalled()
  })

  it.each(periods)('%s: a wrong-period response remains unknown; a confirmed old completion does not complete the new period', async (schedule, oldDate, newDate) => {
    const old: Task = { ...counted, schedule_type: schedule, period_date: oldDate }
    const today: Task = { ...old, period_date: newDate, progress_count: 0 }
    const store = useTasksStore()
    fetchMock.mockResolvedValueOnce(ok({ ...event(2), period_date: today.period_date }))
      .mockResolvedValueOnce(ok({ ...event(2), period_date: old.period_date }))
    expect((await store.progress(old)).outcome).toBe('unknown')
    const key = store.completionActions.count.key
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [today] :
      [{ ...old, status: 'completed' as const, progress_count: 2 }], next_cursor: null }))
    expect((await store.retryCompletion('count')).outcome).toBe('completed')
    await vi.waitFor(() => expect(store.views.open.status).toBe('ready'))
    expect(store.views.open.items[0]).toMatchObject({ period_date: today.period_date, progress_count: 0, status: 'open' })
    expect(store.completionNotices[0].periodDate).toBe(old.period_date)
    expect(keys()).toEqual([key, key])
    expect(getWallet).toHaveBeenCalledTimes(1)
  })

  it('history keeps separate dates for the same template across pages and details use the selected date', async () => {
    const recent: Task = { ...counted, schedule_type: 'daily', period_date: '2026-10-15', status: 'completed' }
    const earlier: Task = { ...recent, period_date: '2026-10-14' }
    vi.mocked(listTasks).mockImplementation(async (_view, cursor) => ({ tasks: [cursor ? earlier : recent], next_cursor: cursor ? null : 'older' }))
    vi.mocked(getTask).mockResolvedValue({ task: earlier })
    const store = useTasksStore()
    await store.loadFirst('history')
    await store.loadMore('history')
    expect(store.views.history.items.map(t => t.period_date)).toEqual([recent.period_date, earlier.period_date])
    await store.openDetail(earlier.id, earlier.period_date)
    expect(getTask).toHaveBeenCalledWith(earlier.id, earlier.period_date)
  })

  it('uses a new key per confirmed increment, reloads authoritative progress and refreshes wallet only at N', async () => {
    const store = useTasksStore()
    fetchMock.mockResolvedValueOnce(ok(event())).mockResolvedValueOnce(ok(event(2)))
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ?
      [{ ...counted, progress_count: 1 }] : [], next_cursor: null }))
    expect((await store.progress(counted)).outcome).toBe('progressed')
    await vi.waitFor(() => expect(store.views.open.status).toBe('ready'))
    expect(store.views.open.items[0].progress_count).toBe(1)
    expect(getWallet).not.toHaveBeenCalled()
    expect(store.completionNotices[0].kind).toBe('progressed')
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'history' ?
      [{ ...counted, progress_count: 2, status: 'completed' as const }] : [], next_cursor: null }))
    expect((await store.progress(counted)).outcome).toBe('completed')
    await vi.waitFor(() => expect(store.views.history.status).toBe('ready'))
    expect(store.views.open.items).toEqual([])
    expect(store.views.history.items[0].progress_count).toBe(2)
    expect(keys()[0]).not.toBe(keys()[1])
    expect(getWallet).toHaveBeenCalledTimes(1)
    expect(store.completionNotices[0]).toMatchObject({ kind: 'completed', flower: 2, sunshine: 3 })
  })

  it.each(['503', 'body-read', 'bad-json', 'bad-shape'])('keeps the same key after %s and an old replay cannot lower newer progress', async kind => {
    const store = useTasksStore()
    const failed = kind === '503' ? { ok: false, status: 503,
      json: async () => ({ error: { code: 'RESULT_UNKNOWN' } }) } : kind === 'body-read' ?
      { ok: true, status: 200, text: async () => { throw new TypeError('lost body') } } : kind === 'bad-json' ?
      { ok: true, status: 200, text: async () => '{' } : ok({ ...event(), settlement: {} })
    fetchMock.mockResolvedValueOnce(failed).mockResolvedValueOnce(ok(event()))
    expect((await store.progress(counted)).outcome).toBe('unknown')
    const key = store.completionActions.count.key
    expect(getWallet).not.toHaveBeenCalled()
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    await store.loadFirst('open')
    expect(store.completionActions.count.key).toBe(key)
    // Another device has already reached N; replay still returns the older event.
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'history' ?
      [{ ...counted, progress_count: 2, status: 'completed' as const }] : [], next_cursor: null }))
    expect((await store.retryCompletion('count')).outcome).toBe('progressed')
    expect(keys()).toEqual([key, key])
    await vi.waitFor(() => expect(store.views.history.status).toBe('ready'))
    expect(store.views.history.items[0].progress_count).toBe(2)
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('duplicate clicks send one request, and confirmed progress survives list refresh failure', async () => {
    const store = useTasksStore()
    let release!: (value: ReturnType<typeof ok>) => void
    fetchMock.mockReturnValueOnce(new Promise<ReturnType<typeof ok>>(resolve => { release = resolve }))
    const first = store.progress(counted)
    expect((await store.progress(counted)).outcome).toBe('busy')
    vi.mocked(listTasks).mockRejectedValue(new Error('read failed'))
    release(ok(event()))
    expect((await first).outcome).toBe('progressed')
    await vi.waitFor(() => expect(store.views.open.status).toBe('error'))
    expect(store.completionNotices[0].kind).toBe('progressed')
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it.each(['logout', 'switch'])('drops a late progress result after %s', async how => {
    const store = useTasksStore()
    let release!: (value: ReturnType<typeof ok>) => void
    fetchMock.mockReturnValueOnce(new Promise<ReturnType<typeof ok>>(resolve => { release = resolve }))
    const result = store.progress(counted)
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1))
    if (how === 'logout') useSessionStore().resetBusinessState()
    else signIn('other')
    release(ok(event(2)))
    expect((await result).outcome).toBe('discarded')
    expect(store.completionActions).toEqual({})
    expect(store.completionNotices).toEqual([])
    expect(getWallet).not.toHaveBeenCalled()
    expect(listTasks).not.toHaveBeenCalled()
  })

  it('blocks offline progress before fetch and does not replay it on online', async () => {
    const store = useTasksStore()
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => false })
    expect((await store.progress(counted)).outcome).toBe('not_sent')
    const key = store.completionActions.count.key
    expect(fetchMock).not.toHaveBeenCalled()
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => true })
    window.dispatchEvent(new Event('online'))
    await Promise.resolve()
    expect(fetchMock).not.toHaveBeenCalled()
    fetchMock.mockResolvedValueOnce(ok(event()))
    await store.retryCompletion('count')
    expect(keys()).toEqual([key])
  })

  it('an already reached target is ignored and never shown as another reward', async () => {
    const store = useTasksStore()
    fetchMock.mockResolvedValueOnce(ok({ ...event(2), event_id: null, recorded_at: null, ignored: true }))
    expect((await store.progress(counted)).outcome).toBe('ignored')
    expect(store.completionNotices[0]).toMatchObject({ kind: 'ignored', flower: 0, sunshine: 0 })
  })
})
