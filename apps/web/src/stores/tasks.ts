/**
 * 任务 store（阶段 1 M3-B）：状态仅保存在内存中，不进行浏览器持久化。
 *
 * 两层失效（刻意分开）：
 * - 会话代数 `sessionGeneration`：reset()（注销、切换账号、引导失败）时递增。
 *   任何在途请求（列表、详情、发布的成功**和**失败）落地时，只有代数未变、
 *   且当前会话用户仍是发起者时才生效，否则静默丢弃：不改列表、不显示错误。
 * - 每个视图自己的请求序号：只用于“较新的列表请求胜出”，与会话失效无关。
 *
 * 发布的幂等键：一次发布动作一个随机键。网络错误、5xx（含 503 RESULT_UNKNOWN）
 * 或离线拦截之后结果未确认（或根本没发出），保留同一个键；用户重试同一份内容时复用它，
 * 服务端会把已提交的原发布作为重放返回，不会重复发布。收到明确答复（2xx 或 4xx）后
 * 丢弃该键；内容改变时换新键；reset() 丢弃所有键。从不自动重放写请求。
 */
import { defineStore } from 'pinia'
import { computed, reactive, ref, shallowRef } from 'vue'

import { ApiError } from '@/api/client'
import { describeError, errorCodeOf } from '@/api/error-messages'
import { createTask, getTask, listTasks, newIdempotencyKey } from '@/api/tasks'
import type { CreateTaskRequest, Task, TaskView } from '@/api/types'
import { useSessionStore } from './session'

export type ListStatus = 'idle' | 'loading' | 'ready' | 'error'

export interface TaskViewState {
  items: Task[]
  nextCursor: string | null
  status: ListStatus
  loadingMore: boolean
  errorCode: string | null
  errorText: string
  /** “加载更多”失败的提示；已加载的条目保留（它们来自同一次列表读取）。 */
  moreErrorText: string
}

export type PublishStatus = 'idle' | 'submitting' | 'unknown' | 'error'

export type PublishResult =
  | { outcome: 'published'; task: Task }
  | { outcome: 'unknown' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'not_sent' }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

interface PendingPublish {
  key: string
  body: string
  payload: CreateTaskRequest
}

function emptyView(): TaskViewState {
  return {
    items: [],
    nextCursor: null,
    status: 'idle',
    loadingMore: false,
    errorCode: null,
    errorText: '',
    moreErrorText: '',
  }
}

/** 结果未确认：没有收到响应（网络错误）或服务端 5xx。 */
function isUnconfirmed(error: unknown): boolean {
  if (!(error instanceof ApiError)) return false
  return (error.status === 0 && error.code === 'NETWORK_ERROR') || error.status >= 500
}

/** 服务端给出了明确答复（4xx）：这次发布动作结束。 */
function isDefinitiveRejection(error: unknown): boolean {
  return error instanceof ApiError && error.status >= 400 && error.status < 500
}

