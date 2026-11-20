/**
 * Client-side mirror of the album rules (P3-2) and the response shapes the store accepts. The server is authoritative; these give
 * immediate hints and make sure what is submitted is normalised.
 */
import { checkTaskTitle, type TextCheck } from './task-rules'
import type { Album, AlbumPhoto, AlbumProposal, AlbumProposalListResponse, AlbumProposalSummary } from './types'

export const ALBUM_NAME_MAX = 40
export const ALBUM_MAX_PHOTOS_PER_ADD = 20

export function checkAlbumName(raw: string): TextCheck {
  const checked = checkTaskTitle(raw, ALBUM_NAME_MAX)
  if (checked.ok) return checked
  return { ok: false, message: checked.message.replace('标题', '相册名称') }
}

function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0
}

function isPositiveInt(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 1
}

export function isAlbum(value: unknown): value is Album {
  if (!value || typeof value !== 'object') return false
  const a = value as Partial<Album>
  if (!isNonEmptyString(a.id) || !isNonEmptyString(a.creator_id) || typeof a.name !== 'string' || a.name.length === 0 || !isPositiveInt(a.version)) return false
  if (typeof a.photo_count !== 'number' || !Number.isInteger(a.photo_count) || a.photo_count < 0) return false
  const cover = a.cover
  if (cover !== null && (!cover || typeof cover !== 'object' || !isNonEmptyString(cover.media_id) || typeof cover.chosen !== 'boolean')) return false
  const pending = a.pending_proposal
  if (pending === null) return true
  return !!pending && typeof pending === 'object' && isNonEmptyString(pending.id) && isNonEmptyString(pending.proposer_id) && isPositiveInt(pending.base_version)
}

export function isAlbumPhoto(value: unknown): value is AlbumPhoto {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<AlbumPhoto>
  return isNonEmptyString(p.media_id) && isNonEmptyString(p.uploader_id) && isPositiveInt(p.seq)
}

/**
 * One summary of this album's history, internally consistent: a pending request has no decision; only an approval names the name
 * version it created, which is newer than its base.
 */
export function isAlbumProposalSummary(value: unknown, albumId: string): value is AlbumProposalSummary {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<AlbumProposalSummary>
  if (!isNonEmptyString(p.id) || p.album_id !== albumId || !isNonEmptyString(p.proposer_id) || !isPositiveInt(p.base_version) ||
    typeof p.created_at !== 'string') return false
  if (p.status === 'pending') return p.decided_by === null && p.decided_at === null && p.approved_version === null
  if (p.status !== 'approved' && p.status !== 'rejected' && p.status !== 'withdrawn') return false
  if (!isNonEmptyString(p.decided_by) || typeof p.decided_at !== 'string') return false
  if (p.status === 'approved') return isPositiveInt(p.approved_version) && p.approved_version > p.base_version
  return p.approved_version === null
}

export function isAlbumProposalPage(value: unknown, albumId: string): value is AlbumProposalListResponse {
  if (!value || typeof value !== 'object') return false
  const r = value as Partial<AlbumProposalListResponse>
  return Array.isArray(r.proposals) && r.proposals.every((p) => isAlbumProposalSummary(p, albumId)) &&
    (r.next_cursor === null || typeof r.next_cursor === 'string') && isPositiveInt(r.current_version)
}

export function isAlbumProposal(value: unknown, albumId: string, proposalId: string): value is AlbumProposal {
  if (!value || typeof value !== 'object') return false
  const p = value as Partial<AlbumProposal>
  return p.id === proposalId && p.album_id === albumId && isNonEmptyString(p.proposer_id) && isPositiveInt(p.base_version) &&
    (p.status === 'pending' || p.status === 'approved' || p.status === 'rejected' || p.status === 'withdrawn') &&
    typeof p.original_name === 'string' && typeof p.proposed_name === 'string'
}
