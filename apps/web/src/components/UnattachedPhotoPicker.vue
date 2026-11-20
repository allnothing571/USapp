<script setup lang="ts">
/**
 * "Choose previously uploaded photos": the caller's own READY uploads that have not been attached anywhere yet, read when opened.
 *
 * - Not a recovery of anything deleted: deleted photos and deleted memories have their own pages, and a photo already in a memory or an
 *   album (even one removed from an album) is never offered again.
 * - A snapshot: choosing reserves nothing and does not extend how long an unattached photo is kept. The save that follows decides, and may
 *   refuse a photo that was cleaned up, deleted or used elsewhere meanwhile; the other choices stay in the form.
 * - Emits ids in the order they were ticked. `exclude` are ids the form already has; `room` is how many more it can take.
 */
import { computed, ref } from 'vue'

import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import { useUnattachedStore } from '@/stores/unattached'

const props = withDefaults(defineProps<{ name: string; exclude?: string[]; room: number; disabled?: boolean }>(), { exclude: () => [], disabled: false })
const emit = defineEmits<{ choose: [ids: string[]] }>()

const store = useUnattachedStore()
const open = ref(false)
const picked = ref<string[]>([])

const excluded = computed(() => new Set(props.exclude))
const offered = computed(() => store.items.filter((photo) => !excluded.value.has(photo.media_id)))

function toggleOpen(): void {
  open.value = !open.value
  picked.value = []
  if (open.value) void store.load()
}

function toggle(id: string): void {
  if (picked.value.includes(id)) picked.value = picked.value.filter((x) => x !== id)
  else if (picked.value.length < props.room) picked.value = [...picked.value, id]
}

function confirm(): void {
  const ids = picked.value.filter((id) => !excluded.value.has(id)).slice(0, Math.max(props.room, 0))
  if (ids.length) emit('choose', ids)
  picked.value = []
  open.value = false
}
</script>

<template>
  <div class="stack" :data-test="`${name}-unattached`">
    <button class="btn btn--small btn--soft" type="button" :disabled="disabled" :data-test="`${name}-unattached-toggle`" @click="toggleOpen()">
      {{ open ? '收起之前上传的照片' : '选择之前上传的照片' }}
    </button>
    <div v-if="open && !disabled" class="stack" :data-test="`${name}-unattached-panel`">
      <p class="card__hint" :data-test="`${name}-unattached-note`">
        这里是你自己上传过、但还没有用在任何回忆或相册里的照片，不包括已删除的照片。打开时查看的列表可能已经变化：选择不会保留照片，也不会延长它的保存时间；保存时如果某张已被清理、删除或用在别处，会被拒绝，其他照片仍保留在表单里。
      </p>
      <template v-if="store.status === 'error'">
        <p class="hint hint--error" role="alert" :data-test="`${name}-unattached-error`">{{ store.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" :data-test="`${name}-unattached-retry`" @click="store.load()">重试</button>
      </template>
      <p v-else-if="store.status !== 'ready'" class="card__hint">加载中…</p>
      <p v-else-if="!offered.length" class="card__hint" :data-test="`${name}-unattached-empty`">没有可以再选择的照片。</p>
      <ul v-else class="list" :data-test="`${name}-unattached-list`">
        <li v-for="photo in offered" :key="photo.media_id" class="list__item memory-editor__photo" :data-test="`${name}-unattached-item`" :data-media="photo.media_id">
          <MemoryPhotoView :media-id="photo.media_id" />
          <label class="row">
            <input type="checkbox" :checked="picked.includes(photo.media_id)" :disabled="!picked.includes(photo.media_id) && picked.length >= room"
              :data-test="`${name}-unattached-pick`" @change="toggle(photo.media_id)" />
            <span class="list__meta">{{ picked.includes(photo.media_id) ? `第 ${picked.indexOf(photo.media_id) + 1} 张` : '选择' }}</span>
          </label>
        </li>
      </ul>
      <p v-if="store.moreErrorText" class="hint hint--error" role="alert">{{ store.moreErrorText }}</p>
      <button v-if="store.status === 'ready' && store.nextCursor" class="btn btn--small btn--soft" type="button" :disabled="store.loadingMore"
        :data-test="`${name}-unattached-more`" @click="store.loadMore()">{{ store.loadingMore ? '加载中…' : '加载更多' }}</button>
      <p v-if="room <= 0" class="card__hint" :data-test="`${name}-unattached-full`">已经达到照片数量上限，先移除一张才能再选。</p>
      <button class="btn btn--small" type="button" :disabled="!picked.length" :data-test="`${name}-unattached-confirm`" @click="confirm()">
        加入所选（{{ picked.length }} 张）
      </button>
    </div>
  </div>
</template>
