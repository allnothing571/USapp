/**
 * The caller's own READY uploads that may still be attached (contract 0.39.0, GET /api/v1/media/unattached). Not the retained list and
 * not a deletion: nothing is restored. A snapshot, not a reservation: listing renews no grant and moves no cleanup deadline, and an
 * attachment may still be refused if cleanup, deletion or another attachment won first.
 */
import { request } from './client'

export interface UnattachedPhoto {
  media_id: string
  uploaded_at: string
  width: number
  height: number
}

export interface UnattachedPage {
  photos: UnattachedPhoto[]
  next_cursor: string | null
}

export const UNATTACHED_PAGE_SIZE = 30

export function listUnattachedPhotos(cursor?: string | null, limit: number = UNATTACHED_PAGE_SIZE): Promise<UnattachedPage> {
  return request<UnattachedPage>('/api/v1/media/unattached', { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

export function isUnattachedPage(value: unknown): value is UnattachedPage {
  if (!value || typeof value !== 'object') return false
  const r = value as Partial<UnattachedPage>
  return Array.isArray(r.photos) && r.photos.every((p) => !!p && typeof p === 'object' && typeof p.media_id === 'string' && p.media_id.length > 0 &&
    typeof p.uploaded_at === 'string') && (r.next_cursor === null || typeof r.next_cursor === 'string')
}
