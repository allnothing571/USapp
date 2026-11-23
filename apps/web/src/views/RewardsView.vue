<script setup lang="ts">
/**
 * 奖励商店（阶段 1 M7）：为对方上架奖励（小红花 1-999），修改、下架；查看对方为我上架的奖励。
 *
 * - 拥有者（将来兑换的一方）由服务端推导为对方；表单没有拥有者字段。
 * - 对方只看到上架中的奖励；上架的一方看到自己上架的全部（含已下架）。
 * - 兑换方式只保存，M7 不实现兑换、扣款或履约。
 * - 乐观并发：开始修改时记录不可变的商品 id 与版本快照，提交始终携带这个原版本；第一次点击“下架”时
 *   同样记录版本，第二次点击按该版本下架。列表刷新、他处修改或下架都不会把草稿改指向新版本，
 *   服务端对过期版本返回 409，并在提示里说明。
 * - 列表按 (created_at, id) 游标分页；分页不是快照，翻页期间的修改或下架可能改变可见内容。
 * - 账号切换或注销时清空本组件的草稿、版本快照、下架确认与本地校验错误；旧账号的晚到结果不触碰新账号。
 * - M8 直接兑换：点击“兑换”时捕获商品 id、名称、价格与版本快照，确认框显示该价格；确认时提交这个版本，
 *   列表刷新不会改变它（价格已变时服务端返回 409，不会按新价格扣款）。申请兑换方式暂不可兑换（M9）。
 *   兑换记录显示快照与状态，上架的一方可以确认履约。
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'

import { buildRewardTerms, type RewardForm } from '@/api/task-rules'
import type { RedemptionMode, Redemption, RedemptionTransition, RewardProduct } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { type RewardTarget, useRewardsStore } from '@/stores/rewards'

const session = useSessionStore()
const rewards = useRewardsStore()
const me = computed(() => session.user?.id ?? null)

const MODE_LABEL: Record<RedemptionMode, string> = { direct: '直接兑换', request: '申请兑换' }

function emptyForm(): RewardForm {
  return { title: '', description: '', price: '', mode: 'direct' }
}

// viewGeneration changes with the session identity; work after an await only touches
// local state when the generation is unchanged and the component is still mounted.
let viewGeneration = 0
let alive = true
onBeforeUnmount(() => { alive = false })

const form = reactive(emptyForm())
const formError = ref('')
/** Immutable snapshot of the product being edited: id, title and version when editing started. */
const draft = ref<RewardTarget | null>(null)
const editForm = reactive(emptyForm())
const editError = ref('')
/** Snapshot taken at the first archive click; the confirming click archives exactly this version. */
const archiveConfirm = ref<RewardTarget | null>(null)
/** Redemption confirmation: the exact product version and price the user is asked to pay. */
const redeemConfirm = ref<(RewardTarget & { price: number; mode: RedemptionMode }) | null>(null)

const STATUS_LABEL: Record<Redemption['status'], string> = { pending_confirmation: '待确认', pending_fulfillment: '待履约', fulfilled: '已履约', cancelled: '已取消' }
const TRANSITION_LABEL: Record<RedemptionTransition, string> = { confirm: '同意申请并扣款', reject: '拒绝申请', withdraw: '撤回申请', cancel_propose: '提出取消', cancel_approve: '同意取消并退款', cancel_reject: '拒绝取消', cancel_withdraw: '撤回取消提议' }
const transitionConfirm = ref<{ id: string; title: string; price: number; kind: RedemptionTransition; proposalId?: string } | null>(null)
function startTransition(item: Redemption, kind: RedemptionTransition): void {
  transitionConfirm.value = { id: item.id, title: item.title, price: item.price_flowers, kind,
    proposalId: item.pending_cancellation?.id }
}
function confirmTransition(): void {
  const target = transitionConfirm.value
  if (!target) return
  transitionConfirm.value = null
  void rewards.transition(target, target.kind, target.proposalId)
}
function redemptionBusy(id: string): boolean {
  return Object.values(rewards.actions).some(action => action.targetId === id)
}

const forMe = computed(() => rewards.products.filter((p) => p.owner_id === me.value && p.status === 'active'))
const mine = computed(() => rewards.products.filter((p) => p.creator_id === me.value))
const pendingActions = computed(() => Object.values(rewards.actions))

