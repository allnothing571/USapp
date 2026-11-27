<script setup lang="ts">
/**
 * One shared album (P3-2c, V1 §27, §29-§31).
 *
 * - Name: the creator renames directly, the partner sends a request, against the version captured when the form opened. While a request
 *   is pending neither can write a name: the creator approves or rejects, and the proposer may withdraw.
 * - Photos: only a photo's uploader sees its remove and delete controls, each explaining what it does and does not do. Either member chooses
 *   a cover from the active photos. A cover chosen, or the fallback after a removal, always comes from the server's answer.
 * - Photo and cover writes are independent of the name, so a pending request does not hold them.
 * - An unconfirmed write stays listed with what it captured, and its retry resends that same request.
 * - Rename-request history lists every request with its decision, read again whenever the name version or the pending request
 *   changes (photos and covers change neither). A decision there is a historical fact: an approval names the name version it created,
 *   which a later rename may have replaced. Deciding stays with the pending request's controls; the history only reads.
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'

import { ALBUM_NAME_MAX, checkAlbumName } from '@/api/album-rules'
import { beijingInstant } from '@/api/reminder-rules'
import type { AlbumProposalSummary } from '@/api/types'
import AlbumPhotoAdder from '@/components/AlbumPhotoAdder.vue'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import PhotoDeleteControl from '@/components/PhotoDeleteControl.vue'
import PhotoDeletionNotices from '@/components/PhotoDeletionNotices.vue'
import { useAlbumsStore, type AlbumAction } from '@/stores/albums'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useSessionStore } from '@/stores/session'
import { useSmartBack } from '@/router/back-navigation'

const props = defineProps<{ id: string }>()
const back = useSmartBack('album-detail', 'albums')

const session = useSessionStore()
const store = useAlbumsStore()

const me = computed(() => session.user?.id ?? '')
const item = computed(() => (store.detail.id === props.id ? store.detail.item : null))
const isCreator = computed(() => !!item.value && item.value.creator_id === me.value)
const pending = computed(() => item.value?.pending_proposal ?? null)
const myPending = computed(() => !!pending.value && pending.value.proposer_id === me.value)
const review = computed(() => (pending.value && store.review.proposalId === pending.value.id ? store.review : null))
const photos = computed(() => (store.photos.albumId === props.id ? store.photos : null))
const history = computed(() => (store.history.albumId === props.id ? store.history : null))
const historyReview = computed(() => (history.value && store.historyReview.proposalId &&
  history.value.items.some((entry) => entry.id === store.historyReview.proposalId) ? store.historyReview : null))

const HISTORY_STATUS: Record<AlbumProposalSummary['status'], string> = { pending: '待处理', approved: '已同意', rejected: '已拒绝', withdrawn: '已撤回' }
const DECISION_VERB: Record<'approved' | 'rejected' | 'withdrawn', string> = { approved: '同意', rejected: '拒绝', withdrawn: '撤回' }

const albumActions = computed(() => Object.values(store.actions).filter((action) => action.albumId === props.id))
const anySubmitting = computed(() => albumActions.value.some((action) => action.status === 'submitting'))
const unconfirmed = computed(() => albumActions.value.filter((action) => action.status !== 'submitting'))
const nameAction = computed(() => store.actions[`${isCreator.value ? 'rename' : 'propose'}:${props.id}`])
const decideAction = computed(() => (pending.value ? store.actions[`decide:${props.id}:${pending.value.id}`] : undefined))

/** The version captured when the form opened. */
const form = ref<{ version: number; name: string } | null>(null)
const nameError = ref('')
const conflict = ref(false)
const confirmingRemoval = ref<string | null>(null)

function closeForm(): void {
  form.value = null
  nameError.value = ''
  conflict.value = false
}

watch([() => props.id, () => session.user?.id ?? null], ([id, user], previous) => {
  if (previous && (previous[0] !== id || previous[1] !== user)) {
    closeForm()
    confirmingRemoval.value = null
  }
  if (user && session.isAuthenticated) void store.openDetail(id)
}, { immediate: true })

