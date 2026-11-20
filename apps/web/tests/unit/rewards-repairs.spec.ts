/**
 * M7 review repairs: immutable version snapshots for edit/archive, cursor pagination
 * (load more, deduplication, stale responses) and session isolation of drafts and
 * late responses.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/rewards', () => ({
  createRewardProduct: vi.fn(),
  listRewardProducts: vi.fn(),
  editRewardProduct: vi.fn(),
  archiveRewardProduct: vi.fn(),
  redeemRewardProduct: vi.fn(),
  listRedemptions: vi.fn(async () => ({ redemptions: [], next_cursor: null })),
  fulfillRedemption: vi.fn(),
}))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn() }))

import { ApiError, setCsrfToken } from '@/api/client'
import { archiveRewardProduct, createRewardProduct, editRewardProduct, listRewardProducts } from '@/api/rewards'
import { newIdempotencyKey } from '@/api/tasks'
import type { RewardProduct, RewardProductListResponse, RewardProductMutationResponse } from '@/api/types'
import { useRewardsStore } from '@/stores/rewards'
import { useSessionStore } from '@/stores/session'
import RewardsView from '@/views/RewardsView.vue'

function product(id: string, overrides: Partial<RewardProduct> = {}): RewardProduct {
  return {
    id, creator_id: 'self', owner_id: 'partner', title: `奖励 ${id}`, description: '', price_flowers: 5,
    redemption_mode: 'direct', status: 'active', version: 1,
    created_at: '2026-10-26T00:00:00.000000Z', updated_at: '2026-10-26T00:00:00.000000Z', ...overrides,
  }
}

function reply(p: RewardProduct, action: 'create' | 'edit' | 'archive'): RewardProductMutationResponse {
  return {
    mutation: { action, version: p.version, actor_id: p.creator_id, title: p.title, description: p.description,
      price_flowers: p.price_flowers, redemption_mode: p.redemption_mode, status: p.status, created_at: p.updated_at },
    product: p,
  }
}

function page(products: RewardProduct[], next: string | null = null): RewardProductListResponse {
  return { products, next_cursor: next }
}

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => {
    resolve = yes
    reject = no
  })
  return { promise, resolve, reject }
}

function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-26T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-26T00:00:00Z',
      members: [] },
  })
  setCsrfToken(`csrf-${id}`)
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  for (const fn of [createRewardProduct, listRewardProducts, editRewardProduct, archiveRewardProduct]) vi.mocked(fn).mockReset()
  vi.mocked(listRewardProducts).mockResolvedValue(page([]))
  let counter = 0
  vi.mocked(newIdempotencyKey).mockReset()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.restoreAllMocks()
})

async function mountWith(products: RewardProduct[]) {
  vi.mocked(listRewardProducts).mockResolvedValue(page(products))
  wrapper = mount(RewardsView, { global: { stubs: { RouterLink: true } } })
  await flushPromises()
  return wrapper
}

async function refreshTo(w: VueWrapper, products: RewardProduct[]) {
  vi.mocked(listRewardProducts).mockResolvedValue(page(products))
  await w.find('[data-test="rewards-refresh"]').trigger('click')
  await flushPromises()
}

describe('version snapshots', () => {
  it('an edit draft opened at v1 still submits v1 after a refresh to v2, and the conflict is shown', async () => {
    const w = await mountWith([product('p1', { version: 1 })])
    await w.find('[data-test="edit-p1"]').trigger('click')
    await refreshTo(w, [product('p1', { version: 2, price_flowers: 9 })])
    expect(w.find('[data-test="edit-drift"]').text()).toContain('第 2 版')
    vi.mocked(editRewardProduct).mockRejectedValueOnce(new ApiError('PRODUCT_VERSION_CONFLICT', 'stale', 409))
    await w.find('[data-test="edit-price"]').setValue('7')
    await w.find('[data-test="edit-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editRewardProduct).mock.calls[0][0]).toBe('p1')
    expect(vi.mocked(editRewardProduct).mock.calls[0][1]).toMatchObject({ price_flowers: 7, expected_version: 1 })
    expect(w.find('[data-test="reward-notice-p1"]').text()).toContain('已被修改')
  })

  it('a draft whose product disappears from the list keeps its original target', async () => {
    const w = await mountWith([product('p2', { version: 3 })])
    await w.find('[data-test="edit-p2"]').trigger('click')
    await refreshTo(w, [product('p9')])
    expect(w.find('[data-test="edit-draft"]').exists()).toBe(true)
    expect(w.find('[data-test="edit-drift"]').text()).toContain('不在当前列表')
    vi.mocked(editRewardProduct).mockResolvedValueOnce(reply(product('p2', { version: 4 }), 'edit'))
    await w.find('[data-test="edit-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editRewardProduct).mock.calls[0][0]).toBe('p2')
    expect(vi.mocked(editRewardProduct).mock.calls[0][1]).toMatchObject({ expected_version: 3 })
  })

  it('archive confirmation captured at v1 archives v1 after a refresh to v2; the conflict is shown', async () => {
    const w = await mountWith([product('p3', { version: 1 })])
    await w.find('[data-test="archive-p3"]').trigger('click')
    await refreshTo(w, [product('p3', { version: 2, title: '新名称' })])
    expect(w.find('[data-test="archive-drift-p3"]').text()).toContain('第 1 版')
    vi.mocked(archiveRewardProduct).mockRejectedValueOnce(new ApiError('PRODUCT_VERSION_CONFLICT', 'stale', 409))
    await w.find('[data-test="archive-p3"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(archiveRewardProduct).mock.calls[0].slice(0, 2)).toEqual(['p3', 1])
    expect(w.find('[data-test="reward-notice-p3"]').text()).toContain('已被修改')
  })
})

describe('pagination', () => {
  it('loads more pages without duplicates and hides load-more at the end', async () => {
    const store = useRewardsStore()
    vi.mocked(listRewardProducts)
      .mockResolvedValueOnce(page([product('a'), product('b')], 'c1'))
      .mockResolvedValueOnce(page([product('b'), product('c')], null))
    await store.load()
    expect(store.nextCursor).toBe('c1')
    await store.loadMore()
    expect(vi.mocked(listRewardProducts).mock.calls[1][0]).toBe('c1')
    expect(store.products.map((p) => p.id)).toEqual(['a', 'b', 'c'])
    expect(store.nextCursor).toBeNull()
    await store.loadMore()
    expect(listRewardProducts).toHaveBeenCalledTimes(2)
  })

  it('a newer first-page load discards an in-flight load-more response', async () => {
    const store = useRewardsStore()
    vi.mocked(listRewardProducts).mockResolvedValueOnce(page([product('a')], 'c1'))
    await store.load()
    const more = deferred<RewardProductListResponse>()
    vi.mocked(listRewardProducts).mockReturnValueOnce(more.promise).mockResolvedValueOnce(page([product('z')], null))
    const pending = store.loadMore()
    await store.load()
    more.resolve(page([product('old')], 'c2'))
    await pending
    expect(store.products.map((p) => p.id)).toEqual(['z'])
    expect(store.nextCursor).toBeNull()
  })
})

describe('session isolation', () => {
  it('late success and failure for the old account change nothing for the new account', async () => {
    const store = useRewardsStore()
    const created = deferred<RewardProductMutationResponse>()
    const edited = deferred<RewardProductMutationResponse>()
    vi.mocked(createRewardProduct).mockReturnValueOnce(created.promise)
    vi.mocked(editRewardProduct).mockReturnValueOnce(edited.promise)
    const terms = { title: 'x', description: '', price_flowers: 5, redemption_mode: 'direct' as const }
    const create = store.create(terms)
    const edit = store.edit({ id: 'p1', title: 'p1', version: 1 }, terms)
    await flushPromises()
    signIn('other')
    vi.mocked(listRewardProducts).mockResolvedValueOnce(page([product('mine-new', { creator_id: 'other' })]))
    await store.load()
    created.resolve(reply(product('old'), 'create'))
    edited.reject(new ApiError('PRODUCT_VERSION_CONFLICT', 'stale', 409))
    expect((await create).outcome).toBe('discarded')
    expect((await edit).outcome).toBe('discarded')
    expect(store.notices).toEqual([])
    expect(store.actions).toEqual({})
    expect(store.products.map((p) => p.id)).toEqual(['mine-new'])
  })

  it('logout/account switch clears drafts, snapshots, confirmation and local errors in the view', async () => {
    const w = await mountWith([product('p5'), product('p6')])
    await w.find('#reward-title').setValue('草稿')
    await w.find('#reward-price').setValue('0')
    await w.find('[data-test="create-form"]').trigger('submit')
    expect(w.find('[data-test="create-error"]').exists()).toBe(true)
    await w.find('[data-test="edit-p5"]').trigger('click')
    await w.find('[data-test="edit-price"]').setValue('abc')
    await w.find('[data-test="edit-form"]').trigger('submit')
    expect(w.find('[data-test="edit-error"]').exists()).toBe(true)
    await w.find('[data-test="archive-p6"]').trigger('click')
    expect(w.find('[data-test="archive-p6"]').text()).toBe('确认下架')

    vi.mocked(listRewardProducts).mockResolvedValue(page([product('p6', { creator_id: 'other' })]))
    signIn('other')
    await flushPromises()
    expect((w.find('#reward-title').element as HTMLInputElement).value).toBe('')
    expect(w.find('[data-test="create-error"]').exists()).toBe(false)
    expect(w.find('[data-test="edit-draft"]').exists()).toBe(false)
    expect(w.find('[data-test="archive-p6"]').text()).toBe('下架')
    expect(editRewardProduct).not.toHaveBeenCalled()
    expect(archiveRewardProduct).not.toHaveBeenCalled()
  })
})
