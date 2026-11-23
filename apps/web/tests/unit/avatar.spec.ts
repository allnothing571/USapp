import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { createHash } from 'node:crypto'
import { resolve } from 'node:path'
vi.mock('@/api/avatar', async original => ({ ...(await original<typeof import('@/api/avatar')>()),
  getAvatarCatalog: vi.fn(), getAvatarAppearance: vi.fn(), getOwnedAvatars: vi.fn(), purchaseAvatar: vi.fn(), saveAvatarLoadout: vi.fn() }))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/tasks', async original => ({ ...(await original<typeof import('@/api/tasks')>()), newIdempotencyKey: vi.fn() }))

import { ApiError, OfflineMutationBlockedError } from '@/api/client'
import { getAvatarCatalog, getAvatarAppearance, getOwnedAvatars, purchaseAvatar, saveAvatarLoadout,
  isAvatarItem, isAppearance, isPurchaseResponse, isLoadoutResponse, isOwnedPage, emptySlots, type LoadoutRequest } from '@/api/avatar'
import { newIdempotencyKey } from '@/api/tasks'
import { getWallet } from '@/api/wallet'
import { figureLayers, emptyFigureSlots, itemThumbnail } from '@/api/avatar-art'
import { useAvatarStore } from '@/stores/avatar'
import { useSessionStore } from '@/stores/session'
import { useWalletStore } from '@/stores/wallet'
import AvatarView from '@/views/AvatarView.vue'
import AvatarFigure from '@/components/AvatarFigure.vue'
import AvatarHome from '@/components/AvatarHome.vue'
import manifest from '@/assets/avatar-manifest.json'
import { id, item, hair, clothes, expression, hat, appearance, purchase, receipt, loadoutReceipt } from './avatar-fixtures'

const deferred = <T>() => { let resolve!: (v: T) => void, reject!: (e: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no }); return { promise, resolve, reject } }
let count = 0
const wrappers: VueWrapper[] = []
function signIn(n = 500) {
  const s = useSessionStore()
  s.status = 'authenticated'; s.user = { id: id(n), email: 'avatar@example.test', display_name: '我', created_at: '2026-11-10T00:00:00Z' }
}
function view(tab: 'shop' | 'wardrobe' = 'shop') {
  const w = mount(AvatarView, { props: { tab }, global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } })
  wrappers.push(w); return w
}
beforeEach(() => {
  setActivePinia(createPinia()); vi.clearAllMocks(); count = 0; signIn()
  vi.mocked(newIdempotencyKey).mockImplementation(() => `avatar-key-${String(++count).padStart(16, '0')}`)
  vi.mocked(getAvatarCatalog).mockResolvedValue({ items: [hair(), clothes(), expression(), hat()] })
  vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: appearance() })
  vi.mocked(getOwnedAvatars).mockResolvedValue({ items: [], next_cursor: null })
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 7, sunshine: 100 } })
  vi.mocked(purchaseAvatar).mockResolvedValue(receipt())
  vi.mocked(saveAvatarLoadout).mockResolvedValue(loadoutReceipt())
})
afterEach(() => { wrappers.splice(0).forEach(w => w.unmount()); vi.unstubAllGlobals() })

