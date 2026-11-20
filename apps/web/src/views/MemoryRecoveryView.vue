<script setup lang="ts">
import { onBeforeUnmount, watch } from 'vue'
import { messageTime } from '@/api/message-rules'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import PhotoDeleteControl from '@/components/PhotoDeleteControl.vue'
import PhotoDeletionNotices from '@/components/PhotoDeletionNotices.vue'
import { useMemoryDeletionsStore } from '@/stores/memory-deletions'
import { useMediaDeletionsStore } from '@/stores/media-deletions'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ kind: 'recovery' | 'retained' }>()
const store = useMemoryDeletionsStore()
const photos = useMediaDeletionsStore()
const session = useSessionStore()
watch([() => props.kind, () => session.user?.id], ([kind, owner]) => {
  if (owner) void store.load(kind)
}, { immediate: true })
watch(() => photos.notices, () => {
  if (props.kind === 'retained' && session.user) void store.load('retained')
})
onBeforeUnmount(() => store.close())
</script>

<template>
  <section class="page">
    <header class="row">
      <h1 class="page__title spacer">{{ kind === 'recovery' ? '最近删除的回忆' : '保留的照片' }}</h1>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'us' }">返回</RouterLink>
    </header>
    <p class="card__hint">{{ kind === 'recovery' ? '这里只显示你自己删除且当前可以撤销的轮次，以服务器处理时的状态为准。' :
      '所属回忆已删除，这些由你上传的照片仍保留并占用空间。原绑定不变，无法加入其他回忆或相册。' }}</p>
    <p v-if="store.notice" class="card" role="status">{{ store.notice }}</p>
    <PhotoDeletionNotices v-if="kind === 'retained'" />
    <div v-for="(action, slot) in store.actions" :key="slot" class="card stack" data-test="memory-pending-deletion">
      <p>原回忆 {{ action.command.memoryId }} 的{{ action.command.kind === 'undo' ? '撤销' : '删除相关' }}请求</p>
      <p class="hint hint--error">{{ action.error || '正在提交…' }}</p>
      <button v-if="action.state !== 'sending'" class="btn btn--small" type="button" @click="store.retry(slot)">重试原请求</button>
    </div>
    <div v-if="store.error" class="card stack" role="alert">
      <p>{{ store.error }}</p>
      <button class="btn btn--small" type="button" @click="store.load(kind)">重新加载</button>
    </div>
    <p v-if="store.loading" class="card__hint">加载中…</p>
    <p v-else-if="!store.error && !(kind === 'recovery' ? store.recovery.length : store.retained.length)" class="card__hint">暂无记录。</p>
    <template v-if="kind === 'recovery'">
      <article v-for="row in store.recovery" :key="row.deletion_id" class="card stack" data-test="memory-recovery-row">
        <h2 class="card__title">{{ row.title }}</h2>
        <p>删除于 {{ messageTime(row.deleted_at) }}</p>
        <p>可撤销至 {{ messageTime(row.purge_after) }}（北京时间）</p>
        <p class="card__hint">撤销会同时处理这一轮关联的照片；事先单独删除的照片保持原状态。</p>
        <button class="btn btn--small" type="button" :disabled="!!store.actions[row.memory_id]" data-test="memory-undo"
          @click="store.submit({ kind: 'undo', memoryId: row.memory_id, deletionId: row.deletion_id })">撤销该次删除</button>
      </article>
    </template>
    <template v-else>
      <article v-for="row in store.retained" :key="row.media_id" class="card stack" data-test="retained-photo">
        <MemoryPhotoView :media-id="row.media_id" />
        <p class="card__hint">上传于 {{ messageTime(row.uploaded_at) }}</p>
        <PhotoDeleteControl :key="row.media_id + ':' + session.user?.id" :media-id="row.media_id" />
      </article>
    </template>
    <button v-if="store.cursor" class="btn btn--soft" type="button" :disabled="store.loading" @click="store.load(kind, true)">加载更多</button>
  </section>
</template>
