/**
 * D6 photo deletion and undo: memory only, nothing persisted, no offline write queue. Modelled on the albums store.
 *
 * - Identity: `reset()` raises the generation on logout, account switch or failed bootstrap. A late result lands only if the
 *   generation and the user are unchanged, so an old account's answer cannot touch the new one.
 * - The list comes only from the server, so a fresh session finds what an earlier one deleted. The newer read wins, and leaving the
 *   page drops late answers. It holds four facts per cycle: no image is requested for it.
 * - Writes, one per target: `delete:<media>`, `undo:<deletion>`. A network error, 5xx or malformed response keeps the original key and
 *   captured target, and only a user click resends them. An undo names one exact cycle.
 * - A retry's error alone does not prove what happened to an earlier send, with the exceptions in PROVEN_ON_RETRY. Each of those is
 *   a refusal that happens after the server replays a known key, for a state that can never change back, so that send has not committed
 *   and never can. The evidence is in docs/phase3-d6-media-deletion-design.md.
 * - MEDIA_ALREADY_DELETED on a delete's retry proves less: the photo is deleted now, by some other cycle, and nothing about the earlier
 *   send. The retry has nothing left to do, so it is retired and the notice says only that. A later deletion is a new decision: it goes
 *   through the confirmation again and gets a new key, so an old click can never delete a photo someone has since restored.
 * - A notice speaks only of the cycle its answer names. A replay reports that cycle as it is today, and says nothing about the photo,
 *   which a later cycle may have deleted again. What the photo is now comes from the re-read.
 * - Every settled outcome re-reads the list and whatever album or memory is open, so counts, covers and placeholders come from the server.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { deleteMedia, isMediaDeletion, isUndoableDeletion, listUndoableMediaDeletions, undoMediaDeletion } from '@/api/media'
import { messageTime } from '@/api/message-rules'
import { newIdempotencyKey } from '@/api/tasks'
import type { MediaDeletion, UndoableMediaDeletion, UndoableMediaDeletionList } from '@/api/types'
import { useAlbumsStore } from './albums'
import { useMemoriesStore } from './memories'
import { useSessionStore } from './session'

export type MediaDeletionLoadStatus = 'idle' | 'loading' | 'ready' | 'error'
export type MediaDeletionActionStatus = 'submitting' | 'unknown' | 'not_sent'

export interface MediaDeletionAction {
  id: string
  kind: 'delete' | 'undo'
  mediaId: string
  /** The exact cycle; undo only. */
  deletionId?: string
  key: string
  status: MediaDeletionActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved?: boolean
}

export interface MediaDeletionNotice {
  id: string
  ok: boolean
  text: string
}

export type MediaDeletionResult =
  | { outcome: 'done'; deletion: MediaDeletion }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

const MALFORMED = '服务器的响应无法确认，请重试。'
export const RECOVERY_NAME = '最近删除的照片'

const REFUSAL_TEXT: Record<string, string> = {
  MEDIA_UNDO_CLOSED: '这次删除已经过了可撤销的时间，或已开始清除，不能再撤销。列表已刷新。',
  MEDIA_DELETION_NOT_ACTIVE: '这次删除已经撤销过了，可能是在其他设备上操作的。列表已刷新。',
  MEDIA_ALREADY_DELETED: `这张照片已经删除了，可以在“${RECOVERY_NAME}”里查看能否撤销。`,
  MEDIA_NOT_FOUND: '这张照片不存在，或你不能执行这项操作。',
}

/** The refusals that end an action whose earlier send is unconfirmed. Anything else keeps its key and request. */
const PROVEN_ON_RETRY: Record<MediaDeletionAction['kind'], ReadonlySet<string>> = {
  delete: new Set(['MEDIA_NOT_FOUND']),
  undo: new Set(['MEDIA_UNDO_CLOSED', 'MEDIA_DELETION_NOT_ACTIVE', 'MEDIA_NOT_FOUND']),
}

/** A delete's retry met another cycle. States the present only: the earlier send may or may not have gone through. */
const RETIRED_DELETE_TEXT = `这张照片目前是删除状态，可以在“${RECOVERY_NAME}”里查看能否撤销。之前那次删除请求的结果无法确认，已不再重试。`

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

/** The response confirms exactly the captured action. A replay of a delete reports that cycle as it is today. */
export function isUsableDeletionResponse(response: unknown, action: MediaDeletionAction): response is { deletion: MediaDeletion } {
  if (!response || typeof response !== 'object') return false
  const deletion = (response as { deletion?: unknown }).deletion
  if (!isMediaDeletion(deletion) || deletion.media_id !== action.mediaId) return false
  return action.kind === 'delete' || (deletion.deletion_id === action.deletionId && deletion.state === 'undone')
}

function isPage(response: unknown): response is UndoableMediaDeletionList {
  if (!response || typeof response !== 'object') return false
  const r = response as Partial<UndoableMediaDeletionList>
  return Array.isArray(r.deletions) && r.deletions.every(isUndoableDeletion) && (r.next_cursor === null || typeof r.next_cursor === 'string')
}

