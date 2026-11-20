/**
 * 奖励商店 store（阶段 1 M7 商品，M8 直接兑换与履约）：内存状态，不做浏览器持久化。
 *
 * - 会话代数 `sessionGeneration`：reset()（注销、切换账号）时递增；在途请求落地时
 *   只有代数未变且当前用户仍是发起者才生效，否则静默丢弃。
 * - 写动作（上架 / 修改 / 下架）：每件商品至多一个，上架用键 'new'。网络错误、5xx
 *   （含 503 RESULT_UNKNOWN）或离线 / 本地缺少令牌时保留原请求标识与原请求体，
 *   只在用户点击“重试”时用同一个键重发；结果未知后，后续错误仍保留原键。
 *   从不自动重放写请求。修改与下架携带 expected_version，过期时服务端返回 409。
 * - M8：兑换（键 `redeem:<商品>`）携带用户确认时捕获的版本；只有确认扣款成功后才刷新钱包，
 *   钱包刷新失败不会把已确认的兑换改成失败。确认履约（键 `fulfill:<兑换>`）不触碰钱包。
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import {
  archiveRewardProduct, createRewardProduct, editRewardProduct, fulfillRedemption, listRedemptions, listRewardProducts,
  redeemRewardProduct, transitionRedemption,
} from '@/api/rewards'
import { newIdempotencyKey } from '@/api/tasks'
import type {
  Redemption, RedemptionTransition, RedemptionMutationResponse, RewardProduct, RewardProductMutationResponse, RewardProductTerms,
} from '@/api/types'
import { useSessionStore } from './session'
import { useWalletStore } from './wallet'

export type RewardListStatus = 'idle' | 'loading' | 'ready' | 'error'
export type RewardActionStatus = 'submitting' | 'unknown' | 'not_sent'

export interface RewardAction {
  /** 'new' for a creation, the product id for edit/archive, 'redeem:<product>' or 'fulfill:<redemption>'. */
  id: string
  kind: 'create' | 'edit' | 'archive' | 'redeem' | 'fulfill' | RedemptionTransition
  proposalId?: string
  /** Product (edit/archive/redeem) or redemption (fulfill) the request targets. */
  targetId?: string
  title: string
  terms?: RewardProductTerms
  expectedVersion?: number
  key: string
  status: RewardActionStatus
  errorText: string
  /** An earlier attempt with this exact key may have committed. */
  unresolved?: boolean
}

/** The immutable identity/version a mutation targets (captured by the view, never re-read). */
export type RewardTarget = Pick<RewardProduct, 'id' | 'title' | 'version'>

export interface RewardNotice {
  id: string
  title: string
  ok: boolean
  text: string
}

