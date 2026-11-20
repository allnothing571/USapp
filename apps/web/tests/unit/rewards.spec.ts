/**
 * M7 reward shop: store mutation lifecycle (unknown result keeps the key for an explicit
 * same-key retry; a stale version ends the action with a notice) and the view's
 * creator/owner sections, archived badge and versioned edit request.
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
import { archiveRewardProduct, createRewardProduct, editRewardProduct, listRewardProducts, listRedemptions } from '@/api/rewards'
import { newIdempotencyKey } from '@/api/tasks'
import { buildRewardTerms } from '@/api/task-rules'
import type { RewardProduct, RewardProductMutationResponse } from '@/api/types'
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

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  for (const fn of [createRewardProduct, listRewardProducts, editRewardProduct, archiveRewardProduct]) vi.mocked(fn).mockReset()
  vi.mocked(listRewardProducts).mockResolvedValue({ products: [], next_cursor: null })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockReset()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: 'csrf',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-26T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-26T00:00:00Z',
      members: [{ user_id: 'partner', slot: 2, display_name: '小 B', joined_at: '2026-10-26T00:00:00Z' }] },
  })
  setCsrfToken('csrf')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.restoreAllMocks()
})

describe('reward rules', () => {
  it('labels an administrator cancellation without offering another refund or exposing its private reason', async () => {
    vi.mocked(listRedemptions).mockResolvedValueOnce({ redemptions: [{
      id: 'admin-cancelled', product_id: 'p1', product_version: 1, creator_id: 'partner', redeemer_id: 'self',
      title: 'Original reward', description: '', price_flowers: 3, redemption_mode: 'direct', operation_id: 'original-debit',
      status: 'cancelled', cancel_reason: 'admin', created_at: '2026-11-14T00:00:00Z',
      cancelled_at: '2026-11-14T01:00:00Z', fulfilled_at: null, fulfilled_by: null,
    }], next_cursor: null })
    wrapper = mount(RewardsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const row = wrapper.get('[data-test="redemption-admin-cancelled"]')
    expect(row.text()).toContain('管理员已取消')
    expect(row.text()).toContain('已扣款部分已原路退回')
    expect(row.findAll('button')).toHaveLength(0)
  })
  it('price is an integer 1-999 and text follows the task rules', () => {
    const base = { title: ' 电影 ', description: '', price: '5', mode: 'direct' as const }
    expect(buildRewardTerms(base)).toEqual({ ok: true,
      terms: { title: '电影', description: '', price_flowers: 5, redemption_mode: 'direct' } })
    for (const price of ['0', '1000', '1.5', '', '-1']) expect(buildRewardTerms({ ...base, price }).ok).toBe(false)
    expect(buildRewardTerms({ ...base, title: '' }).ok).toBe(false)
  })
})

describe('rewards store', () => {
  it('keeps the key after an unknown result and resends the original body only on retry', async () => {
    const store = useRewardsStore()
    const terms = { title: 'x', description: '', price_flowers: 5, redemption_mode: 'direct' as const }
    vi.mocked(createRewardProduct)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(reply(product('p1'), 'create'))
    expect((await store.create(terms)).outcome).toBe('unknown')
    expect(store.actions.new.status).toBe('unknown')
    expect((await store.create(terms)).outcome).toBe('busy')
    expect((await store.retry('new')).outcome).toBe('done')
    const calls = vi.mocked(createRewardProduct).mock.calls
    expect(calls).toHaveLength(2)
    expect(calls[1]).toEqual(calls[0])
    expect(store.actions.new).toBeUndefined()
    expect(store.notices[0]).toMatchObject({ id: 'new', ok: true })
  })

  it('a stale version ends the edit with a notice; edit and archive send expected_version', async () => {
    const store = useRewardsStore()
    const p = product('p2', { version: 3 })
    vi.mocked(editRewardProduct).mockRejectedValueOnce(new ApiError('PRODUCT_VERSION_CONFLICT', 'stale', 409))
    const result = await store.edit(p, { title: 'y', description: '', price_flowers: 6, redemption_mode: 'request' })
    expect(result).toEqual({ outcome: 'rejected', errorCode: 'PRODUCT_VERSION_CONFLICT' })
    expect(vi.mocked(editRewardProduct).mock.calls[0][1]).toEqual(
      { title: 'y', description: '', price_flowers: 6, redemption_mode: 'request', expected_version: 3 })
    expect(store.actions.p2).toBeUndefined()
    expect(store.notices[0]).toMatchObject({ id: 'p2', ok: false })
    vi.mocked(archiveRewardProduct).mockResolvedValueOnce(reply(product('p2', { version: 4, status: 'archived' }), 'archive'))
    expect((await store.archive(p)).outcome).toBe('done')
    expect(vi.mocked(archiveRewardProduct).mock.calls[0].slice(0, 2)).toEqual(['p2', 3])
  })
})

describe('rewards view', () => {
  it('separates the partner shop from my products, badges archived ones, and edits with the current version', async () => {
    const partnerOne = product('p3', { creator_id: 'partner', owner_id: 'self', title: '按摩' })
    const mineActive = product('p4', { version: 2 })
    const mineArchived = product('p5', { status: 'archived', version: 3 })
    vi.mocked(listRewardProducts).mockResolvedValue({ products: [partnerOne, mineActive, mineArchived], next_cursor: null })
    vi.mocked(editRewardProduct).mockResolvedValue(reply(product('p4', { version: 3, price_flowers: 8 }), 'edit'))
    wrapper = mount(RewardsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.find('[data-test="for-me"]').text()).toContain('按摩')
    expect(wrapper.find('[data-test="for-me"]').text()).not.toContain('奖励 p4')
    expect(wrapper.find('[data-test="mine-p5"]').text()).toContain('已下架')
    expect(wrapper.find('[data-test="edit-p5"]').exists()).toBe(false)
    await wrapper.find('[data-test="edit-p4"]').trigger('click')
    await wrapper.find('[data-test="edit-price"]').setValue('8')
    await wrapper.find('[data-test="edit-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editRewardProduct).mock.calls[0][0]).toBe('p4')
    expect(vi.mocked(editRewardProduct).mock.calls[0][1]).toMatchObject({ price_flowers: 8, expected_version: 2 })
  })

  it('archive needs a confirming second click', async () => {
    vi.mocked(listRewardProducts).mockResolvedValue({ products: [product('p6')], next_cursor: null })
    vi.mocked(archiveRewardProduct).mockResolvedValue(reply(product('p6', { version: 2, status: 'archived' }), 'archive'))
    wrapper = mount(RewardsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="archive-p6"]').trigger('click')
    expect(archiveRewardProduct).not.toHaveBeenCalled()
    await wrapper.find('[data-test="archive-p6"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(archiveRewardProduct).mock.calls[0].slice(0, 2)).toEqual(['p6', 1])
  })
})
