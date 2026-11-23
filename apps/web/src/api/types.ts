/**
 * contracts/openapi.yaml 组件的**手写** TypeScript 映射。
 * 字段名一律与契约保持完全一致（snake_case），不做驼峰转换，
 * 这样任何一处字段拼写错误都会在 typecheck 阶段暴露。
 */

import type { OptionalNotificationResult } from './notifications'

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
  room_issuer_side?: 'left' | 'right'
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

/** 任务视图（`listTasks` 的 `view` 参数，M3-B）。 */
export type TaskView = 'open' | 'history'

/**
 * 任务状态由服务端按读取时刻推导。M6：cancelled（经确认取消）与 superseded（经确认修改，
 * 由新任务替换）；优先级 completed > cancelled/superseded > expired > upcoming > open。
 */
export type TaskStatus = 'open' | 'completed' | 'expired' | 'upcoming' | 'cancelled' | 'superseded'

/** M6 `#/components/schemas/TaskClosure`：根任务（重复任务为模板）的关闭记录，当前状态。 */
export interface TaskClosure {
  reason: 'cancelled' | 'superseded'
  closed_at: IsoDateTime
  replaced_by: Uuid | null
}

/** M6 `#/components/schemas/TaskChangeTerms`：提议的完整新内容。 */
export interface TaskChangeTerms {
  title: string
  description: string
  reward_flowers: number
  reward_sunshine: number
  completion_mode: 'single' | 'count'
  target_count: number
  deadline: { local_date: string; local_time: string | null } | null
  start_date?: string
}

export type TaskChangeKind = 'edit' | 'cancel'
export type TaskChangeAction = 'approve' | 'reject' | 'withdraw'

/** M6 `#/components/schemas/TaskPendingChange`：待对方确认的变更（当前状态）。 */
export interface TaskPendingChange {
  id: Uuid
  kind: TaskChangeKind
  proposer_id: Uuid
  created_at: IsoDateTime
  terms: TaskChangeTerms | null
  /** 现在不能同意的原因（仍可拒绝或撤回）；null 表示可以同意（决定时刻会再次检查）。 */
  blocked_reason: 'progress' | 'completed' | 'expired' | 'closed' | null
}

/** `#/components/schemas/TaskDeadlineInput`：Asia/Shanghai 本地日期与可选的 HH:MM。 */
export interface TaskDeadlineInput {
  local_date: string
  local_time?: string | null
}

/** `#/components/schemas/TaskDeadline` */
export interface TaskDeadline {
  local_date: string
  local_time: string | null
  /** 截止时刻（不含），UTC。 */
  at: IsoDateTime
}

/** Schedules: recurring daily (M5-A) and weekly (M5-B); one bounded custom range (M5-C). */
export type ScheduleType = 'one_off' | 'daily' | 'weekly' | 'custom'

/** `#/components/schemas/Task` */
export interface Task {
  reaction?: TaskReaction | null
  schedule_type?: ScheduleType
  /** Period identity: the Beijing date (daily) or that week's Monday (weekly). */
  period_date?: string
  /** Custom range start date (inclusive); its end date is `deadline.local_date`. */
  start_date?: string
  /** Optional for pre-M4 snapshots; omitted values mean a single task. */
  completion_mode?: 'single' | 'count'
  target_count?: number
  progress_count?: number
  id: Uuid
  title: string
  description: string
  creator_id: Uuid
  assignee_id: Uuid
  reward_flowers: number
  reward_sunshine: number
  deadline: TaskDeadline | null
  status: TaskStatus
  completed_at: IsoDateTime | null
  created_at: IsoDateTime
  /** M6, only when present: the root's closure, the replaced root, an undecided change. */
  closure?: TaskClosure
  replaces?: Uuid
  pending_change?: TaskPendingChange
}

/** M6 `#/components/schemas/ProposeTaskChangeRequest` */
export type ProposeTaskChangeRequest = { kind: 'edit'; terms: CreateTaskRequest } | { kind: 'cancel' }