function latest(id: string): RewardProduct | undefined {
  return rewards.products.find((p) => p.id === id)
}

/** How the latest list compares with the draft snapshot (the draft itself never changes). */
const draftDrift = computed(() => {
  if (!draft.value) return ''
  const current = latest(draft.value.id)
  if (!current) return '这件奖励已不在当前列表中；提交时会按你开始修改时的版本检查。'
  if (current.status === 'archived') return '这件奖励已被下架，不能再修改。'
  if (current.version !== draft.value.version) {
    return `这件奖励已更新为第 ${current.version} 版；提交会按你开始修改时的第 ${draft.value.version} 版检查，可能需要刷新后重新修改。`
  }
  return ''
})

function clearLocal(): void {
  Object.assign(form, emptyForm())
  formError.value = ''
  draft.value = null
  Object.assign(editForm, emptyForm())
  editError.value = ''
  archiveConfirm.value = null
  redeemConfirm.value = null
  transitionConfirm.value = null
}

watch(
  () => session.user?.id,
  (id, oldId) => {
    if (oldId !== undefined && id !== oldId) {
      viewGeneration += 1
      clearLocal()
    }
    if (id && session.isAuthenticated) {
      void rewards.load()
      void rewards.loadRedemptions()
    }
  },
)
onMounted(() => {
  if (session.isAuthenticated) {
    void rewards.load()
    void rewards.loadRedemptions()
  }
})

async function submitCreate(): Promise<void> {
  const built = buildRewardTerms(form)
  if (!built.ok) {
    formError.value = built.message
    return
  }
  formError.value = ''
  const generation = viewGeneration
  const submitted = JSON.stringify(built.terms)
  const result = await rewards.create(built.terms)
  if (!alive || generation !== viewGeneration || result.outcome !== 'done') return
  // Clear only if the form still holds the submitted content.
  const current = buildRewardTerms(form)
  if (current.ok && JSON.stringify(current.terms) === submitted) Object.assign(form, emptyForm())
}

function startEdit(product: RewardProduct): void {
  draft.value = { id: product.id, title: product.title, version: product.version }
  Object.assign(editForm, { title: product.title, description: product.description,
    price: String(product.price_flowers), mode: product.redemption_mode })
  editError.value = ''
  archiveConfirm.value = null
}

function cancelEdit(): void {
  draft.value = null
  editError.value = ''
}

async function submitEdit(): Promise<void> {
  const target = draft.value
  if (!target) return
  const built = buildRewardTerms(editForm)
  if (!built.ok) {
    editError.value = built.message
    return
  }
  editError.value = ''
  const generation = viewGeneration
  const result = await rewards.edit(target, built.terms)
  if (!alive || generation !== viewGeneration) return
  // A definitive answer ends this draft; an unknown result keeps it for an explicit retry.
  if ((result.outcome === 'done' || result.outcome === 'rejected') && draft.value === target) draft.value = null
}

function archive(product: RewardProduct): void {
  const pending = archiveConfirm.value
  if (!pending || pending.id !== product.id) {
    archiveConfirm.value = { id: product.id, title: product.title, version: product.version }
    return
  }
  archiveConfirm.value = null
  void rewards.archive(pending)
}

function startRedeem(product: RewardProduct): void {
  redeemConfirm.value = { id: product.id, title: product.title, version: product.version, price: product.price_flowers, mode: product.redemption_mode }
}

function confirmRedeem(): void {
  const target = redeemConfirm.value
  if (!target) return
  redeemConfirm.value = null
  void rewards.redeem(target)
}

/** Drift between the confirmation snapshot and the latest list row (the snapshot never changes). */
const redeemDrift = computed(() => {
  const target = redeemConfirm.value
  if (!target) return ''
  const current = latest(target.id)
  if (!current || current.status !== 'active') return '这件奖励已不在商店中；确认后服务端会拒绝，不会扣款。'
  if (current.version !== target.version) {
    return `奖励已更新（当前价格 ${current.price_flowers}）；确认仍按你看到的第 ${target.version} 版检查，服务端会拒绝过期的价格，不会扣款。`
  }
  return ''
})

