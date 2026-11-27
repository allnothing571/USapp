<script setup lang="ts">
/**
 * Shared albums list and creation (P3-2c, V1 §27): newest first, "load more" by cursor. An album may start empty, so creating one does
 * not depend on uploads. An unconfirmed create stays locked to the name it captured, and survives leaving and returning.
 */
import { computed, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'

import { ALBUM_NAME_MAX, checkAlbumName } from '@/api/album-rules'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import { useAlbumsStore, type AlbumResult } from '@/stores/albums'
import { useSessionStore } from '@/stores/session'
import { useSmartBack } from '@/router/back-navigation'

const back = useSmartBack('albums', 'us')
const session = useSessionStore()
const store = useAlbumsStore()
const router = useRouter()

const me = computed(() => session.user?.id ?? '')
const creating = computed(() => store.actions.new)
const name = ref('')
const nameError = ref('')
const shownName = computed(() => creating.value?.name ?? name.value)
const nameLength = computed(() => Array.from(shownName.value).length)

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id === previous) return
  name.value = ''
  nameError.value = ''
  if (id && session.isAuthenticated) void store.load()
})

onMounted(() => {
  if (session.isAuthenticated) void store.load()
})

async function finished(result: AlbumResult, owner: string | undefined): Promise<void> {
  if (result.outcome !== 'done' || session.user?.id !== owner) return
  name.value = ''
  await router.push({ name: 'album-detail', params: { id: result.response.album.id } })
}

async function submit(): Promise<void> {
  if (creating.value) return
  const checked = checkAlbumName(name.value)
  if (!checked.ok) {
    nameError.value = checked.message
    return
  }
  nameError.value = ''
  const owner = session.user?.id
  await finished(await store.create(checked.value), owner)
}

async function retryCreate(): Promise<void> {
  const owner = session.user?.id
  await finished(await store.retry('new'), owner)
}
</script>

<template>
  <section class="page">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="albums-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">共享相册</h1>
      <p class="subpage-header__subtitle">新建的排在前面，只有你们两个人能看到。</p>
    </header>

    <div v-if="store.status === 'ready' && store.uploadsEnabled === false" class="card card__hint" data-test="albums-uploads-off">
      照片上传暂未开放，现在不能选择新照片。相册仍可创建和查看，其中的照片可能暂时无法显示。
    </div>
    <div v-else-if="store.status === 'ready' && store.uploadsEnabled === null" class="card card__hint" data-test="albums-uploads-unknown">
      暂时无法确认照片上传是否可用。相册仍可创建和查看。
    </div>

    <form class="card stack" data-test="album-create-form" @submit.prevent="submit()">
      <div class="field">
        <label class="field__label" for="album-create-name">新相册名称（{{ nameLength }} / {{ ALBUM_NAME_MAX }}）</label>
        <input v-if="creating" id="album-create-name" class="field__input" type="text" :value="creating.name" disabled data-test="album-create-name" />
        <input v-else id="album-create-name" v-model="name" class="field__input" type="text" data-test="album-create-name" />
      </div>
      <p v-if="nameError" class="hint hint--error" role="alert" data-test="album-create-error">{{ nameError }}</p>
      <button class="btn btn--block" type="submit" :disabled="!!creating" data-test="album-create-submit">创建相册</button>
      <template v-if="creating && creating.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="album-create-pending">{{ creating.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="album-create-retry" @click="retryCreate()">重试创建</button>
      </template>
    </form>

    <ul v-if="store.notices.length" class="card list" data-test="albums-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <div class="card stack" data-test="albums-list" aria-live="polite" :aria-busy="store.status === 'loading'">
      <template v-if="store.status === 'error'">
        <p class="hint hint--error" role="alert" data-test="albums-error">{{ store.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" data-test="albums-reload" @click="store.load()">重试</button>
      </template>
      <p v-else-if="store.status !== 'ready'" class="card__hint">加载中…</p>
      <p v-else-if="store.items.length === 0" class="card__hint" data-test="albums-empty">还没有相册。</p>
      <ul v-else class="list">
        <li v-for="item in store.items" :key="item.id" class="list__item" :data-test="`album-${item.id}`">
          <RouterLink class="list__main message-link row album-row" :to="{ name: 'album-detail', params: { id: item.id } }">
            <MemoryPhotoView v-if="item.cover" :media-id="item.cover.media_id" alt="相册封面" />
            <div v-else class="memory-photo"><div class="memory-photo__missing" data-test="album-no-cover">暂无封面</div></div>
            <div class="spacer">
              <div class="list__title">{{ item.name }}</div>
              <div class="list__meta">{{ item.photo_count }} 张照片 · {{ item.creator_id === me ? '我创建的' : '对方创建的' }}</div>
            </div>
          </RouterLink>
          <span v-if="item.pending_proposal" class="badge badge--accent" data-test="album-pending-badge">有改名申请</span>
        </li>
      </ul>
      <p v-if="store.moreErrorText" class="hint hint--error" role="alert" data-test="albums-more-error">{{ store.moreErrorText }}</p>
      <button v-if="store.status === 'ready' && store.nextCursor" class="btn btn--small btn--soft" type="button" data-test="albums-more"
        :disabled="store.loadingMore" @click="store.loadMore()">{{ store.loadingMore ? '加载中…' : '加载更多' }}</button>
    </div>
  </section>
</template>
