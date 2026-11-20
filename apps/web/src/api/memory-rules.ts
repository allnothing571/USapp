/**
 * Client-side mirror of the memory rules (P3-1b) and the response shapes the store accepts. The server is authoritative; these
 * give immediate hints and make sure what is submitted is normalised. Dates stay strings and are never parsed with `new Date()`.
 */
import { isCivilDate } from './anniversary-rules'
import { checkTaskTitle, normalizeTaskDescription } from './task-rules'
import type { CivilDate, Memory, MemoryContent, MemoryPhoto, MemoryProposal, MemoryProposalListResponse, MemoryProposalSummary, MemoryTerms } from './types'

export const MEMORY_TITLE_MAX = 60
export const MEMORY_DESCRIPTION_MAX = 2000
export const MEMORY_MAX_PHOTOS = 9
export const MEMORY_MIN_DATE: CivilDate = '1900-01-01'
export const MEDIA_MAX_BYTES = 20_000_000
export const MEDIA_TYPES: readonly string[] = ['image/jpeg', 'image/png', 'image/webp']

/** Today's civil date in Beijing, used only as the picker's limit. The server decides "not in the future". */
export function beijingToday(now: Date = new Date()): CivilDate {
  return new Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit' }).format(now)
}

export interface MemoryDraft {
  date: string
  title: string
  description: string
  mediaIds: string[]
}

export type MemoryCheck = { ok: true; terms: MemoryTerms } | { ok: false; message: string }

export function checkMemoryTerms(draft: MemoryDraft): MemoryCheck {
  const title = checkTaskTitle(draft.title, MEMORY_TITLE_MAX)
  if (!title.ok) return { ok: false, message: title.message }
  // Fixed-width YYYY-MM-DD strings compare in calendar order.
  if (!isCivilDate(draft.date) || draft.date < MEMORY_MIN_DATE) return { ok: false, message: '请选择 1900-01-01 之后的有效日期' }
  const description = normalizeTaskDescription(draft.description, MEMORY_DESCRIPTION_MAX)
  if (!description.ok) return { ok: false, message: description.message }
  if (draft.mediaIds.length < 1 || draft.mediaIds.length > MEMORY_MAX_PHOTOS) return { ok: false, message: `每条回忆需要 1 到 ${MEMORY_MAX_PHOTOS} 张照片` }
  if (new Set(draft.mediaIds).size !== draft.mediaIds.length) return { ok: false, message: '同一张照片只能出现一次' }
  return { ok: true, terms: { date: draft.date, title: title.value, description: description.value, media_ids: [...draft.mediaIds] } }
}

/** Photo order is part of the terms. */
export function sameMemoryTerms(a: MemoryTerms, b: MemoryTerms): boolean {
  return a.date === b.date && a.title === b.title && a.description === b.description &&
    a.media_ids.length === b.media_ids.length && a.media_ids.every((id, index) => id === b.media_ids[index])
}

/** D6: a deleted photo stays in history but cannot be named by an edit or request, so it never enters a form. */
export function termsOfContent(item: Pick<MemoryContent, 'date' | 'title' | 'description' | 'photos'>): MemoryTerms {
  return { date: item.date, title: item.title, description: item.description, media_ids: item.photos.filter((p) => !p.deleted).map((p) => p.media_id) }
}

function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0
}

function isVersion(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 1
}

export function isMemoryPhoto(value: unknown): value is MemoryPhoto {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<MemoryPhoto>
  return isNonEmptyString(p.media_id) && typeof p.position === 'number' && (p.deleted === undefined || typeof p.deleted === 'boolean')
}

function isContent(value: unknown): value is MemoryContent {
  if (!value || typeof value !== 'object') return false
  const c = value as Partial<MemoryContent>
  return typeof c.date === 'string' && typeof c.title === 'string' && typeof c.description === 'string' &&
    Array.isArray(c.photos) && c.photos.every(isMemoryPhoto)
}

export function isMemory(value: unknown): value is Memory {
  if (!isContent(value)) return false
  const m = value as unknown as Partial<Memory>
  const deletion = m.pending_delete_request
  if (deletion !== undefined && deletion !== null && (!isNonEmptyString(deletion.id) ||
    !isNonEmptyString(deletion.requester_id) || typeof deletion.created_at !== 'string' || m.pending_proposal !== null)) return false
  if (!isNonEmptyString(m.id) || !isNonEmptyString(m.creator_id) || !isVersion(m.version)) return false
  const pending = m.pending_proposal
  if (pending === null) return true
  return !!pending && typeof pending === 'object' && isNonEmptyString(pending.id) && isNonEmptyString(pending.proposer_id)
}

/**
 * One summary of this memory's history, internally consistent: a pending request has no decision; only an approval names the
 * version it created, which is newer than its base.
 */
export function isMemoryProposalSummary(value: unknown, memoryId: string): value is MemoryProposalSummary {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<MemoryProposalSummary>
  if (!isNonEmptyString(p.id) || p.memory_id !== memoryId || !isNonEmptyString(p.proposer_id) || !isVersion(p.base_version) ||
    typeof p.created_at !== 'string') return false
  if (p.status === 'pending') return p.decided_by === null && p.decided_at === null && p.approved_version === null
  if (p.status !== 'approved' && p.status !== 'rejected' && p.status !== 'withdrawn') return false
  if (!isNonEmptyString(p.decided_by) || typeof p.decided_at !== 'string') return false
  if (p.status === 'approved') return isVersion(p.approved_version) && p.approved_version > p.base_version
  return p.approved_version === null
}

export function isMemoryProposalPage(value: unknown, memoryId: string): value is MemoryProposalListResponse {
  if (!value || typeof value !== 'object') return false
  const r = value as Partial<MemoryProposalListResponse>
  return Array.isArray(r.proposals) && r.proposals.every((p) => isMemoryProposalSummary(p, memoryId)) &&
    (r.next_cursor === null || typeof r.next_cursor === 'string') && isVersion(r.current_version)
}

export function isMemoryProposal(value: unknown, memoryId: string, proposalId: string): value is MemoryProposal {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<MemoryProposal>
  return p.id === proposalId && p.memory_id === memoryId && isNonEmptyString(p.proposer_id) && isVersion(p.base_version) &&
    (p.status === 'pending' || p.status === 'approved' || p.status === 'rejected' || p.status === 'withdrawn') &&
    isContent(p.original) && isContent(p.proposed)
}
