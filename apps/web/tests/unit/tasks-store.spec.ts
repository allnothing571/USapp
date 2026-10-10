/**
 * tasks store（M3-B）：请求标识的生命周期、分页、会话失效与列表请求顺序。
 * 网络层整体 mock；会话用 applyBootstrap 建立（不发网络请求）。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', () => ({
  createTask: vi.fn(),
  listTasks: vi.fn(),
  getTask: vi.fn(),
  newIdempotencyKey: vi.fn(),
}))

import { ApiError, OfflineMutationBlockedError } from '@/api/client'
import { createTask, getTask, listTasks, newIdempotencyKey } from '@/api/tasks'
import type { CreateTaskRequest, Task, TaskListResponse, TaskResponse, User } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'

function user(id: string): User {
  return { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-09T00:00:00Z' }
}

function task(id: string, overrides: Partial<Task> = {}): Task {
  return {
    id,
    title: `task ${id}`,
    description: '',
    creator_id: 'self',
    assignee_id: 'partner',
    reward_flowers: 1,
    reward_sunshine: 0,
    deadline: null,
    status: 'open',
    completed_at: null,
    created_at: '2026-10-09T00:00:00.000000Z',
    ...overrides,
  }
}

function signIn(id = 'self'): void {
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: 'csrf',
    user: user(id),
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-09T00:00:00Z', members: [] },
  })
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

const PAYLOAD: CreateTaskRequest = { title: 'Water', description: '', reward_flowers: 2, reward_sunshine: 0 }
let keyCounter = 0

beforeEach(() => {
  setActivePinia(createPinia())
  vi.mocked(createTask).mockReset()
  vi.mocked(listTasks).mockReset()
  vi.mocked(getTask).mockReset()
  vi.mocked(newIdempotencyKey).mockReset()
  keyCounter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++keyCounter).padStart(16, '0')}`)
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  signIn()
})

function keysSent(): string[] {
  return vi.mocked(createTask).mock.calls.map((call) => call[1])
}

describe('publish idempotency key lifetime', () => {
  it.each([
    ['503 RESULT_UNKNOWN', new ApiError('RESULT_UNKNOWN', 'unknown', 503)],
    ['500', new ApiError('INTERNAL_ERROR', 'boom', 500)],
    ['network error', new ApiError('NETWORK_ERROR', 'offline', 0)],
  ])('keeps the same key for a retry of the same publication after %s', async (_label, failure) => {
    const store = useTasksStore()
    vi.mocked(createTask).mockRejectedValueOnce(failure).mockResolvedValueOnce({ task: task('t1') })
    expect((await store.publish(PAYLOAD)).outcome).toBe('unknown')
    expect(store.publishStatus).toBe('unknown')
    expect(store.publishErrorText).toContain('不会重复发布')
    expect(store.hasPendingPublish()).toBe(true)
    expect((await store.retryPublish()).outcome).toBe('published')
    expect(keysSent()).toEqual([keysSent()[0], keysSent()[0]])
    // 明确成功之后，下一次发布（即使内容相同）使用新键。
    vi.mocked(createTask).mockResolvedValueOnce({ task: task('t2') })
    await store.publish(PAYLOAD)
    expect(keysSent()[2]).not.toBe(keysSent()[0])
    expect(store.hasPendingPublish()).toBe(false)
  })

  it('a definitive 4xx ends the action and the next attempt uses a new key', async () => {
    const store = useTasksStore()
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('VALIDATION_ERROR', '截止时间必须晚于当前时间', 400))
      .mockRejectedValueOnce(new ApiError('IDEMPOTENCY_KEY_REUSED', 'conflict', 409))
    const first = await store.publish(PAYLOAD)
    expect(first).toEqual({ outcome: 'rejected', errorCode: 'VALIDATION_ERROR' })
    expect(store.publishErrorText).toBe('截止时间必须晚于当前时间')
    expect(store.hasPendingPublish()).toBe(false)
    await store.publish(PAYLOAD)
    const [k1, k2] = keysSent()
    expect(k1).not.toBe(k2)
  })

  it('changing the content after an unconfirmed result uses a new key', async () => {
    const store = useTasksStore()
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockResolvedValueOnce({ task: task('t1') })
    await store.publish(PAYLOAD)
    await store.publish({ ...PAYLOAD, reward_flowers: 3 })
    const [k1, k2] = keysSent()
    expect(k1).not.toBe(k2)
  })

  it('an offline-blocked publish is not sent, keeps its key and is never replayed automatically', async () => {
    const store = useTasksStore()
    vi.mocked(createTask).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect((await store.publish(PAYLOAD)).outcome).toBe('not_sent')
    expect(store.publishStatus).toBe('error')
    expect(store.publishErrorText).toContain('离线')
    window.dispatchEvent(new Event('online'))
    await Promise.resolve()
    expect(createTask).toHaveBeenCalledTimes(1)
    vi.mocked(createTask).mockResolvedValueOnce({ task: task('t1') })
    await store.publish(PAYLOAD)
    expect(keysSent()[1]).toBe(keysSent()[0])
  })

  it('reset (logout or account change) discards pending keys', async () => {
    const store = useTasksStore()
    vi.mocked(createTask).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    await store.publish(PAYLOAD)
    useSessionStore().resetBusinessState()
    signIn()
    expect(store.hasPendingPublish()).toBe(false)
    vi.mocked(createTask).mockResolvedValueOnce({ task: task('t1') })
    await store.publish(PAYLOAD)
    expect(keysSent()[1]).not.toBe(keysSent()[0])
  })

  it('a successful publish reloads the open view from page one', async () => {
    const store = useTasksStore()
    vi.mocked(createTask).mockResolvedValueOnce({ task: task('t1') })
    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [task('t1')], next_cursor: null })
    const result = await store.publish(PAYLOAD)
    expect(result.outcome).toBe('published')
    await vi.waitFor(() => expect(store.views.open.status).toBe('ready'))
    expect(listTasks).toHaveBeenCalledWith('open')
    expect(store.views.open.items.map((t) => t.id)).toEqual(['t1'])
  })
})

describe('late publish responses after logout or account change are discarded', () => {
  it.each(['success', 'failure'])('logout during an in-flight publish drops a late %s', async (kind) => {
    const store = useTasksStore()
    const reply = deferred<TaskResponse>()
    vi.mocked(createTask).mockReturnValueOnce(reply.promise)
    const pending = store.publish(PAYLOAD)
    await vi.waitFor(() => expect(createTask).toHaveBeenCalledTimes(1))
    useSessionStore().resetBusinessState()
    if (kind === 'success') reply.resolve({ task: task('late') })
    else reply.reject(new ApiError('RESULT_UNKNOWN', 'late', 503))
    expect((await pending).outcome).toBe('discarded')
    expect(store.publishStatus).toBe('idle')
    expect(store.publishErrorText).toBe('')
    expect(store.views.open.items).toEqual([])
    expect(listTasks).not.toHaveBeenCalled()
    expect(store.hasPendingPublish()).toBe(false)
  })

  it.each(['success', 'failure'])('switching to another account drops a late %s from the old account', async (kind) => {
    const store = useTasksStore()
    const reply = deferred<TaskResponse>()
    vi.mocked(createTask).mockReturnValueOnce(reply.promise)
    const pending = store.publish(PAYLOAD)
    await vi.waitFor(() => expect(createTask).toHaveBeenCalledTimes(1))
    signIn('other')
    vi.mocked(listTasks).mockResolvedValueOnce({ tasks: [task('mine', { creator_id: 'other' })], next_cursor: null })
    await store.loadFirst('open')
    if (kind === 'success') reply.resolve({ task: task('late') })
    else reply.reject(new ApiError('NETWORK_ERROR', 'late', 0))
    expect((await pending).outcome).toBe('discarded')
    expect(store.views.open.items.map((t) => t.id)).toEqual(['mine'])
    expect(store.publishStatus).toBe('idle')
    expect(store.publishErrorText).toBe('')
    expect(listTasks).toHaveBeenCalledTimes(1)
  })
})

describe('pagination and list ordering', () => {
  it('load more passes the cursor back, appends without duplicates, and reload restarts from page one', async () => {
    const store = useTasksStore()
    vi.mocked(listTasks)
      .mockResolvedValueOnce({ tasks: [task('a'), task('b')], next_cursor: 'c1' })
      .mockResolvedValueOnce({ tasks: [task('b'), task('c')], next_cursor: 'c2' })
      .mockResolvedValueOnce({ tasks: [task('d')], next_cursor: null })
      .mockResolvedValueOnce({ tasks: [task('z')], next_cursor: null })
    await store.loadFirst('open')
    expect(store.views.open.nextCursor).toBe('c1')
    await store.loadMore('open')
    await store.loadMore('open')
    expect(vi.mocked(listTasks).mock.calls.slice(0, 3)).toEqual([['open'], ['open', 'c1'], ['open', 'c2']])
    expect(store.views.open.items.map((t) => t.id)).toEqual(['a', 'b', 'c', 'd'])
    expect(store.views.open.nextCursor).toBeNull()
    await store.loadMore('open')  // 没有游标：不发请求
    expect(listTasks).toHaveBeenCalledTimes(3)
    await store.loadFirst('open')
    expect(vi.mocked(listTasks).mock.calls[3]).toEqual(['open'])
    expect(store.views.open.items.map((t) => t.id)).toEqual(['z'])
  })

  it('a newer first-page load wins over an older response; views are independent', async () => {
    const store = useTasksStore()
    const older = deferred<TaskListResponse>()
    const newer = deferred<TaskListResponse>()
    vi.mocked(listTasks)
      .mockReturnValueOnce(older.promise)
      .mockReturnValueOnce(newer.promise)
      .mockResolvedValueOnce({ tasks: [task('h')], next_cursor: null })
    const oldPending = store.loadFirst('open')
    const newPending = store.loadFirst('open')
    await store.loadFirst('history')
    newer.resolve({ tasks: [task('new')], next_cursor: null })
    await newPending
    older.resolve({ tasks: [task('old')], next_cursor: 'stale' })
    await oldPending
    expect(store.views.open.items.map((t) => t.id)).toEqual(['new'])
    expect(store.views.open.nextCursor).toBeNull()
    expect(store.views.history.items.map((t) => t.id)).toEqual(['h'])
  })

  it('a first-page error shows no stale rows; a load-more error keeps loaded rows and offers a reload', async () => {
    const store = useTasksStore()
    vi.mocked(listTasks)
      .mockResolvedValueOnce({ tasks: [task('a')], next_cursor: 'c1' })
      .mockRejectedValueOnce(new ApiError('VALIDATION_ERROR', 'bad cursor', 400))
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    await store.loadFirst('open')
    await store.loadMore('open')
    expect(store.views.open.items.map((t) => t.id)).toEqual(['a'])
    expect(store.views.open.moreErrorText).toContain('重新加载')
    expect(store.views.open.loadingMore).toBe(false)
    await store.loadFirst('open')
    expect(store.views.open.status).toBe('error')
    expect(store.views.open.items).toEqual([])
    expect(store.views.open.nextCursor).toBeNull()
    expect(store.views.open.errorCode).toBe('NETWORK_ERROR')
  })

  it('a first-page reload invalidates an in-flight load more', async () => {
    const store = useTasksStore()
    const more = deferred<TaskListResponse>()
    vi.mocked(listTasks)
      .mockResolvedValueOnce({ tasks: [task('a')], next_cursor: 'c1' })
      .mockReturnValueOnce(more.promise)
      .mockResolvedValueOnce({ tasks: [task('fresh')], next_cursor: null })
    await store.loadFirst('open')
    const pendingMore = store.loadMore('open')
    await store.loadFirst('open')
    more.resolve({ tasks: [task('stale')], next_cursor: 'c9' })
    await pendingMore
    expect(store.views.open.items.map((t) => t.id)).toEqual(['fresh'])
    expect(store.views.open.nextCursor).toBeNull()
  })

  it.each(['success', 'failure'])('a late list or detail %s after logout is ignored', async (kind) => {
    const store = useTasksStore()
    const list = deferred<TaskListResponse>()
    const detail = deferred<TaskResponse>()
    vi.mocked(listTasks).mockReturnValueOnce(list.promise)
    vi.mocked(getTask).mockReturnValueOnce(detail.promise)
    const listPending = store.loadFirst('history')
    const detailPending = store.openDetail('t1')
    useSessionStore().resetBusinessState()
    if (kind === 'success') {
      list.resolve({ tasks: [task('x')], next_cursor: null })
      detail.resolve({ task: task('t1') })
    } else {
      list.reject(new ApiError('NETWORK_ERROR', 'late', 0))
      detail.reject(new ApiError('TASK_NOT_FOUND', 'late', 404))
    }
    await listPending
    await detailPending
    expect(store.views.history.status).toBe('idle')
    expect(store.views.history.items).toEqual([])
    expect(store.views.history.errorText).toBe('')
    expect(store.detail).toBeNull()
    expect(store.detailStatus).toBe('idle')
  })
})
