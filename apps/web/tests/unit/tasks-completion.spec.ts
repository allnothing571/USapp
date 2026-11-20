/**
 * tasks store 的完成动作（M3-C2，设计第 6.4 节）。
 * 完成请求走真实的 api/tasks.completeTask 与共享 client（fetch 被替换），
 * 列表、详情与钱包读取被 mock。只覆盖五类行为与键的生命周期，不测文案与布局。
 */
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/api/tasks')>()
  return { ...actual, createTask: vi.fn(), listTasks: vi.fn(), getTask: vi.fn(), newIdempotencyKey: vi.fn() }
})
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { ApiError, setCsrfToken } from '@/api/client'
import { listTasks, newIdempotencyKey } from '@/api/tasks'
import type { Task, TaskListResponse } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'
import { useWalletStore } from '@/stores/wallet'

function task(id: string, overrides: Partial<Task> = {}): Task {
  return {
    id,
    title: `task ${id}`,
    description: '',
    creator_id: 'partner',
    assignee_id: 'self',
    reward_flowers: 2,
    reward_sunshine: 0,
    deadline: null,
    status: 'open',
    completed_at: null,
    created_at: '2026-10-14T00:00:00.000000Z',
    ...overrides,
  }
}

function signIn(id = 'self'): void {
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-14T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-14T00:00:00Z',
      members: [] },
  })
}

function reply(status: number, body: unknown) {
  return { ok: status >= 200 && status < 300, status, json: async () => body, text: async () => JSON.stringify(body) }
}

function success(id = 't1') {
  return reply(200, {
    task: task(id, { status: 'completed', completed_at: '2026-10-14T01:00:00.000000Z' }),
    settlement: { operation_id: '1f8fad5b-d9cb-469f-a165-70867728950e', flower: 2, sunshine: 0 },
  })
}

function failure(status: number, code: string) {
  return reply(status, { error: { code, message: code } })
}

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => {
    resolve = yes
    reject = no
  })
  return { promise, resolve, reject }
}

let fetchMock: ReturnType<typeof vi.fn>
let keyCounter = 0

function keysSent(): string[] {
  return fetchMock.mock.calls.map((call) => ((call[1] as RequestInit).headers as Record<string, string>)['Idempotency-Key'])
}

