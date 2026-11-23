<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { figureLayers, originalGender, type FigureSlots, type RoomCharacter } from '@/api/avatar-art'
import type { CharacterGender } from '@/api/avatar'

const props = withDefaults(defineProps<{
  slots: FigureSlots
  label?: string
  character?: RoomCharacter
  gender?: CharacterGender
  compact?: boolean
  bald?: boolean
  focus?: 'full' | 'head'
}>(), {
  label: '形象预览',
  focus: 'full',
})
const failed = ref<string[]>([])
const visibleSlots = computed(() => props.bald ? { ...props.slots, hair: null } : props.slots)
const resolvedGender = computed(() => props.gender ?? (props.character && originalGender(props.character)))
const plan = computed(() => figureLayers(visibleSlots.value, resolvedGender.value))
watch(() => plan.value.layers.map(l => l.src).join('|'), () => { failed.value = [] })
const missing = computed(() => plan.value.missing.length > 0 || failed.value.length > 0)
</script>

<template>
  <figure class="avatar-figure" :aria-label="label" data-test="avatar-figure">
    <div class="avatar-canvas" role="img" :aria-label="label">
      <div class="avatar-layers" :data-gender="resolvedGender" :class="{
        'avatar-layers--flipped': character === 'right',
        'avatar-layers--focus-head': focus === 'head',
      }">
        <img v-for="layer in plan.layers.filter(l => !failed.includes(l.src))" :key="layer.key" :src="layer.src" alt=""
          :data-layer="layer.key" draggable="false" @error="failed = [...new Set([...failed, layer.src])]" />
      </div>
    </div>
    <figcaption v-if="!compact" class="card__hint">{{ label }}</figcaption>
    <p v-if="missing" class="hint" role="status" data-test="avatar-art-fallback">部分素材不可用，显示可用部件；保存的装扮没有改变。</p>
    <button v-if="failed.length" type="button" class="btn btn--small btn--soft" @click="failed = []">重试图片</button>
  </figure>
</template>
