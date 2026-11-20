/**
 * Photo memories (P3-1b, contract 0.25.0): timeline, detail, creator edits and partner edit requests.
 * Everything goes through the shared `request()`. Writes carry an `Idempotency-Key` that the memories store generates once per
 * action and reuses only when that same action is retried.
 */
import { request } from './client'
import type {
  MemoryListResponse, MemoryMutationResponse, MemoryProposalListResponse, MemoryProposalResponse, MemoryResponse, MemoryTerms,
} from './types'

export type MemoryDecisionOutcome = 'approve' | 'reject' | 'withdraw'

export const MEMORY_PAGE_SIZE = 20

const BASE = '/api/v1/memories'

function idPath(id: string): string {
  return `${BASE}/${encodeURIComponent(id)}`
}

function proposalPath(id: string, proposalId: string): string {
  return `${idPath(id)}/proposals/${encodeURIComponent(proposalId)}`
}

/** D20: `notifyPartner` is the explicit opt-in captured with the key; undefined sends no field. */
export function createMemory(terms: MemoryTerms, idempotencyKey: string, notifyPartner?: boolean): Promise<MemoryMutationResponse> {
  const body = notifyPartner === undefined ? { ...terms } : { ...terms, notify_partner: notifyPartner }
  return request<MemoryMutationResponse>(BASE, { method: 'POST', body, headers: { 'Idempotency-Key': idempotencyKey } })
}

/** Newest memory date first; the cursor is passed back unchanged. */
export function listMemories(cursor?: string | null, limit: number = MEMORY_PAGE_SIZE): Promise<MemoryListResponse> {
  return request<MemoryListResponse>(BASE, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

export function getMemory(id: string): Promise<MemoryResponse> {
  return request<MemoryResponse>(idPath(id), { method: 'GET' })
}

/** The creator replaces the terms and photos against the version captured when the form opened. */
export function editMemory(id: string, expectedVersion: number, terms: MemoryTerms, idempotencyKey: string): Promise<MemoryMutationResponse> {
  return request<MemoryMutationResponse>(`${idPath(id)}/revisions`, {
    method: 'POST', body: { expected_version: expectedVersion, ...terms }, headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** The partner proposes a full replacement, on the same terms. */
export function proposeMemoryEdit(id: string, expectedVersion: number, terms: MemoryTerms, idempotencyKey: string): Promise<MemoryMutationResponse> {
  return request<MemoryMutationResponse>(`${idPath(id)}/proposals`, {
    method: 'POST', body: { expected_version: expectedVersion, ...terms }, headers: { 'Idempotency-Key': idempotencyKey },
  })
}

/** One memory's edit requests, pending and decided, newest first. Summaries only; the cursor is passed back unchanged. */
export function listMemoryProposals(id: string, cursor?: string | null, limit: number = MEMORY_PAGE_SIZE): Promise<MemoryProposalListResponse> {
  return request<MemoryProposalListResponse>(`${idPath(id)}/proposals`, { method: 'GET', query: { cursor: cursor ?? undefined, limit } })
}

/** The immutable original and suggestion. */
export function getMemoryProposal(id: string, proposalId: string): Promise<MemoryProposalResponse> {
  return request<MemoryProposalResponse>(proposalPath(id, proposalId), { method: 'GET' })
}

export function decideMemoryProposal(id: string, proposalId: string, outcome: MemoryDecisionOutcome, idempotencyKey: string): Promise<MemoryMutationResponse> {
  return request<MemoryMutationResponse>(`${proposalPath(id, proposalId)}/decision`, {
    method: 'POST', body: { outcome }, headers: { 'Idempotency-Key': idempotencyKey },
  })
}