watch(() => pending.value?.id ?? null, (proposalId) => {
  if (proposalId) void store.loadReview(props.id, proposalId)
}, { immediate: true })

// Only once this album has read for this account; again after any change to its name version or pending request.
// Separate sources: each is compared on its own, so replacing the object with an equal copy (a photo or cover answer) reads nothing.
watch([() => props.id, () => me.value, () => item.value?.id ?? null, () => item.value?.version ?? null, () => pending.value?.id ?? null], ([id, user, loaded]) => {
  if (user && loaded === id && session.isAuthenticated) void store.loadHistory(id)
}, { immediate: true })

onMounted(() => {
  // Opened directly: the capability query says whether uploads are on, without loading the album list.
  if (session.isAuthenticated) useMediaCapabilityStore().ensure()
})

onBeforeUnmount(() => store.closeDetail())

function startForm(): void {
  if (!item.value) return
  form.value = { version: item.value.version, name: item.value.name }
  nameError.value = ''
  conflict.value = false
}

/** After a version conflict: continue against the newest version, keeping what was typed. */
function useLatestVersion(): void {
  if (!item.value || !form.value) return
  form.value = { ...form.value, version: item.value.version }
  conflict.value = false
}

async function submitName(): Promise<void> {
  const target = form.value
  if (!target || !item.value) return
  const checked = checkAlbumName(target.name)
  if (!checked.ok) {
    nameError.value = checked.message
    return
  }
  if (checked.value === item.value.name) {
    nameError.value = '名称没有变化。'
    return
  }
  nameError.value = ''
  const id = props.id
  const owner = me.value
  const result = isCreator.value ? await store.rename(id, target.version, checked.value) : await store.propose(id, target.version, checked.value)
  if (props.id !== id || me.value !== owner) return
  if (result.outcome === 'done' || result.outcome === 'unknown') closeForm()
  else if (result.outcome === 'rejected' && result.errorCode === 'ALBUM_VERSION_CONFLICT') conflict.value = true
  else if (result.outcome === 'rejected') closeForm()
}

function decide(outcome: 'approve' | 'reject' | 'withdraw'): void {
  if (pending.value) void store.decide(props.id, pending.value.id, outcome, review.value?.proposal?.proposed_name)
}

function retryReview(): void {
  if (pending.value) void store.loadReview(props.id, pending.value.id)
}

function toggleHistoryReview(proposalId: string): void {
  if (store.historyReview.proposalId === proposalId) store.closeHistoryReview()
  else void store.openHistoryReview(props.id, proposalId)
}

function decisionText(entry: AlbumProposalSummary): string {
  if (entry.status === 'pending') return ''
  const who = entry.decided_by === me.value ? '我' : '对方'
  return `${who}于 ${beijingInstant(entry.decided_at) ?? entry.decided_at ?? ''} ${DECISION_VERB[entry.status]}`
}

function versionText(entry: AlbumProposalSummary): string {
  const made = entry.approved_version
  const now = history.value?.currentVersion ?? null
  if (made === null) return ''
  if (now === made) return `这次同意生成了第 ${made} 版名称，也就是现在的名称。`
  return `这次同意生成了第 ${made} 版名称；之后又改过名，现在是第 ${now ?? '?'} 版。`
}

function chooseCover(mediaId: string): void {
  void store.setCover(props.id, mediaId)
}

async function removePhoto(mediaId: string): Promise<void> {
  confirmingRemoval.value = null
  await store.removePhoto(props.id, mediaId)
}

function describeAction(action: AlbumAction): string {
  switch (action.kind) {
    case 'add': return `加入 ${action.mediaIds?.length ?? 0} 张照片`
    case 'remove': return '移除一张照片'
    case 'cover': return '更换封面'
    case 'rename': return `改名为“${action.name ?? ''}”`
    case 'propose': return `申请改名为“${action.name ?? ''}”`
    case 'decide': return action.outcome === 'approve' ? '同意改名申请' : action.outcome === 'reject' ? '拒绝改名申请' : '撤回改名申请'
    default: return '这次操作'
  }
}
</script>