/** M6 `#/components/schemas/TaskChange`：不可变事实（提议响应中 decision 为 null）。 */
export interface TaskChange {
  id: Uuid
  task_id: Uuid
  kind: TaskChangeKind
  proposer_id: Uuid
  created_at: IsoDateTime
  terms: TaskChangeTerms | null
  decision: {
    outcome: 'approved' | 'rejected' | 'withdrawn'
    decided_by: Uuid
    decided_at: IsoDateTime
    successor_task_id: Uuid | null
  } | null
}

/** M6 `#/components/schemas/TaskChangeResponse`：change 不可变；task/successor 为当前状态。 */
export interface TaskChangeResponse {
  change: TaskChange
  task: Task
  successor: Task | null
}

/** M7 reward products: redemption mode is stored only; M7 implements no redemption. */
export type RedemptionMode = 'direct' | 'request'

/** M7 `#/components/schemas/RewardProduct`：当前状态（读取时刻）。 */
export interface RewardProduct {
  id: Uuid
  creator_id: Uuid
  /** The partner who will redeem it in a later milestone. */
  owner_id: Uuid
  title: string
  description: string
  price_flowers: number
  redemption_mode: RedemptionMode
  status: 'active' | 'archived'
  version: number
  created_at: IsoDateTime
  updated_at: IsoDateTime
}

/** M7 `#/components/schemas/RewardProductTerms` */
export interface RewardProductTerms {
  title: string
  description?: string
  price_flowers: number
  redemption_mode: RedemptionMode
}

/** M7 `#/components/schemas/RewardProductMutation`：一次写入的不可变事实。 */
export interface RewardProductMutation {
  action: 'create' | 'edit' | 'archive'
  version: number
  actor_id: Uuid
  title: string
  description: string
  price_flowers: number
  redemption_mode: RedemptionMode
  status: 'active' | 'archived'
  created_at: IsoDateTime
}

export interface RewardProductMutationResponse {
  mutation: RewardProductMutation
  product: RewardProduct
}

/** Cursor pages in (created_at, id) order; visibility can change between pages (not a snapshot). */
export interface RewardProductListResponse {
  products: RewardProduct[]
  next_cursor: string | null
}

/** M8 `#/components/schemas/Redemption`：兑换时的不可变快照；status/fulfilled_* 为当前状态。 */
export type RedemptionTransition = 'confirm' | 'reject' | 'withdraw' | 'cancel_propose' | 'cancel_approve' | 'cancel_reject' | 'cancel_withdraw'

export interface Redemption {
  id: Uuid
  product_id: Uuid
  product_version: number
  creator_id: Uuid
  redeemer_id: Uuid
  title: string
  description: string
  price_flowers: number
  redemption_mode: RedemptionMode
  operation_id: Uuid | null
  status: 'pending_confirmation' | 'pending_fulfillment' | 'fulfilled' | 'cancelled'
  cancel_reason?: 'withdrawn' | 'rejected' | 'mutual' | 'admin' | null
  cancelled_at?: IsoDateTime | null
  pending_cancellation?: { id: Uuid; proposed_by: Uuid } | null
  created_at: IsoDateTime
  fulfilled_at: IsoDateTime | null
  fulfilled_by: Uuid | null
}

/** M8 `#/components/schemas/RedemptionMutation`：一次兑换或履约的不可变事实。 */
export interface RedemptionMutation {
  action: 'redeem' | 'fulfill' | RedemptionTransition
  refunded_flowers?: number
  redemption_id: Uuid
  price_flowers?: number
  debited_flowers?: number
  product_version?: number
  [key: string]: unknown
}

export interface RedemptionMutationResponse {
  mutation: RedemptionMutation
  redemption: Redemption
}

export interface RedemptionListResponse {
  redemptions: Redemption[]
  next_cursor: string | null
}

/** P2-1 `#/components/schemas/MessageReply`：收到留言的一方的单条回复。 */
export interface MessageReply {
  author_id: Uuid
  body: string
  created_at: IsoDateTime
}

