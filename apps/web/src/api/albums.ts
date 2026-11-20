/**
 * Shared albums (P3-2a and P3-2b, contract 0.27.0). Everything goes through the shared `request()`. Writes carry an
 * `Idempotency-Key` that the albums store generates once per action and reuses only when that same action is retried.
 */
import { request } from './client'
import type {
  AlbumListResponse,
  AlbumMutationResponse,
  AlbumPhotoListResponse,
  AlbumProposalListResponse,
  AlbumProposalResponse,
  AlbumResponse,
} from './types'

export type AlbumDecisionOutcome = 'approve' | 'reject' | 'withdraw'

export const ALBUM_PAGE_SIZE = 20
/** The design's recommendation: lazy pages, to stay within the store's image rate. */
export const ALBUM_PHOTO_PAGE_SIZE = 30

const BASE = '/api/v1/albums'

function idPath(id: string): string {
  return `${BASE}/${encodeURIComponent(id)}`
}

function proposalPath(id: string, proposalId: string): string {
  return `${idPath(id)}/proposals/${encodeURIComponent(proposalId)}`
}

function keyed(idempotencyKey: string) {
  return { 'Idempotency-Key': idempotencyKey }
}

export function createAlbum(name: string, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(BASE, { method: 'POST', body: { name }, headers: keyed(idempotencyKey) })
}

/** Newest first; the cursor is passed back unchanged. */
export function listAlbums(cursor?: string | null, limit: number = ALBUM_PAGE_SIZE): Promise<AlbumListResponse> {
  return request<AlbumListResponse>(BASE, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

export function getAlbum(id: string): Promise<AlbumResponse> {
  return request<AlbumResponse>(idPath(id), { method: 'GET' })
}

/** Active photos: newer adds first, and the chosen order inside each add. */
export function listAlbumPhotos(id: string, cursor?: string | null, limit: number = ALBUM_PHOTO_PAGE_SIZE): Promise<AlbumPhotoListResponse> {
  return request<AlbumPhotoListResponse>(`${idPath(id)}/photos`, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

/** All or nothing, in display order. */
/** D20: `notifyPartner` is the explicit opt-in captured with the key; undefined sends no field. */
export function addAlbumPhotos(id: string, mediaIds: string[], idempotencyKey: string, notifyPartner?: boolean): Promise<AlbumMutationResponse> {
  const body = notifyPartner === undefined ? { media_ids: mediaIds } : { media_ids: mediaIds, notify_partner: notifyPartner }
  return request<AlbumMutationResponse>(`${idPath(id)}/photos`, { method: 'POST', body, headers: keyed(idempotencyKey) })
}

export function removeAlbumPhoto(id: string, mediaId: string, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(`${idPath(id)}/photos/${encodeURIComponent(mediaId)}/removal`, { method: 'POST', headers: keyed(idempotencyKey) })
}

export function setAlbumCover(id: string, mediaId: string, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(`${idPath(id)}/cover`, { method: 'POST', body: { media_id: mediaId }, headers: keyed(idempotencyKey) })
}

export function renameAlbum(id: string, expectedVersion: number, name: string, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(`${idPath(id)}/revisions`, { method: 'POST', body: { expected_version: expectedVersion, name }, headers: keyed(idempotencyKey) })
}

export function proposeAlbumRename(id: string, expectedVersion: number, name: string, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(`${idPath(id)}/proposals`, { method: 'POST', body: { expected_version: expectedVersion, name }, headers: keyed(idempotencyKey) })
}

/** One album's rename requests, pending and decided, newest first. Summaries only; the cursor is passed back unchanged. */
export function listAlbumProposals(id: string, cursor?: string | null, limit: number = ALBUM_PAGE_SIZE): Promise<AlbumProposalListResponse> {
  return request<AlbumProposalListResponse>(`${idPath(id)}/proposals`, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

export function getAlbumProposal(id: string, proposalId: string): Promise<AlbumProposalResponse> {
  return request<AlbumProposalResponse>(proposalPath(id, proposalId), { method: 'GET' })
}

export function decideAlbumProposal(id: string, proposalId: string, outcome: AlbumDecisionOutcome, idempotencyKey: string): Promise<AlbumMutationResponse> {
  return request<AlbumMutationResponse>(`${proposalPath(id, proposalId)}/decision`, { method: 'POST', body: { outcome }, headers: keyed(idempotencyKey) })
}
