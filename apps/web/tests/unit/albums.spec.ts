/**
 * P3-2c shared albums (web): both member roles; pending-request restrictions; replay with captured key and payload; atomic batch
 * failure; cover fallback; removal and its explanation; disabled uploads; late responses after a route or account change. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

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

import { checkAlbumName, isAlbum, isAlbumProposal } from '@/api/album-rules'
import {
  addAlbumPhotos,
  createAlbum,
  decideAlbumProposal,
  getAlbum,
  getAlbumProposal,
  listAlbumPhotos,
  listAlbums,
  proposeAlbumRename,
  removeAlbumPhoto,
  renameAlbum,
  setAlbumCover,
} from '@/api/albums'
import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { getMediaCapabilities } from '@/api/media-capability'
import { completeMediaUpload, createMediaUpload, postToStaging } from '@/api/media'
import type { Album, AlbumMutation, AlbumMutationResponse, AlbumPhoto, AlbumProposal, MediaResponse } from '@/api/types'
import { isUsableAlbumResponse, useAlbumsStore, type AlbumAction } from '@/stores/albums'
import { useMemoriesStore } from '@/stores/memories'
import { useMemoryUploadsStore } from '@/stores/memory-uploads'
import { useSessionStore } from '@/stores/session'
import AlbumDetailView from '@/views/AlbumDetailView.vue'
import AlbumsView from '@/views/AlbumsView.vue'

const A1 = 'album-1'
const P1 = '11111111-1111-4111-8111-111111111111'
const P2 = '22222222-2222-4222-8222-222222222222'
const PR = 'proposal-1'
const NOW = '2026-11-04T01:00:00.000000Z'

function album(overrides: Partial<Album> = {}): Album {
  return { id: A1, creator_id: 'self', name: '旅行', version: 1, pending_proposal: null, created_at: NOW, photo_count: 0, cover: null, ...overrides }
}
function photo(id: string, uploader = 'self', seq = 1): AlbumPhoto {
  return { seq, media_id: id, uploader_id: uploader, width: 4, height: 3, added_at: NOW }
}
function cover(id: string, chosen = false) {
  return { media_id: id, uploader_id: 'self', width: 4, height: 3, chosen }
}
function done(mutation: Partial<AlbumMutation>, a: Album = album()): AlbumMutationResponse {
  const response: AlbumMutationResponse = { mutation: { album_id: a.id, actor_id: 'self', created_at: NOW, action: 'create', ...mutation } as AlbumMutation, album: a }
  // D20 (0.41.0): an add batch always carries its original notification decision; the page sends false unless opted in.
  return mutation.action === 'add_photos' ? { ...response, notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null } } : response
}
function pendingAlbum(proposer = 'partner', creator = 'self'): Album {
  return album({ creator_id: creator, pending_proposal: { id: PR, proposer_id: proposer, base_version: 1, created_at: NOW } })
}
function proposalRecord(overrides: Partial<AlbumProposal> = {}): AlbumProposal {
  return { id: PR, album_id: A1, proposer_id: 'partner', base_version: 1, created_at: NOW, status: 'pending', decided_by: null, decided_at: null, approved_version: null,
    original_name: '旅行', proposed_name: '海边', ...overrides }
}
function page(albums: Album[], next: string | null = null, uploads: unknown = true) {
  return { albums, next_cursor: next, uploads_enabled: uploads } as never
}
function photoPage(photos: AlbumPhoto[], next: string | null = null) {
  return { photos, next_cursor: next } as never
}
function grantResponse(id: string): MediaResponse {
  return {
    media: { media_id: id, state: 'granted', uploader_id: 'self', content_type: 'image/png', size_bytes: 3, created_at: NOW, reason: null, width: null, height: null },
    upload: { url: 'https://bucket.example.test/staging/x', fields: { key: 'k' }, expires_at: new Date(Date.now() + 600_000).toISOString() },
  }
}
function readyResponse(id: string): MediaResponse {
  const base = grantResponse(id)
  return { media: { ...base.media, state: 'ready', width: 4, height: 3 }, upload: null }
}
function file(name = 'a.png'): File {
  return new File(['xxx'], name, { type: 'image/png' })
}
function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: NOW },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: NOW, members: [] },
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
const server = () => new ApiError('RESULT_UNKNOWN', 'x', 503)
const refused = (code: string, status = 409) => new ApiError(code, 'x', status)

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
function photoCard(wrapper: VueWrapper, id: string) {
  return wrapper.find(`[data-test="album-photo"][data-media="${id}"]`)
}
async function openDetail(a: Album, photos: AlbumPhoto[] = []): Promise<VueWrapper> {
  vi.mocked(getAlbum).mockResolvedValue({ album: a })
  vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage(photos))
  vi.mocked(listAlbums).mockResolvedValue(page([a]))
  vi.mocked(getAlbumProposal).mockResolvedValue({ proposal: proposalRecord() })
  const wrapper = show(AlbumDetailView, { id: a.id })
  await flushPromises()
  return wrapper
}
async function pickFiles(wrapper: VueWrapper, files: File[]): Promise<void> {
  const input = test(wrapper, 'album-files')
  Object.defineProperty(input.element, 'files', { value: files, configurable: true })
  await input.trigger('change')
  await flushPromises()
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
  vi.mocked(listAlbums).mockResolvedValue(page([]))
  // Pickers learn upload capability from its own query (contract 0.40.0), not from the album list.
  vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('rules and shapes', () => {
  it('holds the name limits and normalises what is submitted', () => {
    expect(checkAlbumName('  旅行 ')).toEqual({ ok: true, value: '旅行' })
    expect(checkAlbumName('天'.repeat(40)).ok).toBe(true)
    expect(checkAlbumName('天'.repeat(41)).ok).toBe(false)
    expect(checkAlbumName('   ').ok).toBe(false)
    expect(checkAlbumName('a\nb').ok).toBe(false)
  })

  it('accepts only a whole album and proposal', () => {
    expect(isAlbum(album())).toBe(true)
    expect(isAlbum(album({ cover: cover(P1) }))).toBe(true)
    for (const bad of [album({ version: 0 }), album({ name: '' }), album({ photo_count: -1 }), { ...album(), cover: { media_id: P1 } }, { ...album(), pending_proposal: { id: PR } }, { id: A1 }, null]) {
      expect(isAlbum(bad)).toBe(false)
    }
    expect(isAlbumProposal(proposalRecord(), A1, PR)).toBe(true)
    expect(isAlbumProposal(proposalRecord(), 'other', PR)).toBe(false)
    expect(isAlbumProposal(proposalRecord({ status: 'maybe' as never }), A1, PR)).toBe(false)
  })

  it('has a message for every album error code', () => {
    for (const code of ['ALBUM_NOT_FOUND', 'ALBUM_PHOTO_UNAVAILABLE', 'ALBUM_PHOTO_NOT_FOUND', 'ALBUM_PHOTO_FORBIDDEN', 'ALBUM_PHOTO_REMOVED', 'ALBUM_EDIT_FORBIDDEN',
      'ALBUM_VERSION_CONFLICT', 'ALBUM_PROPOSAL_PENDING', 'ALBUM_PROPOSAL_NOT_FOUND', 'ALBUM_PROPOSAL_DECIDED']) {
      expect(describeError(new ApiError(code, 'SERVER-TEXT', 409))).not.toBe('SERVER-TEXT')
    }
  })
})

describe('confirmed facts against the request', () => {
  const base = { key: 'k', status: 'submitting', errorText: '' } as const
  const action = (a: Partial<AlbumAction>) => ({ id: 'x', kind: 'create', ...base, ...a }) as AlbumAction

  it('compares a name only at the mutation\'s own version, and never accepts an older album', () => {
    const rename = action({ kind: 'rename', albumId: A1, expectedVersion: 1, name: '新' })
    const facts = { action: 'edit' as const, version: 2 }
    expect(isUsableAlbumResponse(done(facts, album({ version: 2, name: '新' })), rename, 'self')).toBe(true)
    // A legitimate newer state on replay.
    expect(isUsableAlbumResponse(done(facts, album({ version: 3, name: '更新的' })), rename, 'self')).toBe(true)
    // Wrong name at its own version; older than it confirms; wrong version; another actor; another album; malformed.
    expect(isUsableAlbumResponse(done(facts, album({ version: 2, name: '别的' })), rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done(facts, album({ version: 1, name: '新' })), rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ action: 'edit', version: 3 }, album({ version: 3, name: '新' })), rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ ...facts, actor_id: 'partner' }, album({ version: 2, name: '新' })), rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done(facts, album({ id: 'other', version: 2, name: '新' })), rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse({ mutation: facts }, rename, 'self')).toBe(false)
    expect(isUsableAlbumResponse('nope', rename, 'self')).toBe(false)
  })

  it('requires the add\'s ids in the chosen order, whatever the album shows today', () => {
    const add = action({ kind: 'add', albumId: A1, mediaIds: [P1, P2] })
    const facts = { action: 'add_photos' as const, batch_id: 'b', media_ids: [P1, P2] }
    expect(isUsableAlbumResponse(done(facts, album({ photo_count: 2 })), add, 'self')).toBe(true)
    // A replay after a later removal: the facts stay, and today's count is lower.
    expect(isUsableAlbumResponse(done(facts, album({ photo_count: 1 })), add, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done({ ...facts, media_ids: [P2, P1] }), add, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ ...facts, media_ids: [P1] }), add, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ ...facts, batch_id: '' }), add, 'self')).toBe(false)
  })

  it('checks removal, cover and proposal facts', () => {
    const remove = action({ kind: 'remove', albumId: A1, mediaId: P1 })
    expect(isUsableAlbumResponse(done({ action: 'remove_photo', media_id: P1 }), remove, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done({ action: 'remove_photo', media_id: P2 }), remove, 'self')).toBe(false)
    const chooseCover = action({ kind: 'cover', albumId: A1, mediaId: P1 })
    expect(isUsableAlbumResponse(done({ action: 'set_cover', media_id: P1, event_no: 1 }), chooseCover, 'self')).toBe(true)
    // A later choice may already show on replay: today's cover is not compared.
    expect(isUsableAlbumResponse(done({ action: 'set_cover', media_id: P1, event_no: 1 }, album({ cover: cover(P2, true) })), chooseCover, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done({ action: 'set_cover', media_id: P2, event_no: 1 }), chooseCover, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ action: 'set_cover', media_id: P1 }), chooseCover, 'self')).toBe(false)
    const propose = action({ kind: 'propose', albumId: A1, expectedVersion: 1, name: '海边' })
    expect(isUsableAlbumResponse(done({ action: 'propose', proposal_id: PR, base_version: 1 }), propose, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done({ action: 'propose', proposal_id: PR, base_version: 2 }, album({ version: 2 })), propose, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ action: 'propose', base_version: 1 }), propose, 'self')).toBe(false)
  })

  it('checks decisions: an approval applies exactly the requested name at its version', () => {
    const approve = action({ kind: 'decide', albumId: A1, proposalId: PR, outcome: 'approve', name: '海边' })
    const facts = { action: 'decide' as const, proposal_id: PR, outcome: 'approved' as const, base_version: 1, version: 2 }
    expect(isUsableAlbumResponse(done(facts, album({ version: 2, name: '海边' })), approve, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done(facts, album({ version: 4, name: '后来' })), approve, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done(facts, album({ version: 2, name: '别的' })), approve, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done(facts, album({ version: 1, name: '海边' })), approve, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ ...facts, outcome: 'rejected' }, album({ version: 2, name: '海边' })), approve, 'self')).toBe(false)
    expect(isUsableAlbumResponse(done({ ...facts, proposal_id: 'other' }, album({ version: 2, name: '海边' })), approve, 'self')).toBe(false)
    const reject = action({ kind: 'decide', albumId: A1, proposalId: PR, outcome: 'reject' })
    expect(isUsableAlbumResponse(done({ action: 'decide', proposal_id: PR, outcome: 'rejected', base_version: 1, version: null }), reject, 'self')).toBe(true)
    expect(isUsableAlbumResponse(done({ action: 'decide', proposal_id: PR, outcome: 'rejected', base_version: 1, version: 2 }), reject, 'self')).toBe(false)
  })
})

describe('list', () => {
  it('pages by the cursor unchanged, keeps a failed later page from losing the first, and states uploads', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbums).mockResolvedValueOnce(page([album({ id: 'a' }), album({ id: 'b' })], 'c1'))
    await store.load()
    expect(store.items.map((a) => a.id)).toEqual(['a', 'b'])
    expect(store.uploadsEnabled).toBe(true)
    vi.mocked(listAlbums).mockRejectedValueOnce(network())
    await store.loadMore()
    expect(store.items).toHaveLength(2)
    expect(store.moreErrorText).not.toBe('')
    vi.mocked(listAlbums).mockResolvedValueOnce({ albums: [{ id: 'x' }], next_cursor: null } as never)
    await store.loadMore()
    expect(store.nextCursor).toBe('c1')
    vi.mocked(listAlbums).mockResolvedValueOnce(page([album({ id: 'b', name: 'moved' }), album({ id: 'c' })]))
    await store.loadMore()
    expect(listAlbums).toHaveBeenLastCalledWith('c1')
    expect(store.items.map((a) => a.id)).toEqual(['a', 'b', 'c'])
    expect(store.items[1].name).toBe('旅行')
    expect(store.nextCursor).toBeNull()
  })

  it.each([['not an object', 'nope'], ['no list', { next_cursor: null }], ['bad row', { albums: [{ id: 'x' }], next_cursor: null }], ['bad cursor', { albums: [], next_cursor: 3 }]])(
    'treats a malformed page as an error: %s', async (_name, response) => {
      vi.mocked(listAlbums).mockResolvedValue(response as never)
      const wrapper = show(AlbumsView)
      await flushPromises()
      expect(test(wrapper, 'albums-error').exists()).toBe(true)
    })

  it('shows rows, the empty state, a missing cover and a pending badge', async () => {
    vi.mocked(listAlbums).mockResolvedValue(page([album({ photo_count: 2, cover: cover(P1) }), album({ id: 'b', pending_proposal: pendingAlbum().pending_proposal })]))
    const wrapper = show(AlbumsView)
    await flushPromises()
    expect(wrapper.text()).toContain('2 张照片')
    expect(test(wrapper, 'album-no-cover').exists()).toBe(true)
    expect(test(wrapper, 'album-pending-badge').exists()).toBe(true)
    vi.mocked(listAlbums).mockResolvedValue(page([]))
    await useAlbumsStore().load()
    await flushPromises()
    expect(test(wrapper, 'albums-empty').exists()).toBe(true)
  })

  it('creates an empty album with uploads off, trimmed, and opens it', async () => {
    vi.mocked(listAlbums).mockResolvedValue(page([], null, false))
    const made = album({ name: '旅行' })
    vi.mocked(createAlbum).mockResolvedValue(done({ action: 'create', version: 1 }, made))
    const wrapper = show(AlbumsView)
    await flushPromises()
    expect(test(wrapper, 'albums-uploads-off').exists()).toBe(true)
    await test(wrapper, 'album-create-name').setValue('  旅行  ')
    await test(wrapper, 'album-create-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(createAlbum).mock.calls[0][0]).toBe('旅行')
    expect(routerPush).toHaveBeenCalledWith({ name: 'album-detail', params: { id: A1 } })
  })

  it('refuses a bad name before sending', async () => {
    const wrapper = show(AlbumsView)
    await flushPromises()
    await test(wrapper, 'album-create-name').setValue('天'.repeat(41))
    await test(wrapper, 'album-create-form').trigger('submit')
    expect(createAlbum).not.toHaveBeenCalled()
    expect(test(wrapper, 'album-create-error').text()).toContain('相册名称')
  })

  it('locks an unconfirmed create to its capture, replays with the same key and name, and survives leaving', async () => {
    vi.mocked(createAlbum).mockRejectedValueOnce(network())
    const wrapper = show(AlbumsView)
    await flushPromises()
    await test(wrapper, 'album-create-name').setValue('旅行')
    await test(wrapper, 'album-create-form').trigger('submit')
    await flushPromises()
    expect(test(wrapper, 'album-create-pending').exists()).toBe(true)
    expect((test(wrapper, 'album-create-name').element as HTMLInputElement).disabled).toBe(true)
    const [name, key] = vi.mocked(createAlbum).mock.calls[0]
    wrapper.unmount()
    mounted.pop()

    const back = show(AlbumsView)
    await flushPromises()
    expect((test(back, 'album-create-name').element as HTMLInputElement).value).toBe('旅行')
    vi.mocked(createAlbum).mockResolvedValueOnce(done({ action: 'create', version: 1 }, album()))
    await test(back, 'album-create-retry').trigger('click')
    await flushPromises()
    expect(vi.mocked(createAlbum).mock.calls[1]).toEqual([name, key])
    expect(routerPush).toHaveBeenCalledTimes(1)
  })
})

describe('both member roles and pending restrictions', () => {
  it('the creator renames directly; the partner only requests', async () => {
    const creator = await openDetail(album())
    await test(creator, 'album-start-rename').trigger('click')
    expect(test(creator, 'album-start-rename').exists()).toBe(false)
    await test(creator, 'album-name-input').setValue('新名')
    vi.mocked(renameAlbum).mockResolvedValue(done({ action: 'edit', version: 2 }, album({ version: 2, name: '新名' })))
    await test(creator, 'album-name-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(renameAlbum).mock.calls[0].slice(0, 3)).toEqual([A1, 1, '新名'])
    expect(proposeAlbumRename).not.toHaveBeenCalled()
    expect(test(creator, 'album-name').text()).toBe('新名')

    creator.unmount()
    mounted.pop()
    const partner = await openDetail(album({ creator_id: 'partner' }))
    expect(test(partner, 'album-start-rename').text()).toBe('申请改名')
    await test(partner, 'album-start-rename').trigger('click')
    await test(partner, 'album-name-input').setValue('海边')
    vi.mocked(proposeAlbumRename).mockResolvedValue(done({ action: 'propose', proposal_id: PR, base_version: 1 }, pendingAlbum('self', 'partner')))
    await test(partner, 'album-name-form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(proposeAlbumRename).mock.calls[0].slice(0, 3)).toEqual([A1, 1, '海边'])
    expect(renameAlbum).toHaveBeenCalledTimes(1)
    // The name has not changed; the request is what shows.
    expect(test(partner, 'album-name').text()).toBe('旅行')
    expect(test(partner, 'album-pending').exists()).toBe(true)
    expect(test(partner, 'album-name-write').exists()).toBe(false)
  })

  it('refuses the same name and a bad name before sending', async () => {
    const wrapper = await openDetail(album())
    await test(wrapper, 'album-start-rename').trigger('click')
    await test(wrapper, 'album-name-form').trigger('submit')
    expect(test(wrapper, 'album-name-error').text()).toContain('没有变化')
    await test(wrapper, 'album-name-input').setValue('  ')
    await test(wrapper, 'album-name-form').trigger('submit')
    expect(renameAlbum).not.toHaveBeenCalled()
  })

  it('while pending: the creator sees the names and decides, and cannot rename', async () => {
    const wrapper = await openDetail(pendingAlbum())
    expect(test(wrapper, 'album-name-write').exists()).toBe(false)
    expect(test(wrapper, 'album-review-original').text()).toBe('旅行')
    expect(test(wrapper, 'album-review-proposed').text()).toBe('海边')
    expect(test(wrapper, 'album-approve').exists()).toBe(true)
    expect(test(wrapper, 'album-reject').exists()).toBe(true)
    expect(test(wrapper, 'album-withdraw').exists()).toBe(false)
    vi.mocked(decideAlbumProposal).mockResolvedValue(done({ action: 'decide', proposal_id: PR, outcome: 'approved', base_version: 1, version: 2 }, album({ version: 2, name: '海边' })))
    await test(wrapper, 'album-approve').trigger('click')
    await flushPromises()
    expect(vi.mocked(decideAlbumProposal).mock.calls[0].slice(0, 3)).toEqual([A1, PR, 'approve'])
    expect(test(wrapper, 'album-name').text()).toBe('海边')
    expect(test(wrapper, 'album-pending').exists()).toBe(false)
    expect(test(wrapper, 'album-start-rename').exists()).toBe(true)
  })

  it('while pending: the proposer may withdraw, not approve; rejection and withdrawal keep the name', async () => {
    const partner = await openDetail(pendingAlbum('self', 'partner'))
    expect(test(partner, 'album-approve').exists()).toBe(false)
    expect(test(partner, 'album-reject').exists()).toBe(false)
    expect(test(partner, 'album-name-write').exists()).toBe(false)
    vi.mocked(decideAlbumProposal).mockResolvedValue(done({ action: 'decide', proposal_id: PR, outcome: 'withdrawn', base_version: 1, version: null }, album({ creator_id: 'partner' })))
    await test(partner, 'album-withdraw').trigger('click')
    await flushPromises()
    expect(vi.mocked(decideAlbumProposal).mock.calls[0][2]).toBe('withdraw')
    expect(test(partner, 'album-name').text()).toBe('旅行')
    expect(test(partner, 'album-pending').exists()).toBe(false)
    partner.unmount()
    mounted.pop()

    const creator = await openDetail(pendingAlbum())
    vi.mocked(decideAlbumProposal).mockResolvedValue(done({ action: 'decide', proposal_id: PR, outcome: 'rejected', base_version: 1, version: null }, album()))
    await test(creator, 'album-reject').trigger('click')
    await flushPromises()
    expect(test(creator, 'album-name').text()).toBe('旅行')
  })

  it('approval waits for the names, and a failed review read can be retried', async () => {
    vi.mocked(getAlbumProposal).mockRejectedValueOnce(network())
    vi.mocked(getAlbum).mockResolvedValue({ album: pendingAlbum() })
    vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([]))
    const wrapper = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(test(wrapper, 'album-review-error').exists()).toBe(true)
    expect((test(wrapper, 'album-approve').element as HTMLButtonElement).disabled).toBe(true)
    vi.mocked(getAlbumProposal).mockResolvedValue({ proposal: proposalRecord() })
    await test(wrapper, 'album-review-retry').trigger('click')
    await flushPromises()
    expect((test(wrapper, 'album-approve').element as HTMLButtonElement).disabled).toBe(false)
  })

  it('a pending refusal re-reads and shows the request, and a version conflict keeps what was typed', async () => {
    const wrapper = await openDetail(album())
    await test(wrapper, 'album-start-rename').trigger('click')
    await test(wrapper, 'album-name-input').setValue('打字')
    vi.mocked(renameAlbum).mockRejectedValueOnce(refused('ALBUM_VERSION_CONFLICT'))
    vi.mocked(getAlbum).mockResolvedValue({ album: album({ version: 2, name: '别人改的' }) })
    await test(wrapper, 'album-name-form').trigger('submit')
    await flushPromises()
    expect(test(wrapper, 'album-conflict').text()).toContain('别人改的')
    expect((test(wrapper, 'album-name-input').element as HTMLInputElement).value).toBe('打字')
    expect((test(wrapper, 'album-name-submit').element as HTMLButtonElement).disabled).toBe(true)
    await test(wrapper, 'album-use-latest').trigger('click')
    vi.mocked(renameAlbum).mockResolvedValueOnce(done({ action: 'edit', version: 3 }, album({ version: 3, name: '打字' })))
    await test(wrapper, 'album-name-form').trigger('submit')
    await flushPromises()
    // A new request, on the newest version, with a new key.
    const [first, second] = vi.mocked(renameAlbum).mock.calls
    expect(second[1]).toBe(2)
    expect(second[3]).not.toBe(first[3])

    await test(wrapper, 'album-start-rename').trigger('click')
    await test(wrapper, 'album-name-input').setValue('再改')
    vi.mocked(renameAlbum).mockRejectedValueOnce(refused('ALBUM_PROPOSAL_PENDING'))
    vi.mocked(getAlbum).mockResolvedValue({ album: { ...pendingAlbum(), version: 3, name: '打字', pending_proposal: { id: PR, proposer_id: 'partner', base_version: 3, created_at: NOW } } })
    await test(wrapper, 'album-name-form').trigger('submit')
    await flushPromises()
    expect(test(wrapper, 'album-pending').exists()).toBe(true)
    expect(test(wrapper, 'album-name-write').exists()).toBe(false)
    expect(test(wrapper, 'album-notices').text()).toContain('待处理')
  })

  it('a decided request re-reads, and shows no controls for it', async () => {
    const wrapper = await openDetail(pendingAlbum())
    vi.mocked(decideAlbumProposal).mockRejectedValueOnce(refused('ALBUM_PROPOSAL_DECIDED'))
    vi.mocked(getAlbum).mockResolvedValue({ album: album() })
    await test(wrapper, 'album-approve').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-pending').exists()).toBe(false)
    expect(test(wrapper, 'album-notices').text()).toContain('已经处理过')
    expect(test(wrapper, 'album-action-decide').exists()).toBe(false)
  })
})

describe('replay of unconfirmed writes', () => {
  it('an approval replay sends the same key and body once more, shows the original facts, and one notice', async () => {
    const wrapper = await openDetail(pendingAlbum())
    vi.mocked(decideAlbumProposal).mockRejectedValueOnce(server())
    await test(wrapper, 'album-approve').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-action-decide').exists()).toBe(true)
    expect((test(wrapper, 'album-approve').element as HTMLButtonElement).disabled).toBe(true)
    // Today's album is newer than what the replay confirms: accepted.
    vi.mocked(decideAlbumProposal).mockResolvedValueOnce(done({ action: 'decide', proposal_id: PR, outcome: 'approved', base_version: 1, version: 2 }, album({ version: 3, name: '后来' })))
    await test(wrapper, 'album-action-retry').trigger('click')
    await flushPromises()
    const [first, second] = vi.mocked(decideAlbumProposal).mock.calls
    expect(second).toEqual(first)
    expect(test(wrapper, 'album-name').text()).toBe('后来')
    expect(useAlbumsStore().notices).toHaveLength(1)
    expect(test(wrapper, 'album-unconfirmed').exists()).toBe(false)
  })

  it('an older album leaves the action unconfirmed and the page unchanged, and a retry can still succeed', async () => {
    const wrapper = await openDetail(album({ version: 2, name: '现在' }))
    await test(wrapper, 'album-start-rename').trigger('click')
    await test(wrapper, 'album-name-input').setValue('新')
    vi.mocked(renameAlbum).mockResolvedValueOnce(done({ action: 'edit', version: 3 }, album({ version: 2, name: '现在' })))
    await test(wrapper, 'album-name-form').trigger('submit')
    await flushPromises()
    expect(test(wrapper, 'album-action-rename').exists()).toBe(true)
    expect(useAlbumsStore().notices).toHaveLength(0)
    expect(test(wrapper, 'album-name').text()).toBe('现在')
    vi.mocked(renameAlbum).mockResolvedValueOnce(done({ action: 'edit', version: 3 }, album({ version: 3, name: '新' })))
    await test(wrapper, 'album-action-retry').trigger('click')
    await flushPromises()
    const [first, second] = vi.mocked(renameAlbum).mock.calls
    expect(second).toEqual(first)
    expect(test(wrapper, 'album-name').text()).toBe('新')
  })

  it('a retry\'s definite refusal does not prove the first send failed', async () => {
    const store = useAlbumsStore()
    vi.mocked(setAlbumCover).mockRejectedValueOnce(network())
    await store.setCover(A1, P1)
    expect(store.actions[`cover:${A1}`].status).toBe('unknown')
    vi.mocked(setAlbumCover).mockRejectedValueOnce(refused('ALBUM_PHOTO_NOT_FOUND', 404))
    await store.retry(`cover:${A1}`)
    expect(store.actions[`cover:${A1}`].status).toBe('unknown')
    expect(store.actions[`cover:${A1}`].unresolved).toBe(true)
    vi.mocked(setAlbumCover).mockResolvedValueOnce(done({ action: 'set_cover', media_id: P1, event_no: 1 }, album({ cover: cover(P1, true) })))
    await store.retry(`cover:${A1}`)
    expect(store.actions[`cover:${A1}`]).toBeUndefined()
    const keys = vi.mocked(setAlbumCover).mock.calls.map((call) => call[2])
    expect(new Set(keys).size).toBe(1)
  })

  it('offline before any send is not sent; offline on a retry stays unknown', async () => {
    const store = useAlbumsStore()
    vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(new OfflineMutationBlockedError())
    await store.removePhoto(A1, P1)
    expect(store.actions[`remove:${A1}:${P1}`].status).toBe('not_sent')
    vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(server())
    await store.retry(`remove:${A1}:${P1}`)
    expect(store.actions[`remove:${A1}:${P1}`].status).toBe('unknown')
    vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(new OfflineMutationBlockedError())
    await store.retry(`remove:${A1}:${P1}`)
    expect(store.actions[`remove:${A1}:${P1}`].status).toBe('unknown')
  })

  it('only one write per album is in flight, and a double click starts nothing more', async () => {
    const store = useAlbumsStore()
    const held = deferred<AlbumMutationResponse>()
    vi.mocked(setAlbumCover).mockReturnValueOnce(held.promise)
    const first = store.setCover(A1, P1)
    await flushPromises()
    expect(await store.setCover(A1, P2)).toEqual({ outcome: 'busy' })
    expect(await store.removePhoto(A1, P1)).toEqual({ outcome: 'busy' })
    expect(await store.rename(A1, 1, 'x')).toEqual({ outcome: 'busy' })
    // Another album is free.
    vi.mocked(renameAlbum).mockRejectedValueOnce(network())
    await store.rename('other', 1, 'x')
    expect(renameAlbum).toHaveBeenCalledTimes(1)
    held.resolve(done({ action: 'set_cover', media_id: P1, event_no: 1 }))
    await first
    expect(setAlbumCover).toHaveBeenCalledTimes(1)
  })

  it('a retry also waits while another write of that album is in flight', async () => {
    const store = useAlbumsStore()
    vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(network())
    await store.removePhoto(A1, P1)
    const held = deferred<AlbumMutationResponse>()
    vi.mocked(setAlbumCover).mockReturnValueOnce(held.promise)
    const covering = store.setCover(A1, P2)
    await flushPromises()
    expect(await store.retry(`remove:${A1}:${P1}`)).toEqual({ outcome: 'busy' })
    expect(removeAlbumPhoto).toHaveBeenCalledTimes(1)
    expect(store.actions[`remove:${A1}:${P1}`].status).toBe('unknown')
    held.resolve(done({ action: 'set_cover', media_id: P2, event_no: 1 }))
    await covering
    vi.mocked(removeAlbumPhoto).mockResolvedValueOnce(done({ action: 'remove_photo', media_id: P1 }))
    expect((await store.retry(`remove:${A1}:${P1}`)).outcome).toBe('done')
  })
})

describe('adding photos', () => {
  function primeUpload(ids: string[]): void {
    for (const id of ids) vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(id))
    vi.mocked(postToStaging).mockResolvedValue()
    vi.mocked(completeMediaUpload).mockImplementation((id: string) => Promise.resolve(readyResponse(id)))
  }

  it('keeps upload completion separate: READY uploads wait for an explicit add, in the chosen order, then leave the queue', async () => {
    const wrapper = await openDetail(album())
    primeUpload([P1, P2])
    await pickFiles(wrapper, [file('a.png'), file('b.png')])
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(2)
    expect(addAlbumPhotos).not.toHaveBeenCalled()
    expect(test(wrapper, 'album-add-submit').text()).toContain('2 张')
    expect(wrapper.text()).toContain('还没有加入相册')

    vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([photo(P1, 'self', 2), photo(P2, 'self', 1)]))
    vi.mocked(addAlbumPhotos).mockResolvedValue(done({ action: 'add_photos', batch_id: 'b', media_ids: [P1, P2] }, album({ photo_count: 2, cover: cover(P1) })))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(vi.mocked(addAlbumPhotos).mock.calls[0].slice(0, 2)).toEqual([A1, [P1, P2]])
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(0)
    expect(wrapper.findAll('[data-test="album-photo"]').map((p) => p.attributes('data-media'))).toEqual([P1, P2])
    expect(test(wrapper, 'album-cover-source').text()).toContain('自动封面')
  })

  it('sends only READY uploads; an unfinished one stays behind', async () => {
    const wrapper = await openDetail(album())
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1)).mockResolvedValueOnce(grantResponse(P2))
    const held = deferred<void>()
    vi.mocked(postToStaging).mockResolvedValueOnce().mockReturnValueOnce(held.promise)
    vi.mocked(completeMediaUpload).mockImplementation((id: string) => Promise.resolve(readyResponse(id)))
    await pickFiles(wrapper, [file('a.png'), file('b.png')])
    vi.mocked(addAlbumPhotos).mockResolvedValue(done({ action: 'add_photos', batch_id: 'b', media_ids: [P1] }, album({ photo_count: 1 })))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(vi.mocked(addAlbumPhotos).mock.calls[0][1]).toEqual([P1])
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(1)
    held.resolve()
    await flushPromises()
  })

  it('has nothing to add until something is READY, and caps a pick at twenty', async () => {
    const wrapper = await openDetail(album())
    vi.mocked(createMediaUpload).mockRejectedValue(network())
    await pickFiles(wrapper, Array.from({ length: 21 }, (_, i) => file(`${i}.png`)))
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(20)
    expect(test(wrapper, 'album-picker-error').exists()).toBe(true)
    expect((test(wrapper, 'album-add-submit').element as HTMLButtonElement).disabled).toBe(true)
  })

  it('an atomic refusal adds nothing and keeps every upload; removing one and adding again is a new request', async () => {
    const wrapper = await openDetail(album())
    primeUpload([P1, P2])
    await pickFiles(wrapper, [file('a.png'), file('b.png')])
    vi.mocked(addAlbumPhotos).mockRejectedValueOnce(refused('ALBUM_PHOTO_UNAVAILABLE'))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="album-upload"]')).toHaveLength(2)
    expect(wrapper.findAll('[data-test="album-photo"]')).toHaveLength(0)
    expect(test(wrapper, 'album-notices').text()).toContain('没有加入任何照片')
    expect(useAlbumsStore().actions[`add:${A1}`]).toBeUndefined()

    await wrapper.findAll('[data-test="album-upload-remove"]')[0].trigger('click')
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(done({ action: 'add_photos', batch_id: 'b', media_ids: [P2] }, album({ photo_count: 1 })))
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    const [first, second] = vi.mocked(addAlbumPhotos).mock.calls
    expect(second[1]).toEqual([P2])
    expect(second[2]).not.toBe(first[2])
  })

  it('an unconfirmed add stays locked to its ids, its uploads stay, and it survives leaving and returning', async () => {
    const wrapper = await openDetail(album())
    primeUpload([P1, P2])
    await pickFiles(wrapper, [file('a.png'), file('b.png')])
    vi.mocked(addAlbumPhotos).mockRejectedValueOnce(server())
    await test(wrapper, 'album-add-submit').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-action-add').text()).toContain('2 张')
    expect((test(wrapper, 'album-add-submit').element as HTMLButtonElement).disabled).toBe(true)
    expect(wrapper.findAll('[data-test="album-upload-remove"]').every((b) => (b.element as HTMLButtonElement).disabled)).toBe(true)
    const first = vi.mocked(addAlbumPhotos).mock.calls[0]
    wrapper.unmount()
    mounted.pop()
    // Uploads are forgotten on leaving; the captured add is not.
    expect(useMemoryUploadsStore().items).toHaveLength(0)

    const back = await openDetail(album({ photo_count: 0 }))
    expect(test(back, 'album-action-add').exists()).toBe(true)
    vi.mocked(addAlbumPhotos).mockResolvedValueOnce(done({ action: 'add_photos', batch_id: 'b', media_ids: [P1, P2] }, album({ photo_count: 1 })))
    await test(back, 'album-action-retry').trigger('click')
    await flushPromises()
    expect(vi.mocked(addAlbumPhotos).mock.calls[1]).toEqual(first)
    expect(test(back, 'album-unconfirmed').exists()).toBe(false)
  })

  it('offers each upload failure its own retry, on the same media id', async () => {
    const wrapper = await openDetail(album())
    vi.mocked(createMediaUpload).mockResolvedValueOnce(grantResponse(P1))
    vi.mocked(postToStaging).mockResolvedValueOnce()
    vi.mocked(completeMediaUpload).mockRejectedValueOnce(network())
    await pickFiles(wrapper, [file()])
    expect(test(wrapper, 'album-upload-status').attributes('data-status')).toBe('unconfirmed')
    expect((test(wrapper, 'album-add-submit').element as HTMLButtonElement).disabled).toBe(true)
    vi.mocked(completeMediaUpload).mockResolvedValueOnce(readyResponse(P1))
    await test(wrapper, 'album-upload-retry').trigger('click')
    await flushPromises()
    expect(createMediaUpload).toHaveBeenCalledTimes(1)
    expect(postToStaging).toHaveBeenCalledTimes(1)
    expect(vi.mocked(completeMediaUpload).mock.calls.map((call) => call[0])).toEqual([P1, P1])
    expect((test(wrapper, 'album-add-submit').element as HTMLButtonElement).disabled).toBe(false)
  })
})

describe('disabled uploads', () => {
  it('capability false, malformed, or unreadable: albums stay usable and no picker is offered, without reading the album list', async () => {
    vi.mocked(getAlbum).mockResolvedValue({ album: album({ photo_count: 1 }) })
    vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([photo(P1)]))
    vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: false })
    const off = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(listAlbums).not.toHaveBeenCalled()
    expect(getMediaCapabilities).toHaveBeenCalledTimes(1)
    expect(test(off, 'album-uploads-off').exists()).toBe(true)
    expect(test(off, 'album-files').exists()).toBe(false)
    expect(test(off, 'album-set-cover').exists()).toBe(true)
    expect(test(off, 'album-remove').exists()).toBe(true)
    expect(test(off, 'album-start-rename').exists()).toBe(true)
    off.unmount()
    mounted.pop()

    signIn('third')
    signIn('self')
    vi.mocked(getMediaCapabilities).mockResolvedValue({} as never)
    const missing = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(test(missing, 'album-uploads-unknown').exists()).toBe(true)
    expect(test(missing, 'album-files').exists()).toBe(false)
    missing.unmount()
    mounted.pop()

    signIn('third')
    signIn('self')
    vi.mocked(getMediaCapabilities).mockRejectedValue(network())
    const unreadable = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(test(unreadable, 'album-uploads-unknown').exists()).toBe(true)
    expect(test(unreadable, 'album-name').text()).toBe('旅行')
    vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
    await test(unreadable, 'album-uploads-recheck').trigger('click')
    await flushPromises()
    expect(test(unreadable, 'album-files').exists()).toBe(true)
  })

  it('MEDIA_DISABLED mid-flow tells both this page and the memories page', async () => {
    const wrapper = await openDetail(album())
    vi.mocked(createMediaUpload).mockRejectedValueOnce(new ApiError('MEDIA_DISABLED', 'x', 503))
    await pickFiles(wrapper, [file()])
    expect(useAlbumsStore().uploadsEnabled).toBe(false)
    expect(useMemoriesStore().uploadsEnabled).toBe(false)
    expect(test(wrapper, 'album-uploads-off').exists()).toBe(true)
    expect(test(wrapper, 'album-upload-status').attributes('data-status')).toBe('disabled')
  })
})

describe('removal and cover', () => {
  it('shows remove only on the caller\'s own photos, explains it first, and cancelling sends nothing', async () => {
    const wrapper = await openDetail(album({ photo_count: 2 }), [photo(P1, 'self', 2), photo(P2, 'partner', 1)])
    expect(photoCard(wrapper, P1).find('[data-test="album-remove"]').exists()).toBe(true)
    expect(photoCard(wrapper, P2).find('[data-test="album-remove"]').exists()).toBe(false)
    expect(test(wrapper, 'album-remove-explain').exists()).toBe(false)
    await photoCard(wrapper, P1).find('[data-test="album-remove"]').trigger('click')
    const text = test(wrapper, 'album-remove-explain').text()
    expect(text).toContain('隐藏')
    expect(text).toContain('文件仍然保留')
    expect(text).toContain('已经打开过的链接')
    await test(wrapper, 'album-remove-cancel').trigger('click')
    expect(removeAlbumPhoto).not.toHaveBeenCalled()
  })

  it('removal drops the photo and takes the fallback cover from the answer, with no separate read', async () => {
    const wrapper = await openDetail(album({ photo_count: 2, cover: cover(P1, true) }), [photo(P1, 'self', 2), photo(P2, 'self', 1)])
    expect(test(wrapper, 'album-cover-source').text()).toBe('选定的封面')
    const reads = vi.mocked(listAlbumPhotos).mock.calls.length
    vi.mocked(removeAlbumPhoto).mockResolvedValue(done({ action: 'remove_photo', media_id: P1 }, album({ photo_count: 1, cover: cover(P2) })))
    await photoCard(wrapper, P1).find('[data-test="album-remove"]').trigger('click')
    await test(wrapper, 'album-remove-confirm').trigger('click')
    await flushPromises()
    expect(photoCard(wrapper, P1).exists()).toBe(false)
    expect(test(wrapper, 'album-cover-source').text()).toContain('自动封面')
    expect(photoCard(wrapper, P2).find('[data-test="album-photo-cover"]').exists()).toBe(true)
    expect(test(wrapper, 'album-notices').text()).toContain('文件仍然保留')
    expect(vi.mocked(listAlbumPhotos).mock.calls.length).toBe(reads)
  })

  it('removing the last photo leaves no cover', async () => {
    const wrapper = await openDetail(album({ photo_count: 1, cover: cover(P1) }), [photo(P1)])
    vi.mocked(removeAlbumPhoto).mockResolvedValue(done({ action: 'remove_photo', media_id: P1 }, album()))
    await test(wrapper, 'album-remove').trigger('click')
    await test(wrapper, 'album-remove-confirm').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-no-cover').exists()).toBe(true)
    expect(test(wrapper, 'album-photos-empty').exists()).toBe(true)
  })

  it('an unconfirmed removal keeps the photo showing until confirmed, and its replay is the same request', async () => {
    const wrapper = await openDetail(album({ photo_count: 1 }), [photo(P1)])
    vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(network())
    await test(wrapper, 'album-remove').trigger('click')
    await test(wrapper, 'album-remove-confirm').trigger('click')
    await flushPromises()
    expect(photoCard(wrapper, P1).exists()).toBe(true)
    expect(test(wrapper, 'album-action-remove').exists()).toBe(true)
    vi.mocked(removeAlbumPhoto).mockResolvedValueOnce(done({ action: 'remove_photo', media_id: P1 }, album()))
    await test(wrapper, 'album-action-retry').trigger('click')
    await flushPromises()
    const [first, second] = vi.mocked(removeAlbumPhoto).mock.calls
    expect(second).toEqual(first)
    expect(photoCard(wrapper, P1).exists()).toBe(false)
  })

  it.each([['ALBUM_PHOTO_REMOVED', 409, '已经移除'], ['ALBUM_PHOTO_NOT_FOUND', 404, '没有这张照片'], ['ALBUM_PHOTO_FORBIDDEN', 403, '上传者']])(
    'a definite %s reports it and re-reads the photos', async (code, status, text) => {
      const wrapper = await openDetail(album({ photo_count: 1 }), [photo(P1)])
      vi.mocked(removeAlbumPhoto).mockRejectedValueOnce(refused(code, status))
      vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([]))
      await test(wrapper, 'album-remove').trigger('click')
      await test(wrapper, 'album-remove-confirm').trigger('click')
      await flushPromises()
      expect(test(wrapper, 'album-notices').text()).toContain(text)
      expect(photoCard(wrapper, P1).exists()).toBe(false)
    })

  it('either member chooses from active photos; a fallback can be chosen, a chosen cover need not be', async () => {
    const wrapper = await openDetail(album({ creator_id: 'partner', photo_count: 2, cover: cover(P1) }), [photo(P1, 'partner', 2), photo(P2, 'partner', 1)])
    expect(photoCard(wrapper, P1).find('[data-test="album-set-cover"]').exists()).toBe(true)
    vi.mocked(setAlbumCover).mockResolvedValue(done({ action: 'set_cover', media_id: P2, event_no: 1 }, album({ creator_id: 'partner', photo_count: 2, cover: cover(P2, true) })))
    await photoCard(wrapper, P2).find('[data-test="album-set-cover"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(setAlbumCover).mock.calls[0].slice(0, 2)).toEqual([A1, P2])
    expect(test(wrapper, 'album-cover-source').text()).toBe('选定的封面')
    expect(photoCard(wrapper, P2).find('[data-test="album-set-cover"]').exists()).toBe(false)
    expect(photoCard(wrapper, P1).find('[data-test="album-set-cover"]').exists()).toBe(true)
  })

  it('a pending request holds no photo or cover write, and neither moves the name', async () => {
    const wrapper = await openDetail({ ...pendingAlbum(), photo_count: 1 }, [photo(P1)])
    expect(test(wrapper, 'album-set-cover').exists()).toBe(true)
    expect(test(wrapper, 'album-remove').exists()).toBe(true)
    vi.mocked(setAlbumCover).mockResolvedValue(done({ action: 'set_cover', media_id: P1, event_no: 1 }, { ...pendingAlbum(), photo_count: 1, cover: cover(P1, true) }))
    await test(wrapper, 'album-set-cover').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-pending').exists()).toBe(true)
    expect(test(wrapper, 'album-name').text()).toBe('旅行')
  })

  it('a removed cover choice re-reads and reports', async () => {
    const wrapper = await openDetail(album({ photo_count: 1 }), [photo(P1)])
    vi.mocked(setAlbumCover).mockRejectedValueOnce(refused('ALBUM_PHOTO_NOT_FOUND', 404))
    vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([]))
    await test(wrapper, 'album-set-cover').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-notices').text()).toContain('没有这张照片')
    expect(photoCard(wrapper, P1).exists()).toBe(false)
  })

  it('pages photos by the cursor unchanged, and a failed later page keeps the first', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbumPhotos).mockResolvedValueOnce(photoPage([photo(P1, 'self', 3)], '3'))
    await store.loadPhotos(A1)
    vi.mocked(listAlbumPhotos).mockRejectedValueOnce(network())
    await store.loadMorePhotos()
    expect(store.photos.items).toHaveLength(1)
    expect(store.photos.moreErrorText).not.toBe('')
    expect(store.photos.nextCursor).toBe('3')
    vi.mocked(listAlbumPhotos).mockResolvedValueOnce(photoPage([photo(P1, 'self', 3), photo(P2, 'self', 2)]))
    await store.loadMorePhotos()
    expect(listAlbumPhotos).toHaveBeenLastCalledWith(A1, '3')
    expect(store.photos.items.map((p) => p.media_id)).toEqual([P1, P2])
    expect(store.photos.nextCursor).toBeNull()
  })

  it('shows a read failure for photos without losing the album, and retries', async () => {
    vi.mocked(getAlbum).mockResolvedValue({ album: album() })
    vi.mocked(listAlbumPhotos).mockResolvedValueOnce({ photos: [{ media_id: P1 }], next_cursor: null } as never)
    const wrapper = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(test(wrapper, 'album-photos-error').exists()).toBe(true)
    expect(test(wrapper, 'album-name').exists()).toBe(true)
    vi.mocked(listAlbumPhotos).mockResolvedValueOnce(photoPage([photo(P1)]))
    await test(wrapper, 'album-photos-reload').trigger('click')
    await flushPromises()
    expect(photoCard(wrapper, P1).exists()).toBe(true)
  })

  it('answers a foreign or absent album plainly', async () => {
    vi.mocked(getAlbum).mockRejectedValue(refused('ALBUM_NOT_FOUND', 404))
    vi.mocked(listAlbumPhotos).mockRejectedValue(refused('ALBUM_NOT_FOUND', 404))
    const wrapper = show(AlbumDetailView, { id: A1 })
    await flushPromises()
    expect(test(wrapper, 'album-detail-error').text()).toBe('相册不存在。')
    expect(test(wrapper, 'album-adder').exists()).toBe(false)
  })
})

describe('late responses', () => {
  it('drops a write answer from before an account change, and the new account keeps nothing of the old', async () => {
    const store = useAlbumsStore()
    await store.load()
    const held = deferred<AlbumMutationResponse>()
    vi.mocked(createAlbum).mockReturnValueOnce(held.promise)
    const pending = store.create('旧账号')
    await flushPromises()
    signIn('other')
    expect(store.actions.new).toBeUndefined()
    held.resolve(done({ action: 'create', version: 1, actor_id: 'self' }, album({ name: '旧账号' })))
    expect(await pending).toEqual({ outcome: 'discarded' })
    expect(store.items).toEqual([])
    expect(store.notices).toEqual([])
    expect(store.actions).toEqual({})
  })

  it('drops list, detail, photos and review reads from before an account change', async () => {
    const store = useAlbumsStore()
    const list = deferred<never>()
    const one = deferred<never>()
    const photos = deferred<never>()
    const proposal = deferred<never>()
    vi.mocked(listAlbums).mockReturnValueOnce(list.promise)
    vi.mocked(getAlbum).mockReturnValueOnce(one.promise)
    vi.mocked(listAlbumPhotos).mockReturnValueOnce(photos.promise)
    vi.mocked(getAlbumProposal).mockReturnValueOnce(proposal.promise)
    void store.load()
    void store.openDetail(A1)
    void store.loadReview(A1, PR)
    signIn('other')
    list.resolve(page([album()]) as never)
    one.resolve({ album: album() } as never)
    photos.resolve(photoPage([photo(P1)]) as never)
    proposal.resolve({ proposal: proposalRecord() } as never)
    await flushPromises()
    expect(store.items).toEqual([])
    expect(store.detail.item).toBeNull()
    expect(store.photos.items).toEqual([])
    expect(store.review.proposal).toBeNull()
  })

  it('leaving the page drops late reads, and a late upload changes nothing', async () => {
    const wrapper = await openDetail(album())
    const held = deferred<MediaResponse>()
    vi.mocked(createMediaUpload).mockReturnValueOnce(held.promise)
    await pickFiles(wrapper, [file()])
    const store = useAlbumsStore()
    const late = deferred<never>()
    vi.mocked(getAlbum).mockReturnValueOnce(late.promise)
    void store.openDetail(A1)
    wrapper.unmount()
    mounted.pop()
    late.resolve({ album: album({ name: '迟到' }) } as never)
    held.resolve(grantResponse(P1))
    await flushPromises()
    expect(store.detail.item).toBeNull()
    expect(useMemoryUploadsStore().items).toEqual([])
    expect(postToStaging).not.toHaveBeenCalled()
  })

  it('a read already in flight cannot overwrite a confirmed write, and an older version never replaces a newer one', async () => {
    const store = useAlbumsStore()
    vi.mocked(getAlbum).mockResolvedValueOnce({ album: album() })
    vi.mocked(listAlbumPhotos).mockResolvedValue(photoPage([]))
    await store.openDetail(A1)
    const slow = deferred<never>()
    vi.mocked(getAlbum).mockReturnValueOnce(slow.promise)
    const reading = store.openDetail(A1)
    vi.mocked(renameAlbum).mockResolvedValueOnce(done({ action: 'edit', version: 2 }, album({ version: 2, name: '新' })))
    await store.rename(A1, 1, '新')
    slow.resolve({ album: album({ name: '旧读' }) } as never)
    await reading
    expect(store.detail.item?.name).toBe('新')
    // A replay whose today is behind this copy.
    vi.mocked(renameAlbum).mockResolvedValueOnce(done({ action: 'edit', version: 2 }, album({ version: 3, name: '更新' })))
    await store.rename(A1, 1, '更新')
    expect(store.detail.item?.version).toBe(3)
    vi.mocked(setAlbumCover).mockResolvedValueOnce(done({ action: 'set_cover', media_id: P1, event_no: 1 }, album({ version: 2, name: '旧的' })))
    await store.setCover(A1, P1)
    expect(store.detail.item?.name).toBe('更新')
  })

  it('a newer list read wins over an older one, and changing album clears the form', async () => {
    const store = useAlbumsStore()
    const older = deferred<never>()
    vi.mocked(listAlbums).mockReturnValueOnce(older.promise)
    void store.load()
    vi.mocked(listAlbums).mockResolvedValueOnce(page([album({ name: '新读' })]))
    await store.load()
    older.resolve(page([album({ name: '旧读' })]) as never)
    await flushPromises()
    expect(store.items[0].name).toBe('新读')

    const wrapper = await openDetail(album())
    await test(wrapper, 'album-start-rename').trigger('click')
    await test(wrapper, 'album-name-input').setValue('打字')
    vi.mocked(getAlbum).mockResolvedValue({ album: album({ id: 'b' }) })
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    expect(test(wrapper, 'album-name-form').exists()).toBe(false)
  })
})
