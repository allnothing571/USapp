/**
 * 奖励商店商品（阶段 1 M7，契约 0.11.0）：上架、列表、详情、修改与下架。
 *
 * 全部经共享的 `request()`（同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里）。
 * 写请求必须带 `Idempotency-Key`；键由 rewards store 为每次动作生成，只在显式重试同一次动作时复用。
 * M7 不实现兑换，也不读写余额。
 */
import { request } from './client'
import type {
  RedemptionListResponse,
  RedemptionMutationResponse,
  RewardProductListResponse,
  RewardProductMutationResponse,
  RewardProductTerms,
} from './types'

/** `POST /api/v1/rewards/products`：201 新上架，200 同键重放。 */
export function createRewardProduct(body: RewardProductTerms, idempotencyKey: string): Promise<RewardProductMutationResponse> {
  return request<RewardProductMutationResponse>('/api/v1/rewards/products', {
    method: 'POST',
    body,
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/**
 * `GET /api/v1/rewards/products?cursor=&limit=`：调用者可见的奖励（对方只看到上架中的），
 * 游标原样回传；分页不是快照，两次请求之间的修改或下架可能改变可见内容。
 */
export function listRewardProducts(cursor?: string | null, limit?: number): Promise<RewardProductListResponse> {
  return request<RewardProductListResponse>('/api/v1/rewards/products', {
    method: 'GET',
    query: { cursor: cursor ?? undefined, limit },
  })
}

/** `POST /api/v1/rewards/products/{id}/revisions`：完整替换，expected_version 过期返回 409。 */
export function editRewardProduct(productId: string, body: RewardProductTerms & { expected_version: number },
  idempotencyKey: string): Promise<RewardProductMutationResponse> {
  return request<RewardProductMutationResponse>(
    `/api/v1/rewards/products/${encodeURIComponent(productId)}/revisions`, {
      method: 'POST',
      body,
      headers: { 'Idempotency-Key': idempotencyKey },
    })
}

/**
 * M8 `POST /api/v1/rewards/products/{id}/redemptions`：拥有者按确认时显示的版本直接兑换；
 * 201 新兑换，200 同键重放；响应不含余额。
 */
export function redeemRewardProduct(productId: string, expectedVersion: number,
  idempotencyKey: string): Promise<RedemptionMutationResponse> {
  return request<RedemptionMutationResponse>(
    `/api/v1/rewards/products/${encodeURIComponent(productId)}/redemptions`, {
      method: 'POST',
      body: { expected_version: expectedVersion },
      headers: { 'Idempotency-Key': idempotencyKey },
    })
}

/** M8 `GET /api/v1/rewards/redemptions?cursor=&limit=`：作为兑换方或上架方的兑换记录。 */
export function listRedemptions(cursor?: string | null, limit?: number): Promise<RedemptionListResponse> {
  return request<RedemptionListResponse>('/api/v1/rewards/redemptions', {
    method: 'GET',
    query: { cursor: cursor ?? undefined, limit },
  })
}

/** M8 `POST /api/v1/rewards/redemptions/{id}/fulfillment`：上架的一方确认履约（请求体为空）。 */
export function fulfillRedemption(redemptionId: string, idempotencyKey: string): Promise<RedemptionMutationResponse> {
  return request<RedemptionMutationResponse>(
    `/api/v1/rewards/redemptions/${encodeURIComponent(redemptionId)}/fulfillment`, {
      method: 'POST',
      headers: { 'Idempotency-Key': idempotencyKey },
    })
}

/** `POST /api/v1/rewards/products/{id}/archive`：单向下架。 */
export function archiveRewardProduct(productId: string, expectedVersion: number,
  idempotencyKey: string): Promise<RewardProductMutationResponse> {
  return request<RewardProductMutationResponse>(
    `/api/v1/rewards/products/${encodeURIComponent(productId)}/archive`, {
      method: 'POST',
      body: { expected_version: expectedVersion },
      headers: { 'Idempotency-Key': idempotencyKey },
    })
}

/** M9: proposal identity is captured with the user's confirmation, never re-read. */
export function transitionRedemption(id: string, action: import('./types').RedemptionTransition,
  proposalId: string | undefined, key: string): Promise<RedemptionMutationResponse> {
  return request<RedemptionMutationResponse>(`/api/v1/rewards/redemptions/${encodeURIComponent(id)}/transitions`, {
    method: 'POST', headers: { 'Idempotency-Key': key },
    body: { action, ...(proposalId ? { proposal_id: proposalId } : {}) },
  })
}
