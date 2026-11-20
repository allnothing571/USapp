/**
 * Photo memories store (P3-1c, docs/phase3-plan.md Section 23): memory only, nothing persisted, no offline write queue.
 *
 * - Identity: `reset()` raises the generation on logout, account switch or failed bootstrap. A late result lands only if the
 *   generation and the user are unchanged, so an old account's answer cannot touch the new one.
 * - Reads: the list, detail and review each have a request sequence; the newer read wins, and leaving the page raises the
 *   detail and review sequences so their late answers are dropped.
 * - `uploadsEnabled`: true or false as the timeline last said; null while unknown or unavailable. Existing memories stay
 *   readable in every case.
 * - Writes, at most one per target: 'new', `edit:<id>`, `propose:<id>`, `decide:<id>:<proposal>`. As the anniversary store: a network
 *   error, 5xx or malformed response keeps the original key and captured payload, and only a user click resends them. Once a
 *   send is unconfirmed, no later error proves it failed, so the action stays until a same-key success arrives. A first
 *   send's definite 4xx ends the action.
 * - A success must match the captured target, otherwise it is unconfirmed. A replay returns the original facts and today's memory:
 *   so an approval replay shows its confirmed version and the current state, and duplicates nothing.
 * - Logout or an identity switch clears these actions from memory. Whether a request was written can then only be seen by
 *   reading again after signing in.
 * - Edit-request history: summaries read from the server each time the detail opens or changes, with their own request
 *   sequence; a historical comparison has its own sequence too, separate from the pending review. Both are dropped on leaving
 *   the page, on an identity change, when the memory's detail cannot be read, and when the memory is deleted. Nothing about it
 *   is kept in browser storage, and it never decides anything: the pending request's controls are the existing ones.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import {
  createMemory,
  decideMemoryProposal,
  editMemory,
  getMemory,
  getMemoryProposal,
  listMemories,
  listMemoryProposals,
  proposeMemoryEdit,
  type MemoryDecisionOutcome,
} from '@/api/memories'
import { isMemory, isMemoryProposal, isMemoryProposalPage, sameMemoryTerms, termsOfContent } from '@/api/memory-rules'
import { isValidNotificationFacts } from '@/api/notifications'
import { newIdempotencyKey } from '@/api/tasks'
import type { Memory, MemoryMutationResponse, MemoryProposal, MemoryProposalSummary, MemoryTerms } from '@/api/types'
import { useMediaCapabilityStore } from './media-capability'
import { useSessionStore } from './session'

export type MemoryLoadStatus = 'idle' | 'loading' | 'ready' | 'error'
export type MemoryActionStatus = 'submitting' | 'unknown' | 'not_sent'
export type MemoryActionKind = 'create' | 'edit' | 'propose' | 'decide'

export interface MemoryAction {
  id: string
  kind: MemoryActionKind
  memoryId?: string
  proposalId?: string
  expectedVersion?: number
  terms?: MemoryTerms
  outcome?: MemoryDecisionOutcome
  /** D20: a direct create's explicit notify_partner, captured with the key and never changed by a retry. */
  notify?: boolean
  key: string
  status: MemoryActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved?: boolean
}

export interface MemoryNotice {
  id: string
  ok: boolean
  text: string
}

