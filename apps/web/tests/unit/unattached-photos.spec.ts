/**
 * Choosing previously uploaded, still unattached photos (contract 0.39.0, web): the page validator; the identity-scoped store and its
 * late answers; the memory editor's combined order, limit and duplicate rules; the album adder's separate selection, ordered payload,
 * refusal handling and locked retry; and that it is not a deletion recovery and stores nothing in the browser. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/unattached', async (original) => ({
  ...(await original<typeof import('@/api/unattached')>()),
  listUnattachedPhotos: vi.fn(),
}))
vi.mock('@/api/albums', () => ({
  ALBUM_PAGE_SIZE: 20, ALBUM_PHOTO_PAGE_SIZE: 30,
  createAlbum: vi.fn(), listAlbums: vi.fn(), getAlbum: vi.fn(), listAlbumPhotos: vi.fn(), addAlbumPhotos: vi.fn(), removeAlbumPhoto: vi.fn(),
  setAlbumCover: vi.fn(), renameAlbum: vi.fn(), proposeAlbumRename: vi.fn(), getAlbumProposal: vi.fn(), decideAlbumProposal: vi.fn(),
  listAlbumProposals: vi.fn(),
}))
vi.mock('@/api/media', async (original) => ({
  ...(await original<typeof import('@/api/media')>()),
  createMediaUpload: vi.fn(), completeMediaUpload: vi.fn(), postToStaging: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))

import { addAlbumPhotos, listAlbumPhotos } from '@/api/albums'
import { ApiError, setCsrfToken } from '@/api/client'
import { completeMediaUpload, createMediaUpload, postToStaging } from '@/api/media'
import { isUnattachedPage, listUnattachedPhotos, type UnattachedPage } from '@/api/unattached'
import type { Album, AlbumMutation, AlbumMutationResponse, MediaResponse, MemoryTerms } from '@/api/types'
import AlbumPhotoAdder from '@/components/AlbumPhotoAdder.vue'
import MemoryEditor from '@/components/MemoryEditor.vue'
import { useAlbumsStore } from '@/stores/albums'
import { useSessionStore } from '@/stores/session'
import { useUnattachedStore } from '@/stores/unattached'
import { useMediaCapabilityStore } from '@/stores/media-capability'

const NOW = '2026-11-08T01:00:00.000000Z'
const id = (n: number) => `00000000-0000-4000-8000-${String(n).padStart(12, '0')}`
const R1 = id(1)
const R2 = id(2)
const R3 = id(3)
const UP = id(90)

function photos(...ids: string[]): UnattachedPage {
  return { photos: ids.map((media_id) => ({ media_id, uploaded_at: NOW, width: 4, height: 3 })), next_cursor: null }
}
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
  const promise = new Promise<T>((yes) => { resolveIt = yes })
  return { promise, resolve: resolveIt }
}
function album(overrides: Partial<Album> = {}): Album {
  return { id: 'album-1', creator_id: 'self', name: '旅行', version: 1, pending_proposal: null, created_at: NOW, photo_count: 0, cover: null, ...overrides }
}
function added(ids: string[]): AlbumMutationResponse {
  return { mutation: { album_id: 'album-1', actor_id: 'self', created_at: NOW, action: 'add_photos', batch_id: 'b', media_ids: ids } as unknown as AlbumMutation,
    album: album({ photo_count: ids.length }), notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null } }
}
function grantResponse(mediaId: string): MediaResponse {
  return {
    media: { media_id: mediaId, state: 'granted', uploader_id: 'self', content_type: 'image/png', size_bytes: 3, created_at: NOW, reason: null, width: null, height: null },
    upload: { url: 'https://bucket.example.test/staging/x', fields: { key: 'k' }, expires_at: new Date(Date.now() + 600_000).toISOString() },
  }
}
function readyResponse(mediaId: string): MediaResponse {
  const base = grantResponse(mediaId)
  return { media: { ...base.media, state: 'ready', width: 4, height: 3 }, upload: null }
}

const stubs = { RouterLink: { template: '<a><slot /></a>' } }
const mounted: VueWrapper[] = []
function show<T extends object>(component: T, props: Record<string, unknown>): VueWrapper {
  const wrapper = mount(component as never, { props: props as never, global: { stubs } }) as VueWrapper
  mounted.push(wrapper)
  return wrapper
}
function test(wrapper: VueWrapper, name: string) {
  return wrapper.find(`[data-test="${name}"]`)
}
async function openPicker(wrapper: VueWrapper, name: string): Promise<void> {
  await test(wrapper, `${name}-unattached-toggle`).trigger('click')
  await flushPromises()
}
async function tick(wrapper: VueWrapper, name: string, mediaId: string): Promise<void> {
  await wrapper.find(`[data-test="${name}-unattached-item"][data-media="${mediaId}"] [data-test="${name}-unattached-pick"]`).trigger('change')
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
  vi.mocked(listAlbumPhotos).mockResolvedValue({ photos: [], next_cursor: null } as never)
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('page and store', () => {
  it('accepts only well-formed pages', () => {
    expect(isUnattachedPage(photos(R1))).toBe(true)
    expect(isUnattachedPage({ photos: [{ media_id: '' }], next_cursor: null })).toBe(false)
    expect(isUnattachedPage({ photos: [], next_cursor: 3 })).toBe(false)
    expect(isUnattachedPage({ next_cursor: null })).toBe(false)
  })

  it('pages without duplicates, and a newer read wins', async () => {
    const store = useUnattachedStore()
    vi.mocked(listUnattachedPhotos).mockResolvedValueOnce({ ...photos(R1, R2), next_cursor: 'c1' })
    await store.load()
    vi.mocked(listUnattachedPhotos).mockResolvedValueOnce(photos(R2, R3))
    await store.loadMore()
    expect(listUnattachedPhotos).toHaveBeenLastCalledWith('c1')
    expect(store.items.map((p) => p.media_id)).toEqual([R1, R2, R3])
    const slow = deferred<UnattachedPage>()
    vi.mocked(listUnattachedPhotos).mockReturnValueOnce(slow.promise).mockResolvedValueOnce(photos(R3))
    const first = store.load()
    await store.load()
    slow.resolve(photos(R1))
    await first
    expect(store.items.map((p) => p.media_id)).toEqual([R3])
  })

  it('drops a late page after an account switch, and a fresh session reads the server again', async () => {
    const store = useUnattachedStore()
    const slow = deferred<UnattachedPage>()
    vi.mocked(listUnattachedPhotos).mockReturnValueOnce(slow.promise)
    const loading = store.load()
    signIn('other')
    slow.resolve(photos(R1))
    await loading
    expect(store.items).toEqual([])
    expect(store.status).toBe('idle')
    setActivePinia(createPinia())
    signIn('self')
    const fresh = useUnattachedStore()
    vi.mocked(listUnattachedPhotos).mockResolvedValueOnce(photos(R1))
    await fresh.load()
    expect(fresh.items.map((p) => p.media_id)).toEqual([R1])
  })
})

describe('memory editor', () => {
  const initial: MemoryTerms = { date: '2026-03-01', title: '海边', description: '', media_ids: [id(10), id(11), id(12), id(13), id(14), id(15), id(16)] }

  it('adds chosen photos after the existing ones, in the order ticked, within the limit, never twice', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, id(10), R2, R3))
    const wrapper = show(MemoryEditor, { name: 'w', scope: 'write:a', submitLabel: '保存', initial })
    await openPicker(wrapper, 'w')
    // Already in the form: not offered.
    expect(wrapper.find(`[data-test="w-unattached-item"][data-media="${id(10)}"]`).exists()).toBe(false)
    expect(test(wrapper, 'w-unattached-note').text()).toContain('不包括已删除的照片')
    expect(test(wrapper, 'w-unattached-note').text()).toContain('不会延长')
    // Two places left (7 of 9): the third box cannot be ticked.
    await tick(wrapper, 'w', R2)
    await tick(wrapper, 'w', R1)
    const third = wrapper.find(`[data-test="w-unattached-item"][data-media="${R3}"] [data-test="w-unattached-pick"]`)
    expect((third.element as HTMLInputElement).disabled).toBe(true)
    await test(wrapper, 'w-unattached-confirm').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="w-photo"]')).toHaveLength(9)
    await test(wrapper, 'w-form').trigger('submit')
    const emitted = wrapper.emitted('submit') as [MemoryTerms][]
    expect(emitted[0][0].media_ids).toEqual([...initial.media_ids, R2, R1])
    // Full now: the picker says so.
    await openPicker(wrapper, 'w')
    expect(test(wrapper, 'w-unattached-full').exists()).toBe(true)
  })

  it('is hidden while the form is locked by an action, so a captured payload cannot change', async () => {
    const wrapper = show(MemoryEditor, { name: 'w', scope: 'write:a', submitLabel: '保存', initial, busy: true })
    expect(test(wrapper, 'w-unattached').exists()).toBe(false)
    expect(listUnattachedPhotos).not.toHaveBeenCalled()
  })
})

describe('album adder', () => {
  function adder(): VueWrapper {
    useMediaCapabilityStore().$patch({ uploadsEnabled: true, status: 'ready' })
    return show(AlbumPhotoAdder, { albumId: 'album-1' })
  }
  async function choose(wrapper: VueWrapper, ids: string[]): Promise<void> {
    await openPicker(wrapper, 'album')
    for (const mediaId of ids) await tick(wrapper, 'album', mediaId)
    await test(wrapper, 'album-unattached-confirm').trigger('click')
    await flushPromises()
  }
  async function uploadOne(wrapper: VueWrapper): Promise<void> {
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(UP))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockImplementation((mediaId: string) => Promise.resolve(readyResponse(mediaId)))
    const input = test(wrapper, 'album-files')
    Object.defineProperty(input.element, 'files', { value: [new File(['xxx'], 'a.png', { type: 'image/png' })], configurable: true })
    await input.trigger('change')
    await flushPromises()
  }

  it('keeps chosen photos apart from uploads and sends both in the user\'s order, each once', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2, UP))
    const wrapper = adder()
    await choose(wrapper, [R1])
    await uploadOne(wrapper)
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(1)
    expect(wrapper.findAll('[data-test="album-chosen-item"]')).toHaveLength(1)
    // The finished upload is now in the form, so the list does not offer it again.
    await openPicker(wrapper, 'album')
    expect(wrapper.find(`[data-test="album-unattached-item"][data-media="${UP}"]`).exists()).toBe(false)
    await tick(wrapper, 'album', R2)
    await test(wrapper, 'album-unattached-confirm').trigger('click')
    await flushPromises()
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(added([R1, UP, R2]))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(vi.mocked(addAlbumPhotos).mock.calls[0].slice(0, 2)).toEqual(['album-1', [R1, UP, R2]])
    expect(wrapper.findAll('[data-test="album-chosen-item"]')).toHaveLength(0)
  })

  it('a refused add keeps every choice, says the list may have changed, and reads it again', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2))
    const wrapper = adder()
    await choose(wrapper, [R1, R2])
    vi.mocked(addAlbumPhotos).mockRejectedValueOnce(new ApiError('ALBUM_PHOTO_UNAVAILABLE', 'x', 409))
    const reads = vi.mocked(listUnattachedPhotos).mock.calls.length
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="album-chosen-item"]').map((w) => w.attributes('data-media'))).toEqual([R1, R2])
    expect(test(wrapper, 'album-refused-note').text()).toContain('列表可能已经变化')
    expect(vi.mocked(listUnattachedPhotos).mock.calls.length).toBe(reads + 1)
    expect(addAlbumPhotos).toHaveBeenCalledTimes(1)
  })

  it('an unconfirmed add locks the selection, and its retry resends exactly what it captured', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2))
    const wrapper = adder()
    await choose(wrapper, [R2, R1])
    vi.mocked(addAlbumPhotos).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect((test(wrapper, 'album-unattached-toggle').element as HTMLButtonElement).disabled).toBe(true)
    for (const remove of wrapper.findAll('[data-test="album-chosen-remove"]')) expect((remove.element as HTMLButtonElement).disabled).toBe(true)
    const store = useAlbumsStore()
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(added([R2, R1]))
    await store.retry('add:album-1')
    await flushPromises()
    const [first, second] = vi.mocked(addAlbumPhotos).mock.calls
    expect(second[1]).toEqual([R2, R1])
    expect(second[2]).toBe(first[2])
  })

  it('a chosen photo leaves the selection once the album shows it, also after a confirmation that came another way', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2))
    const wrapper = adder()
    await choose(wrapper, [R1, R2])
    const store = useAlbumsStore()
    store.photos = { albumId: 'album-1', status: 'ready', items: [{ seq: 1, media_id: R1, uploader_id: 'self', width: 4, height: 3, added_at: NOW }],
      nextCursor: null, loadingMore: false, errorText: '', moreErrorText: '' }
    await flushPromises()
    expect(wrapper.findAll('[data-test="album-chosen-item"]').map((w) => w.attributes('data-media'))).toEqual([R2])
  })

  it('counts chosen photos and uploads against the same per-add limit', async () => {
    const many = Array.from({ length: 21 }, (_, n) => id(100 + n))
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(...many))
    const wrapper = adder()
    await openPicker(wrapper, 'album')
    for (const mediaId of many.slice(0, 20)) await tick(wrapper, 'album', mediaId)
    const last = wrapper.find(`[data-test="album-unattached-item"][data-media="${many[20]}"] [data-test="album-unattached-pick"]`)
    expect((last.element as HTMLInputElement).disabled).toBe(true)
    await test(wrapper, 'album-unattached-confirm').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="album-chosen-item"]')).toHaveLength(20)
    expect((test(wrapper, 'album-files').element as HTMLInputElement).disabled).toBe(true)
  })
})

describe('D20 opt-in partner notice on an album batch', () => {
  const delivered = { requested: true, created: true, reason: 'delivered_to_inbox' as const, notification_id: id(77) }
  function adder(): VueWrapper {
    useMediaCapabilityStore().$patch({ uploadsEnabled: true, status: 'ready' })
    return show(AlbumPhotoAdder, { albumId: 'album-1' })
  }
  async function choose(wrapper: VueWrapper, ids: string[]): Promise<void> {
    await openPicker(wrapper, 'album')
    for (const mediaId of ids) await tick(wrapper, 'album', mediaId)
    await test(wrapper, 'album-unattached-confirm').trigger('click')
    await flushPromises()
  }
  const box = (wrapper: VueWrapper) => test(wrapper, 'album-add-notify-input').element as HTMLInputElement

  it('offers one off-by-default choice per batch and sends a three-photo batch as one request with it', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2, R3))
    const wrapper = adder()
    await flushPromises()
    expect(test(wrapper, 'album-add-notify').exists()).toBe(false)
    await choose(wrapper, [R1, R2, R3])
    expect(box(wrapper).checked).toBe(false)
    await test(wrapper, 'album-add-notify-input').setValue(true)
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce({ ...added([R1, R2, R3]), notification: delivered })
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(addAlbumPhotos).toHaveBeenCalledTimes(1)
    expect(vi.mocked(addAlbumPhotos).mock.calls[0][0]).toBe('album-1')
    expect(vi.mocked(addAlbumPhotos).mock.calls[0][1]).toEqual([R1, R2, R3])
    expect(vi.mocked(addAlbumPhotos).mock.calls[0][3]).toBe(true)
    // The next batch starts off again: nothing is remembered.
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(UP))
    await choose(wrapper, [UP])
    expect(box(wrapper).checked).toBe(false)
  })

  it('keeps an unconfirmed batch locked to its captured choice and retries it unchanged', async () => {
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1, R2))
    const wrapper = adder()
    await choose(wrapper, [R1, R2])
    await test(wrapper, 'album-add-notify-input').setValue(true)
    vi.mocked(addAlbumPhotos).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(box(wrapper).checked).toBe(true)
    expect(box(wrapper).disabled).toBe(true)
    // Facts for another choice do not confirm it; the matching ones do.
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(added([R1, R2]))
    await useAlbumsStore().retry('add:album-1')
    await flushPromises()
    expect(box(wrapper).disabled).toBe(true)
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce({ ...added([R1, R2]), notification: delivered })
    await useAlbumsStore().retry('add:album-1')
    await flushPromises()
    const calls = vi.mocked(addAlbumPhotos).mock.calls
    expect(calls).toHaveLength(3)
    expect(calls[1]).toEqual(calls[0])
    expect(calls[2]).toEqual(calls[0])
    expect(calls[0][3]).toBe(true)
    expect(useAlbumsStore().actions['add:album-1']).toBeUndefined()
  })

  it('with no partner the choice is unavailable and the batch sends false', async () => {
    useSessionStore().applyBootstrap({
      authenticated: true, csrf_token: 'csrf-self',
      user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: NOW },
      space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 1, created_at: NOW, members: [] },
    })
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1))
    const wrapper = adder()
    await choose(wrapper, [R1])
    expect(box(wrapper).disabled).toBe(true)
    expect(test(wrapper, 'album-add-notify-unavailable').exists()).toBe(true)
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(added([R1]))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(vi.mocked(addAlbumPhotos).mock.calls[0][3]).toBe(false)
  })
})

describe('what stays private', () => {
  it('no source of this feature touches storage, caches or the console, or offers to restore anything', () => {
    for (const path of ['api/unattached.ts', 'stores/unattached.ts', 'components/UnattachedPhotoPicker.vue', 'components/AlbumPhotoAdder.vue', 'components/MemoryEditor.vue']) {
      const source = readFileSync(resolve(__dirname, '../../src', path), 'utf8')
      expect(source, path).not.toMatch(/localStorage|sessionStorage|indexedDB|caches\./)
      expect(source, path).not.toMatch(/console\./)
    }
    const picker = readFileSync(resolve(__dirname, '../../src/components/UnattachedPhotoPicker.vue'), 'utf8')
    expect(picker).not.toMatch(/undo|恢复/)
  })

  it('a full choose flow writes nothing to storage', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem')
    vi.mocked(listUnattachedPhotos).mockResolvedValue(photos(R1))
    const wrapper = show(MemoryEditor, { name: 'w', scope: 'write:b', submitLabel: '保存', initial: null })
    await openPicker(wrapper, 'w')
    await tick(wrapper, 'w', R1)
    await test(wrapper, 'w-unattached-confirm').trigger('click')
    await flushPromises()
    expect(setItem).not.toHaveBeenCalled()
    expect(localStorage.length).toBe(0)
    expect(sessionStorage.length).toBe(0)
  })
})
