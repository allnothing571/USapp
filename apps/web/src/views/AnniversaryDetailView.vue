<script setup lang="ts">
/**
 * 纪念日详情（阶段 2 P2-2，V1 §24 / §29-§31）。
 *
 * - 创建者：直接修改（标题、日期、类型、备注一起，带开始修改时捕获的版本）；有待处理的修改申请时
 *   先同意或拒绝（“拒绝并自己修改”）。
 * - 对方：提交修改申请（完整内容，带捕获的版本），或撤回自己待处理的申请。
 * - 双方：“在我的首页展示”只影响自己的首页。
 * - 天数只来自服务端（`as_of_date`，北京时间）。版本冲突时保留填写的内容，提示刷新后继续。
 * - 换一个纪念日或换账号时清空草稿与捕获的版本；旧目标的晚到结果不会关闭或改动新目标的表单。
 */
import { computed, ref, watch } from 'vue'

import { anniversaryHeadline, anniversaryNextLine, civilDateLabel, KIND_LABEL } from '@/api/anniversary-rules'
import type { AnniversaryTerms } from '@/api/types'
import AnniversaryForm from '@/components/AnniversaryForm.vue'
import ReminderControls from '@/components/ReminderControls.vue'
import { useAnniversariesStore } from '@/stores/anniversaries'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ id: string }>()

const session = useSessionStore()
const store = useAnniversariesStore()

const me = computed(() => session.user?.id ?? '')
const item = computed(() => (store.detail.id === props.id ? store.detail.item : null))
const isCreator = computed(() => !!item.value && item.value.creator_id === me.value)
const pending = computed(() => item.value?.pending_proposal ?? null)
const myPending = computed(() => !!pending.value && pending.value.proposer_id === me.value)

/** The version captured when the edit or proposal form opened (M7 pattern). */
const snapshot = ref<{ id: string; version: number; terms: AnniversaryTerms } | null>(null)
const formKey = ref(0)
const conflict = ref(false)

const editAction = computed(() => store.actions[`edit:${props.id}`])
const proposeAction = computed(() => store.actions[`propose:${props.id}`])
const writeAction = computed(() => (isCreator.value ? editAction.value : proposeAction.value))
const decideAction = computed(() => (pending.value ? store.actions[`decide:${props.id}:${pending.value.id}`] : undefined))
const pinAction = computed(() => store.actions.pin)
const partnerPresent = computed(() => (session.space?.member_count ?? 0) >= 2)

function closeForm(): void {
  snapshot.value = null
  conflict.value = false
  formKey.value += 1
}

watch([() => props.id, () => session.user?.id ?? null], ([id, user], previous) => {
  if (previous && (previous[0] !== id || previous[1] !== user)) closeForm()
  if (user && session.isAuthenticated) void store.openDetail(id)
}, { immediate: true })

function sameTarget(id: string, owner: string): boolean {
  return props.id === id && me.value === owner
}

function startForm(): void {
  if (!item.value) return
  const { title, date, kind, note } = item.value
  snapshot.value = { id: item.value.id, version: item.value.version, terms: { title, date, kind, note } }
  conflict.value = false
  formKey.value += 1
}

/** After a version conflict: continue from the newest version, keeping what was typed. */
function useLatestVersion(): void {
  if (!item.value || !snapshot.value) return
  snapshot.value = { ...snapshot.value, version: item.value.version }
  conflict.value = false
}

async function submitTerms(terms: AnniversaryTerms, notifyPartner: boolean | undefined): Promise<void> {
  const target = snapshot.value
  if (!target || target.id !== props.id) return
  const id = props.id
  const owner = me.value
  const result = isCreator.value
    ? await store.edit(id, target.version, terms, notifyPartner)
    : await store.propose(id, target.version, terms)
  if (!sameTarget(id, owner)) return
  if (result.outcome === 'done') closeForm()
  else if (result.outcome === 'rejected' && result.errorCode === 'ANNIVERSARY_VERSION_CONFLICT') conflict.value = true
}

async function retryWrite(): Promise<void> {
  const id = props.id
  const owner = me.value
  const result = await store.retry(`${isCreator.value ? 'edit' : 'propose'}:${id}`)
  if (result.outcome === 'done' && sameTarget(id, owner)) closeForm()
}

async function decide(outcome: 'approve' | 'reject' | 'withdraw', thenEdit = false): Promise<void> {
  if (!pending.value) return
  const id = props.id
  const owner = me.value
  const result = await store.decide(id, pending.value.id, outcome)
  if (result.outcome === 'done' && thenEdit && sameTarget(id, owner)) startForm()
}

function retryDecide(): void {
  if (decideAction.value) void store.retry(decideAction.value.id)
}

function togglePin(): void {
  if (!item.value || pinAction.value) return
  void store.pin(item.value.pinned_by_me ? null : item.value.id)
}

