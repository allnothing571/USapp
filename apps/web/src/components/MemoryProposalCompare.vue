<script setup lang="ts">
/**
 * The immutable original and suggestion of one memory edit request, side by side, with what differs marked. Read from the proposal
 * record. Used for the pending request and for a request from the history, whose original is the version it was based on.
 */
import { civilDateLabel } from '@/api/anniversary-rules'
import type { MemoryContent, MemoryProposal } from '@/api/types'
import MemoryPhotoView from '@/components/MemoryPhotoView.vue'

const props = withDefaults(defineProps<{ proposal: MemoryProposal; prefix?: string; originalLabel?: string }>(), {
  prefix: 'memory',
  originalLabel: '现在的内容',
})

function differs(a: MemoryContent, b: MemoryContent, field: 'date' | 'title' | 'description' | 'photos'): boolean {
  if (field !== 'photos') return a[field] !== b[field]
  return a.photos.map((p) => p.media_id).join() !== b.photos.map((p) => p.media_id).join()
}
</script>

<template>
  <div class="memory-compare" :data-test="`${props.prefix}-review`">
    <section v-for="side in (['original', 'proposed'] as const)" :key="side" class="stack" :data-test="`${props.prefix}-review-${side}`">
      <h3 class="list__title">{{ side === 'original' ? props.originalLabel : '建议改成' }}</h3>
      <dl class="kv">
        <dt class="kv__key">标题<span v-if="differs(proposal.original, proposal.proposed, 'title')" class="badge badge--accent" :data-test="`${props.prefix}-diff-title`">有变化</span></dt>
        <dd class="kv__value">{{ proposal[side].title }}</dd>
        <dt class="kv__key">日期<span v-if="differs(proposal.original, proposal.proposed, 'date')" class="badge badge--accent" :data-test="`${props.prefix}-diff-date`">有变化</span></dt>
        <dd class="kv__value">{{ civilDateLabel(proposal[side].date) }}</dd>
        <dt class="kv__key">描述<span v-if="differs(proposal.original, proposal.proposed, 'description')" class="badge badge--accent" :data-test="`${props.prefix}-diff-description`">有变化</span></dt>
        <dd class="kv__value message-body">{{ proposal[side].description || '（无）' }}</dd>
      </dl>
      <p class="kv__key">照片（{{ proposal[side].photos.length }} 张）<span v-if="differs(proposal.original, proposal.proposed, 'photos')" class="badge badge--accent" :data-test="`${props.prefix}-diff-photos`">有变化</span></p>
      <div class="memory-photos memory-photos--row">
        <MemoryPhotoView v-for="photo in proposal[side].photos" :key="photo.media_id" :media-id="photo.media_id" :deleted="photo.deleted" />
      </div>
    </section>
  </div>
</template>