export type MemoryResult =
  | { outcome: 'done'; response: MemoryMutationResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

export interface MemoryDetailState {
  id: string | null
  status: MemoryLoadStatus
  item: Memory | null
  errorText: string
}

export interface MemoryReviewState {
  proposalId: string | null
  status: MemoryLoadStatus
  proposal: MemoryProposal | null
  errorText: string
}

export interface MemoryHistoryState {
  memoryId: string | null
  status: MemoryLoadStatus
  items: MemoryProposalSummary[]
  nextCursor: string | null
  /** The memory's latest version as the newest page read it; what an old approval is compared with. */
  currentVersion: number | null
  errorText: string
  loadingMore: boolean
  moreErrorText: string
}

const OUTCOME_OF = { approve: 'approved', reject: 'rejected', withdraw: 'withdrawn' } as const

const DONE_TEXT: Record<MemoryActionKind, string> = {
  create: '回忆已保存。',
  edit: '修改已保存。',
  propose: '修改申请已提交，等待创建者处理。',
  decide: '已处理这条修改申请。',
}

const DECIDE_TEXT: Record<MemoryDecisionOutcome, string> = {
  approve: '已同意修改，回忆已更新。',
  reject: '已拒绝这条修改申请。',
  withdraw: '已撤回这条修改申请。',
}

const MALFORMED = '服务器的响应无法确认，请重试。'

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

/** The response confirms exactly the captured action for this owner; anything else stays unknown. */
export function isUsableMemoryResponse(response: unknown, action: MemoryAction, owner: string): response is MemoryMutationResponse {
  if (!response || typeof response !== 'object') return false
  const r = response as Partial<MemoryMutationResponse>
  const m = r.mutation
  if (!m || !isMemory(r.memory) || m.memory_id !== r.memory.id) return false
  const item = r.memory
  if (action.kind !== 'create' && item.id !== action.memoryId) return false
  // A replay carries today's memory, which may be newer but never older than what it confirms.
  const notOlder = typeof m.version === 'number' && item.version >= m.version
  // A newer memory may legitimately differ, so content is compared only at the mutation's own version.
  const contentAgrees = notOlder && (item.version !== m.version || (!!action.terms && sameMemoryTerms(termsOfContent(item), action.terms)))
  switch (action.kind) {
    case 'create':
      // The original decision must agree with the captured choice (absent or not requested when none was captured).
      return m.action === 'create' && m.version === 1 && m.actor_id === owner && item.creator_id === owner && contentAgrees &&
        isValidNotificationFacts(r.notification, action.notify, true)
    case 'edit':
      return m.action === 'edit' && m.actor_id === owner && m.version === (action.expectedVersion ?? 0) + 1 && contentAgrees
    case 'propose':
      return m.action === 'propose' && m.proposer_id === owner && m.base_version === action.expectedVersion && typeof m.proposal_id === 'string' && m.proposal_id.length > 0
    case 'decide': {
      if (m.action !== 'decide' || m.proposal_id !== action.proposalId || m.decided_by !== owner ||
        !action.outcome || m.outcome !== OUTCOME_OF[action.outcome]) return false
      if (action.outcome === 'approve') return typeof m.base_version === 'number' && m.version === m.base_version + 1 && notOlder
      return m.version === null || m.version === undefined
    }
    default:
      return false
  }
}

const EMPTY_DETAIL: MemoryDetailState = { id: null, status: 'idle', item: null, errorText: '' }
const EMPTY_REVIEW: MemoryReviewState = { proposalId: null, status: 'idle', proposal: null, errorText: '' }
function emptyHistory(): MemoryHistoryState {
  return { memoryId: null, status: 'idle', items: [], nextCursor: null, currentVersion: null, errorText: '', loadingMore: false, moreErrorText: '' }
}

export const useMemoriesStore = defineStore('memories', () => {
  const items = ref<Memory[]>([])
  const status = ref<MemoryLoadStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  const uploadsEnabled = ref<boolean | null>(null)
  const detail = ref<MemoryDetailState>({ ...EMPTY_DETAIL })
  const review = ref<MemoryReviewState>({ ...EMPTY_REVIEW })
  const history = ref<MemoryHistoryState>(emptyHistory())
  const historyReview = ref<MemoryReviewState>({ ...EMPTY_REVIEW })
  const actions = ref<Record<string, MemoryAction>>({})
  const notices = ref<MemoryNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0
  let detailSeq = 0
  let reviewSeq = 0
  let historySeq = 0
  let historyReviewSeq = 0
  const hidden = new Set<string>()

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    hidden.clear()
    sessionGeneration += 1
    listSeq += 1
    detailSeq += 1
    reviewSeq += 1
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    uploadsEnabled.value = null
    detail.value = { ...EMPTY_DETAIL }
    review.value = { ...EMPTY_REVIEW }
    clearHistory()
    actions.value = {}
    notices.value = []
  }

  function readUploadsEnabled(value: unknown): void {
    uploadsEnabled.value = typeof value === 'boolean' ? value : null
  }

  /** The server just said uploads are off: the view explains it, without another read. */
  function markUploadsDisabled(): void {
    uploadsEnabled.value = false
  }

  function isPage(response: unknown): response is { memories: Memory[]; next_cursor: string | null; uploads_enabled?: unknown } {
    if (!response || typeof response !== 'object') return false
    const r = response as { memories?: unknown; next_cursor?: unknown }
    return Array.isArray(r.memories) && r.memories.every(isMemory) && (r.next_cursor === null || typeof r.next_cursor === 'string')
  }

  async function load(): Promise<void> {
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const capability = useMediaCapabilityStore().token()
    status.value = 'loading'
    errorText.value = ''
    loadingMore.value = false
    moreErrorText.value = ''
    try {
      const response = await listMemories()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      items.value = response.memories
      nextCursor.value = response.next_cursor
      readUploadsEnabled(response.uploads_enabled)
      useMediaCapabilityStore().observe(response.uploads_enabled, capability)
      status.value = 'ready'
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      items.value = []
      nextCursor.value = null
      uploadsEnabled.value = null
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
      const response = await listMemories(nextCursor.value)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      // An edit that moved a date can show a row twice during a walk: the first copy stays.
      const seen = new Set(items.value.map((m) => m.id))
      items.value = [...items.value, ...response.memories.filter((m) => !seen.has(m.id))]
      nextCursor.value = response.next_cursor
      readUploadsEnabled(response.uploads_enabled)
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
    const known = items.value.find((m) => m.id === id) ?? null
    detail.value = { id, status: 'loading', item: known, errorText: '' }
    try {
      const response = await getMemory(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      if (!response || !isMemory(response.memory) || response.memory.id !== id) throw new Error(MALFORMED)
      hidden.delete(id)
      detail.value = { id, status: 'ready', item: response.memory, errorText: '' }
    } catch (error) {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'error', item: null, errorText: describeError(error) }
      // Unreadable now, perhaps deleted: its history goes with it until the memory reads again.
      if (history.value.memoryId === id) clearHistory()
    }
  }

  /** Leaving the page: late answers for it are dropped. */
  function closeDetail(): void {
    detailSeq += 1
    reviewSeq += 1
    detail.value = { ...EMPTY_DETAIL }
    review.value = { ...EMPTY_REVIEW }
    clearHistory()
  }

  function clearHistory(): void {
    historySeq += 1
    historyReviewSeq += 1
    history.value = emptyHistory()
    historyReview.value = { ...EMPTY_REVIEW }
  }

  /** The first page of one memory's edit requests. What was shown for the same memory stays until the answer arrives. */
  async function loadHistory(memoryId: string): Promise<void> {
    const seq = ++historySeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const same = history.value.memoryId === memoryId
    history.value = {
      ...emptyHistory(), memoryId, status: 'loading', items: same ? history.value.items : [], currentVersion: same ? history.value.currentVersion : null,
    }
    try {
      const response = await listMemoryProposals(memoryId)
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      if (!isMemoryProposalPage(response, memoryId)) throw new Error(MALFORMED)
      history.value = {
        memoryId, status: 'ready', items: response.proposals, nextCursor: response.next_cursor, currentVersion: response.current_version,
        errorText: '', loadingMore: false, moreErrorText: '',
      }
    } catch (error) {
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      history.value = { ...emptyHistory(), memoryId, status: 'error', errorText: describeError(error) }
    }
  }

  async function loadMoreHistory(): Promise<void> {
    const current = history.value
    const memoryId = current.memoryId
    if (!memoryId || current.status !== 'ready' || !current.nextCursor || current.loadingMore) return
    const seq = ++historySeq
    const generation = sessionGeneration
    const owner = currentUserId()
    history.value = { ...current, loadingMore: true, moreErrorText: '' }
    try {
      const response = await listMemoryProposals(memoryId, current.nextCursor)
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      if (!isMemoryProposalPage(response, memoryId)) throw new Error(MALFORMED)
      // Requests never change order, so a repeat cannot happen; the first copy would stay if it did.
      const seen = new Set(history.value.items.map((p) => p.id))
      history.value = {
        ...history.value, items: [...history.value.items, ...response.proposals.filter((p) => !seen.has(p.id))],
        nextCursor: response.next_cursor, currentVersion: response.current_version, loadingMore: false,
      }
    } catch (error) {
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      history.value = { ...history.value, loadingMore: false, moreErrorText: describeError(error) }
    }
  }

  /** The comparison of one listed request, read from the proposal record, apart from the pending review. */
  async function openHistoryReview(memoryId: string, proposalId: string): Promise<void> {
    const seq = ++historyReviewSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    historyReview.value = { proposalId, status: 'loading', proposal: null, errorText: '' }
    try {
      const response = await getMemoryProposal(memoryId, proposalId)
      if (seq !== historyReviewSeq || !stillCurrent(generation, owner) || history.value.memoryId !== memoryId) return
      if (!response || !isMemoryProposal(response.proposal, memoryId, proposalId)) throw new Error(MALFORMED)
      historyReview.value = { proposalId, status: 'ready', proposal: response.proposal, errorText: '' }
    } catch (error) {
      if (seq !== historyReviewSeq || !stillCurrent(generation, owner) || history.value.memoryId !== memoryId) return
      historyReview.value = { proposalId, status: 'error', proposal: null, errorText: describeError(error) }
    }
  }

  function closeHistoryReview(): void {
    historyReviewSeq += 1
    historyReview.value = { ...EMPTY_REVIEW }
  }

  async function loadReview(memoryId: string, proposalId: string): Promise<void> {
    const seq = ++reviewSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    review.value = { proposalId, status: 'loading', proposal: null, errorText: '' }
    try {
      const response = await getMemoryProposal(memoryId, proposalId)
      if (seq !== reviewSeq || !stillCurrent(generation, owner)) return
      if (!response || !isMemoryProposal(response.proposal, memoryId, proposalId)) throw new Error(MALFORMED)
      review.value = { proposalId, status: 'ready', proposal: response.proposal, errorText: '' }
    } catch (error) {
      if (seq !== reviewSeq || !stillCurrent(generation, owner)) return
      review.value = { proposalId, status: 'error', proposal: null, errorText: describeError(error) }
    }
  }

  function clearReview(): void {
    reviewSeq += 1
    review.value = { ...EMPTY_REVIEW }
  }

  function absorb(item: Memory): void {
    if (hidden.has(item.id)) return
    items.value = items.value.map((m) => (m.id === item.id ? item : m))
    if (detail.value.id === item.id) detail.value = { id: item.id, status: 'ready', item, errorText: '' }
  }

  function refreshAfterMemoryChange(id: string, hide: boolean): void {
    listSeq++
    detailSeq++
    reviewSeq++
    review.value = { ...EMPTY_REVIEW }
    if (hide) {
      hidden.add(id)
      if (history.value.memoryId === id) clearHistory()
      items.value = items.value.filter(item => item.id !== id)
      if (detail.value.id === id) detail.value = { id, status: 'error', item: null, errorText: '回忆已不可用。可在最近删除的回忆中查看你自己的恢复记录。' }
    }
    void load()
    if (detail.value.id === id) void openDetail(id)
  }

  /** Re-reads one memory after a rejection that may mean the copy is stale. */
  async function refreshOne(id: string): Promise<void> {
    const generation = sessionGeneration
    const owner = currentUserId()
    try {
      const response = await getMemory(id)
      if (!stillCurrent(generation, owner) || !response || !isMemory(response.memory)) return
      absorb(response.memory)
    } catch {
      // The notice already reports the rejection; a failed refresh changes nothing.
    }
  }

  /** D6: a photo was deleted or restored. The open memory and any review come back from the server, placeholders included. */
  async function refreshAfterPhotoChange(): Promise<void> {
    const id = detail.value.id
    if (!id) return
    const generation = sessionGeneration
    const owner = currentUserId()
    if (review.value.proposalId) void loadReview(id, review.value.proposalId)
    if (historyReview.value.proposalId && history.value.memoryId === id) void openHistoryReview(id, historyReview.value.proposalId)
    const seq = ++detailSeq
    try {
      const response = await getMemory(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      if (!response || !isMemory(response.memory) || response.memory.id !== id) throw new Error(MALFORMED)
      absorb(response.memory)
    } catch {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      notices.value = [{ id: `refresh:${id}`, ok: false, text: '回忆没有刷新成功，照片状态可能不是最新的，请重新打开这个页面。' }, ...notices.value.filter((n) => n.id !== `refresh:${id}`)]
    }
  }

  function update(id: string, patch: Partial<MemoryAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: MemoryNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  function keepUnknown(id: string, what: string): MemoryResult {
    update(id, { status: 'unknown', errorText: `${what}之前的提交结果仍未确认，请保留原请求重试。` })
    return { outcome: 'unknown' }
  }

  function call(action: MemoryAction): Promise<unknown> {
    const terms = action.terms as MemoryTerms
    switch (action.kind) {
      case 'create':
        return createMemory(terms, action.key, action.notify)
      case 'edit':
        return editMemory(action.memoryId ?? '', action.expectedVersion ?? 0, terms, action.key)
      case 'propose':
        return proposeMemoryEdit(action.memoryId ?? '', action.expectedVersion ?? 0, terms, action.key)
      default:
        return decideMemoryProposal(action.memoryId ?? '', action.proposalId ?? '', action.outcome ?? 'reject', action.key)
    }
  }

  async function send(id: string): Promise<MemoryResult> {
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
    let response: MemoryMutationResponse
    try {
      const received = await call(action)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableMemoryResponse(received, action, owner)) throw new Error(MALFORMED)
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (!isUnconfirmedOrUnsent(error)) {
        // A retry's error alone does not prove what happened to an earlier request.
        if (action.unresolved) return keepUnknown(id, `本次重试未确认原结果（${describeError(error)}）。`)
        end(id)
        notify({ id, ok: false, text: describeError(error) })
        if (action.memoryId) void refreshOne(action.memoryId)
        return { outcome: 'rejected', errorCode: error instanceof ApiError ? error.code : null }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复保存。' })
      return { outcome: 'unknown' }
    }
    end(id)
    notify({ id, ok: true, text: action.kind === 'decide' && action.outcome ? DECIDE_TEXT[action.outcome] : DONE_TEXT[action.kind] })
    if (action.kind === 'create') {
      void load()
    } else {
      absorb(response.memory)
      // A date change can move this row: the timeline re-reads when it is next opened.
    }
    if (action.kind === 'decide') clearReview()
    return { outcome: 'done', response }
  }

  function register(action: Omit<MemoryAction, 'key' | 'status' | 'errorText'>): Promise<MemoryResult> {
    if (actions.value[action.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  /** `notifyPartner` (D20) is sent only when given; the new-memory page always gives it. */
  function create(terms: MemoryTerms, notifyPartner?: boolean): Promise<MemoryResult> {
    return register({ id: 'new', kind: 'create', terms: { ...terms, media_ids: [...terms.media_ids] },
      ...(notifyPartner === undefined ? {} : { notify: notifyPartner }) })
  }

  /** `expectedVersion` is the version captured when the form opened. */
  function edit(memoryId: string, expectedVersion: number, terms: MemoryTerms): Promise<MemoryResult> {
    return register({ id: `edit:${memoryId}`, kind: 'edit', memoryId, expectedVersion, terms: { ...terms, media_ids: [...terms.media_ids] } })
  }

  function propose(memoryId: string, expectedVersion: number, terms: MemoryTerms): Promise<MemoryResult> {
    return register({ id: `propose:${memoryId}`, kind: 'propose', memoryId, expectedVersion, terms: { ...terms, media_ids: [...terms.media_ids] } })
  }

  function decide(memoryId: string, proposalId: string, outcome: MemoryDecisionOutcome): Promise<MemoryResult> {
    return register({ id: `decide:${memoryId}:${proposalId}`, kind: 'decide', memoryId, proposalId, outcome })
  }

  /** Resend an unknown or unsent action with its original key and payload (user click only). */
  function retry(id: string): Promise<MemoryResult> {
    return send(id)
  }

  return {
    items, status, errorText, nextCursor, loadingMore, moreErrorText, uploadsEnabled, detail, review, history, historyReview, actions, notices,
    reset, load, loadMore, markUploadsDisabled, openDetail, closeDetail, loadReview, refreshAfterPhotoChange, refreshAfterMemoryChange, create, edit, propose, decide, retry, dismissNotice,
    loadHistory, loadMoreHistory, openHistoryReview, closeHistoryReview,
  }
})
