/**
 * 一次性任务的发布与读取（阶段 1 M3-B，契约 0.4.0）。
 *
 * 全部经共享的 `request()`：同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里，
 * 这里不另写一份。发布必须带 `Idempotency-Key`；键由调用方（tasks store）
 * 为每次发布动作生成，只在重试同一次发布时复用，只存在于内存。
 * 本模块不提供完成接口（M3-C）。
 */
import { request } from './client'
import type { CreateTaskRequest, TaskListResponse, TaskResponse, TaskView } from './types'

/** `POST /api/v1/tasks`。成功是 201（新任务）或 200（同键重放），两者都返回任务。 */
export function createTask(body: CreateTaskRequest, idempotencyKey: string): Promise<TaskResponse> {
  return request<TaskResponse>('/api/v1/tasks', {
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
export function getTask(taskId: string): Promise<TaskResponse> {
  return request<TaskResponse>(`/api/v1/tasks/${encodeURIComponent(taskId)}`, { method: 'GET' })
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
