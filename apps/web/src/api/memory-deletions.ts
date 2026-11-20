import { request } from './client'

export interface MemoryCycle {
  deletion_id: string
  memory_id: string
  cycle_no: number
  deleted_at: string
  purge_after: string
  undone_at: string | null
  state: 'deleted' | 'undone'
}
export interface MemoryRecovery {
  deletion_id: string
  memory_id: string
  title: string
  deleted_at: string
  purge_after: string
}
export interface DeleteRequest {
  request_id: string
  memory_id: string
  requester_id: string
  created_at: string
  status: 'pending' | 'approved' | 'rejected' | 'withdrawn'
  decided_by: string | null
  decided_at: string | null
}
export interface RetainedPhoto { media_id: string; uploaded_at: string; width: number; height: number }
export type MemoryDeleteCommand =
  | { kind: 'delete'; memoryId: string }
  | { kind: 'undo'; memoryId: string; deletionId: string }
  | { kind: 'request'; memoryId: string }
  | { kind: 'decide'; memoryId: string; requestId: string; requesterId: string; outcome: 'approve' | 'reject' | 'withdraw' }

const path = (id: string) => '/api/v1/memories/' + encodeURIComponent(id)
export function sendMemoryDeletion(command: MemoryDeleteCommand, key: string): Promise<unknown> {
  const options = { method: 'POST' as const, headers: { 'Idempotency-Key': key } }
  switch (command.kind) {
    case 'delete': return request(path(command.memoryId) + '/deletions', options)
    case 'undo': return request(path(command.memoryId) + '/deletions/' + encodeURIComponent(command.deletionId) + '/undo', options)
    case 'request': return request(path(command.memoryId) + '/delete-requests', options)
    case 'decide': return request(path(command.memoryId) + '/delete-requests/' + encodeURIComponent(command.requestId) + '/decision',
      { ...options, body: { outcome: command.outcome } })
  }
}
export function listMemoryDeletions(cursor?: string | null): Promise<{ deletions: MemoryRecovery[]; next_cursor: string | null }> {
  return request('/api/v1/memories/deletions', { query: { cursor: cursor ?? undefined, limit: 20 } })
}
export function listRetainedPhotos(cursor?: string | null): Promise<{ photos: RetainedPhoto[]; next_cursor: string | null }> {
  return request('/api/v1/media/retained', { query: { cursor: cursor ?? undefined, limit: 20 } })
}
const record = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v)
const id = (v: unknown): v is string => typeof v === 'string' && v.length > 0
const time = (v: unknown): v is string => typeof v === 'string' && Number.isFinite(Date.parse(v))
export function isRecovery(value: unknown): value is MemoryRecovery {
  return record(value) && id(value.memory_id) && id(value.deletion_id) && time(value.deleted_at) && time(value.purge_after) &&
    Date.parse(value.purge_after) > Date.parse(value.deleted_at) && typeof value.title === 'string'
}
export function isRetained(value: unknown): value is RetainedPhoto {
  return record(value) && id(value.media_id) && time(value.uploaded_at) && Number.isInteger(value.width) &&
    Number.isInteger(value.height) && Number(value.width) > 0 && Number(value.height) > 0
}
export function isCycle(value: unknown): value is MemoryCycle {
  return record(value) && id(value.memory_id) && id(value.deletion_id) && Number.isInteger(value.cycle_no) && Number(value.cycle_no) > 0 &&
    time(value.deleted_at) && time(value.purge_after) && (value.state === 'deleted' ? value.undone_at === null :
      value.state === 'undone' && time(value.undone_at))
}
export function validMemoryDeleteResponse(value: unknown, command: MemoryDeleteCommand, owner: string): boolean {
  if (!record(value)) return false
  if (command.kind === 'delete' || command.kind === 'undo') {
    return isCycle(value.deletion) && value.deletion.memory_id === command.memoryId &&
      (command.kind === 'delete' || (value.deletion.deletion_id === command.deletionId && value.deletion.state === 'undone'))
  }
  const r = value.request
  if (!record(r) || !id(r.request_id) || r.memory_id !== command.memoryId || !time(r.created_at)) return false
  if (r.status === 'pending' ? r.decided_by !== null || r.decided_at !== null :
    !['approved', 'rejected', 'withdrawn'].includes(String(r.status)) || !id(r.decided_by) || !time(r.decided_at)) return false
  if (command.kind === 'request') return r.requester_id === owner
  const expected = { approve: 'approved', reject: 'rejected', withdraw: 'withdrawn' }
  return r.request_id === command.requestId && r.requester_id === command.requesterId && r.decided_by === owner &&
    r.status === expected[command.outcome] && (command.outcome !== 'approve' ||
      (isCycle(value.deletion) && value.deletion.memory_id === command.memoryId))
}
