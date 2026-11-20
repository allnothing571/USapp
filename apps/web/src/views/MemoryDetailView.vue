<script setup lang="ts">
/**
 * One photo memory (P3-1c, V1 §29-§31).
 *
 * - The creator edits directly, against the version captured when the form opened. The partner submits a full replacement as a
 *   request. While one is pending, neither can write: the creator approves or rejects, and the proposer may withdraw.
 * - The review shows the immutable current and proposed versions side by side, read from the proposal record, and marks what differs.
 * - A version conflict keeps what was typed; continuing against the newest version is an explicit click.
 * - An unconfirmed write stays locked to what it captured, and a retry resends that same request. Replaying an approval shows its
 *   confirmed result and writes nothing new.
 * - Changing memory or account clears the form; leaving the page drops late reads and forgets the form's uploads.
 * - Edit-request history lists every request with its decision, read again whenever the memory or its pending request changes.
 *   A decision there is a historical fact: an approval names the version it created, which a later revision may have replaced.
 *   Deciding stays with the pending request's controls above; the history only reads.
 */
import { computed, onBeforeUnmount, ref, watch } from 'vue'

import { civilDateLabel } from '@/api/anniversary-rules'
import { termsOfContent } from '@/api/memory-rules'
import { beijingInstant } from '@/api/reminder-rules'
import type { MemoryProposalSummary, MemoryTerms } from '@/api/types'
import MemoryEditor from '@/components/MemoryEditor.vue'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import MemoryProposalCompare from '@/components/MemoryProposalCompare.vue'
import MemoryDeletionControls from '@/components/MemoryDeletionControls.vue'
import { useMemoryDeletionsStore } from '@/stores/memory-deletions'
import PhotoDeleteControl from '@/components/PhotoDeleteControl.vue'
import PhotoDeletionNotices from '@/components/PhotoDeletionNotices.vue'
import { useMemoriesStore } from '@/stores/memories'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ id: string }>()

const session = useSessionStore()
const store = useMemoriesStore()
const deletions = useMemoryDeletionsStore()

const me = computed(() => session.user?.id ?? '')
const item = computed(() => (store.detail.id === props.id ? store.detail.item : null))
const isCreator = computed(() => !!item.value && item.value.creator_id === me.value)
const pending = computed(() => item.value?.pending_proposal ?? null)
const myPending = computed(() => !!pending.value && pending.value.proposer_id === me.value)
const review = computed(() => (pending.value && store.review.proposalId === pending.value.id ? store.review : null))
const history = computed(() => (store.history.memoryId === props.id ? store.history : null))
const historyReview = computed(() => (history.value && store.historyReview.proposalId &&
  history.value.items.some((entry) => entry.id === store.historyReview.proposalId) ? store.historyReview : null))

const HISTORY_STATUS: Record<MemoryProposalSummary['status'], string> = { pending: '待处理', approved: '已同意', rejected: '已拒绝', withdrawn: '已撤回' }
const DECISION_VERB: Record<'approved' | 'rejected' | 'withdrawn', string> = { approved: '同意', rejected: '拒绝', withdrawn: '撤回' }

/** The version captured when the form opened. */
const snapshot = ref<{ id: string; version: number; terms: MemoryTerms } | null>(null)
const formKey = ref(0)
const conflict = ref(false)

const writeActionId = computed(() => `${isCreator.value ? 'edit' : 'propose'}:${props.id}`)
const writeAction = computed(() => store.actions[writeActionId.value])
const decideAction = computed(() => (pending.value ? store.actions[`decide:${props.id}:${pending.value.id}`] : undefined))
const formInitial = computed(() => writeAction.value?.terms ?? snapshot.value?.terms ?? null)
/** An unconfirmed decision whose proposal is no longer the pending one: a re-read may already show its result. It can still be replayed. */
const orphanDecide = computed(() => Object.values(store.actions).find((action) =>
  action.kind === 'decide' && action.memoryId === props.id && action.proposalId !== pending.value?.id && action.status !== 'submitting'))

function closeForm(): void {
  snapshot.value = null
  conflict.value = false
  formKey.value += 1
}

