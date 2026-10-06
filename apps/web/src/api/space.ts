/**
 * 情侣空间与邀请端点封装。路径、方法、状态码与 contracts/openapi.yaml 严格一致。
 */
import { request } from './client'
import type { InviteListResponse, InviteResponse, Space, SpaceResponse } from './types'

export type { CreatedInvite, Invite, InviteListResponse, InviteResponse, Space, SpaceMember, SpaceResponse } from './types'

/**
 * `POST /api/v1/spaces` —— 创建情侣空间，调用者占 slot=1。
 *
 * 注意：契约中该端点**没有声明 requestBody**，所以这里不发送任何请求体。
 * 前端不发明契约里不存在的字段（例如自定义空间名）；如后续需要，先改契约再加参数。
 * 每个用户最多属于一个 couple，重复创建为 409 `SPACE_ALREADY_EXISTS`。
 */
export function createSpace(): Promise<SpaceResponse> {
  return request<SpaceResponse>('/api/v1/spaces', { method: 'POST' })
}

/** `GET /api/v1/spaces/current` —— 当前空间与成员；无空间时 404 `SPACE_NOT_FOUND`。 */
export function getCurrentSpace(): Promise<SpaceResponse> {
  return request<SpaceResponse>('/api/v1/spaces/current', { method: 'GET' })
}

/**
 * `GET /api/v1/spaces/{spaceId}` —— 按 id 查询空间。
 * 非本空间成员一律 404 `SPACE_NOT_FOUND`，前端不要据此区分"不存在"与"无权限"。
 */
export function getSpaceById(spaceId: string): Promise<SpaceResponse> {
  return request<SpaceResponse>(`/api/v1/spaces/${encodeURIComponent(spaceId)}`, { method: 'GET' })
}

/** `GET /api/v1/spaces/current/invites` —— 列出邀请元数据；契约保证不含邀请码明文。 */
export function listSpaceInvites(): Promise<InviteListResponse> {
  return request<InviteListResponse>('/api/v1/spaces/current/invites', { method: 'GET' })
}

/**
 * `POST /api/v1/spaces/current/invites` —— 生成邀请。
 * 邀请码明文只在此响应里出现一次（数据库只存 SHA-256）；空间已满时 409 `SPACE_FULL`。
 */
export function createSpaceInvite(): Promise<InviteResponse> {
  return request<InviteResponse>('/api/v1/spaces/current/invites', { method: 'POST' })
}

/** `DELETE /api/v1/spaces/current/invites/{inviteId}` —— 撤销邀请。返回 204。 */
export function revokeSpaceInvite(inviteId: string): Promise<void> {
  return request<void>(`/api/v1/spaces/current/invites/${encodeURIComponent(inviteId)}`, {
    method: 'DELETE',
  })
}

/**
 * `POST /api/v1/invites/{code}/accept` —— 接受邀请并加入空间。
 * 服务端在单个事务内校验邀请有效性、空间容量与调用者是否已在空间内。
 */
export function acceptInvite(code: string): Promise<SpaceResponse> {
  return request<SpaceResponse>(`/api/v1/invites/${encodeURIComponent(code)}/accept`, {
    method: 'POST',
  })
}

/** 邀请链接前缀：`{PUBLIC_ORIGIN}/invite/{code}`。无配置时退化为当前 origin。 */
export function inviteUrlFor(code: string, fallbackOrigin?: string): string {
  const origin =
    (import.meta.env.VITE_PUBLIC_ORIGIN ?? '').replace(/\/+$/, '') ||
    fallbackOrigin ||
    (typeof window !== 'undefined' ? window.location.origin : '')
  return `${origin}/invite/${encodeURIComponent(code)}`
}

/** 兜底工具：从任意 `Space | null` 里取出成员列表。 */
export function membersOf(space: Space | null | undefined): Space['members'] {
  return space?.members ?? []
}