describe('avatar response boundaries', () => {
  it('validates all categories and rejects mismatched asset metadata and invalid prices', () => {
    expect(isAvatarItem(hair())).toBe(true)
    expect(isAvatarItem({ ...hat(), price_sunshine: null })).toBe(false)
    expect(isAvatarItem({ ...hair(), asset: { ...hair().asset, key: '../x.svg' } })).toBe(false)
    expect(isAvatarItem({ ...hair(), asset: { ...hair().asset, layers: ['front', 'front', 'back'] } })).toBe(false)
    expect(isAvatarItem({ ...hat(), asset: { ...hat().asset, covers_hair_top: null } })).toBe(false)
  })
  it('distinguishes absent defaults from a complete saved outfit', () => {
    expect(isAppearance(appearance())).toBe(true)
    expect(isAppearance({ ...appearance(), complete: false })).toBe(false)
    expect(isAppearance({ ...appearance(), source: 'saved_loadout' })).toBe(false)
    expect(isAppearance({ ...appearance(2), slots: { ...appearance(2).slots, hair: null } })).toBe(false)
    expect(isAppearance({ ...appearance(), complete: false, starter_version: null, slots: emptyFigureSlots() })).toBe(true)
  })
  it('requires original purchase facts to match the captured revision and price, accepting newer current state', () => {
    const r = receipt(); r.current.item.version = 3; r.current.item.name = '新名称'; r.current.item.price_sunshine = 40
    expect(isPurchaseResponse(r, hat())).toBe(true)
    expect(isPurchaseResponse({ ...r, purchase: { ...r.purchase, debited_sunshine: 19 } }, hat())).toBe(false)
    expect(isPurchaseResponse({ ...r, purchase: { ...r.purchase, item_version: 2 } }, hat())).toBe(false)
    expect(isPurchaseResponse({ ...r, purchase: { ...r.purchase, item_id: id(8) } }, hat())).toBe(false)
  })
  it('validates save facts against requested slots, default resolution and minimum current version', () => {
    const req = { expected_version: 2, slots: emptySlots() }
    expect(isLoadoutResponse(loadoutReceipt(req), req)).toBe(true)
    expect(isLoadoutResponse(loadoutReceipt(req, null), req)).toBe(true)
    expect(isLoadoutResponse(loadoutReceipt(req, appearance(2)), req)).toBe(false)
    const r = loadoutReceipt(req); r.loadout.slots.hat = id(9)
    expect(isLoadoutResponse(r, req)).toBe(false)
    expect(isOwnedPage({ items: [{ purchase: purchase(), current_item: { ...hat(), listed: false } }], next_cursor: null })).toBe(true)
  })
})