function archiveDrift(product: RewardProduct): string {
  const pending = archiveConfirm.value
  if (!pending || pending.id !== product.id || pending.version === product.version) return ''
  return `已更新为第 ${product.version} 版；确认下架仍按第 ${pending.version} 版检查。`
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">奖励商店</h1>
      <p class="page__subtitle">为对方上架用小红花兑换的奖励，兑换对方为你准备的奖励；申请兑换将在之后的版本提供。</p>
    </header>

    <RouterLink class="btn btn--small btn--ghost" :to="{ name: 'home' }">返回首页</RouterLink>

    <div v-if="pendingActions.length || rewards.notices.length" class="card" aria-live="polite" data-test="reward-panel">
      <ul v-if="pendingActions.length" class="list">
        <li v-for="action in pendingActions" :key="action.id" class="list__item">
          <div class="list__main">
            <div class="list__title">{{ action.title }}</div>
            <div class="list__meta">{{ action.status === 'submitting' ? '正在提交…' : action.errorText }}</div>
          </div>
          <button v-if="action.status !== 'submitting'" class="btn btn--small btn--soft" type="button"
            :data-test="`reward-retry-${action.id}`" @click="rewards.retry(action.id)">重试</button>
        </li>
      </ul>
      <ul v-if="rewards.notices.length" class="list">
        <li v-for="notice in rewards.notices" :key="notice.id" class="list__item">
          <div class="list__main">
            <div class="list__title" :class="notice.ok ? 'hint hint--ok' : 'hint hint--error'"
              :data-test="`reward-notice-${notice.id}`">
              「{{ notice.title }}」：{{ notice.text }}
            </div>
          </div>
          <button class="btn btn--small btn--ghost" type="button" @click="rewards.dismissNotice(notice.id)">知道了</button>
        </li>
      </ul>
    </div>

    <!-- 修改草稿：绑定到开始修改时的商品与版本快照，不随列表刷新改变目标 -->
    <div v-if="draft" class="card" data-test="edit-draft">
      <h2 class="card__title">修改「{{ draft.title }}」（第 {{ draft.version }} 版）</h2>
      <form class="stack" data-test="edit-form" @submit.prevent="submitEdit()">
        <input v-model="editForm.title" class="field__input" type="text" aria-label="名称" data-test="edit-title" />
        <textarea v-model="editForm.description" class="field__input" rows="2" aria-label="描述"></textarea>
        <div class="row">
          <input v-model="editForm.price" class="field__input" type="text" inputmode="numeric" aria-label="价格"
            data-test="edit-price" />
          <select v-model="editForm.mode" class="field__input" aria-label="兑换方式">
            <option value="direct">直接兑换</option>
            <option value="request">申请兑换</option>
          </select>
        </div>
        <p v-if="draftDrift" class="hint hint--error" data-test="edit-drift">{{ draftDrift }}</p>
        <p v-if="editError" class="hint hint--error" role="alert" data-test="edit-error">{{ editError }}</p>
        <div class="row">
          <button class="btn btn--small" type="submit" :disabled="!!rewards.actions[draft.id]">保存</button>
          <button class="btn btn--small btn--ghost" type="button" @click="cancelEdit()">放弃</button>
        </div>
      </form>
    </div>

    <div class="card" data-test="for-me">
      <h2 class="card__title">对方为我准备的</h2>
      <p v-if="rewards.status === 'loading' || rewards.status === 'idle'" class="card__hint">加载中…</p>
      <template v-else-if="rewards.status === 'error'">
        <p class="hint hint--error" role="alert">{{ rewards.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="rewards.load()">重试</button>
      </template>
      <p v-else-if="forMe.length === 0" class="card__hint">对方还没有上架奖励。</p>
      <ul v-else class="list">
        <li v-for="product in forMe" :key="product.id" class="list__item">
          <div class="list__main">
            <div class="list__title">{{ product.title }}</div>
            <div v-if="product.description" class="list__meta reward-text">{{ product.description }}</div>
            <div class="list__meta">小红花 {{ product.price_flowers }} · {{ MODE_LABEL[product.redemption_mode] }}</div>
          </div>
          <button class="btn btn--small" type="button"
            :data-test="`redeem-${product.id}`" :disabled="!!rewards.actions[`redeem:${product.id}`]"
            @click="startRedeem(product)">{{ product.redemption_mode === 'request' ? '申请兑换' : '兑换' }}</button>
        </li>
      </ul>
    </div>

    <!-- 兑换确认：价格与版本取自点击“兑换”时的快照 -->
    <div v-if="redeemConfirm" class="card confirmation-card" data-test="redeem-confirm">
      <h2 class="card__title">确认兑换</h2>
      <p v-if="redeemConfirm.mode === 'direct'" class="card__hint">将扣除 <strong>{{ redeemConfirm.price }}</strong> 朵小红花兑换「{{ redeemConfirm.title }}」。扣除后由对方履约。</p>
      <p v-else class="card__hint">申请「{{ redeemConfirm.title }}」，价格 {{ redeemConfirm.price }} 朵小红花。现在不扣款、不预留，待对方同意时扣款。</p>
      <p v-if="redeemDrift" class="hint hint--error" data-test="redeem-drift">{{ redeemDrift }}</p>
      <div class="row">
        <button class="btn btn--small" type="button" data-test="redeem-submit" @click="confirmRedeem()">
          {{ redeemConfirm.mode === 'request' ? '确认申请' : '确认兑换' }}（{{ redeemConfirm.price }} 朵小红花）
        </button>
        <button class="btn btn--small btn--ghost" type="button" @click="redeemConfirm = null">取消</button>
      </div>
    </div>

    <div v-if="transitionConfirm" class="card confirmation-card" data-test="transition-confirm">
      <h2 class="card__title">{{ TRANSITION_LABEL[transitionConfirm.kind] }}</h2>
      <p>「{{ transitionConfirm.title }}」 · {{ transitionConfirm.price }} 朵小红花</p>
      <p v-if="transitionConfirm.kind === 'confirm'" class="card__hint">从申请方扣除快照价格；余额不足时申请保持待确认。</p>
      <p v-if="transitionConfirm.kind === 'cancel_approve'" class="card__hint">原额退还申请方。退款超出余额上限时取消不会生效。</p>
      <p v-if="transitionConfirm.kind === 'cancel_propose'" class="card__hint">需要对方同意后才退款；已履约的兑换不能取消。</p>
      <button class="btn btn--small" type="button" data-test="transition-submit" @click="confirmTransition()">确认{{ TRANSITION_LABEL[transitionConfirm.kind] }}</button>
      <button class="btn btn--small btn--ghost" type="button" @click="transitionConfirm = null">返回</button>
    </div>

    <div class="card" data-test="redemptions">
      <h2 class="card__title">兑换记录</h2>
      <p v-if="rewards.redemptionStatus === 'loading' || rewards.redemptionStatus === 'idle'" class="card__hint">加载中…</p>
      <template v-else-if="rewards.redemptionStatus === 'error'">
        <p class="hint hint--error" role="alert">{{ rewards.redemptionErrorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="rewards.loadRedemptions()">重试</button>
      </template>
      <p v-else-if="rewards.redemptions.length === 0" class="card__hint">还没有兑换记录。</p>
      <ul v-else class="list">
        <li v-for="item in rewards.redemptions" :key="item.id" class="list__item" :data-test="`redemption-${item.id}`">
          <div class="list__main">
            <div class="list__title">{{ item.title }}</div>
            <div class="list__meta">
              {{ item.redeemer_id === me ? '我兑换的' : '对方兑换的' }} · 小红花 {{ item.price_flowers }}
              · 第 {{ item.product_version }} 版
            </div>
          </div>
          <button v-if="item.status === 'pending_fulfillment' && item.creator_id === me"
            class="btn btn--small btn--soft" type="button" :data-test="`fulfill-${item.id}`"
            :disabled="redemptionBusy(item.id)" @click="rewards.fulfill(item)">确认已履约</button>
          <span class="badge badge--muted">{{ STATUS_LABEL[item.status] }}</span>
          <span v-if="item.cancel_reason" class="list__meta">{{ { withdrawn: '申请已撤回', rejected: '申请被拒绝', mutual: '双方同意取消，已退款', admin: '管理员已取消，已扣款部分已原路退回' }[item.cancel_reason] }}</span>
          <div v-if="item.status === 'pending_confirmation'" class="row">
            <template v-if="item.creator_id === me">
              <button type="button" class="btn btn--small" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'confirm')">同意申请</button>
              <button type="button" class="btn btn--small btn--ghost" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'reject')">拒绝</button>
            </template>
            <button v-else type="button" class="btn btn--small btn--ghost" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'withdraw')">撤回申请</button>
          </div>
          <div v-if="item.status === 'pending_fulfillment'" class="row">
            <button v-if="!item.pending_cancellation" type="button" class="btn btn--small btn--ghost" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'cancel_propose')">提出取消</button>
            <template v-else>
              <span class="list__meta">等待取消确认</span>
              <button v-if="item.pending_cancellation.proposed_by === me" type="button" class="btn btn--small btn--ghost" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'cancel_withdraw')">撤回取消</button>
              <template v-else>
                <button type="button" class="btn btn--small" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'cancel_approve')">同意取消并退款</button>
                <button type="button" class="btn btn--small btn--ghost" :disabled="redemptionBusy(item.id)" @click="startTransition(item, 'cancel_reject')">拒绝取消</button>
              </template>
            </template>
          </div>
        </li>
      </ul>
      <button v-if="rewards.redemptionCursor" class="btn btn--small btn--soft" type="button" data-test="redemptions-more"
        :disabled="rewards.redemptionLoadingMore" @click="rewards.loadMoreRedemptions()">
        {{ rewards.redemptionLoadingMore ? '加载中…' : '加载更多' }}
      </button>
    </div>

    <div class="card" data-test="mine">
      <h2 class="card__title">我为对方上架的</h2>
      <p v-if="rewards.status === 'ready' && mine.length === 0" class="card__hint">还没有上架奖励。</p>
      <ul v-else class="list">
        <li v-for="product in mine" :key="product.id" class="list__item" :data-test="`mine-${product.id}`">
          <div class="list__main">
            <div class="list__title">{{ product.title }}</div>
            <div v-if="product.description" class="list__meta reward-text">{{ product.description }}</div>
            <div class="list__meta">小红花 {{ product.price_flowers }} · {{ MODE_LABEL[product.redemption_mode] }}</div>
            <div v-if="archiveDrift(product)" class="list__meta hint hint--error" :data-test="`archive-drift-${product.id}`">
              {{ archiveDrift(product) }}
            </div>
          </div>
          <span v-if="product.status === 'archived'" class="badge badge--muted">已下架</span>
          <div v-else-if="!rewards.actions[product.id] && draft?.id !== product.id" class="row">
            <button class="btn btn--small btn--soft" type="button" :data-test="`edit-${product.id}`"
              @click="startEdit(product)">修改</button>
            <button class="btn btn--small btn--ghost" type="button" :data-test="`archive-${product.id}`"
              @click="archive(product)">{{ archiveConfirm?.id === product.id ? '确认下架' : '下架' }}</button>
          </div>
        </li>
      </ul>
      <p v-if="rewards.moreErrorText" class="hint hint--error" role="alert">{{ rewards.moreErrorText }}</p>
      <div class="row">
        <button v-if="rewards.nextCursor" class="btn btn--small btn--soft" type="button" data-test="rewards-more"
          :disabled="rewards.loadingMore" @click="rewards.loadMore()">
          {{ rewards.loadingMore ? '加载中…' : '加载更多' }}
        </button>
        <button class="btn btn--small btn--ghost" type="button" data-test="rewards-refresh" @click="rewards.load()">刷新</button>
      </div>
    </div>

    <div class="card">
      <h2 class="card__title">上架给对方</h2>
      <form class="stack" data-test="create-form" @submit.prevent="submitCreate()">
        <div class="field">
          <label class="field__label" for="reward-title">名称</label>
          <input id="reward-title" v-model="form.title" class="field__input" type="text" />
        </div>
        <div class="field">
          <label class="field__label" for="reward-description">描述（可选）</label>
          <textarea id="reward-description" v-model="form.description" class="field__input" rows="3"></textarea>
        </div>
        <div class="row">
          <div class="field">
            <label class="field__label" for="reward-price">价格（小红花 1–999）</label>
            <input id="reward-price" v-model="form.price" class="field__input" type="text" inputmode="numeric" />
          </div>
          <div class="field">
            <label class="field__label" for="reward-mode">兑换方式</label>
            <select id="reward-mode" v-model="form.mode" class="field__input">
              <option value="direct">直接兑换</option>
              <option value="request">申请兑换</option>
            </select>
          </div>
        </div>
        <p v-if="formError" class="hint hint--error" role="alert" data-test="create-error">{{ formError }}</p>
        <button class="btn btn--block" type="submit" data-test="create-submit" :disabled="!!rewards.actions.new">
          {{ rewards.actions.new ? '处理中…' : '上架' }}
        </button>
      </form>
    </div>
  </section>
</template>

<style scoped>
.reward-text {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
</style>
