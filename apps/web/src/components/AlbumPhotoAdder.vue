<script setup lang="ts">
/**
 * Adding photos to an album (P3-2c). Two separate steps, because each can fail on its own: uploading a file and getting it to READY,
 * then adding READY photos to the album, all or nothing. A refused add keeps the uploads, since it added nothing; a confirmed add
 * takes them out of the queue. An unconfirmed add stays locked to the ids it captured, and only its own retry resends them.
 *
 * Previously uploaded, still unattached photos can be chosen too. They are kept apart from the upload queue (no grant or file is
 * invented), joined with READY uploads in the order the user put them, without duplicates, and counted against the same per-add limit.
 * While an add is unconfirmed the selection is locked, so its retry resends exactly what it captured. A refused add keeps every choice
 * and says the list may have changed; a confirmed add drops the chosen ids it added.
 *
 * D20: one opt-in “通知对方” per batch, off on every fresh batch and never remembered; an unconfirmed add keeps the choice it captured.
 *
 * With uploads off or unknown the picker is replaced by an explanation; photos already READY here can still be added. Leaving the
 * route forgets this album's uploads and ignores their late results.
 */
import { computed, onBeforeUnmount, ref, watch } from 'vue'

import { ALBUM_MAX_PHOTOS_PER_ADD } from '@/api/album-rules'
import { MEDIA_TYPES } from '@/api/memory-rules'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import PartnerNoticeToggle from '@/components/PartnerNoticeToggle.vue'
import UnattachedPhotoPicker from '@/components/UnattachedPhotoPicker.vue'
import { useAlbumsStore } from '@/stores/albums'
import { useMemoryUploadsStore, type UploadItem, type UploadStatus } from '@/stores/memory-uploads'
import { useUnattachedStore } from '@/stores/unattached'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ albumId: string }>()

const albums = useAlbumsStore()
const uploads = useMemoryUploadsStore()
const capability = useMediaCapabilityStore()
const session = useSessionStore()

const scope = computed(() => `album:${props.albumId}`)
const queue = computed(() => uploads.items.filter((item) => item.scope === scope.value))
const addAction = computed(() => albums.actions[`add:${props.albumId}`])
/** Chosen previously uploaded photos, in the order chosen. Never part of the upload queue. */
const chosen = ref<string[]>([])
/** The user's order across both kinds: `m:<media id>` for a chosen photo, `u:<upload id>` for an upload. */
const order = ref<string[]>([])
const orderedKeys = computed(() => {
  const known = new Set(order.value)
  const live = order.value.filter((key) => (key.startsWith('m:') ? chosen.value.includes(key.slice(2)) : queue.value.some((item) => `u:${item.id}` === key)))
  return [...live, ...queue.value.map((item) => `u:${item.id}`).filter((key) => !known.has(key))]
})
/** What an add sends: chosen photos and READY uploads, in the user's order, each id once. */
const readyIds = computed(() => {
  const ids: string[] = []
  for (const key of orderedKeys.value) {
    const id = key.startsWith('m:') ? key.slice(2) : queue.value.find((item) => `u:${item.id}` === key && item.status === 'ready')?.mediaId
    if (id && !ids.includes(id)) ids.push(id)
  }
  return ids
})
const heldIds = computed(() => [...new Set([...chosen.value, ...queue.value.flatMap((item) => (item.mediaId ? [item.mediaId] : []))])])
const room = computed(() => ALBUM_MAX_PHOTOS_PER_ADD - queue.value.length - chosen.value.length)
const pickerError = ref('')
const refusedNote = ref('')
const notify = ref(false)
const partnerPresent = computed(() => (session.space?.member_count ?? 0) >= 2)
const shownNotify = computed(() => (addAction.value ? addAction.value.notify === true : notify.value))
// Another album or another account starts a fresh batch.
watch([() => props.albumId, () => session.user?.id ?? null], () => { notify.value = false })

const STATUS_TEXT: Record<UploadStatus, string> = {
  queued: '等待上传…', granting: '准备上传…', grant_unknown: '', uploading: '正在上传…', interrupted: '', completing: '正在检查…',
  unconfirmed: '', ready: '已上传，还没有加入相册', rejected: '', expired: '', failed: '', disabled: '',
}

const RETRY_LABEL: Partial<Record<UploadStatus, string>> = {
  grant_unknown: '重试', interrupted: '继续上传', unconfirmed: '重试确认', failed: '重试',
}

function statusText(upload: UploadItem): string {
  return upload.errorText || STATUS_TEXT[upload.status]
}

function pick(event: Event): void {
  const input = event.target as HTMLInputElement
  const files = Array.from(input.files ?? [])
  input.value = ''
  pickerError.value = ''
  const left = room.value
  if (files.length > left) pickerError.value = `一次最多 ${ALBUM_MAX_PHOTOS_PER_ADD} 张照片，多出的没有加入。`
  const added = uploads.add(scope.value, files.slice(0, Math.max(left, 0)))
  order.value = [...order.value, ...added.map((item) => `u:${item.id}`)]
}

function chooseUnattached(ids: string[]): void {
  pickerError.value = ''
  refusedNote.value = ''
  const held = new Set(heldIds.value)
  const fresh = ids.filter((id, index) => !held.has(id) && ids.indexOf(id) === index)
  if (fresh.length > room.value) pickerError.value = `一次最多 ${ALBUM_MAX_PHOTOS_PER_ADD} 张照片，多出的没有加入。`
  const taken = fresh.slice(0, Math.max(room.value, 0))
  chosen.value = [...chosen.value, ...taken]
  order.value = [...order.value, ...taken.map((id) => `m:${id}`)]
}

