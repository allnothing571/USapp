/**
 * Upload capability (contract 0.40.0, web): the identity-scoped store and its ordering (its own reads, list pages, MEDIA_DISABLED);
 * direct navigation to memory and album pages without an unrelated list read; unknown and failed states with an explicit retry; and
 * that READY selections, previously uploaded photos and uncertain uploads survive a disabled capability. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/media-capability', async (original) => ({
  ...(await original<typeof import('@/api/media-capability')>()),
  getMediaCapabilities: vi.fn(),
}))
vi.mock('@/api/memories', () => ({
  MEMORY_PAGE_SIZE: 20,
  createMemory: vi.fn(), listMemories: vi.fn(), getMemory: vi.fn(), editMemory: vi.fn(), proposeMemoryEdit: vi.fn(),
  getMemoryProposal: vi.fn(), decideMemoryProposal: vi.fn(), listMemoryProposals: vi.fn(),
}))
vi.mock('@/api/albums', () => ({
  ALBUM_PAGE_SIZE: 20, ALBUM_PHOTO_PAGE_SIZE: 30,
  createAlbum: vi.fn(), listAlbums: vi.fn(), getAlbum: vi.fn(), listAlbumPhotos: vi.fn(), addAlbumPhotos: vi.fn(), removeAlbumPhoto: vi.fn(),
  setAlbumCover: vi.fn(), renameAlbum: vi.fn(), proposeAlbumRename: vi.fn(), getAlbumProposal: vi.fn(), decideAlbumProposal: vi.fn(),
  listAlbumProposals: vi.fn(),
}))
vi.mock('@/api/unattached', async (original) => ({
  ...(await original<typeof import('@/api/unattached')>()),
  listUnattachedPhotos: vi.fn(),
}))
vi.mock('@/api/media', async (original) => ({
  ...(await original<typeof import('@/api/media')>()),
  createMediaUpload: vi.fn(), completeMediaUpload: vi.fn(), postToStaging: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: vi.fn() }),
}))

import { getAlbum, listAlbumPhotos, listAlbumProposals, listAlbums } from '@/api/albums'
import { ApiError, setCsrfToken } from '@/api/client'
import { completeMediaUpload, createMediaUpload, postToStaging } from '@/api/media'
import { getMediaCapabilities, isMediaCapabilities, type MediaCapabilities } from '@/api/media-capability'
import { getMemory, listMemories, listMemoryProposals } from '@/api/memories'
import { listUnattachedPhotos } from '@/api/unattached'
import type { Album, MediaResponse, Memory } from '@/api/types'
import AlbumPhotoAdder from '@/components/AlbumPhotoAdder.vue'
import MemoryEditor from '@/components/MemoryEditor.vue'
import { useAlbumsStore } from '@/stores/albums'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useMemoriesStore } from '@/stores/memories'
import { useMemoryUploadsStore } from '@/stores/memory-uploads'
import { useSessionStore } from '@/stores/session'
import AlbumDetailView from '@/views/AlbumDetailView.vue'
import MemoriesView from '@/views/MemoriesView.vue'
import MemoryDetailView from '@/views/MemoryDetailView.vue'
import MemoryNewView from '@/views/MemoryNewView.vue'

const NOW = '2026-11-09T01:00:00.000000Z'
const P1 = '11111111-1111-4111-8111-111111111111'
const P2 = '22222222-2222-4222-8222-222222222222'

function signIn(user: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${user}`,
    user: { id: user, email: `${user}@example.com`, display_name: user, created_at: NOW },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: NOW, members: [] },
  })
  setCsrfToken(`csrf-${user}`)
}
function deferred<T>() {
  let resolveIt!: (value: T) => void
  let rejectIt!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolveIt = yes; rejectIt = no })
  return { promise, resolve: resolveIt, reject: rejectIt }
}
function memory(id: string): Memory {
  return { id, creator_id: 'self', version: 1, date: '2026-03-01', title: '海边', description: '', created_at: NOW, updated_at: NOW,
    photos: [{ position: 1, media_id: P1, uploader_id: 'self', width: 4, height: 3, deleted: false }], pending_proposal: null }
}
function album(): Album {
  return { id: 'album-1', creator_id: 'self', name: '旅行', version: 1, pending_proposal: null, created_at: NOW, photo_count: 0, cover: null }
}
function grantResponse(mediaId: string): MediaResponse {
  return {
    media: { media_id: mediaId, state: 'granted', uploader_id: 'self', content_type: 'image/png', size_bytes: 3, created_at: NOW, reason: null, width: null, height: null },
    upload: { url: 'https://bucket.example.test/staging/x', fields: { key: 'k' }, expires_at: new Date(Date.now() + 600_000).toISOString() },
  }
}

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

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
  vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
  vi.mocked(listAlbumPhotos).mockResolvedValue({ photos: [], next_cursor: null } as never)
  vi.mocked(listAlbumProposals).mockResolvedValue({ proposals: [], next_cursor: null, current_version: 1 } as never)
  vi.mocked(listMemoryProposals).mockResolvedValue({ proposals: [], next_cursor: null, current_version: 1 } as never)
  vi.mocked(listUnattachedPhotos).mockResolvedValue({ photos: [{ media_id: P2, uploaded_at: NOW, width: 4, height: 3 }], next_cursor: null })
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('store', () => {
  it('reads true or false, treats a malformed or failed read as unknown, and retries explicitly', async () => {
    const store = useMediaCapabilityStore()
    expect(isMediaCapabilities({ uploads_enabled: false })).toBe(true)
    expect(isMediaCapabilities({ uploads_enabled: 'yes' })).toBe(false)
    await store.load()
    expect(store.uploadsEnabled).toBe(true)
    vi.mocked(getMediaCapabilities).mockResolvedValueOnce({ uploads_enabled: false })
    await store.load()
    expect(store.uploadsEnabled).toBe(false)
    vi.mocked(getMediaCapabilities).mockResolvedValueOnce({} as never)
    await store.load()
    expect(store.uploadsEnabled).toBeNull()
    expect(store.status).toBe('error')
    vi.mocked(getMediaCapabilities).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    await store.load()
    expect(store.status).toBe('error')
    expect(store.errorText).not.toBe('')
    await store.load()
    expect(store.uploadsEnabled).toBe(true)
    // ensure() reads only once nothing is known.
    vi.clearAllMocks()
    store.ensure()
    expect(getMediaCapabilities).not.toHaveBeenCalled()
  })

  it('MEDIA_DISABLED outranks an enabled answer still in flight, and a list page requested before it', async () => {
    const store = useMediaCapabilityStore()
    const slow = deferred<MediaCapabilities>()
    vi.mocked(getMediaCapabilities).mockReturnValueOnce(slow.promise)
    const reading = store.load()
    const listToken = store.token()
    store.markDisabled()
    slow.resolve({ uploads_enabled: true })
    await reading
    expect(store.uploadsEnabled).toBe(false)
    store.observe(true, listToken)
    expect(store.uploadsEnabled).toBe(false)
    // Only a read started after it can turn uploads back on.
    await store.load()
    expect(store.uploadsEnabled).toBe(true)
  })

  it('takes a list page\'s value only when nothing newer is known, and a real upload\'s MEDIA_DISABLED reaches it', async () => {
    const store = useMediaCapabilityStore()
    store.observe(false, store.token())
    expect(store.uploadsEnabled).toBe(false)
    vi.mocked(listMemories).mockResolvedValueOnce({ memories: [], next_cursor: null, uploads_enabled: true } as never)
    await useMemoriesStore().load()
    expect(store.uploadsEnabled).toBe(true)
    vi.mocked(createMediaUpload).mockRejectedValueOnce(new ApiError('MEDIA_DISABLED', 'x', 503))
    useMemoryUploadsStore().add('s', [new File(['xxx'], 'a.png', { type: 'image/png' })])
    await flushPromises()
    expect(store.uploadsEnabled).toBe(false)
    expect(useMemoriesStore().uploadsEnabled).toBe(false)
    expect(useAlbumsStore().uploadsEnabled).toBe(false)
  })

  it('drops a late answer after an account switch, and the new account reads its own', async () => {
    const store = useMediaCapabilityStore()
    const slow = deferred<MediaCapabilities>()
    vi.mocked(getMediaCapabilities).mockReturnValueOnce(slow.promise)
    const reading = store.load()
    signIn('other')
    slow.resolve({ uploads_enabled: true })
    await reading
    expect(store.uploadsEnabled).toBeNull()
    expect(store.status).toBe('idle')
    vi.mocked(getMediaCapabilities).mockResolvedValueOnce({ uploads_enabled: false })
    await store.load()
    expect(store.uploadsEnabled).toBe(false)
  })
})

describe('direct navigation', () => {
  it('a new memory page, a memory edit and an album page read the capability, never an unrelated list', async () => {
    show(MemoryNewView)
    await flushPromises()
    expect(getMediaCapabilities).toHaveBeenCalledTimes(1)

    setActivePinia(createPinia())
    signIn('self')
    vi.mocked(getMemory).mockResolvedValue({ memory: memory('m1') } as never)
    const detail = show(MemoryDetailView, { id: 'm1' })
    await flushPromises()
    await test(detail, 'memory-start-write').trigger('click')
    await flushPromises()
    expect(test(detail, 'memory-write-files').exists()).toBe(true)

    setActivePinia(createPinia())
    signIn('self')
    vi.mocked(getAlbum).mockResolvedValue({ album: album() })
    const albumPage = show(AlbumDetailView, { id: 'album-1' })
    await flushPromises()
    expect(test(albumPage, 'album-files').exists()).toBe(true)

    expect(getMediaCapabilities).toHaveBeenCalledTimes(3)
    expect(listMemories).not.toHaveBeenCalled()
    expect(listAlbums).not.toHaveBeenCalled()
  })

  it('while the capability is loading no picker is offered; the timeline still loads and shows its own hint', async () => {
    const slow = deferred<MediaCapabilities>()
    vi.mocked(getMediaCapabilities).mockReturnValueOnce(slow.promise)
    const page = show(MemoryNewView)
    await flushPromises()
    expect(test(page, 'memory-new-loading').exists()).toBe(true)
    expect(test(page, 'memory-create-files').exists()).toBe(false)
    expect(test(page, 'memory-create-uploads-loading').exists()).toBe(true)
    slow.resolve({ uploads_enabled: true })
    await flushPromises()
    expect(test(page, 'memory-create-files').exists()).toBe(true)

    vi.mocked(listMemories).mockResolvedValueOnce({ memories: [memory('m2')], next_cursor: null, uploads_enabled: false } as never)
    const timeline = show(MemoriesView)
    await flushPromises()
    expect(listMemories).toHaveBeenCalledTimes(1)
    expect(test(timeline, 'memories-uploads-off').exists()).toBe(true)
  })
})

describe('disabled uploads keep what already exists', () => {
  it('the memory editor still offers previously uploaded photos and keeps an uncertain upload\'s retry', async () => {
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    useMediaCapabilityStore().$patch({ uploadsEnabled: true, status: 'ready' })
    const wrapper = show(MemoryEditor, { name: 'f', scope: 's', submitLabel: '保存', initial: null })
    const input = test(wrapper, 'f-files')
    Object.defineProperty(input.element, 'files', { value: [new File(['xxx'], 'a.png', { type: 'image/png' })], configurable: true })
    await input.trigger('change')
    await flushPromises()
    expect(test(wrapper, 'f-upload-retry').exists()).toBe(true)
    useMediaCapabilityStore().markDisabled()
    await flushPromises()
    expect(test(wrapper, 'f-files').exists()).toBe(false)
    expect(test(wrapper, 'f-uploads-off').exists()).toBe(true)
    expect(useMemoryUploadsStore().items).toHaveLength(1)
    expect(test(wrapper, 'f-upload-retry').exists()).toBe(true)
    await test(wrapper, 'f-unattached-toggle').trigger('click')
    await flushPromises()
    expect(wrapper.find(`[data-test="f-unattached-item"][data-media="${P2}"]`).exists()).toBe(true)
  })

  it('the album adder still lets previously uploaded photos be chosen and added', async () => {
    useMediaCapabilityStore().$patch({ uploadsEnabled: false, status: 'ready' })
    const wrapper = show(AlbumPhotoAdder, { albumId: 'album-1' })
    expect(test(wrapper, 'album-files').exists()).toBe(false)
    expect(test(wrapper, 'album-uploads-off').exists()).toBe(true)
    await test(wrapper, 'album-unattached-toggle').trigger('click')
    await flushPromises()
    await wrapper.find(`[data-test="album-unattached-item"][data-media="${P2}"] [data-test="album-unattached-pick"]`).trigger('change')
    await test(wrapper, 'album-unattached-confirm').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="album-chosen-item"]')).toHaveLength(1)
    expect((test(wrapper, 'album-add-submit').element as HTMLButtonElement).disabled).toBe(false)
  })
})

describe('what stays private', () => {
  it('no capability source touches storage, caches or the console', () => {
    for (const path of ['api/media-capability.ts', 'stores/media-capability.ts']) {
      const source = readFileSync(resolve(__dirname, '../../src', path), 'utf8')
      expect(source, path).not.toMatch(/localStorage|sessionStorage|indexedDB|caches\.|console\./)
    }
  })
})
