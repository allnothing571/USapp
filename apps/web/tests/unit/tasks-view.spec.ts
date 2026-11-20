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
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { ApiError } from '@/api/client'
import { completeTask, createTask, listTasks, newIdempotencyKey, progressTask } from '@/api/tasks'
import { getWallet } from '@/api/wallet'
import type { SessionBootstrap, Task, TaskResponse } from '@/api/types'
import { useSessionStore } from '@/stores/session'
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
    deadline: { local_date: '2026-10-20', local_time: null, at: '2026-10-20T16:00:00.000000Z' },
    status: 'open',
    completed_at: null,
    created_at: '2026-10-09T00:00:00.000000Z',
    ...overrides,
  }
}

// P2-3b: a new server always returns the original notification facts.
const NOT_REQUESTED = { requested: false, created: false, reason: 'not_requested' as const, notification_id: null }

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.mocked(createTask).mockReset()
  vi.mocked(completeTask).mockReset()
  vi.mocked(progressTask).mockReset()
  vi.mocked(listTasks).mockReset()
  vi.mocked(getWallet).mockReset()
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 0, sunshine: 0 } })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockReset()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: 'csrf',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-09T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-09T00:00:00Z',
      members: [{ user_id: 'partner', slot: 2, display_name: '小 B', joined_at: '2026-10-09T00:00:00Z' }] },
  })
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.restoreAllMocks()
})

