/**
 * Shared albums store (P3-2c): memory only, nothing persisted, no offline write queue. Modelled on the memories store.
 *
 * - Identity: `reset()` raises the generation on logout, account switch or failed bootstrap. A late result lands only if the
 *   generation and the user are unchanged, so an old account's answer cannot touch the new one.
 * - Reads: the list, detail, photos and review each have a request sequence; the newer read wins, a write's answer outranks a read
 *   already in flight, and leaving the page drops late answers.
 * - `uploadsEnabled`: true or false as the album list last said; null while unknown. Albums stay readable and writable in every
 *   case: only choosing new files depends on it.
 * - Writes, at most one per target, and at most one in flight per album, so one member's own answers cannot arrive out of order:
 *   'new', `add:<id>`, `remove:<id>:<media>`, `cover:<id>`, `rename:<id>`, `propose:<id>`, `decide:<id>:<proposal>`. A network error,
 *   5xx or malformed response keeps the original key and captured payload, and only a user click resends them. Once a send is
 *   unconfirmed, no later error proves it failed, so the action stays until a same-key success arrives. A first send's definite 4xx
 *   ends the action.
 * - A success must confirm the captured action. A replay returns the original facts and today's album, so what is compared is the
 *   facts against the request; today's album may be newer, but its version never older than the facts say.
 * - Rename-request history: summaries read from the server whenever the detail opens or its name version or pending request changes,
 *   with their own request sequence; a historical comparison has its own sequence too, apart from the pending review. Both are dropped
 *   on leaving the page, on an identity change and when the album cannot be read. Nothing about it is kept in browser storage, and it
 *   never decides anything: the pending request's controls are the existing ones.
 * - An upload's completion and an album's add are separate. An add is sent only with READY ids, and those uploads leave the queue only
 *   when the add is confirmed: a refusal keeps them, because it added nothing.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { isAlbum, isAlbumPhoto, isAlbumProposal, isAlbumProposalPage } from '@/api/album-rules'
import {
  addAlbumPhotos,
  createAlbum,
  decideAlbumProposal,
  getAlbum,
  getAlbumProposal,
  listAlbumPhotos,
  listAlbumProposals,
  listAlbums,
  proposeAlbumRename,
  removeAlbumPhoto,
  renameAlbum,
  setAlbumCover,
  type AlbumDecisionOutcome,
} from '@/api/albums'
import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { isValidNotificationFacts } from '@/api/notifications'
import { newIdempotencyKey } from '@/api/tasks'
import type { Album, AlbumMutationResponse, AlbumPhoto, AlbumProposal, AlbumProposalSummary } from '@/api/types'
import { useMemoryUploadsStore } from './memory-uploads'
import { useMediaCapabilityStore } from './media-capability'
import { useSessionStore } from './session'

export type AlbumLoadStatus = 'idle' | 'loading' | 'ready' | 'error'
export type AlbumActionStatus = 'submitting' | 'unknown' | 'not_sent'
export type AlbumActionKind = 'create' | 'add' | 'remove' | 'cover' | 'rename' | 'propose' | 'decide'

export interface AlbumAction {
  id: string
  kind: AlbumActionKind
  albumId?: string
  proposalId?: string
  expectedVersion?: number
  name?: string
  mediaIds?: string[]
  mediaId?: string
  outcome?: AlbumDecisionOutcome
  /** D20: an add batch's explicit notify_partner, captured with the key and never changed by a retry. */
  notify?: boolean
  key: string
  status: AlbumActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved?: boolean
}

export interface AlbumNotice {
  id: string
  ok: boolean
  text: string
}

