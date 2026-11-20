/**
 * Memory edit-request history (contract 0.37.0, web): the summary rules; the store's own read sequence, paging, account and route
 * changes and late answers; discoverability after signing in again; the detail section, which tells a historical approval apart from
 * the memory's current version, opens the existing comparison, and never decides anything; and deleted memories. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/memories', () => ({
  MEMORY_PAGE_SIZE: 20,
  createMemory: vi.fn(), listMemories: vi.fn(), getMemory: vi.fn(), editMemory: vi.fn(), proposeMemoryEdit: vi.fn(),
  getMemoryProposal: vi.fn(), decideMemoryProposal: vi.fn(), listMemoryProposals: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: vi.fn() }),
}))

import { ApiError, setCsrfToken } from '@/api/client'
import { decideMemoryProposal, getMemory, getMemoryProposal, listMemories, listMemoryProposals } from '@/api/memories'
import { isMemoryProposalPage, isMemoryProposalSummary } from '@/api/memory-rules'
import type { Memory, MemoryProposal, MemoryProposalListResponse, MemoryProposalSummary } from '@/api/types'
import { useMemoriesStore } from '@/stores/memories'
import { useSessionStore } from '@/stores/session'
import MemoryDetailView from '@/views/MemoryDetailView.vue'

const P1 = '11111111-1111-4111-8111-111111111111'
const P2 = '22222222-2222-4222-8222-222222222222'

function photo(id: string, position = 1) {
  return { position, media_id: id, uploader_id: 'self', width: 4, height: 3, deleted: false }
}

function memory(id: string, overrides: Partial<Memory> = {}): Memory {
  return {
    id, creator_id: 'partner', version: 3, date: '2026-03-01', title: '海边', description: '', created_at: '2026-03-02T01:00:00.000000Z',
    updated_at: '2026-03-05T01:00:00.000000Z', photos: [photo(P1)], pending_proposal: null, ...overrides,
  }
}

function summary(memoryId: string, id: string, overrides: Partial<MemoryProposalSummary> = {}): MemoryProposalSummary {
  return {
    id, memory_id: memoryId, proposer_id: 'self', base_version: 1, created_at: '2026-03-03T01:00:00.000000Z', status: 'rejected',
    decided_by: 'partner', decided_at: '2026-03-03T02:00:00.000000Z', approved_version: null, ...overrides,
  }
}

const approvedOld = (memoryId: string) => summary(memoryId, 'p-approved', { status: 'approved', approved_version: 2, base_version: 1 })
const withdrawn = (memoryId: string) => summary(memoryId, 'p-withdrawn', { status: 'withdrawn', decided_by: 'self' })
const rejected = (memoryId: string) => summary(memoryId, 'p-rejected')
const pendingOf = (memoryId: string) => summary(memoryId, 'p-pending', { status: 'pending', decided_by: null, decided_at: null, base_version: 3 })

function historyPage(proposals: MemoryProposalSummary[], next: string | null = null, current = 3): MemoryProposalListResponse {
  return { proposals, next_cursor: next, current_version: current }
}

function proposalRecord(memoryId: string, id: string, overrides: Partial<MemoryProposal> = {}): MemoryProposal {
  return {
    id, memory_id: memoryId, proposer_id: 'self', base_version: 1, created_at: '2026-03-03T01:00:00.000000Z', status: 'approved',
    decided_by: 'partner', decided_at: '2026-03-03T02:00:00.000000Z', approved_version: 2,
    original: { version: 1, date: '2026-03-01', title: 'HISTORY-ORIGINAL', description: '', photos: [photo(P1)] },
    proposed: { date: '2026-03-02', title: 'HISTORY-PROPOSED', description: '', photos: [photo(P2)] },
    ...overrides,
  }
}

function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-29T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-29T00:00:00Z', members: [] },
  })
  setCsrfToken(`csrf-${id}`)
}

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

const stubs = { RouterLink: { template: '<a><slot /></a>' } }
const mounted: VueWrapper[] = []
function show(props: Record<string, unknown>): VueWrapper {
  const wrapper = mount(MemoryDetailView as never, { props: props as never, global: { stubs } }) as VueWrapper
  mounted.push(wrapper)
  return wrapper
}
function test(wrapper: VueWrapper, name: string) {
  return wrapper.find(`[data-test="${name}"]`)
}
function all(wrapper: VueWrapper, name: string) {
  return wrapper.findAll(`[data-test="${name}"]`)
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
  vi.mocked(listMemories).mockResolvedValue({ memories: [], next_cursor: null, uploads_enabled: true } as never)
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('rules', () => {
  it('accepts each status only with the decision facts it implies', () => {
    expect(isMemoryProposalSummary(pendingOf('a'), 'a')).toBe(true)
    expect(isMemoryProposalSummary(rejected('a'), 'a')).toBe(true)
    expect(isMemoryProposalSummary(withdrawn('a'), 'a')).toBe(true)
    expect(isMemoryProposalSummary(approvedOld('a'), 'a')).toBe(true)
    // Another memory's, a pending one with a decision, an approval without a newer version, a refusal naming a version.
    expect(isMemoryProposalSummary(rejected('b'), 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...pendingOf('a'), decided_by: 'partner' }, 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...approvedOld('a'), approved_version: null }, 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...approvedOld('a'), approved_version: 1 }, 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...rejected('a'), approved_version: 2 }, 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...rejected('a'), decided_at: null }, 'a')).toBe(false)
    expect(isMemoryProposalSummary({ ...rejected('a'), status: 'deleted' }, 'a')).toBe(false)
    expect(isMemoryProposalPage(historyPage([rejected('a')]), 'a')).toBe(true)
    expect(isMemoryProposalPage({ ...historyPage([rejected('a')]), current_version: 0 }, 'a')).toBe(false)
    expect(isMemoryProposalPage({ proposals: [rejected('a')], next_cursor: null }, 'a')).toBe(false)
  })
})

describe('store', () => {
  it('reads pages in order, keeps the newest current version, and reports a failed page apart', async () => {
    const store = useMemoriesStore()
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([pendingOf('a'), approvedOld('a')], 'c1', 3))
    await store.loadHistory('a')
    expect(listMemoryProposals).toHaveBeenLastCalledWith('a')
    expect(store.history.status).toBe('ready')
    expect(store.history.items.map((p) => p.id)).toEqual(['p-pending', 'p-approved'])
    expect(store.history.currentVersion).toBe(3)
    vi.mocked(listMemoryProposals).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    await store.loadMoreHistory()
    expect(store.history.moreErrorText).not.toBe('')
    expect(store.history.items).toHaveLength(2)
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([approvedOld('a'), withdrawn('a'), rejected('a')], null, 4))
    await store.loadMoreHistory()
    expect(listMemoryProposals).toHaveBeenLastCalledWith('a', 'c1')
    expect(store.history.items.map((p) => p.id)).toEqual(['p-pending', 'p-approved', 'p-withdrawn', 'p-rejected'])
    expect(store.history.nextCursor).toBeNull()
    expect(store.history.currentVersion).toBe(4)
    expect(store.history.moreErrorText).toBe('')
  })

  it('treats a malformed page, or one naming another memory, as an error and shows nothing from it', async () => {
    const store = useMemoriesStore()
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([rejected('b')]))
    await store.loadHistory('a')
    expect(store.history.status).toBe('error')
    expect(store.history.items).toEqual([])
    vi.mocked(listMemoryProposals).mockRejectedValueOnce(new ApiError('MEMORY_NOT_FOUND', '回忆不存在', 404))
    await store.loadHistory('a')
    expect(store.history.status).toBe('error')
    expect(store.history.items).toEqual([])
  })

  it('drops a late answer after the route moves to another memory, after leaving, and after an account switch', async () => {
    const store = useMemoriesStore()
    const late = deferred<MemoryProposalListResponse>()
    vi.mocked(listMemoryProposals).mockReturnValueOnce(late.promise).mockResolvedValueOnce(historyPage([rejected('b')]))
    const first = store.loadHistory('a')
    await store.loadHistory('b')
    late.resolve(historyPage([approvedOld('a')]))
    await first
    expect(store.history.memoryId).toBe('b')
    expect(store.history.items.map((p) => p.memory_id)).toEqual(['b'])

    const afterLeaving = deferred<MemoryProposalListResponse>()
    vi.mocked(listMemoryProposals).mockReturnValueOnce(afterLeaving.promise)
    const leaving = store.loadHistory('a')
    store.closeDetail()
    afterLeaving.resolve(historyPage([approvedOld('a')]))
    await leaving
    expect(store.history.memoryId).toBeNull()
    expect(store.history.items).toEqual([])

    const afterSwitch = deferred<MemoryProposalListResponse>()
    vi.mocked(listMemoryProposals).mockReturnValueOnce(afterSwitch.promise)
    const switching = store.loadHistory('a')
    signIn('other')
    afterSwitch.resolve(historyPage([approvedOld('a')]))
    await switching
    expect(store.history.items).toEqual([])
    expect(store.history.memoryId).toBeNull()
  })

  it('drops a late comparison after the history moved on, and keeps it apart from the pending review', async () => {
    const store = useMemoriesStore()
    vi.mocked(listMemoryProposals).mockResolvedValue(historyPage([approvedOld('a')]))
    await store.loadHistory('a')
    const late = deferred<{ proposal: MemoryProposal }>()
    vi.mocked(getMemoryProposal).mockReturnValueOnce(late.promise as never)
    const opening = store.openHistoryReview('a', 'p-approved')
    expect(store.review.proposalId).toBeNull()
    store.closeHistoryReview()
    late.resolve({ proposal: proposalRecord('a', 'p-approved') })
    await opening
    expect(store.historyReview.proposalId).toBeNull()
    expect(store.historyReview.proposal).toBeNull()
  })

  it('a proposer who signs in again finds their decided request from the server alone', async () => {
    const store = useMemoriesStore()
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([pendingOf('a')]))
    await store.loadHistory('a')
    // Signed out and back in: nothing carried over in memory.
    signIn('other')
    expect(store.history.items).toEqual([])
    setActivePinia(createPinia())
    signIn('self')
    const fresh = useMemoriesStore()
    expect(fresh.history.items).toEqual([])
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([summary('a', 'p-pending', { status: 'rejected', base_version: 3 })]))
    await fresh.loadHistory('a')
    expect(fresh.history.items[0]).toMatchObject({ id: 'p-pending', status: 'rejected', decided_by: 'partner' })
  })
})

describe('detail section', () => {
  async function detailOf(item: Memory, page: MemoryProposalListResponse) {
    vi.mocked(getMemory).mockResolvedValue({ memory: item } as never)
    vi.mocked(listMemoryProposals).mockResolvedValue(page)
    vi.mocked(getMemoryProposal).mockImplementation(async (memoryId: string, proposalId: string) =>
      ({ proposal: proposalRecord(memoryId, proposalId, { status: 'pending', decided_by: null, decided_at: null, approved_version: null, base_version: 3 }) }) as never)
    const wrapper = show({ id: item.id })
    await flushPromises()
    return wrapper
  }

  it('lists every status, tells an old approval from the current version, and leaves deciding to the existing controls', async () => {
    const item = memory('a', { pending_proposal: { id: 'p-pending', proposer_id: 'self', base_version: 3, created_at: 'x' } })
    const wrapper = await detailOf(item, historyPage([pendingOf('a'), approvedOld('a'), withdrawn('a'), rejected('a')], null, 3))
    expect(listMemoryProposals).toHaveBeenCalledWith('a')
    const rows = all(wrapper, 'memory-history-item')
    expect(rows.map((row) => row.attributes('data-proposal'))).toEqual(['p-pending', 'p-approved', 'p-withdrawn', 'p-rejected'])
    expect(rows.map((row) => row.find('[data-test="memory-history-status"]').text())).toEqual(['待处理', '已同意', '已撤回', '已拒绝'])
    expect(test(wrapper, 'memory-history-note').text()).toContain('不代表回忆现在的内容')
    expect(test(wrapper, 'memory-history-note').text()).toContain('第 3 版')
    const version = rows[1].find('[data-test="memory-history-version"]').text()
    expect(version).toContain('第 2 版')
    expect(version).toContain('现在是第 3 版')
    expect(version).not.toContain('也就是现在的版本')
    expect(rows[2].find('[data-test="memory-history-decision"]').text()).toContain('我于')
    expect(rows[3].find('[data-test="memory-history-decision"]').text()).toContain('对方于')
    // The pending one points at the controls above and has no comparison or control of its own in the history.
    expect(rows[0].find('[data-test="memory-history-pending"]').exists()).toBe(true)
    expect(rows[0].find('[data-test="memory-history-open"]').exists()).toBe(false)
    expect(test(wrapper, 'memory-history').findAll('button').map((b) => b.attributes('data-test'))).not.toContain('memory-withdraw')
    expect(test(wrapper, 'memory-withdraw').exists()).toBe(true)
  })

  it('says when an approval is still the current version', async () => {
    const wrapper = await detailOf(memory('a', { version: 2 }), historyPage([approvedOld('a')], null, 2))
    expect(test(wrapper, 'memory-history-version').text()).toContain('也就是现在的版本')
  })

  it('opens a decided request in the existing comparison, labelled as the version it was based on', async () => {
    const wrapper = await detailOf(memory('a'), historyPage([approvedOld('a')]))
    vi.mocked(getMemoryProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'p-approved') } as never)
    await test(wrapper, 'memory-history-open').trigger('click')
    await flushPromises()
    expect(getMemoryProposal).toHaveBeenLastCalledWith('a', 'p-approved')
    expect(test(wrapper, 'memory-history-review-original').text()).toContain('申请时的内容（第 1 版）')
    expect(test(wrapper, 'memory-history-review-original').text()).toContain('HISTORY-ORIGINAL')
    expect(test(wrapper, 'memory-history-review-proposed').text()).toContain('HISTORY-PROPOSED')
    expect(test(wrapper, 'memory-history-diff-title').exists()).toBe(true)
    expect(test(wrapper, 'memory-history-open').text()).toBe('收起对比')
    await test(wrapper, 'memory-history-open').trigger('click')
    expect(test(wrapper, 'memory-history-review').exists()).toBe(false)
  })

  it('pages with a button, shows an empty history plainly, and offers a retry on failure', async () => {
    const wrapper = await detailOf(memory('a'), historyPage([rejected('a')], 'c1'))
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([withdrawn('a')], null))
    await test(wrapper, 'memory-history-more').trigger('click')
    await flushPromises()
    expect(all(wrapper, 'memory-history-item')).toHaveLength(2)
    expect(test(wrapper, 'memory-history-more').exists()).toBe(false)

    const empty = await detailOf(memory('b'), historyPage([]))
    expect(test(empty, 'memory-history-empty').exists()).toBe(true)

    vi.mocked(getMemory).mockResolvedValue({ memory: memory('c') } as never)
    vi.mocked(listMemoryProposals).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    const failed = show({ id: 'c' })
    await flushPromises()
    expect(test(failed, 'memory-history-error').exists()).toBe(true)
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([rejected('c')]))
    await test(failed, 'memory-history-retry').trigger('click')
    await flushPromises()
    expect(all(failed, 'memory-history-item')).toHaveLength(1)
  })

  it('reads the history again after a decision through the existing controls', async () => {
    const item = memory('a', { creator_id: 'self', pending_proposal: { id: 'p-pending', proposer_id: 'partner', base_version: 3, created_at: 'x' } })
    const wrapper = await detailOf(item, historyPage([{ ...pendingOf('a'), proposer_id: 'partner' }]))
    expect(listMemoryProposals).toHaveBeenCalledTimes(1)
    vi.mocked(decideMemoryProposal).mockResolvedValueOnce({
      mutation: { action: 'decide', memory_id: 'a', proposal_id: 'p-pending', outcome: 'rejected', decided_by: 'self', version: null },
      memory: memory('a', { creator_id: 'self' }),
    } as never)
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([summary('a', 'p-pending', { proposer_id: 'partner', decided_by: 'self', base_version: 3 })]))
    await test(wrapper, 'memory-reject').trigger('click')
    await flushPromises()
    expect(listMemoryProposals).toHaveBeenCalledTimes(2)
    expect(test(wrapper, 'memory-history-status').text()).toBe('已拒绝')
  })

  it('does not read the history again when a photo refresh returns the same version', async () => {
    await detailOf(memory('a'), historyPage([approvedOld('a')]))
    expect(listMemoryProposals).toHaveBeenCalledTimes(1)
    await useMemoriesStore().refreshAfterPhotoChange()
    await flushPromises()
    expect(getMemory).toHaveBeenCalledTimes(2)
    expect(listMemoryProposals).toHaveBeenCalledTimes(1)
  })

  it('a deleted memory shows no history, never asks for it, and loads no photo through it', async () => {
    vi.mocked(getMemory).mockRejectedValue(new ApiError('MEMORY_NOT_FOUND', '回忆不存在', 404))
    const wrapper = show({ id: 'gone' })
    await flushPromises()
    expect(test(wrapper, 'memory-history').exists()).toBe(false)
    expect(listMemoryProposals).not.toHaveBeenCalled()
    expect(getMemoryProposal).not.toHaveBeenCalled()
    expect(wrapper.findAll('img')).toHaveLength(0)
  })

  it('clears an open history and comparison when the memory is deleted, or reads as gone, while the page is open', async () => {
    const wrapper = await detailOf(memory('a'), historyPage([approvedOld('a')]))
    vi.mocked(getMemoryProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'p-approved') } as never)
    await test(wrapper, 'memory-history-open').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'memory-history-review').exists()).toBe(true)
    const store = useMemoriesStore()
    vi.mocked(getMemory).mockRejectedValue(new ApiError('MEMORY_NOT_FOUND', '回忆不存在', 404))
    store.refreshAfterMemoryChange('a', true)
    await flushPromises()
    expect(store.history.items).toEqual([])
    expect(store.historyReview.proposal).toBeNull()
    expect(test(wrapper, 'memory-history').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('HISTORY-ORIGINAL')
  })

  it('moving to another memory or account never shows the previous one\'s history', async () => {
    const late = deferred<MemoryProposalListResponse>()
    vi.mocked(getMemory).mockImplementation(async (id: string) => ({ memory: memory(id) }) as never)
    vi.mocked(listMemoryProposals).mockReturnValueOnce(late.promise).mockResolvedValueOnce(historyPage([rejected('b')]))
    const wrapper = show({ id: 'a' })
    await flushPromises()
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    late.resolve(historyPage([approvedOld('a')]))
    await flushPromises()
    expect(all(wrapper, 'memory-history-item').map((row) => row.attributes('data-proposal'))).toEqual(['p-rejected'])
    expect(useMemoriesStore().history.memoryId).toBe('b')

    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([withdrawn('b')]))
    signIn('other')
    await flushPromises()
    const rows = all(wrapper, 'memory-history-item')
    expect(rows.map((row) => row.attributes('data-proposal'))).toEqual(['p-withdrawn'])
    // For the new account, the old account's own request is the partner's.
    expect(rows[0].text()).toContain('对方提交的申请')
  })
})

describe('what stays private', () => {
  it('no history source touches storage, caches or the console', () => {
    for (const path of ['stores/memories.ts', 'api/memories.ts', 'api/memory-rules.ts', 'components/MemoryProposalCompare.vue', 'views/MemoryDetailView.vue']) {
      const source = readFileSync(resolve(__dirname, '../../src', path), 'utf8')
      expect(source, path).not.toMatch(/localStorage|sessionStorage|indexedDB|caches\.|console\./)
    }
  })

  it('a full history flow writes nothing to storage', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem')
    const store = useMemoriesStore()
    vi.mocked(listMemoryProposals).mockResolvedValueOnce(historyPage([approvedOld('a')]))
    await store.loadHistory('a')
    vi.mocked(getMemoryProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'p-approved') } as never)
    await store.openHistoryReview('a', 'p-approved')
    expect(store.historyReview.status).toBe('ready')
    expect(setItem).not.toHaveBeenCalled()
    expect(localStorage.length).toBe(0)
    expect(sessionStorage.length).toBe(0)
  })
})
