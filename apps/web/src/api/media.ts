/**
 * Private media (P3-1, contract 0.25.0): grant, direct upload to the staging location, completion, and the read URL.
 *
 * - The grant and completion go through the shared `request()`, with same-origin cookie, CSRF and the offline guard.
 * - The staging POST goes directly to storage with `credentials: 'omit'`: no cookie or CSRF token leaves for the store.
 *   The form is signed, so it is never logged, and nothing here stores it.
 * - Photos are displayed through the authenticated route. Its redirect to a short-lived signed URL is followed by the browser
 *   for `<img>`; script never sees that URL.
 */
import { buildUrl, request } from './client'
import type { MediaDeletion, MediaDeletionContext, MediaDeletionResponse, MediaFacts, MediaResponse, MediaUploadForm, UndoableMediaDeletion, UndoableMediaDeletionList } from './types'

export type MediaVariant = 'thumb' | 'view' | 'original'

export const MEDIA_DELETION_PAGE_SIZE = 20

/** D6: the caller's own photo; no body. 201 a new cycle, 200 a same-key replay with that cycle's current facts. */
export function deleteMedia(mediaId: string, idempotencyKey: string): Promise<MediaDeletionResponse> {
  return request<MediaDeletionResponse>(`/api/v1/media/${encodeURIComponent(mediaId)}/deletions`, {
    method: 'POST', headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** D6: one exact cycle; no body. */
export function undoMediaDeletion(mediaId: string, deletionId: string, idempotencyKey: string): Promise<MediaDeletionResponse> {
  return request<MediaDeletionResponse>(`/api/v1/media/${encodeURIComponent(mediaId)}/deletions/${encodeURIComponent(deletionId)}/undo`, {
    method: 'POST', headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** D6: the caller's own cycles that can be undone now, newest first; the cursor is passed back unchanged. */
export function listUndoableMediaDeletions(cursor?: string | null, limit: number = MEDIA_DELETION_PAGE_SIZE): Promise<UndoableMediaDeletionList> {
  return request<UndoableMediaDeletionList>('/api/v1/media/deletions', { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

function isTime(value: unknown): value is string {
  return typeof value === 'string' && !Number.isNaN(Date.parse(value))
}

function isCycleFacts(value: unknown): boolean {
  if (!value || typeof value !== 'object') return false
  const d = value as Partial<MediaDeletion>
  return typeof d.deletion_id === 'string' && d.deletion_id.length > 0 && typeof d.media_id === 'string' && d.media_id.length > 0 &&
    isTime(d.deleted_at) && isTime(d.purge_after)
}

/** A parent is named only as memory or album; anything else carries nothing of one. */
function isDeletionContext(value: unknown): value is MediaDeletionContext {
  if (!value || typeof value !== 'object') return false
  const c = value as Partial<MediaDeletionContext>
  if (c.kind === 'memory' || c.kind === 'album') {
    return (c.relation === 'current' || c.relation === 'former') && typeof c.target_id === 'string' && c.target_id.length > 0 &&
      typeof c.label === 'string' && c.label.length > 0 && (c.position === null || (typeof c.position === 'number' && c.position >= 1))
  }
  return (c.kind === 'unavailable' || c.kind === 'none') && c.relation === null && c.target_id === null && c.label === null && c.position === null
}

export function isUndoableDeletion(value: unknown): value is UndoableMediaDeletion {
  if (!isCycleFacts(value)) return false
  const d = value as Partial<UndoableMediaDeletion>
  return isTime(d.uploaded_at) && isDeletionContext(d.context)
}

/** What the recovery view calls a row. Text only: a deleted photo cannot be displayed. */
export function describeDeletionContext(context: MediaDeletionContext): string {
  switch (context.kind) {
    case 'memory':
      if (context.relation === 'former') return `回忆《${context.label}》的早期版本`
      return context.position === null ? `回忆《${context.label}》` : `回忆《${context.label}》第 ${context.position} 张`
    case 'album':
      return context.relation === 'former' ? `曾在相册《${context.label}》` : `相册《${context.label}》`
    case 'unavailable':
      return '所属内容已不可用'
    default:
      return '未加入回忆或相册'
  }
}

export function isMediaDeletion(value: unknown): value is MediaDeletion {
  if (!isCycleFacts(value)) return false
  const state = (value as Partial<MediaDeletion>).state
  return state === 'deleted' || state === 'undone' || state === 'purging' || state === 'purged'
}

/** The staging POST failed or was refused. Carries no URL, field or response body. */
export class StagingUploadError extends Error {
  constructor() {
    super('照片没有上传完成。')
    this.name = 'StagingUploadError'
  }
}

/** By name: an abort's DOMException is not an `Error` instance in every environment. */
export function isAbort(error: unknown): boolean {
  return !!error && typeof error === 'object' && (error as { name?: unknown }).name === 'AbortError'
}

/** `POST /api/v1/media/uploads`: 201 a new grant, 200 a same-key replay. */
export function createMediaUpload(contentType: string, sizeBytes: number, idempotencyKey: string): Promise<MediaResponse> {
  return request<MediaResponse>('/api/v1/media/uploads', {
    method: 'POST',
    body: { content_type: contentType, size_bytes: sizeBytes },
    headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** `POST /api/v1/media/uploads/{id}/complete`: safe to retry; the decided state, ready or rejected. */
export function completeMediaUpload(mediaId: string): Promise<MediaResponse> {
  return request<MediaResponse>(`/api/v1/media/uploads/${encodeURIComponent(mediaId)}/complete`, { method: 'POST' })
}

/** Direct to storage. The file goes last, as the form POST requires. */
export async function postToStaging(form: MediaUploadForm, file: Blob, signal?: AbortSignal): Promise<void> {
  const body = new FormData()
  for (const [name, value] of Object.entries(form.fields)) body.append(name, value)
  body.append('file', file)
  let response: Response
  try {
    response = await fetch(form.url, {
      method: 'POST', body, credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer', redirect: 'error', signal,
    })
  } catch (error) {
    if (isAbort(error)) throw error
    throw new StagingUploadError()
  }
  if (!response.ok) throw new StagingUploadError()
}

/** The authenticated, same-origin read route; `/api/` is never cached by the service worker. */
export function mediaContentUrl(mediaId: string, variant: MediaVariant): string {
  return buildUrl(`/api/v1/media/${encodeURIComponent(mediaId)}/content`, { variant })
}

export function isMediaFacts(value: unknown, mediaId?: string): value is MediaFacts {
  if (!value || typeof value !== 'object') return false
  const m = value as Partial<MediaFacts>
  return typeof m.media_id === 'string' && m.media_id.length > 0 && (mediaId === undefined || m.media_id === mediaId) &&
    (m.state === 'granted' || m.state === 'expired' || m.state === 'ready' || m.state === 'rejected' || m.state === 'removed')
}

/** A usable form: a web URL, string fields and a real expiry. Anything else is treated as an unconfirmed response. */
export function isUploadForm(value: unknown): value is MediaUploadForm {
  if (!value || typeof value !== 'object') return false
  const f = value as Partial<MediaUploadForm>
  if (typeof f.url !== 'string' || typeof f.expires_at !== 'string' || Number.isNaN(Date.parse(f.expires_at))) return false
  if (!f.fields || typeof f.fields !== 'object' || Array.isArray(f.fields)) return false
  if (!Object.values(f.fields).every((v) => typeof v === 'string')) return false
  try {
    const url = new URL(f.url)
    return url.protocol === 'https:' || (url.protocol === 'http:' && (url.hostname === 'localhost' || url.hostname === '127.0.0.1'))
  } catch {
    return false
  }
}
