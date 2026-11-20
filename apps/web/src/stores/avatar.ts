/** B4 private state is memory-only. Reads never settle uncertain writes; retries keep exact captured terms. */
import { computed, ref } from 'vue'
import { defineStore } from 'pinia'
import { ApiError, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { newIdempotencyKey } from '@/api/tasks'
import { getAvatarAppearance, getAvatarCatalog, getOwnedAvatars, purchaseAvatar, saveAvatarLoadout,
  isAppearance, isCatalog, isOwnedPage, isPurchaseResponse, isLoadoutResponse,
  type Appearance, type AvatarItem, type OwnedAvatar, type LoadoutRequest } from '@/api/avatar'
import { useSessionStore } from './session'
import { useWalletStore } from './wallet'

type ReadStatus = 'idle' | 'loading' | 'ready' | 'error'
interface Action { key: string; status: 'sending' | 'unconfirmed' | 'rejected'; unresolved: boolean; error: string }
export interface AvatarPurchaseAction extends Action { item: AvatarItem }
export interface AvatarSaveAction extends Action { request: LoadoutRequest }
export type AvatarWriteResult = 'done' | 'rejected' | 'unconfirmed' | 'blocked'
const malformed = () => new Error('服务器的装扮响应无法确认，请使用原请求重试。')
const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T
function definite(error: unknown): boolean {
  return error instanceof OfflineMutationBlockedError ||
    (error instanceof ApiError && error.status >= 400 && error.status < 500 && error.code !== 'RESULT_UNKNOWN')
}

export const useAvatarStore = defineStore('avatar', () => {
  const catalog = ref<AvatarItem[]>([])
  const catalogStatus = ref<ReadStatus>('idle')
  const catalogError = ref('')
  const owned = ref<OwnedAvatar[]>([])
  const ownedStatus = ref<ReadStatus>('idle')
  const ownedError = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const appearance = ref<Appearance | null>(null)
  const appearanceStatus = ref<ReadStatus>('idle')
  const appearanceError = ref('')
  const confirmedVersion = ref(0)
  const purchaseAction = ref<AvatarPurchaseAction | null>(null)
  const saveAction = ref<AvatarSaveAction | null>(null)
  const notice = ref('')
  let generation = 0
  let catalogSeq = 0, ownedSeq = 0, appearanceSeq = 0
  const owner = () => {
    const session = useSessionStore()
    return session.isAuthenticated ? session.user?.id ?? null : null
  }
  const token = () => ({ generation, owner: owner() })
  const current = (t: ReturnType<typeof token>) => !!t.owner && generation === t.generation && owner() === t.owner
  const owns = (id: string) => owned.value.some(row => row.purchase.item_id === id)
  const ownedComplete = computed(() => ownedStatus.value === 'ready' && !nextCursor.value)
  const purchaseBusy = computed(() => !!purchaseAction.value && purchaseAction.value.status !== 'rejected')
  const saveBusy = computed(() => !!saveAction.value && saveAction.value.status !== 'rejected')
  function reset(): void {
    generation++; catalogSeq++; ownedSeq++; appearanceSeq++
    catalog.value = []; owned.value = []; appearance.value = null
    catalogStatus.value = ownedStatus.value = appearanceStatus.value = 'idle'
    catalogError.value = ownedError.value = appearanceError.value = ''
    nextCursor.value = null; loadingMore.value = false; confirmedVersion.value = 0
    purchaseAction.value = null; saveAction.value = null; notice.value = ''
  }
  async function loadCatalog(): Promise<void> {
    const t = token(), seq = ++catalogSeq
    if (!t.owner) return
    catalogStatus.value = 'loading'; catalogError.value = ''
    try {
      const response = await getAvatarCatalog()
      if (!current(t) || seq !== catalogSeq) return
      if (!isCatalog(response)) throw malformed()
      catalog.value = response.items; catalogStatus.value = 'ready'
    } catch (error) {
      if (!current(t) || seq !== catalogSeq) return
      catalogStatus.value = 'error'; catalogError.value = describeError(error)
    }
  }
  function applyAppearance(value: Appearance): boolean {
    if (value.loadout_version < Math.max(confirmedVersion.value, appearance.value?.loadout_version ?? 0)) return false
    // The loadout version does not advance when an administrator corrects an item's metadata.
    // Keep the newest already-observed revision of an identical item, even on a delayed save receipt.
    const next = { ...value, slots: { ...value.slots } }
    for (const slot of ['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory'] as const) {
      const incoming = next.slots[slot]
      const known = appearance.value?.slots[slot]
      if (incoming && known?.id === incoming.id && known.version > incoming.version) next.slots[slot] = known
    }
    appearance.value = next; appearanceStatus.value = 'ready'; appearanceError.value = ''
    confirmedVersion.value = Math.max(confirmedVersion.value, value.loadout_version)
    return true
  }
  async function loadAppearance(): Promise<void> {
    const t = token(), seq = ++appearanceSeq
    if (!t.owner) return
    appearanceStatus.value = 'loading'; appearanceError.value = ''
    try {
      const response = await getAvatarAppearance()
      if (!current(t) || seq !== appearanceSeq) return
      const value = response && typeof response === 'object' ? (response as { appearance?: unknown }).appearance : null
      if (!isAppearance(value) || !applyAppearance(value)) throw malformed()
    } catch (error) {
      if (!current(t) || seq !== appearanceSeq) return
      appearanceStatus.value = 'error'; appearanceError.value = describeError(error)
    }
  }
  async function loadOwned(more = false): Promise<void> {
    if (more && (!nextCursor.value || loadingMore.value || ownedStatus.value !== 'ready')) return
    const t = token(), seq = ++ownedSeq, cursor = more ? nextCursor.value : null
    if (!t.owner) return
    if (more) loadingMore.value = true
    else { ownedStatus.value = 'loading'; loadingMore.value = false }
    ownedError.value = ''
    try {
      const response = await getOwnedAvatars(cursor)
      if (!current(t) || seq !== ownedSeq) return
      if (!isOwnedPage(response) || (more && response.next_cursor === cursor)) throw malformed()
      const items = more ? [...owned.value, ...response.items] : response.items
      owned.value = [...new Map(items.map(i => [i.purchase.item_id, i])).values()]
      nextCursor.value = response.next_cursor; ownedStatus.value = 'ready'; loadingMore.value = false
    } catch (error) {
      if (!current(t) || seq !== ownedSeq) return
      if (!more) ownedStatus.value = 'error'
      ownedError.value = describeError(error); loadingMore.value = false
    }
  }
  function readAll(): void {
    void loadCatalog(); void loadOwned(); void loadAppearance(); void useWalletStore().load()
  }
  async function sendPurchase(action: AvatarPurchaseAction): Promise<AvatarWriteResult> {
    if (purchaseAction.value?.key !== action.key || action.status === 'sending') return 'blocked'
    const t = token()
    if (!t.owner) return 'blocked'
    action.status = 'sending'; action.error = ''
    try {
      const response = await purchaseAvatar({ item_id: action.item.id, expected_version: action.item.version }, action.key)
      if (!current(t) || purchaseAction.value?.key !== action.key) return 'blocked'
      if (!isPurchaseResponse(response, action.item)) throw malformed()
      const p = response.purchase
      notice.value = `这笔购买已确认：${p.name}（第 ${p.item_version} 版），扣除 ${p.debited_sunshine} 阳光，原交易后余额 ${p.sunshine_balance_after}。没有自动穿戴；当前余额以重新读取为准。`
      purchaseAction.value = null
      // Never write the historical post-debit balance or a replay's current field into the wallet store.
      void useWalletStore().load(); void loadOwned(); void loadCatalog()
      return 'done'
    } catch (error) {
      if (!current(t) || purchaseAction.value?.key !== action.key) return 'blocked'
      action.error = describeError(error)
      if (!action.unresolved && definite(error)) { action.status = 'rejected'; return 'rejected' }
      action.unresolved = true; action.status = 'unconfirmed'; return 'unconfirmed'
    }
  }
  async function buy(item: AvatarItem): Promise<AvatarWriteResult> {
    if (!owner() || purchaseAction.value || item.starter || owns(item.id) || !ownedComplete.value) return 'blocked'
    purchaseAction.value = { key: newIdempotencyKey(), item: clone(item), status: 'unconfirmed', unresolved: false, error: '' }
    return sendPurchase(purchaseAction.value)
  }
  async function retryPurchase(): Promise<AvatarWriteResult> {
    const action = purchaseAction.value
    return action?.status === 'unconfirmed' ? sendPurchase(action) : 'blocked'
  }
  function dismissPurchase(): void {
    if (purchaseAction.value?.status === 'rejected') purchaseAction.value = null
  }
  async function sendSave(action: AvatarSaveAction): Promise<AvatarWriteResult> {
    if (saveAction.value?.key !== action.key || action.status === 'sending') return 'blocked'
    const t = token()
    if (!t.owner) return 'blocked'
    action.status = 'sending'; action.error = ''
    appearanceSeq++ // Invalidate a read that began before this write.
    if (appearanceStatus.value === 'loading') appearanceStatus.value = appearance.value ? 'ready' : 'idle'
    try {
      const response = await saveAvatarLoadout(clone(action.request), action.key)
      if (!current(t) || saveAction.value?.key !== action.key) return 'blocked'
      if (!isLoadoutResponse(response, action.request)) throw malformed()
      appearanceSeq++ // A read started during the write must not roll its confirmed result back.
      confirmedVersion.value = Math.max(confirmedVersion.value, response.loadout.version)
      notice.value = `第 ${response.loadout.version} 版形象保存已确认。历史保存结果不代表之后没有其他修改。`
      saveAction.value = null
      if (response.current) applyAppearance(response.current.appearance)
      else if (!appearance.value || appearance.value.loadout_version < confirmedVersion.value) {
        appearance.value = null; appearanceStatus.value = 'error'
        appearanceError.value = '保存已确认，但当前形象读取失败。请重新读取，不必再保存。'
      }
      return 'done'
    } catch (error) {
      if (!current(t) || saveAction.value?.key !== action.key) return 'blocked'
      action.error = describeError(error)
      if (!action.unresolved && definite(error)) { action.status = 'rejected'; return 'rejected' }
      action.unresolved = true; action.status = 'unconfirmed'; return 'unconfirmed'
    }
  }
  async function save(request: LoadoutRequest): Promise<AvatarWriteResult> {
    if (!owner() || saveAction.value) return 'blocked'
    saveAction.value = { key: newIdempotencyKey(), request: clone(request), status: 'unconfirmed', unresolved: false, error: '' }
    return sendSave(saveAction.value)
  }
  async function retrySave(): Promise<AvatarWriteResult> {
    const action = saveAction.value
    return action?.status === 'unconfirmed' ? sendSave(action) : 'blocked'
  }
  function dismissSave(): void { if (saveAction.value?.status === 'rejected') saveAction.value = null }
  return { catalog, catalogStatus, catalogError, owned, ownedStatus, ownedError, nextCursor, loadingMore,
    appearance, appearanceStatus, appearanceError, confirmedVersion, purchaseAction, saveAction, notice,
    owns, ownedComplete, purchaseBusy, saveBusy, reset, readAll, loadCatalog, loadOwned, loadAppearance,
    buy, retryPurchase, dismissPurchase, save, retrySave, dismissSave }
})