/** P2-1 `#/components/schemas/Message`：留言（只追加）与它的回复（可能为 null）。 */
export interface Message {
  id: Uuid
  author_id: Uuid
  recipient_id: Uuid
  body: string
  created_at: IsoDateTime
  reply: MessageReply | null
}

/** P2-1 `#/components/schemas/MessageMutation`：一次留言或回复的不可变事实，同键重放时不变。 */
export interface MessageMutation {
  action: 'create' | 'reply'
  message_id: Uuid
  author_id: Uuid
  recipient_id?: Uuid
  body: string
  created_at: IsoDateTime
}

export interface MessageMutationResponse {
  mutation: MessageMutation
  message: Message
  /** P2-3b: original notification facts; null for a legacy write. */
  notification?: OptionalNotificationResult | null
}

export interface MessageResponse {
  message: Message
}

/** Cursor pages in (created_at DESC, id DESC); not a snapshot. */
export interface MessageListResponse {
  messages: Message[]
  next_cursor: string | null
}

/** P2-2: civil date `YYYY-MM-DD` (Beijing); never parsed with `new Date()`. */
export type CivilDate = string
export type AnniversaryKind = 'anniversary' | 'countdown'

/** P2-2 `#/components/schemas/AnniversaryTerms`: the full, normalised editable terms. */
export interface AnniversaryTerms {
  title: string
  date: CivilDate
  kind: AnniversaryKind
  note: string
}

/** P2-2 `#/components/schemas/AnniversaryComputed`: server values against `as_of_date`. */
export interface AnniversaryComputed {
  /** Anniversary only: as_of_date - date (negative before the date). */
  days_since: number | null
  /** Anniversary only: the next yearly occurrence on or after as_of_date (may pass 2199). */
  next: { date: CivilDate; years: number; days_until: number } | null
  /** Countdown only: date - as_of_date (negative once passed). */
  days_until: number | null
}

export interface AnniversaryProposal extends AnniversaryTerms {
  id: Uuid
  proposer_id: Uuid
  base_version: number
  created_at: IsoDateTime
}

export interface AnniversaryDecision {
  proposal_id: Uuid
  proposer_id: Uuid
  outcome: 'approved' | 'rejected' | 'withdrawn'
  decided_by: Uuid
  decided_at: IsoDateTime
}

/** P2-2 `#/components/schemas/Anniversary` */
export interface Anniversary extends AnniversaryTerms {
  id: Uuid
  creator_id: Uuid
  version: number
  created_at: IsoDateTime
  updated_at: IsoDateTime
  pinned_by_me: boolean
  computed: AnniversaryComputed
  pending_proposal: AnniversaryProposal | null
  last_decision: AnniversaryDecision | null
}

/** P2-2 `#/components/schemas/AnniversaryMutation`: immutable facts, identical on same-key replay. */
export interface AnniversaryMutation {
  action: 'create' | 'edit' | 'approve' | 'propose' | 'decide'
  anniversary_id: Uuid
  version?: number | null
  actor_id?: Uuid
  proposal_id?: Uuid | null
  proposer_id?: Uuid
  base_version?: number
  outcome?: AnniversaryDecision['outcome']
  decided_by?: Uuid
  decided_at?: IsoDateTime
  title?: string
  date?: CivilDate
  kind?: AnniversaryKind
  note?: string
  created_at?: IsoDateTime
}

export interface AnniversaryMutationResponse {
  mutation: AnniversaryMutation
  as_of_date: CivilDate
  anniversary: Anniversary
  /** P2-3b, create and creator edit only: original notification facts; null for a legacy write. */
  notification?: OptionalNotificationResult | null
}

export interface AnniversaryResponse {
  as_of_date: CivilDate
  anniversary: Anniversary
}

/** Pages in the immutable identity order (created_at DESC, id DESC). */
export interface AnniversaryListResponse {
  as_of_date: CivilDate
  anniversaries: Anniversary[]
  next_cursor: string | null
}

export type AnniversaryHomeSource = 'pinned' | 'auto' | 'auto_past' | 'none'

export interface AnniversaryHomeResponse {
  as_of_date: CivilDate
  source: AnniversaryHomeSource
  anniversary: Anniversary | null
}

