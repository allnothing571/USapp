/**
 * 会话与登录端点封装。路径、方法、状态码与 contracts/openapi.yaml 严格一致。
 */
import { request } from './client'
import type { EmailCodeAccepted, MeResponse, SessionBootstrap } from './types'

export type { EmailCodeAccepted, MeResponse, SessionBootstrap }

/**
 * `GET /api/v1/session` —— 会话引导。
 *
 * 这是 CSRF 令牌的**唯一**获取入口，本身不需要 CSRF 令牌（契约明确说明）。
 * - 已有正式会话 → `authenticated: true` + user/space
 * - 否则建立/复用 30 分钟预认证会话 → `authenticated: false`
 * 两种情况都返回 `csrf_token`。
 */
export function bootstrapSession(signal?: AbortSignal): Promise<SessionBootstrap> {
  return request<SessionBootstrap>('/api/v1/session', { method: 'GET', signal })
}

/**
 * `POST /api/v1/auth/email-codes` —— 发送一次性邮箱验证码。
 *
 * 契约保证：无论邮箱是否在白名单、是否已注册，一律 202，避免账号枚举。
 * 因此前端**不能**从 202 推断该邮箱能否注册，白名单错误只会在验证码校验阶段（403）出现。
 */
export function requestEmailCode(email: string, signal?: AbortSignal): Promise<EmailCodeAccepted> {
  return request<EmailCodeAccepted>('/api/v1/auth/email-codes', {
    method: 'POST',
    body: { email },
    signal,
  })
}

/**
 * `POST /api/v1/auth/sessions` —— 校验验证码并建立正式会话。
 *
 * 成功时服务端会同时轮换会话标识与 CSRF 令牌，调用方必须用响应里的
 * `csrf_token` 覆盖本地内存中的旧令牌。
 * 邮箱未注册且不在白名单时为 403 `REGISTRATION_NOT_ALLOWED`。
 */
export function createSession(email: string, code: string): Promise<SessionBootstrap> {
  return request<SessionBootstrap>('/api/v1/auth/sessions', {
    method: 'POST',
    body: { email, code },
  })
}

/** `DELETE /api/v1/auth/sessions` —— 注销；服务端立即失效会话并清空 Cookie。返回 204。 */
export function deleteSession(): Promise<void> {
  return request<void>('/api/v1/auth/sessions', { method: 'DELETE' })
}

/** `GET /api/v1/me` —— 当前用户与所属空间。 */
export function getMe(): Promise<MeResponse> {
  return request<MeResponse>('/api/v1/me', { method: 'GET' })
}