describe('tasks view', () => {
  it('daily publication forces counting; returning to the page refreshes reads without a mutation', async () => {
    vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('visible')
    const daily = task('daily', { schedule_type: 'daily', period_date: '2026-10-16', completion_mode: 'count', target_count: 2 })
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [daily] : [], next_cursor: null }))
    vi.mocked(createTask).mockResolvedValue({ notification: NOT_REQUESTED, task: daily })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('#task-schedule').setValue('daily')
    await wrapper.find('#task-target').setValue('2')
    await fillDraft(wrapper, '每天', '1')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(createTask).toHaveBeenCalledWith(expect.objectContaining({ schedule_type: 'daily', completion_mode: 'count', target_count: 2 }), expect.any(String))
    vi.mocked(listTasks).mockClear()
    window.dispatchEvent(new Event('focus'))
    await flushPromises()
    expect(listTasks).toHaveBeenCalledTimes(2)
    expect(progressTask).not.toHaveBeenCalled()
    expect(completeTask).not.toHaveBeenCalled()
  })

  it('weekly publication forces counting, shows the week range and only relevant deadline hints', async () => {
    const weekly = task('weekly', { schedule_type: 'weekly', period_date: '2026-10-19', completion_mode: 'count',
      target_count: 3, progress_count: 1,
      deadline: { local_date: '2026-10-25', local_time: null, at: '2026-10-25T16:00:00.000000Z' } })
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [weekly] : [], next_cursor: null }))
    vi.mocked(createTask).mockResolvedValue({ notification: NOT_REQUESTED, task: weekly })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="open-view"]').text()).toContain('每周重复 · 2026-10-19 至 2026-10-25')
    expect(wrapper.find('[data-test="date-only-hint"]').exists()).toBe(false)
    await wrapper.find('#task-date').setValue('2026-10-30')
    expect(wrapper.find('[data-test="date-only-hint"]').exists()).toBe(true)
    await wrapper.find('#task-time').setValue('21:30')
    expect(wrapper.find('[data-test="date-only-hint"]').exists()).toBe(false)
    await wrapper.find('#task-schedule').setValue('weekly')
    expect(wrapper.find('[data-test="weekly-hint"]').exists()).toBe(true)
    expect(wrapper.find('#task-date').exists()).toBe(false)
    expect(wrapper.find('[data-test="date-only-hint"]').exists()).toBe(false)
    expect((wrapper.find('#task-completion-mode').element as HTMLSelectElement).value).toBe('count')
    await wrapper.find('#task-target').setValue('3')
    await fillDraft(wrapper, '每周', '1')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(createTask).toHaveBeenCalledWith(expect.objectContaining({ schedule_type: 'weekly', completion_mode: 'count',
      target_count: 3 }), expect.any(String))
    expect(vi.mocked(createTask).mock.calls[0][0]).not.toHaveProperty('deadline')
  })

  it('custom range publication sends inclusive dates; an upcoming range shows its range and no +1', async () => {
    const upcoming = task('range', { schedule_type: 'custom', start_date: '2026-10-27', completion_mode: 'count',
      target_count: 3, progress_count: 0, status: 'upcoming',
      deadline: { local_date: '2026-10-29', local_time: null, at: '2026-10-29T16:00:00.000000Z' } })
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [upcoming] : [], next_cursor: null }))
    vi.mocked(createTask).mockResolvedValue({ notification: NOT_REQUESTED, task: upcoming })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const open = wrapper.find('[data-test="open-view"]')
    expect(open.text()).toContain('日期范围 · 2026-10-27 至 2026-10-29')
    expect(open.text()).toContain('未开始')
    expect(wrapper.find('[data-test="progress-range"]').exists()).toBe(false)
    await wrapper.find('#task-schedule').setValue('custom')
    expect(wrapper.find('[data-test="custom-hint"]').exists()).toBe(true)
    expect(wrapper.find('#task-date').exists()).toBe(false)
    expect((wrapper.find('#task-completion-mode').element as HTMLSelectElement).value).toBe('count')
    await wrapper.find('#task-target').setValue('3')
    await wrapper.find('#task-start').setValue('2026-10-27')
    await wrapper.find('#task-end').setValue('2026-10-29')
    await fillDraft(wrapper, '范围', '1')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(createTask).toHaveBeenCalledWith(expect.objectContaining({ schedule_type: 'custom', completion_mode: 'count',
      target_count: 3, start_date: '2026-10-27', end_date: '2026-10-29' }), expect.any(String))
    expect(vi.mocked(createTask).mock.calls[0][0]).not.toHaveProperty('deadline')
  })

  it.each(['0', '1.5', '2147483648'])('rejects invalid counting target %s before publication', async target => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('#task-completion-mode').setValue('count')
    await wrapper.find('#task-target').setValue(target)
    await fillDraft(wrapper, '计数', '2')
    await wrapper.find('form').trigger('submit')
    expect(wrapper.find('form [role="alert"]').exists()).toBe(true)
    expect(createTask).not.toHaveBeenCalled()
  })

  it('publishes counting tasks and uses +1 rather than the single completion endpoint', async () => {
    const counted = task('count', { completion_mode: 'count', target_count: 2, progress_count: 0 })
    vi.mocked(listTasks).mockImplementation(async view => ({ tasks: view === 'open' ? [counted] : [], next_cursor: null }))
    vi.mocked(progressTask).mockImplementation(async () => {
      counted.progress_count = 1
      return { task_id: counted.id, event_id: 'event', progress_count: 1, target_count: 2,
        recorded_at: '2026-10-14T01:00:00.000000Z', completed: false, ignored: false, settlement: null }
    })
    vi.mocked(createTask).mockResolvedValue({ notification: NOT_REQUESTED, task: counted })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="complete-count"]').exists()).toBe(false)
    await wrapper.find('[data-test="progress-count"]').trigger('click')
    await flushPromises()
    expect(progressTask).toHaveBeenCalledTimes(1)
    expect(completeTask).not.toHaveBeenCalled()
    expect(getWallet).not.toHaveBeenCalled()
    await wrapper.find('#task-completion-mode').setValue('count')
    await wrapper.find('#task-target').setValue('2')
    await fillDraft(wrapper, '计数', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(createTask).toHaveBeenCalledWith(expect.objectContaining({ completion_mode: 'count', target_count: 2 }), expect.any(String))
  })

  it('lists both views, loads more with the returned cursor, and offers completion only to the assignee', async () => {
    vi.mocked(listTasks).mockImplementation(async (view, cursor) => {
      if (view === 'history') return { tasks: [task('h1', { status: 'expired' })], next_cursor: null }
      if (cursor === 'c1') return { tasks: [task('o2', { creator_id: 'self', assignee_id: 'partner' })], next_cursor: null }
      return { tasks: [task('o1')], next_cursor: 'c1' }
    })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const open = wrapper.find('[data-test="open-view"]')
    expect(open.text()).toContain('任务 o1')
    expect(open.text()).toContain('待我完成（1）')
    expect(wrapper.find('[data-test="history-view"]').text()).toContain('已过期')
    await wrapper.find('[data-test="open-more"]').trigger('click')
    await flushPromises()
    expect(listTasks).toHaveBeenCalledWith('open', 'c1')
    expect(open.text()).toContain('我发布的（1）')
    expect(wrapper.find('[data-test="open-more"]').exists()).toBe(false)
    // 完成控件只出现在“进行中且接收者是我”的任务上；我发布的任务没有。
    expect(wrapper.find('[data-test="complete-o1"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="complete-o2"]').exists()).toBe(false)
  })

  it('an unknown completion keeps its same-key retry control after a refresh removes the task', async () => {
    let openTasks: Task[] = [task('o1')]
    vi.mocked(listTasks).mockImplementation(async (view) =>
      view === 'open' ? { tasks: openTasks, next_cursor: null } : { tasks: [], next_cursor: null })
    vi.mocked(completeTask)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce({
        task: task('o1', { status: 'completed', completed_at: '2026-10-14T01:00:00.000000Z' }),
        settlement: { operation_id: 'op-1', flower: 2, sunshine: 0 },
      })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="complete-o1"]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="completion-retry-o1"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="complete-o1"]').exists()).toBe(false)

    openTasks = []
    const refresh = wrapper.find('[data-test="open-view"]').findAll('button').find((b) => b.text() === '刷新')
    await refresh!.trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="open-view"]').text()).not.toContain('任务 o1')
    expect(wrapper.find('[data-test="completion-retry-o1"]').exists()).toBe(true)

    await wrapper.find('[data-test="completion-retry-o1"]').trigger('click')
    await flushPromises()
    const calls = vi.mocked(completeTask).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1]).toEqual(calls[0])
    expect(wrapper.find('[data-test="completion-retry-o1"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="completion-notices"]').exists()).toBe(true)
  })

  it('a confirmed completion moves the task from the open section to the history section', async () => {
    let completed = false
    vi.mocked(listTasks).mockImplementation(async (view) => {
      if (view === 'open') return { tasks: completed ? [] : [task('o1')], next_cursor: null }
      return {
        tasks: completed ? [task('o1', { status: 'completed', completed_at: '2026-10-14T01:00:00.000000Z' })] : [],
        next_cursor: null,
      }
    })
    vi.mocked(completeTask).mockImplementationOnce(async () => {
      completed = true
      return {
        task: task('o1', { status: 'completed', completed_at: '2026-10-14T01:00:00.000000Z' }),
        settlement: { operation_id: 'op-1', flower: 2, sunshine: 0 },
      }
    })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="open-view"]').text()).toContain('任务 o1')
    await wrapper.find('[data-test="complete-o1"]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="open-view"]').text()).not.toContain('任务 o1')
    expect(wrapper.find('[data-test="history-view"]').text()).toContain('任务 o1')
    expect(wrapper.find('[data-test="completion-notices"]').exists()).toBe(true)
    expect(getWallet).toHaveBeenCalledTimes(1)
  })

  it('shows local validation errors without sending a request', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('form').trigger('submit')
    expect(wrapper.text()).toContain('标题不能为空')
    await wrapper.find('#task-title').setValue('浇花')
    await wrapper.find('form').trigger('submit')
    const rewardAlert = wrapper.find('form [role="alert"]')
    expect(rewardAlert.exists()).toBe(true)
    expect(rewardAlert.text()).toContain('至少需要一项大于 0 的奖励')
    await wrapper.find('#task-flowers').setValue('2')
    await wrapper.find('#task-time').setValue('21:30')
    await wrapper.find('form').trigger('submit')
    expect(wrapper.text()).toContain('请先选择截止日期')
    expect(createTask).not.toHaveBeenCalled()
  })

  it('an unconfirmed publish offers an explicit retry that reuses the same key', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce({ notification: NOT_REQUESTED, task: task('new', { creator_id: 'self', assignee_id: 'partner' }) })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('#task-title').setValue('浇花')
    await wrapper.find('#task-flowers').setValue('2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(wrapper.text()).toContain('不会重复发布')
    expect(createTask).toHaveBeenCalledTimes(1)
    await wrapper.find('[data-test="publish-retry"]').trigger('click')
    await flushPromises()
    expect(createTask).toHaveBeenCalledTimes(2)
    const calls = vi.mocked(createTask).mock.calls
    expect(calls[1][1]).toBe(calls[0][1])
    expect(calls[0][0]).toEqual({ title: '浇花', description: '', reward_flowers: 2, reward_sunshine: 0,
      notify_partner: false })
    expect(wrapper.text()).toContain('已确认发布')
  })

  it('editing after RESULT_UNKNOWN preserves the new draft and retries the original publication', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce({ notification: NOT_REQUESTED, task: task('edited', { creator_id: 'self', assignee_id: 'partner' }) })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await fillDraft(wrapper, '浇花', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(true)

    await wrapper.find('#task-title').setValue('浇花和喂猫')
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="draft-changed"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="publish-submit"]').exists()).toBe(false)
    await wrapper.find('[data-test="publish-retry"]').trigger('click')
    await flushPromises()

    const calls = vi.mocked(createTask).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1][0]).toEqual(calls[0][0])
    expect(calls[1][1]).toBe(calls[0][1])
    expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('浇花和喂猫')
    expect(wrapper.text()).toContain('已确认发布')
  })

  it('reverting to the original content restores the retry, which sends the original payload and key', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockResolvedValueOnce({ notification: NOT_REQUESTED, task: task('orig', { creator_id: 'self', assignee_id: 'partner' }) })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await fillDraft(wrapper, '浇花', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    await wrapper.find('#task-title').setValue('别的内容')
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(true)
    await wrapper.find('#task-title').setValue('  浇花 ')  // 规范化后与原发布相同
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(true)
    await wrapper.find('[data-test="publish-retry"]').trigger('click')
    await flushPromises()
    const calls = vi.mocked(createTask).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1][0]).toEqual(calls[0][0])
    expect(calls[1][1]).toBe(calls[0][1])
    expect(wrapper.text()).toContain('已确认发布')
  })

  it.each(['success', 'failure'])(
    'switching from account A to B clears the draft; a late %s for A keeps B draft and shows no notice',
    async (kind) => {
      vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
      const reply = deferred<TaskResponse>()
      vi.mocked(createTask).mockReturnValueOnce(reply.promise)
      wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
      await flushPromises()
      await fillDraft(wrapper, 'A 的任务', '3')
      await wrapper.find('form').trigger('submit')
      await flushPromises()
      expect(createTask).toHaveBeenCalledTimes(1)

      useSessionStore().applyBootstrap(sessionFor('user-b'))
      await flushPromises()
      expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('')
      expect((wrapper.find('#task-flowers').element as HTMLInputElement).value).toBe('')
      await wrapper.find('#task-title').setValue('B 的草稿')

      if (kind === 'success') reply.resolve({ task: task('late', { creator_id: 'self', assignee_id: 'partner' }) })
      else reply.reject(new ApiError('RESULT_UNKNOWN', 'late', 503))
      await flushPromises()
      expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('B 的草稿')
      expect(wrapper.text()).not.toContain('已发布')
      expect(wrapper.text()).not.toContain('未确认')
      expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(false)
    },
  )

  it('becoming anonymous clears fields, local validation errors and notices', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('#task-title').setValue('草稿')
    await wrapper.find('form').trigger('submit')
    const alert = wrapper.find('form [role="alert"]')
    expect(alert.exists()).toBe(true)
    expect(alert.text()).toContain('至少需要一项大于 0 的奖励')
    expect(createTask).not.toHaveBeenCalled()
    useSessionStore().resetBusinessState()
    await flushPromises()
    expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('')
    expect(wrapper.find('form [role="alert"]').exists()).toBe(false)
  })

  it('a publish that settles after unmount does not touch a newly mounted view', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    const reply = deferred<TaskResponse>()
    vi.mocked(createTask).mockReturnValueOnce(reply.promise)
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await fillDraft(wrapper, '浇花', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    wrapper.unmount()
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    reply.resolve({ task: task('late', { creator_id: 'self', assignee_id: 'partner' }) })
    await flushPromises()
    await wrapper.find('#task-title').setValue('新页面的草稿')
    expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('新页面的草稿')
    expect(wrapper.text()).not.toContain('已发布，对方')
  })
})

async function fillDraft(view: VueWrapper, title: string, flowers: string): Promise<void> {
  await view.find('#task-title').setValue(title)
  await view.find('#task-flowers').setValue(flowers)
}

function sessionFor(id: string): SessionBootstrap {
  return {
    authenticated: true,
    csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-09T00:00:00Z' },
    space: { id: 'space-b', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-09T00:00:00Z',
      members: [] },
  }
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
