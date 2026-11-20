<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { figureLayers, type FigureSlots } from '@/api/avatar-art'

const props = withDefaults(defineProps<{ slots: FigureSlots; label?: string }>(), { label: '占位形象预览' })
const failed = ref<string[]>([])
const plan = computed(() => figureLayers(props.slots))
watch(() => plan.value.layers.map(l => l.src).join('|'), () => { failed.value = [] })
const missing = computed(() => plan.value.missing.length > 0 || failed.value.length > 0)
</script>

<template>
  <figure class="avatar-figure" :aria-label="label" data-test="avatar-figure">
    <div class="avatar-canvas" role="img" :aria-label="label">
      <img v-for="layer in plan.layers.filter(l => !failed.includes(l.src))" :key="layer.src" :src="layer.src" alt=""
        :data-layer="layer.key" draggable="false" @error="failed = [...new Set([...failed, layer.src])]" />
    </div>
    <figcaption class="card__hint">{{ label }} · 占位素材，非正式美术</figcaption>
    <p v-if="missing" class="hint" role="status" data-test="avatar-art-fallback">部分素材不可用，显示基础占位形象；保存的装扮没有改变。</p>
    <button v-if="failed.length" type="button" class="btn btn--small btn--soft" @click="failed = []">重试图片</button>
  </figure>
</template>