export interface AnniversaryPin {
  anniversary_id: Uuid | null
  seq: number
  updated_at: IsoDateTime | null
}

export interface AnniversaryPinResponse {
  mutation: { action: 'pin'; user_id: Uuid; seq: number; anniversary_id: Uuid | null; created_at: IsoDateTime }
  pin: AnniversaryPin
}

/** `#/components/schemas/CreateTaskRequest`：没有接收者字段，接收者由服务端推导。 */
export interface CreateTaskRequest {
  /** P2-3b optional partner notice; the form always sends an explicit boolean. */
  notify_partner?: boolean
  schedule_type?: ScheduleType
  completion_mode?: 'single' | 'count'
  target_count?: number
  title: string
  description?: string
  reward_flowers: number
  reward_sunshine: number
  deadline?: TaskDeadlineInput | null
  /** Custom range only: inclusive Beijing dates. */
  start_date?: string
  end_date?: string
}

/** `#/components/schemas/TaskResponse` */
export interface TaskResponse {
  task: Task
}

/** P2-3b `#/components/schemas/TaskPublishResponse`: a publication with its original notification facts. */
export interface TaskPublishResponse extends TaskResponse {
  notification?: OptionalNotificationResult | null
}

/** `#/components/schemas/TaskListResponse` */
export interface TaskListResponse {
  tasks: Task[]
  next_cursor: string | null
}

/** `#/components/schemas/TaskSettlement`：本次入账金额（未奖励的资产为 0）。 */
export interface TaskSettlement {
  operation_id: Uuid
  flower: number
  sunshine: number
}

/** `#/components/schemas/CompleteTaskResponse`：只含不可变事实，没有余额。 */
export interface CompleteTaskResponse {
  task: Task
  settlement: TaskSettlement
}

