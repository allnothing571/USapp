/**
 * M6 task changes: store action lifecycle (key kept on an unknown result, explicit same-key
 * retry, definitive answers end the action) and the detail view's propose/decide controls.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', () => ({
  createTask: vi.fn(),
  completeTask: vi.fn(),
  progressTask: vi.fn(),
  listTasks: vi.fn(),
  getTask: vi.fn(),
  newIdempotencyKey: vi.fn(),
  proposeTaskChange: vi.fn(),
  decideTaskChange: vi.fn(),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { ApiError, setCsrfToken } from '@/api/client'
import { decideTaskChange, getTask, listTasks, newIdempotencyKey, proposeTaskChange } from '@/api/tasks'
import type { Task, TaskChangeResponse, TaskPendingChange } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'
import TasksView from '@/views/TasksView.vue'

function task(id: string, overrides: Partial<Task> = {}): Task {
  return {
    id,
    title: `任务 ${id}`,
    description: '',
    creator_id: 'partner',
    assignee_id: 'self',
    reward_flowers: 2,
    reward_sunshine: 0,
    deadline: null,
    status: 'open',
    completed_at: null,
    created_at: '2026-10-25T00:00:00.000000Z',
    ...overrides,
  }
}

function pending(overrides: Partial<TaskPendingChange> = {}): TaskPendingChange {
  return {
    id: 'p1', kind: 'edit', proposer_id: 'partner', created_at: '2026-10-25T01:00:00.000000Z',
    terms: { title: '新标题', description: '', reward_flowers: 5, reward_sunshine: 0, completion_mode: 'single',
      target_count: 1, deadline: null },
    blocked_reason: null,
    ...overrides,
  }
}

function changeReply(t: Task): TaskChangeResponse {
  return {
    change: { id: 'p1', task_id: t.id, kind: 'edit', proposer_id: 'self', created_at: '2026-10-25T01:00:00.000000Z',
      terms: null, decision: null },
    task: t,
    successor: null,
  }
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  for (const fn of [proposeTaskChange, decideTaskChange, listTasks, getTask, getWallet]) vi.mocked(fn).mockReset()
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 0, sunshine: 0 } })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockReset()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: 'csrf',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-25T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-25T00:00:00Z',
      members: [{ user_id: 'partner', slot: 2, display_name: '小 B', joined_at: '2026-10-25T00:00:00Z' }] },
  })
  setCsrfToken('csrf')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.restoreAllMocks()
})

describe('task change actions (store)', () => {
  it('keeps the original proposal key after an unknown result and a rejected retry', async () => {
    const store = useTasksStore()
    const t = task('t1', { creator_id: 'self', assignee_id: 'partner' })
    vi.mocked(proposeTaskChange).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(new ApiError('TASK_NOT_FOUND', 'not found', 404))
      .mockResolvedValueOnce(changeReply(t))
    expect((await store.proposeChange(t, { kind: 'cancel' })).outcome).toBe('unknown')
    const key = store.changeActions.t1.key
    expect((await store.retryChange('t1')).outcome).toBe('unknown')
    expect(store.changeActions.t1).toMatchObject({ key, status: 'unknown', unresolved: true })
    expect((await store.retryChange('t1')).outcome).toBe('done')
    expect(vi.mocked(proposeTaskChange).mock.calls.map((call) => call[2])).toEqual([key, key, key])
  })
  it('keeps the key after an unknown result and resends it only on an explicit retry', async () => {
    const store = useTasksStore()
    const t = task('t1', { creator_id: 'self', assignee_id: 'partner' })
    vi.mocked(proposeTaskChange)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(changeReply(t))
    expect((await store.proposeChange(t, { kind: 'cancel' })).outcome).toBe('unknown')
    expect(store.changeActions.t1.status).toBe('unknown')
    expect(store.changeNotices).toEqual([])
    expect((await store.retryChange('t1')).outcome).toBe('done')
    const keys = vi.mocked(proposeTaskChange).mock.calls.map((call) => call[2])
    expect(keys).toEqual([keys[0], keys[0]])
    expect(vi.mocked(proposeTaskChange).mock.calls[1][1]).toEqual({ kind: 'cancel' })
    expect(store.changeActions.t1).toBeUndefined()
    expect(store.changeNotices[0]).toMatchObject({ taskId: 't1', ok: true })
    expect(listTasks).toHaveBeenCalled()
  })

  it('a definitive refusal ends the action with a notice; the proposal id and action are sent', async () => {
    const store = useTasksStore()
    const t = task('t2', { pending_change: pending() })
    vi.mocked(decideTaskChange).mockRejectedValueOnce(new ApiError('TASK_HAS_PROGRESS', 'progress', 409))
    const result = await store.decideChange(t, 'approve')
    expect(result).toEqual({ outcome: 'rejected', errorCode: 'TASK_HAS_PROGRESS' })
    expect(vi.mocked(decideTaskChange).mock.calls[0].slice(0, 3)).toEqual(['t2', 'p1', 'approve'])
    expect(store.changeActions.t2).toBeUndefined()
    expect(store.changeNotices[0]).toMatchObject({ taskId: 't2', ok: false })
    expect((await store.decideChange(task('t3'), 'approve')).outcome).toBe('discarded')
  })
})

describe('task change controls (view)', () => {
  async function openDetail(t: Task) {
    vi.mocked(listTasks).mockImplementation(async (view) => ({ tasks: view === 'open' ? [t] : [], next_cursor: null }))
    vi.mocked(getTask).mockResolvedValue({ task: t })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="open-view"] .list__item').trigger('click')
    await flushPromises()
    return wrapper
  }

  it('the recipient sees a pending edit with a warning; a blocked approval is disabled but reject works', async () => {
    const t = task('t4', { completion_mode: 'count', target_count: 3, progress_count: 1,
      pending_change: pending({ blocked_reason: 'progress' }) })
    vi.mocked(decideTaskChange).mockResolvedValue(changeReply(t))
    const w = await openDetail(t)
    expect(w.find('[data-test="edit-pending-t4"]').exists()).toBe(true)
    expect(w.find('[data-test="pending-change"]').text()).toContain('小 B')
    expect(w.find('[data-test="change-blocked"]').exists()).toBe(true)
    expect(w.find('[data-test="change-approve"]').attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="change-withdraw"]').exists()).toBe(false)
    await w.find('[data-test="change-reject"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(decideTaskChange).mock.calls[0].slice(0, 3)).toEqual(['t4', 'p1', 'reject'])
  })

  it('the creator proposes an edit from a prefilled form; the schedule type stays fixed', async () => {
    const t = task('t5', { creator_id: 'self', assignee_id: 'partner', reward_flowers: 2 })
    vi.mocked(proposeTaskChange).mockResolvedValue(changeReply(t))
    const w = await openDetail(t)
    await w.find('[data-test="change-edit"]').trigger('click')
    expect((w.find('#edit-title').element as HTMLInputElement).value).toBe('任务 t5')
    await w.find('#edit-flowers').setValue('7')
    await w.find('[data-test="change-form"]').trigger('submit')
    await flushPromises()
    const [id, body] = vi.mocked(proposeTaskChange).mock.calls[0]
    expect(id).toBe('t5')
    expect(body).toEqual({ kind: 'edit', terms: { title: '任务 t5', description: '', reward_flowers: 7, reward_sunshine: 0 } })
    expect(w.find('[data-test="change-cancel"]').exists()).toBe(true)
  })

  it('a superseded task shows its closure and no change controls', async () => {
    const t = task('t6', { status: 'superseded',
      closure: { reason: 'superseded', closed_at: '2026-10-25T02:00:00.000000Z', replaced_by: 't7' } })
    vi.mocked(listTasks).mockImplementation(async (view) => ({ tasks: view === 'history' ? [t] : [], next_cursor: null }))
    vi.mocked(getTask).mockResolvedValue({ task: t })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="history-view"]').text()).toContain('已替换')
    await wrapper.find('[data-test="history-view"] .list__item').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="task-closure"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="change-edit"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="change-cancel"]').exists()).toBe(false)
  })
})
