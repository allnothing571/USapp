/**
 * 任务发布、读取、完成及按日/按周计数（阶段 1 M5-A/M5-B，契约 0.8.0）。
 *
 * 全部经共享的 `request()`：同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里，
 * 这里不另写一份。发布与完成都必须带 `Idempotency-Key`；键由调用方（tasks store）
 * 为每次动作生成，只在显式重试同一次动作时复用，只存在于内存。
 */
import { request } from './client'
import type {
  CompleteTaskResponse,
  CreateTaskRequest,
  ProgressTaskResponse,
  ProposeTaskChangeRequest,
  TaskChangeAction,
  TaskChangeResponse,
  TaskListResponse,
  TaskPublishResponse,
  TaskResponse,
  TaskView,
} from './types'

/** `POST /api/v1/tasks`。成功是 201（新任务）或 200（同键重放），两者都返回任务。 */
export function createTask(body: CreateTaskRequest, idempotencyKey: string): Promise<TaskPublishResponse> {
  return request<TaskPublishResponse>('/api/v1/tasks', {
    method: 'POST',
    body,
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `GET /api/v1/tasks?view=&cursor=&limit=`；游标原样回传。 */
export function listTasks(view: TaskView, cursor?: string | null, limit?: number): Promise<TaskListResponse> {
  return request<TaskListResponse>('/api/v1/tasks', {
    method: 'GET',
    query: { view, cursor: cursor ?? undefined, limit },
  })
}

/** `GET /api/v1/tasks/{taskId}` */
export function getTask(taskId: string, periodDate?: string): Promise<TaskResponse> {
  return request<TaskResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}`, {
    method: 'GET', query: { period_date: periodDate },
  })
}

/**
 * `POST /api/v1/tasks/{taskId}/completion`：请求体为空。首次完成与同键重放都返回 200，
 * 响应只含不可变的任务与结算事实，不含余额（余额另读 `GET /api/v1/wallet`）。
 */
export function completeTask(taskId: string, idempotencyKey: string): Promise<CompleteTaskResponse> {
  return request<CompleteTaskResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}/completion`, {
    method: 'POST',
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** One counting action; the same key replays its original event. */
export function progressTask(taskId: string, idempotencyKey: string, periodDate?: string): Promise<ProgressTaskResponse> {
  return request<ProgressTaskResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}/progress`, {
    method: 'POST',
    headers: { 'Idempotency-Key': idempotencyKey },
    body: periodDate ? { period_date: periodDate } : undefined,
  })
}

/** M6: propose an edit (creator) or a cancellation (either member); 201 new, 200 replay. */
export function proposeTaskChange(taskId: string, body: ProposeTaskChangeRequest,
  idempotencyKey: string): Promise<TaskChangeResponse> {
  return request<TaskChangeResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}/changes`, {
    method: 'POST',
    body,
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** M6: approve / reject (the other member) or withdraw (the proposer). */
export function decideTaskChange(taskId: string, proposalId: string, action: TaskChangeAction,
  idempotencyKey: string): Promise<TaskChangeResponse> {
  return request<TaskChangeResponse>(
    `/api/v1/tasks/${encodeURIComponent(taskId)}/changes/${encodeURIComponent(proposalId)}/decision`, {
      method: 'POST',
      body: { action },
      headers: { 'Idempotency-Key': idempotencyKey },
    })
}

/** 随机 UUID（v4），作为一次发布动作的 Idempotency-Key。不持久化。 */
export function newIdempotencyKey(): string {
  const source = globalThis.crypto
  if (source && typeof source.randomUUID === 'function') return source.randomUUID()
  if (source && typeof source.getRandomValues === 'function') {
    const bytes = new Uint8Array(16)
    source.getRandomValues(bytes)
    bytes[6] = (bytes[6] & 0x0f) | 0x40
    bytes[8] = (bytes[8] & 0x3f) | 0x80
    const hex = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
  }
  throw new Error('当前环境无法生成安全的随机请求标识。')
}

export function reactToTask(id: string, body: import('./types').TaskReactionInput, key: string): Promise<import('./types').TaskReactionResponse> {
  return request(`/api/v1/tasks/${encodeURIComponent(id)}/reaction`, { method: 'POST', body, headers: { 'Idempotency-Key': key } })
}