const DECISION_TEXT = { approved: '已同意', rejected: '已拒绝', withdrawn: '已撤回' } as const
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">纪念日</h1>
        <p class="page__subtitle">按北京时间计算天数。</p>
      </div>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'us' }">返回</RouterLink>
    </header>

    <div class="card stack" data-test="anniversary-detail" aria-live="polite"
      :aria-busy="store.detail.status === 'loading'">
      <template v-if="store.detail.status === 'error' && store.detail.id === id">
        <p class="hint hint--error" role="alert">{{ store.detail.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="store.openDetail(id)">重试</button>
      </template>
      <p v-else-if="!item" class="card__hint">加载中…</p>
      <template v-else>
        <h2 class="card__title" data-test="anniversary-title">{{ item.title }}</h2>
        <div class="list__meta">{{ KIND_LABEL[item.kind] }} · {{ civilDateLabel(item.date) }}</div>
        <p class="kv__value" data-test="anniversary-headline">{{ anniversaryHeadline(item) }}</p>
        <p v-if="anniversaryNextLine(item)" class="card__hint" data-test="anniversary-next">{{ anniversaryNextLine(item) }}</p>
        <p v-if="item.note" class="message-body" data-test="anniversary-note">{{ item.note }}</p>
        <div class="list__meta">{{ isCreator ? '我创建的' : '对方创建的' }}</div>

        <div class="row">
          <button class="btn btn--small btn--soft" type="button" data-test="anniversary-pin"
            :disabled="!!pinAction && pinAction.status === 'submitting'" @click="togglePin()">
            {{ item.pinned_by_me ? '取消在我的首页展示' : '在我的首页展示' }}
          </button>
        </div>
        <template v-if="pinAction && pinAction.status !== 'submitting'">
          <p class="hint hint--error" role="alert" data-test="anniversary-pin-pending">{{ pinAction.errorText }}</p>
          <button class="btn btn--small" type="button" data-test="anniversary-pin-retry" @click="store.retry('pin')">重试</button>
        </template>
      </template>
    </div>

    <div v-if="item" class="card">
      <ReminderControls :key="`${me}:${item.id}`" :anniversary-id="item.id" />
    </div>

    <ul v-if="store.notices.length" class="card list" data-test="anniversary-detail-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <!-- 待处理的修改申请 -->
    <div v-if="item && pending" class="card stack" data-test="anniversary-pending">
      <h2 class="card__title">{{ myPending ? '我提交的修改申请' : '对方的修改申请' }}</h2>
      <dl class="kv">
        <dt class="kv__key">标题</dt><dd class="kv__value">{{ pending.title }}</dd>
        <dt class="kv__key">日期</dt><dd class="kv__value">{{ civilDateLabel(pending.date) }}</dd>
        <dt class="kv__key">类型</dt><dd class="kv__value">{{ KIND_LABEL[pending.kind] }}</dd>
        <dt class="kv__key">备注</dt><dd class="kv__value">{{ pending.note || '（无）' }}</dd>
      </dl>
      <template v-if="decideAction && decideAction.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="anniversary-decide-pending">{{ decideAction.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="anniversary-decide-retry" @click="retryDecide()">重试</button>
      </template>
      <div v-else-if="isCreator" class="row">
        <button class="btn btn--small" type="button" :disabled="!!decideAction" data-test="anniversary-approve"
          @click="decide('approve')">同意</button>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!decideAction" data-test="anniversary-reject"
          @click="decide('reject')">拒绝</button>
        <button class="btn btn--small btn--ghost" type="button" :disabled="!!decideAction"
          data-test="anniversary-reject-edit" @click="decide('reject', true)">拒绝并自己修改</button>
      </div>
      <div v-else-if="myPending" class="row">
        <p class="card__hint spacer">等待创建者处理。</p>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!decideAction"
          data-test="anniversary-withdraw" @click="decide('withdraw')">撤回申请</button>
      </div>
    </div>
    <p v-else-if="item && item.last_decision && item.last_decision.proposer_id === me" class="card card__hint"
      data-test="anniversary-last-decision">
      我上次的修改申请：{{ DECISION_TEXT[item.last_decision.outcome] }}
    </p>

    <!-- 修改（创建者）或修改申请（对方） -->
    <div v-if="item && !pending" class="card stack" data-test="anniversary-write">
      <template v-if="snapshot || writeAction">
        <h2 class="card__title">{{ isCreator ? '修改纪念日' : '申请修改（需要创建者同意）' }}</h2>
        <template v-if="conflict">
          <p class="hint hint--error" role="alert" data-test="anniversary-conflict">
            纪念日已被修改。你填写的内容仍保留，确认后可基于最新版本提交。
          </p>
          <button class="btn btn--small btn--soft" type="button" data-test="anniversary-use-latest"
            @click="useLatestVersion()">基于最新版本继续</button>
        </template>
        <AnniversaryForm :key="formKey" name="anniversary-write" :initial="snapshot ? snapshot.terms : null"
          :submit-label="isCreator ? '保存修改' : '提交申请'" :busy="!!writeAction || conflict"
          :notify-operation="isCreator ? 'anniversary_edit' : null" :partner-present="partnerPresent"
          :captured-notify="writeAction && isCreator ? writeAction.notify === true : null" @submit="submitTerms" />
        <template v-if="writeAction && writeAction.status !== 'submitting'">
          <p class="hint hint--error" role="alert" data-test="anniversary-write-pending">{{ writeAction.errorText }}</p>
          <button class="btn btn--small" type="button" data-test="anniversary-write-retry" @click="retryWrite()">重试</button>
        </template>
        <button v-if="!writeAction" class="btn btn--small btn--ghost" type="button" @click="closeForm()">取消</button>
      </template>
      <button v-else class="btn btn--small btn--soft" type="button" data-test="anniversary-start-write" @click="startForm()">
        {{ isCreator ? '修改' : '申请修改' }}
      </button>
    </div>
  </section>
</template>
