import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', () => ({
  createTask: vi.fn(),
  listTasks: vi.fn(),
  getTask: vi.fn(),
  newIdempotencyKey: vi.fn(),
}))

import { ApiError } from '@/api/client'
import { createTask, listTasks, newIdempotencyKey } from '@/api/tasks'
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

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.mocked(createTask).mockReset()
  vi.mocked(listTasks).mockReset()
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
})

describe('tasks view', () => {
  it('lists both views, loads more with the returned cursor, and offers no completion control', async () => {
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
    const buttons = wrapper.findAll('button').map((b) => b.text())
    expect(buttons.some((text) => text.includes('完成'))).toBe(false)
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
      .mockResolvedValueOnce({ task: task('new', { creator_id: 'self', assignee_id: 'partner' }) })
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
    expect(calls[0][0]).toEqual({ title: '浇花', description: '', reward_flowers: 2, reward_sunshine: 0 })
    expect(wrapper.text()).toContain('已确认发布')
  })

  it('editing after RESULT_UNKNOWN is a new publication with a new key, never a retry of the old payload', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce({ task: task('edited', { creator_id: 'self', assignee_id: 'partner' }) })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await fillDraft(wrapper, '浇花', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(true)

    await wrapper.find('#task-title').setValue('浇花和喂猫')
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="draft-changed"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="publish-submit"]').text()).toContain('新的发布')
    await wrapper.find('form').trigger('submit')
    await flushPromises()

    const calls = vi.mocked(createTask).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1][0]).toEqual({ title: '浇花和喂猫', description: '', reward_flowers: 2, reward_sunshine: 0 })
    expect(calls[1][1]).not.toBe(calls[0][1])
    expect((wrapper.find('#task-title').element as HTMLInputElement).value).toBe('')
    expect(wrapper.text()).toContain('已发布')
  })

  it('reverting to the original content restores the retry, which sends the original payload and key', async () => {
    vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockResolvedValueOnce({ task: task('orig', { creator_id: 'self', assignee_id: 'partner' }) })
    wrapper = mount(TasksView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await fillDraft(wrapper, '浇花', '2')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    await wrapper.find('#task-title').setValue('别的内容')
    expect(wrapper.find('[data-test="publish-retry"]').exists()).toBe(false)
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
