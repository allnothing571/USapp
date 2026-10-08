/**
 * contracts/openapi.yaml 组件的**手写** TypeScript 映射。
 * 字段名一律与契约保持完全一致（snake_case），不做驼峰转换，
 * 这样任何一处字段拼写错误都会在 typecheck 阶段暴露。
 */

/** 契约里所有 id 都是 uuid 字符串；用别名表达意图，不引入运行时校验。 */
export type Uuid = string
/** ISO 8601 date-time 字符串（原样保留，展示层负责本地化）。 */
export type IsoDateTime = string
/** 空间时区，阶段 0 契约里是 const Asia/Shanghai。 */
export type Timezone = 'Asia/Shanghai'

/** `#/components/schemas/User` */
export interface User {
  id: Uuid
  email: string
  display_name: string
  created_at: IsoDateTime
}

/** `#/components/schemas/SpaceMember` */
export interface SpaceMember {
  user_id: Uuid
  /** 契约 enum: [1, 2]，slot 1 为创建者。 */
  slot: 1 | 2
  display_name: string
  joined_at: IsoDateTime
}

/** `#/components/schemas/Space` */
export interface Space {
  id: Uuid
  name: string
  timezone: Timezone
  member_count: number
  created_at: IsoDateTime
  /** 契约里不是 required；`GET /spaces/current` 会带上成员列表。 */
  members?: SpaceMember[]
}

/** `#/components/schemas/Invite`（只有元数据，**没有**邀请码明文） */
export interface Invite {
  id: Uuid
  expires_at: IsoDateTime
  created_at: IsoDateTime
  max_uses: number
  used_count: number
  revoked?: boolean
}

/** `#/components/schemas/CreatedInvite`：`code` / `invite_url` 只在创建响应里出现一次。 */
export interface CreatedInvite extends Invite {
  code: string
  invite_url: string
}

/** `#/components/schemas/SessionBootstrap` */
export interface SessionBootstrap {
  authenticated: boolean
  /** CSRF 令牌。只允许存在于内存中，不得写入 localStorage / sessionStorage / cookie。 */
  csrf_token: string
  user?: User | null
  space?: Space | null
  pre_auth_expires_at?: IsoDateTime | null
}

/** `#/components/schemas/EmailCodeAccepted` */
export interface EmailCodeAccepted {
  resend_after_seconds: number
  expires_in_seconds: number
}

/** `#/components/schemas/MeResponse` */
export interface MeResponse {
  user: User
  space?: Space | null
}

/** `#/components/schemas/UpdateMeRequest`（阶段 1 M1）：只允许这一个字段。 */
export interface UpdateMeRequest {
  display_name: string
}

/** `#/components/schemas/SpaceResponse` */
export interface SpaceResponse {
  space: Space
}

/** Own ledger balances; bounds are below Number.MAX_SAFE_INTEGER. */
export interface Wallet {
  flower: number
  sunshine: number
}

export interface WalletResponse {
  wallet: Wallet
}

/** `#/components/schemas/InviteResponse` */
export interface InviteResponse {
  invite: CreatedInvite
}

/** `#/components/schemas/InviteListResponse` */
export interface InviteListResponse {
  invites: Invite[]
}

/**
 * `#/components/schemas/ErrorEnvelope.error.code` 的完整枚举。
 * 与契约一一对应；`code` 是唯一可编程判定的字段，`message` 只用于兜底展示。
 */
export type ServerErrorCode =
  | 'VALIDATION_ERROR'
  | 'UNAUTHENTICATED'
  | 'CSRF_TOKEN_INVALID'
  | 'ORIGIN_NOT_ALLOWED'
  | 'RATE_LIMITED'
  | 'CODE_INVALID_OR_EXPIRED'
  | 'REGISTRATION_NOT_ALLOWED'
  | 'SPACE_NOT_FOUND'
  | 'SPACE_ALREADY_EXISTS'
  | 'SPACE_FULL'
  | 'INVITE_INVALID_OR_EXPIRED'
  | 'INVITE_REVOKED'
  | 'ALREADY_IN_SPACE'
  | 'NOT_FOUND'
  | 'INTERNAL_ERROR'

/** `#/components/schemas/ErrorEnvelope` */
export interface ErrorEnvelope {
  error: {
    code: ServerErrorCode | string
    message: string
    details?: Record<string, unknown>
  }
}
