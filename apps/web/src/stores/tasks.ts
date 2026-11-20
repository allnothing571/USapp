/**
 * 任务 store（发布、读取、单次完成与 M4 计数）：状态仅保存在内存中，不进行浏览器持久化。
 *
 * 两层失效（刻意分开）：
 * - 会话代数 `sessionGeneration`：reset()（注销、切换账号、引导失败）时递增。
 *   任何在途请求（列表、详情、发布的成功**和**失败）落地时，只有代数未变、
 *   且当前会话用户仍是发起者时才生效，否则静默丢弃：不改列表、不显示错误。
 * - 每个视图自己的请求序号：只用于“较新的列表请求胜出”，与会话失效无关。
 *
 * 发布的幂等键：一次发布动作一个随机键。网络错误、5xx（含 503 RESULT_UNKNOWN）
 * 或离线拦截之后结果未确认（或根本没发出），保留同一个键；用户重试同一份内容时复用它，
 * 服务端会把已提交的原发布作为重放返回。结果未知时须先确认原发布，再发布修改后的内容；
 * reset() 丢弃所有键。从不自动重放写请求。
 *
 * 完成（M3-C2，D40）：每个任务至多一个“完成动作”，保存任务快照与幂等键，与列表成员无关
 * （列表刷新从不创建、修改或删除动作）。网络错误、5xx（含 RESULT_UNKNOWN）保留键，
 * 由用户显式重试；离线拦截或本地缺少令牌时请求根本没发出，同样保留键等待显式重试。
 * 结果未知后，后续错误继续保留原键。确认成功（含同键重放）时：记录成功提示、
 * 立即从进行中移除该任务、从第一页重新加载进行中与历史（使旧的分页响应失效）、
 * 调用一次钱包 load()；结算金额只作提示，从不写入钱包余额；刷新失败不改变已确认的完成。
 */
import { defineStore } from 'pinia'
import { computed, reactive, ref, shallowRef } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { isValidNotificationFacts } from '@/api/notifications'
import { describeError, errorCodeOf } from '@/api/error-messages'
import { isRecurring, periodLabel } from '@/api/task-rules'
import {
  completeTask, createTask, decideTaskChange, getTask, listTasks, newIdempotencyKey, progressTask, proposeTaskChange,
} from '@/api/tasks'
import type {
  CompleteTaskResponse, CreateTaskRequest, ProgressTaskResponse, ProposeTaskChangeRequest, Task, TaskChangeAction,
  TaskChangeResponse, TaskSettlement, TaskView,
} from '@/api/types'
import { useNotificationChoicesStore } from './notification-choices'
import { useSessionStore } from './session'
import { useWalletStore } from './wallet'

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

/**
 * 完成动作：submitting 在途；unknown 结果未确认（已发出）；not_sent 被离线或本地令牌拦下（未发出）。
 * 后两者都保留键，只在用户点击“重试完成”时用同一个键重发。
 */
export type CompletionStatus = 'submitting' | 'unknown' | 'not_sent'

export interface CompletionAction {
  periodDate?: string
  /** Snapshot for display only; the period identity is periodDate. */
  periodLabel?: string
  kind: 'complete' | 'progress'
  taskId: string
  key: string
  title: string
  rewardFlowers: number
  rewardSunshine: number
  status: CompletionStatus
  errorText: string
  unresolved?: boolean
}

/** 完成结果提示：与列表、钱包的刷新状态分开保存。 */
export interface CompletionNotice {
  periodDate?: string
  periodLabel?: string
  taskId: string
  title: string
  kind: 'completed' | 'rejected' | 'progressed' | 'ignored'
  flower: number
  sunshine: number
  text: string
}

export type CompleteResult =
  | { outcome: 'completed' }
  | { outcome: 'progressed' }
  | { outcome: 'ignored' }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

/**
 * M6 change action (propose an edit/cancellation, or approve/reject/withdraw one), at most one
 * per root task, kept apart from list membership like completion actions. Unknown or unsent
 * results keep the key; only an explicit user retry resends it. A definitive answer ends it.
 */
export interface ChangeAction {
  taskId: string
  title: string
  kind: 'propose' | 'decide'
  proposal?: ProposeTaskChangeRequest
  proposalId?: string
  action?: TaskChangeAction
  key: string
  status: CompletionStatus
  errorText: string
  unresolved?: boolean
}