export type AlbumResult =
  | { outcome: 'done'; response: AlbumMutationResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

export interface AlbumDetailState {
  id: string | null
  status: AlbumLoadStatus
  item: Album | null
  errorText: string
}

export interface AlbumPhotosState {
  albumId: string | null
  status: AlbumLoadStatus
  items: AlbumPhoto[]
  nextCursor: string | null
  loadingMore: boolean
  errorText: string
  moreErrorText: string
}

export interface AlbumReviewState {
  proposalId: string | null
  status: AlbumLoadStatus
  proposal: AlbumProposal | null
  errorText: string
}

export interface AlbumHistoryState {
  albumId: string | null
  status: AlbumLoadStatus
  items: AlbumProposalSummary[]
  nextCursor: string | null
  /** The album's latest name version as the newest page read it; what an old approval is compared with. */
  currentVersion: number | null
  errorText: string
  loadingMore: boolean
  moreErrorText: string
}

const OUTCOME_OF = { approve: 'approved', reject: 'rejected', withdraw: 'withdrawn' } as const

const DONE_TEXT: Record<Exclude<AlbumActionKind, 'decide'>, string> = {
  create: '相册已创建。',
  add: '照片已加入相册。',
  remove: '照片已从相册移除。文件仍然保留。',
  cover: '封面已更新。',
  rename: '相册名称已修改。',
  propose: '改名申请已提交，等待创建者处理。',
}

const DECIDE_TEXT: Record<AlbumDecisionOutcome, string> = {
  approve: '已同意改名，相册名称已更新。',
  reject: '已拒绝这条改名申请。',
  withdraw: '已撤回这条改名申请。',
}

const MALFORMED = '服务器的响应无法确认，请重试。'

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

function sameIds(a: unknown, b: string[] | undefined): boolean {
  return Array.isArray(a) && !!b && a.length === b.length && a.every((id, index) => id === b[index])
}

/** The response confirms exactly the captured action for this owner; anything else stays unknown. */
export function isUsableAlbumResponse(response: unknown, action: AlbumAction, owner: string): response is AlbumMutationResponse {
  if (!response || typeof response !== 'object') return false
  const r = response as Partial<AlbumMutationResponse>
  const m = r.mutation
  if (!m || !isAlbum(r.album) || m.album_id !== r.album.id || m.actor_id !== owner) return false
  const album = r.album
  if (action.kind !== 'create' && album.id !== action.albumId) return false
  // A replay carries today's album, which may be newer but never older than what it confirms. A name is compared only at the
  // mutation's own version: a newer album may legitimately have another.
  const notOlder = typeof m.version === 'number' && album.version >= m.version
  const nameAgrees = notOlder && (album.version !== m.version || album.name === action.name)
  switch (action.kind) {
    case 'create':
      return m.action === 'create' && m.version === 1 && album.creator_id === owner && nameAgrees
    case 'rename':
      return m.action === 'edit' && m.version === (action.expectedVersion ?? 0) + 1 && nameAgrees
    case 'propose':
      return m.action === 'propose' && typeof m.base_version === 'number' && m.base_version === action.expectedVersion &&
        typeof m.proposal_id === 'string' && m.proposal_id.length > 0 && album.version >= m.base_version
    case 'add':
      return m.action === 'add_photos' && typeof m.batch_id === 'string' && m.batch_id.length > 0 && sameIds(m.media_ids, action.mediaIds) &&
        isValidNotificationFacts(r.notification, action.notify, true)
    case 'remove':
      return m.action === 'remove_photo' && m.media_id === action.mediaId
    case 'cover':
      return m.action === 'set_cover' && m.media_id === action.mediaId && typeof m.event_no === 'number' && m.event_no >= 1
    case 'decide': {
      if (m.action !== 'decide' || m.proposal_id !== action.proposalId || !action.outcome || m.outcome !== OUTCOME_OF[action.outcome]) return false
      if (action.outcome !== 'approve') return m.version === null || m.version === undefined
      if (typeof m.base_version !== 'number' || m.version !== m.base_version + 1 || !notOlder) return false
      // Exactly the requested name, when that name is known and this is still its version.
      return album.version !== m.version || action.name === undefined || album.name === action.name
    }
    default:
      return false
  }
}

const EMPTY_DETAIL: AlbumDetailState = { id: null, status: 'idle', item: null, errorText: '' }
const EMPTY_PHOTOS: AlbumPhotosState = { albumId: null, status: 'idle', items: [], nextCursor: null, loadingMore: false, errorText: '', moreErrorText: '' }
const EMPTY_REVIEW: AlbumReviewState = { proposalId: null, status: 'idle', proposal: null, errorText: '' }
function emptyHistory(): AlbumHistoryState {
  return { albumId: null, status: 'idle', items: [], nextCursor: null, currentVersion: null, errorText: '', loadingMore: false, moreErrorText: '' }
}

export const useAlbumsStore = defineStore('albums', () => {
  const items = ref<Album[]>([])
  const status = ref<AlbumLoadStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  const uploadsEnabled = ref<boolean | null>(null)
  const detail = ref<AlbumDetailState>({ ...EMPTY_DETAIL })
  const photos = ref<AlbumPhotosState>({ ...EMPTY_PHOTOS })
  const review = ref<AlbumReviewState>({ ...EMPTY_REVIEW })
  const history = ref<AlbumHistoryState>(emptyHistory())
  const historyReview = ref<AlbumReviewState>({ ...EMPTY_REVIEW })
  const actions = ref<Record<string, AlbumAction>>({})
  const notices = ref<AlbumNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0
  let detailSeq = 0
  let photosSeq = 0
  let reviewSeq = 0
  let historySeq = 0
  let historyReviewSeq = 0

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
    photosSeq += 1
    reviewSeq += 1
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    uploadsEnabled.value = null
    detail.value = { ...EMPTY_DETAIL }
    photos.value = { ...EMPTY_PHOTOS }
    review.value = { ...EMPTY_REVIEW }
    clearHistory()
    actions.value = {}
    notices.value = []
  }

  /** The server just said uploads are off: the view explains it, without another read. */
  function markUploadsDisabled(): void {
    uploadsEnabled.value = false
  }

  function isPage(response: unknown): response is { albums: Album[]; next_cursor: string | null; uploads_enabled?: unknown } {
    if (!response || typeof response !== 'object') return false
    const r = response as { albums?: unknown; next_cursor?: unknown }
    return Array.isArray(r.albums) && r.albums.every(isAlbum) && (r.next_cursor === null || typeof r.next_cursor === 'string')
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
      const response = await listAlbums()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      items.value = response.albums
      nextCursor.value = response.next_cursor
      uploadsEnabled.value = typeof response.uploads_enabled === 'boolean' ? response.uploads_enabled : null
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
      const response = await listAlbums(nextCursor.value)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      const seen = new Set(items.value.map((a) => a.id))
      items.value = [...items.value, ...response.albums.filter((a) => !seen.has(a.id))]
      nextCursor.value = response.next_cursor
      if (typeof response.uploads_enabled === 'boolean') uploadsEnabled.value = response.uploads_enabled
      loadingMore.value = false
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  async function loadPhotos(albumId: string): Promise<void> {
    const seq = ++photosSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    photos.value = { ...photos.value, albumId, status: 'loading', errorText: '', moreErrorText: '', loadingMore: false }
    try {
      const response = await listAlbumPhotos(albumId)
      if (seq !== photosSeq || !stillCurrent(generation, owner)) return
      if (!response || !Array.isArray(response.photos) || !response.photos.every(isAlbumPhoto) ||
        (response.next_cursor !== null && typeof response.next_cursor !== 'string')) throw new Error(MALFORMED)
      photos.value = { ...EMPTY_PHOTOS, albumId, status: 'ready', items: response.photos, nextCursor: response.next_cursor }
    } catch (error) {
      if (seq !== photosSeq || !stillCurrent(generation, owner)) return
      photos.value = { ...EMPTY_PHOTOS, albumId, status: 'error', errorText: describeError(error) }
    }
  }

  async function loadMorePhotos(): Promise<void> {
    const current = photos.value
    if (current.status !== 'ready' || !current.nextCursor || current.loadingMore || !current.albumId) return
    const albumId = current.albumId
    const seq = ++photosSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    photos.value = { ...current, loadingMore: true, moreErrorText: '' }
    try {
      const response = await listAlbumPhotos(albumId, current.nextCursor)
      if (seq !== photosSeq || !stillCurrent(generation, owner)) return
      if (!response || !Array.isArray(response.photos) || !response.photos.every(isAlbumPhoto) ||
        (response.next_cursor !== null && typeof response.next_cursor !== 'string')) throw new Error(MALFORMED)
      const seen = new Set(photos.value.items.map((p) => p.media_id))
      photos.value = { ...photos.value, items: [...photos.value.items, ...response.photos.filter((p) => !seen.has(p.media_id))], nextCursor: response.next_cursor, loadingMore: false }
    } catch (error) {
      if (seq !== photosSeq || !stillCurrent(generation, owner)) return
      photos.value = { ...photos.value, loadingMore: false, moreErrorText: describeError(error) }
    }
  }

  async function openDetail(id: string): Promise<void> {
    const seq = ++detailSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const known = items.value.find((a) => a.id === id) ?? null
    detail.value = { id, status: 'loading', item: known, errorText: '' }
    void loadPhotos(id)
    try {
      const response = await getAlbum(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      if (!response || !isAlbum(response.album) || response.album.id !== id) throw new Error(MALFORMED)
      detail.value = { id, status: 'ready', item: response.album, errorText: '' }
    } catch (error) {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'error', item: null, errorText: describeError(error) }
      // Unreadable now: its history goes with it until the album reads again.
      if (history.value.albumId === id) clearHistory()
    }
  }

  /** Leaving the page: late answers for it are dropped. */
  function closeDetail(): void {
    detailSeq += 1
    photosSeq += 1
    reviewSeq += 1
    detail.value = { ...EMPTY_DETAIL }
    photos.value = { ...EMPTY_PHOTOS }
    review.value = { ...EMPTY_REVIEW }
    clearHistory()
  }

  function clearHistory(): void {
    historySeq += 1
    historyReviewSeq += 1
    history.value = emptyHistory()
    historyReview.value = { ...EMPTY_REVIEW }
  }

  /** The first page of one album's rename requests. What was shown for the same album stays until the answer arrives. */
  async function loadHistory(albumId: string): Promise<void> {
    const seq = ++historySeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const same = history.value.albumId === albumId
    history.value = {
      ...emptyHistory(), albumId, status: 'loading', items: same ? history.value.items : [], currentVersion: same ? history.value.currentVersion : null,
    }
    try {
      const response = await listAlbumProposals(albumId)
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      if (!isAlbumProposalPage(response, albumId)) throw new Error(MALFORMED)
      history.value = {
        albumId, status: 'ready', items: response.proposals, nextCursor: response.next_cursor, currentVersion: response.current_version,
        errorText: '', loadingMore: false, moreErrorText: '',
      }
    } catch (error) {
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      history.value = { ...emptyHistory(), albumId, status: 'error', errorText: describeError(error) }
    }
  }

  async function loadMoreHistory(): Promise<void> {
    const current = history.value
    const albumId = current.albumId
    if (!albumId || current.status !== 'ready' || !current.nextCursor || current.loadingMore) return
    const seq = ++historySeq
    const generation = sessionGeneration
    const owner = currentUserId()
    history.value = { ...current, loadingMore: true, moreErrorText: '' }
    try {
      const response = await listAlbumProposals(albumId, current.nextCursor)
      if (seq !== historySeq || !stillCurrent(generation, owner)) return
      if (!isAlbumProposalPage(response, albumId)) throw new Error(MALFORMED)
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

  /** The original and proposed names of one listed request, from the request record, apart from the pending review. */
  async function openHistoryReview(albumId: string, proposalId: string): Promise<void> {
    const seq = ++historyReviewSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    historyReview.value = { proposalId, status: 'loading', proposal: null, errorText: '' }
    try {
      const response = await getAlbumProposal(albumId, proposalId)
      if (seq !== historyReviewSeq || !stillCurrent(generation, owner) || history.value.albumId !== albumId) return
      if (!response || !isAlbumProposal(response.proposal, albumId, proposalId)) throw new Error(MALFORMED)
      historyReview.value = { proposalId, status: 'ready', proposal: response.proposal, errorText: '' }
    } catch (error) {
      if (seq !== historyReviewSeq || !stillCurrent(generation, owner) || history.value.albumId !== albumId) return
      historyReview.value = { proposalId, status: 'error', proposal: null, errorText: describeError(error) }
    }
  }

  function closeHistoryReview(): void {
    historyReviewSeq += 1
    historyReview.value = { ...EMPTY_REVIEW }
  }

  async function loadReview(albumId: string, proposalId: string): Promise<void> {
    const seq = ++reviewSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    review.value = { proposalId, status: 'loading', proposal: null, errorText: '' }
    try {
      const response = await getAlbumProposal(albumId, proposalId)
      if (seq !== reviewSeq || !stillCurrent(generation, owner)) return
      if (!response || !isAlbumProposal(response.proposal, albumId, proposalId)) throw new Error(MALFORMED)
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

  /** A write's answer is newer than any read still in flight, and an older version never replaces a newer one. */
  function absorb(album: Album): void {
    items.value = items.value.map((a) => (a.id === album.id && album.version >= a.version ? album : a))
    if (detail.value.id !== album.id) return
    detailSeq += 1
    const current = detail.value.item
    if (!current || album.version >= current.version) detail.value = { id: album.id, status: 'ready', item: album, errorText: '' }
    else if (detail.value.status !== 'ready') detail.value = { ...detail.value, status: 'ready' }
  }

  /** Re-reads after a rejection that may mean this copy is stale. */
  async function refreshOne(id: string): Promise<void> {
    const generation = sessionGeneration
    const owner = currentUserId()
    if (detail.value.id === id) void loadPhotos(id)
    const seq = detail.value.id === id ? ++detailSeq : detailSeq
    try {
      const response = await getAlbum(id)
      if (!stillCurrent(generation, owner) || !response || !isAlbum(response.album)) return
      if (detail.value.id === id && seq !== detailSeq) return
      absorb(response.album)
    } catch {
      // The notice already reports the rejection; a failed refresh changes nothing.
    }
  }

  /** D6: a photo was deleted or restored. The open album's items, count and cover come back from the server. */
  async function refreshAfterPhotoChange(): Promise<void> {
    const id = detail.value.id
    if (!id) return
    const generation = sessionGeneration
    const owner = currentUserId()
    void loadPhotos(id)
    const seq = ++detailSeq
    try {
      const response = await getAlbum(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      if (!response || !isAlbum(response.album) || response.album.id !== id) throw new Error(MALFORMED)
      absorb(response.album)
    } catch {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      notify({ id: `refresh:${id}`, ok: false, text: '相册信息没有刷新成功，照片数量和封面可能不是最新的，请重新打开这个页面。' })
    }
  }

  function update(id: string, patch: Partial<AlbumAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: AlbumNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  function keepUnknown(id: string, what: string): AlbumResult {
    update(id, { status: 'unknown', errorText: `${what}之前的提交结果仍未确认，请保留原请求重试。` })
    return { outcome: 'unknown' }
  }

  function call(action: AlbumAction): Promise<unknown> {
    const albumId = action.albumId ?? ''
    switch (action.kind) {
      case 'create':
        return createAlbum(action.name ?? '', action.key)
      case 'add':
        return addAlbumPhotos(albumId, action.mediaIds ?? [], action.key, action.notify)
      case 'remove':
        return removeAlbumPhoto(albumId, action.mediaId ?? '', action.key)
      case 'cover':
        return setAlbumCover(albumId, action.mediaId ?? '', action.key)
      case 'rename':
        return renameAlbum(albumId, action.expectedVersion ?? 0, action.name ?? '', action.key)
      case 'propose':
        return proposeAlbumRename(albumId, action.expectedVersion ?? 0, action.name ?? '', action.key)
      default:
        return decideAlbumProposal(albumId, action.proposalId ?? '', action.outcome ?? 'reject', action.key)
    }
  }

  function otherInFlight(action: AlbumAction): boolean {
    return !!action.albumId && Object.values(actions.value).some((a) => a.id !== action.id && a.albumId === action.albumId && a.status === 'submitting')
  }

  /** What a confirmed write changes on the page. */
  function applyDone(action: AlbumAction, response: AlbumMutationResponse): void {
    absorb(response.album)
    if (action.kind === 'create') void load()
    if (action.kind === 'add') {
      const uploads = useMemoryUploadsStore()
      const added = new Set(action.mediaIds ?? [])
      for (const item of uploads.items.filter((u) => u.scope === `album:${action.albumId}` && u.mediaId && added.has(u.mediaId))) uploads.remove(item.id)
      if (photos.value.albumId === action.albumId) void loadPhotos(action.albumId ?? '')
    }
    if (action.kind === 'remove' && photos.value.albumId === action.albumId) {
      // A photo removed mid-walk simply stops appearing; the cursor is by seq, so nothing shifts.
      photosSeq += 1
      photos.value = { ...photos.value, status: 'ready', loadingMore: false, items: photos.value.items.filter((p) => p.media_id !== action.mediaId) }
    }
    if (action.kind === 'decide') clearReview()
  }

  async function send(id: string): Promise<AlbumResult> {
    const action = actions.value[id]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting' || otherInFlight(action)) return { outcome: 'busy' }
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
    let response: AlbumMutationResponse
    try {
      const received = await call(action)
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableAlbumResponse(received, action, owner)) throw new Error(MALFORMED)
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
        if (action.albumId) void refreshOne(action.albumId)
        return { outcome: 'rejected', errorCode: error instanceof ApiError ? error.code : null }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复保存。' })
      return { outcome: 'unknown' }
    }
    end(id)
    notify({ id, ok: true, text: action.kind === 'decide' && action.outcome ? DECIDE_TEXT[action.outcome] : DONE_TEXT[action.kind as Exclude<AlbumActionKind, 'decide'>] })
    applyDone(action, response)
    return { outcome: 'done', response }
  }

  function register(action: Omit<AlbumAction, 'key' | 'status' | 'errorText'>): Promise<AlbumResult> {
    if (actions.value[action.id] || otherInFlight(action as AlbumAction)) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  function create(name: string): Promise<AlbumResult> {
    return register({ id: 'new', kind: 'create', name })
  }

  /** All or nothing, READY ids only, in display order. */
  /** `notifyPartner` (D20) is sent only when given: at most one notice for the whole batch, never one per photo. */
  function addPhotos(albumId: string, mediaIds: string[], notifyPartner?: boolean): Promise<AlbumResult> {
    return register({ id: `add:${albumId}`, kind: 'add', albumId, mediaIds: [...mediaIds], ...(notifyPartner === undefined ? {} : { notify: notifyPartner }) })
  }

  function removePhoto(albumId: string, mediaId: string): Promise<AlbumResult> {
    return register({ id: `remove:${albumId}:${mediaId}`, kind: 'remove', albumId, mediaId })
  }

  function setCover(albumId: string, mediaId: string): Promise<AlbumResult> {
    return register({ id: `cover:${albumId}`, kind: 'cover', albumId, mediaId })
  }

  /** `expectedVersion` is the version captured when the form opened. */
  function rename(albumId: string, expectedVersion: number, name: string): Promise<AlbumResult> {
    return register({ id: `rename:${albumId}`, kind: 'rename', albumId, expectedVersion, name })
  }

  function propose(albumId: string, expectedVersion: number, name: string): Promise<AlbumResult> {
    return register({ id: `propose:${albumId}`, kind: 'propose', albumId, expectedVersion, name })
  }

  /** `proposedName` is what the creator was shown; an approval must apply exactly that. */
  function decide(albumId: string, proposalId: string, outcome: AlbumDecisionOutcome, proposedName?: string): Promise<AlbumResult> {
    return register({ id: `decide:${albumId}:${proposalId}`, kind: 'decide', albumId, proposalId, outcome, name: outcome === 'approve' ? proposedName : undefined })
  }

  /** Resend an unknown or unsent action with its original key and payload (user click only). */
  function retry(id: string): Promise<AlbumResult> {
    return send(id)
  }

  return {
    items, status, errorText, nextCursor, loadingMore, moreErrorText, uploadsEnabled, detail, photos, review, history, historyReview, actions, notices,
    reset, load, loadMore, loadPhotos, loadMorePhotos, markUploadsDisabled, openDetail, closeDetail, loadReview, refreshAfterPhotoChange,
    create, addPhotos, removePhoto, setCover, rename, propose, decide, retry, dismissNotice,
    loadHistory, loadMoreHistory, openHistoryReview, closeHistoryReview,
  }
})
