<script setup lang="ts">
/**
 * D6 recovery: the signed-in uploader's own deleted photos that can still be restored. The server decides what is listed, so this works
 * from any session. Text only: a deleted photo cannot be displayed, and nothing here requests one. A row says where the photo belongs,
 * as far as the server lets this member know, and links there only when the server names a parent. Each undo names its exact cycle.
 */
import { computed, onBeforeUnmount, watch } from 'vue'

import { describeDeletionContext } from '@/api/media'
import { messageTime } from '@/api/message-rules'
import type { UndoableMediaDeletion } from '@/api/types'
import PhotoDeletionNotices from '@/components/PhotoDeletionNotices.vue'
import { useMediaDeletionsStore } from '@/stores/media-deletions'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const store = useMediaDeletionsStore()

const unconfirmed = computed(() => Object.values(store.actions).filter((action) => action.status !== 'submitting'))

watch(() => session.user?.id ?? null, (user) => {
  if (user && session.isAuthenticated) void store.load()
}, { immediate: true })

onBeforeUnmount(() => store.closeList())

/** An unconfirmed write may outlive its row, so its row is only used when it is still listed. */
function describeTarget(mediaId: string): string {
  const row = store.items.find((item) => item.media_id === mediaId)
  return row ? describeDeletionContext(row.context) : `照片 ${mediaId.slice(0, 8)}`
}

function parentRoute(item: UndoableMediaDeletion) {
  const { kind, target_id: id } = item.context
  if (!id) return null
  if (kind === 'memory') return { name: 'memory-detail', params: { id } }
  return kind === 'album' ? { name: 'album-detail', params: { id } } : null
}
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">最近删除的照片</h1>
        <p class="page__subtitle">只有你自己删除的照片会出现在这里。每张照片可以在期限之前撤销删除，过期后不能恢复。</p>
      </div>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'us' }">返回</RouterLink>
    </header>

    <PhotoDeletionNotices />

    <!-- Every unconfirmed write, with what it captured. A retry resends exactly that. -->
    <ul v-if="unconfirmed.length" class="card list" data-test="deletions-unconfirmed">
      <li v-for="action in unconfirmed" :key="action.id" class="list__item" :data-test="`deletions-action-${action.kind}`">
        <div class="list__main">
          <div class="list__title">{{ action.kind === 'undo' ? '撤销删除' : '删除' }}：{{ describeTarget(action.mediaId) }}</div>
          <p class="hint hint--error" role="alert">{{ action.errorText }}</p>
        </div>
        <button class="btn btn--small" type="button" data-test="deletions-action-retry" @click="store.retry(action.id)">重试</button>
      </li>
    </ul>

    <div class="card" data-test="deletions-list" aria-live="polite" :aria-busy="store.status === 'loading'">
      <template v-if="store.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="deletions-error">{{ store.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="deletions-reload" @click="store.load()">重试</button>
      </template>
      <p v-else-if="store.status !== 'ready'" class="card__hint">加载中…</p>
      <p v-else-if="store.items.length === 0" class="card__hint" data-test="deletions-empty">没有可以恢复的照片。</p>
      <ul v-else class="list">
        <li v-for="item in store.items" :key="item.deletion_id" class="list__item" data-test="deletion-row" :data-deletion="item.deletion_id">
          <div class="list__main">
            <RouterLink v-if="parentRoute(item)" class="list__title message-link" :to="parentRoute(item)!" data-test="deletion-context">{{ describeDeletionContext(item.context) }}</RouterLink>
            <div v-else class="list__title" data-test="deletion-context">{{ describeDeletionContext(item.context) }}</div>
            <div class="list__meta" data-test="deletion-uploaded-at">上传于 {{ messageTime(item.uploaded_at) }}</div>
            <div class="list__meta" data-test="deletion-deleted-at">删除于 {{ messageTime(item.deleted_at) }}</div>
            <div class="list__meta" data-test="deletion-deadline">可撤销至 {{ messageTime(item.purge_after) }}（北京时间）</div>
          </div>
          <button class="btn btn--small btn--soft" type="button" :disabled="!!store.actions[`undo:${item.deletion_id}`]" data-test="deletion-undo"
            @click="store.undo(item.media_id, item.deletion_id)">撤销删除</button>
        </li>
      </ul>
      <p v-if="store.moreErrorText" class="hint hint--error" role="alert" data-test="deletions-more-error">{{ store.moreErrorText }}</p>
      <button v-if="store.status === 'ready' && store.nextCursor" class="btn btn--small btn--soft" type="button" data-test="deletions-more"
        :disabled="store.loadingMore" @click="store.loadMore()">{{ store.loadingMore ? '加载中…' : '加载更多' }}</button>
    </div>
  </section>
</template>