describe('avatar identity, sequencing and write recovery', () => {
  it('discards late account-A reads after identity reset', async () => {
    const pending = deferred<unknown>(); vi.mocked(getAvatarAppearance).mockReturnValueOnce(pending.promise)
    const store = useAvatarStore(), reading = store.loadAppearance()
    store.reset(); signIn(501); pending.resolve({ appearance: appearance(9) }); await reading
    expect(store.appearance).toBeNull(); expect(store.appearanceStatus).toBe('idle')
  })
  it('session logout reset clears private avatar state', () => {
    const store = useAvatarStore(); store.catalog = [hat()]; store.notice = 'private'
    useSessionStore().resetBusinessState()
    expect(store.catalog).toEqual([]); expect(store.notice).toBe(''); expect(store.purchaseAction).toBeNull()
  })
  it('newer reads win; an older appearance version cannot replace a newer confirmed view', async () => {
    const old = deferred<unknown>(); vi.mocked(getAvatarAppearance).mockReturnValueOnce(old.promise)
    const store = useAvatarStore(), a = store.loadAppearance()
    vi.mocked(getAvatarAppearance).mockResolvedValueOnce({ appearance: appearance(3) }); await store.loadAppearance()
    old.resolve({ appearance: appearance(1) }); await a
    expect(store.appearance?.loadout_version).toBe(3)
    vi.mocked(getAvatarAppearance).mockResolvedValueOnce({ appearance: appearance(2) }); await store.loadAppearance()
    expect(store.appearance?.loadout_version).toBe(3); expect(store.appearanceStatus).toBe('error')
  })
  it('paginates owned items, retains unlisted ownership and deduplicates repeated rows', async () => {
    const row = { purchase: purchase(), current_item: { ...hat(), listed: false } }
    vi.mocked(getOwnedAvatars).mockResolvedValueOnce({ items: [row], next_cursor: 'next' }).mockResolvedValueOnce({ items: [row], next_cursor: null })
    const store = useAvatarStore(); await store.loadOwned(); expect(store.ownedComplete).toBe(false)
    await store.loadOwned(true); expect(getOwnedAvatars).toHaveBeenLastCalledWith('next')
    expect(store.owned).toHaveLength(1); expect(store.owned[0].current_item.listed).toBe(false); expect(store.ownedComplete).toBe(true)
  })
  it('rejects a non-progressing ownership cursor rather than loop or claim completeness', async () => {
    vi.mocked(getOwnedAvatars).mockResolvedValueOnce({ items: [], next_cursor: 'same' }).mockResolvedValueOnce({ items: [], next_cursor: 'same' })
    const s = useAvatarStore(); await s.loadOwned(); await s.loadOwned(true)
    expect(s.ownedComplete).toBe(false); expect(s.ownedError).not.toBe('')
  })
  it('requires ownership to be fully loaded before offering a new purchase', async () => {
    const s = useAvatarStore(); expect(await s.buy(hat())).toBe('blocked')
    await s.loadOwned(); expect(await s.buy(hair())).toBe('blocked'); expect(purchaseAvatar).not.toHaveBeenCalled()
  })
  it('requires explicit confirmation, then buys without auto-equipping', async () => {
    const w = view(); await flushPromises(); await w.get('[data-test="avatar-category-hat"]').trigger('click')
    await w.get(`[data-test="avatar-buy-${id(4)}"]`).trigger('click')
    expect(purchaseAvatar).not.toHaveBeenCalled(); expect(w.get('[data-test="avatar-purchase-confirm"]').text()).toContain('20 阳光')
    await w.get('[data-test="avatar-confirm-buy"]').trigger('click'); await flushPromises()
    expect(purchaseAvatar).toHaveBeenCalledWith({ item_id: id(4), expected_version: 1 }, expect.any(String))
    expect(saveAvatarLoadout).not.toHaveBeenCalled(); expect(w.text()).toContain('没有自动穿戴')
  })
  it('keeps the exact purchase request across unknown results, changed catalog and later 4xx', async () => {
    const s = useAvatarStore(); await s.loadOwned()
    vi.mocked(purchaseAvatar).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'lost', 503))
    expect(await s.buy(hat())).toBe('unconfirmed')
    const first = vi.mocked(purchaseAvatar).mock.calls[0]
    expect(await s.buy(item(5, 'hat', false))).toBe('blocked')
    s.catalog = [{ ...hat(), version: 2, price_sunshine: 50 }]
    vi.mocked(purchaseAvatar).mockRejectedValueOnce(new ApiError('AVATAR_ITEM_UNAVAILABLE', 'off', 409))
    expect(await s.retryPurchase()).toBe('unconfirmed'); expect(vi.mocked(purchaseAvatar).mock.calls[1]).toEqual(first)
    const replay = receipt(); replay.replayed = true; replay.current.item.version = 2; replay.current.item.price_sunshine = 50
    vi.mocked(purchaseAvatar).mockResolvedValueOnce(replay)
    expect(await s.retryPurchase()).toBe('done'); expect(vi.mocked(purchaseAvatar).mock.calls[2]).toEqual(first)
    expect(s.notice).toContain('20 阳光'); expect(s.purchaseAction).toBeNull()
  })
  it.each(['INSUFFICIENT_BALANCE', 'AVATAR_VERSION_CONFLICT', 'AVATAR_ALREADY_OWNED', 'AVATAR_ITEM_UNAVAILABLE'])('a first %s is a refusal requiring a new confirmation', async code => {
    const s = useAvatarStore(); await s.loadOwned()
    vi.mocked(purchaseAvatar).mockRejectedValueOnce(new ApiError(code, 'refused', 409))
    expect(await s.buy(hat())).toBe('rejected'); expect(await s.retryPurchase()).toBe('blocked')
    s.dismissPurchase(); expect(s.purchaseAction).toBeNull()
  })
  it('treats malformed successful receipts as unconfirmed, not a successful purchase', async () => {
    const s = useAvatarStore(); await s.loadOwned(); vi.mocked(purchaseAvatar).mockResolvedValueOnce({ ...receipt(), purchase: { ...purchase(), price_sunshine: 99 } })
    expect(await s.buy(hat())).toBe('unconfirmed'); expect(s.notice).toBe('')
  })
  it('blocks duplicate in-flight purchase clicks and discards a late success after an account change', async () => {
    const pending = deferred<unknown>(), s = useAvatarStore(); await s.loadOwned()
    vi.mocked(purchaseAvatar).mockReturnValueOnce(pending.promise); const sending = s.buy(hat())
    expect(await s.retryPurchase()).toBe('blocked'); expect(purchaseAvatar).toHaveBeenCalledTimes(1)
    s.reset(); signIn(501); pending.resolve(receipt()); expect(await sending).toBe('blocked'); expect(s.notice).toBe('')
  })
  it('refreshes the wallet instead of installing an original or replay balance; old reads cannot replace it', async () => {
    const s = useAvatarStore(), wallet = useWalletStore(), pending = deferred<{ wallet: { flower: number; sunshine: number } }>()
    vi.mocked(getWallet).mockReturnValueOnce(pending.promise); const reading = wallet.load()
    await s.loadOwned(); vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 7, sunshine: 60 } })
    await s.buy(hat()); await flushPromises(); pending.resolve({ wallet: { flower: 7, sunshine: 100 } }); await reading
    expect(wallet.balances).toEqual({ flower: 7, sunshine: 60 })
  })
  it('an offline first send is definitely blocked, with no automatic retry', async () => {
    const s = useAvatarStore(); await s.loadOwned(); vi.mocked(purchaseAvatar).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect(await s.buy(hat())).toBe('rejected'); expect(purchaseAvatar).toHaveBeenCalledTimes(1)
  })
  it('captures the whole save request and keeps it after defaults change and a lost answer', async () => {
    const s = useAvatarStore(), request = { expected_version: 0, slots: emptySlots() }
    vi.mocked(saveAvatarLoadout).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'lost', 503))
    expect(await s.save(request)).toBe('unconfirmed'); request.slots.hair = id(99)
    const sent = vi.mocked(saveAvatarLoadout).mock.calls[0]
    vi.mocked(saveAvatarLoadout).mockResolvedValueOnce({ ...loadoutReceipt(), replayed: true })
    expect(await s.retrySave()).toBe('done'); expect(vi.mocked(saveAvatarLoadout).mock.calls[1]).toEqual(sent)
    expect(s.appearance?.loadout_version).toBe(1)
  })
  it('confirmed save with current=null is done and offers only a current-state re-read', async () => {
    const s = useAvatarStore(); await s.loadAppearance(); vi.mocked(saveAvatarLoadout).mockResolvedValueOnce(loadoutReceipt(undefined, null))
    expect(await s.save({ expected_version: 0, slots: emptySlots() })).toBe('done')
    expect(s.saveAction).toBeNull(); expect(s.notice).toContain('保存已确认'); expect(s.appearanceStatus).toBe('error')
    expect(s.appearance).toBeNull()
  })
  it('invalidates reads both before and during the save', async () => {
    const s = useAvatarStore(), old = deferred<unknown>(), writing = deferred<unknown>()
    vi.mocked(getAvatarAppearance).mockReturnValueOnce(old.promise); const reading = s.loadAppearance()
    vi.mocked(saveAvatarLoadout).mockReturnValueOnce(writing.promise); const save = s.save({ expected_version: 0, slots: emptySlots() })
    old.resolve({ appearance: appearance() }); await reading
    writing.resolve(loadoutReceipt()); await save
    expect(s.appearance?.loadout_version).toBe(1)
  })
  it('does not accept a current appearance older than the confirmed save version', async () => {
    const s = useAvatarStore(); vi.mocked(saveAvatarLoadout).mockResolvedValueOnce(loadoutReceipt(undefined, appearance()))
    expect(await s.save({ expected_version: 0, slots: emptySlots() })).toBe('unconfirmed')
  })
  it('a delayed save receipt cannot roll back newer asset metadata on the same loadout version', async () => {
    const s = useAvatarStore(), pending = deferred<unknown>()
    vi.mocked(saveAvatarLoadout).mockReturnValueOnce(pending.promise)
    const writing = s.save({ expected_version: 0, slots: emptySlots() })
    const newer = appearance(1); newer.slots.hair = { ...hair(), version: 3, name: '修正后的名称' }
    vi.mocked(getAvatarAppearance).mockResolvedValueOnce({ appearance: newer }); await s.loadAppearance()
    pending.resolve(loadoutReceipt()); expect(await writing).toBe('done')
    expect(s.appearance?.slots.hair?.version).toBe(3)
    expect(s.appearance?.slots.hair?.name).toBe('修正后的名称')
  })
  it('also discards an old read that started while the save was in flight', async () => {
    const s = useAvatarStore(), sending = deferred<unknown>(), reading = deferred<unknown>()
    vi.mocked(saveAvatarLoadout).mockReturnValueOnce(sending.promise)
    const write = s.save({ expected_version: 0, slots: emptySlots() })
    vi.mocked(getAvatarAppearance).mockReturnValueOnce(reading.promise); const read = s.loadAppearance()
    sending.resolve(loadoutReceipt()); await write; reading.resolve({ appearance: appearance() }); await read
    expect(s.appearance?.loadout_version).toBe(1); expect(s.appearanceStatus).toBe('ready')
  })
  it('retains the captured save after an unknown send followed by a conflict', async () => {
    const s = useAvatarStore()
    vi.mocked(saveAvatarLoadout).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'lost', 503))
    await s.save({ expected_version: 0, slots: emptySlots() })
    vi.mocked(saveAvatarLoadout).mockRejectedValueOnce(new ApiError('AVATAR_VERSION_CONFLICT', 'stale', 409))
    expect(await s.retrySave()).toBe('unconfirmed')
    expect(vi.mocked(saveAvatarLoadout).mock.calls[1]).toEqual(vi.mocked(saveAvatarLoadout).mock.calls[0])
    s.dismissSave(); expect(s.saveAction).not.toBeNull()
  })
  it('does not install private catalog/ownership answers for a reset identity', async () => {
    const s = useAvatarStore(), c = deferred<unknown>(), o = deferred<unknown>()
    vi.mocked(getAvatarCatalog).mockReturnValueOnce(c.promise); vi.mocked(getOwnedAvatars).mockReturnValueOnce(o.promise)
    const cr = s.loadCatalog(), or = s.loadOwned(); s.reset(); signIn(501)
    c.resolve({ items: [hat()] }); o.resolve({ items: [{ purchase: purchase(), current_item: { ...hat(), listed: true } }], next_cursor: null })
    await Promise.all([cr, or]); expect(s.catalog).toEqual([]); expect(s.owned).toEqual([])
  })
  it('does not persist private purchase or save state', async () => {
    const storage = vi.spyOn(Storage.prototype, 'setItem'), s = useAvatarStore()
    await s.loadOwned(); await s.buy(hat()); await s.save({ expected_version: 0, slots: emptySlots() }); await flushPromises()
    expect(storage).not.toHaveBeenCalled()
  })
  it('a late save cannot install the old user\'s appearance', async () => {
    const s = useAvatarStore(), pending = deferred<unknown>(); vi.mocked(saveAvatarLoadout).mockReturnValueOnce(pending.promise)
    const writing = s.save({ expected_version: 0, slots: emptySlots() }); s.reset(); signIn(501)
    pending.resolve(loadoutReceipt()); expect(await writing).toBe('blocked'); expect(s.appearance).toBeNull()
  })
})

