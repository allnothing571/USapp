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
  RESULT_UNKNOWN: '结果未确认，可以重试；重试不会重复提交。',
  REMINDER_NOT_ASSIGNEE: '只有任务的接收者可以设置提醒。',
  REMINDER_TARGET_CLOSED: '这个任务已经结束，不能这样设置提醒。',
  REMINDER_TIME_INVALID: '这个提醒时间不适用于该任务。',
  REMINDER_TIME_PASSED: '这个提醒时间已经过去，请换一个时间。',
  TASK_NOT_ASSIGNEE: '只有任务的接收者可以完成这项任务。',
  TASK_ALREADY_COMPLETED: '这项任务已经完成了。',
  TASK_EXPIRED: '这项任务已过截止时间，无法完成。',
  WALLET_BALANCE_LIMIT: '资产余额已接近上限，这次奖励无法入账。',
  TASK_COMPLETION_MODE_INVALID: '计数任务请使用 +1，单次任务请使用完成。',
  TASK_NOT_STARTED: '任务尚未开始，开始日期当天起才能记录。',
  TASK_CHANGE_PENDING: '这项任务已有一个待对方确认的变更，请先处理它。',
  TASK_HAS_PROGRESS: '任务（或本周期）已有进度，不能修改；可以拒绝或撤回，或改为取消任务。',
  TASK_CHANGE_DECIDED: '这项变更已经处理过了。',
  TASK_CHANGE_FORBIDDEN: '你不能执行这项变更操作。',
  TASK_CHANGE_NOT_FOUND: '变更不存在，请刷新任务后重试。',
  TASK_CANCELLED: '任务已取消，不能再记录或完成。',
  TASK_SUPERSEDED: '任务已按确认的修改替换为新任务，请刷新后操作新任务。',
  PRODUCT_NOT_FOUND: '奖励不存在，或已下架。',
  PRODUCT_FORBIDDEN: '只有上架这件奖励的一方可以修改或下架。',
  PRODUCT_ARCHIVED: '奖励已下架，不能再修改。',
  PRODUCT_VERSION_CONFLICT: '奖励已被修改，请刷新后再操作。',
  INSUFFICIENT_BALANCE: '小红花不足，无法兑换（没有扣除任何小红花）。',
  REDEMPTION_NOT_FOUND: '兑换记录不存在。',
  REDEMPTION_FORBIDDEN: '你不能执行这项兑换操作。',
  REDEMPTION_MODE_UNSUPPORTED: '申请兑换将在之后的版本提供。',
  REDEMPTION_STATE_CONFLICT: '兑换状态已改变，请刷新后重试。',
  REDEMPTION_ALREADY_FULFILLED: '这次兑换已经确认履约了。',
  MESSAGE_NOT_FOUND: '留言不存在。',
  MESSAGE_REPLY_FORBIDDEN: '只有收到留言的一方可以回复。',
  TASK_NOT_COMPLETED: '任务完成后才能回应。',
  TASK_REACTION_FORBIDDEN: '只有发布任务的一方可以回应。',
  TASK_ALREADY_REACTED: '这个任务已经回应过了。',
  MESSAGE_ALREADY_REPLIED: '这条留言已经回复过了。',
  ANNIVERSARY_NOT_FOUND: '纪念日不存在。',
  ANNIVERSARY_EDIT_FORBIDDEN: '你不能直接执行这项修改。',
  ANNIVERSARY_VERSION_CONFLICT: '纪念日已被修改，请刷新后再操作（你填写的内容仍保留）。',
  ANNIVERSARY_PROPOSAL_PENDING: '有一条待处理的修改申请，请先处理它。',
  ANNIVERSARY_PROPOSAL_NOT_FOUND: '修改申请不存在。',
  ANNIVERSARY_PROPOSAL_DECIDED: '这条修改申请已经处理过了。',
  ANNIVERSARY_PROPOSAL_STALE: '纪念日已更新，这条修改申请不能再同意。',
  MEDIA_DISABLED: '照片上传暂未开放。',
  MEDIA_NOT_FOUND: '照片不存在，或暂时无法查看。',
  MEDIA_LIMIT_EXCEEDED: '上传次数或空间已达上限，请稍后再试。',
  MEDIA_NOT_UPLOADED: '存储还没有收到这张照片，请重新上传。',
  MEDIA_GRANT_EXPIRED: '这次上传已过期，请重新上传。',
  MEDIA_STORE_UNAVAILABLE: '图片存储暂时不可用，请稍后重试。',
  MEDIA_ALREADY_DELETED: '这张照片已经删除了。',
  MEDIA_UNDO_CLOSED: '已经过了可撤销的时间，这张照片不能再恢复。',
  MEDIA_DELETION_NOT_ACTIVE: '这次删除已经撤销过了。',
  MEMORY_NOT_FOUND: '回忆不存在。',
  MEMORY_EDIT_FORBIDDEN: '你不能直接执行这项修改。',
  MEMORY_VERSION_CONFLICT: '回忆已被修改，请确认最新内容后再提交（你填写的内容仍保留）。',
  MEMORY_PROPOSAL_PENDING: '有一条待处理的修改申请，请先处理它。',
  MEMORY_PROPOSAL_NOT_FOUND: '修改申请不存在。',
  MEMORY_PROPOSAL_DECIDED: '这条修改申请已经处理过了。',
  MEMORY_PROPOSAL_STALE: '回忆已更新，这条修改申请不能再同意。',
  MEMORY_PHOTO_UNAVAILABLE: '有照片已不可用，请重新选择（没有保存任何内容）。',
  ALBUM_NOT_FOUND: '相册不存在。',
  ALBUM_PHOTO_UNAVAILABLE: '有照片不能加入相册，这次没有加入任何照片。可以移除后重试。',
  ALBUM_PHOTO_NOT_FOUND: '相册里没有这张照片。',
  ALBUM_PHOTO_FORBIDDEN: '只有上传者可以移除这张照片。',
  ALBUM_PHOTO_REMOVED: '这张照片已经移除了。',
  ALBUM_EDIT_FORBIDDEN: '你不能执行这项改名操作。',
  ALBUM_VERSION_CONFLICT: '相册名称已更新，请确认最新名称后再提交（你填写的内容仍保留）。',
  PLAN_EDIT_FORBIDDEN: '当前账号没有权限执行这项计划操作。',
  PLAN_CONFLICT: '计划的版本、状态或关联已变化，请重新加载后再试。',
  MEMORY_ALREADY_DELETED: '回忆目前已删除，请查看最近删除的回忆。',
  MEMORY_UNDO_CLOSED: '该次删除或其关联照片已不能撤销，回忆保持删除状态。',
  MEMORY_DELETION_NOT_ACTIVE: '该次删除已撤销，请以重新读取的状态为准。',
  MEDIA_DELETION_LINKED: '这张照片随整条回忆删除，请到最近删除的回忆中一起撤销。',
  ALBUM_PROPOSAL_PENDING: '有一条待处理的改名申请，请先处理它。',
  ALBUM_PROPOSAL_NOT_FOUND: '改名申请不存在。',
  ALBUM_PROPOSAL_DECIDED: '这条改名申请已经处理过了。',
  PUSH_UNAVAILABLE: '设备提醒暂未开放。',
  PUSH_DEVICE_LIMIT: '最多可在 10 台设备上开启提醒，请先关闭一台设备。',
  PUSH_SUBSCRIPTION_NOT_FOUND: '设备不存在，可能已被移除。',
  AVATAR_ITEM_NOT_FOUND: '装扮不存在，请重新读取。',
  AVATAR_VERSION_CONFLICT: '装扮或形象版本已变化，请重新读取并检查后再确认。',
  AVATAR_ASSET_INVALID: '这件装扮的素材配置不可用。',
  AVATAR_STARTER_INVALID: '初始形象配置无效。',
  AVATAR_ALREADY_OWNED: '已经拥有这件装扮，这次没有重复购买。',
  AVATAR_ITEM_UNAVAILABLE: '这件装扮现在不能购买，请重新读取商店。',
  AVATAR_SLOT_INVALID: '所选装扮不属于对应部位，请检查草稿。',
  AVATAR_NOT_OWNED: '尚未拥有所选装扮，草稿没有保存。',
  AVATAR_DEFAULTS_UNAVAILABLE: '尚未配置完整的初始形象，暂时不能使用默认部位。',
  EXPORT_TOO_LARGE: '数据量超过单次导出上限，没有生成文件。',
  EXPORT_UNAVAILABLE: '暂时无法生成导出文件，没有生成任何文件，请稍后重试。',

  // —— 前端合成码 ——
  NETWORK_ERROR: '网络不可用，请检查网络连接后重试。',
  OFFLINE_MUTATION_BLOCKED: '当前离线，业务操作不会提交。',
  DOWNLOAD_STREAM_UNSUPPORTED: '当前浏览器无法按上限分块接收文件，已放弃下载。',
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