watch([() => props.id, () => session.user?.id ?? null], ([id, user], previous) => {
  if (previous && (previous[0] !== id || previous[1] !== user)) closeForm()
  if (user && session.isAuthenticated) void store.openDetail(id)
}, { immediate: true })

watch(() => pending.value?.id ?? null, (proposalId) => {
  if (proposalId) void store.loadReview(props.id, proposalId)
}, { immediate: true })

// Only once this memory has read for this account; again after any change to its version or pending request.
// Separate sources: each is compared on its own, so replacing the object with an equal copy (a photo or cover answer) reads nothing.
watch([() => props.id, () => me.value, () => item.value?.id ?? null, () => item.value?.version ?? null, () => pending.value?.id ?? null], ([id, user, loaded]) => {
  if (user && loaded === id && session.isAuthenticated) void store.loadHistory(id)
}, { immediate: true })

onBeforeUnmount(() => store.closeDetail())

function sameTarget(id: string, owner: string): boolean {
  return props.id === id && me.value === owner
}

function startForm(): void {
  if (!item.value) return
  // Opened directly: the capability query says whether uploads are on, without loading the timeline.
  useMediaCapabilityStore().ensure()
  snapshot.value = { id: item.value.id, version: item.value.version, terms: termsOfContent(item.value) }
  conflict.value = false
  formKey.value += 1
}

/** After a version conflict: continue against the newest version, keeping what was typed. */
function useLatestVersion(): void {
  if (!item.value || !snapshot.value) return
  snapshot.value = { ...snapshot.value, version: item.value.version }
  conflict.value = false
}

async function submitTerms(terms: MemoryTerms): Promise<void> {
  const target = snapshot.value
  if (!target || target.id !== props.id) return
  const id = props.id
  const owner = me.value
  const result = isCreator.value ? await store.edit(id, target.version, terms) : await store.propose(id, target.version, terms)
  if (!sameTarget(id, owner)) return
  if (result.outcome === 'done') closeForm()
  else if (result.outcome === 'rejected' && result.errorCode === 'MEMORY_VERSION_CONFLICT') conflict.value = true
}

async function retryWrite(): Promise<void> {
  const id = props.id
  const owner = me.value
  const result = await store.retry(writeActionId.value)
  if (result.outcome === 'done' && sameTarget(id, owner)) closeForm()
}

function decide(outcome: 'approve' | 'reject' | 'withdraw'): void {
  if (pending.value) void store.decide(props.id, pending.value.id, outcome)
}

function retryDecide(): void {
  const action = decideAction.value ?? orphanDecide.value
  if (action) void store.retry(action.id)
}

function retryReview(): void {
  if (pending.value) void store.loadReview(props.id, pending.value.id)
}

function toggleHistoryReview(proposalId: string): void {
  if (store.historyReview.proposalId === proposalId) store.closeHistoryReview()
  else void store.openHistoryReview(props.id, proposalId)
}

function decisionText(entry: MemoryProposalSummary): string {
  if (entry.status === 'pending') return ''
  const who = entry.decided_by === me.value ? '我' : '对方'
  return `${who}于 ${beijingInstant(entry.decided_at) ?? entry.decided_at ?? ''} ${DECISION_VERB[entry.status]}`
}

