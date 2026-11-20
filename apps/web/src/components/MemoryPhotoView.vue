<script setup lang="ts">
/**
 * One memory photo, through the authenticated media route (P3-1c). The browser follows its redirect; script never sees the
 * signed URL. A missing, inaccessible or not yet available photo becomes a quiet placeholder, so a timeline or detail page keeps
 * working. Nothing is fetched by script and nothing is cached by it.
 *
 * D6: a photo the server says is deleted renders no image at all, so no media request is made for it.
 */
import { computed, ref, watch } from 'vue'

import { mediaContentUrl, type MediaVariant } from '@/api/media'

const props = withDefaults(defineProps<{ mediaId: string; variant?: MediaVariant; alt?: string; deleted?: boolean }>(), { variant: 'thumb', alt: '回忆照片', deleted: false })

const failed = ref(false)
const src = computed(() => (props.deleted ? null : mediaContentUrl(props.mediaId, props.variant)))

watch(src, () => { failed.value = false })
</script>

<template>
  <div class="memory-photo" :class="{ 'memory-photo--view': variant === 'view' }">
    <div v-if="src === null" class="memory-photo__missing" data-test="memory-photo-deleted">照片已删除</div>
    <div v-else-if="failed" class="memory-photo__missing" data-test="memory-photo-missing">照片暂时无法显示</div>
    <img v-else :src="src" :alt="alt" loading="lazy" decoding="async" referrerpolicy="no-referrer" data-test="memory-photo-image"
      @error="failed = true" />
  </div>
</template>
