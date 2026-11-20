/**
 * M8 direct redemption (web): the confirmation captures the product version/price shown,
 * a refreshed row never changes what is submitted, the wallet reloads only after a
 * confirmed debit (and its failure never turns success into failure), explicit same-key
 * retry, request mode not redeemable, creator-only fulfillment and session isolation.
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
  listRedemptions: vi.fn(),
  fulfillRedemption: vi.fn(),
  transitionRedemption: vi.fn(),
}))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn() }))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { fulfillRedemption, listRedemptions, listRewardProducts, redeemRewardProduct, transitionRedemption } from '@/api/rewards'
import { newIdempotencyKey } from '@/api/tasks'
import type { Redemption, RedemptionMutationResponse, RewardProduct } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useRewardsStore } from '@/stores/rewards'
import { useSessionStore } from '@/stores/session'
import RewardsView from '@/views/RewardsView.vue'

function product(id: string, overrides: Partial<RewardProduct> = {}): RewardProduct {
  return {
    id, creator_id: 'partner', owner_id: 'self', title: `奖励 ${id}`, description: '', price_flowers: 5,
    redemption_mode: 'direct', status: 'active', version: 1,
    created_at: '2026-10-26T00:00:00.000000Z', updated_at: '2026-10-26T00:00:00.000000Z', ...overrides,
  }
}

function redemption(id: string, overrides: Partial<Redemption> = {}): Redemption {
  return {
    id, product_id: 'p1', product_version: 1, creator_id: 'partner', redeemer_id: 'self', title: '快照标题',
    description: '', price_flowers: 5, redemption_mode: 'direct', operation_id: 'op', status: 'pending_fulfillment',
    created_at: '2026-10-26T01:00:00.000000Z', fulfilled_at: null, fulfilled_by: null, ...overrides,
  }
}

function redeemed(r: Redemption): RedemptionMutationResponse {
  return { mutation: { action: 'redeem', redemption_id: r.id, price_flowers: r.price_flowers, debited_flowers: r.price_flowers }, redemption: r }
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
  for (const fn of [listRewardProducts, redeemRewardProduct, listRedemptions, fulfillRedemption, transitionRedemption, getWallet]) {
    vi.mocked(fn).mockReset()
  }
  vi.mocked(listRewardProducts).mockResolvedValue({ products: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 0, sunshine: 0 } })
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

async function mountWith(products: RewardProduct[], redemptions: Redemption[] = []) {
  vi.mocked(listRewardProducts).mockResolvedValue({ products, next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions, next_cursor: null })
  wrapper = mount(RewardsView, { global: { stubs: { RouterLink: true } } })
  await flushPromises()
  return wrapper
}

describe('redeem confirmation', () => {
  it('submits the confirmed version after a refresh shows a new price; the conflict is shown and the wallet untouched', async () => {
    const w = await mountWith([product('p1', { version: 1, price_flowers: 5 })])
    await w.find('[data-test="redeem-p1"]').trigger('click')
    expect(w.find('[data-test="redeem-confirm"]').text()).toContain('5')
    vi.mocked(listRewardProducts).mockResolvedValue({ products: [product('p1', { version: 2, price_flowers: 50 })], next_cursor: null })
    await w.find('[data-test="rewards-refresh"]').trigger('click')
    await flushPromises()
    expect(w.find('[data-test="redeem-confirm"]').text()).toContain('确认兑换（5 朵小红花）')
    expect(w.find('[data-test="redeem-drift"]').text()).toContain('50')
    vi.mocked(redeemRewardProduct).mockRejectedValueOnce(new ApiError('PRODUCT_VERSION_CONFLICT', 'stale', 409))
    await w.find('[data-test="redeem-submit"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(redeemRewardProduct).mock.calls[0].slice(0, 2)).toEqual(['p1', 1])
    expect(w.find('[data-test="reward-notice-redeem:p1"]').text()).toContain('已被修改')
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('request-mode products offer an explicit unfunded application confirmation', async () => {
    const w = await mountWith([product('p2', { redemption_mode: 'request' })])
    expect(w.find('[data-test="redeem-p2"]').exists()).toBe(true)
    await w.find('[data-test="redeem-p2"]').trigger('click')
    expect(w.find('[data-test="redeem-confirm"]').text()).toContain('现在不扣款')
    expect(redeemRewardProduct).not.toHaveBeenCalled()
    expect(w.find('[data-test="for-me"]').text()).toContain('申请兑换')
  })
})

describe('redeem store', () => {
  it.each([
    new ApiError('UNAUTHENTICATED', 'session expired', 401),
    new ApiError('CSRF_TOKEN_INVALID', 'csrf', 403),
    new ApiError('INTERNAL_ERROR', 'gateway response', 403),
    new ApiError('PRODUCT_NOT_FOUND', 'not found', 404),
    new OfflineMutationBlockedError(),
  ])('keeps the original redemption after a failed retry: %s', async (failure) => {
    const store = useRewardsStore()
    vi.mocked(redeemRewardProduct).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(failure).mockResolvedValueOnce(redeemed(redemption('r1')))
    expect((await store.redeem({ id: 'p1', title: 't', version: 1 })).outcome).toBe('unknown')
    const key = store.actions['redeem:p1'].key
    expect((await store.retry('redeem:p1')).outcome).toBe('unknown')
    expect(store.actions['redeem:p1']).toMatchObject({ status: 'unknown', unresolved: true, key, expectedVersion: 1 })
    expect(getWallet).not.toHaveBeenCalled()
    expect((await store.redeem({ id: 'p1', title: 't', version: 1 })).outcome).toBe('busy')
    expect((await store.retry('redeem:p1')).outcome).toBe('done')
    expect(vi.mocked(redeemRewardProduct).mock.calls).toEqual([
      ['p1', 1, key], ['p1', 1, key], ['p1', 1, key],
    ])
  })

  it('keeps an earlier unknown redemption if CSRF preparation fails', async () => {
    const store = useRewardsStore()
    vi.mocked(redeemRewardProduct).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    await store.redeem({ id: 'p1', title: 't', version: 1 })
    const spy = vi.spyOn(useSessionStore(), 'ensureCsrfToken').mockRejectedValueOnce(new Error('token unavailable'))
    expect((await store.retry('redeem:p1')).outcome).toBe('unknown')
    expect(store.actions['redeem:p1']).toMatchObject({ status: 'unknown', unresolved: true })
    expect(store.actions['redeem:p1'].errorText).toContain('仍未确认')
    expect(redeemRewardProduct).toHaveBeenCalledTimes(1)
    spy.mockRestore()
  })

  it('does not confirm a redemption when the response references another product', async () => {
    const store = useRewardsStore()
    vi.mocked(redeemRewardProduct)
      .mockResolvedValueOnce(redeemed(redemption('r1', { product_id: 'wrong-product' })))
      .mockResolvedValueOnce(redeemed(redemption('r1')))
    expect((await store.redeem({ id: 'p1', title: 't', version: 1 })).outcome).toBe('unknown')
    const key = store.actions['redeem:p1'].key
    expect(getWallet).not.toHaveBeenCalled()
    expect((await store.retry('redeem:p1')).outcome).toBe('done')
    expect(vi.mocked(redeemRewardProduct).mock.calls).toEqual([['p1', 1, key], ['p1', 1, key]])
  })
  it('reloads the wallet only after a confirmed debit; a wallet failure keeps the success', async () => {
    const store = useRewardsStore()
    vi.mocked(redeemRewardProduct)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(redeemed(redemption('r1', { product_version: 3 })))
    vi.mocked(getWallet).mockRejectedValue(new ApiError('INTERNAL_ERROR', 'down', 500))
    expect((await store.redeem({ id: 'p1', title: 't', version: 3 })).outcome).toBe('unknown')
    expect(getWallet).not.toHaveBeenCalled()
    expect(store.actions['redeem:p1'].status).toBe('unknown')
    const result = await store.retry('redeem:p1')
    await flushPromises()
    expect(result.outcome).toBe('done')
    const calls = vi.mocked(redeemRewardProduct).mock.calls
    expect(calls[1]).toEqual(calls[0])
    expect(calls[0].slice(0, 2)).toEqual(['p1', 3])
    expect(getWallet).toHaveBeenCalledTimes(1)
    expect(store.notices[0]).toMatchObject({ id: 'redeem:p1', ok: true })
  })

  it('an insufficient balance ends the action with a notice and no wallet reload', async () => {
    const store = useRewardsStore()
    vi.mocked(redeemRewardProduct).mockRejectedValueOnce(new ApiError('INSUFFICIENT_BALANCE', 'low', 409))
    expect(await store.redeem({ id: 'p1', title: 't', version: 1 })).toEqual({ outcome: 'rejected', errorCode: 'INSUFFICIENT_BALANCE' })
    expect(store.actions['redeem:p1']).toBeUndefined()
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('a late redemption result for the old account is discarded after an account switch', async () => {
    const store = useRewardsStore()
    let resolve!: (v: RedemptionMutationResponse) => void
    vi.mocked(redeemRewardProduct).mockReturnValueOnce(new Promise((yes) => { resolve = yes }))
    const pending = store.redeem({ id: 'p1', title: 't', version: 1 })
    await flushPromises()
    signIn('other')
    resolve(redeemed(redemption('r9')))
    expect((await pending).outcome).toBe('discarded')
    expect(store.notices).toEqual([])
    expect(getWallet).not.toHaveBeenCalled()
  })
})

describe('M9 request and mutual cancellation', () => {
  it('request success never loads wallet or assigns balances', async () => {
    const store = useRewardsStore()
    const req = redemption('r1', { redemption_mode: 'request', status: 'pending_confirmation', operation_id: null })
    vi.mocked(redeemRewardProduct).mockResolvedValueOnce({ mutation: { action: 'redeem', redemption_id: req.id, debited_flowers: 0 }, redemption: req })
    expect((await store.redeem({ id: 'p1', title: '申请', version: 1 })).outcome).toBe('done')
    expect(store.notices[0]?.text).toContain('尚未扣款')
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('an insufficient confirmation is definitive but leaves the request available for a new attempt', async () => {
    const store = useRewardsStore()
    vi.mocked(transitionRedemption).mockRejectedValueOnce(new ApiError('INSUFFICIENT_BALANCE', 'low', 409))
    const req = redemption('r1', { status: 'pending_confirmation' })
    expect((await store.transition(req, 'confirm')).outcome).toBe('rejected')
    expect(store.actions['confirm:r1']).toBeUndefined()
    expect(getWallet).not.toHaveBeenCalled()
    vi.mocked(transitionRedemption).mockResolvedValueOnce({ mutation: { action: 'confirm', redemption_id: req.id, debited_flowers: 5 }, redemption: req })
    await store.transition(req, 'confirm')
    expect(vi.mocked(transitionRedemption).mock.calls[0]?.[3]).not.toBe(vi.mocked(transitionRedemption).mock.calls[1]?.[3])
    expect(getWallet).toHaveBeenCalledTimes(1)
  })

  it('unknown refund preserves both key and proposal identity across refresh and explicitly retries once', async () => {
    const store = useRewardsStore()
    const paid = redemption('r1', { pending_cancellation: { id: 'old-proposal', proposed_by: 'partner' } })
    vi.mocked(transitionRedemption).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    expect((await store.transition(paid, 'cancel_approve', 'old-proposal')).outcome).toBe('unknown')
    const original = vi.mocked(transitionRedemption).mock.calls[0]!
    vi.mocked(listRedemptions).mockResolvedValueOnce({ redemptions: [{ ...paid, pending_cancellation: { id: 'new-proposal', proposed_by: 'partner' } }], next_cursor: null })
    await store.loadRedemptions()
    expect(transitionRedemption).toHaveBeenCalledTimes(1)
    vi.mocked(transitionRedemption).mockResolvedValueOnce({ mutation: { action: 'cancel_approve', redemption_id: paid.id, refunded_flowers: 5 }, redemption: { ...paid, status: 'cancelled', cancel_reason: 'mutual' } })
    vi.mocked(getWallet).mockRejectedValueOnce(new Error('wallet unavailable'))
    await store.retry('cancel_approve:r1')
    expect(vi.mocked(transitionRedemption).mock.calls[1]).toEqual(original)
    expect(getWallet).toHaveBeenCalledTimes(1)
    expect(store.notices.some(n => n.ok && n.text.includes('原额退还'))).toBe(true)
  })

  it('late transition success after account switch has no notice, reload or wallet effect', async () => {
    const store = useRewardsStore()
    let resolve!: (value: RedemptionMutationResponse) => void
    vi.mocked(transitionRedemption).mockReturnValueOnce(new Promise(yes => { resolve = yes }))
    const req = redemption('r1')
    const pending = store.transition(req, 'confirm')
    await flushPromises()
    signIn('other')
    resolve({ mutation: { action: 'confirm', redemption_id: req.id, debited_flowers: 5 }, redemption: req })
    expect((await pending).outcome).toBe('discarded')
    expect(store.notices).toEqual([])
    expect(listRedemptions).not.toHaveBeenCalled()
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('cancellation confirmation captures the proposal before a list refresh replaces it', async () => {
    const paid = redemption('r1', { pending_cancellation: { id: 'old-proposal', proposed_by: 'partner' } })
    const w = await mountWith([], [paid])
    const button = w.findAll('button').find(b => b.text() === '同意取消并退款')!
    await button.trigger('click')
    const store = useRewardsStore()
    store.redemptions = [{ ...paid, pending_cancellation: { id: 'new-proposal', proposed_by: 'partner' } }]
    vi.mocked(transitionRedemption).mockRejectedValueOnce(new ApiError('REDEMPTION_STATE_CONFLICT', 'changed', 409))
    await w.find('[data-test="transition-submit"]').trigger('click')
    await flushPromises()
    expect(transitionRedemption).toHaveBeenCalledWith('r1', 'cancel_approve', 'old-proposal', expect.any(String))
    expect(store.notices.some(n => !n.ok)).toBe(true)
  })

  it('account switch clears a pending transition confirmation', async () => {
    const req = redemption('r1', { creator_id: 'self', redeemer_id: 'partner', status: 'pending_confirmation' })
    const w = await mountWith([], [req])
    await w.findAll('button').find(b => b.text() === '同意申请')!.trigger('click')
    expect(w.find('[data-test="transition-confirm"]').exists()).toBe(true)
    signIn('other')
    await flushPromises()
    expect(w.find('[data-test="transition-confirm"]').exists()).toBe(false)
    expect(transitionRedemption).not.toHaveBeenCalled()
  })
})

describe('history and fulfillment', () => {
  it('only the creator sees fulfil for a pending redemption; fulfil does not touch the wallet', async () => {
    const mineToFulfil = redemption('r1', { creator_id: 'self', redeemer_id: 'partner', title: '电影' })
    const redeemedByMe = redemption('r2')
    const done = redemption('r3', { creator_id: 'self', redeemer_id: 'partner', status: 'fulfilled' })
    const w = await mountWith([], [mineToFulfil, redeemedByMe, done])
    expect(w.find('[data-test="fulfill-r1"]').exists()).toBe(true)
    expect(w.find('[data-test="fulfill-r2"]').exists()).toBe(false)
    expect(w.find('[data-test="redemption-r3"]').text()).toContain('已履约')
    vi.mocked(fulfillRedemption).mockResolvedValueOnce({
      mutation: { action: 'fulfill', redemption_id: 'r1' },
      redemption: { ...mineToFulfil, status: 'fulfilled', fulfilled_at: '2026-10-26T02:00:00.000000Z', fulfilled_by: 'self' },
    })
    await w.find('[data-test="fulfill-r1"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(fulfillRedemption).mock.calls[0][0]).toBe('r1')
    expect(w.find('[data-test="reward-notice-fulfill:r1"]').text()).toContain('已确认履约')
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('an account switch clears an open redemption confirmation', async () => {
    const w = await mountWith([product('p1')])
    await w.find('[data-test="redeem-p1"]').trigger('click')
    expect(w.find('[data-test="redeem-confirm"]').exists()).toBe(true)
    signIn('other')
    await flushPromises()
    expect(w.find('[data-test="redeem-confirm"]').exists()).toBe(false)
    expect(redeemRewardProduct).not.toHaveBeenCalled()
  })
})