export const useTasksStore = defineStore('tasks', () => {
  const views = reactive<Record<TaskView, TaskViewState>>({ open: emptyView(), history: emptyView() })
  const publishStatus = ref<PublishStatus>('idle')
  const publishErrorCode = ref<string | null>(null)
  const publishErrorText = ref('')
  const detail = ref<Task | null>(null)
  const detailStatus = ref<ListStatus>('idle')
  const detailErrorText = ref('')

  let sessionGeneration = 0
  const viewSeq: Record<TaskView, number> = { open: 0, history: 0 }
  let detailSeq = 0
  // 未确认的发布动作（键 + 规范化请求体）。可观察，供视图判断表单是否仍是“同一次发布”。
  const pending = shallowRef<PendingPublish | null>(null)
  /** 未确认发布的规范化请求体（JSON 串）；没有时为 null。视图用它区分“重试原发布”与“发布新内容”。 */
  const pendingPublishBody = computed(() => pending.value?.body ?? null)

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    viewSeq.open += 1
    viewSeq.history += 1
    detailSeq += 1
    pending.value = null
    Object.assign(views.open, emptyView())
    Object.assign(views.history, emptyView())
    publishStatus.value = 'idle'
    publishErrorCode.value = null
    publishErrorText.value = ''
    detail.value = null
    detailStatus.value = 'idle'
    detailErrorText.value = ''
  }

  function appendUnique(state: TaskViewState, tasks: Task[]): void {
    const seen = new Set(state.items.map((task) => task.id))
    for (const task of tasks) {
      if (!seen.has(task.id)) {
        seen.add(task.id)
        state.items.push(task)
      }
    }
  }

  /** 从第一页重新加载（也是“刷新”）。旧的条目先清空：错误旁边不留旧数据。 */
  async function loadFirst(view: TaskView): Promise<void> {
    const state = views[view]
    const seq = ++viewSeq[view]
    const generation = sessionGeneration
    const owner = currentUserId()
    Object.assign(state, emptyView(), { status: 'loading' as ListStatus })
    try {
      const response = await listTasks(view)
      if (seq !== viewSeq[view] || !stillCurrent(generation, owner)) return
      state.items = []
      appendUnique(state, response.tasks)
      state.nextCursor = response.next_cursor
      state.status = 'ready'
    } catch (error) {
      if (seq !== viewSeq[view] || !stillCurrent(generation, owner)) return
      Object.assign(state, emptyView(), {
        status: 'error' as ListStatus,
        errorCode: errorCodeOf(error),
        errorText: describeError(error),
      })
    }
  }

  /** 追加下一页；只在 ready 且有游标时生效。较新的 loadFirst 会使它失效。 */
  async function loadMore(view: TaskView): Promise<void> {
    const state = views[view]
    if (state.status !== 'ready' || !state.nextCursor || state.loadingMore) return
    const seq = ++viewSeq[view]
    const generation = sessionGeneration
    const owner = currentUserId()
    const cursor = state.nextCursor
    state.loadingMore = true
    state.moreErrorText = ''
    try {
      const response = await listTasks(view, cursor)
      if (seq !== viewSeq[view] || !stillCurrent(generation, owner)) return
      appendUnique(state, response.tasks)
      state.nextCursor = response.next_cursor
      state.loadingMore = false
    } catch (error) {
      if (seq !== viewSeq[view] || !stillCurrent(generation, owner)) return
      state.loadingMore = false
      state.moreErrorText =
        errorCodeOf(error) === 'VALIDATION_ERROR' ? '列表已变化，请从第一页重新加载。' : describeError(error)
    }
  }

  async function openDetail(taskId: string): Promise<void> {
    const seq = ++detailSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    detail.value = null
    detailStatus.value = 'loading'
    detailErrorText.value = ''
    try {
      const response = await getTask(taskId)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = response.task
      detailStatus.value = 'ready'
    } catch (error) {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = null
      detailStatus.value = 'error'
      detailErrorText.value = describeError(error)
    }
  }

  function closeDetail(): void {
    detailSeq += 1
    detail.value = null
    detailStatus.value = 'idle'
    detailErrorText.value = ''
  }

  /**
   * 发布。内容与上一次未确认的发布相同则复用其键，否则换新键。
   * 不自动重试；结果未确认时由用户决定是否重试。
   */
  async function publish(payload: CreateTaskRequest): Promise<PublishResult> {
    if (publishStatus.value === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }

    const body = JSON.stringify(payload)
    if (!pending.value || pending.value.body !== body) {
      pending.value = { key: newIdempotencyKey(), body, payload }
    }
    const key = pending.value.key
    publishStatus.value = 'submitting'
    publishErrorCode.value = null
    publishErrorText.value = ''

    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      const response = await createTask(payload, key)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (pending.value?.key === key) pending.value = null
      publishStatus.value = 'idle'
      void loadFirst('open')
      return { outcome: 'published', task: response.task }
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      publishErrorCode.value = errorCodeOf(error)
      if (isUnconfirmed(error)) {
        // 保留键：重试同一份内容会得到原发布的重放，而不是第二个任务。
        publishStatus.value = 'unknown'
        publishErrorText.value = '发布结果未确认。可以重试，重试不会重复发布。'
        return { outcome: 'unknown' }
      }
      publishStatus.value = 'error'
      if (isDefinitiveRejection(error)) {
        if (pending.value?.key === key) pending.value = null
        publishErrorText.value =
          error instanceof ApiError && error.code === 'VALIDATION_ERROR' && error.message
            ? error.message
            : describeError(error)
        return { outcome: 'rejected', errorCode: errorCodeOf(error) }
      }
      // 离线拦截或本地缺少安全令牌：请求没有发出，保留键以便原样重试。
      publishErrorText.value = describeError(error)
      return { outcome: 'not_sent' }
    }
  }

  /**
   * 用同一个键重发上一次未确认的发布（由用户点击触发），发送的是**原**请求体。
   * 视图只在表单的规范化内容与它相同时才提供这个入口。
   */
  function retryPublish(): Promise<PublishResult> {
    if (!pending.value) return Promise.resolve({ outcome: 'discarded' })
    return publish(pending.value.payload)
  }

  function hasPendingPublish(): boolean {
    return pending.value !== null
  }

  return {
    views,
    publishStatus,
    publishErrorCode,
    publishErrorText,
    detail,
    detailStatus,
    detailErrorText,
    reset,
    loadFirst,
    loadMore,
    openDetail,
    closeDetail,
    publish,
    retryPublish,
    hasPendingPublish,
    pendingPublishBody,
  }
})