beforeEach(() => {
  setActivePinia(createPinia())
  fetchMock = vi.fn()
  vi.stubGlobal('fetch', fetchMock)
  keyCounter = 0
  vi.mocked(newIdempotencyKey).mockReset()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++keyCounter).padStart(16, '0')}`)
  vi.mocked(listTasks).mockReset()
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(getWallet).mockReset()
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 2, sunshine: 0 } })
  signIn()
})

afterEach(() => {
  vi.unstubAllGlobals()
  Reflect.deleteProperty(window.navigator, 'onLine')
})

describe('completion: manual same-key retry after an unknown result', () => {
  it.each([
    ['503 RESULT_UNKNOWN', () => Promise.resolve(failure(503, 'RESULT_UNKNOWN'))],
    ['500', () => Promise.resolve(failure(500, 'INTERNAL_ERROR'))],
    ['network error', () => Promise.reject(new TypeError('network'))],
    ['response body read failure', () => Promise.resolve({
      ...success(), text: async () => { throw new TypeError('connection lost while reading body') },
    })],
    ['invalid JSON in HTTP 200', () => Promise.resolve({ ...success(), text: async () => '{invalid' })],
    ['unusable settlement in HTTP 200', () => Promise.resolve(reply(200, {
      task: task('t1', { status: 'completed', completed_at: '2026-10-14T01:00:00.000000Z' }),
      settlement: null,
    }))],
  ])('after %s the key is kept and only an explicit retry resends it; a 200 replay is confirmed', async (_l, first) => {
    const store = useTasksStore()
    fetchMock.mockImplementationOnce(first).mockResolvedValueOnce(success())
    expect((await store.complete(task('t1'))).outcome).toBe('unknown')
    expect(store.completionActions.t1.status).toBe('unknown')
    const key = store.completionActions.t1.key
    expect(store.completionNotices).toEqual([])
    expect(listTasks).not.toHaveBeenCalled()
    expect(getWallet).not.toHaveBeenCalled()
    expect(fetchMock).toHaveBeenCalledTimes(1)

    expect((await store.retryCompletion('t1')).outcome).toBe('completed')
    expect(keysSent()).toEqual([key, key])
    expect(store.completionActions.t1).toBeUndefined()
    expect(store.completionNotices[0]).toMatchObject({ taskId: 't1', kind: 'completed', flower: 2, sunshine: 0 })
    expect(getWallet).toHaveBeenCalledTimes(1)
    expect(listTasks).toHaveBeenCalledWith('open')
    expect(listTasks).toHaveBeenCalledWith('history')
  })

  it('a duplicate click while submitting sends one request', async () => {
    const store = useTasksStore()
    const pending = deferred<ReturnType<typeof reply>>()
    fetchMock.mockReturnValueOnce(pending.promise)
    const first = store.complete(task('t1'))
    expect((await store.complete(task('t1'))).outcome).toBe('busy')
    expect((await store.retryCompletion('t1')).outcome).toBe('busy')
    pending.resolve(success())
    expect((await first).outcome).toBe('completed')
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('a definitive 4xx ends the action; expired/already-completed reload both views; the next attempt uses a new key', async () => {
    const store = useTasksStore()
    fetchMock.mockResolvedValueOnce(failure(409, 'TASK_EXPIRED')).mockResolvedValueOnce(success())
    expect((await store.complete(task('t1')))).toEqual({ outcome: 'rejected', errorCode: 'TASK_EXPIRED' })
    expect(store.completionActions.t1).toBeUndefined()
    expect(store.completionNotices[0]).toMatchObject({ taskId: 't1', kind: 'rejected' })
    expect(listTasks).toHaveBeenCalledWith('open')
    expect(listTasks).toHaveBeenCalledWith('history')
    expect(getWallet).not.toHaveBeenCalled()
    await store.complete(task('t1'))
    expect(keysSent()[1]).not.toBe(keysSent()[0])
  })
})

describe('completion: pending retry independent of list membership', () => {
  it('a refresh that removes the task keeps the action, its key and the retry', async () => {
    const store = useTasksStore()
    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [task('t1')], next_cursor: null })
    await store.loadFirst('open')
    fetchMock.mockResolvedValueOnce(failure(503, 'RESULT_UNKNOWN')).mockResolvedValueOnce(success())
    await store.complete(task('t1'))
    const key = store.completionActions.t1.key

    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [], next_cursor: null })
    await store.loadFirst('open')
    expect(store.views.open.items).toEqual([])
    expect(store.completionActions.t1).toMatchObject({ key, status: 'unknown', title: 'task t1' })

    expect((await store.retryCompletion('t1')).outcome).toBe('completed')
    expect(keysSent()).toEqual([key, key])
  })
})

describe('completion: logout and account switch', () => {
  it.each(['logout', 'switch'])('%s discards a late success or failure without notice, list change or wallet load', async (how) => {
    for (const outcome of ['success', 'failure'] as const) {
      setActivePinia(createPinia())
      signIn()
      vi.mocked(listTasks).mockClear()
      vi.mocked(getWallet).mockClear()
      const store = useTasksStore()
      const pending = deferred<ReturnType<typeof reply>>()
      fetchMock.mockReturnValueOnce(pending.promise)
      const result = store.complete(task('t1'))
      await vi.waitFor(() => expect(fetchMock).toHaveBeenCalled())
      if (how === 'logout') useSessionStore().resetBusinessState()
      else signIn('other')
      expect(store.completionActions).toEqual({})
      if (outcome === 'success') pending.resolve(success())
      else pending.resolve(failure(503, 'RESULT_UNKNOWN'))
      expect((await result).outcome).toBe('discarded')
      expect(store.completionNotices).toEqual([])
      expect(store.completionActions).toEqual({})
      expect(getWallet).not.toHaveBeenCalled()
      expect(listTasks).not.toHaveBeenCalled()
      fetchMock.mockClear()
    }
  })
})

describe('completion: offline', () => {
  it('a missing local CSRF token keeps the key without sending; explicit retry can recover', async () => {
    const store = useTasksStore()
    setCsrfToken(null)
    expect((await store.complete(task('t1'))).outcome).toBe('not_sent')
    expect(fetchMock).not.toHaveBeenCalled()
    expect(store.completionActions.t1.status).toBe('not_sent')
    expect(store.completionNotices).toEqual([])
    expect(listTasks).not.toHaveBeenCalled()
    expect(getWallet).not.toHaveBeenCalled()
    const key = store.completionActions.t1.key

    setCsrfToken('csrf-self')
    fetchMock.mockResolvedValueOnce(success())
    expect((await store.retryCompletion('t1')).outcome).toBe('completed')
    expect(keysSent()).toEqual([key])
  })

  it('is blocked before fetch, is not sent when connectivity returns, and an explicit retry reuses the key', async () => {
    const store = useTasksStore()
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => false })
    expect((await store.complete(task('t1'))).outcome).toBe('not_sent')
    expect(fetchMock).not.toHaveBeenCalled()
    expect(store.completionActions.t1.status).toBe('not_sent')
    const key = store.completionActions.t1.key

    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => true })
    window.dispatchEvent(new Event('online'))
    await Promise.resolve()
    expect(fetchMock).not.toHaveBeenCalled()

    fetchMock.mockResolvedValueOnce(success())
    expect((await store.retryCompletion('t1')).outcome).toBe('completed')
    expect(keysSent()).toEqual([key])
  })
})

describe('completion: confirmed success is independent of refresh failures', () => {
  it('removes the task from open, ignores older page responses, and stays confirmed when list and wallet reloads fail', async () => {
    const store = useTasksStore()
    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [task('t1'), task('t2')], next_cursor: null })
    await store.loadFirst('open')
    const stale = deferred<TaskListResponse>()
    vi.mocked(listTasks)
      .mockReturnValueOnce(stale.promise) // an older open reload still in flight
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0)) // open reload after completion
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0)) // history reload after completion
    const staleLoad = store.loadFirst('open')
    vi.mocked(getWallet).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    fetchMock.mockResolvedValueOnce(success('t1'))

    expect((await store.complete(task('t1'))).outcome).toBe('completed')
    stale.resolve({ tasks: [task('t1'), task('t2')], next_cursor: null })
    await staleLoad
    await vi.waitFor(() => expect(store.views.history.status).toBe('error'))
    await vi.waitFor(() => expect(useWalletStore().status).toBe('error'))

    expect(store.views.open.items.map((t) => t.id)).not.toContain('t1')
    expect(store.views.open.status).toBe('error')
    expect(store.completionNotices[0]).toMatchObject({ taskId: 't1', kind: 'completed', flower: 2 })
    expect(store.completionActions.t1).toBeUndefined()
    expect(getWallet).toHaveBeenCalledTimes(1)
    expect(useWalletStore().balances).toBeNull()
  })
})
