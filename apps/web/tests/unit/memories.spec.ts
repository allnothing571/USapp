/**
 * P3-1c photo memories (web): the rules; the timeline's states and paging; unknown-result retries with the captured key and payload;
 * approval replay; the upload queue's separate failure states; account and route changes; edit permissions; the review;
 * and what must stay out of storage, logs and the service worker's cache. All data is fake.
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
vi.mock('@/api/media', async (original) => ({
  ...(await original<typeof import('@/api/media')>()),
  createMediaUpload: vi.fn(), completeMediaUpload: vi.fn(), postToStaging: vi.fn(),
}))
vi.mock('@/api/media-capability', async (original) => ({
  ...(await original<typeof import('@/api/media-capability')>()),
  getMediaCapabilities: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
const routerPush = vi.fn()
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: routerPush }),
}))

import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { completeMediaUpload, createMediaUpload, isUploadForm, mediaContentUrl, postToStaging, StagingUploadError } from '@/api/media'
import {
  createMemory,
  decideMemoryProposal,
  editMemory,
  getMemory,
  getMemoryProposal,
  listMemories,
  proposeMemoryEdit,
} from '@/api/memories'
import { beijingToday, checkMemoryTerms, sameMemoryTerms } from '@/api/memory-rules'
import { getMediaCapabilities } from '@/api/media-capability'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import type { MediaResponse, Memory, MemoryMutationResponse, MemoryProposal, MemoryTerms } from '@/api/types'
import { isCacheableRequest, isSensitivePath } from '@/pwa/cache-policy'
import { isUsableMemoryResponse, useMemoriesStore, type MemoryAction } from '@/stores/memories'
import { MAX_CONCURRENT_UPLOADS, useMemoryUploadsStore } from '@/stores/memory-uploads'
import { useSessionStore } from '@/stores/session'
import MemoryEditor from '@/components/MemoryEditor.vue'
import MemoriesView from '@/views/MemoriesView.vue'
import MemoryDetailView from '@/views/MemoryDetailView.vue'
import MemoryNewView from '@/views/MemoryNewView.vue'

const P1 = '11111111-1111-4111-8111-111111111111'
const P2 = '22222222-2222-4222-8222-222222222222'
const P3 = '33333333-3333-4333-8333-333333333333'
const SIGNED = 'https://bucket.example.test/staging/fake-secret-path'
const SIGNATURE = 'fake-signature-value'

function photo(id: string, position = 1) {
  return { position, media_id: id, uploader_id: 'self', width: 4, height: 3, deleted: false }
}

function memory(id: string, overrides: Partial<Memory> = {}): Memory {
  return {
    id, creator_id: 'self', version: 1, date: '2026-03-01', title: '海边', description: '', created_at: '2026-03-02T01:00:00.000000Z',
    updated_at: '2026-03-02T01:00:00.000000Z', photos: [photo(P1)], pending_proposal: null, ...overrides,
  }
}

function terms(overrides: Partial<MemoryTerms> = {}): MemoryTerms {
  return { date: '2026-03-01', title: '海边', description: '', media_ids: [P1], ...overrides }
}

function page(memories: Memory[], nextCursor: string | null = null, uploadsEnabled: unknown = true) {
  return { memories, next_cursor: nextCursor, uploads_enabled: uploadsEnabled } as never
}

function created(m: Memory, actor = 'self'): MemoryMutationResponse {
  // D20 (0.41.0): a direct create always carries its original notification decision; the page sends false unless opted in.
  return { mutation: { action: 'create', memory_id: m.id, version: 1, actor_id: actor }, memory: m,
    notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null } } as MemoryMutationResponse
}

function approval(m: Memory, proposalId: string, version: number, decidedBy = 'self'): MemoryMutationResponse {
  return {
    mutation: { action: 'decide', memory_id: m.id, proposal_id: proposalId, outcome: 'approved', decided_by: decidedBy, base_version: version - 1, version },
    memory: m,
  }
}

function proposalRecord(memoryId: string, proposalId: string, overrides: Partial<MemoryProposal> = {}): MemoryProposal {
  return {
    id: proposalId, memory_id: memoryId, proposer_id: 'partner', base_version: 1, created_at: '2026-03-03T01:00:00.000000Z', status: 'pending',
    decided_by: null, decided_at: null, approved_version: null,
    original: { version: 1, date: '2026-03-01', title: '海边', description: '原来', photos: [photo(P1)] },
    proposed: { date: '2026-03-02', title: '山上', description: '原来', photos: [photo(P2), photo(P1, 2)] },
    ...overrides,
  }
}

function grantResponse(id: string, overrides: Partial<MediaResponse> = {}): MediaResponse {
  return {
    media: { media_id: id, state: 'granted', uploader_id: 'self', content_type: 'image/png', size_bytes: 3, created_at: '2026-03-03T00:00:00Z', reason: null, width: null, height: null },
    upload: { url: SIGNED, fields: { key: 'staging/x', policy: 'p', signature: SIGNATURE }, expires_at: new Date(Date.now() + 600_000).toISOString() },
    ...overrides,
  }
}

function decided(id: string, state: 'ready' | 'rejected' = 'ready', reason: string | null = null): MediaResponse {
  const base = grantResponse(id)
  return { media: { ...base.media, state, reason, width: 4, height: 3 }, upload: null }
}

function file(name = 'private-name.png', type = 'image/png', size = 3): File {
  const out = new File(['x'.repeat(Math.min(size, 16))], name, { type })
  Object.defineProperty(out, 'size', { value: size })
  return out
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

const network = () => new ApiError('NETWORK_ERROR', 'x', 0)

const stubs = { RouterLink: { template: '<a><slot /></a>' } }
const mounted: VueWrapper[] = []
function show<T extends object>(component: T, props: Record<string, unknown> = {}): VueWrapper {
  const wrapper = mount(component as never, { props: props as never, global: { stubs } }) as VueWrapper
  mounted.push(wrapper)
  return wrapper
}
function test(wrapper: VueWrapper, name: string) {
  return wrapper.find(`[data-test="${name}"]`)
}

async function pickFiles(wrapper: VueWrapper, name: string, files: File[]): Promise<void> {
  const input = test(wrapper, `${name}-files`)
  Object.defineProperty(input.element, 'files', { value: files, configurable: true })
  await input.trigger('change')
  await flushPromises()
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
  vi.mocked(listMemories).mockResolvedValue(page([]))
  // Pickers learn upload capability from its own query (contract 0.40.0), not from the timeline.
  vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('rules', () => {
  it('holds the reviewed limits and normalises what is submitted', () => {
    const ok = checkMemoryTerms({ date: '2026-03-01', title: ' 海边 ', description: ' a\r\nb\n', mediaIds: [P1] })
    expect(ok).toEqual({ ok: true, terms: { date: '2026-03-01', title: '海边', description: 'a\nb', media_ids: [P1] } })
    const draft = { date: '2026-03-01', title: 't', description: '', mediaIds: [P1] }
    expect(checkMemoryTerms({ ...draft, title: '天'.repeat(60) }).ok).toBe(true)
    expect(checkMemoryTerms({ ...draft, title: '天'.repeat(61) }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, description: '天'.repeat(2000) }).ok).toBe(true)
    expect(checkMemoryTerms({ ...draft, description: '天'.repeat(2001) }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, date: '1899-12-31' }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, date: '2023-02-29' }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, mediaIds: [] }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, mediaIds: [P1, P1] }).ok).toBe(false)
    expect(checkMemoryTerms({ ...draft, mediaIds: Array.from({ length: 10 }, (_, i) => `id-${i}`) }).ok).toBe(false)
  })

  it('treats photo order as part of the terms, and takes today from Beijing', () => {
    expect(sameMemoryTerms(terms({ media_ids: [P1, P2] }), terms({ media_ids: [P2, P1] }))).toBe(false)
    expect(beijingToday(new Date('2026-03-01T16:30:00Z'))).toBe('2026-03-02')
  })

  it('accepts only a usable form', () => {
    const form = grantResponse(P1).upload
    expect(isUploadForm(form)).toBe(true)
    expect(isUploadForm({ ...form, url: 'javascript:alert(1)' })).toBe(false)
    expect(isUploadForm({ ...form, url: 'http://bucket.example.test/' })).toBe(false)
    expect(isUploadForm({ ...form, fields: { key: 1 } })).toBe(false)
    expect(isUploadForm({ ...form, expires_at: 'later' })).toBe(false)
    expect(isUploadForm(null)).toBe(false)
  })
})

describe('timeline', () => {
  it('pages by cursor, keeps the first copy of a moved row, and reports a failed later page without losing the first', async () => {
    const store = useMemoriesStore()
    vi.mocked(listMemories).mockResolvedValueOnce(page([memory('a'), memory('b')], 'c1'))
    await store.load()
    expect(store.items.map((m) => m.id)).toEqual(['a', 'b'])
    vi.mocked(listMemories).mockResolvedValueOnce(page([memory('b', { title: 'moved' }), memory('c')], 'c2'))
    await store.loadMore()
    expect(listMemories).toHaveBeenLastCalledWith('c1')
    expect(store.items.map((m) => m.id)).toEqual(['a', 'b', 'c'])
    expect(store.items[1].title).toBe('海边')
    expect(store.nextCursor).toBe('c2')

    vi.mocked(listMemories).mockRejectedValueOnce(network())
    await store.loadMore()
    expect(store.items).toHaveLength(3)
    expect(store.moreErrorText).not.toBe('')
    expect(store.status).toBe('ready')
    // A malformed later page is a failure too, and the walk can continue from the same cursor.
    vi.mocked(listMemories).mockResolvedValueOnce({ memories: [{ id: 'x' }], next_cursor: null } as never)
    await store.loadMore()
    expect(store.items).toHaveLength(3)
    expect(store.nextCursor).toBe('c2')
    vi.mocked(listMemories).mockResolvedValueOnce(page([memory('d')]))
    await store.loadMore()
    expect(listMemories).toHaveBeenLastCalledWith('c2')
    expect(store.nextCursor).toBeNull()
    await store.loadMore()
    expect(listMemories).toHaveBeenCalledTimes(5)
  })

  it.each([
    ['not an object', 'nope'],
    ['no list', { next_cursor: null }],
    ['a bad row', { memories: [{ id: 'a', photos: 'none' }], next_cursor: null }],
    ['a bad photo', { memories: [memory('a', { photos: [{ position: 1 } as never] })], next_cursor: null }],
    ['a bad cursor', { memories: [], next_cursor: 7 }],
  ])('treats a malformed list as an error: %s', async (_name, response) => {
    vi.mocked(listMemories).mockResolvedValue(response as never)
    const store = useMemoriesStore()
    await store.load()
    expect(store.status).toBe('error')
    expect(store.items).toEqual([])
    expect(store.uploadsEnabled).toBeNull()
  })

  it('with uploads off, keeps memories readable, explains why, and offers no way to add', async () => {
    vi.mocked(listMemories).mockResolvedValue(page([memory('a')], null, false))
    const wrapper = show(MemoriesView)
    await flushPromises()
    expect(test(wrapper, 'memory-a').text()).toContain('海边')
    expect(test(wrapper, 'memories-uploads-off').text()).toContain('暂未开放')
    expect(test(wrapper, 'memories-new').exists()).toBe(false)
  })

  it('with the flag missing, treats uploads as unavailable and keeps memories readable', async () => {
    vi.mocked(listMemories).mockResolvedValue({ memories: [memory('a')], next_cursor: null } as never)
    const wrapper = show(MemoriesView)
    await flushPromises()
    expect(useMemoriesStore().uploadsEnabled).toBeNull()
    expect(test(wrapper, 'memory-a').exists()).toBe(true)
    expect(test(wrapper, 'memories-uploads-unknown').exists()).toBe(true)
    expect(test(wrapper, 'memories-new').exists()).toBe(false)
  })

  it('offers to add only when uploads are on, shows empty and retryable error states', async () => {
    vi.mocked(listMemories).mockRejectedValueOnce(network()).mockResolvedValueOnce(page([]))
    const wrapper = show(MemoriesView)
    await flushPromises()
    expect(test(wrapper, 'memories-error').exists()).toBe(true)
    await test(wrapper, 'memories-reload').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'memories-empty').exists()).toBe(true)
    expect(test(wrapper, 'memories-new').exists()).toBe(true)
  })

  it('renders through the authenticated route, and a missing photo does not break its row or the others', async () => {
    vi.mocked(listMemories).mockResolvedValue(page([memory('a', { photos: [photo(P1), photo(P2, 2)] }), memory('b', { photos: [photo(P3)] })], 'next'))
    const wrapper = show(MemoriesView)
    await flushPromises()
    const images = wrapper.findAll('[data-test="memory-photo-image"]')
    expect(images.map((i) => i.attributes('src'))).toEqual([
      `/api/v1/media/${P1}/content?variant=thumb`, `/api/v1/media/${P2}/content?variant=thumb`, `/api/v1/media/${P3}/content?variant=thumb`,
    ])
    expect(images[0].attributes('referrerpolicy')).toBe('no-referrer')
    await images[0].trigger('error')
    expect(wrapper.findAll('[data-test="memory-photo-missing"]')).toHaveLength(1)
    expect(wrapper.findAll('[data-test="memory-photo-image"]')).toHaveLength(2)
    expect(test(wrapper, 'memory-a').text()).toContain('海边')
    expect(test(wrapper, 'memories-more').exists()).toBe(true)
  })
})

describe('late results', () => {
  it('drops a list answer that arrives after an account switch', async () => {
    const store = useMemoriesStore()
    const pending = deferred<never>()
    vi.mocked(listMemories).mockReturnValueOnce(pending.promise)
    const loading = store.load()
    signIn('other')
    store.reset()
    pending.resolve(page([memory('old')], null, true))
    await loading
    expect(store.items).toEqual([])
    expect(store.uploadsEnabled).toBeNull()
    expect(store.status).toBe('idle')
  })

  it('drops a detail or review answer after leaving the page, and the older of two detail reads', async () => {
    const store = useMemoriesStore()
    const first = deferred<never>()
    vi.mocked(getMemory).mockReturnValueOnce(first.promise)
    const opening = store.openDetail('a')
    store.closeDetail()
    first.resolve({ memory: memory('a') } as never)
    await opening
    expect(store.detail.id).toBeNull()

    const older = deferred<never>()
    vi.mocked(getMemory).mockReturnValueOnce(older.promise).mockResolvedValueOnce({ memory: memory('b') } as never)
    const a = store.openDetail('a')
    await store.openDetail('b')
    older.resolve({ memory: memory('a') } as never)
    await a
    expect(store.detail.id).toBe('b')

    const review = deferred<never>()
    vi.mocked(getMemoryProposal).mockReturnValueOnce(review.promise)
    const reviewing = store.loadReview('b', 'p')
    store.closeDetail()
    review.resolve({ proposal: proposalRecord('b', 'p') } as never)
    await reviewing
    expect(store.review.proposal).toBeNull()
  })

  it('drops a write answer after an account switch, and clears the action', async () => {
    const store = useMemoriesStore()
    const pending = deferred<never>()
    vi.mocked(createMemory).mockReturnValueOnce(pending.promise)
    const writing = store.create(terms())
    await flushPromises()
    signIn('other')
    store.reset()
    pending.resolve(created(memory('a')) as never)
    expect(await writing).toEqual({ outcome: 'discarded' })
    expect(store.actions).toEqual({})
    expect(store.notices).toEqual([])
  })
})

describe('unconfirmed writes', () => {
  it('keeps the key and captured payload across retries; only a click resends; a double click starts nothing', async () => {
    const store = useMemoriesStore()
    vi.mocked(createMemory).mockRejectedValueOnce(network())
    const input = terms({ media_ids: [P1, P2] })
    const first = store.create(input)
    expect(await store.create(terms({ title: 'second' }))).toEqual({ outcome: 'busy' })
    expect(await first).toEqual({ outcome: 'unknown' })
    expect(createMemory).toHaveBeenCalledTimes(1)
    input.title = 'changed afterwards'
    input.media_ids.push(P3)
    const [sentTerms, sentKey] = vi.mocked(createMemory).mock.calls[0]

    // A malformed success is unknown, not a success.
    vi.mocked(createMemory).mockResolvedValueOnce({ mutation: {}, memory: {} } as never)
    expect(await store.retry('new')).toEqual({ outcome: 'unknown' })
    // A later definite error cannot prove the first failed.
    vi.mocked(createMemory).mockRejectedValueOnce(new ApiError('MEMORY_PHOTO_UNAVAILABLE', 'x', 409))
    expect(await store.retry('new')).toEqual({ outcome: 'unknown' })
    expect(store.actions.new).toBeDefined()
    // Nor can being offline.
    vi.mocked(createMemory).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect(await store.retry('new')).toEqual({ outcome: 'unknown' })

    const made = memory('a', { title: '海边', photos: [photo(P1), photo(P2, 2)] })
    vi.mocked(createMemory).mockResolvedValueOnce(created(made))
    const done = await store.retry('new')
    expect(done.outcome).toBe('done')
    expect(store.actions.new).toBeUndefined()
    for (const call of vi.mocked(createMemory).mock.calls) {
      expect(call[1]).toBe(sentKey)
      expect(call[0]).toEqual(sentTerms)
    }
    expect(sentTerms).toEqual(terms({ media_ids: [P1, P2] }))
  })

  it('ends on a first definite refusal, reports it, and refreshes the copy', async () => {
    const store = useMemoriesStore()
    vi.mocked(editMemory).mockRejectedValueOnce(new ApiError('MEMORY_VERSION_CONFLICT', 'x', 409))
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: memory('a', { version: 2 }) } as never)
    await store.openDetail('a')
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: memory('a', { version: 3 }) } as never)
    const result = await store.edit('a', 1, terms({ title: 'x' }))
    expect(result).toEqual({ outcome: 'rejected', errorCode: 'MEMORY_VERSION_CONFLICT' })
    expect(store.actions).toEqual({})
    expect(store.notices[0].ok).toBe(false)
    await flushPromises()
    expect(store.detail.item?.version).toBe(3)
  })

  it('does not call an offline first send unknown', async () => {
    const store = useMemoriesStore()
    vi.mocked(createMemory).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect(await store.create(terms())).toEqual({ outcome: 'not_sent' })
    expect(store.actions.new.unresolved).toBeUndefined()
  })

  it('confirms only the response that matches the captured action', () => {
    const edit: MemoryAction = { id: 'edit:a', kind: 'edit', memoryId: 'a', expectedVersion: 1, terms: terms({ title: 'new' }), key: 'k', status: 'submitting', errorText: '' }
    const ok = { mutation: { action: 'edit', memory_id: 'a', version: 2, actor_id: 'self' }, memory: memory('a', { version: 2, title: 'new' }) }
    expect(isUsableMemoryResponse(ok, edit, 'self')).toBe(true)
    expect(isUsableMemoryResponse(ok, edit, 'other')).toBe(false)
    expect(isUsableMemoryResponse({ ...ok, mutation: { ...ok.mutation, version: 3 } }, edit, 'self')).toBe(false)
    expect(isUsableMemoryResponse({ ...ok, mutation: { ...ok.mutation, memory_id: 'b' } }, edit, 'self')).toBe(false)
    // At its own version the content must agree, including photo order.
    expect(isUsableMemoryResponse({ ...ok, memory: memory('a', { version: 2, title: 'other' }) }, edit, 'self')).toBe(false)
    // A replay carries today's newer memory, whose content may legitimately differ.
    expect(isUsableMemoryResponse({ ...ok, memory: memory('a', { version: 5, title: 'later' }) }, edit, 'self')).toBe(true)

    const decide: MemoryAction = { id: 'd', kind: 'decide', memoryId: 'a', proposalId: 'p', outcome: 'approve', key: 'k', status: 'submitting', errorText: '' }
    expect(isUsableMemoryResponse(approval(memory('a', { version: 2 }), 'p', 2), decide, 'self')).toBe(true)
    expect(isUsableMemoryResponse(approval(memory('a', { version: 2 }), 'q', 2), decide, 'self')).toBe(false)
    expect(isUsableMemoryResponse(approval(memory('a', { version: 2 }), 'p', 2, 'other'), decide, 'self')).toBe(false)
    const rejected = { mutation: { action: 'decide', memory_id: 'a', proposal_id: 'p', outcome: 'rejected', decided_by: 'self', version: null }, memory: memory('a') }
    expect(isUsableMemoryResponse(rejected, decide, 'self')).toBe(false)
    expect(isUsableMemoryResponse(rejected, { ...decide, outcome: 'reject' }, 'self')).toBe(true)
    expect(isUsableMemoryResponse(null, decide, 'self')).toBe(false)
  })

  describe('a returned memory older than the confirmed mutation', () => {
    const editOf = (version: number, title: string): MemoryMutationResponse => ({
      mutation: { action: 'edit', memory_id: 'a', version: 2, actor_id: 'self' }, memory: memory('a', { version, title }),
    })

    it('leaves an edit unconfirmed, keeps its key and payload, and an explicit retry succeeds', async () => {
      const store = useMemoriesStore()
      vi.mocked(getMemory).mockResolvedValueOnce({ memory: memory('a') } as never)
      await store.openDetail('a')
      const input = terms({ title: 'new' })
      // Even with the submitted content: version 1 cannot confirm a write of version 2.
      vi.mocked(editMemory).mockResolvedValueOnce(editOf(1, 'new'))
      expect(await store.edit('a', 1, input)).toEqual({ outcome: 'unknown' })
      expect(store.actions['edit:a']).toMatchObject({ status: 'unknown', unresolved: true, expectedVersion: 1, terms: terms({ title: 'new' }) })
      expect(store.detail.item?.version).toBe(1)
      expect(store.notices).toEqual([])
      expect(editMemory).toHaveBeenCalledTimes(1)

      input.title = 'changed afterwards'
      vi.mocked(editMemory).mockResolvedValueOnce(editOf(2, 'new'))
      expect((await store.retry('edit:a')).outcome).toBe('done')
      const calls = vi.mocked(editMemory).mock.calls
      expect(calls).toHaveLength(2)
      expect(calls[1]).toEqual(calls[0])
      expect(calls[1].slice(0, 3)).toEqual(['a', 1, terms({ title: 'new' })])
      expect(store.actions).toEqual({})
      expect(store.detail.item?.version).toBe(2)
    })

    it('leaves an approval unconfirmed, keeps its key, and an explicit retry succeeds', async () => {
      const store = useMemoriesStore()
      vi.mocked(decideMemoryProposal).mockResolvedValueOnce(approval(memory('a', { version: 1 }), 'p', 2))
      expect(await store.decide('a', 'p', 'approve')).toEqual({ outcome: 'unknown' })
      expect(store.actions['decide:a:p']).toMatchObject({ status: 'unknown', unresolved: true, outcome: 'approve' })
      expect(store.notices).toEqual([])

      vi.mocked(decideMemoryProposal).mockResolvedValueOnce(approval(memory('a', { version: 2 }), 'p', 2))
      expect((await store.retry('decide:a:p')).outcome).toBe('done')
      const calls = vi.mocked(decideMemoryProposal).mock.calls
      expect(calls).toHaveLength(2)
      expect(calls[1]).toEqual(calls[0])
      expect(store.actions).toEqual({})
    })

    it('leaves a create unconfirmed, keeps its key and payload, and an explicit retry succeeds', async () => {
      const store = useMemoriesStore()
      vi.mocked(createMemory).mockResolvedValueOnce(created(memory('a', { version: 0 })))
      expect(await store.create(terms())).toEqual({ outcome: 'unknown' })
      expect(store.actions.new).toMatchObject({ status: 'unknown', unresolved: true, terms: terms() })

      vi.mocked(createMemory).mockResolvedValueOnce(created(memory('a')))
      expect((await store.retry('new')).outcome).toBe('done')
      const calls = vi.mocked(createMemory).mock.calls
      expect(calls).toHaveLength(2)
      expect(calls[1]).toEqual(calls[0])
    })

    it('still accepts the same version with the submitted terms, refuses it with others, and accepts a newer replay', () => {
      const edit: MemoryAction = { id: 'edit:a', kind: 'edit', memoryId: 'a', expectedVersion: 1, terms: terms({ title: 'new' }), key: 'k', status: 'submitting', errorText: '' }
      expect(isUsableMemoryResponse(editOf(2, 'new'), edit, 'self')).toBe(true)
      expect(isUsableMemoryResponse(editOf(2, 'other'), edit, 'self')).toBe(false)
      expect(isUsableMemoryResponse(editOf(1, 'new'), edit, 'self')).toBe(false)
      expect(isUsableMemoryResponse(editOf(3, 'edited since'), edit, 'self')).toBe(true)

      const create: MemoryAction = { id: 'new', kind: 'create', terms: terms(), key: 'k', status: 'submitting', errorText: '' }
      expect(isUsableMemoryResponse(created(memory('a')), create, 'self')).toBe(true)
      expect(isUsableMemoryResponse(created(memory('a', { title: 'other' })), create, 'self')).toBe(false)
      expect(isUsableMemoryResponse(created(memory('a', { version: 4, title: 'edited since' })), create, 'self')).toBe(true)

      const approve: MemoryAction = { id: 'd', kind: 'decide', memoryId: 'a', proposalId: 'p', outcome: 'approve', key: 'k', status: 'submitting', errorText: '' }
      expect(isUsableMemoryResponse(approval(memory('a', { version: 2 }), 'p', 2), approve, 'self')).toBe(true)
      expect(isUsableMemoryResponse(approval(memory('a', { version: 1 }), 'p', 2), approve, 'self')).toBe(false)
      expect(isUsableMemoryResponse(approval(memory('a', { version: 5 }), 'p', 2), approve, 'self')).toBe(true)
    })

    it('leaves rejection and withdrawal as they were: no mutation version, any current version', () => {
      const decided = (outcome: 'rejected' | 'withdrawn', version: number) => ({
        mutation: { action: 'decide', memory_id: 'a', proposal_id: 'p', outcome, decided_by: 'self', base_version: 3, version: null },
        memory: memory('a', { version }),
      })
      const action: MemoryAction = { id: 'd', kind: 'decide', memoryId: 'a', proposalId: 'p', outcome: 'reject', key: 'k', status: 'submitting', errorText: '' }
      expect(isUsableMemoryResponse(decided('rejected', 3), action, 'self')).toBe(true)
      expect(isUsableMemoryResponse(decided('rejected', 1), action, 'self')).toBe(true)
      expect(isUsableMemoryResponse(decided('withdrawn', 1), { ...action, outcome: 'withdraw' }, 'self')).toBe(true)
    })
  })

  it('replays an approval once: same key, the confirmed result shown, nothing duplicated', async () => {
    const store = useMemoriesStore()
    const pendingMemory = memory('a', { pending_proposal: { id: 'p', proposer_id: 'partner', base_version: 1, created_at: 'x' } })
    vi.mocked(getMemory).mockResolvedValue({ memory: pendingMemory } as never)
    vi.mocked(getMemoryProposal).mockResolvedValue({ proposal: proposalRecord('a', 'p') } as never)
    vi.mocked(listMemories).mockResolvedValue(page([pendingMemory]))
    const wrapper = show(MemoryDetailView, { id: 'a' })
    await flushPromises()

    vi.mocked(decideMemoryProposal).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'x', 503))
    await test(wrapper, 'memory-approve').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'memory-decide-pending').text()).toContain('结果未确认')
    expect(test(wrapper, 'memory-approve').exists()).toBe(false)

    // The server replays: original facts (version 2), today's memory (already edited on to version 3).
    const latest = memory('a', { version: 3, title: '后来又改过', pending_proposal: null })
    vi.mocked(decideMemoryProposal).mockResolvedValueOnce(approval(latest, 'p', 2))
    await test(wrapper, 'memory-decide-retry').trigger('click')
    await flushPromises()
    const calls = vi.mocked(decideMemoryProposal).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1]).toEqual(calls[0])
    expect(calls[1].slice(0, 3)).toEqual(['a', 'p', 'approve'])
    expect(test(wrapper, 'memory-title').text()).toBe('后来又改过')
    expect(test(wrapper, 'memory-pending').exists()).toBe(false)
    expect(store.actions).toEqual({})
    expect(store.notices.map((n) => n.text)).toEqual(['已同意修改，回忆已更新。'])
  })

  it('can still replay an unconfirmed approval after a re-read already shows it applied', async () => {
    const store = useMemoriesStore()
    const pendingMemory = memory('a', { pending_proposal: { id: 'p', proposer_id: 'partner', base_version: 1, created_at: 'x' } })
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: pendingMemory } as never)
    vi.mocked(getMemoryProposal).mockResolvedValue({ proposal: proposalRecord('a', 'p') } as never)
    const first = show(MemoryDetailView, { id: 'a' })
    await flushPromises()
    vi.mocked(decideMemoryProposal).mockRejectedValueOnce(network())
    await test(first, 'memory-approve').trigger('click')
    await flushPromises()
    first.unmount()
    mounted.pop()
    expect(store.actions['decide:a:p'].status).toBe('unknown')

    const applied = memory('a', { version: 2, title: '山上' })
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: applied } as never)
    const back = show(MemoryDetailView, { id: 'a' })
    await flushPromises()
    expect(test(back, 'memory-title').text()).toBe('山上')
    vi.mocked(decideMemoryProposal).mockResolvedValueOnce(approval(applied, 'p', 2))
    await test(back, 'memory-decide-orphan-retry').trigger('click')
    await flushPromises()
    expect(test(back, 'memory-decide-orphan').exists()).toBe(false)
    expect(vi.mocked(decideMemoryProposal).mock.calls[1]).toEqual(vi.mocked(decideMemoryProposal).mock.calls[0])
  })
})

describe('upload queue', () => {
  function queue() {
    const store = useMemoryUploadsStore()
    return { store, status: (i = 0) => store.items[i].status }
  }

  it('goes grant, staging POST, completion, and only then is attachable', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    const posted = deferred<void>()
    vi.mocked(postToStaging).mockReturnValueOnce(posted.promise)
    const completion = deferred<MediaResponse>()
    vi.mocked(completeMediaUpload).mockReturnValueOnce(completion.promise)
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('uploading')
    expect(store.items[0].mediaId).toBe(P1)
    expect(vi.mocked(createMediaUpload).mock.calls[0].slice(0, 2)).toEqual(['image/png', 3])
    posted.resolve()
    await flushPromises()
    expect(status()).toBe('completing')
    completion.resolve(decided(P1))
    await flushPromises()
    expect(status()).toBe('ready')
    expect(completeMediaUpload).toHaveBeenCalledWith(P1)
  })

  it('refuses a wrong type or size before sending anything, with nothing to retry', async () => {
    const { store } = queue()
    store.add('s', [file('a.heic', 'image/heic'), file('b.png', 'image/png', 20_000_001), file('c.png', 'image/png', 0)])
    await flushPromises()
    expect(store.items.map((i) => i.status)).toEqual(['failed', 'failed', 'failed'])
    expect(store.items.every((i) => i.local)).toBe(true)
    expect(createMediaUpload).not.toHaveBeenCalled()
    store.retry(store.items[0].id)
    await flushPromises()
    expect(createMediaUpload).not.toHaveBeenCalled()
  })

  it('retries an unconfirmed grant with the same key and body', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockRejectedValueOnce(network()).mockResolvedValueOnce({ media: { id: 'bad' } } as never)
      .mockRejectedValueOnce(new OfflineMutationBlockedError()).mockResolvedValueOnce(grantResponse(P1, { upload: { url: 'ftp://x', fields: {}, expires_at: 'never' } }))
      .mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockResolvedValue(decided(P1))
    store.add('s', [file()])
    await flushPromises()
    for (let i = 0; i < 4; i += 1) {
      expect(status()).toBe('grant_unknown')
      store.retry(store.items[0].id)
      await flushPromises()
    }
    expect(status()).toBe('ready')
    const calls = vi.mocked(createMediaUpload).mock.calls
    expect(calls).toHaveLength(5)
    expect(new Set(calls.map((c) => c.join('|'))).size).toBe(1)
  })

  it('keeps an interrupted upload apart from an unconfirmed completion, with each one\'s own retry', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockRejectedValueOnce(new StagingUploadError())
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('interrupted')
    expect(completeMediaUpload).not.toHaveBeenCalled()

    // Interrupted: re-POST the same form. No new grant.
    vi.mocked(postToStaging).mockResolvedValueOnce()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(network())
    store.retry(store.items[0].id)
    await flushPromises()
    expect(status()).toBe('unconfirmed')
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
    expect(postToStaging).toHaveBeenCalledTimes(2)
    expect(vi.mocked(postToStaging).mock.calls[1][0]).toEqual(vi.mocked(postToStaging).mock.calls[0][0])

    // Unconfirmed: complete the same media id. No new grant and no new POST, whatever the failure.
    const failures: unknown[] = [new ApiError('MEDIA_STORE_UNAVAILABLE', 'x', 503), new ApiError('RESULT_UNKNOWN', 'x', 503), new OfflineMutationBlockedError(), { media: { media_id: P2, state: 'ready' } }, { media: { media_id: P1, state: 'granted' } }]
    for (const failure of failures) {
      if (failure instanceof Error) vi.mocked(completeMediaUpload).mockRejectedValueOnce(failure)
      else vi.mocked(completeMediaUpload).mockResolvedValueOnce(failure as never)
      store.retry(store.items[0].id)
      await flushPromises()
      expect(status()).toBe('unconfirmed')
    }
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(decided(P1))
    store.retry(store.items[0].id)
    await flushPromises()
    expect(status()).toBe('ready')
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
    expect(postToStaging).toHaveBeenCalledTimes(2)
    for (const call of vi.mocked(completeMediaUpload).mock.calls) expect(call[0]).toBe(P1)
    // Nothing retried by itself in all that: one attempt per click.
    expect(completeMediaUpload).toHaveBeenCalledTimes(failures.length + 2)
  })

  it('goes back to the POST if the store says nothing arrived', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(new ApiError('MEDIA_NOT_UPLOADED', 'x', 409))
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('interrupted')
  })

  it('asks for a new grant only on an explicit click, once a grant has really ended', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(new ApiError('MEDIA_GRANT_EXPIRED', 'x', 409))
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('expired')
    // The ordinary retry does nothing here.
    store.retry(store.items[0].id)
    await flushPromises()
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P2))
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(decided(P2))
    store.uploadAgain(store.items[0].id)
    await flushPromises()
    expect(status()).toBe('ready')
    expect(store.items[0].mediaId).toBe(P2)
    const calls = vi.mocked(createMediaUpload).mock.calls
    expect(calls[1][2]).not.toBe(calls[0][2])
  })

  it('does not POST a form already past its expiry, and does not re-grant by itself', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1, { upload: { url: SIGNED, fields: {}, expires_at: new Date(Date.now() - 1000).toISOString() } }))
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('expired')
    expect(postToStaging).not.toHaveBeenCalled()
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
  })

  it('treats rejection as terminal', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(decided(P1, 'rejected', 'not_image'))
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('rejected')
    expect(store.items[0].errorText).toContain('不是可用的图片')
    store.retry(store.items[0].id)
    await flushPromises()
    expect(completeMediaUpload).toHaveBeenCalledTimes(1)
  })

  it('takes a replayed grant\'s decided state, and an ended grant with no form as expired', async () => {
    const { store } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(decided(P1)).mockResolvedValueOnce(grantResponse(P2, { upload: null }))
    store.add('s', [file()])
    await flushPromises()
    store.add('s', [file()])
    await flushPromises()
    expect(store.items.map((i) => i.status)).toEqual(['ready', 'expired'])
    expect(postToStaging).not.toHaveBeenCalled()
  })

  it('marks uploads off when the server says so', async () => {
    const { store, status } = queue()
    vi.mocked(createMediaUpload).mockRejectedValueOnce(new ApiError('MEDIA_DISABLED', 'x', 503))
    store.add('s', [file()])
    await flushPromises()
    expect(status()).toBe('disabled')
    expect(useMemoriesStore().uploadsEnabled).toBe(false)
    expect(useMediaCapabilityStore().uploadsEnabled).toBe(false)
  })

  it('runs at most three transfers at once, and starts the next as one ends', async () => {
    const { store } = queue()
    const held = [deferred<MediaResponse>(), deferred<MediaResponse>(), deferred<MediaResponse>(), deferred<MediaResponse>()]
    for (const h of held) vi.mocked(createMediaUpload).mockReturnValueOnce(h.promise)
    store.add('s', [file(), file(), file(), file()])
    await flushPromises()
    expect(createMediaUpload).toHaveBeenCalledTimes(MAX_CONCURRENT_UPLOADS)
    expect(store.items[3].status).toBe('queued')
    held[0].reject(network())
    await flushPromises()
    expect(createMediaUpload).toHaveBeenCalledTimes(4)
  })

  it('aborts and ignores everything on an account switch', async () => {
    const { store } = queue()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    let signal: AbortSignal | undefined
    const posted = deferred<void>()
    vi.mocked(postToStaging).mockImplementationOnce((_form, _file, s) => { signal = s; return posted.promise })
    store.add('s', [file()])
    await flushPromises()
    expect(signal?.aborted).toBe(false)
    signIn('other')
    store.reset()
    expect(signal?.aborted).toBe(true)
    posted.resolve()
    await flushPromises()
    expect(store.items).toEqual([])
    expect(completeMediaUpload).not.toHaveBeenCalled()
  })

  it('ignores a late grant and completion after the same user changes account mid-flight', async () => {
    const { store } = queue()
    const granting = deferred<MediaResponse>()
    vi.mocked(createMediaUpload).mockReturnValueOnce(granting.promise)
    store.add('s', [file()])
    await flushPromises()
    signIn('other')
    granting.resolve(grantResponse(P1))
    await flushPromises()
    expect(postToStaging).not.toHaveBeenCalled()
  })

  it('forgets a form\'s uploads on leaving its route, and a late completion changes nothing', async () => {
    const { store } = queue()
    vi.mocked(createMediaUpload).mockResolvedValue(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    const completion = deferred<MediaResponse>()
    vi.mocked(completeMediaUpload).mockReturnValueOnce(completion.promise)
    store.add('left', [file()])
    await flushPromises()
    store.clearScope('left')
    expect(store.items).toEqual([])
    completion.resolve(decided(P1))
    await flushPromises()
    expect(store.items).toEqual([])
  })
})

describe('editor', () => {
  async function editor(initial: MemoryTerms | null = null, uploadsEnabled: boolean | null = true) {
    useMediaCapabilityStore().$patch({ uploadsEnabled, status: uploadsEnabled === null ? 'error' : 'ready' })
    return show(MemoryEditor, { name: 'f', scope: 's', submitLabel: '保存', initial })
  }

  it('allows attachment only after READY, and submits the order shown', async () => {
    const wrapper = await editor(terms({ media_ids: [P1] }))
    await test(wrapper, 'f-title').setValue('  新标题 ')
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P2))
    const posted = deferred<void>()
    vi.mocked(postToStaging).mockReturnValueOnce(posted.promise)
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(decided(P2))
    await pickFiles(wrapper, 'f', [file()])
    expect(test(wrapper, 'f-upload-status').attributes('data-status')).toBe('uploading')

    await test(wrapper, 'f-form').trigger('submit')
    expect(wrapper.emitted('submit')).toBeUndefined()
    expect(test(wrapper, 'f-error').text()).toContain('没有确认可用')

    posted.resolve()
    await flushPromises()
    expect(test(wrapper, 'f-upload-status').exists()).toBe(false)
    await wrapper.findAll('[data-test="f-photo-up"]')[1].trigger('click')
    await test(wrapper, 'f-form').trigger('submit')
    expect(wrapper.emitted('submit')?.[0]).toEqual([terms({ title: '新标题', media_ids: [P2, P1] })])
  })

  it('shows each failure\'s own retry, which sends nothing new until clicked', async () => {
    const wrapper = await editor()
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValueOnce()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(network()).mockResolvedValueOnce(decided(P1))
    await pickFiles(wrapper, 'f', [file()])
    expect(test(wrapper, 'f-upload-status').attributes('data-status')).toBe('unconfirmed')
    expect(test(wrapper, 'f-upload-retry').text()).toBe('重试确认')
    expect(completeMediaUpload).toHaveBeenCalledTimes(1)
    await test(wrapper, 'f-upload-retry').trigger('click')
    await flushPromises()
    expect(completeMediaUpload).toHaveBeenCalledTimes(2)
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
    expect(wrapper.findAll('[data-test="memory-photo-image"]')).toHaveLength(1)
  })

  it('removes a failed upload so the rest can be submitted', async () => {
    const wrapper = await editor(terms())
    vi.mocked(createMediaUpload).mockRejectedValueOnce(network())
    await pickFiles(wrapper, 'f', [file()])
    await test(wrapper, 'f-form').trigger('submit')
    expect(wrapper.emitted('submit')).toBeUndefined()
    await wrapper.findAll('[data-test="f-photo-remove"]')[1].trigger('click')
    expect(useMemoryUploadsStore().items).toEqual([])
    await test(wrapper, 'f-form').trigger('submit')
    expect(wrapper.emitted('submit')).toHaveLength(1)
  })

  it('does not take more than nine photos', async () => {
    const wrapper = await editor(terms({ media_ids: Array.from({ length: 8 }, (_, i) => `id-${i}`) }))
    vi.mocked(createMediaUpload).mockReturnValue(deferred<MediaResponse>().promise)
    await pickFiles(wrapper, 'f', [file(), file(), file()])
    expect(useMemoryUploadsStore().items).toHaveLength(1)
    expect(wrapper.findAll('[data-test="f-photo"]')).toHaveLength(9)
    expect(wrapper.text()).toContain('最多 9 张')
  })

  it('with uploads off, explains it and still allows reordering existing photos', async () => {
    const wrapper = await editor(terms({ media_ids: [P1, P2] }), false)
    expect(test(wrapper, 'f-files').exists()).toBe(false)
    expect(test(wrapper, 'f-uploads-off').exists()).toBe(true)
    await wrapper.findAll('[data-test="f-photo-down"]')[0].trigger('click')
    await test(wrapper, 'f-form').trigger('submit')
    expect(wrapper.emitted('submit')?.[0]).toEqual([terms({ media_ids: [P2, P1] })])
  })

  it('with uploads unknown, offers a re-check instead of the picker', async () => {
    const wrapper = await editor(terms(), null)
    expect(test(wrapper, 'f-files').exists()).toBe(false)
    vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
    await test(wrapper, 'f-uploads-recheck').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'f-files').exists()).toBe(true)
  })

  it('forgets its uploads when unmounted', async () => {
    const wrapper = await editor()
    vi.mocked(createMediaUpload).mockReturnValue(deferred<MediaResponse>().promise)
    await pickFiles(wrapper, 'f', [file()])
    expect(useMemoryUploadsStore().items).toHaveLength(1)
    wrapper.unmount()
    mounted.pop()
    expect(useMemoryUploadsStore().items).toEqual([])
  })
})

describe('new memory page', () => {
  it('with uploads off it explains, offers no file picker but keeps the form for previously uploaded photos, and reads only the capability', async () => {
    vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: false })
    const wrapper = show(MemoryNewView)
    await flushPromises()
    expect(getMediaCapabilities).toHaveBeenCalledTimes(1)
    expect(listMemories).not.toHaveBeenCalled()
    expect(test(wrapper, 'memory-new-off').exists()).toBe(true)
    expect(test(wrapper, 'memory-create-form').exists()).toBe(true)
    expect(test(wrapper, 'memory-create-files').exists()).toBe(false)
    expect(test(wrapper, 'memory-create-unattached-toggle').exists()).toBe(true)
  })

  it('offers a re-check when the capability cannot be read', async () => {
    vi.mocked(getMediaCapabilities).mockRejectedValueOnce(network()).mockResolvedValueOnce({ uploads_enabled: true })
    const wrapper = show(MemoryNewView)
    await flushPromises()
    expect(test(wrapper, 'memory-new-unknown').exists()).toBe(true)
    expect(test(wrapper, 'memory-create-files').exists()).toBe(false)
    await test(wrapper, 'memory-new-recheck').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'memory-new-unknown').exists()).toBe(false)
    expect(test(wrapper, 'memory-create-files').exists()).toBe(true)
  })

  it('keeps an unconfirmed create locked to what it captured, across leaving and returning, and opens the result', async () => {
    const first = show(MemoryNewView)
    await flushPromises()
    await test(first, 'memory-create-date').setValue('2026-03-01')
    await test(first, 'memory-create-title').setValue('海边')
    vi.mocked(createMediaUpload).mockResolvedValueOnce(decided(P1))
    await pickFiles(first, 'memory-create', [file()])
    vi.mocked(createMemory).mockRejectedValueOnce(network())
    await test(first, 'memory-create-form').trigger('submit')
    await flushPromises()
    expect(test(first, 'memory-create-pending').text()).toContain('结果未确认')
    first.unmount()
    mounted.pop()

    const back = show(MemoryNewView)
    await flushPromises()
    expect((test(back, 'memory-create-title').element as HTMLInputElement).value).toBe('海边')
    expect((test(back, 'memory-create-title').element as HTMLInputElement).disabled).toBe(true)
    expect(back.findAll('[data-test="memory-create-photo"]')).toHaveLength(1)
    vi.mocked(createMemory).mockResolvedValueOnce(created(memory('new-id')))
    await test(back, 'memory-create-retry').trigger('click')
    await flushPromises()
    const calls = vi.mocked(createMemory).mock.calls
    expect(calls[1]).toEqual(calls[0])
    expect(routerPush).toHaveBeenCalledWith({ name: 'memory-detail', params: { id: 'new-id' } })
  })
})

describe('D20 opt-in partner notice on a new memory', () => {
  const delivered = { requested: true, created: true, reason: 'delivered_to_inbox' as const, notification_id: '99999999-9999-4999-8999-999999999999' }
  function withFacts(m: Memory, notification: unknown): MemoryMutationResponse {
    return { ...created(m), notification } as MemoryMutationResponse
  }
  async function fill(wrapper: VueWrapper): Promise<void> {
    await test(wrapper, 'memory-create-date').setValue('2026-03-01')
    await test(wrapper, 'memory-create-title').setValue('海边')
    vi.mocked(createMediaUpload).mockResolvedValueOnce(decided(P1))
    await pickFiles(wrapper, 'memory-create', [file()])
  }
  const box = (wrapper: VueWrapper) => test(wrapper, 'memory-create-notify-input').element as HTMLInputElement

  it('is off on a fresh form and sends an explicit false unless chosen', async () => {
    const wrapper = show(MemoryNewView)
    await flushPromises()
    expect(box(wrapper).checked).toBe(false)
    expect(box(wrapper).disabled).toBe(false)
    await fill(wrapper)
    vi.mocked(createMemory).mockResolvedValueOnce(created(memory('new-id')))
    await test(wrapper, 'memory-create-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(createMemory).mock.calls[0][2]).toBe(false)
    expect(routerPush).toHaveBeenCalledWith({ name: 'memory-detail', params: { id: 'new-id' } })
  })

  it('captures true with the key, keeps it locked while unconfirmed, retries it unchanged, and a new form starts off again', async () => {
    const first = show(MemoryNewView)
    await flushPromises()
    await fill(first)
    await test(first, 'memory-create-notify-input').setValue(true)
    // Facts that disagree with the captured choice are not a confirmation.
    vi.mocked(createMemory).mockResolvedValueOnce(withFacts(memory('new-id'), { requested: false, created: false, reason: 'not_requested', notification_id: null }))
    await test(first, 'memory-create-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(createMemory).mock.calls[0][2]).toBe(true)
    expect(test(first, 'memory-create-pending').text()).toContain('结果未确认')
    expect(box(first).checked).toBe(true)
    expect(box(first).disabled).toBe(true)
    expect(routerPush).not.toHaveBeenCalled()
    first.unmount()
    mounted.pop()

    // Leaving and returning shows the captured choice, still locked; the retry resends the same terms, key and choice.
    const back = show(MemoryNewView)
    await flushPromises()
    expect(box(back).checked).toBe(true)
    expect(box(back).disabled).toBe(true)
    vi.mocked(createMemory).mockResolvedValueOnce(withFacts(memory('new-id'), delivered))
    await test(back, 'memory-create-retry').trigger('click')
    await flushPromises()
    const calls = vi.mocked(createMemory).mock.calls
    expect(calls[1]).toEqual(calls[0])
    expect(routerPush).toHaveBeenCalledWith({ name: 'memory-detail', params: { id: 'new-id' } })
    back.unmount()
    mounted.pop()

    // Nothing is remembered: the next form is off.
    const fresh = show(MemoryNewView)
    await flushPromises()
    expect(box(fresh).checked).toBe(false)
    expect(box(fresh).disabled).toBe(false)
  })

  it('with no partner the toggle is unavailable, the create sends false, and no_partner facts confirm it', async () => {
    useSessionStore().applyBootstrap({
      authenticated: true, csrf_token: 'csrf-self',
      user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-29T00:00:00Z' },
      space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 1, created_at: '2026-10-29T00:00:00Z', members: [] },
    })
    const wrapper = show(MemoryNewView)
    await flushPromises()
    expect(box(wrapper).disabled).toBe(true)
    expect(test(wrapper, 'memory-create-notify-unavailable').exists()).toBe(true)
    await fill(wrapper)
    vi.mocked(createMemory).mockResolvedValueOnce(created(memory('solo-id')))
    await test(wrapper, 'memory-create-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(createMemory).mock.calls[0][2]).toBe(false)
    expect(routerPush).toHaveBeenCalledWith({ name: 'memory-detail', params: { id: 'solo-id' } })
  })

  it('the store accepts no_partner only for a requested create and refuses facts on a create that captured nothing but asked', () => {
    const action: MemoryAction = { id: 'new', kind: 'create', terms: terms(), key: 'k', status: 'submitting', errorText: '', notify: true }
    const noPartner = { requested: true, created: false, reason: 'no_partner', notification_id: null }
    expect(isUsableMemoryResponse(withFacts(memory('a'), noPartner), action, 'self')).toBe(true)
    expect(isUsableMemoryResponse(withFacts(memory('a'), delivered), action, 'self')).toBe(true)
    expect(isUsableMemoryResponse(withFacts(memory('a'), null), action, 'self')).toBe(false)
    expect(isUsableMemoryResponse(withFacts(memory('a'), undefined), action, 'self')).toBe(false)
    const quiet: MemoryAction = { ...action, notify: false }
    expect(isUsableMemoryResponse(withFacts(memory('a'), noPartner), quiet, 'self')).toBe(false)
    expect(isUsableMemoryResponse(created(memory('a')), quiet, 'self')).toBe(true)
    // Plan conversion and older store callers capture nothing: facts may be absent, or present and not requested.
    const none: MemoryAction = { ...action, notify: undefined }
    expect(isUsableMemoryResponse(withFacts(memory('a'), undefined), none, 'self')).toBe(true)
    expect(isUsableMemoryResponse(withFacts(memory('a'), delivered), none, 'self')).toBe(false)
  })

  it('an identity change resets the choice', async () => {
    const wrapper = show(MemoryNewView)
    await flushPromises()
    await test(wrapper, 'memory-create-notify-input').setValue(true)
    expect(box(wrapper).checked).toBe(true)
    signIn('other')
    await flushPromises()
    expect(box(wrapper).checked).toBe(false)
  })
})

describe('detail: permissions and review', () => {
  const pendingBy = (proposer: string) => ({ id: 'p', proposer_id: proposer, base_version: 1, created_at: 'x' })

  async function detailOf(item: Memory) {
    vi.mocked(getMemory).mockResolvedValue({ memory: item } as never)
    vi.mocked(getMemoryProposal).mockResolvedValue({ proposal: proposalRecord(item.id, 'p', { proposer_id: item.pending_proposal?.proposer_id ?? 'partner' }) } as never)
    const wrapper = show(MemoryDetailView, { id: item.id })
    await flushPromises()
    return wrapper
  }

  it('the creator edits directly against the captured version', async () => {
    const wrapper = await detailOf(memory('a', { version: 4 }))
    expect(test(wrapper, 'memory-start-write').text()).toBe('修改')
    await test(wrapper, 'memory-start-write').trigger('click')
    await test(wrapper, 'memory-write-title').setValue('新的')
    vi.mocked(editMemory).mockResolvedValueOnce({ mutation: { action: 'edit', memory_id: 'a', version: 5, actor_id: 'self' }, memory: memory('a', { version: 5, title: '新的' }) })
    await test(wrapper, 'memory-write-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editMemory).mock.calls[0].slice(0, 3)).toEqual(['a', 4, terms({ title: '新的' })])
    expect(proposeMemoryEdit).not.toHaveBeenCalled()
    expect(test(wrapper, 'memory-title').text()).toBe('新的')
    expect(test(wrapper, 'memory-write-form').exists()).toBe(false)
  })

  it('the partner can only request, and the memory is unchanged until approved', async () => {
    const wrapper = await detailOf(memory('a', { creator_id: 'partner', version: 2 }))
    expect(test(wrapper, 'memory-start-write').text()).toBe('申请修改')
    await test(wrapper, 'memory-start-write').trigger('click')
    await test(wrapper, 'memory-write-title').setValue('建议')
    const after = memory('a', { creator_id: 'partner', version: 2, pending_proposal: pendingBy('self') })
    vi.mocked(proposeMemoryEdit).mockResolvedValueOnce({ mutation: { action: 'propose', memory_id: 'a', proposal_id: 'p', proposer_id: 'self', base_version: 2 }, memory: after })
    await test(wrapper, 'memory-write-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(proposeMemoryEdit).mock.calls[0].slice(0, 2)).toEqual(['a', 2])
    expect(editMemory).not.toHaveBeenCalled()
    expect(test(wrapper, 'memory-title').text()).toBe('海边')
    expect(test(wrapper, 'memory-withdraw').exists()).toBe(true)
    expect(test(wrapper, 'memory-approve').exists()).toBe(false)
    expect(test(wrapper, 'memory-start-write').exists()).toBe(false)
  })

  it('the creator sees current and proposed side by side, marked where they differ, and cannot edit meanwhile', async () => {
    const wrapper = await detailOf(memory('a', { pending_proposal: pendingBy('partner') }))
    expect(getMemoryProposal).toHaveBeenCalledWith('a', 'p')
    const original = test(wrapper, 'memory-review-original')
    const proposed = test(wrapper, 'memory-review-proposed')
    expect(original.text()).toContain('海边')
    expect(original.text()).toContain('2026年3月1日')
    expect(proposed.text()).toContain('山上')
    expect(proposed.text()).toContain('2026年3月2日')
    expect(original.findAll('img').map((i) => i.attributes('src'))).toEqual([`/api/v1/media/${P1}/content?variant=thumb`])
    expect(proposed.findAll('img').map((i) => i.attributes('src'))).toEqual([`/api/v1/media/${P2}/content?variant=thumb`, `/api/v1/media/${P1}/content?variant=thumb`])
    expect(test(wrapper, 'memory-diff-title').exists()).toBe(true)
    expect(test(wrapper, 'memory-diff-date').exists()).toBe(true)
    expect(test(wrapper, 'memory-diff-photos').exists()).toBe(true)
    expect(test(wrapper, 'memory-diff-description').exists()).toBe(false)
    expect(test(wrapper, 'memory-approve').exists()).toBe(true)
    expect(test(wrapper, 'memory-reject').exists()).toBe(true)
    expect(test(wrapper, 'memory-withdraw').exists()).toBe(false)
    expect(test(wrapper, 'memory-start-write').exists()).toBe(false)
  })

  it('a malformed or failed review cannot be approved blind, but can be retried', async () => {
    vi.mocked(getMemory).mockResolvedValue({ memory: memory('a', { pending_proposal: pendingBy('partner') }) } as never)
    vi.mocked(getMemoryProposal).mockResolvedValueOnce({ proposal: { id: 'p' } } as never)
    const wrapper = show(MemoryDetailView, { id: 'a' })
    await flushPromises()
    expect(test(wrapper, 'memory-review-error').exists()).toBe(true)
    expect(test(wrapper, 'memory-approve').attributes('disabled')).toBeDefined()
    vi.mocked(getMemoryProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'p') } as never)
    await test(wrapper, 'memory-review-retry').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'memory-approve').attributes('disabled')).toBeUndefined()
  })

  it('the proposer withdraws; a reject and withdraw return to the unchanged memory', async () => {
    const wrapper = await detailOf(memory('a', { creator_id: 'partner', pending_proposal: pendingBy('self') }))
    vi.mocked(decideMemoryProposal).mockResolvedValueOnce({
      mutation: { action: 'decide', memory_id: 'a', proposal_id: 'p', outcome: 'withdrawn', decided_by: 'self', version: null },
      memory: memory('a', { creator_id: 'partner' }),
    })
    await test(wrapper, 'memory-withdraw').trigger('click')
    await flushPromises()
    expect(vi.mocked(decideMemoryProposal).mock.calls[0].slice(0, 3)).toEqual(['a', 'p', 'withdraw'])
    expect(test(wrapper, 'memory-pending').exists()).toBe(false)
    expect(test(wrapper, 'memory-start-write').text()).toBe('申请修改')
    expect(useMemoriesStore().review.proposal).toBeNull()
  })

  it('a version conflict keeps what was typed, and continuing is explicit', async () => {
    const wrapper = await detailOf(memory('a', { version: 1 }))
    await test(wrapper, 'memory-start-write').trigger('click')
    await test(wrapper, 'memory-write-title').setValue('我的内容')
    vi.mocked(editMemory).mockRejectedValueOnce(new ApiError('MEMORY_VERSION_CONFLICT', 'x', 409))
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: memory('a', { version: 2 }) } as never)
    await test(wrapper, 'memory-write-form').trigger('submit')
    await flushPromises()
    expect(test(wrapper, 'memory-conflict').exists()).toBe(true)
    expect((test(wrapper, 'memory-write-title').element as HTMLInputElement).value).toBe('我的内容')
    expect((test(wrapper, 'memory-write-title').element as HTMLInputElement).disabled).toBe(true)
    await test(wrapper, 'memory-use-latest').trigger('click')
    vi.mocked(editMemory).mockResolvedValueOnce({ mutation: { action: 'edit', memory_id: 'a', version: 3, actor_id: 'self' }, memory: memory('a', { version: 3, title: '我的内容' }) })
    await test(wrapper, 'memory-write-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editMemory).mock.calls[1][1]).toBe(2)
  })

  it('an unavailable photo refuses the write but keeps the form', async () => {
    const wrapper = await detailOf(memory('a'))
    await test(wrapper, 'memory-start-write').trigger('click')
    await test(wrapper, 'memory-write-title').setValue('x')
    vi.mocked(editMemory).mockRejectedValueOnce(new ApiError('MEMORY_PHOTO_UNAVAILABLE', 'x', 409))
    await test(wrapper, 'memory-write-form').trigger('submit')
    await flushPromises()
    expect((test(wrapper, 'memory-write-title').element as HTMLInputElement).value).toBe('x')
    expect(test(wrapper, 'memory-notices').text()).toContain('有照片已不可用')
  })

  it('a missing memory or malformed detail shows a retry; a missing photo does not break the page', async () => {
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: { id: 'a' } } as never)
    const wrapper = show(MemoryDetailView, { id: 'a' })
    await flushPromises()
    expect(test(wrapper, 'memory-detail-error').exists()).toBe(true)
    vi.mocked(getMemory).mockResolvedValueOnce({ memory: memory('a') } as never)
    await wrapper.find('[data-test="memory-detail"] button').trigger('click')
    await flushPromises()
    await test(wrapper, 'memory-photo-image').trigger('error')
    expect(test(wrapper, 'memory-photo-missing').exists()).toBe(true)
    expect(test(wrapper, 'memory-title').text()).toBe('海边')
  })

  it('switching account or memory clears the form', async () => {
    const wrapper = await detailOf(memory('a'))
    await test(wrapper, 'memory-start-write').trigger('click')
    await test(wrapper, 'memory-write-title').setValue('draft')
    vi.mocked(getMemory).mockResolvedValue({ memory: memory('b') } as never)
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    expect(test(wrapper, 'memory-write-form').exists()).toBe(false)
    expect(test(wrapper, 'memory-start-write').exists()).toBe(true)
  })

  it('leaving drops the page state', async () => {
    const wrapper = await detailOf(memory('a'))
    wrapper.unmount()
    mounted.pop()
    expect(useMemoriesStore().detail.id).toBeNull()
  })
})

describe('session', () => {
  it('clears memories and uploads on an identity reset', async () => {
    const memories = useMemoriesStore()
    const uploads = useMemoryUploadsStore()
    vi.mocked(listMemories).mockResolvedValue(page([memory('a')]))
    await memories.load()
    vi.mocked(createMemory).mockRejectedValueOnce(network())
    await memories.create(terms())
    vi.mocked(createMediaUpload).mockReturnValue(deferred<MediaResponse>().promise)
    uploads.add('s', [file()])
    await flushPromises()
    expect(memories.actions.new).toBeDefined()
    expect(uploads.items).toHaveLength(1)
    // The real path: a bootstrap naming another user.
    signIn('other')
    expect(memories.items).toEqual([])
    expect(memories.actions).toEqual({})
    expect(memories.uploadsEnabled).toBeNull()
    expect(uploads.items).toEqual([])
  })
})

describe('what stays private', () => {
  it('never caches a photo route or a signed URL in the service worker, whichever way it is requested', () => {
    const route = mediaContentUrl(P1, 'view')
    expect(isSensitivePath(route)).toBe(true)
    for (const destination of ['image', 'document', '']) {
      expect(isCacheableRequest(route, 'GET', { sameOrigin: true, destination })).toBe(false)
    }
    // After the redirect, and the grant's POST: other origins are never cached, even with an image's extension.
    expect(isCacheableRequest('https://bucket.example.test/img.png?sig=1', 'GET', { sameOrigin: false, destination: 'image' })).toBe(false)
    expect(isCacheableRequest(SIGNED, 'POST', { sameOrigin: false })).toBe(false)
    expect(isCacheableRequest('/api/v1/media/uploads', 'POST', { sameOrigin: true })).toBe(false)
    expect(isCacheableRequest('/api/v1/memories', 'GET', { sameOrigin: true })).toBe(false)
  })

  it('the service worker registers the sensitive route first and uses the shared policy', () => {
    const source = readFileSync(resolve(__dirname, '../../src/pwa/sw.ts'), 'utf8')
    const sensitive = source.indexOf('isSensitivePath(url.pathname), networkOnly')
    const shell = source.indexOf('isCacheableRequest(url.toString()')
    expect(sensitive).toBeGreaterThan(0)
    expect(shell).toBeGreaterThan(sensitive)
    expect(source).toContain('sameOrigin: url.origin === self.location.origin')
  })

  it('leaves no grant, signature, file name or media in storage, logs or reactive state through a full flow, including failures', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem')
    const logs = (['log', 'info', 'warn', 'error', 'debug'] as const).map((level) => vi.spyOn(console, level).mockImplementation(() => {}))
    const memories = useMemoriesStore()
    const uploads = useMemoryUploadsStore()

    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockRejectedValueOnce(new StagingUploadError()).mockResolvedValueOnce()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(network())
    uploads.add('s', [file('secret-holiday-name.png')])
    await flushPromises()
    expect(JSON.stringify(uploads.items)).not.toContain(SIGNATURE)
    expect(JSON.stringify(uploads.items)).not.toContain(SIGNED)
    expect(JSON.stringify(uploads.items)).not.toContain('secret-holiday-name')
    uploads.retry(uploads.items[0].id)
    await flushPromises()
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(decided(P1))
    uploads.retry(uploads.items[0].id)
    await flushPromises()
    vi.mocked(createMemory).mockResolvedValueOnce(created(memory('a')))
    await memories.create(terms())

    expect(setItem).not.toHaveBeenCalled()
    expect(localStorage.length).toBe(0)
    expect(sessionStorage.length).toBe(0)
    const logged = JSON.stringify(logs.flatMap((spy) => spy.mock.calls))
    expect(logged).not.toContain(SIGNATURE)
    expect(logged).not.toContain(SIGNED)
    const state = JSON.stringify([uploads.$state, memories.$state])
    expect(state).not.toContain(SIGNATURE)
    expect(state).not.toContain('secret-holiday-name')
  })

  it('no memory or upload source touches storage, caches or the console', () => {
    for (const path of ['stores/memories.ts', 'stores/memory-uploads.ts', 'api/media.ts', 'api/memories.ts', 'api/memory-rules.ts',
      'components/MemoryEditor.vue', 'components/MemoryPhotoView.vue', 'views/MemoriesView.vue', 'views/MemoryNewView.vue', 'views/MemoryDetailView.vue']) {
      const source = readFileSync(resolve(__dirname, '../../src', path), 'utf8')
      expect(source, path).not.toMatch(/localStorage|sessionStorage|indexedDB|caches\.|console\./)
    }
  })
})
