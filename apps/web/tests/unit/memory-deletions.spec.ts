import { createPinia, setActivePinia } from 'pinia'
import { flushPromises, mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'
vi.mock('@/api/memory-deletions', async original => ({
  ...(await original<typeof import('@/api/memory-deletions')>()),
  listMemoryDeletions: vi.fn(), listRetainedPhotos: vi.fn(), sendMemoryDeletion: vi.fn(),
}))
vi.mock('@/api/memories', () => ({ getMemory: vi.fn(), listMemories: vi.fn(), getMemoryProposal: vi.fn(), listMemoryProposals: vi.fn() }))
import { listMemoryDeletions, listRetainedPhotos, sendMemoryDeletion } from '@/api/memory-deletions'
import { getMemory, listMemories } from '@/api/memories'
import { ApiError, setCsrfToken } from '@/api/client'
import { useSessionStore } from '@/stores/session'
import { useMemoriesStore } from '@/stores/memories'
import { useMemoryDeletionsStore } from '@/stores/memory-deletions'
import MemoryDeletionControls from '@/components/MemoryDeletionControls.vue'
import MemoryRecoveryView from '@/views/MemoryRecoveryView.vue'
import MemoryDetailView from '@/views/MemoryDetailView.vue'
import type { Memory } from '@/api/types'

const now = '2026-11-05T01:00:00Z'
const end = '2026-11-12T01:00:00Z'
const cycle = { memory_id: 'm1', deletion_id: 'd1', cycle_no: 1, deleted_at: now, purge_after: end, state: 'deleted', undone_at: null }
const row = { deletion_id: 'd1', memory_id: 'm1', title: '海边', deleted_at: now, purge_after: end }
const memory = { id: 'm1', creator_id: 'self', version: 1, date: '2026-11-05', title: '海边', description: 'private',
  photos: [], pending_proposal: null, pending_delete_request: null, created_at: now, updated_at: now } as Memory
function deferred<T>() {
  let resolve!: (value: T) => void
  const promise = new Promise<T>(r => { resolve = r })
  return { resolve, promise }
}
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  useSessionStore().user = { id: 'self' } as never
  vi.spyOn(useSessionStore(), 'ensureCsrfToken').mockResolvedValue(undefined)
  setCsrfToken('csrf')
  vi.mocked(listMemories).mockResolvedValue({ memories: [], next_cursor: null, uploads_enabled: false })
  vi.mocked(getMemory).mockRejectedValue(new ApiError('MEMORY_NOT_FOUND', '', 404))
  vi.mocked(listMemoryDeletions).mockResolvedValue({ deletions: [], next_cursor: null })
  vi.mocked(listRetainedPhotos).mockResolvedValue({ photos: [], next_cursor: null })
})

