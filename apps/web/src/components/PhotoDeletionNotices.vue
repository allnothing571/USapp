<script setup lang="ts">
/** D6: what a photo deletion or undo came to, shown wherever one can be started. */
import { useMediaDeletionsStore } from '@/stores/media-deletions'

const store = useMediaDeletionsStore()
</script>

<template>
  <ul v-if="store.notices.length" class="card list" aria-live="polite" data-test="photo-deletion-notices">
    <li v-for="notice in store.notices" :key="notice.id" class="list__item">
      <div class="list__main">
        <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'" data-test="photo-deletion-notice">{{ notice.text }}</div>
      </div>
      <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
    </li>
  </ul>
</template>
