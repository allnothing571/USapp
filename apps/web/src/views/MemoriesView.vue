<script setup lang="ts">
/**
 * Photo memories timeline (P3-1c, V1 §47): newest memory date first, "load more" by cursor. Existing memories stay readable whatever
 * the upload state, and a missing photo becomes a placeholder rather than breaking its row.
 */
import { computed, onMounted, watch } from 'vue'

import { civilDateLabel } from '@/api/anniversary-rules'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import { useMemoriesStore } from '@/stores/memories'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const store = useMemoriesStore()

const me = computed(() => session.user?.id ?? '')

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id !== previous && id && session.isAuthenticated) void store.load()
})

onMounted(() => {
  if (session.isAuthenticated) void store.load()
})
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">照片回忆</h1>
        <p class="page__subtitle">按回忆的日期排列，只有你们两个人能看到。</p>
      </div>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'us' }">返回</RouterLink>
    </header>

    <RouterLink class="btn btn--small btn--soft" :to="{ name: 'memory-deletions' }">最近删除的回忆</RouterLink>
    <div v-if="store.status === 'ready' && store.uploadsEnabled === false" class="card card__hint" data-test="memories-uploads-off">
      照片上传暂未开放，现在不能添加新回忆。已有的回忆仍可查看，其中的照片可能暂时无法显示。
    </div>
    <div v-else-if="store.status === 'ready' && store.uploadsEnabled === null" class="card card__hint" data-test="memories-uploads-unknown">
      暂时无法确认照片上传是否可用。已有的回忆仍可查看。
    </div>
    <RouterLink v-else-if="store.status === 'ready'" class="btn btn--block" :to="{ name: 'memory-new' }" data-test="memories-new">添加回忆</RouterLink>

    <ul v-if="store.notices.length" class="card list" data-test="memories-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <div class="card stack" data-test="memories-timeline" aria-live="polite" :aria-busy="store.status === 'loading'">
      <template v-if="store.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="memories-error">{{ store.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="memories-reload" @click="store.load()">重试</button>
      </template>
      <p v-else-if="store.status !== 'ready'" class="card__hint">加载中…</p>
      <p v-else-if="store.items.length === 0" class="card__hint" data-test="memories-empty">还没有回忆。</p>
      <ul v-else class="list">
        <li v-for="item in store.items" :key="item.id" class="list__item memory-row" :data-test="`memory-${item.id}`">
          <RouterLink class="list__main message-link" :to="{ name: 'memory-detail', params: { id: item.id } }">
            <div class="memory-photos memory-photos--row">
              <MemoryPhotoView v-for="photo in item.photos.slice(0, 3)" :key="photo.media_id" :media-id="photo.media_id" :deleted="photo.deleted" />
            </div>
            <div class="list__title">{{ item.title }}</div>
            <div class="list__meta">{{ civilDateLabel(item.date) }} · {{ item.photos.length }} 张照片 · {{ item.creator_id === me ? '我添加的' : '对方添加的' }}</div>
          </RouterLink>
          <span v-if="item.pending_proposal" class="badge badge--accent">有修改申请</span>
        </li>
      </ul>
      <p v-if="store.moreErrorText" class="hint hint--error" role="alert" data-test="memories-more-error">{{ store.moreErrorText }}</p>
      <button v-if="store.status === 'ready' && store.nextCursor" class="btn btn--small btn--soft" type="button" data-test="memories-more"
        :disabled="store.loadingMore" @click="store.loadMore()">{{ store.loadingMore ? '加载中…' : '加载更多' }}</button>
    </div>
  </section>
</template>
