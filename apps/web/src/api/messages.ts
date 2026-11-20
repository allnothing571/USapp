/**
 * 双人留言板（阶段 2 P2-1，契约 0.14.0）：留言、历史、详情与单条回复。
 *
 * 全部经共享的 `request()`：同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里。
 * 写请求必须带 `Idempotency-Key`；键由 messages store 为每次动作生成，只在显式重试同一次动作时复用。
 */
import { request } from './client'
import type { MessageListResponse, MessageMutationResponse, MessageResponse } from './types'

/** `POST /api/v1/messages`：201 新留言，200 同键重放。 */
export function createMessage(body: string, idempotencyKey: string,
  notifyPartner?: boolean): Promise<MessageMutationResponse> {
  return request<MessageMutationResponse>('/api/v1/messages', {
    method: 'POST',
    body: notifyPartner === undefined ? { body } : { body, notify_partner: notifyPartner },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `GET /api/v1/messages?from=&cursor=&limit=`：`from=partner` 只列对方写的留言；游标原样回传。 */
export function listMessages(cursor?: string | null, limit?: number,
  from: 'all' | 'partner' = 'all'): Promise<MessageListResponse> {
  return request<MessageListResponse>('/api/v1/messages', {
    method: 'GET',
    query: { from: from === 'partner' ? 'partner' : undefined, cursor: cursor ?? undefined, limit },
  })
}

/** `GET /api/v1/messages/{id}` */
export function getMessage(messageId: string): Promise<MessageResponse> {
  return request<MessageResponse>(`/api/v1/messages/${encodeURIComponent(messageId)}`, { method: 'GET' })
}

/** `POST /api/v1/messages/{id}/reply`：收到留言的一方回复一句话；201 新回复，200 同键重放。 */
export function replyMessage(messageId: string, body: string, idempotencyKey: string,
  notifyPartner?: boolean): Promise<MessageMutationResponse> {
  return request<MessageMutationResponse>(`/api/v1/messages/${encodeURIComponent(messageId)}/reply`, {
    method: 'POST',
    body: notifyPartner === undefined ? { body } : { body, notify_partner: notifyPartner },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}