function versionText(entry: MemoryProposalSummary): string {
  const made = entry.approved_version
  const now = history.value?.currentVersion ?? null
  if (made === null) return ''
  if (now === made) return `这次同意生成了第 ${made} 版，也就是现在的版本。`
  return `这次同意生成了第 ${made} 版；之后回忆又有修改，现在是第 ${now ?? '?'} 版。`
}
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">回忆</h1>
      </div>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'memories' }">返回</RouterLink>
    </header>

    <div class="card stack" data-test="memory-detail" aria-live="polite" :aria-busy="store.detail.status === 'loading'">
      <template v-if="store.detail.status === 'error' && store.detail.id === id">
        <p class="hint hint--error" role="alert" data-test="memory-detail-error">{{ store.detail.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="store.openDetail(id)">重试</button>
      </template>
      <p v-else-if="!item" class="card__hint">加载中…</p>
      <template v-else>
        <h2 class="card__title" data-test="memory-title">{{ item.title }}</h2>
        <div class="list__meta">{{ civilDateLabel(item.date) }} · {{ isCreator ? '我添加的' : '对方添加的' }}</div>
        <p v-if="item.description" class="message-body" data-test="memory-description">{{ item.description }}</p>
        <div class="memory-photos" data-test="memory-photos">
          <div v-for="photo in item.photos" :key="photo.media_id" class="album-photo" data-test="memory-photo-item" :data-media="photo.media_id">
            <MemoryPhotoView :media-id="photo.media_id" variant="view" :deleted="photo.deleted" />
            <!-- Only its uploader, and not while a form holds this memory's photos. -->
            <PhotoDeleteControl v-if="photo.uploader_id === me && !photo.deleted && !snapshot && !writeAction" :key="`${photo.media_id}:${me}`" :media-id="photo.media_id" :disabled="!!deletions.actions[id]" />
            <RouterLink v-else-if="photo.uploader_id === me && photo.deleted" class="btn btn--small btn--soft" :to="{ name: 'media-deletions' }"
              data-test="memory-photo-recover">去恢复</RouterLink>
          </div>
        </div>
      </template>
    </div>

    <PhotoDeletionNotices />
    <MemoryDeletionControls :key="id + ':' + me" :memory-id="id" :memory="item"
      :busy="!!writeAction || !!decideAction || !!snapshot" />

    <ul v-if="store.notices.length" class="card list" data-test="memory-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <!-- A pending request: current and proposed, side by side. -->
    <div v-if="item && pending" class="card stack" data-test="memory-pending">
      <h2 class="card__title">{{ myPending ? '我提交的修改申请' : '对方的修改申请' }}</h2>
      <template v-if="review && review.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="memory-review-error">{{ review.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="memory-review-retry" @click="retryReview()">重试</button>
      </template>
      <p v-else-if="!review || !review.proposal" class="card__hint">加载中…</p>
      <MemoryProposalCompare v-else :proposal="review.proposal" />
      <template v-if="decideAction && decideAction.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="memory-decide-pending">{{ decideAction.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="memory-decide-retry" @click="retryDecide()">重试</button>
      </template>
      <div v-else-if="isCreator" class="row">
        <button class="btn btn--small" type="button" :disabled="!!deletions.actions[id] || !!decideAction || !review || !review.proposal" data-test="memory-approve"
          @click="decide('approve')">同意</button>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!deletions.actions[id] || !!decideAction" data-test="memory-reject" @click="decide('reject')">拒绝</button>
      </div>
      <div v-else-if="myPending" class="row">
        <p class="card__hint spacer">等待创建者处理。</p>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!deletions.actions[id] || !!decideAction" data-test="memory-withdraw" @click="decide('withdraw')">撤回申请</button>
      </div>
    </div>

    <div v-if="orphanDecide" class="card stack" data-test="memory-decide-orphan">
      <p class="hint hint--error" role="alert">{{ orphanDecide.errorText }}</p>
      <button class="btn btn--small" type="button" data-test="memory-decide-orphan-retry" @click="retryDecide()">重试确认</button>
    </div>

    <!-- Edit-request history: what was asked and decided, which is not necessarily what the memory says now. -->
    <div v-if="item && history" class="card stack" data-test="memory-history" aria-live="polite" :aria-busy="history.status === 'loading'">
      <h2 class="card__title">修改申请记录</h2>
      <p class="card__hint" data-test="memory-history-note">
        这里是每条申请当时的处理结果，不代表回忆现在的内容。<template v-if="history.currentVersion">回忆现在是第 {{ history.currentVersion }} 版。</template>
      </p>
      <template v-if="history.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="memory-history-error">{{ history.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="memory-history-retry" @click="store.loadHistory(id)">重试</button>
      </template>
      <p v-else-if="history.status === 'loading' && !history.items.length" class="card__hint">加载中…</p>
      <p v-else-if="history.status === 'ready' && !history.items.length" class="card__hint" data-test="memory-history-empty">还没有修改申请。</p>
      <ul v-if="history.items.length" class="list" data-test="memory-history-list">
        <li v-for="entry in history.items" :key="entry.id" class="list__item" data-test="memory-history-item" :data-proposal="entry.id">
          <div class="list__main">
            <div class="list__title">
              {{ entry.proposer_id === me ? '我提交的申请' : '对方提交的申请' }}
              <span class="badge" :class="entry.status === 'pending' ? 'badge--accent' : ''" data-test="memory-history-status">{{ HISTORY_STATUS[entry.status] }}</span>
            </div>
            <div class="list__meta">提交于 {{ beijingInstant(entry.created_at) ?? entry.created_at }} · 基于第 {{ entry.base_version }} 版</div>
            <div v-if="entry.status !== 'pending'" class="list__meta" data-test="memory-history-decision">{{ decisionText(entry) }}</div>
            <div v-if="entry.status === 'approved'" class="list__meta" data-test="memory-history-version">{{ versionText(entry) }}</div>
            <div v-if="entry.status === 'pending'" class="list__meta" data-test="memory-history-pending">正在等待处理，处理入口在上方。</div>
          </div>
          <button v-if="entry.status !== 'pending'" class="btn btn--small btn--soft" type="button" data-test="memory-history-open"
            @click="toggleHistoryReview(entry.id)">{{ store.historyReview.proposalId === entry.id ? '收起对比' : '查看对比' }}</button>
        </li>
      </ul>
      <div v-if="historyReview" class="stack" data-test="memory-history-review-panel">
        <template v-if="historyReview.status === 'error'">
          <p class="hint hint--error" role="alert" data-test="memory-history-review-error">{{ historyReview.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" @click="store.openHistoryReview(id, historyReview.proposalId ?? '')">重试</button>
        </template>
        <p v-else-if="!historyReview.proposal" class="card__hint">加载中…</p>
        <MemoryProposalCompare v-else :proposal="historyReview.proposal" prefix="memory-history"
          :original-label="`申请时的内容（第 ${historyReview.proposal.base_version} 版）`" />
      </div>
      <template v-if="history.status === 'ready' && history.nextCursor">
        <p v-if="history.moreErrorText" class="hint hint--error" role="alert" data-test="memory-history-more-error">{{ history.moreErrorText }}</p>
        <button class="btn btn--small btn--soft" type="button" :disabled="history.loadingMore" data-test="memory-history-more"
          @click="store.loadMoreHistory()">{{ history.loadingMore ? '加载中…' : '加载更多' }}</button>
      </template>
    </div>

    <!-- Edit (creator) or request (partner). -->
    <div v-if="item && !pending && !item.pending_delete_request && !deletions.actions[id]" class="card stack" data-test="memory-write">
      <template v-if="snapshot || writeAction">
        <h2 class="card__title">{{ isCreator ? '修改回忆' : '申请修改（需要创建者同意）' }}</h2>
        <template v-if="conflict">
          <p class="hint hint--error" role="alert" data-test="memory-conflict">回忆已被修改。你填写的内容仍保留，确认后可基于最新版本提交。</p>
          <button class="btn btn--small btn--soft" type="button" data-test="memory-use-latest" @click="useLatestVersion()">基于最新版本继续</button>
        </template>
        <p v-if="item.photos.some((photo) => photo.deleted)" class="card__hint" data-test="memory-write-deleted-hint">已删除的照片不会带入这次修改。原来的记录不变。</p>
        <MemoryEditor :key="`${formKey}:${id}:${me}`" name="memory-write" :scope="`write:${id}`" :initial="formInitial"
          :submit-label="isCreator ? '保存修改' : '提交申请'" :busy="!!writeAction || conflict" @submit="submitTerms" />
        <template v-if="writeAction && writeAction.status !== 'submitting'">
          <p class="hint hint--error" role="alert" data-test="memory-write-pending">{{ writeAction.errorText }}</p>
          <button class="btn btn--small" type="button" data-test="memory-write-retry" @click="retryWrite()">重试</button>
        </template>
        <button v-if="!writeAction" class="btn btn--small btn--ghost" type="button" data-test="memory-write-cancel" @click="closeForm()">取消</button>
      </template>
      <button v-else class="btn btn--small btn--soft" type="button" data-test="memory-start-write" @click="startForm()">
        {{ isCreator ? '修改' : '申请修改' }}
      </button>
    </div>
  </section>
</template>
