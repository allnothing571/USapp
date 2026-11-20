/**
 * 纪念日 store（阶段 2 P2-2，docs/phase2-anniversaries.md）：内存状态，不做浏览器持久化，没有离线写队列。
 *
 * - 会话代数 `sessionGeneration`：reset()（注销、切换账号、引导失败）时递增；在途请求的成功和失败
 *   落地时，只有代数未变且当前用户仍是发起者才生效，否则静默丢弃。
 * - 列表与详情各有请求序号（较新的读取胜出）。列表按不可变的创建顺序 (created_at, id) 倒序游标分页；
 *   所有天数都来自服务端（`as_of_date` 为北京时间日期），前端不计算日期。
 * - 写动作（每个目标至多一个）：新建 'new'、创建者修改 `edit:<id>`、修改申请 `propose:<id>`、
 *   处理申请 `decide:<id>:<proposal>`、首页置顶 'pin'。修改与申请带表单开始时捕获的版本（expected_version）。
 * - P2-1 第 12 节的重试规则：网络错误、5xx（含 503 RESULT_UNKNOWN）或离线 / 本地缺少令牌时保留原键与原内容，
 *   只在用户点击“重试”时用同一个键重发，从不自动重放。一旦某次发送的结果未确认，之后的任何错误
 *   （本地离线或令牌问题、认证失败、业务 4xx）都不能证明原请求失败：保留原键、原内容与“未确认”状态，
 *   直到收到与动作目标一致的同键成功响应。首次发送的明确 4xx 仍结束动作。
 * - 成功响应必须与捕获的目标一致（纪念日 id、version = expected + 1、内容、申请 id 与结果、置顶目标），
 *   否则视为结果未确认。
 * - 注销或身份切换会清空内存中的动作：原请求是否已写入只能重新登录后查看确认（与 P2-1 相同的局限）。
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import {
  createAnniversary,
  decideAnniversaryProposal,
  editAnniversary,
  getAnniversary,
  listAnniversaries,
  proposeAnniversaryEdit,
  setHomeAnniversaryPin,
  type AnniversaryDecisionOutcome,
} from '@/api/anniversaries'
import { sameTerms } from '@/api/anniversary-rules'
import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { isValidNotificationFacts } from '@/api/notifications'
import { describeError } from '@/api/error-messages'
import { newIdempotencyKey } from '@/api/tasks'
import type {
  Anniversary,
  AnniversaryMutationResponse,
  AnniversaryPin,
  AnniversaryPinResponse,
  AnniversaryTerms,
  CivilDate,
} from '@/api/types'
import { useNotificationChoicesStore } from './notification-choices'
import { useSessionStore } from './session'

export type AnniversaryListStatus = 'idle' | 'loading' | 'ready' | 'error'
export type AnniversaryActionStatus = 'submitting' | 'unknown' | 'not_sent'
export type AnniversaryActionKind = 'create' | 'edit' | 'propose' | 'decide' | 'pin'

export interface AnniversaryAction {
  id: string
  kind: AnniversaryActionKind
  anniversaryId?: string
  proposalId?: string
  expectedVersion?: number
  terms?: AnniversaryTerms
  outcome?: AnniversaryDecisionOutcome
  /** Pin target; null clears the pin. */
  pinTarget?: string | null
  /** P2-3b, create / creator edit: the explicit notify_partner captured with the payload. */
  notify?: boolean
  key: string
  status: AnniversaryActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved?: boolean
}

export interface AnniversaryNotice {
  id: string
  ok: boolean
  text: string
}

type AnyResponse = AnniversaryMutationResponse | AnniversaryPinResponse

