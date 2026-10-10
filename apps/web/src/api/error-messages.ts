/**
 * 错误码 → 中文提示。
 *
 * 判定一律以 `code` 为准，`message` 只作为未知码时的兜底，
 * 避免因服务端文案调整导致前端逻辑变化。
 */
import { ApiError } from './client'
import type { ClientErrorCode } from './client'
import type { ServerErrorCode } from './types'

type KnownCode = ServerErrorCode | ClientErrorCode

const MESSAGES: Record<KnownCode, string> = {
  // —— 契约枚举 ——
  VALIDATION_ERROR: '请求参数不合法，请检查后重试。',
  UNAUTHENTICATED: '登录状态已失效，请重新登录。',
  CSRF_TOKEN_INVALID: '安全令牌已失效，请刷新页面后重试。',
  ORIGIN_NOT_ALLOWED: '当前访问来源不被允许，请通过正式地址打开。',
  RATE_LIMITED: '操作过于频繁，请稍后再试。',
  CODE_INVALID_OR_EXPIRED: '验证码错误或已过期，请重新获取。',
  REGISTRATION_NOT_ALLOWED: '该邮箱不在注册白名单内，暂时无法创建新账号。',
  SPACE_NOT_FOUND: '未找到该空间，可能尚未创建或邀请已失效。',
  SPACE_ALREADY_EXISTS: '你已经创建过空间了，不需要重复创建。',
  SPACE_FULL: '这个空间已经住满了（2 位成员）。',
  INVITE_INVALID_OR_EXPIRED: '邀请码无效或已过期，请让对方重新生成。',
  INVITE_REVOKED: '该邀请已被撤销。',
  ALREADY_IN_SPACE: '你已经在一个空间里了。',
  NOT_FOUND: '请求的资源不存在。',
  INTERNAL_ERROR: '服务暂时不可用，请稍后再试。',
  TASK_NOT_FOUND: '任务不存在，或不属于你所在的空间。',
  PARTNER_REQUIRED: '需要对方加入空间后才能发布任务。',
  IDEMPOTENCY_KEY_REUSED: '这次提交与之前的请求冲突，请重新填写后再发布。',
  RESULT_UNKNOWN: '结果未确认，可以重试；重试不会重复发布。',

  // —— 前端合成码 ——
  NETWORK_ERROR: '网络不可用，请检查网络连接后重试。',
  OFFLINE_MUTATION_BLOCKED: '当前离线，业务操作不会提交。',
}

/** 把任意异常转换成可以直接展示给用户的中文文案。 */
export function describeError(error: unknown): string {
  if (error instanceof ApiError) {
    const known = MESSAGES[error.code as KnownCode]
    if (known) return known
    return error.message || '出现未知错误，请稍后再试。'
  }
  if (error instanceof Error) {
    return error.message || '出现未知错误，请稍后再试。'
  }
  return '出现未知错误，请稍后再试。'
}

/** 取出错误码，非 ApiError 时返回 null。 */
export function errorCodeOf(error: unknown): string | null {
  return error instanceof ApiError ? error.code : null
}