export type RewardResult =
  | { outcome: 'done'; response: RewardProductMutationResponse | RedemptionMutationResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

function isUsableMutation(response: unknown, action: RewardAction):
  response is RewardProductMutationResponse | RedemptionMutationResponse {
  if (!response || typeof response !== 'object') return false
  if (action.kind !== 'create' && action.kind !== 'edit' && action.kind !== 'archive') {
    const r = response as Partial<RedemptionMutationResponse>
    if (r.mutation?.action !== action.kind || typeof r.redemption?.id !== 'string' || !r.redemption.id ||
        r.mutation.redemption_id !== r.redemption.id) return false
    return action.kind === 'redeem'
      ? r.redemption.product_id === action.targetId && r.redemption.product_version === action.expectedVersion
      : r.redemption.id === action.targetId
  }
  const r = response as Partial<RewardProductMutationResponse>
  return r.mutation?.action === action.kind && Number.isInteger(r.mutation.version) &&
    typeof r.product?.id === 'string' && r.product.id.length > 0 &&
    (action.kind === 'create' || r.product.id === (action.targetId ?? action.id))
}

const DONE_TEXT: Record<RewardAction['kind'], string> = {
  create: '已上架，对方会在奖励商店里看到它。',
  edit: '已保存修改。',
  archive: '已下架，对方不再看到它。',
  redeem: '已兑换并扣除小红花，等待对方履约。',
  fulfill: '已确认履约。',
  confirm: '已同意申请并扣款，等待履约。', reject: '已拒绝申请。', withdraw: '已撤回申请。',
  cancel_propose: '已提出取消，等待对方同意；履约前才可取消。',
  cancel_approve: '已同意取消并原额退还小红花。', cancel_reject: '已拒绝取消，兑换仍待履约。',
  cancel_withdraw: '已撤回取消提议。',
}

export const useRewardsStore = defineStore('rewards', () => {
  const products = ref<RewardProduct[]>([])
  const status = ref<RewardListStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  // M8 redemption history (both parties), paginated like products.
  const redemptions = ref<Redemption[]>([])
  const redemptionStatus = ref<RewardListStatus>('idle')
  const redemptionErrorText = ref('')
  const redemptionCursor = ref<string | null>(null)
  const redemptionLoadingMore = ref(false)
  const actions = ref<Record<string, RewardAction>>({})
  const notices = ref<RewardNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0
  let redemptionSeq = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    listSeq += 1
    redemptionSeq += 1
    redemptions.value = []
    redemptionStatus.value = 'idle'
    redemptionErrorText.value = ''
    redemptionCursor.value = null
    redemptionLoadingMore.value = false
    products.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    actions.value = {}
    notices.value = []
  }

  function appendUnique(items: RewardProduct[]): void {
    const seen = new Set(products.value.map((p) => p.id))
    for (const item of items) {
      if (!seen.has(item.id)) {
        seen.add(item.id)
        products.value.push(item)
      }
    }
  }

  /** First page (also "refresh"). A newer load or session change discards older responses. */
  async function load(): Promise<void> {
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    status.value = 'loading'
    errorText.value = ''
    loadingMore.value = false
    moreErrorText.value = ''
    try {
      const response = await listRewardProducts()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      products.value = []
      appendUnique(response.products)
      nextCursor.value = response.next_cursor
      status.value = 'ready'
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      products.value = []
      nextCursor.value = null
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  /** Next page, appended without duplicates; a newer load() invalidates it. */
  async function loadMore(): Promise<void> {
    if (status.value !== 'ready' || !nextCursor.value || loadingMore.value) return
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const cursor = nextCursor.value
    loadingMore.value = true
    moreErrorText.value = ''
    try {
      const response = await listRewardProducts(cursor)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      appendUnique(response.products)
      nextCursor.value = response.next_cursor
      loadingMore.value = false
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  /** First page of the caller's redemptions (as redeemer or creator). */
  async function loadRedemptions(): Promise<void> {
    const seq = ++redemptionSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    redemptionStatus.value = 'loading'
    redemptionErrorText.value = ''
    redemptionLoadingMore.value = false
    try {
      const response = await listRedemptions()
      if (seq !== redemptionSeq || !stillCurrent(generation, owner)) return
      redemptions.value = response.redemptions
      redemptionCursor.value = response.next_cursor
      redemptionStatus.value = 'ready'
    } catch (error) {
      if (seq !== redemptionSeq || !stillCurrent(generation, owner)) return
      redemptions.value = []
      redemptionCursor.value = null
      redemptionStatus.value = 'error'
      redemptionErrorText.value = describeError(error)
    }
  }

  async function loadMoreRedemptions(): Promise<void> {
    if (redemptionStatus.value !== 'ready' || !redemptionCursor.value || redemptionLoadingMore.value) return
    const seq = ++redemptionSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    redemptionLoadingMore.value = true
    try {
      const response = await listRedemptions(redemptionCursor.value)
      if (seq !== redemptionSeq || !stillCurrent(generation, owner)) return
      const seen = new Set(redemptions.value.map((r) => r.id))
      for (const item of response.redemptions) if (!seen.has(item.id)) redemptions.value.push(item)
      redemptionCursor.value = response.next_cursor
      redemptionLoadingMore.value = false
    } catch (error) {
      if (seq !== redemptionSeq || !stillCurrent(generation, owner)) return
      redemptionLoadingMore.value = false
      redemptionErrorText.value = describeError(error)
    }
  }

  function update(id: string, patch: Partial<RewardAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: RewardNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  async function send(id: string): Promise<RewardResult> {
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
      if (action.unresolved) {
        update(id, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请保留原请求重试。` })
        return { outcome: 'unknown' }
      }
      update(id, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }
    let response: RewardProductMutationResponse | RedemptionMutationResponse
    const target = action.targetId ?? id
    try {
      const received = action.kind === 'create' && action.terms
        ? await createRewardProduct(action.terms, action.key)
        : action.kind === 'edit' && action.terms
          ? await editRewardProduct(target, { ...action.terms, expected_version: action.expectedVersion ?? 0 }, action.key)
          : action.kind === 'archive'
            ? await archiveRewardProduct(target, action.expectedVersion ?? 0, action.key)
            : action.kind === 'redeem'
              ? await redeemRewardProduct(target, action.expectedVersion ?? 0, action.key)
              : action.kind === 'fulfill'
                ? await fulfillRedemption(target, action.key)
                : await transitionRedemption(target, action.kind as RedemptionTransition, action.proposalId, action.key)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableMutation(received, action)) throw new Error('奖励响应无法确认。')
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) {
          update(id, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的操作结果仍未确认，请保留原请求重试。` })
          return { outcome: 'unknown' }
        }
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (!isUnconfirmedOrUnsent(error)) {
        if (action.unresolved) {
          update(id, { status: 'unknown', errorText: `本次重试未确认原结果（${describeError(error)}）。请保留原请求重试，或先核对兑换记录。` })
          return { outcome: 'unknown' }
        }
        end(id)
        notify({ id, title: action.title, ok: false, text: describeError(error) })
        refreshAfter(action.kind)
        return { outcome: 'rejected', errorCode: error instanceof ApiError ? error.code : null }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复处理。' })
      return { outcome: 'unknown' }
    }
    end(id)
    notify({ id, title: action.title, ok: true, text: action.kind === 'redeem' && 'redemption' in response && response.redemption.redemption_mode === 'request'
      ? '已提交申请，尚未扣款，等待对方确认。' : DONE_TEXT[action.kind] })
    refreshAfter(action.kind)
    // Only a confirmed debit refreshes the wallet; a refresh failure stays in the wallet store.
    if ('redemption' in response && ((response.mutation.debited_flowers ?? 0) > 0 || (response.mutation.refunded_flowers ?? 0) > 0)) void useWalletStore().load()
    return { outcome: 'done', response }
  }

  function refreshAfter(kind: RewardAction['kind']): void {
    if (kind !== 'create' && kind !== 'edit' && kind !== 'archive') {
      void loadRedemptions()
      if (kind === 'redeem') void load()
    } else {
      void load()
    }
  }

  function register(action: Omit<RewardAction, 'key' | 'status' | 'errorText'>): Promise<RewardResult> {
    if (actions.value[action.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  function create(terms: RewardProductTerms): Promise<RewardResult> {
    return register({ id: 'new', kind: 'create', title: terms.title, terms })
  }

  /** The target's version is the one the user saw when starting; a newer version yields a 409 notice. */
  function edit(target: RewardTarget, terms: RewardProductTerms): Promise<RewardResult> {
    return register({ id: target.id, kind: 'edit', title: target.title, terms, expectedVersion: target.version })
  }

  function archive(target: RewardTarget): Promise<RewardResult> {
    return register({ id: target.id, kind: 'archive', title: target.title, expectedVersion: target.version })
  }

  /** M8: redeem the version the user confirmed (captured when the confirmation opened). */
  function redeem(target: RewardTarget): Promise<RewardResult> {
    return register({ id: `redeem:${target.id}`, kind: 'redeem', title: target.title, targetId: target.id,
      expectedVersion: target.version })
  }

  function transition(redemption: Pick<Redemption, 'id' | 'title'>, kind: RedemptionTransition,
    proposalId?: string): Promise<RewardResult> {
    return register({ id: `${kind}:${redemption.id}`, kind, title: redemption.title,
      targetId: redemption.id, proposalId })
  }

  /** M8: the creator confirms a pending redemption as fulfilled. */
  function fulfill(redemption: Pick<Redemption, 'id' | 'title'>): Promise<RewardResult> {
    return register({ id: `fulfill:${redemption.id}`, kind: 'fulfill', title: redemption.title, targetId: redemption.id })
  }

  /** Resend an unknown or unsent action with its original key and body (user click only). */
  function retry(id: string): Promise<RewardResult> {
    return send(id)
  }

  return {
    products, status, errorText, nextCursor, loadingMore, moreErrorText, actions, notices,
    redemptions, redemptionStatus, redemptionErrorText, redemptionCursor, redemptionLoadingMore,
    reset, load, loadMore, loadRedemptions, loadMoreRedemptions, create, edit, archive, redeem, fulfill, transition, retry,
    dismissNotice,
  }
})