export type AnniversaryResult =
  | { outcome: 'done'; response: AnyResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

export interface AnniversaryDetailState {
  id: string | null
  status: AnniversaryListStatus
  item: Anniversary | null
  asOfDate: CivilDate
  errorText: string
}

const OUTCOME_OF: Record<AnniversaryDecisionOutcome, 'approved' | 'rejected' | 'withdrawn'> = {
  approve: 'approved', reject: 'rejected', withdraw: 'withdrawn',
}

const DONE_TEXT: Record<AnniversaryActionKind, string> = {
  create: '纪念日已添加。',
  edit: '修改已保存。',
  propose: '修改申请已提交，等待创建者处理。',
  decide: '已处理这条修改申请。',
  pin: '首页展示已更新（只影响你自己的首页）。',
}

const PIN_SUPERSEDED_TEXT = '这次首页设置之前已保存，但之后又被更改过；现在显示的是最新的首页设置。'

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

function termsOf(source: Partial<AnniversaryTerms> | undefined): AnniversaryTerms | null {
  if (!source || typeof source.title !== 'string' || typeof source.date !== 'string' ||
    (source.kind !== 'anniversary' && source.kind !== 'countdown') || typeof source.note !== 'string') return null
  return { title: source.title, date: source.date, kind: source.kind, note: source.note }
}

function isPositiveInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 1
}

/**
 * The caller's current pin at read time. A same-key replay returns the original mutation but the
 * current pin, which may be newer (seq ≥ the mutation's) and point elsewhere or be cleared (null).
 */
function isCurrentPin(pin: unknown, mutationSeq: number): pin is AnniversaryPin {
  if (!pin || typeof pin !== 'object') return false
  const p = pin as Partial<AnniversaryPin>
  const target = p.anniversary_id
  return (target === null || (typeof target === 'string' && target.length > 0)) &&
    isPositiveInteger(p.seq) && p.seq >= mutationSeq &&
    (p.updated_at === null || (typeof p.updated_at === 'string' && p.updated_at.length > 0))
}

/** The response confirms exactly the captured action for this owner; anything else stays unknown. */
export function isUsableAnniversaryResponse(response: unknown, action: AnniversaryAction, owner: string):
  response is AnyResponse {
  if (!response || typeof response !== 'object') return false
  if (action.kind === 'pin') {
    const r = response as Partial<AnniversaryPinResponse>
    // The immutable mutation must be exactly this action; the current pin is checked separately.
    return r.mutation?.action === 'pin' && r.mutation.user_id === owner && isPositiveInteger(r.mutation.seq) &&
      r.mutation.anniversary_id === (action.pinTarget ?? null) && isCurrentPin(r.pin, r.mutation.seq)
  }
  const r = response as Partial<AnniversaryMutationResponse>
  const m = r.mutation
  const item = r.anniversary
  if (!m || !item || typeof item.id !== 'string' || item.id.length === 0 || m.anniversary_id !== item.id) return false
  if (action.kind !== 'create' && item.id !== action.anniversaryId) return false
  const terms = action.terms
  const facts = termsOf(m)
  switch (action.kind) {
    case 'create':
      return m.action === 'create' && m.version === 1 && m.actor_id === owner && !!terms && !!facts &&
        sameTerms(facts, terms) && item.creator_id === owner && isValidNotificationFacts(r.notification, action.notify, true)
    case 'edit':
      return m.action === 'edit' && m.actor_id === owner && m.version === (action.expectedVersion ?? 0) + 1 &&
        !!terms && !!facts && sameTerms(facts, terms) && isValidNotificationFacts(r.notification, action.notify, true)
    case 'propose':
      return m.action === 'propose' && m.proposer_id === owner && m.base_version === action.expectedVersion &&
        typeof m.proposal_id === 'string' && !!terms && !!facts && sameTerms(facts, terms)
    case 'decide': {
      if (m.action !== 'decide' || m.proposal_id !== action.proposalId || m.decided_by !== owner ||
        !action.outcome || m.outcome !== OUTCOME_OF[action.outcome]) return false
      if (action.outcome === 'approve') return typeof m.base_version === 'number' && m.version === m.base_version + 1
      return m.version === null || m.version === undefined
    }
    default:
      return false
  }
}