<template>
  <section class="page">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="album-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">共享相册</h1>
      <RouterLink v-if="back.group !== 'albums'" class="btn btn--small btn--soft" :to="{ name: 'albums' }"
        data-test="all-albums-link">全部相册</RouterLink>
    </header>

    <div class="card stack" data-test="album-detail" aria-live="polite" :aria-busy="store.detail.status === 'loading'">
      <template v-if="store.detail.status === 'error' && store.detail.id === id">
        <p class="hint hint--error" role="alert" data-test="album-detail-error">{{ store.detail.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="store.openDetail(id)">重试</button>
      </template>
      <p v-else-if="!item" class="card__hint">加载中…</p>
      <template v-else>
        <h2 class="card__title" data-test="album-name">{{ item.name }}</h2>
        <div class="list__meta">{{ item.photo_count }} 张照片 · {{ isCreator ? '我创建的' : '对方创建的' }}</div>
        <div v-if="item.cover" class="album-cover stack" data-test="album-cover">
          <MemoryPhotoView :media-id="item.cover.media_id" variant="view" alt="相册封面" />
          <span class="list__meta" data-test="album-cover-source">{{ item.cover.chosen ? '选定的封面' : '自动封面：最近一次加入的第一张' }}</span>
        </div>
        <p v-else class="card__hint" data-test="album-no-cover">还没有封面。加入照片后会自动选择。</p>
      </template>
    </div>

    <PhotoDeletionNotices />

    <ul v-if="store.notices.length" class="card list" data-test="album-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <!-- Every unconfirmed write of this album, with what it captured. A retry resends exactly that. -->
    <ul v-if="unconfirmed.length" class="card list" data-test="album-unconfirmed">
      <li v-for="action in unconfirmed" :key="action.id" class="list__item" :data-test="`album-action-${action.kind}`">
        <div class="list__main">
          <div class="list__title">{{ describeAction(action) }}</div>
          <p class="hint hint--error" role="alert">{{ action.errorText }}</p>
        </div>
        <button class="btn btn--small" type="button" :disabled="anySubmitting" data-test="album-action-retry" @click="store.retry(action.id)">重试</button>
      </li>
    </ul>

    <!-- A pending request: the current and requested names. -->
    <div v-if="item && pending" class="card stack" data-test="album-pending">
      <h2 class="card__title">{{ myPending ? '我提交的改名申请' : '对方的改名申请' }}</h2>
      <template v-if="review && review.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="album-review-error">{{ review.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="album-review-retry" @click="retryReview()">重试</button>
      </template>
      <p v-else-if="!review || !review.proposal" class="card__hint">加载中…</p>
      <dl v-else class="kv" data-test="album-review">
        <dt class="kv__key">现在的名称</dt>
        <dd class="kv__value" data-test="album-review-original">{{ review.proposal.original_name }}</dd>
        <dt class="kv__key">建议改成</dt>
        <dd class="kv__value" data-test="album-review-proposed">{{ review.proposal.proposed_name }}</dd>
      </dl>
      <div v-if="isCreator" class="row">
        <button class="btn btn--small" type="button" :disabled="!!decideAction || anySubmitting || !review || !review.proposal" data-test="album-approve" @click="decide('approve')">同意</button>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!decideAction || anySubmitting" data-test="album-reject" @click="decide('reject')">拒绝</button>
      </div>
      <div v-else-if="myPending" class="row">
        <p class="card__hint spacer">等待创建者处理。</p>
        <button class="btn btn--small btn--soft" type="button" :disabled="!!decideAction || anySubmitting" data-test="album-withdraw" @click="decide('withdraw')">撤回申请</button>
      </div>
    </div>

    <!-- Rename-request history: what was asked and decided, which is not necessarily the album's name now. -->
    <div v-if="item && history" class="card stack" data-test="album-history" aria-live="polite" :aria-busy="history.status === 'loading'">
      <h2 class="card__title">改名申请记录</h2>
      <p class="card__hint" data-test="album-history-note">
        这里是每条申请当时的处理结果，之后的改名可能已经替换了当时同意的名称。<template v-if="history.currentVersion">相册名称现在是第 {{ history.currentVersion }} 版。</template>
      </p>
      <template v-if="history.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="album-history-error">{{ history.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="album-history-retry" @click="store.loadHistory(id)">重试</button>
      </template>
      <p v-else-if="history.status === 'loading' && !history.items.length" class="card__hint">加载中…</p>
      <p v-else-if="history.status === 'ready' && !history.items.length" class="card__hint" data-test="album-history-empty">还没有改名申请。</p>
      <ul v-if="history.items.length" class="list" data-test="album-history-list">
        <li v-for="entry in history.items" :key="entry.id" class="list__item" data-test="album-history-item" :data-proposal="entry.id">
          <div class="list__main">
            <div class="list__title">
              {{ entry.proposer_id === me ? '我提交的申请' : '对方提交的申请' }}
              <span class="badge" :class="entry.status === 'pending' ? 'badge--accent' : ''" data-test="album-history-status">{{ HISTORY_STATUS[entry.status] }}</span>
            </div>
            <div class="list__meta">提交于 {{ beijingInstant(entry.created_at) ?? entry.created_at }} · 基于第 {{ entry.base_version }} 版名称</div>
            <div v-if="entry.status !== 'pending'" class="list__meta" data-test="album-history-decision">{{ decisionText(entry) }}</div>
            <div v-if="entry.status === 'approved'" class="list__meta" data-test="album-history-version">{{ versionText(entry) }}</div>
            <div v-if="entry.status === 'pending'" class="list__meta" data-test="album-history-pending">正在等待处理，处理入口在上方。</div>
          </div>
          <button v-if="entry.status !== 'pending'" class="btn btn--small btn--soft" type="button" data-test="album-history-open"
            @click="toggleHistoryReview(entry.id)">{{ store.historyReview.proposalId === entry.id ? '收起对比' : '查看对比' }}</button>
        </li>
      </ul>
      <div v-if="historyReview" class="stack" data-test="album-history-review-panel">
        <template v-if="historyReview.status === 'error'">
          <p class="hint hint--error" role="alert" data-test="album-history-review-error">{{ historyReview.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" @click="store.openHistoryReview(id, historyReview.proposalId ?? '')">重试</button>
        </template>
        <p v-else-if="!historyReview.proposal" class="card__hint">加载中…</p>
        <dl v-else class="kv" data-test="album-history-review">
          <dt class="kv__key">申请时的名称（第 {{ historyReview.proposal.base_version }} 版）</dt>
          <dd class="kv__value" data-test="album-history-review-original">{{ historyReview.proposal.original_name }}</dd>
          <dt class="kv__key">建议改成</dt>
          <dd class="kv__value" data-test="album-history-review-proposed">{{ historyReview.proposal.proposed_name }}</dd>
        </dl>
      </div>
      <template v-if="history.status === 'ready' && history.nextCursor">
        <p v-if="history.moreErrorText" class="hint hint--error" role="alert" data-test="album-history-more-error">{{ history.moreErrorText }}</p>
        <button class="btn btn--small btn--soft" type="button" :disabled="history.loadingMore" data-test="album-history-more"
          @click="store.loadMoreHistory()">{{ history.loadingMore ? '加载中…' : '加载更多' }}</button>
      </template>
    </div>

    <!-- Rename (creator) or request (partner). Not while a request is pending, or one of these is unconfirmed. -->
    <div v-if="item && !pending && !nameAction" class="card stack" data-test="album-name-write">
      <form v-if="form" class="stack" data-test="album-name-form" @submit.prevent="submitName()">
        <div class="field">
          <label class="field__label" for="album-name-input">{{ isCreator ? '新名称' : '建议的新名称（需要创建者同意）' }}（{{ Array.from(form.name).length }} / {{ ALBUM_NAME_MAX }}）</label>
          <input id="album-name-input" v-model="form.name" class="field__input" type="text" data-test="album-name-input" />
        </div>
        <p v-if="nameError" class="hint hint--error" role="alert" data-test="album-name-error">{{ nameError }}</p>
        <template v-if="conflict">
          <p class="hint hint--error" role="alert" data-test="album-conflict">相册名称已被修改，现在是“{{ item.name }}”。你填写的内容仍保留，确认后可基于最新版本提交。</p>
          <button class="btn btn--small btn--soft" type="button" data-test="album-use-latest" @click="useLatestVersion()">基于最新版本继续</button>
        </template>
        <div class="row">
          <button class="btn btn--small" type="submit" :disabled="conflict || anySubmitting" data-test="album-name-submit">{{ isCreator ? '保存名称' : '提交申请' }}</button>
          <button class="btn btn--small btn--ghost" type="button" data-test="album-name-cancel" @click="closeForm()">取消</button>
        </div>
      </form>
      <button v-else class="btn btn--small btn--soft" type="button" data-test="album-start-rename" @click="startForm()">{{ isCreator ? '改名' : '申请改名' }}</button>
    </div>

    <AlbumPhotoAdder v-if="item" :key="`${id}:${me}`" :album-id="id" />

    <div class="card stack" data-test="album-photos" :aria-busy="photos?.status === 'loading'">
      <h2 class="card__title">照片</h2>
      <template v-if="photos && photos.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="album-photos-error">{{ photos.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="album-photos-reload" @click="store.loadPhotos(id)">重试</button>
      </template>
      <p v-else-if="!photos || (photos.status !== 'ready' && photos.items.length === 0)" class="card__hint">加载中…</p>
      <p v-else-if="photos.items.length === 0" class="card__hint" data-test="album-photos-empty">这个相册还没有照片。</p>
      <div v-else class="memory-photos">
        <div v-for="photo in photos.items" :key="photo.media_id" class="album-photo" data-test="album-photo" :data-media="photo.media_id">
          <MemoryPhotoView :media-id="photo.media_id" alt="相册照片" />
          <span v-if="item?.cover?.media_id === photo.media_id" class="badge badge--accent" data-test="album-photo-cover">封面</span>
          <button v-if="!(item?.cover?.media_id === photo.media_id && item?.cover?.chosen)" class="btn btn--small btn--soft" type="button"
            :disabled="anySubmitting || !!store.actions[`cover:${id}`]" data-test="album-set-cover" @click="chooseCover(photo.media_id)">设为封面</button>
          <template v-if="photo.uploader_id === me">
            <template v-if="confirmingRemoval === photo.media_id">
              <p class="card__hint" data-test="album-remove-explain">只从相册里隐藏：之后对方不能再打开这张照片，已经打开过的链接可能还能用几分钟。文件仍然保留，不会被删除，也不能恢复或再加入任何相册。</p>
              <div class="row">
                <button class="btn btn--small" type="button" :disabled="anySubmitting" data-test="album-remove-confirm" @click="removePhoto(photo.media_id)">确认移除</button>
                <button class="btn btn--small btn--ghost" type="button" data-test="album-remove-cancel" @click="confirmingRemoval = null">取消</button>
              </div>
            </template>
            <button v-else class="btn btn--small btn--ghost" type="button" :disabled="!!store.actions[`remove:${id}:${photo.media_id}`]" data-test="album-remove"
              @click="confirmingRemoval = photo.media_id">从相册移除</button>
            <!-- Not the same thing: removal only hides it from this album. Deletion stops the photo everywhere, and can be undone. -->
            <PhotoDeleteControl :key="`${photo.media_id}:${me}`" :media-id="photo.media_id" :disabled="anySubmitting" />
          </template>
        </div>
      </div>
      <p v-if="photos?.moreErrorText" class="hint hint--error" role="alert" data-test="album-photos-more-error">{{ photos.moreErrorText }}</p>
      <button v-if="photos?.status === 'ready' && photos.nextCursor" class="btn btn--small btn--soft" type="button" data-test="album-photos-more"
        :disabled="photos.loadingMore" @click="store.loadMorePhotos()">{{ photos.loadingMore ? '加载中…' : '加载更多' }}</button>
    </div>
  </section>
</template>