export const useMediaDeletionsStore = defineStore('media-deletions', () => {
  const items = ref<UndoableMediaDeletion[]>([])
  const status = ref<MediaDeletionLoadStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  const actions = ref<Record<string, MediaDeletionAction>>({})
  const notices = ref<MediaDeletionNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    listSeq += 1
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    actions.value = {}
    notices.value = []
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
      const response = await listUndoableMediaDeletions()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      items.value = response.deletions
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
      const response = await listUndoableMediaDeletions(nextCursor.value)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      if (!isPage(response)) throw new Error(MALFORMED)
      const seen = new Set(items.value.map((d) => d.deletion_id))
      items.value = [...items.value, ...response.deletions.filter((d) => !seen.has(d.deletion_id))]
      nextCursor.value = response.next_cursor
      loadingMore.value = false
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  /** Leaving the page: late answers are dropped. Unconfirmed writes stay. */
  function closeList(): void {
    listSeq += 1
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
  }

  function update(id: string, patch: Partial<MediaDeletionAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: MediaDeletionNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  /** Everything on screen that this outcome may have changed, from the server. */
  function refreshAffected(): void {
    if (status.value !== 'idle') void load()
    void useAlbumsStore().refreshAfterPhotoChange()
    void useMemoriesStore().refreshAfterPhotoChange()
  }

  function keepUnknown(id: string, what: string): MediaDeletionResult {
    update(id, { status: 'unknown', errorText: `${what}之前的提交结果仍未确认，请保留原请求重试。` })
    return { outcome: 'unknown' }
  }

  /** Only what this cycle's facts say. No answer here can say whether the photo is visible now. */
  function doneText(action: MediaDeletionAction, deletion: MediaDeletion): string {
    const when = `删除于 ${messageTime(deletion.deleted_at)} 的这次删除`
    if (action.kind === 'undo') return `${when}已撤销。如果之后又删除过，照片仍是删除状态。`
    switch (deletion.state) {
      case 'undone': return `${when}后来已撤销。这张照片现在的状态以页面显示为准。`
      case 'purging': return `${when}已过撤销期限，正在清除，不能再撤销。`
      case 'purged': return `${when}已过撤销期限并已清除，不能再撤销。`
      default: return `${when}已生效。可以在“${RECOVERY_NAME}”里撤销，期限到 ${messageTime(deletion.purge_after)}。`
    }
  }

  async function send(id: string): Promise<MediaDeletionResult> {
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
    let deletion: MediaDeletion
    try {
      const received = await (action.kind === 'delete'
        ? deleteMedia(action.mediaId, action.key)
        : undoMediaDeletion(action.mediaId, action.deletionId ?? '', action.key))
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableDeletionResponse(received, action)) throw new Error(MALFORMED)
      deletion = received.deletion
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) return keepUnknown(id, `本次重试未发出（${describeError(error)}）。`)
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (!isUnconfirmedOrUnsent(error)) {
        const code = error instanceof ApiError ? error.code : null
        const known = code !== null ? REFUSAL_TEXT[code] : undefined
        if (action.unresolved && action.kind === 'delete' && code === 'MEDIA_ALREADY_DELETED') {
          end(id)
          notify({ id, ok: false, text: RETIRED_DELETE_TEXT })
          refreshAffected()
          return { outcome: 'rejected', errorCode: code }
        }
        if (action.unresolved && (code === null || !PROVEN_ON_RETRY[action.kind].has(code))) {
          // The page can still show what is true now, while this request stays exactly as captured.
          if (known) refreshAffected()
          return keepUnknown(id, `本次重试未确认原结果（${known ?? describeError(error)}）。`)
        }
        end(id)
        notify({ id, ok: false, text: known ?? describeError(error) })
        refreshAffected()
        return { outcome: 'rejected', errorCode: code }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复操作。' })
      return { outcome: 'unknown' }
    }
    end(id)
    notify({ id, ok: true, text: doneText(action, deletion) })
    refreshAffected()
    return { outcome: 'done', deletion }
  }

  function register(action: Omit<MediaDeletionAction, 'key' | 'status' | 'errorText'>): Promise<MediaDeletionResult> {
    if (actions.value[action.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  function deletePhoto(mediaId: string): Promise<MediaDeletionResult> {
    return register({ id: `delete:${mediaId}`, kind: 'delete', mediaId })
  }

  function undo(mediaId: string, deletionId: string): Promise<MediaDeletionResult> {
    return register({ id: `undo:${deletionId}`, kind: 'undo', mediaId, deletionId })
  }

  /** Resend an unknown or unsent action with its original key and target (user click only). */
  function retry(id: string): Promise<MediaDeletionResult> {
    return send(id)
  }

  return {
    items, status, errorText, nextCursor, loadingMore, moreErrorText, actions, notices,
    reset, load, loadMore, closeList, deletePhoto, undo, retry, dismissNotice,
  }
})