export const useAnniversariesStore = defineStore('anniversaries', () => {
  const items = ref<Anniversary[]>([])
  const asOfDate = ref<CivilDate>('')
  const status = ref<AnniversaryListStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  const detail = ref<AnniversaryDetailState>({ id: null, status: 'idle', item: null, asOfDate: '', errorText: '' })
  const actions = ref<Record<string, AnniversaryAction>>({})
  const notices = ref<AnniversaryNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0
  let detailSeq = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    listSeq += 1
    detailSeq += 1
    items.value = []
    asOfDate.value = ''
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    detail.value = { id: null, status: 'idle', item: null, asOfDate: '', errorText: '' }
    actions.value = {}
    notices.value = []
  }

  /** Replaces a known item in the list and the open detail with a newer server copy. */
  function absorb(item: Anniversary, itemAsOf: CivilDate): void {
    items.value = items.value.map((a) => (a.id === item.id ? item : a))
    if (detail.value.id === item.id) {
      detail.value = { ...detail.value, status: 'ready', item, asOfDate: itemAsOf, errorText: '' }
    }
  }

  async function load(): Promise<void> {
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    status.value = 'loading'
    errorText.value = ''
    loadingMore.value = false
    moreErrorText.value = ''
    try {
      const response = await listAnniversaries()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      items.value = response.anniversaries
      asOfDate.value = response.as_of_date
      nextCursor.value = response.next_cursor
      status.value = 'ready'
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      items.value = []
      nextCursor.value = null
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  async function loadMore(): Promise<void> {
    if (status.value !== 'ready' || !nextCursor.value || loadingMore.value) return
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    loadingMore.value = true
    moreErrorText.value = ''
    try {
      const response = await listAnniversaries(nextCursor.value)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      // A later page may be computed on a later Beijing date: re-read from the start instead of mixing dates.
      if (response.as_of_date !== asOfDate.value) {
        loadingMore.value = false
        void load()
        return
      }
      const seen = new Set(items.value.map((a) => a.id))
      for (const item of response.anniversaries) if (!seen.has(item.id)) items.value.push(item)
      nextCursor.value = response.next_cursor
      loadingMore.value = false
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  async function openDetail(id: string): Promise<void> {
    const seq = ++detailSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const known = items.value.find((a) => a.id === id) ?? null
    detail.value = { id, status: 'loading', item: known, asOfDate: known ? asOfDate.value : '', errorText: '' }
    try {
      const response = await getAnniversary(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'ready', item: response.anniversary, asOfDate: response.as_of_date, errorText: '' }
    } catch (error) {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'error', item: null, asOfDate: '', errorText: describeError(error) }
    }
  }

  /** Re-reads one item (after a rejection that may mean the copy is stale). */
  async function refreshOne(id: string): Promise<void> {
    const generation = sessionGeneration
    const owner = currentUserId()
    try {
      const response = await getAnniversary(id)
      if (!stillCurrent(generation, owner)) return
      absorb(response.anniversary, response.as_of_date)
    } catch {
      // The notice already reports the rejection; a failed refresh changes nothing.
    }
  }

  function update(id: string, patch: Partial<AnniversaryAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: AnniversaryNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  function keepUnknown(id: string, what: string): AnniversaryResult {
    update(id, { status: 'unknown', errorText: `${what}之前的提交结果仍未确认，请保留原请求重试。` })
    return { outcome: 'unknown' }
  }

  function call(action: AnniversaryAction): Promise<unknown> {
    switch (action.kind) {
      // The captured snapshot is resent unchanged; the choice is never re-read from preferences.
      case 'create':
        return action.notify === undefined ? createAnniversary(action.terms as AnniversaryTerms, action.key)
          : createAnniversary(action.terms as AnniversaryTerms, action.key, action.notify)
      case 'edit':
        return action.notify === undefined
          ? editAnniversary(action.anniversaryId ?? '', action.expectedVersion ?? 0, action.terms as AnniversaryTerms,
            action.key)
          : editAnniversary(action.anniversaryId ?? '', action.expectedVersion ?? 0, action.terms as AnniversaryTerms,
            action.key, action.notify)
      case 'propose':
        return proposeAnniversaryEdit(action.anniversaryId ?? '', action.expectedVersion ?? 0,
          action.terms as AnniversaryTerms, action.key)
      case 'decide':
        return decideAnniversaryProposal(action.anniversaryId ?? '', action.proposalId ?? '',
          action.outcome ?? 'reject', action.key)
      default:
        return setHomeAnniversaryPin(action.pinTarget ?? null, action.key)
    }
  }

  async function send(id: string): Promise<AnniversaryResult> {
    const action = actions.value[id]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }
    update(id, { status: 'submitting', errorText: '' })
    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
      update(id, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }
    let response: AnyResponse
    try {
      const received = await call(action)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableAnniversaryResponse(received, action, owner)) throw new Error('纪念日响应无法确认。')
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (!isUnconfirmedOrUnsent(error)) {
        // A retry error alone does not prove what happened to an earlier request.
        if (action.unresolved) return keepUnknown(id, `本次重试未确认原结果（${describeError(error)}）。`)
        end(id)
        notify({ id, ok: false, text: describeError(error) })
        if (action.anniversaryId) void refreshOne(action.anniversaryId)
        return { outcome: 'rejected', errorCode: error instanceof ApiError ? error.code : null }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复保存。' })
      return { outcome: 'unknown' }
    }
    end(id)
    // Remembered defaults for FUTURE forms only.
    if (action.notify !== undefined) void useNotificationChoicesStore().refresh()
    // A replayed pin may have been superseded since (another device): the current pin decides.
    const currentPin = action.kind === 'pin' ? (response as AnniversaryPinResponse).pin.anniversary_id : null
    const superseded = action.kind === 'pin' && currentPin !== (action.pinTarget ?? null)
    notify({ id, ok: true, text: superseded ? PIN_SUPERSEDED_TEXT : DONE_TEXT[action.kind] })
    if (action.kind === 'create') {
      void load()
    } else if (action.kind === 'pin') {
      const target = currentPin
      const mark = (a: Anniversary): Anniversary => ({ ...a, pinned_by_me: a.id === target })
      items.value = items.value.map(mark)
      if (detail.value.item) detail.value = { ...detail.value, item: mark(detail.value.item) }
    } else {
      const r = response as AnniversaryMutationResponse
      absorb(r.anniversary, r.as_of_date)
    }
    return { outcome: 'done', response }
  }

  function register(action: Omit<AnniversaryAction, 'key' | 'status' | 'errorText'>): Promise<AnniversaryResult> {
    if (actions.value[action.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  /** Terms are normalised by the view; the server re-validates them. */
  function create(terms: AnniversaryTerms, notifyPartner?: boolean): Promise<AnniversaryResult> {
    return register({ id: 'new', kind: 'create', terms: { ...terms }, notify: notifyPartner })
  }

  /** `expectedVersion` is the version captured when the form started. */
  function edit(anniversaryId: string, expectedVersion: number, terms: AnniversaryTerms,
    notifyPartner?: boolean): Promise<AnniversaryResult> {
    return register({ id: `edit:${anniversaryId}`, kind: 'edit', anniversaryId, expectedVersion, terms: { ...terms },
      notify: notifyPartner })
  }

  function propose(anniversaryId: string, expectedVersion: number, terms: AnniversaryTerms): Promise<AnniversaryResult> {
    return register({ id: `propose:${anniversaryId}`, kind: 'propose', anniversaryId, expectedVersion, terms: { ...terms } })
  }

  function decide(anniversaryId: string, proposalId: string, outcome: AnniversaryDecisionOutcome): Promise<AnniversaryResult> {
    return register({ id: `decide:${anniversaryId}:${proposalId}`, kind: 'decide', anniversaryId, proposalId, outcome })
  }

  function pin(anniversaryId: string | null): Promise<AnniversaryResult> {
    return register({ id: 'pin', kind: 'pin', pinTarget: anniversaryId })
  }

  /** Resend an unknown or unsent action with its original key and payload (user click only). */
  function retry(id: string): Promise<AnniversaryResult> {
    return send(id)
  }

  return {
    items, asOfDate, status, errorText, nextCursor, loadingMore, moreErrorText, detail, actions, notices,
    reset, load, loadMore, openDetail, create, edit, propose, decide, pin, retry, dismissNotice,
  }
})