export interface ChangeNotice {
  taskId: string
  title: string
  ok: boolean
  text: string
}

export type ChangeResult =
  | { outcome: 'done'; response: TaskChangeResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

function isUsableChange(response: unknown, taskId: string): response is TaskChangeResponse {
  if (!response || typeof response !== 'object') return false
  const r = response as Partial<TaskChangeResponse>
  return typeof r.change?.id === 'string' && r.change.id.length > 0 && r.change.task_id === taskId &&
    r.task?.id === taskId
}

function changeDoneText(action: ChangeAction, response: TaskChangeResponse): string {
  if (action.kind === 'propose') {
    return action.proposal?.kind === 'cancel' ? '已提出取消，等待对方确认。' : '已提出修改，等待对方确认。'
  }
  if (action.action === 'withdraw') return '已撤回这项变更。'
  if (action.action === 'reject') return '已拒绝这项变更，原任务不变。'
  return response.change.kind === 'cancel' ? '已同意取消，任务已关闭（已有进度保留，不发放奖励）。'
    : '已同意修改，新任务已生效。'
}

interface PendingPublish {
  key: string
  body: string
  payload: CreateTaskRequest
  unresolved?: boolean
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

/** 服务端给出了明确答复（4xx）：这次发布动作结束。 */
function isDefinitiveRejection(error: unknown): boolean {
  return error instanceof ApiError && error.status >= 400 && error.status < 500
}

/** 在丢弃重试键之前，确认响应中用于完成提示的事实可用。 */
function isUsableSettlement(value: unknown): value is TaskSettlement {
  if (!value || typeof value !== 'object') return false
  const s = value as TaskSettlement
  return typeof s.operation_id === 'string' && s.operation_id.length > 0 &&
    Number.isInteger(s.flower) && s.flower >= 0 && s.flower <= 999 &&
    Number.isInteger(s.sunshine) && s.sunshine >= 0 && s.sunshine <= 100000 &&
    (s.flower > 0 || s.sunshine > 0)
}

function isUsableCompletion(response: unknown, taskId: string): response is CompleteTaskResponse {
  if (!response || typeof response !== 'object') return false
  const { task, settlement } = response as Partial<CompleteTaskResponse>
  return task?.id === taskId && task.status === 'completed' &&
    typeof task.completed_at === 'string' && task.completed_at.length > 0 &&
    isUsableSettlement(settlement)
}

function isUsableProgress(response: unknown, taskId: string, periodDate?: string): response is ProgressTaskResponse {
  if (!response || typeof response !== 'object') return false
  const r = response as ProgressTaskResponse
  if (r.period_date !== periodDate) return false
  if (r.task_id !== taskId || !Number.isInteger(r.target_count) || r.target_count < 1 || r.target_count > 2147483647 ||
      !Number.isInteger(r.progress_count) || r.progress_count < 1 || r.progress_count > r.target_count ||
      typeof r.completed !== 'boolean' || r.completed !== (r.progress_count === r.target_count) ||
      typeof r.ignored !== 'boolean') return false
  if (r.ignored ? (!r.completed || r.event_id !== null || r.recorded_at !== null) :
      (typeof r.event_id !== 'string' || !r.event_id || typeof r.recorded_at !== 'string' || !r.recorded_at)) return false
  if (!r.completed) return r.settlement === null
  return isUsableSettlement(r.settlement)
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
  // 完成动作（按任务 id）与完成提示，都与列表成员无关。
  const completionActions = ref<Record<string, CompletionAction>>({})
  const completionNotices = ref<CompletionNotice[]>([])
  // M6 change actions and their notices (per root task), independent of list membership.
  const changeActions = ref<Record<string, ChangeAction>>({})
  const changeNotices = ref<ChangeNotice[]>([])

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
    completionActions.value = {}
    completionNotices.value = []
    changeActions.value = {}
    changeNotices.value = []
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
    const rowKey = (task: Task) => `${task.id}:${task.period_date ?? ''}`
    const seen = new Set(state.items.map(rowKey))
    for (const task of tasks) {
      if (!seen.has(rowKey(task))) {
        seen.add(rowKey(task))
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

  async function openDetail(taskId: string, periodDate?: string): Promise<void> {
    const seq = ++detailSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    detail.value = null
    detailStatus.value = 'loading'
    detailErrorText.value = ''
    try {
      const response = periodDate ? await getTask(taskId, periodDate) : await getTask(taskId)
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
   * 发布。未知结果必须先通过同键重试确认；未发送的草稿可更换内容。
   * 不自动重试；结果未确认时由用户决定是否重试。
   */
  async function publish(payload: CreateTaskRequest): Promise<PublishResult> {
    if (publishStatus.value === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }

    const body = JSON.stringify(payload)
    if (pending.value?.unresolved && pending.value.body !== body) {
      publishStatus.value = 'unknown'
      publishErrorText.value = '上一次发布结果仍未确认，请先重试原请求；修改后的草稿会保留。'
      return { outcome: 'unknown' }
    }
    if (!pending.value || pending.value.body !== body) {
      pending.value = { key: newIdempotencyKey(), body, payload: JSON.parse(body) as CreateTaskRequest }
    }
    const key = pending.value.key
    publishStatus.value = 'submitting'
    publishErrorCode.value = null
    publishErrorText.value = ''

    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      publishErrorCode.value = errorCodeOf(error)
      if (pending.value?.unresolved) {
        publishStatus.value = 'unknown'
        publishErrorText.value = `本次重试未发出（${describeError(error)}）。之前的发布结果仍未确认，请重试原请求。`
        return { outcome: 'unknown' }
      }
      publishStatus.value = 'error'
      publishErrorText.value = describeError(error)
      return { outcome: 'not_sent' }
    }
    try {
      const response = await createTask(pending.value.payload, key)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!response || typeof response.task?.id !== 'string' || !response.task.id ||
        !isValidNotificationFacts(response.notification, pending.value?.payload.notify_partner)) {
        throw new Error('发布响应无法确认。')
      }
      // Remembered defaults for FUTURE forms only.
      if (pending.value?.payload.notify_partner !== undefined) void useNotificationChoicesStore().refresh()
      if (pending.value?.key === key) pending.value = null
      publishStatus.value = 'idle'
      void loadFirst('open')
      return { outcome: 'published', task: response.task }
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      publishErrorCode.value = errorCodeOf(error)
      if (pending.value?.unresolved) {
        publishStatus.value = 'unknown'
        publishErrorText.value = `本次重试未确认原结果（${describeError(error)}）。之前的发布结果仍未确认，请重试原请求。`
        return { outcome: 'unknown' }
      }
      if (error instanceof OfflineMutationBlockedError) {
        publishStatus.value = 'error'
        publishErrorText.value = describeError(error)
        return { outcome: 'not_sent' }
      }
      if (isDefinitiveRejection(error)) {
        publishStatus.value = 'error'
        if (pending.value?.key === key) pending.value = null
        publishErrorText.value =
          error instanceof ApiError && error.code === 'VALIDATION_ERROR' && error.message
            ? error.message
            : describeError(error)
        return { outcome: 'rejected', errorCode: errorCodeOf(error) }
      }
      // 已调用写请求：响应读取错误和不完整的 2xx 也不能证明没有提交。
      if (pending.value?.key === key) pending.value = { ...pending.value, unresolved: true }
      publishStatus.value = 'unknown'
      publishErrorText.value = '发布结果未确认。可以重试，重试不会重复发布。'
      return { outcome: 'unknown' }
    }
  }

  /**
   * 用同一个键重发上一次未确认的发布（由用户点击触发），发送的是**原**请求体。
   * 表单已修改时仍可重试原请求，新的草稿由视图保留。
   */
  function retryPublish(): Promise<PublishResult> {
    if (!pending.value) return Promise.resolve({ outcome: 'discarded' })
    return publish(pending.value.payload)
  }

  function hasPendingPublish(): boolean {
    return pending.value !== null
  }

  function setNotice(notice: CompletionNotice): void {
    completionNotices.value = [notice, ...completionNotices.value.filter((n) => n.taskId !== notice.taskId)]
  }

  function dismissNotice(taskId: string): void {
    completionNotices.value = completionNotices.value.filter((n) => n.taskId !== taskId)
  }

  function updateAction(taskId: string, patch: Partial<CompletionAction>): void {
    const current = completionActions.value[taskId]
    if (current) completionActions.value = { ...completionActions.value, [taskId]: { ...current, ...patch } }
  }

  function endAction(taskId: string): void {
    const next = { ...completionActions.value }
    delete next[taskId]
    completionActions.value = next
  }

  async function sendCompletion(taskId: string): Promise<CompleteResult> {
    const action = completionActions.value[taskId]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }
    const key = action.key
    updateAction(taskId, { status: 'submitting', errorText: '' })
    // 预检失败时，完成请求尚未调用；不能把本地令牌错误当成服务端的明确拒绝。
    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，完成请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.unresolved) {
        updateAction(taskId, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请重试原请求。` })
        return { outcome: 'unknown' }
      }
      updateAction(taskId, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }

    let settlement: TaskSettlement | null
    let completed = false
    let ignored = false
    try {
      const received = action.kind === 'progress' ?
        (action.periodDate ? await progressTask(taskId, key, action.periodDate) : await progressTask(taskId, key)) :
        await completeTask(taskId, key)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.kind === 'progress') {
        if (!isUsableProgress(received, taskId, action.periodDate)) throw new Error('进度响应无法确认。')
        settlement = received.settlement
        completed = received.completed
        ignored = received.ignored
      } else {
        if (!isUsableCompletion(received, taskId)) throw new Error('完成响应无法确认。')
        settlement = received.settlement
        completed = true
      }
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) {
          updateAction(taskId, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请重试原请求。` })
          return { outcome: 'unknown' }
        }
        updateAction(taskId, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (isDefinitiveRejection(error)) {
        if (action.unresolved) {
          updateAction(taskId, { status: 'unknown', errorText: `本次重试未确认原结果（${describeError(error)}）。请保留原请求重试，或先核对任务记录。` })
          return { outcome: 'unknown' }
        }
        endAction(taskId)
        const code = errorCodeOf(error)
        setNotice({ taskId, title: action.title, periodDate: action.periodDate, periodLabel: action.periodLabel,
          kind: 'rejected', flower: 0, sunshine: 0, text: describeError(error) })
        if (code === 'TASK_ALREADY_COMPLETED' || code === 'TASK_EXPIRED' || code === 'TASK_NOT_STARTED' ||
            code === 'TASK_CANCELLED' || code === 'TASK_SUPERSEDED') {
          void loadFirst('open')
          void loadFirst('history')
        }
        return { outcome: 'rejected', errorCode: code }
      }
      // 请求可能已到达服务端，包括读取响应体失败或成功响应不可用；保留原键确认结果。
      updateAction(taskId, { status: 'unknown', unresolved: true, errorText: '操作结果未确认。可以重试，重试不会重复计数或入账。' })
      return { outcome: 'unknown' }
    }

    // 已确认的完成与随后的刷新分开处理，刷新错误不能改写完成结果。
    endAction(taskId)
    setNotice({
      taskId,
      title: action.title,
      periodDate: action.periodDate,
      periodLabel: action.periodLabel,
      kind: ignored ? 'ignored' : completed ? 'completed' : 'progressed',
      flower: ignored ? 0 : settlement?.flower ?? 0,
      sunshine: ignored ? 0 : settlement?.sunshine ?? 0,
      text: ignored ? '已达目标，本次未增加次数。' : completed ? '' : '已确认记录一次 +1。',
    })
    if (completed) views.open.items = views.open.items.filter((task) =>
      task.id !== taskId || task.period_date !== action.periodDate)
    // Event replies are immutable snapshots. Always reload authoritative progress;
    // replaying an older event must never overwrite a newer task counter.
    void loadFirst('open')
    void loadFirst('history')
    if (action.kind === 'progress' && detail.value?.id === taskId) void openDetail(taskId, detail.value.period_date)
    if (settlement) void useWalletStore().load()
    return { outcome: ignored ? 'ignored' : completed ? 'completed' : 'progressed' }
  }

  /** 完成一项任务（只由按钮触发）。同一任务已有动作时不再提交。 */
  function complete(task: Task): Promise<CompleteResult> {
    return registerAction(task, 'complete')
  }

  function progress(task: Task): Promise<CompleteResult> {
    return registerAction(task, 'progress')
  }

  function registerAction(task: Task, kind: CompletionAction['kind']): Promise<CompleteResult> {
    if (completionActions.value[task.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    completionActions.value = {
      ...completionActions.value,
      [task.id]: {
        kind,
        periodDate: task.period_date,
        periodLabel: isRecurring(task) || task.schedule_type === 'custom' ? periodLabel(task) : undefined,
        taskId: task.id,
        key: newIdempotencyKey(),
        title: task.title,
        rewardFlowers: task.reward_flowers,
        rewardSunshine: task.reward_sunshine,
        // 同步登记动作，防止重复点击；sendCompletion 在第一个 await 之前就置为 submitting。
        status: 'not_sent',
        errorText: '',
      },
    }
    return sendCompletion(task.id)
  }

  /** 用同一个键重发未确认或未发出的完成动作（只由用户点击触发）。 */
  function retryCompletion(taskId: string): Promise<CompleteResult> {
    return sendCompletion(taskId)
  }

  function updateChange(taskId: string, patch: Partial<ChangeAction>): void {
    const current = changeActions.value[taskId]
    if (current) changeActions.value = { ...changeActions.value, [taskId]: { ...current, ...patch } }
  }

  function endChange(taskId: string): void {
    const next = { ...changeActions.value }
    delete next[taskId]
    changeActions.value = next
  }

  function setChangeNotice(notice: ChangeNotice): void {
    changeNotices.value = [notice, ...changeNotices.value.filter((n) => n.taskId !== notice.taskId)]
  }

  function dismissChangeNotice(taskId: string): void {
    changeNotices.value = changeNotices.value.filter((n) => n.taskId !== taskId)
  }

  function refreshAfterChange(taskId: string): void {
    void loadFirst('open')
    void loadFirst('history')
    if (detail.value?.id === taskId) void openDetail(taskId, detail.value.period_date)
  }

  async function sendChange(taskId: string): Promise<ChangeResult> {
    const action = changeActions.value[taskId]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }
    const key = action.key
    updateChange(taskId, { status: 'submitting', errorText: '' })
    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.unresolved) {
        updateChange(taskId, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请重试原请求。` })
        return { outcome: 'unknown' }
      }
      updateChange(taskId, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }
    let response: TaskChangeResponse
    try {
      const received = action.kind === 'propose' && action.proposal
        ? await proposeTaskChange(taskId, action.proposal, key)
        : await decideTaskChange(taskId, action.proposalId ?? '', action.action ?? 'reject', key)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableChange(received, taskId)) throw new Error('变更响应无法确认。')
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) {
          updateChange(taskId, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请重试原请求。` })
          return { outcome: 'unknown' }
        }
        updateChange(taskId, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (isDefinitiveRejection(error)) {
        if (action.unresolved) {
          updateChange(taskId, { status: 'unknown', errorText: `本次重试未确认原结果（${describeError(error)}）。请保留原请求重试。` })
          return { outcome: 'unknown' }
        }
        endChange(taskId)
        setChangeNotice({ taskId, title: action.title, ok: false, text: describeError(error) })
        refreshAfterChange(taskId)
        return { outcome: 'rejected', errorCode: errorCodeOf(error) }
      }
      updateChange(taskId, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复处理。' })
      return { outcome: 'unknown' }
    }
    endChange(taskId)
    setChangeNotice({ taskId, title: action.title, ok: true, text: changeDoneText(action, response) })
    refreshAfterChange(taskId)
    return { outcome: 'done', response }
  }

  function registerChange(task: Task, fields: Pick<ChangeAction, 'kind' | 'proposal' | 'proposalId' | 'action'>):
    Promise<ChangeResult> {
    if (changeActions.value[task.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    changeActions.value = {
      ...changeActions.value,
      [task.id]: { taskId: task.id, title: task.title, key: newIdempotencyKey(), status: 'not_sent', errorText: '', ...fields },
    }
    return sendChange(task.id)
  }

  /** M6: propose an edit (creator) or a cancellation (either member). */
  function proposeChange(task: Task, proposal: ProposeTaskChangeRequest): Promise<ChangeResult> {
    return registerChange(task, { kind: 'propose', proposal })
  }

  /** M6: approve / reject (the other member) or withdraw (the proposer) the task's pending change. */
  function decideChange(task: Task, action: TaskChangeAction): Promise<ChangeResult> {
    if (!task.pending_change) return Promise.resolve({ outcome: 'discarded' })
    return registerChange(task, { kind: 'decide', proposalId: task.pending_change.id, action })
  }

  /** Resend an unknown or unsent change action with its original key (user click only). */
  function retryChange(taskId: string): Promise<ChangeResult> {
    return sendChange(taskId)
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
    completionActions,
    completionNotices,
    complete,
    progress,
    retryCompletion,
    dismissNotice,
    changeActions,
    changeNotices,
    proposeChange,
    decideChange,
    retryChange,
    dismissChangeNotice,
  }
})
