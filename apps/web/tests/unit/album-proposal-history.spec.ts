/**
 * Album rename-request history (contract 0.38.0, web): the summary rules; the store's own read sequence, paging, account and route
 * changes and late answers; discoverability after signing in again; the detail section, which tells a historical approval apart from
 * today's name, opens the original and proposed names, never decides anything, and is not refreshed by photo or cover changes; and
 * unavailable albums. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/albums', () => ({
  ALBUM_PAGE_SIZE: 20, ALBUM_PHOTO_PAGE_SIZE: 30,
  createAlbum: vi.fn(), listAlbums: vi.fn(), getAlbum: vi.fn(), listAlbumPhotos: vi.fn(), addAlbumPhotos: vi.fn(), removeAlbumPhoto: vi.fn(),
  setAlbumCover: vi.fn(), renameAlbum: vi.fn(), proposeAlbumRename: vi.fn(), getAlbumProposal: vi.fn(), decideAlbumProposal: vi.fn(),
  listAlbumProposals: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: vi.fn() }),
}))

import { isAlbumProposalPage, isAlbumProposalSummary } from '@/api/album-rules'
import { decideAlbumProposal, getAlbum, getAlbumProposal, listAlbumPhotos, listAlbumProposals, listAlbums, setAlbumCover } from '@/api/albums'
import { ApiError, setCsrfToken } from '@/api/client'
import type { Album, AlbumMutation, AlbumMutationResponse, AlbumProposal, AlbumProposalListResponse, AlbumProposalSummary } from '@/api/types'
import { useAlbumsStore } from '@/stores/albums'
import { useSessionStore } from '@/stores/session'
import AlbumDetailView from '@/views/AlbumDetailView.vue'

const NOW = '2026-11-04T01:00:00.000000Z'
const P1 = '11111111-1111-4111-8111-111111111111'

function album(id: string, overrides: Partial<Album> = {}): Album {
  return { id, creator_id: 'partner', name: '旅行', version: 3, pending_proposal: null, created_at: NOW, photo_count: 1, cover: null, ...overrides }
}
function summary(albumId: string, id: string, overrides: Partial<AlbumProposalSummary> = {}): AlbumProposalSummary {
  return {
    id, album_id: albumId, proposer_id: 'self', base_version: 1, created_at: NOW, status: 'rejected',
    decided_by: 'partner', decided_at: '2026-11-04T02:00:00.000000Z', approved_version: null, ...overrides,
  }
}
const approvedOld = (albumId: string) => summary(albumId, 'r-approved', { status: 'approved', approved_version: 2 })
const withdrawn = (albumId: string) => summary(albumId, 'r-withdrawn', { status: 'withdrawn', decided_by: 'self' })
const rejected = (albumId: string) => summary(albumId, 'r-rejected')
const pendingOf = (albumId: string) => summary(albumId, 'r-pending', { status: 'pending', decided_by: null, decided_at: null, base_version: 3 })

function historyPage(proposals: AlbumProposalSummary[], next: string | null = null, current = 3): AlbumProposalListResponse {
  return { proposals, next_cursor: next, current_version: current }
}
function proposalRecord(albumId: string, id: string, overrides: Partial<AlbumProposal> = {}): AlbumProposal {
  return {
    id, album_id: albumId, proposer_id: 'self', base_version: 1, created_at: NOW, status: 'approved', decided_by: 'partner',
    decided_at: '2026-11-04T02:00:00.000000Z', approved_version: 2, original_name: 'HISTORY-ORIGINAL', proposed_name: 'HISTORY-PROPOSED', ...overrides,
  }
}
function done(mutation: Partial<AlbumMutation>, a: Album): AlbumMutationResponse {
  return { mutation: { album_id: a.id, actor_id: 'self', created_at: NOW, action: 'create', ...mutation } as AlbumMutation, album: a }
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

const stubs = { RouterLink: { template: '<a><slot /></a>' } }
const mounted: VueWrapper[] = []
function show(props: Record<string, unknown>): VueWrapper {
  const wrapper = mount(AlbumDetailView as never, { props: props as never, global: { stubs } }) as VueWrapper
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
  vi.mocked(listAlbums).mockResolvedValue({ albums: [], next_cursor: null, uploads_enabled: true } as never)
  vi.mocked(listAlbumPhotos).mockResolvedValue({ photos: [], next_cursor: null } as never)
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
  vi.restoreAllMocks()
})

describe('rules', () => {
  it('accepts each status only with the decision facts it implies', () => {
    expect(isAlbumProposalSummary(pendingOf('a'), 'a')).toBe(true)
    expect(isAlbumProposalSummary(rejected('a'), 'a')).toBe(true)
    expect(isAlbumProposalSummary(withdrawn('a'), 'a')).toBe(true)
    expect(isAlbumProposalSummary(approvedOld('a'), 'a')).toBe(true)
    expect(isAlbumProposalSummary(rejected('b'), 'a')).toBe(false)
    expect(isAlbumProposalSummary({ ...pendingOf('a'), decided_at: NOW }, 'a')).toBe(false)
    expect(isAlbumProposalSummary({ ...approvedOld('a'), approved_version: null }, 'a')).toBe(false)
    expect(isAlbumProposalSummary({ ...approvedOld('a'), approved_version: 1 }, 'a')).toBe(false)
    expect(isAlbumProposalSummary({ ...withdrawn('a'), approved_version: 2 }, 'a')).toBe(false)
    expect(isAlbumProposalSummary({ ...rejected('a'), decided_by: null }, 'a')).toBe(false)
    expect(isAlbumProposalPage(historyPage([]), 'a')).toBe(true)
    expect(isAlbumProposalPage({ ...historyPage([]), current_version: 0 }, 'a')).toBe(false)
    expect(isAlbumProposalPage({ proposals: [], next_cursor: 5, current_version: 1 }, 'a')).toBe(false)
  })
})

describe('store', () => {
  it('reads pages in order, keeps the newest current version, and reports a failed page apart', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([pendingOf('a'), approvedOld('a')], 'c1', 3))
    await store.loadHistory('a')
    expect(listAlbumProposals).toHaveBeenLastCalledWith('a')
    expect(store.history.items.map((p) => p.id)).toEqual(['r-pending', 'r-approved'])
    vi.mocked(listAlbumProposals).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    await store.loadMoreHistory()
    expect(store.history.moreErrorText).not.toBe('')
    expect(store.history.items).toHaveLength(2)
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([approvedOld('a'), withdrawn('a'), rejected('a')], null, 4))
    await store.loadMoreHistory()
    expect(listAlbumProposals).toHaveBeenLastCalledWith('a', 'c1')
    expect(store.history.items.map((p) => p.id)).toEqual(['r-pending', 'r-approved', 'r-withdrawn', 'r-rejected'])
    expect(store.history.nextCursor).toBeNull()
    expect(store.history.currentVersion).toBe(4)
  })

  it('treats a malformed page, one naming another album, or an unavailable album as an error and shows nothing from it', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([rejected('b')]))
    await store.loadHistory('a')
    expect(store.history.status).toBe('error')
    expect(store.history.items).toEqual([])
    vi.mocked(listAlbumProposals).mockRejectedValueOnce(new ApiError('ALBUM_NOT_FOUND', '相册不存在', 404))
    await store.loadHistory('a')
    expect(store.history.status).toBe('error')
    expect(store.history.items).toEqual([])
  })

  it('drops a late answer after the route moves to another album, after leaving, and after an account switch', async () => {
    const store = useAlbumsStore()
    const late = deferred<AlbumProposalListResponse>()
    vi.mocked(listAlbumProposals).mockReturnValueOnce(late.promise).mockResolvedValueOnce(historyPage([rejected('b')]))
    const first = store.loadHistory('a')
    await store.loadHistory('b')
    late.resolve(historyPage([approvedOld('a')]))
    await first
    expect(store.history.albumId).toBe('b')
    expect(store.history.items.map((p) => p.album_id)).toEqual(['b'])

    const afterLeaving = deferred<AlbumProposalListResponse>()
    vi.mocked(listAlbumProposals).mockReturnValueOnce(afterLeaving.promise)
    const leaving = store.loadHistory('a')
    store.closeDetail()
    afterLeaving.resolve(historyPage([approvedOld('a')]))
    await leaving
    expect(store.history.albumId).toBeNull()

    const afterSwitch = deferred<AlbumProposalListResponse>()
    vi.mocked(listAlbumProposals).mockReturnValueOnce(afterSwitch.promise)
    const switching = store.loadHistory('a')
    signIn('other')
    afterSwitch.resolve(historyPage([approvedOld('a')]))
    await switching
    expect(store.history.items).toEqual([])
    expect(store.history.albumId).toBeNull()
  })

  it('drops a late comparison after it was closed, and keeps it apart from the pending review', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbumProposals).mockResolvedValue(historyPage([approvedOld('a')]))
    await store.loadHistory('a')
    const late = deferred<{ proposal: AlbumProposal }>()
    vi.mocked(getAlbumProposal).mockReturnValueOnce(late.promise as never)
    const opening = store.openHistoryReview('a', 'r-approved')
    expect(store.review.proposalId).toBeNull()
    store.closeHistoryReview()
    late.resolve({ proposal: proposalRecord('a', 'r-approved') })
    await opening
    expect(store.historyReview.proposal).toBeNull()
  })

  it('a proposer who signs in again finds their decided request from the server alone', async () => {
    const store = useAlbumsStore()
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([pendingOf('a')]))
    await store.loadHistory('a')
    signIn('other')
    expect(store.history.items).toEqual([])
    setActivePinia(createPinia())
    signIn('self')
    const fresh = useAlbumsStore()
    expect(fresh.history.items).toEqual([])
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([summary('a', 'r-pending', { base_version: 3 })]))
    await fresh.loadHistory('a')
    expect(fresh.history.items[0]).toMatchObject({ id: 'r-pending', status: 'rejected', decided_by: 'partner' })
  })
})

describe('detail section', () => {
  async function detailOf(a: Album, history: AlbumProposalListResponse) {
    vi.mocked(getAlbum).mockResolvedValue({ album: a })
    vi.mocked(listAlbumProposals).mockResolvedValue(history)
    vi.mocked(getAlbumProposal).mockImplementation(async (albumId: string, proposalId: string) =>
      ({ proposal: proposalRecord(albumId, proposalId, { status: 'pending', decided_by: null, decided_at: null, approved_version: null, base_version: 3 }) }) as never)
    const wrapper = show({ id: a.id })
    await flushPromises()
    return wrapper
  }

  it('lists every status, tells an old approval from today\'s name, and leaves deciding to the existing controls', async () => {
    const a = album('a', { pending_proposal: { id: 'r-pending', proposer_id: 'self', base_version: 3, created_at: NOW } })
    const wrapper = await detailOf(a, historyPage([pendingOf('a'), approvedOld('a'), withdrawn('a'), rejected('a')], null, 3))
    const rows = all(wrapper, 'album-history-item')
    expect(rows.map((row) => row.attributes('data-proposal'))).toEqual(['r-pending', 'r-approved', 'r-withdrawn', 'r-rejected'])
    expect(rows.map((row) => row.find('[data-test="album-history-status"]').text())).toEqual(['待处理', '已同意', '已撤回', '已拒绝'])
    expect(test(wrapper, 'album-history-note').text()).toContain('之后的改名可能已经替换了当时同意的名称')
    expect(test(wrapper, 'album-history-note').text()).toContain('第 3 版')
    const version = rows[1].find('[data-test="album-history-version"]').text()
    expect(version).toContain('第 2 版名称')
    expect(version).toContain('现在是第 3 版')
    expect(rows[2].find('[data-test="album-history-decision"]').text()).toContain('我于')
    expect(rows[3].find('[data-test="album-history-decision"]').text()).toContain('对方于')
    expect(rows[0].find('[data-test="album-history-pending"]').exists()).toBe(true)
    expect(rows[0].find('[data-test="album-history-open"]').exists()).toBe(false)
    expect(test(wrapper, 'album-history').findAll('button').map((b) => b.attributes('data-test'))).not.toContain('album-withdraw')
    expect(test(wrapper, 'album-withdraw').exists()).toBe(true)
  })

  it('shows an empty history plainly, and says when an approval is still today\'s name', async () => {
    const empty = await detailOf(album('a'), historyPage([], null, 1))
    expect(test(empty, 'album-history-empty').exists()).toBe(true)
    const current = await detailOf(album('b', { version: 2 }), historyPage([approvedOld('b')], null, 2))
    expect(test(current, 'album-history-version').text()).toContain('也就是现在的名称')
  })

  it('opens a decided request\'s original and proposed names, labelled with the version it was based on', async () => {
    const wrapper = await detailOf(album('a'), historyPage([approvedOld('a')]))
    vi.mocked(getAlbumProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'r-approved') })
    await test(wrapper, 'album-history-open').trigger('click')
    await flushPromises()
    expect(getAlbumProposal).toHaveBeenLastCalledWith('a', 'r-approved')
    expect(test(wrapper, 'album-history-review').text()).toContain('申请时的名称（第 1 版）')
    expect(test(wrapper, 'album-history-review-original').text()).toBe('HISTORY-ORIGINAL')
    expect(test(wrapper, 'album-history-review-proposed').text()).toBe('HISTORY-PROPOSED')
    await test(wrapper, 'album-history-open').trigger('click')
    expect(test(wrapper, 'album-history-review').exists()).toBe(false)
  })

  it('pages with a button and offers a retry on failure', async () => {
    const wrapper = await detailOf(album('a'), historyPage([rejected('a')], 'c1'))
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([withdrawn('a')], null))
    await test(wrapper, 'album-history-more').trigger('click')
    await flushPromises()
    expect(all(wrapper, 'album-history-item')).toHaveLength(2)
    expect(test(wrapper, 'album-history-more').exists()).toBe(false)

    vi.mocked(getAlbum).mockResolvedValue({ album: album('c') })
    vi.mocked(listAlbumProposals).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'x', 0))
    const failed = show({ id: 'c' })
    await flushPromises()
    expect(test(failed, 'album-history-error').exists()).toBe(true)
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([rejected('c')]))
    await test(failed, 'album-history-retry').trigger('click')
    await flushPromises()
    expect(all(failed, 'album-history-item')).toHaveLength(1)
  })

  it('reads the history again after a decision through the existing controls, but not after a cover change', async () => {
    const a = album('a', { creator_id: 'self', pending_proposal: { id: 'r-pending', proposer_id: 'partner', base_version: 3, created_at: NOW } })
    const wrapper = await detailOf(a, historyPage([{ ...pendingOf('a'), proposer_id: 'partner' }]))
    expect(listAlbumProposals).toHaveBeenCalledTimes(1)
    vi.mocked(decideAlbumProposal).mockResolvedValueOnce(done({ action: 'decide', proposal_id: 'r-pending', outcome: 'rejected', base_version: 3, version: null },
      album('a', { creator_id: 'self' })))
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([summary('a', 'r-pending', { proposer_id: 'partner', decided_by: 'self', base_version: 3 })]))
    await test(wrapper, 'album-reject').trigger('click')
    await flushPromises()
    expect(listAlbumProposals).toHaveBeenCalledTimes(2)
    expect(test(wrapper, 'album-history-status').text()).toBe('已拒绝')

    // A cover choice returns the same name version: the history is not read again.
    const store = useAlbumsStore()
    vi.mocked(setAlbumCover).mockResolvedValueOnce(done({ action: 'set_cover', media_id: P1, event_no: 1 } as Partial<AlbumMutation>,
      album('a', { creator_id: 'self', cover: { media_id: P1, uploader_id: 'self', width: 4, height: 3, chosen: true } })))
    const covered = await store.setCover('a', P1)
    await flushPromises()
    expect(covered.outcome).toBe('done')
    expect(test(wrapper, 'album-cover-source').text()).toContain('选定的封面')
    expect(listAlbumProposals).toHaveBeenCalledTimes(2)
  })

  it('an unavailable album shows no history and never asks for it', async () => {
    vi.mocked(getAlbum).mockRejectedValue(new ApiError('ALBUM_NOT_FOUND', '相册不存在', 404))
    const wrapper = show({ id: 'gone' })
    await flushPromises()
    expect(test(wrapper, 'album-history').exists()).toBe(false)
    expect(listAlbumProposals).not.toHaveBeenCalled()
  })

  it('clears an open history and comparison when the album reads as unavailable while the page is open', async () => {
    const wrapper = await detailOf(album('a'), historyPage([approvedOld('a')]))
    vi.mocked(getAlbumProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'r-approved') })
    await test(wrapper, 'album-history-open').trigger('click')
    await flushPromises()
    expect(test(wrapper, 'album-history-review').exists()).toBe(true)
    const store = useAlbumsStore()
    vi.mocked(getAlbum).mockRejectedValue(new ApiError('ALBUM_NOT_FOUND', '相册不存在', 404))
    await store.openDetail('a')
    await flushPromises()
    expect(store.history.items).toEqual([])
    expect(store.historyReview.proposal).toBeNull()
    expect(test(wrapper, 'album-history').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('HISTORY-ORIGINAL')
  })

  it('moving to another album or account never shows the previous one\'s history', async () => {
    const late = deferred<AlbumProposalListResponse>()
    vi.mocked(getAlbum).mockImplementation(async (id: string) => ({ album: album(id) }))
    vi.mocked(listAlbumProposals).mockReturnValueOnce(late.promise).mockResolvedValueOnce(historyPage([rejected('b')]))
    const wrapper = show({ id: 'a' })
    await flushPromises()
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    late.resolve(historyPage([approvedOld('a')]))
    await flushPromises()
    expect(all(wrapper, 'album-history-item').map((row) => row.attributes('data-proposal'))).toEqual(['r-rejected'])
    expect(useAlbumsStore().history.albumId).toBe('b')

    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([withdrawn('b')]))
    signIn('other')
    await flushPromises()
    const rows = all(wrapper, 'album-history-item')
    expect(rows.map((row) => row.attributes('data-proposal'))).toEqual(['r-withdrawn'])
    expect(rows[0].text()).toContain('对方提交的申请')
  })
})

describe('what stays private', () => {
  it('no history source touches storage, caches or the console', () => {
    for (const path of ['stores/albums.ts', 'api/albums.ts', 'api/album-rules.ts', 'views/AlbumDetailView.vue']) {
      const source = readFileSync(resolve(__dirname, '../../src', path), 'utf8')
      expect(source, path).not.toMatch(/localStorage|sessionStorage|indexedDB|caches\.|console\./)
    }
  })

  it('a full history flow writes nothing to storage', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem')
    const store = useAlbumsStore()
    vi.mocked(listAlbumProposals).mockResolvedValueOnce(historyPage([approvedOld('a')]))
    await store.loadHistory('a')
    vi.mocked(getAlbumProposal).mockResolvedValueOnce({ proposal: proposalRecord('a', 'r-approved') })
    await store.openHistoryReview('a', 'r-approved')
    expect(store.historyReview.status).toBe('ready')
    expect(setItem).not.toHaveBeenCalled()
    expect(localStorage.length).toBe(0)
    expect(sessionStorage.length).toBe(0)
  })
})
