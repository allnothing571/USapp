/**
 * 纪念日与倒数日（阶段 2 P2-2，契约 0.15.0）：创建、列表、详情、创建者修改、对方修改申请与处理、
 * 首页展示与个人置顶。
 *
 * 全部经共享的 `request()`：同源 Cookie、写请求的 CSRF 令牌与离线拦截都在那里。
 * 写请求必须带 `Idempotency-Key`；键由 anniversaries store 为每次动作生成，只在显式重试同一次动作时复用。
 */
import { request } from './client'
import type {
  AnniversaryHomeResponse,
  AnniversaryListResponse,
  AnniversaryMutationResponse,
  AnniversaryPinResponse,
  AnniversaryResponse,
  AnniversaryTerms,
} from './types'

export type AnniversaryDecisionOutcome = 'approve' | 'reject' | 'withdraw'

const BASE = '/api/v1/anniversaries'

function idPath(id: string): string {
  return `${BASE}/${encodeURIComponent(id)}`
}

/** `POST /api/v1/anniversaries`：201 新建，200 同键重放。 */
export function createAnniversary(terms: AnniversaryTerms, idempotencyKey: string,
  notifyPartner?: boolean): Promise<AnniversaryMutationResponse> {
  return request<AnniversaryMutationResponse>(BASE, {
    method: 'POST',
    body: notifyPartner === undefined ? { ...terms } : { ...terms, notify_partner: notifyPartner },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `GET /api/v1/anniversaries?cursor=&limit=`：按创建顺序倒序；游标原样回传。 */
export function listAnniversaries(cursor?: string | null, limit?: number): Promise<AnniversaryListResponse> {
  return request<AnniversaryListResponse>(BASE, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

/** `GET /api/v1/anniversaries/{id}` */
export function getAnniversary(id: string): Promise<AnniversaryResponse> {
  return request<AnniversaryResponse>(idPath(id), { method: 'GET' })
}

/** `POST /api/v1/anniversaries/{id}/revisions`：创建者直接修改（完整内容 + expected_version）。 */
export function editAnniversary(id: string, expectedVersion: number, terms: AnniversaryTerms,
  idempotencyKey: string, notifyPartner?: boolean): Promise<AnniversaryMutationResponse> {
  return request<AnniversaryMutationResponse>(`${idPath(id)}/revisions`, {
    method: 'POST',
    body: notifyPartner === undefined ? { expected_version: expectedVersion, ...terms }
      : { expected_version: expectedVersion, ...terms, notify_partner: notifyPartner },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `POST /api/v1/anniversaries/{id}/proposals`：对方提交修改申请（完整内容 + expected_version）。 */
export function proposeAnniversaryEdit(id: string, expectedVersion: number, terms: AnniversaryTerms,
  idempotencyKey: string): Promise<AnniversaryMutationResponse> {
  return request<AnniversaryMutationResponse>(`${idPath(id)}/proposals`, {
    method: 'POST',
    body: { expected_version: expectedVersion, ...terms },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `POST /api/v1/anniversaries/{id}/proposals/{proposalId}/decision`：创建者同意 / 拒绝，申请人撤回。 */
export function decideAnniversaryProposal(id: string, proposalId: string, outcome: AnniversaryDecisionOutcome,
  idempotencyKey: string): Promise<AnniversaryMutationResponse> {
  return request<AnniversaryMutationResponse>(`${idPath(id)}/proposals/${encodeURIComponent(proposalId)}/decision`, {
    method: 'POST',
    body: { outcome },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `GET /api/v1/anniversaries/home`：首页卡片（置顶 / 自动 / 都已过去 / 没有）。 */
export function getHomeAnniversary(): Promise<AnniversaryHomeResponse> {
  return request<AnniversaryHomeResponse>(`${BASE}/home`, { method: 'GET' })
}

/** `POST /api/v1/anniversaries/home-pin`：只影响自己的首页；null 取消置顶。 */
export function setHomeAnniversaryPin(anniversaryId: string | null, idempotencyKey: string): Promise<AnniversaryPinResponse> {
  return request<AnniversaryPinResponse>(`${BASE}/home-pin`, {
    method: 'POST',
    body: { anniversary_id: anniversaryId },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}
