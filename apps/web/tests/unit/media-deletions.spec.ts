/**
 * D6 photo deletion and recovery (web): who sees which control, placeholders with no media request, refresh from the server, the recovery
 * view from a fresh session, conflicts, replay with the captured key, and late answers after an account change. All data is fake.
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
vi.mock('@/api/memories', async (original) => ({
  ...(await original<typeof import('@/api/memories')>()),
  listMemories: vi.fn(), getMemory: vi.fn(), getMemoryProposal: vi.fn(), listMemoryProposals: vi.fn(),
}))
vi.mock('@/api/media', async (original) => ({
  ...(await original<typeof import('@/api/media')>()),
  deleteMedia: vi.fn(), undoMediaDeletion: vi.fn(), listUndoableMediaDeletions: vi.fn(), mediaContentUrl: vi.fn((id: string) => `/content/${id}`),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: vi.fn() }),
}))

import { getAlbum, listAlbumPhotos, listAlbums, removeAlbumPhoto } from '@/api/albums'
import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { deleteMedia, listUndoableMediaDeletions, mediaContentUrl, undoMediaDeletion } from '@/api/media'
import { getMemory, getMemoryProposal, listMemories } from '@/api/memories'
import { termsOfContent } from '@/api/memory-rules'
import type { Album, AlbumPhoto, MediaDeletion, MediaDeletionContext, Memory, MemoryPhoto, UndoableMediaDeletion } from '@/api/types'
import PhotoDeleteControl from '@/components/PhotoDeleteControl.vue'
import { isUsableDeletionResponse, useMediaDeletionsStore, type MediaDeletionAction } from '@/stores/media-deletions'
import { useMemoriesStore } from '@/stores/memories'
import { useSessionStore } from '@/stores/session'
import AlbumDetailView from '@/views/AlbumDetailView.vue'
import MediaDeletionsView from '@/views/MediaDeletionsView.vue'
import MemoriesView from '@/views/MemoriesView.vue'
import MemoryDetailView from '@/views/MemoryDetailView.vue'
import UsView from '@/views/UsView.vue'

const NOW = '2026-11-04T01:00:00.000000Z'
const LATER = '2026-11-11T01:00:00.000000Z'
const P1 = '11111111-1111-4111-8111-111111111111'
const P2 = '22222222-2222-4222-8222-222222222222'
const D1 = 'dddddddd-1111-4111-8111-111111111111'
const D2 = 'dddddddd-2222-4222-8222-222222222222'
const M1 = 'memory-1'
const A1 = 'album-1'

function memoryPhoto(id: string, uploader = 'self', deleted = false, position = 1): MemoryPhoto {
  return { position, media_id: id, uploader_id: uploader, width: deleted ? null : 4, height: deleted ? null : 3, deleted }
}
function memory(photos: MemoryPhoto[], overrides: Partial<Memory> = {}): Memory {
  return { id: M1, creator_id: 'self', version: 1, date: '2026-03-01', title: '海边', description: '留下的文字', created_at: NOW, updated_at: NOW, photos, pending_proposal: null, ...overrides }
}
function album(overrides: Partial<Album> = {}): Album {
  return { id: A1, creator_id: 'self', name: '旅行', version: 1, pending_proposal: null, created_at: NOW, photo_count: 2, cover: null, ...overrides }
}
function albumPhoto(id: string, uploader = 'self', seq = 1): AlbumPhoto {
  return { seq, media_id: id, uploader_id: uploader, width: 4, height: 3, added_at: NOW }
}
function cycle(id: string = D1, media: string = P1, overrides: Partial<MediaDeletion> = {}): MediaDeletion {
  return { deletion_id: id, media_id: media, cycle_no: 1, deleted_at: NOW, purge_after: LATER, undone_at: null, state: 'deleted', ...overrides }
}
const UNBOUND: MediaDeletionContext = { kind: 'none', relation: null, target_id: null, label: null, position: null }
function row(id: string = D1, media: string = P1, context: MediaDeletionContext = UNBOUND): UndoableMediaDeletion {
  return { deletion_id: id, media_id: media, deleted_at: NOW, purge_after: LATER, uploaded_at: NOW, context }
}
function listPage(rows: UndoableMediaDeletion[], next: string | null = null) {
  return { deletions: rows, next_cursor: next }
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
const refused = (code: string, status = 409) => new ApiError(code, 'x', status)

const mounted: VueWrapper[] = []
function show<T extends object>(component: T, props: Record<string, unknown> = {}): VueWrapper {
  const wrapper = mount(component as never, { props: props as never, global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } }) as VueWrapper
  mounted.push(wrapper)
  return wrapper
}
function by(wrapper: VueWrapper, name: string) {
  return wrapper.find(`[data-test="${name}"]`)
}
function card(wrapper: VueWrapper, name: string, media: string) {
  return wrapper.find(`[data-test="${name}"][data-media="${media}"]`)
}
function deletionRow(wrapper: VueWrapper, id: string) {
  return wrapper.find(`[data-test="deletion-row"][data-deletion="${id}"]`)
}
async function openMemory(m: Memory): Promise<VueWrapper> {
  vi.mocked(getMemory).mockResolvedValue({ memory: m })
  vi.mocked(listMemories).mockResolvedValue({ memories: [m], next_cursor: null, uploads_enabled: true } as never)
  const wrapper = show(MemoryDetailView, { id: m.id })
  await flushPromises()
  return wrapper
}
async function openAlbum(a: Album, photos: AlbumPhoto[]): Promise<VueWrapper> {
  vi.mocked(getAlbum).mockResolvedValue({ album: a })
  vi.mocked(listAlbumPhotos).mockResolvedValue({ photos, next_cursor: null } as never)
  vi.mocked(listAlbums).mockResolvedValue({ albums: [a], next_cursor: null, uploads_enabled: true } as never)
  const wrapper = show(AlbumDetailView, { id: a.id })
  await flushPromises()
  return wrapper
}
async function openRecovery(rows: UndoableMediaDeletion[], next: string | null = null): Promise<VueWrapper> {
  vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage(rows, next))
  const wrapper = show(MediaDeletionsView)
  await flushPromises()
  return wrapper
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn('self')
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
})

describe('who can delete, and what they are told', () => {
  it('shows the control only on the uploader\'s own memory photos, and explains before anything is sent', async () => {
    const wrapper = await openMemory(memory([memoryPhoto(P1, 'self'), memoryPhoto(P2, 'partner', false, 2)], { creator_id: 'partner' }))
    expect(card(wrapper, 'memory-photo-item', P1).find('[data-test="photo-delete"]').exists()).toBe(true)
    expect(card(wrapper, 'memory-photo-item', P2).find('[data-test="photo-delete"]').exists()).toBe(false)

    await card(wrapper, 'memory-photo-item', P1).find('[data-test="photo-delete"]').trigger('click')
    expect(deleteMedia).not.toHaveBeenCalled()
    const text = by(wrapper, 'photo-delete-explain').text()
    expect(text).toContain('不再提供')
    expect(text).toContain('7 天内')
    expect(text).toContain('撤销')
    expect(text).toContain('链接')
    expect(text).toContain('副本')
    await by(wrapper, 'photo-delete-cancel').trigger('click')
    expect(deleteMedia).not.toHaveBeenCalled()
    expect(by(wrapper, 'photo-delete-explain').exists()).toBe(false)
  })

  it('keeps album removal and photo deletion as separate controls, each with its own request', async () => {
    const wrapper = await openAlbum(album(), [albumPhoto(P1, 'self', 2), albumPhoto(P2, 'partner', 1)])
    const mine = card(wrapper, 'album-photo', P1)
    expect(mine.find('[data-test="album-remove"]').text()).toBe('从相册移除')
    expect(mine.find('[data-test="photo-delete"]').text()).toBe('删除照片')
    const theirs = card(wrapper, 'album-photo', P2)
    expect(theirs.find('[data-test="album-remove"]').exists()).toBe(false)
    expect(theirs.find('[data-test="photo-delete"]').exists()).toBe(false)

    vi.mocked(deleteMedia).mockResolvedValue({ deletion: cycle() })
    await mine.find('[data-test="photo-delete"]').trigger('click')
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    expect(deleteMedia).toHaveBeenCalledTimes(1)
    expect(removeAlbumPhoto).not.toHaveBeenCalled()
  })

  it('offers no delete control while an edit form holds the memory\'s photos', async () => {
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))
    await by(wrapper, 'memory-start-write').trigger('click')
    await flushPromises()
    expect(card(wrapper, 'memory-photo-item', P1).find('[data-test="photo-delete"]').exists()).toBe(false)
  })
})

describe('deleted memory photos', () => {
  it('render placeholders and issue no media request, in the detail and the timeline, with the text kept', async () => {
    const wrapper = await openMemory(memory([memoryPhoto(P1, 'self', true), memoryPhoto(P2, 'partner', true, 2)]))
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(wrapper.findAll('[data-test="memory-photo-deleted"]')).toHaveLength(2)
    expect(mediaContentUrl).not.toHaveBeenCalled()
    expect(by(wrapper, 'memory-title').text()).toBe('海边')
    expect(by(wrapper, 'memory-description').text()).toBe('留下的文字')
    // Only the uploader is pointed at recovery; neither is offered another delete.
    expect(card(wrapper, 'memory-photo-item', P1).find('[data-test="memory-photo-recover"]').exists()).toBe(true)
    expect(card(wrapper, 'memory-photo-item', P2).find('[data-test="memory-photo-recover"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="photo-delete"]').exists()).toBe(false)

    vi.mocked(listMemories).mockResolvedValue({ memories: [memory([memoryPhoto(P1, 'self', true)])], next_cursor: null, uploads_enabled: true } as never)
    const timeline = show(MemoriesView)
    await flushPromises()
    expect(timeline.findAll('img')).toHaveLength(0)
    expect(timeline.findAll('[data-test="memory-photo-deleted"]')).toHaveLength(1)
    expect(mediaContentUrl).not.toHaveBeenCalled()
  })

  it('in a review too, side by side', async () => {
    const pending = memory([memoryPhoto(P1, 'self', true)], { creator_id: 'self', pending_proposal: { id: 'pr', proposer_id: 'partner', base_version: 1, created_at: NOW } })
    const content = { date: '2026-03-01', title: 't', description: '', photos: [memoryPhoto(P1, 'self', true)] }
    vi.mocked(getMemoryProposal).mockResolvedValue({
      proposal: { id: 'pr', memory_id: M1, proposer_id: 'partner', base_version: 1, created_at: NOW, status: 'pending', decided_by: null, decided_at: null, approved_version: null,
        original: { version: 1, ...content }, proposed: content },
    } as never)
    const wrapper = await openMemory(pending)
    await flushPromises()
    expect(by(wrapper, 'memory-review').exists()).toBe(true)
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(mediaContentUrl).not.toHaveBeenCalled()
  })

  it('never enter an edit form, which says so', async () => {
    const m = memory([memoryPhoto(P1, 'self', true), memoryPhoto(P2, 'self', false, 2)])
    expect(termsOfContent(m).media_ids).toEqual([P2])
    const wrapper = await openMemory(m)
    vi.mocked(mediaContentUrl).mockClear()
    await by(wrapper, 'memory-start-write').trigger('click')
    await flushPromises()
    expect(by(wrapper, 'memory-write-deleted-hint').exists()).toBe(true)
    expect(vi.mocked(mediaContentUrl).mock.calls.every(([id]) => id !== P1)).toBe(true)
  })

  it('becomes a placeholder from the server\'s answer after a confirmed deletion, and returns after an undo', async () => {
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))
    expect(wrapper.findAll('img')).toHaveLength(1)
    vi.mocked(deleteMedia).mockResolvedValue({ deletion: cycle() })
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })
    await by(wrapper, 'photo-delete').trigger('click')
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(by(wrapper, 'memory-photo-deleted').exists()).toBe(true)
    expect(by(wrapper, 'photo-deletion-notice').text()).toContain('最近删除的照片')

    vi.mocked(undoMediaDeletion).mockResolvedValue({ deletion: cycle(D1, P1, { state: 'undone', undone_at: LATER }) })
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1)]) })
    await useMediaDeletionsStore().undo(P1, D1)
    await flushPromises()
    expect(wrapper.findAll('img')).toHaveLength(1)
  })

  it('a slower read that started before the deletion cannot bring the photo back', async () => {
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))
    const old = deferred<{ memory: Memory }>()
    vi.mocked(getMemory).mockReturnValueOnce(old.promise)
    void useMemoriesStore().openDetail(M1)
    vi.mocked(deleteMedia).mockResolvedValue({ deletion: cycle() })
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })
    await useMediaDeletionsStore().deletePhoto(P1)
    await flushPromises()
    old.resolve({ memory: memory([memoryPhoto(P1)]) })
    await flushPromises()
    expect(wrapper.findAll('img')).toHaveLength(0)
  })
})

describe('album refresh', () => {
  it('takes items, count and cover from the server after a confirmed deletion and after an undo', async () => {
    const wrapper = await openAlbum(album({ photo_count: 2, cover: { media_id: P1, uploader_id: 'self', width: 4, height: 3, chosen: true } }), [albumPhoto(P1, 'self', 2), albumPhoto(P2, 'self', 1)])
    expect(wrapper.text()).toContain('2 张照片')
    vi.mocked(deleteMedia).mockResolvedValue({ deletion: cycle() })
    vi.mocked(getAlbum).mockResolvedValue({ album: album({ photo_count: 1, cover: { media_id: P2, uploader_id: 'self', width: 4, height: 3, chosen: false } }) })
    vi.mocked(listAlbumPhotos).mockResolvedValue({ photos: [albumPhoto(P2, 'self', 1)], next_cursor: null } as never)
    await card(wrapper, 'album-photo', P1).find('[data-test="photo-delete"]').trigger('click')
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('1 张照片')
    expect(card(wrapper, 'album-photo', P1).exists()).toBe(false)
    expect(by(wrapper, 'album-cover-source').text()).toContain('自动封面')

    vi.mocked(undoMediaDeletion).mockResolvedValue({ deletion: cycle(D1, P1, { state: 'undone', undone_at: LATER }) })
    vi.mocked(getAlbum).mockResolvedValue({ album: album({ photo_count: 2, cover: { media_id: P1, uploader_id: 'self', width: 4, height: 3, chosen: true } }) })
    vi.mocked(listAlbumPhotos).mockResolvedValue({ photos: [albumPhoto(P1, 'self', 2), albumPhoto(P2, 'self', 1)], next_cursor: null } as never)
    await useMediaDeletionsStore().undo(P1, D1)
    await flushPromises()
    expect(wrapper.text()).toContain('2 张照片')
    expect(by(wrapper, 'album-cover-source').text()).toContain('选定的封面')
  })

  it('says so when the album could not be re-read', async () => {
    const wrapper = await openAlbum(album(), [albumPhoto(P1)])
    vi.mocked(deleteMedia).mockResolvedValue({ deletion: cycle() })
    vi.mocked(getAlbum).mockRejectedValue(network())
    await useMediaDeletionsStore().deletePhoto(P1)
    await flushPromises()
    expect(by(wrapper, 'album-notices').text()).toContain('没有刷新成功')
  })
})

describe('recovery view', () => {
  it('can be reached from the Us page', () => {
    const wrapper = show(UsView)
    expect(by(wrapper, 'media-deletions-link').text()).toContain('最近删除的照片')
  })

  it('after a fresh session, finds the cycle from the server and undoes that exact one, showing times and no image', async () => {
    // Another identity and back: nothing local survives, so only the server can name the cycle.
    signIn('someone-else')
    signIn('self')
    expect(useMediaDeletionsStore().items).toEqual([])
    const wrapper = await openRecovery([row(D2, P2), row(D1, P1)])
    expect(deletionRow(wrapper, D1).find('[data-test="deletion-deleted-at"]').text()).toContain('删除于')
    expect(deletionRow(wrapper, D1).find('[data-test="deletion-deadline"]').text()).toContain('可撤销至')
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(mediaContentUrl).not.toHaveBeenCalled()

    vi.mocked(undoMediaDeletion).mockResolvedValue({ deletion: cycle(D1, P1, { state: 'undone', undone_at: LATER }) })
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([row(D2, P2)]))
    await deletionRow(wrapper, D1).find('[data-test="deletion-undo"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(undoMediaDeletion).mock.calls[0].slice(0, 2)).toEqual([P1, D1])
    expect(deletionRow(wrapper, D1).exists()).toBe(false)
    expect(deletionRow(wrapper, D2).exists()).toBe(true)
    expect(by(wrapper, 'photo-deletion-notice').text()).toContain('已撤销')
  })

  it('names each kind of parent as text, with its upload time, and never renders or requests an image', async () => {
    const context = (over: Partial<MediaDeletionContext>): MediaDeletionContext => ({ ...UNBOUND, ...over })
    const ids = ['a', 'b', 'c', 'd', 'e', 'f'].map((letter) => `dddddddd-${letter.repeat(4)}-4111-8111-111111111111`)
    const wrapper = await openRecovery([
      row(ids[0], P1, context({ kind: 'memory', relation: 'current', target_id: M1, label: '海边', position: 2 })),
      row(ids[1], P1, context({ kind: 'memory', relation: 'former', target_id: M1, label: '海边' })),
      row(ids[2], P1, context({ kind: 'album', relation: 'current', target_id: A1, label: '旅行' })),
      row(ids[3], P1, context({ kind: 'album', relation: 'former', target_id: A1, label: '旅行' })),
      row(ids[4], P1, context({ kind: 'unavailable' })),
      row(ids[5], P1),
    ])
    const shown = ids.map((id) => deletionRow(wrapper, id).find('[data-test="deletion-context"]').text())
    expect(shown).toEqual(['回忆《海边》第 2 张', '回忆《海边》的早期版本', '相册《旅行》', '曾在相册《旅行》', '所属内容已不可用', '未加入回忆或相册'])
    expect(deletionRow(wrapper, ids[5]).find('[data-test="deletion-uploaded-at"]').text()).toContain('上传于')
    // Only a named parent is a link; the generic rows offer nothing to follow.
    expect(deletionRow(wrapper, ids[0]).find('a[data-test="deletion-context"]').exists()).toBe(true)
    expect(deletionRow(wrapper, ids[4]).find('a[data-test="deletion-context"]').exists()).toBe(false)
    expect(deletionRow(wrapper, ids[5]).find('a[data-test="deletion-context"]').exists()).toBe(false)
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(mediaContentUrl).not.toHaveBeenCalled()
  })

  it('shows a title as text, not markup', async () => {
    const wrapper = await openRecovery([row(D1, P1, { kind: 'album', relation: 'current', target_id: A1, label: '<img src=x>', position: null })])
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(by(wrapper, 'deletion-context').text()).toContain('<img src=x>')
  })

  it('after a fresh session, the context comes from the server again, and a row that leaks a parent through a generic kind is refused', async () => {
    signIn('someone-else')
    signIn('self')
    const wrapper = await openRecovery([row(D1, P1, { kind: 'memory', relation: 'current', target_id: M1, label: '海边', position: 1 })])
    expect(by(wrapper, 'deletion-context').text()).toBe('回忆《海边》第 1 张')
    expect(mediaContentUrl).not.toHaveBeenCalled()

    const malformed = [
      { kind: 'unavailable', relation: null, target_id: M1, label: null, position: null },
      { kind: 'unavailable', relation: null, target_id: null, label: '海边', position: null },
      { kind: 'none', relation: 'current', target_id: null, label: null, position: null },
      { kind: 'memory', relation: 'current', target_id: null, label: '海边', position: null },
      { kind: 'request', relation: null, target_id: null, label: null, position: null },
    ]
    for (const context of malformed) {
      vi.mocked(listUndoableMediaDeletions).mockResolvedValueOnce(listPage([row(D1, P1, context as never)]))
      await useMediaDeletionsStore().load()
      expect(useMediaDeletionsStore().status).toBe('error')
      expect(wrapper.find('[data-test="deletion-row"]').exists()).toBe(false)
    }
  })

  it('shows the partner only what the server returns for them, with nothing of the uploader\'s', async () => {
    const uploader = await openRecovery([row()])
    expect(uploader.findAll('[data-test="deletion-row"]')).toHaveLength(1)
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([]))
    signIn('partner')
    await flushPromises()
    expect(uploader.find('[data-test="deletion-row"]').exists()).toBe(false)
    expect(by(uploader, 'deletions-empty').exists()).toBe(true)
  })

  it('pages with the server\'s cursor and drops repeats', async () => {
    const wrapper = await openRecovery([row(D2, P2)], 'cursor-1')
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([row(D2, P2), row(D1, P1)]))
    await by(wrapper, 'deletions-more').trigger('click')
    await flushPromises()
    expect(vi.mocked(listUndoableMediaDeletions).mock.calls.at(-1)?.[0]).toBe('cursor-1')
    expect(wrapper.findAll('[data-test="deletion-row"]')).toHaveLength(2)
    expect(by(wrapper, 'deletions-more').exists()).toBe(false)
  })

  it.each([
    ['MEDIA_UNDO_CLOSED', '不能再撤销'],
    ['MEDIA_DELETION_NOT_ACTIVE', '已经撤销过了'],
  ])('%s refreshes the list and explains the outcome', async (code, text) => {
    const wrapper = await openRecovery([row()])
    vi.mocked(undoMediaDeletion).mockRejectedValue(refused(code))
    vi.mocked(listUndoableMediaDeletions).mockClear()
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([]))
    await by(wrapper, 'deletion-undo').trigger('click')
    await flushPromises()
    expect(listUndoableMediaDeletions).toHaveBeenCalledTimes(1)
    expect(by(wrapper, 'deletions-empty').exists()).toBe(true)
    expect(by(wrapper, 'photo-deletion-notice').text()).toContain(text)
    expect(wrapper.find('[data-test="deletions-unconfirmed"]').exists()).toBe(false)
  })

  it('a conflict on a retry settles it: the server replays a known key before looking at state', async () => {
    const wrapper = await openRecovery([row()])
    vi.mocked(undoMediaDeletion).mockRejectedValueOnce(network())
    await by(wrapper, 'deletion-undo').trigger('click')
    await flushPromises()
    expect(by(wrapper, 'deletions-action-undo').exists()).toBe(true)
    vi.mocked(undoMediaDeletion).mockRejectedValueOnce(refused('MEDIA_UNDO_CLOSED'))
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([]))
    await by(wrapper, 'deletions-action-retry').trigger('click')
    await flushPromises()
    expect(by(wrapper, 'deletions-action-undo').exists()).toBe(false)
    expect(by(wrapper, 'photo-deletion-notice').text()).toContain('不能再撤销')
  })

  it('a failed read shows the error and a retry, not stale rows', async () => {
    vi.mocked(listUndoableMediaDeletions).mockRejectedValue(network())
    const wrapper = show(MediaDeletionsView)
    await flushPromises()
    expect(by(wrapper, 'deletions-error').exists()).toBe(true)
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([row()]))
    await by(wrapper, 'deletions-reload').trigger('click')
    await flushPromises()
    expect(wrapper.findAll('[data-test="deletion-row"]')).toHaveLength(1)
  })
})

describe('unconfirmed writes', () => {
  it('keep the captured target and original key, and resend only on a click', async () => {
    const wrapper = show(PhotoDeleteControl, { mediaId: P1 })
    vi.mocked(deleteMedia).mockRejectedValueOnce(network())
    await by(wrapper, 'photo-delete').trigger('click')
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    const key = vi.mocked(deleteMedia).mock.calls[0][1]
    expect(key).not.toBe('')
    expect(by(wrapper, 'photo-delete-pending').text()).toContain('未确认')
    await flushPromises()
    expect(deleteMedia).toHaveBeenCalledTimes(1)

    vi.mocked(deleteMedia).mockResolvedValueOnce({ deletion: cycle() })
    await by(wrapper, 'photo-delete-retry').trigger('click')
    await flushPromises()
    expect(vi.mocked(deleteMedia).mock.calls[1]).toEqual([P1, key])
    expect(useMediaDeletionsStore().actions[`delete:${P1}`]).toBeUndefined()
  })

  it('an undo keeps its exact cycle and key across a 5xx and a malformed answer', async () => {
    const store = useMediaDeletionsStore()
    vi.mocked(undoMediaDeletion).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'x', 503))
    expect((await store.undo(P1, D1)).outcome).toBe('unknown')
    const key = vi.mocked(undoMediaDeletion).mock.calls[0][2]
    vi.mocked(undoMediaDeletion).mockResolvedValueOnce({ deletion: cycle(D2, P1, { state: 'undone' }) })
    expect((await store.retry(`undo:${D1}`)).outcome).toBe('unknown')
    expect(vi.mocked(undoMediaDeletion).mock.calls[1]).toEqual([P1, D1, key])
    vi.mocked(undoMediaDeletion).mockResolvedValueOnce({ deletion: cycle(D1, P1, { state: 'undone' }) })
    expect((await store.retry(`undo:${D1}`)).outcome).toBe('done')
    expect(vi.mocked(undoMediaDeletion).mock.calls[2]).toEqual([P1, D1, key])
  })

  it('delete, undo with a lost response, re-delete on another device, then replay the old undo: the notice speaks of the old cycle only', async () => {
    const store = useMediaDeletionsStore()
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))

    // Cycle 1, confirmed here.
    vi.mocked(deleteMedia).mockResolvedValueOnce({ deletion: cycle(D1, P1) })
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })
    expect((await store.deletePhoto(P1)).outcome).toBe('done')
    await flushPromises()

    // Its undo commits on the server, and the answer is lost.
    vi.mocked(undoMediaDeletion).mockRejectedValueOnce(network())
    expect((await store.undo(P1, D1)).outcome).toBe('unknown')
    const key = vi.mocked(undoMediaDeletion).mock.calls[0][2]

    // Another device deletes the photo again: cycle 2 is what the server now holds.
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })

    // The replay truthfully confirms cycle 1 as undone.
    vi.mocked(undoMediaDeletion).mockResolvedValueOnce({ deletion: cycle(D1, P1, { state: 'undone', undone_at: LATER }) })
    expect((await store.retry(`undo:${D1}`)).outcome).toBe('done')
    await flushPromises()
    expect(vi.mocked(undoMediaDeletion).mock.calls[1]).toEqual([P1, D1, key])

    const text = store.notices.find((n) => n.id === `undo:${D1}`)?.text ?? ''
    expect(text).toContain('这次删除已撤销')
    expect(text).toContain('如果之后又删除过')
    expect(text).not.toContain('已恢复')
    expect(text).not.toContain('可用')
    // What the photo is now comes from the re-read, which still shows the placeholder.
    expect(wrapper.findAll('img')).toHaveLength(0)
    expect(by(wrapper, 'memory-photo-deleted').exists()).toBe(true)
    expect(mediaContentUrl).toHaveBeenCalledTimes(1)
    expect(store.actions[`undo:${D1}`]).toBeUndefined()
  })

  it('a replay of an old delete whose cycle was undone does not say the photo is available, and refreshes', async () => {
    const store = useMediaDeletionsStore()
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))
    vi.mocked(deleteMedia).mockRejectedValueOnce(network())
    await store.deletePhoto(P1)
    const key = vi.mocked(deleteMedia).mock.calls[0][1]
    // Meanwhile: undone, then deleted again elsewhere.
    vi.mocked(getMemory).mockClear()
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })
    vi.mocked(deleteMedia).mockResolvedValueOnce({ deletion: cycle(D1, P1, { state: 'undone', undone_at: LATER }) })
    expect((await store.retry(`delete:${P1}`)).outcome).toBe('done')
    await flushPromises()
    expect(vi.mocked(deleteMedia).mock.calls[1]).toEqual([P1, key])
    const text = store.notices[0].text
    expect(text).toContain('后来已撤销')
    expect(text).not.toContain('可用')
    expect(text).not.toContain('已恢复')
    expect(getMemory).toHaveBeenCalledTimes(1)
    expect(by(wrapper, 'memory-photo-deleted').exists()).toBe(true)
  })

  it.each([
    ['purging', '正在清除'],
    ['purged', '已清除'],
  ] as const)('a replayed delete in state %s does not offer an undo', async (state, text) => {
    const store = useMediaDeletionsStore()
    vi.mocked(deleteMedia).mockResolvedValueOnce({ deletion: cycle(D1, P1, { state }) })
    await store.deletePhoto(P1)
    expect(store.notices[0].text).toContain(text)
    expect(store.notices[0].text).toContain('不能再撤销')
  })

  it('uncertain delete, MEDIA_ALREADY_DELETED on retry, undo on another device: the old retry is gone, and a new deletion is confirmed with a fresh key', async () => {
    const store = useMediaDeletionsStore()
    const wrapper = await openMemory(memory([memoryPhoto(P1)]))

    // The first send's answer is lost.
    vi.mocked(deleteMedia).mockRejectedValueOnce(network())
    await by(wrapper, 'photo-delete').trigger('click')
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    expect(by(wrapper, 'photo-delete-retry').exists()).toBe(true)
    const oldKey = vi.mocked(deleteMedia).mock.calls[0][1]

    // The retry meets a cycle: the server has not seen this key, and the photo is deleted now.
    vi.mocked(getMemory).mockClear()
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1, 'self', true)]) })
    vi.mocked(deleteMedia).mockRejectedValueOnce(refused('MEDIA_ALREADY_DELETED'))
    await by(wrapper, 'photo-delete-retry').trigger('click')
    await flushPromises()
    expect(vi.mocked(deleteMedia).mock.calls[1]).toEqual([P1, oldKey])

    // Retired, the views re-read, and the notice states the present without judging the first send.
    expect(store.actions[`delete:${P1}`]).toBeUndefined()
    expect(getMemory).toHaveBeenCalledTimes(1)
    expect(by(wrapper, 'memory-photo-deleted').exists()).toBe(true)
    const text = by(wrapper, 'photo-deletion-notice').text()
    expect(text).toContain('目前是删除状态')
    expect(text).toContain('无法确认')
    for (const claim of ['已生效', '已成功', '删除成功', '删除失败', '没有删除', '未删除']) expect(text).not.toContain(claim)

    // Another device undoes that cycle, and the photo is visible again here.
    vi.mocked(getMemory).mockResolvedValue({ memory: memory([memoryPhoto(P1)]) })
    await useMemoriesStore().openDetail(M1)
    await flushPromises()
    expect(wrapper.findAll('img')).toHaveLength(1)

    // The old retry cannot send anything: no button, and the store has no such action.
    expect(by(wrapper, 'photo-delete-retry').exists()).toBe(false)
    expect(await store.retry(`delete:${P1}`)).toEqual({ outcome: 'discarded' })
    expect(deleteMedia).toHaveBeenCalledTimes(2)

    // A new deletion starts from the confirmation, which alone sends nothing, and uses a new key.
    await by(wrapper, 'photo-delete').trigger('click')
    expect(by(wrapper, 'photo-delete-explain').exists()).toBe(true)
    expect(deleteMedia).toHaveBeenCalledTimes(2)
    vi.mocked(deleteMedia).mockResolvedValueOnce({ deletion: cycle(D2, P1) })
    await by(wrapper, 'photo-delete-confirm').trigger('click')
    await flushPromises()
    expect(deleteMedia).toHaveBeenCalledTimes(3)
    const [media, newKey] = vi.mocked(deleteMedia).mock.calls[2]
    expect(media).toBe(P1)
    expect(newKey).not.toBe(oldKey)
    expect(newKey.length).toBeGreaterThan(0)
  })

  it('a first send refused with MEDIA_ALREADY_DELETED keeps its own message', async () => {
    const store = useMediaDeletionsStore()
    vi.mocked(deleteMedia).mockRejectedValueOnce(refused('MEDIA_ALREADY_DELETED'))
    await store.deletePhoto(P1)
    expect(store.notices[0].text).toContain('已经删除')
    expect(store.notices[0].text).not.toContain('无法确认')
  })

  it.each([
    ['delete', 'MEDIA_NOT_FOUND'],
    ['undo', 'MEDIA_NOT_FOUND'],
    ['undo', 'MEDIA_UNDO_CLOSED'],
    ['undo', 'MEDIA_DELETION_NOT_ACTIVE'],
  ] as const)('on a retry, %s with %s can never commit later, so it ends', async (kind, code) => {
    const store = useMediaDeletionsStore()
    const send = kind === 'delete' ? deleteMedia : undoMediaDeletion
    vi.mocked(send).mockRejectedValueOnce(network())
    if (kind === 'delete') await store.deletePhoto(P1)
    else await store.undo(P1, D1)
    const id = kind === 'delete' ? `delete:${P1}` : `undo:${D1}`
    vi.mocked(send).mockRejectedValueOnce(refused(code))
    expect(await store.retry(id)).toEqual({ outcome: 'rejected', errorCode: code })
    expect(store.actions[id]).toBeUndefined()
  })

  it.each(['SPACE_NOT_FOUND', 'IDEMPOTENCY_KEY_REUSED', 'VALIDATION_ERROR', 'UNAUTHENTICATED', 'ORIGIN_NOT_ALLOWED', 'RATE_LIMITED'])(
    '%s on a retry can come before or instead of the replay, so key and request stay', async (code) => {
      const store = useMediaDeletionsStore()
      vi.mocked(undoMediaDeletion).mockRejectedValueOnce(network())
      await store.undo(P1, D1)
      const key = vi.mocked(undoMediaDeletion).mock.calls[0][2]
      vi.mocked(undoMediaDeletion).mockRejectedValueOnce(refused(code, 403))
      expect((await store.retry(`undo:${D1}`)).outcome).toBe('unknown')
      expect(store.actions[`undo:${D1}`]?.key).toBe(key)
      expect(store.actions[`undo:${D1}`]?.deletionId).toBe(D1)
    })

  it('another 4xx on a retry proves nothing about the first send, so it stays', async () => {
    const store = useMediaDeletionsStore()
    vi.mocked(deleteMedia).mockRejectedValueOnce(network())
    await store.deletePhoto(P1)
    vi.mocked(deleteMedia).mockRejectedValueOnce(refused('CSRF_TOKEN_INVALID', 403))
    expect((await store.retry(`delete:${P1}`)).outcome).toBe('unknown')
    expect(store.actions[`delete:${P1}`]?.unresolved).toBe(true)
  })

  it('a definite refusal of a first send ends it and explains', async () => {
    const store = useMediaDeletionsStore()
    vi.mocked(deleteMedia).mockRejectedValueOnce(refused('MEDIA_ALREADY_DELETED'))
    const result = await store.deletePhoto(P1)
    expect(result).toEqual({ outcome: 'rejected', errorCode: 'MEDIA_ALREADY_DELETED' })
    expect(store.actions[`delete:${P1}`]).toBeUndefined()
    expect(store.notices[0].text).toContain('已经删除')
  })

  it('offline writes are not sent and stay retryable', async () => {
    const store = useMediaDeletionsStore()
    vi.mocked(deleteMedia).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect((await store.deletePhoto(P1)).outcome).toBe('not_sent')
    expect(store.actions[`delete:${P1}`]?.status).toBe('not_sent')
  })

  it('a double click starts one request', async () => {
    const store = useMediaDeletionsStore()
    const pending = deferred<{ deletion: MediaDeletion }>()
    vi.mocked(deleteMedia).mockReturnValueOnce(pending.promise)
    const first = store.deletePhoto(P1)
    expect((await store.deletePhoto(P1)).outcome).toBe('busy')
    await flushPromises()
    pending.resolve({ deletion: cycle() })
    await first
    expect(deleteMedia).toHaveBeenCalledTimes(1)
  })

  it('accepts only a response that confirms the captured action', () => {
    const del: MediaDeletionAction = { id: 'd', kind: 'delete', mediaId: P1, key: 'k', status: 'submitting', errorText: '' }
    const und: MediaDeletionAction = { id: 'u', kind: 'undo', mediaId: P1, deletionId: D1, key: 'k', status: 'submitting', errorText: '' }
    expect(isUsableDeletionResponse({ deletion: cycle() }, del)).toBe(true)
    expect(isUsableDeletionResponse({ deletion: cycle(D1, P1, { state: 'undone' }) }, del)).toBe(true)
    expect(isUsableDeletionResponse({ deletion: cycle(D1, P2) }, del)).toBe(false)
    expect(isUsableDeletionResponse({}, del)).toBe(false)
    expect(isUsableDeletionResponse({ deletion: cycle(D1, P1, { state: 'undone' }) }, und)).toBe(true)
    expect(isUsableDeletionResponse({ deletion: cycle(D1, P1) }, und)).toBe(false)
    expect(isUsableDeletionResponse({ deletion: cycle(D2, P1, { state: 'undone' }) }, und)).toBe(false)
  })
})

describe('identity and late answers', () => {
  it('an account change clears the list, actions and notices, and an old list answer is dropped', async () => {
    const store = useMediaDeletionsStore()
    const wrapper = await openRecovery([row()])
    vi.mocked(undoMediaDeletion).mockRejectedValueOnce(network())
    await by(wrapper, 'deletion-undo').trigger('click')
    await flushPromises()
    expect(Object.keys(store.actions)).toHaveLength(1)
    wrapper.unmount()
    mounted.pop()

    const late = deferred<ReturnType<typeof listPage>>()
    vi.mocked(listUndoableMediaDeletions).mockReturnValueOnce(late.promise)
    void store.load()
    signIn('partner')
    expect(store.items).toEqual([])
    expect(store.actions).toEqual({})
    expect(store.notices).toEqual([])
    expect(store.status).toBe('idle')
    late.resolve(listPage([row()]))
    await flushPromises()
    expect(store.items).toEqual([])
    expect(store.status).toBe('idle')
  })

  it('a write answered after an account change touches nothing', async () => {
    const store = useMediaDeletionsStore()
    const pending = deferred<{ deletion: MediaDeletion }>()
    vi.mocked(deleteMedia).mockReturnValueOnce(pending.promise)
    const result = store.deletePhoto(P1)
    await flushPromises()
    signIn('partner')
    pending.resolve({ deletion: cycle() })
    expect((await result).outcome).toBe('discarded')
    expect(store.notices).toEqual([])
    expect(store.actions).toEqual({})
    expect(getMemory).not.toHaveBeenCalled()
    expect(getAlbum).not.toHaveBeenCalled()
  })

  it('a slower list read cannot overwrite the refresh after an undo, and leaving the page drops it', async () => {
    const store = useMediaDeletionsStore()
    const wrapper = await openRecovery([row()])
    const slow = deferred<ReturnType<typeof listPage>>()
    vi.mocked(listUndoableMediaDeletions).mockReturnValueOnce(slow.promise)
    void store.load()
    vi.mocked(undoMediaDeletion).mockResolvedValueOnce({ deletion: cycle(D1, P1, { state: 'undone' }) })
    vi.mocked(listUndoableMediaDeletions).mockResolvedValue(listPage([]))
    await store.undo(P1, D1)
    await flushPromises()
    slow.resolve(listPage([row()]))
    await flushPromises()
    expect(store.items).toEqual([])

    const gone = deferred<ReturnType<typeof listPage>>()
    vi.mocked(listUndoableMediaDeletions).mockReturnValueOnce(gone.promise)
    void store.load()
    wrapper.unmount()
    mounted.pop()
    gone.resolve(listPage([row()]))
    await flushPromises()
    expect(store.items).toEqual([])
    expect(store.status).toBe('idle')
  })
})
