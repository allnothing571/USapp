<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { AVATAR_SLOTS, emptySlots, type AvatarItem, type AvatarSlot, type SlotIds } from '@/api/avatar'
import { SLOT_LABEL, itemThumbnail, emptyFigureSlots, type FigureSlots } from '@/api/avatar-art'
import AvatarFigure from '@/components/AvatarFigure.vue'
import { useAvatarStore } from '@/stores/avatar'
import { useSessionStore } from '@/stores/session'
import { useWalletStore } from '@/stores/wallet'

withDefaults(defineProps<{ tab?: 'shop' | 'wardrobe' }>(), { tab: 'shop' })
const store = useAvatarStore(), session = useSessionStore(), wallet = useWalletStore()
const category = ref<AvatarSlot>('hair')
const draft = ref<SlotIds>(emptySlots())
const draftVersion = ref<number | null>(null)
const dirty = ref(false)
const confirmation = ref<AvatarItem | null>(null)
const confirmationOwner = ref<string | null>(null)
const draftOwner = ref<string | null>(null)
const failedThumbs = ref<string[]>([])
function syncDraft(): void {
  if (!store.appearance || store.appearanceStatus !== 'ready') return
  draft.value = Object.fromEntries(AVATAR_SLOTS.map(s => [s, store.appearance?.slots[s]?.id ?? null])) as SlotIds
  draftVersion.value = store.appearance.loadout_version
  draftOwner.value = session.user?.id ?? null
  dirty.value = false
}
watch([() => session.user?.id, () => session.isAuthenticated], ([id, authenticated]) => {
  draft.value = emptySlots(); draftVersion.value = null; dirty.value = false; confirmation.value = null; failedThumbs.value = []
  draftOwner.value = null; confirmationOwner.value = null
  if (id && authenticated) store.readAll()
}, { immediate: true })
watch(() => store.appearance, () => { if (!dirty.value && !store.saveBusy) syncDraft() })
const available = computed(() => {
  const items = new Map<string, AvatarItem>()
    const remember = (item: AvatarItem) => {
      const previous = items.get(item.id)
      if (!previous || previous.version < item.version) items.set(item.id, item)
      else if (previous.version === item.version) items.set(item.id, { ...previous, ...item })
    }
  store.catalog.filter(i => i.starter || store.owns(i.id)).forEach(remember)
  store.owned.forEach(row => remember(row.current_item))
  if (store.appearance) AVATAR_SLOTS.forEach(s => { const i = store.appearance?.slots[s]; if (i) remember(i) })
  return [...items.values()]
})
const preview = computed<FigureSlots>(() => Object.fromEntries(AVATAR_SLOTS.map(s =>
  [s, available.value.find(i => i.id === draft.value[s] && i.category === s) ?? null])) as FigureSlots)
const options = (slot: AvatarSlot) => available.value.filter(i => i.category === slot)
const visibleCatalog = computed(() => store.catalog.filter(i => i.category === category.value))
const changedBase = computed(() => dirty.value && store.appearanceStatus === 'ready' && store.appearance &&
  draftVersion.value !== null && store.appearance.loadout_version !== draftVersion.value)
const defaultsPending = computed(() => ['hair', 'clothes', 'expression'].some(s => draft.value[s as AvatarSlot] === null))
const purchaseLocked = computed(() => !!store.purchaseAction)
function askBuy(item: AvatarItem): void {
  if (purchaseLocked.value || !store.ownedComplete || wallet.status !== 'ready') return
  confirmation.value = JSON.parse(JSON.stringify(item)) as AvatarItem
  confirmationOwner.value = session.user?.id ?? null
}
async function confirmBuy(): Promise<void> {
  const item = confirmation.value
  confirmation.value = null
  if (item && session.user?.id === confirmationOwner.value) await store.buy(item)
}
async function save(): Promise<void> {
  if (draftVersion.value === null || store.saveAction || draftOwner.value !== session.user?.id) return
  const result = await store.save({ expected_version: draftVersion.value, slots: { ...draft.value } })
  if (result === 'done') { dirty.value = false; syncDraft() }
}
async function retrySave(): Promise<void> {
  if (await store.retrySave() === 'done') { dirty.value = false; syncDraft() }
}
async function reviewCurrent(): Promise<void> {
  dirty.value = true // A read cannot replace a draft after a conflict/refusal.
  store.dismissSave()
  await store.loadAppearance()
  void store.loadOwned(); void store.loadCatalog()
}
function acceptNewBase(): void {
  if (!store.appearance || store.appearanceStatus !== 'ready') return
  draftVersion.value = store.appearance.loadout_version
  dirty.value = true
}
</script>