/** Immutable facts for one +1 action, not the task's latest progress. */
export interface ProgressTaskResponse {
  period_date?: string
  task_id: Uuid
  event_id: Uuid | null
  progress_count: number
  target_count: number
  recorded_at: string | null
  completed: boolean
  ignored: boolean
  settlement: TaskSettlement | null
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
/** P3-1 `#/components/schemas/Media`: facts only, never an object key. */
export interface MediaFacts {
  media_id: string
  state: 'granted' | 'expired' | 'ready' | 'rejected' | 'removed'
  uploader_id: string
  content_type: string
  size_bytes: number
  created_at: string
  reason: string | null
  width: number | null
  height: number | null
}

/** P3-1: the form that writes the staging location. Held in memory only, never logged or persisted. */
export interface MediaUploadForm {
  url: string
  fields: Record<string, string>
  expires_at: string
}

export interface MediaResponse {
  media: MediaFacts
  upload: MediaUploadForm | null
}

/** D6 `#/components/schemas/MediaDeletion` */
export interface MediaDeletion {
  deletion_id: string
  media_id: string
  cycle_no: number
  deleted_at: string
  purge_after: string
  undone_at: string | null
  state: 'deleted' | 'undone' | 'purging' | 'purged'
}

export interface MediaDeletionResponse {
  deletion: MediaDeletion
}

/** D6 `#/components/schemas/MediaDeletionContext`: `unavailable` withholds the parent's kind, id and text. */
export interface MediaDeletionContext {
  kind: 'memory' | 'album' | 'unavailable' | 'none'
  relation: 'current' | 'former' | null
  target_id: string | null
  label: string | null
  position: number | null
}

/** D6 one row of `#/components/schemas/UndoableMediaDeletionList`: these facts and nothing else. */
export interface UndoableMediaDeletion {
  deletion_id: string
  media_id: string
  deleted_at: string
  purge_after: string
  uploaded_at: string
  context: MediaDeletionContext
}

export interface UndoableMediaDeletionList {
  deletions: UndoableMediaDeletion[]
  next_cursor: string | null
}

/** P3-1b `#/components/schemas/MemoryPhoto` */
export interface MemoryPhoto {
  position: number
  media_id: string
  uploader_id: string
  width: number | null
  height: number | null
  /** D6: a placeholder. Its content route answers 404, so nothing may request it. */
  deleted: boolean
}

/** The full, normalised editable terms; `media_ids` are in display order. */
export interface MemoryTerms {
  date: CivilDate
  title: string
  description: string
  media_ids: string[]
}

/** P3-1b `#/components/schemas/Memory` */
export interface Memory {
  id: string
  creator_id: string
  version: number
  date: CivilDate
  title: string
  description: string
  created_at: string
  updated_at: string
  photos: MemoryPhoto[]
  pending_proposal: { id: string; proposer_id: string; base_version: number; created_at: string } | null
  pending_delete_request?: { id: string; requester_id: string; created_at: string } | null
}

/** P3-1b `#/components/schemas/MemoryMutation`: immutable facts, identical on same-key replay. */
export interface MemoryMutation {
  action: 'create' | 'edit' | 'approve' | 'propose' | 'decide'
  memory_id: string
  proposal_id?: string | null
  version?: number | null
  actor_id?: string
  proposer_id?: string
  base_version?: number
  outcome?: 'approved' | 'rejected' | 'withdrawn'
  decided_by?: string
  created_at?: string
  decided_at?: string
}

export interface MemoryMutationResponse {
  mutation: MemoryMutation
  memory: Memory
  /** D20: a direct create's original notification decision (0.41.0); absent on other memory writes. */
  notification?: OptionalNotificationResult | null
}

export interface MemoryResponse {
  memory: Memory
}

export interface MemoryListResponse {
  memories: Memory[]
  next_cursor: string | null
  uploads_enabled: boolean
}

/** One side of a review record. */
export interface MemoryContent {
  version?: number
  date: CivilDate
  title: string
  description: string
  photos: MemoryPhoto[]
}

/** P3-1b `#/components/schemas/MemoryProposalResponse`: the immutable original and suggestion. */
export interface MemoryProposal {
  id: string
  memory_id: string
  proposer_id: string
  base_version: number
  created_at: string
  status: 'pending' | 'approved' | 'rejected' | 'withdrawn'
  decided_by: string | null
  decided_at: string | null
  approved_version: number | null
  original: MemoryContent
  proposed: MemoryContent
}

export interface MemoryProposalResponse {
  proposal: MemoryProposal
}

/**
 * 0.37.0 `#/components/schemas/MemoryProposalListResponse`: one memory's edit requests as summaries, newest first. No text or photos:
 * the comparison is read from the proposal itself. A decision is a historical fact; `current_version` is the memory's latest version
 * as the same statement read it.
 */
export interface MemoryProposalSummary {
  id: string
  memory_id: string
  proposer_id: string
  base_version: number
  created_at: string
  status: 'pending' | 'approved' | 'rejected' | 'withdrawn'
  decided_by: string | null
  decided_at: string | null
  approved_version: number | null
}

export interface MemoryProposalListResponse {
  proposals: MemoryProposalSummary[]
  next_cursor: string | null
  current_version: number
}

/** P3-2a `#/components/schemas/AlbumCover`. `chosen`: a member's latest choice; otherwise the first photo chosen in the latest add. */
export interface AlbumCover {
  media_id: string
  uploader_id: string
  width: number | null
  height: number | null
  chosen: boolean
}

/** P3-2 `#/components/schemas/Album`. `version` moves only when the name changes. */
export interface Album {
  id: string
  creator_id: string
  name: string
  version: number
  pending_proposal: { id: string; proposer_id: string; base_version: number; created_at: string } | null
  created_at: string
  photo_count: number
  cover: AlbumCover | null
}

/** P3-2a `#/components/schemas/AlbumPhoto` */
export interface AlbumPhoto {
  seq: number
  media_id: string
  uploader_id: string
  width: number | null
  height: number | null
  added_at: string
}

/** P3-2 `#/components/schemas/AlbumMutation`: immutable facts, identical on same-key replay. */
export interface AlbumMutation {
  action: 'create' | 'edit' | 'add_photos' | 'remove_photo' | 'set_cover' | 'propose' | 'decide'
  album_id: string
  actor_id: string
  version?: number | null
  proposal_id?: string
  base_version?: number
  outcome?: 'approved' | 'rejected' | 'withdrawn'
  batch_id?: string
  media_ids?: string[]
  media_id?: string
  event_no?: number
  created_at: string
}

export interface AlbumMutationResponse {
  mutation: AlbumMutation
  album: Album
  /** D20: an add batch's original notification decision (0.41.0); absent on other album writes. */
  notification?: OptionalNotificationResult | null
}

export interface AlbumResponse {
  album: Album
}

export interface AlbumListResponse {
  albums: Album[]
  next_cursor: string | null
  uploads_enabled: boolean
}

export interface AlbumPhotoListResponse {
  photos: AlbumPhoto[]
  next_cursor: string | null
}

/** P3-2b `#/components/schemas/AlbumProposalResponse`: V1 §30's original, suggestion, requester, time and status. */
export interface AlbumProposal {
  id: string
  album_id: string
  proposer_id: string
  base_version: number
  created_at: string
  status: 'pending' | 'approved' | 'rejected' | 'withdrawn'
  decided_by: string | null
  decided_at: string | null
  approved_version: number | null
  original_name: string
  proposed_name: string
}

export interface AlbumProposalResponse {
  proposal: AlbumProposal
}

/**
 * 0.38.0 `#/components/schemas/AlbumProposalListResponse`: one album's rename requests as summaries, newest first. No names: the
 * comparison is read from the request itself. A decision is a historical fact; `current_version` is the album's latest name version as
 * the same statement read it.
 */
export interface AlbumProposalSummary {
  id: string
  album_id: string
  proposer_id: string
  base_version: number
  created_at: string
  status: 'pending' | 'approved' | 'rejected' | 'withdrawn'
  decided_by: string | null
  decided_at: string | null
  approved_version: number | null
}

export interface AlbumProposalListResponse {
  proposals: AlbumProposalSummary[]
  next_cursor: string | null
  current_version: number
}

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
  | 'TASK_NOT_FOUND'
  | 'PARTNER_REQUIRED'
  | 'IDEMPOTENCY_KEY_REUSED'
  | 'REMINDER_NOT_ASSIGNEE'
  | 'REMINDER_TARGET_CLOSED'
  | 'REMINDER_TIME_INVALID'
  | 'REMINDER_TIME_PASSED'
  | 'RESULT_UNKNOWN'
  | 'TASK_NOT_ASSIGNEE'
  | 'TASK_ALREADY_COMPLETED'
  | 'TASK_EXPIRED'
  | 'WALLET_BALANCE_LIMIT'
  | 'TASK_COMPLETION_MODE_INVALID'
  | 'TASK_NOT_STARTED'
  | 'TASK_CHANGE_PENDING'
  | 'TASK_HAS_PROGRESS'
  | 'TASK_CHANGE_DECIDED'
  | 'TASK_CHANGE_FORBIDDEN'
  | 'TASK_CHANGE_NOT_FOUND'
  | 'TASK_CANCELLED'
  | 'TASK_SUPERSEDED'
  | 'PRODUCT_NOT_FOUND'
  | 'PRODUCT_FORBIDDEN'
  | 'PRODUCT_ARCHIVED'
  | 'PRODUCT_VERSION_CONFLICT'
  | 'INSUFFICIENT_BALANCE'
  | 'REDEMPTION_NOT_FOUND'
  | 'REDEMPTION_FORBIDDEN'
  | 'REDEMPTION_MODE_UNSUPPORTED'
  | 'REDEMPTION_ALREADY_FULFILLED'
  | 'REDEMPTION_STATE_CONFLICT'
  | 'MESSAGE_NOT_FOUND'
  | 'MESSAGE_REPLY_FORBIDDEN'
  | 'MESSAGE_ALREADY_REPLIED'
  | 'TASK_NOT_COMPLETED'
  | 'TASK_REACTION_FORBIDDEN'
  | 'TASK_ALREADY_REACTED'
  | 'ANNIVERSARY_NOT_FOUND'
  | 'ANNIVERSARY_EDIT_FORBIDDEN'
  | 'ANNIVERSARY_VERSION_CONFLICT'
  | 'ANNIVERSARY_PROPOSAL_PENDING'
  | 'ANNIVERSARY_PROPOSAL_NOT_FOUND'
  | 'ANNIVERSARY_PROPOSAL_DECIDED'
  | 'ANNIVERSARY_PROPOSAL_STALE'
  | 'MEDIA_DISABLED'
  | 'MEDIA_NOT_FOUND'
  | 'MEDIA_LIMIT_EXCEEDED'
  | 'MEDIA_NOT_UPLOADED'
  | 'MEDIA_GRANT_EXPIRED'
  | 'MEDIA_STORE_UNAVAILABLE'
  | 'MEDIA_ALREADY_DELETED'
  | 'MEDIA_UNDO_CLOSED'
  | 'MEDIA_DELETION_NOT_ACTIVE'
  | 'MEMORY_NOT_FOUND'
  | 'MEMORY_EDIT_FORBIDDEN'
  | 'MEMORY_VERSION_CONFLICT'
  | 'MEMORY_PROPOSAL_PENDING'
  | 'MEMORY_PROPOSAL_NOT_FOUND'
  | 'MEMORY_PROPOSAL_DECIDED'
  | 'MEMORY_PROPOSAL_STALE'
  | 'MEMORY_PHOTO_UNAVAILABLE'
  | 'ALBUM_NOT_FOUND'
  | 'ALBUM_PHOTO_UNAVAILABLE'
  | 'ALBUM_PHOTO_NOT_FOUND'
  | 'ALBUM_PHOTO_FORBIDDEN'
  | 'ALBUM_PHOTO_REMOVED'
  | 'ALBUM_EDIT_FORBIDDEN'
  | 'ALBUM_VERSION_CONFLICT'
  | 'PLAN_EDIT_FORBIDDEN'
  | 'PLAN_CONFLICT'
  | 'MEMORY_ALREADY_DELETED'
  | 'MEMORY_UNDO_CLOSED'
  | 'MEMORY_DELETION_NOT_ACTIVE'
  | 'MEDIA_DELETION_LINKED'
  | 'ALBUM_PROPOSAL_PENDING'
  | 'ALBUM_PROPOSAL_NOT_FOUND'
  | 'ALBUM_PROPOSAL_DECIDED'
  | 'AVATAR_ITEM_NOT_FOUND'
  | 'AVATAR_VERSION_CONFLICT'
  | 'SHARED_ROOM_VERSION_CONFLICT'
  | 'ROOM_FURNITURE_OWNED'
  | 'SHARED_GARDEN_DATE_CHANGED'
  | 'AVATAR_ASSET_INVALID'
  | 'AVATAR_STARTER_INVALID'
  | 'AVATAR_ALREADY_OWNED'
  | 'AVATAR_ITEM_UNAVAILABLE'
  | 'AVATAR_SLOT_INVALID'
  | 'AVATAR_NOT_OWNED'
  | 'AVATAR_DEFAULTS_UNAVAILABLE'
  | 'EXPORT_TOO_LARGE'
  | 'EXPORT_UNAVAILABLE'
  | 'PUSH_UNAVAILABLE'
  | 'PUSH_DEVICE_LIMIT'
  | 'PUSH_SUBSCRIPTION_NOT_FOUND'

/** `#/components/schemas/ErrorEnvelope` */
export interface ErrorEnvelope {
  error: {
    code: ServerErrorCode | string
    message: string
    details?: Record<string, unknown>
  }
}

export type ReactionEmoji = 'heart' | 'clap' | 'flower'
export type TaskReactionInput = ({ kind: 'emoji'; emoji_code: ReactionEmoji } | { kind: 'text'; body: string }) & { period_date?: string }
export interface TaskReaction {
  id: string; task_id: string; period_date: string | null; author_id: string
  kind: 'emoji' | 'text'; emoji_code: ReactionEmoji | null; body: string | null; created_at: string
}
export interface TaskReactionResponse { mutation: TaskReaction; task: Task }