describe('memory deletion state', () => {
  it('keeps the original key across a lost response and an authentication refusal', async () => {
    const store = useMemoryDeletionsStore()
    vi.mocked(sendMemoryDeletion).mockRejectedValueOnce(new Error('lost')).mockRejectedValueOnce(new ApiError('UNAUTHENTICATED', '', 401))
      .mockResolvedValueOnce({ deletion: cycle })
    await store.submit({ kind: 'delete', memoryId: 'm1' })
    const key = store.actions.m1?.key
    await store.retry('m1')
    expect(store.actions.m1?.state).toBe('unknown')
    await store.retry('m1')
    expect(store.actions.m1).toBeUndefined()
    expect(vi.mocked(sendMemoryDeletion).mock.calls).toEqual(Array.from({ length: 3 }, () => [{ kind: 'delete', memoryId: 'm1' }, key]))
  })
  it('keeps contradictory exact-cycle responses unknown', async () => {
    const store = useMemoryDeletionsStore()
    vi.mocked(sendMemoryDeletion).mockResolvedValue({ deletion: { ...cycle, deletion_id: 'other', state: 'undone', undone_at: now } })
    await store.submit({ kind: 'undo', memoryId: 'm1', deletionId: 'd1' })
    expect(store.actions.m1?.state).toBe('unknown')
    expect(store.actions.m1?.command).toMatchObject({ deletionId: 'd1' })
  })
  it.each(['MEMORY_UNDO_CLOSED', 'MEMORY_DELETION_NOT_ACTIVE'])('refreshes and retires exact undo for %s', async code => {
    const store = useMemoryDeletionsStore()
    await store.load('recovery')
    vi.mocked(sendMemoryDeletion).mockRejectedValue(new ApiError(code, code, 409))
    await store.submit({ kind: 'undo', memoryId: 'm1', deletionId: 'd1' })
    expect(store.actions.m1).toBeUndefined()
    await flushPromises()
    expect(listMemoryDeletions).toHaveBeenCalledTimes(2)
  })
  it('states only the old undo cycle when a later deletion may exist', async () => {
    const store = useMemoryDeletionsStore()
    vi.mocked(sendMemoryDeletion).mockResolvedValue({ deletion: { ...cycle, state: 'undone', undone_at: now } })
    await store.submit({ kind: 'undo', memoryId: 'm1', deletionId: 'd1' })
    expect(store.notice).toContain('该次删除')
    expect(store.notice).not.toContain('已恢复')
  })
  it('clears a loaded private memory and its review after delete', async () => {
    const memories = useMemoriesStore()
    memories.items = [memory]
    memories.detail = { id: 'm1', status: 'ready', item: memory, errorText: '' }
    memories.review = { proposalId: 'p1', status: 'ready', proposal: { original: memory, proposed: memory } as never, errorText: '' }
    vi.mocked(sendMemoryDeletion).mockResolvedValue({ deletion: cycle })
    await useMemoryDeletionsStore().submit({ kind: 'delete', memoryId: 'm1' })
    await flushPromises()
    expect(memories.items).toEqual([])
    expect(memories.detail.item).toBeNull()
    expect(memories.review.proposal).toBeNull()
  })
  it('rejects the wrong requester on approval', async () => {
    const store = useMemoryDeletionsStore()
    vi.mocked(sendMemoryDeletion).mockResolvedValue({ request: { request_id: 'r1', memory_id: 'm1', requester_id: 'someone-else',
      created_at: now, status: 'approved', decided_by: 'self', decided_at: now }, deletion: cycle })
    await store.submit({ kind: 'decide', memoryId: 'm1', requestId: 'r1', requesterId: 'partner', outcome: 'approve' })
    expect(store.actions.m1?.state).toBe('unknown')
  })
  it('discards a late result after an identity change', async () => {
    const store = useMemoryDeletionsStore()
    const wait = deferred<unknown>()
    vi.mocked(sendMemoryDeletion).mockReturnValue(wait.promise)
    const write = store.submit({ kind: 'delete', memoryId: 'm1' })
    await flushPromises()
    store.reset()
    useSessionStore().user = { id: 'partner' } as never
    wait.resolve({ deletion: cycle })
    await write
    expect(store.actions).toEqual({})
    expect(store.notice).toBe('')
  })
  it('paginates by exact cursor and rejects a repeated cursor', async () => {
    const store = useMemoryDeletionsStore()
    vi.mocked(listMemoryDeletions).mockResolvedValueOnce({ deletions: [row], next_cursor: 'page2' })
      .mockResolvedValueOnce({ deletions: [row], next_cursor: 'page2' })
    await store.load('recovery')
    await store.load('recovery', true)
    expect(listMemoryDeletions).toHaveBeenLastCalledWith('page2')
    expect(store.recovery).toHaveLength(1)
    expect(store.error).not.toBe('')
  })
  it('loads the server recovery list in a fresh store and renders no media', async () => {
    vi.mocked(listMemoryDeletions).mockResolvedValue({ deletions: [row], next_cursor: null })
    const view = mount(MemoryRecoveryView, { props: { kind: 'recovery' }, global: { stubs: ['RouterLink'] } })
    await flushPromises()
    expect(view.get('[data-test="memory-recovery-row"]').text()).toContain(row.title)
    expect(view.find('img').exists()).toBe(false)
    await view.get('[data-test="memory-undo"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(sendMemoryDeletion).mock.calls[0]?.[0]).toEqual({ kind: 'undo', memoryId: 'm1', deletionId: 'd1' })
    view.unmount()
  })
})
describe('memory deletion controls', () => {
  it('hides the body editor while a delete request is pending', async () => {
    const memories = useMemoriesStore()
    memories.detail = { id: 'm1', status: 'ready', item: { ...memory,
      pending_delete_request: { id: 'r1', requester_id: 'partner', created_at: now } }, errorText: '' }
    const view = mount(MemoryDetailView, { props: { id: 'm1' }, global: { stubs: ['RouterLink'] } })
    expect(view.find('[data-test="memory-start-write"]').exists()).toBe(false)
    expect(view.find('[data-test="memory-delete-approve"]').exists()).toBe(true)
    view.unmount()
  })
  it('requires confirmation and describes cascade, retained photos and plan text', async () => {
    const view = mount(MemoryDeletionControls, { props: { memoryId: 'm1', memory, busy: false } })
    await view.get('[data-test="memory-delete"]').trigger('click')
    expect(sendMemoryDeletion).not.toHaveBeenCalled()
    expect(view.get('[data-test="memory-delete-explanation"]').text()).toContain('伴侣上传的照片由伴侣保留')
    expect(view.text()).toContain('原计划自己的文字会保留')
    vi.mocked(sendMemoryDeletion).mockResolvedValue({ deletion: cycle })
    await view.get('[data-test="memory-delete-confirm"]').trigger('click')
    await flushPromises()
    expect(sendMemoryDeletion).toHaveBeenCalledTimes(1)
    view.unmount()
  })
  it('offers only withdrawal to the partner with a pending delete request', async () => {
    useSessionStore().user = { id: 'partner' } as never
    const view = mount(MemoryDeletionControls, { props: { memoryId: 'm1', busy: false, memory: { ...memory,
      pending_delete_request: { id: 'r1', requester_id: 'partner', created_at: now } } } })
    expect(view.find('[data-test="memory-delete-withdraw"]').exists()).toBe(true)
    expect(view.find('[data-test="memory-delete"]').exists()).toBe(false)
    expect(view.find('[data-test="memory-delete-approve"]').exists()).toBe(false)
    view.unmount()
  })
})
