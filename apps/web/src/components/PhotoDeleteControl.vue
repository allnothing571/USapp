<script setup lang="ts">
/**
 * D6: deleting one's own photo. Only the parent knows who uploaded it, and it shows this only to the uploader. Separate from an album's
 * removal, which only hides a photo from that album. The explanation comes before the confirmation; an unconfirmed write keeps its
 * target and key, and only a click resends it. The parent remounts this by `key` on a change of photo or account.
 */
import { computed, ref } from 'vue'

import { RECOVERY_NAME, useMediaDeletionsStore } from '@/stores/media-deletions'

const props = withDefaults(defineProps<{ mediaId: string; disabled?: boolean }>(), { disabled: false })

const store = useMediaDeletionsStore()
const confirming = ref(false)
const action = computed(() => store.actions[`delete:${props.mediaId}`])

function confirm(): void {
  confirming.value = false
  void store.deletePhoto(props.mediaId)
}
</script>

<template>
  <div class="stack" data-test="photo-delete-control">
    <template v-if="action && action.status !== 'submitting'">
      <p class="hint hint--error" role="alert" data-test="photo-delete-pending">{{ action.errorText }}</p>
      <button class="btn btn--small" type="button" data-test="photo-delete-retry" @click="store.retry(action.id)">重试删除</button>
    </template>
    <p v-else-if="action" class="card__hint" data-test="photo-delete-submitting">正在删除…</p>
    <template v-else-if="confirming">
      <p class="card__hint" data-test="photo-delete-explain">
        删除后，应用不再提供这张照片，上传者本人也不能查看。7 天内你可以在“{{ RECOVERY_NAME }}”里撤销，之后不能恢复。之前发出的链接可能还能用一段时间，已经下载或保存的副本也不会因此消失。
      </p>
      <div class="row">
        <button class="btn btn--small" type="button" :disabled="disabled" data-test="photo-delete-confirm" @click="confirm()">确认删除</button>
        <button class="btn btn--small btn--ghost" type="button" data-test="photo-delete-cancel" @click="confirming = false">取消</button>
      </div>
    </template>
    <button v-else class="btn btn--small btn--ghost" type="button" :disabled="disabled" data-test="photo-delete" @click="confirming = true">删除照片</button>
  </div>
</template>
