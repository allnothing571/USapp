<script setup lang="ts">
/**
 * The form for a memory (P3-1c): date, title, description and ordered photos. Used for create, creator edit and partner request.
 *
 * A photo joins the submission only when the backend has confirmed it READY, or it is already this memory's. Each upload's failure
 * shows its own retry; nothing retries by itself. With uploads off, unknown, loading or unreadable (the capability store) the file picker
 * is replaced by an explanation; photos already on this memory can still be reordered or removed, and previously uploaded photos can still
 * be chosen.
 *
 * Previously uploaded photos that are still unattached can be chosen too. They join the order as photos already known (`m:`), never as
 * uploads, and an id already in the form is not offered or added twice. The photo limit counts every entry.
 *
 * The draft belongs to the parent, which remounts this component by `key` to clear it. While an action exists the form is locked and
 * shows what that action captured. Leaving the route forgets this form's uploads and ignores their late results.
 */
import { computed, onBeforeUnmount, reactive, ref } from 'vue'

import { beijingToday, checkMemoryTerms, MEDIA_TYPES, MEMORY_DESCRIPTION_MAX, MEMORY_MAX_PHOTOS, MEMORY_MIN_DATE, MEMORY_TITLE_MAX } from '@/api/memory-rules'
import type { MemoryTerms } from '@/api/types'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'
import UnattachedPhotoPicker from '@/components/UnattachedPhotoPicker.vue'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useMemoryUploadsStore, type UploadItem, type UploadStatus } from '@/stores/memory-uploads'

const props = withDefaults(defineProps<{
  /** Prefix for data-test attributes and input ids. */
  name: string
  /** Which uploads belong to this form. */
  scope: string
  submitLabel: string
  initial?: MemoryTerms | null
  busy?: boolean
}>(), { initial: null, busy: false })

const emit = defineEmits<{ submit: [terms: MemoryTerms] }>()

const capability = useMediaCapabilityStore()
const uploads = useMemoryUploadsStore()

const draft = reactive({ date: props.initial?.date ?? '', title: props.initial?.title ?? '', description: props.initial?.description ?? '' })
/** Display order: `m:<media id>` for a photo already known, `u:<upload id>` for an upload in this form. */
const order = ref<string[]>((props.initial?.media_ids ?? []).map((id) => `m:${id}`))
const fieldError = ref('')
const pickerError = ref('')
const today = beijingToday()

const titleLength = computed(() => Array.from(draft.title).length)
const descriptionLength = computed(() => Array.from(draft.description).length)

interface Entry {
  key: string
  mediaId: string | null
  upload: UploadItem | null
}

const entries = computed<Entry[]>(() => order.value.flatMap((key): Entry[] => {
  if (key.startsWith('m:')) return [{ key, mediaId: key.slice(2), upload: null }]
  const upload = uploads.items.find((item) => item.id === key.slice(2))
  if (!upload) return []
  return [{ key, mediaId: upload.status === 'ready' ? upload.mediaId : null, upload }]
}))

const STATUS_TEXT: Record<UploadStatus, string> = {
  queued: '等待上传…', granting: '准备上传…', grant_unknown: '', uploading: '正在上传…', interrupted: '', completing: '正在检查…',
  unconfirmed: '', ready: '', rejected: '', expired: '', failed: '', disabled: '',
}

function statusText(upload: UploadItem): string {
  return upload.errorText || STATUS_TEXT[upload.status]
}

const RETRY_LABEL: Partial<Record<UploadStatus, string>> = {
  grant_unknown: '重试', interrupted: '继续上传', unconfirmed: '重试确认', failed: '重试',
}

function pick(event: Event): void {
  const input = event.target as HTMLInputElement
  const files = Array.from(input.files ?? [])
  input.value = ''
  pickerError.value = ''
  const room = MEMORY_MAX_PHOTOS - order.value.length
  if (files.length > room) pickerError.value = `每条回忆最多 ${MEMORY_MAX_PHOTOS} 张照片，多出的没有加入。`
  const added = uploads.add(props.scope, files.slice(0, Math.max(room, 0)))
  order.value = [...order.value, ...added.map((item) => `u:${item.id}`)]
}

/** Every media id the form already holds: its known photos and its READY uploads. */
const heldIds = computed(() => entries.value.flatMap((entry) => (entry.mediaId ? [entry.mediaId] : [])))

/** Previously uploaded photos, in the order they were ticked, after what is already here. */
function chooseUnattached(ids: string[]): void {
  pickerError.value = ''
  const held = new Set(heldIds.value)
  const fresh = ids.filter((id, index) => !held.has(id) && ids.indexOf(id) === index)
  const room = MEMORY_MAX_PHOTOS - order.value.length
  if (fresh.length > room) pickerError.value = `每条回忆最多 ${MEMORY_MAX_PHOTOS} 张照片，多出的没有加入。`
  order.value = [...order.value, ...fresh.slice(0, Math.max(room, 0)).map((id) => `m:${id}`)]
}

function move(index: number, delta: number): void {
  const next = [...order.value]
  const target = index + delta
  if (target < 0 || target >= next.length) return
  ;[next[index], next[target]] = [next[target], next[index]]
  order.value = next
}

function removeEntry(entry: Entry): void {
  if (entry.upload) uploads.remove(entry.upload.id)
  order.value = order.value.filter((key) => key !== entry.key)
}