describe('avatar UI and public-art boundary', () => {
  it('empty registry/catalog shows no invented purchasable product', async () => {
    vi.mocked(getAvatarCatalog).mockResolvedValueOnce({ items: [] }); const w = view(); await flushPromises()
    expect(w.get('[data-test="avatar-empty-catalog"]').text()).toContain('不会生成'); expect(w.findAll('[data-test="avatar-shop-item"]')).toHaveLength(0)
  })
  it('shows starters without purchase controls and unlisted owned items as wearable', async () => {
    vi.mocked(getOwnedAvatars).mockResolvedValue({ items: [{ purchase: purchase(), current_item: { ...hat(), listed: false } }], next_cursor: null })
    const w = view('wardrobe'); await flushPromises()
    expect(w.get('[data-test="avatar-slot-hair"]').text()).toContain('免费初始')
    await w.get('[data-test="avatar-category-hat"]').trigger('click')
    expect(w.get('[data-test="avatar-slot-hat"]').text()).toContain('已下架，仍可穿戴')
    expect(w.get('[data-test="avatar-slot-hat"]').text()).not.toContain('装扮99')
  })
  it('does not offer unowned paid items in the wardrobe', async () => {
    const w = view('wardrobe'); await flushPromises()
    expect(w.get('[data-test="avatar-slot-hair"]').text()).toContain(hair().name)
    await w.get('[data-test="avatar-category-hat"]').trigger('click')
    expect(w.get('[data-test="avatar-slot-hat"]').text()).not.toContain(hat().name)
  })
  it('keeps the known unlisted flag when the equipped item read omits that optional field', async () => {
    vi.mocked(getAvatarCatalog).mockResolvedValue({ items: [hair(), clothes(), expression()] })
    vi.mocked(getOwnedAvatars).mockResolvedValue({ items: [{ purchase: purchase(), current_item: { ...hat(), listed: false } }], next_cursor: null })
    const a = appearance(1); a.slots.hat = hat()
    vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: a })
    const w = view('wardrobe'); await flushPromises()
    await w.get('[data-test="avatar-category-hat"]').trigger('click')
    expect(w.get('[data-test="avatar-slot-hat"]').text()).toContain('已下架，仍可穿戴')
  })
  it('does not submit account A\'s confirmation after switching identity', async () => {
    const w = view(); await flushPromises(); await w.get('[data-test="avatar-category-hat"]').trigger('click')
    await w.get(`[data-test="avatar-buy-${id(4)}"]`).trigger('click')
    signIn(501)
    await w.get('[data-test="avatar-confirm-buy"]').trigger('click'); await flushPromises()
    expect(purchaseAvatar).not.toHaveBeenCalled()
  })
  it('tries on a shop item without purchasing it or changing the saved appearance', async () => {
    const w = view(); await flushPromises()
    await w.get('[data-test="avatar-category-hat"]').trigger('click')
    await w.get(`[data-test="avatar-try-${id(4)}"]`).trigger('click')
    expect(w.get('.avatar-stage').text()).toContain(hat().name)
    expect(w.get(`[data-test="avatar-try-${id(4)}"]`).attributes('aria-pressed')).toBe('true')
    expect(purchaseAvatar).not.toHaveBeenCalled()
    expect(saveAvatarLoadout).not.toHaveBeenCalled()
    expect(useAvatarStore().appearance?.slots.hat).toBeNull()
  })
  it('preview changes are local; saving sends six slots with the captured base', async () => {
    vi.mocked(getOwnedAvatars).mockResolvedValue({ items: [{ purchase: purchase(), current_item: { ...hat(), listed: true } }], next_cursor: null })
    const w = view('wardrobe'); await flushPromises()
    await w.get('[data-test="avatar-category-hat"]').trigger('click')
    await w.get(`[data-test="avatar-wear-${id(4)}"]`).trigger('click')
    expect(saveAvatarLoadout).not.toHaveBeenCalled()
    const req: LoadoutRequest = { expected_version: 0, slots: { hair: id(1), clothes: id(2), expression: id(3), hat: id(4), handheld: null, accessory: null } }
    const current = appearance(1); current.slots.hat = hat(); vi.mocked(saveAvatarLoadout).mockResolvedValueOnce(loadoutReceipt(req, current))
    await w.get('[data-test="avatar-save"]').trigger('click'); await flushPromises()
    expect(saveAvatarLoadout).toHaveBeenCalledWith(req, expect.any(String)); expect(w.text()).toContain('第 1 版形象保存已确认')
  })
  it('previews a role independently and saves it with the same six equipped slots', async () => {
    const w = view('wardrobe'); await flushPromises()
    await w.get('[data-test="avatar-gender-male"]').trigger('click')
    expect(w.get('[data-test="avatar-gender-male"]').attributes('aria-pressed')).toBe('true')
    const layers = w.get('.avatar-stage .avatar-layers')
    expect(layers.attributes('data-gender')).toBe('male')
    expect(layers.classes()).toContain('avatar-layers--focus-head')
    await w.get('[data-test="avatar-category-clothes"]').trigger('click')
    expect(layers.classes()).not.toContain('avatar-layers--focus-head')
    await w.get('[data-test="avatar-category-expression"]').trigger('click')
    expect(layers.classes()).toContain('avatar-layers--focus-head')
    expect(saveAvatarLoadout).not.toHaveBeenCalled()
    const req: LoadoutRequest = { expected_version: 0, character_gender: 'male',
      slots: { hair: id(1), clothes: id(2), expression: id(3), hat: null, handheld: null, accessory: null } }
    vi.mocked(saveAvatarLoadout).mockResolvedValueOnce(loadoutReceipt(req, { ...appearance(1), character_gender: 'male' }))
    await w.get('[data-test="avatar-save"]').trigger('click'); await flushPromises()
    expect(saveAvatarLoadout).toHaveBeenCalledWith(req, expect.any(String))
    expect(useAvatarStore().appearance?.character_gender).toBe('male')
  })
  it('shows a clean bald preview and saves it without removing the equipped hair item', async () => {
    const curl = { ...hair(), id: id(8), asset: { ...hair().asset, key: 'hair.placeholder_wave.v1' } }
    vi.mocked(getAvatarCatalog).mockResolvedValueOnce({ items: [hair(), curl, clothes(), expression(), hat()] })
    const w = view('wardrobe'); await flushPromises()
    await w.get('[data-test="avatar-gender-male"]').trigger('click')
    const stage = w.get('.avatar-stage')
    expect(stage.get('[data-layer="body"]').attributes('src')).toBe('/avatars/' + manifest.bases.male.file)
    expect(stage.get('[data-layer="hair:front"]').attributes('src')).toBe(itemThumbnail(hair()))
    expect(w.get(`[data-test="avatar-wear-${id(1)}"] img`).attributes('src')).toBe(itemThumbnail(hair()))
    expect(w.get('[data-test="avatar-bald"] img').attributes('src')).toBe('/avatars/' + manifest.bases.male.file)
    await w.get('[data-test="avatar-bald"]').trigger('click')
    expect(stage.findAll('[data-layer^="hair:"]')).toHaveLength(0)
    await w.get('[data-test="avatar-bald"]').trigger('click')
    expect(stage.get('[data-layer="hair:front"]').attributes('src')).toBe(itemThumbnail(hair()))
    await w.get('[data-test="avatar-bald"]').trigger('click')
    expect(stage.findAll('[data-layer^="hair:"]')).toHaveLength(0)
    expect(saveAvatarLoadout).not.toHaveBeenCalled()
    const req: LoadoutRequest = { expected_version: 0, character_gender: 'male', hair_hidden: true,
      slots: { hair: id(1), clothes: id(2), expression: id(3), hat: null, handheld: null, accessory: null } }
    vi.mocked(saveAvatarLoadout).mockResolvedValueOnce(loadoutReceipt(req, { ...appearance(1), character_gender: 'male', hair_hidden: true }))
    await w.get('[data-test="avatar-save"]').trigger('click'); await flushPromises()
    expect(saveAvatarLoadout).toHaveBeenCalledWith(req, expect.any(String))
    expect(useAvatarStore().appearance?.hair_hidden).toBe(true)
    await w.get(`[data-test="avatar-wear-${curl.id}"]`).trigger('click')
    expect(stage.get('[data-layer="hair:front"]').attributes('src')).toBe(itemThumbnail(curl))
  })
  it('uses catalog thumbnails and offers the hair category to a male appearance', async () => {
    vi.mocked(getAvatarAppearance).mockResolvedValueOnce({ appearance: { ...appearance(), character_gender: 'male' } })
    const w = view(); await flushPromises()
    const card = w.get(`[data-test="avatar-try-${id(1)}"]`)
    expect(card.findAll('img')).toHaveLength(1)
    expect(card.get('img').attributes('src')).toBe(itemThumbnail(hair()))
    expect(card.attributes('data-category')).toBe('hair')
    expect(card.attributes('data-art-format')).toBe(hair().asset.format)
  })
  it('preserves a conflict draft and requires explicit acceptance of a newer base', async () => {
    const w = view('wardrobe'); await flushPromises(); await w.get('[data-test="avatar-use-default"]').trigger('click')
    vi.mocked(saveAvatarLoadout).mockRejectedValueOnce(new ApiError('AVATAR_VERSION_CONFLICT', 'stale', 409))
    await w.get('[data-test="avatar-save"]').trigger('click'); await flushPromises()
    vi.mocked(getAvatarAppearance).mockResolvedValueOnce({ appearance: appearance(4) })
    await w.get('[data-test="avatar-review-current"]').trigger('click'); await flushPromises()
    expect(w.get('[data-test="avatar-save"]').attributes('disabled')).toBeDefined()
    expect(w.get('[data-test="avatar-use-default"]').attributes('aria-pressed')).toBe('true')
    await w.get('[data-test="avatar-accept-base"]').trigger('click')
    expect(w.get('[data-test="avatar-save"]').attributes('disabled')).toBeUndefined()
  })
  it('missing configuration is labelled honestly and never presented as a saved character', async () => {
    vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: { ...appearance(), complete: false, starter_version: null, slots: emptyFigureSlots() } })
    const w = view('wardrobe'); await flushPromises(); expect(w.text()).toContain('初始配置尚未完成')
    expect(w.text()).not.toContain('已保存第 0 版')
    vi.mocked(saveAvatarLoadout).mockRejectedValueOnce(new ApiError('AVATAR_DEFAULTS_UNAVAILABLE', 'missing', 409))
    await w.get('[data-test="avatar-save"]').trigger('click'); await flushPromises()
    expect(w.text()).toContain('尚未配置完整')
  })
  it('home has character navigation and distinguishes a fallback from confirmed current appearance', async () => {
    const w = mount(AvatarHome, { global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } }); wrappers.push(w)
    expect(w.text()).toContain('当前形象未确认'); expect(w.text()).toContain('装扮商店')
    await useAvatarStore().loadAppearance(); await flushPromises()
    expect(w.text()).toContain('当前形象')
    expect(w.find('.avatar-layers img[data-layer="body"]').exists()).toBe(true)
  })
  it('back hair is behind the body and a covering hat chooses under-hat hair', () => {
    const slots = appearance().slots; slots.hat = hat()
    const result = figureLayers(slots), names = result.layers.map(l => l.key)
    expect(names.indexOf('hair:back')).toBeLessThan(names.indexOf('body'))
    expect(names).toContain('hair:front_under_hat'); expect(names).not.toContain('hair:front')
    expect(names.indexOf('hair:front_under_hat')).toBeLessThan(names.indexOf('hat:main'))
  })
  it('flips the whole layer stack by room side independently of gender and composes head focus', async () => {
    const w = mount(AvatarFigure, { props: { slots: appearance().slots, character: 'right', gender: 'female', focus: 'head' } })
    wrappers.push(w)
    const layers = w.get('.avatar-layers')
    expect(layers.classes()).toContain('avatar-layers--flipped')
    expect(layers.classes()).toContain('avatar-layers--focus-head')
    expect(layers.attributes('data-gender')).toBe('female')
    const body = () => layers.get('[data-layer="body"]').attributes('src')
    expect(body()).toBe('/avatars/' + manifest.bases.female.file)
    await w.setProps({ character: 'left', gender: 'male', focus: 'full' })
    expect(layers.classes()).toEqual(['avatar-layers'])
    expect(body()).toBe('/avatars/' + manifest.bases.male.file)
    expect(figureLayers(emptyFigureSlots()).layers[0]?.src).toBe('/avatars/' + manifest.base.file)
  })
  it('unknown keys do not become URLs; image failure renders a stable fallback without changing saved ids', async () => {
    const unknown = { ...hair(), asset: { ...hair().asset, key: 'hair.unknown.v1' } }
    expect(itemThumbnail(unknown)).toBeNull()
    const slots = { ...appearance().slots, hair: unknown }
    const w = mount(AvatarFigure, { props: { slots } }); wrappers.push(w)
    expect(w.findAll('img').every(img => img.attributes('src')?.startsWith('/avatars/') === true)).toBe(true)
    expect(w.text()).toContain('部分素材不可用')
    await w.get('img').trigger('error'); expect(slots.hair.id).toBe(id(1)); expect(w.text()).toContain('保存的装扮没有改变')
  })
  it('checks manifest hashes, shared WebP canvas and safe legacy SVG contents', () => {
    const files = [manifest.base, ...Object.values(manifest.bases), ...Object.values(manifest.assets).flatMap(a => Object.values(a.layers))]
    expect(new Set(files.map(f => f.file)).size).toBe(14)
    for (const f of files) {
      const bytes = readFileSync(resolve('public/avatars', f.file))
      expect(createHash('sha256').update(bytes).digest('hex')).toBe(f.sha256)
      if (f.file.endsWith('.webp')) {
        expect(bytes.toString('ascii', 0, 4)).toBe('RIFF')
        expect(bytes.toString('ascii', 8, 16)).toBe('WEBPVP8L')
        const header = bytes.readUInt32LE(21)
        expect(bytes[20]).toBe(0x2f)
        expect([1 + (header & 0x3fff), 1 + ((header >>> 14) & 0x3fff)]).toEqual(manifest.canvas)
        expect((header >>> 28) & 1).toBe(1)
      } else {
        expect(bytes.toString()).toContain('viewBox="0 0 512 512"')
        expect(bytes.toString()).not.toMatch(/<script|foreignObject|\bonload=/i)
      }
    }
  })
  it('keeps private avatar APIs outside the service-worker cache and public art outside precache', async () => {
    const { isCacheableRequest } = await import('@/pwa/cache-policy')
    for (const p of ['/api/v1/avatar', '/api/v1/avatar/owned', '/api/v1/avatar/catalog', '/api/v1/avatar/purchases']) expect(isCacheableRequest(p, 'GET')).toBe(false)
    expect(isCacheableRequest('/avatars/' + manifest.base.file, 'GET', { sameOrigin: true, destination: 'image' })).toBe(true)
    expect(readFileSync(resolve('vite.config.ts'), 'utf8')).toContain("'**/avatars/**'")
  })
})