function dropChosen(id: string): void {
  chosen.value = chosen.value.filter((x) => x !== id)
  order.value = order.value.filter((key) => key !== `m:${id}`)
}

async function addToAlbum(): Promise<void> {
  if (!readyIds.value.length) return
  refusedNote.value = ''
  const albumId = props.albumId
  const sent = [...readyIds.value]
  const result = await albums.addPhotos(albumId, sent, notify.value && partnerPresent.value)
  if (props.albumId !== albumId) return
  if (result.outcome === 'done') {
    notify.value = false
    const added = new Set(sent)
    chosen.value = chosen.value.filter((id) => !added.has(id))
    order.value = order.value.filter((key) => !(key.startsWith('m:') && added.has(key.slice(2))))
  } else if (result.outcome === 'rejected' && result.errorCode === 'ALBUM_PHOTO_UNAVAILABLE') {
    refusedNote.value = '有照片已不能加入，可能已被清理、删除或用在别处，之前看到的列表可能已经变化。所选照片都还保留，请移除不可用的那张后再加入。'
    if (useUnattachedStore().status !== 'idle') void useUnattachedStore().load()
  }
}

// A chosen photo that the album now shows was added, also when an unconfirmed add was confirmed later by its own retry.
watch(() => (albums.photos.albumId === props.albumId ? albums.photos.items.map((p) => p.media_id) : []), (inAlbum) => {
  const present = new Set(inAlbum)
  if (!chosen.value.some((id) => present.has(id))) return
  chosen.value = chosen.value.filter((id) => !present.has(id))
  order.value = order.value.filter((key) => !(key.startsWith('m:') && present.has(key.slice(2))))
})

onBeforeUnmount(() => uploads.clearScope(scope.value))
</script>

<template>
  <div class="card stack" data-test="album-adder">
    <h2 class="card__title">加入照片</h2>
    <p class="card__hint">先上传，再加入相册。一次最多 {{ ALBUM_MAX_PHOTOS_PER_ADD }} 张，要么全部加入，要么都不加入。加入后不能移到别的相册。</p>

    <ul v-if="chosen.length" class="list" data-test="album-chosen">
      <li v-for="id in chosen" :key="id" class="list__item memory-editor__photo" data-test="album-chosen-item" :data-media="id">
        <MemoryPhotoView :media-id="id" alt="之前上传的照片" />
        <div class="list__main">
          <p class="card__hint">之前上传的照片，还没有加入相册</p>
          <button class="btn btn--small btn--ghost" type="button" :disabled="!!addAction" data-test="album-chosen-remove" @click="dropChosen(id)">不加这张</button>
        </div>
      </li>
    </ul>

    <ul v-if="queue.length" class="list" data-test="album-uploads">
      <li v-for="upload in queue" :key="upload.id" class="list__item memory-editor__photo" data-test="album-upload">
        <MemoryPhotoView v-if="upload.status === 'ready' && upload.mediaId" :media-id="upload.mediaId" alt="相册照片" />
        <div v-else class="memory-photo"><div class="memory-photo__missing">还不能显示</div></div>
        <div class="list__main">
          <p :class="upload.errorText ? 'hint hint--error' : 'card__hint'" data-test="album-upload-status" :data-status="upload.status">{{ statusText(upload) }}</p>
          <div class="row">
            <button v-if="RETRY_LABEL[upload.status] && !upload.local" class="btn btn--small" type="button" data-test="album-upload-retry"
              @click="uploads.retry(upload.id)">{{ RETRY_LABEL[upload.status] }}</button>
            <button v-if="upload.status === 'expired'" class="btn btn--small" type="button" data-test="album-upload-again"
              @click="uploads.uploadAgain(upload.id)">重新上传</button>
            <button class="btn btn--small btn--ghost" type="button" :disabled="!!addAction" data-test="album-upload-remove"
              @click="uploads.remove(upload.id)">不加这张</button>
          </div>
        </div>
      </li>
    </ul>

    <input v-if="capability.uploadsEnabled === true" class="field__input" type="file" multiple :accept="MEDIA_TYPES.join(',')" :disabled="room <= 0"
      data-test="album-files" aria-label="选择照片" @change="pick" />
    <p v-else-if="capability.uploadsEnabled === false" class="card__hint" data-test="album-uploads-off">照片上传暂未开放，现在不能选择新照片。相册里已有的照片和其他操作不受影响。</p>
    <p v-else-if="capability.status === 'loading'" class="card__hint" data-test="album-uploads-loading">正在确认照片上传是否可用…</p>
    <template v-else>
      <p class="card__hint" data-test="album-uploads-unknown">暂时无法确认照片上传是否可用。{{ capability.errorText }}</p>
      <button class="btn btn--small btn--soft" type="button" data-test="album-uploads-recheck" @click="capability.load()">重新检查</button>
    </template>
    <UnattachedPhotoPicker name="album" :exclude="heldIds" :room="room" :disabled="!!addAction" @choose="chooseUnattached" />
    <p v-if="pickerError" class="hint hint--error" role="alert" data-test="album-picker-error">{{ pickerError }}</p>
    <p v-if="refusedNote" class="hint hint--error" role="alert" data-test="album-refused-note">{{ refusedNote }}</p>

    <PartnerNoticeToggle v-if="queue.length || chosen.length || addAction" name="album-add" :model-value="shownNotify"
      :locked="!!addAction" :unavailable="!partnerPresent" @update:model-value="notify = $event" />
    <button v-if="queue.length || chosen.length" class="btn btn--block" type="button" :disabled="!readyIds.length || !!addAction" data-test="album-add-submit" @click="addToAlbum()">
      {{ addAction ? '等待这次加入的结果' : `加入相册（${readyIds.length} 张）` }}
    </button>
  </div>
</template>