function submit(): void {
  if (props.busy) return
  if (entries.value.some((entry) => !entry.mediaId)) {
    fieldError.value = '还有照片没有确认可用。请等它完成、重试，或先移除它。'
    return
  }
  const checked = checkMemoryTerms({ ...draft, mediaIds: entries.value.map((entry) => entry.mediaId as string) })
  if (!checked.ok) {
    fieldError.value = checked.message
    return
  }
  fieldError.value = ''
  emit('submit', checked.terms)
}

onBeforeUnmount(() => uploads.clearScope(props.scope))
</script>

<template>
  <form class="stack" :data-test="`${name}-form`" @submit.prevent="submit()">
    <div class="field">
      <label class="field__label" :for="`${name}-date`">日期</label>
      <input :id="`${name}-date`" v-model="draft.date" class="field__input" type="date" :min="MEMORY_MIN_DATE" :max="today" :disabled="busy"
        :data-test="`${name}-date`" />
    </div>
    <div class="field">
      <label class="field__label" :for="`${name}-title`">标题（{{ titleLength }} / {{ MEMORY_TITLE_MAX }}）</label>
      <input :id="`${name}-title`" v-model="draft.title" class="field__input" type="text" :disabled="busy" :data-test="`${name}-title`" />
    </div>
    <div class="field">
      <label class="field__label" :for="`${name}-description`">描述（可选，{{ descriptionLength }} / {{ MEMORY_DESCRIPTION_MAX }}）</label>
      <textarea :id="`${name}-description`" v-model="draft.description" class="field__input" rows="3" :disabled="busy"
        :data-test="`${name}-description`"></textarea>
    </div>

    <div class="field">
      <span class="field__label">照片（{{ entries.length }} / {{ MEMORY_MAX_PHOTOS }}，按这个顺序展示）</span>
      <ul v-if="entries.length" class="list" :data-test="`${name}-photos`">
        <li v-for="(entry, index) in entries" :key="entry.key" class="list__item memory-editor__photo" :data-test="`${name}-photo`">
          <MemoryPhotoView v-if="entry.mediaId" :media-id="entry.mediaId" />
          <div v-else class="memory-photo"><div class="memory-photo__missing">还不能显示</div></div>
          <div class="list__main">
            <p v-if="entry.upload && !entry.mediaId" :class="entry.upload.errorText ? 'hint hint--error' : 'card__hint'"
              :data-test="`${name}-upload-status`" :data-status="entry.upload.status">{{ statusText(entry.upload) }}</p>
            <div class="row">
              <button v-if="entry.upload && RETRY_LABEL[entry.upload.status] && !entry.upload.local" class="btn btn--small" type="button"
                :data-test="`${name}-upload-retry`" @click="uploads.retry(entry.upload.id)">{{ RETRY_LABEL[entry.upload.status] }}</button>
              <button v-if="entry.upload && entry.upload.status === 'expired'" class="btn btn--small" type="button"
                :data-test="`${name}-upload-again`" @click="uploads.uploadAgain(entry.upload.id)">重新上传</button>
              <template v-if="!busy">
                <button class="btn btn--small btn--ghost" type="button" :disabled="index === 0" :data-test="`${name}-photo-up`"
                  @click="move(index, -1)">上移</button>
                <button class="btn btn--small btn--ghost" type="button" :disabled="index === entries.length - 1" :data-test="`${name}-photo-down`"
                  @click="move(index, 1)">下移</button>
                <button class="btn btn--small btn--ghost" type="button" :data-test="`${name}-photo-remove`" @click="removeEntry(entry)">移除</button>
              </template>
            </div>
          </div>
        </li>
      </ul>
      <template v-if="!busy">
        <input v-if="capability.uploadsEnabled === true" :id="`${name}-files`" class="field__input" type="file" multiple :accept="MEDIA_TYPES.join(',')"
          :disabled="entries.length >= MEMORY_MAX_PHOTOS" :data-test="`${name}-files`" aria-label="选择照片" @change="pick" />
        <p v-else-if="capability.uploadsEnabled === false" class="card__hint" :data-test="`${name}-uploads-off`">
          照片上传暂未开放，现在不能添加新照片。已有的照片可以调整顺序或移除。
        </p>
        <p v-else-if="capability.status === 'loading'" class="card__hint" :data-test="`${name}-uploads-loading`">正在确认照片上传是否可用…</p>
        <template v-else>
          <p class="card__hint" :data-test="`${name}-uploads-unknown`">暂时无法确认照片上传是否可用。{{ capability.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" :data-test="`${name}-uploads-recheck`" @click="capability.load()">重新检查</button>
        </template>
      </template>
      <UnattachedPhotoPicker v-if="!busy" :name="name" :exclude="heldIds" :room="MEMORY_MAX_PHOTOS - entries.length" @choose="chooseUnattached" />
      <p v-if="pickerError" class="hint hint--error" role="alert" :data-test="`${name}-picker-error`">{{ pickerError }}</p>
    </div>

    <p v-if="fieldError" class="hint hint--error" role="alert" :data-test="`${name}-error`">{{ fieldError }}</p>
    <!-- D20: the new-memory page puts its opt-in notice toggle here, above the save button. -->
    <slot />
    <button class="btn btn--block" type="submit" :disabled="busy" :data-test="`${name}-submit`">{{ submitLabel }}</button>
  </form>
</template>