<template>
  <section class="page stack avatar-page" data-test="avatar-page">
    <header class="page__header"><h1 class="page__title">{{ tab === 'shop' ? '装扮商店' : '我的衣柜' }}</h1>
      <p class="page__subtitle">用阳光挑选装扮。这里使用开发占位素材，正式画风尚未确定。</p></header>
    <nav class="row" aria-label="装扮导航"><RouterLink :to="{ name: 'avatar-shop' }">商店</RouterLink>
      <RouterLink :to="{ name: 'avatar-wardrobe' }">衣柜</RouterLink><RouterLink :to="{ name: 'home' }">首页</RouterLink></nav>
    <p v-if="store.notice" class="hint" role="status" data-test="avatar-notice">{{ store.notice }}</p>
    <div class="card stack" data-test="avatar-wallet">
      <p v-if="wallet.status === 'ready' && wallet.balances">我的阳光：<strong>{{ wallet.balances.sunshine }}</strong> · 装扮只使用阳光，不消耗小红花。</p>
      <p v-else-if="wallet.status === 'loading'" class="card__hint">正在读取我的阳光…</p>
      <p v-else class="hint hint--error">{{ wallet.errorText || '尚未读取余额。' }}</p>
      <button type="button" class="btn btn--small btn--soft" @click="wallet.load()">刷新我的余额</button>
    </div>

    <div v-if="store.purchaseAction" class="card stack" data-test="avatar-purchase-action" role="status">
      <p>这次购买：{{ store.purchaseAction.item.name }}，第 {{ store.purchaseAction.item.version }} 版，{{ store.purchaseAction.item.price_sunshine }} 阳光。</p>
      <p>{{ store.purchaseAction.status === 'sending' ? '正在提交…' : store.purchaseAction.error }}</p>
      <template v-if="store.purchaseAction.status === 'unconfirmed'">
        <p class="card__hint">结果未确认。保留了原价格、版本和请求；不会自动重买。刷新页面会丢失此待确认记录。</p>
        <button type="button" class="btn" data-test="avatar-purchase-retry" @click="store.retryPurchase()">使用原请求重试</button>
      </template>
      <button v-if="store.purchaseAction.status === 'rejected'" type="button" class="btn btn--soft" data-test="avatar-purchase-dismiss"
        @click="store.dismissPurchase(); store.loadCatalog(); store.loadOwned(); wallet.load()">关闭这次拒绝并刷新，重新选择</button>
    </div>
    <div v-if="store.saveAction" class="card stack" data-test="avatar-save-action" role="status">
      <p>这次保存基于第 {{ store.saveAction.request.expected_version }} 版。</p>
      <p>{{ store.saveAction.status === 'sending' ? '正在保存…' : store.saveAction.error }}</p>
      <template v-if="store.saveAction.status === 'unconfirmed'">
        <p class="card__hint">原请求已保留。若原保存成功，重试会返回原结果；若未成功，仍会尝试该请求。默认项以实际成功时解析的配置为准，不会绕过版本检查。</p>
        <button type="button" class="btn" data-test="avatar-save-retry" @click="retrySave()">使用原请求重试保存</button>
      </template>
      <button v-if="store.saveAction.status === 'rejected'" type="button" class="btn btn--soft" data-test="avatar-review-current" @click="reviewCurrent()">保留草稿，重新读取当前形象</button>
    </div>

    <div v-if="tab === 'shop'" class="stack">
      <div class="row avatar-tabs" role="group" aria-label="装扮分类">
        <button v-for="slot in AVATAR_SLOTS" :key="slot" type="button" class="btn btn--small btn--soft" :aria-pressed="category === slot"
          :data-test="`avatar-category-${slot}`" @click="category = slot">{{ SLOT_LABEL[slot] }}</button>
      </div>
      <p v-if="store.catalogStatus === 'loading'" class="card__hint">正在读取商店…</p>
      <div v-else-if="store.catalogStatus === 'error'" class="card stack"><p class="hint hint--error">{{ store.catalogError }}</p>
        <button type="button" class="btn" @click="store.loadCatalog()">重新读取商店</button></div>
      <template v-else-if="store.catalogStatus === 'ready'">
        <p v-if="!store.catalog.length" class="card card__hint" data-test="avatar-empty-catalog">目前没有上架装扮，目录可能尚未配置。不会生成可购买的演示商品。</p>
        <p v-else-if="!visibleCatalog.length" class="card__hint">这个分类暂时没有上架装扮。</p>
        <div class="avatar-item-grid">
          <article v-for="item in visibleCatalog" :key="item.id" class="card stack" data-test="avatar-shop-item">
            <div class="avatar-thumbnail"><img v-if="itemThumbnail(item) && !failedThumbs.includes(item.asset.key)" :src="itemThumbnail(item)!" alt="" @error="failedThumbs.push(item.asset.key)" />
              <span v-else class="card__hint">素材暂不可用</span></div>
            <h2 class="card__title">{{ item.name }}</h2>
            <p class="list__meta">{{ SLOT_LABEL[item.category] }} · 第 {{ item.version }} 版 · {{ item.starter ? '免费初始装扮' : `${item.price_sunshine} 阳光` }}</p>
            <RouterLink v-if="item.starter || store.owns(item.id)" :to="{ name: 'avatar-wardrobe' }">{{ item.starter ? '去衣柜选择' : '已拥有，去衣柜穿戴' }}</RouterLink>
            <button v-else type="button" class="btn" :disabled="purchaseLocked || !store.ownedComplete || wallet.status !== 'ready'"
              :data-test="`avatar-buy-${item.id}`" @click="askBuy(item)">购买前确认</button>
          </article>
        </div>
      </template>
    </div>

    <div class="card stack" data-test="avatar-owned-status">
      <p v-if="store.ownedStatus === 'loading'" class="card__hint">正在读取我的已购装扮…</p>
      <p v-if="store.ownedError" class="hint hint--error">{{ store.ownedError }}</p>
      <button v-if="store.ownedStatus === 'error'" type="button" class="btn btn--small" @click="store.loadOwned()">重新读取已购装扮</button>
      <template v-if="store.nextCursor"><p class="card__hint">已购清单尚未读完。继续读取后再确认其他商品的购买。</p>
        <button type="button" class="btn btn--small" :disabled="store.loadingMore" data-test="avatar-owned-more" @click="store.loadOwned(true)">继续读取我的装扮</button></template>
      <p v-if="store.ownedComplete">已读取我的 {{ store.owned.length }} 件已购装扮；免费初始装扮另列。</p>
    </div>

    <div v-if="tab === 'wardrobe'" class="stack">
      <div class="avatar-wardrobe-grid">
        <div class="card stack">
          <h2 class="card__title">试穿草稿</h2>
          <AvatarFigure :slots="preview" label="本地试穿（尚未保存）" />
          <p v-if="defaultsPending" class="card__hint">必选部位的“使用默认”将在保存时由服务器确定；预览不代表届时的默认项。</p>
          <p class="card__hint">草稿基于第 {{ draftVersion ?? '未知' }} 版。购买不会自动穿戴。</p>
          <label v-for="slot in AVATAR_SLOTS" :key="slot" class="field">
            <span class="field__label">{{ SLOT_LABEL[slot] }}</span>
            <select v-model="draft[slot]" class="field__input" :disabled="!!store.saveAction" :data-test="`avatar-slot-${slot}`" @change="dirty = true">
              <option :value="null">{{ ['hair', 'clothes', 'expression'].includes(slot) ? '使用配置的默认项' : '不穿戴' }}</option>
              <option v-if="draft[slot] && !options(slot).some(i => i.id === draft[slot])" :value="draft[slot]">原选择当前不可用，请检查</option>
              <option v-for="item in options(slot)" :key="item.id" :value="item.id">{{ item.name }}{{ item.starter ? ' · 免费初始' : ' · 已购买' }}{{ item.listed === false ? ' · 已下架，仍可穿戴' : '' }}</option>
            </select>
          </label>
          <button type="button" class="btn" :disabled="draftVersion === null || store.appearanceStatus !== 'ready' || !!store.saveAction || !!changedBase"
            data-test="avatar-save" @click="save()">确认保存整套形象</button>
        </div>
        <div class="card stack">
          <h2 class="card__title">服务器当前形象</h2>
          <AvatarFigure :slots="store.appearanceStatus === 'ready' && store.appearance ? store.appearance.slots : emptyFigureSlots()"
            :label="store.appearanceStatus === 'ready' && store.appearance?.complete ? '当前形象' : '基础占位形象（当前状态未完整确认）'" />
          <p v-if="store.appearanceStatus === 'ready' && store.appearance" class="card__hint">
            {{ store.appearance.source === 'saved_loadout' ? `已保存第 ${store.appearance.loadout_version} 版` : '还没有保存过形象，显示初始配置' }}
            <template v-if="!store.appearance.complete">；初始配置尚未完成，不能据此显示已保存的角色。</template></p>
          <p v-if="store.appearanceStatus === 'loading'" class="card__hint">正在读取当前形象…</p>
          <p v-if="store.appearanceError" class="hint hint--error">{{ store.appearanceError }}</p>
          <button type="button" class="btn btn--small btn--soft" :disabled="store.saveBusy" data-test="avatar-refresh-appearance" @click="reviewCurrent()">保留草稿并重新读取</button>
          <template v-if="changedBase"><p class="hint">当前已是第 {{ store.appearance?.loadout_version }} 版，草稿仍基于第 {{ draftVersion }} 版。请比较后决定，尚未覆盖。</p>
            <button type="button" class="btn" data-test="avatar-accept-base" :disabled="!!store.saveAction" @click="acceptNewBase()">确认保留草稿，以当前版本为基础</button></template>
        </div>
      </div>
    </div>

    <div v-if="confirmation" class="card stack avatar-confirm" role="region" aria-label="确认购买" data-test="avatar-purchase-confirm">
      <h2 class="card__title">确认购买 {{ confirmation.name }}？</h2>
      <p>第 {{ confirmation.version }} 版，{{ confirmation.price_sunshine }} 阳光。只购买一次，不会自动穿戴。</p>
      <p v-if="wallet.status === 'ready' && wallet.balances">当前读取余额 {{ wallet.balances.sunshine }}；
        <template v-if="wallet.balances.sunshine >= (confirmation.price_sunshine ?? 0)">预计购买后 {{ wallet.balances.sunshine - (confirmation.price_sunshine ?? 0) }}。</template>
        <template v-else>阳光可能不足，请以服务器检查为准。</template></p>
      <p class="card__hint">若价格或版本变化，这次购买会被拒绝，需要重新确认。</p>
      <button type="button" class="btn" :disabled="purchaseLocked" data-test="avatar-confirm-buy" @click="confirmBuy()">确认使用阳光购买</button>
      <button type="button" class="btn btn--soft" @click="confirmation = null">取消</button>
    </div>
  </section>
</template>
