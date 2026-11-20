<script setup lang="ts">
import { reactive, ref } from 'vue'
import type { PlanBody } from '@/api/shared-plans'

const props = defineProps<{ initial?: PlanBody; busy?: boolean; label: string }>()
const emit = defineEmits<{ submit: [body: PlanBody] }>()
const draft = reactive({ title: props.initial?.title ?? '', date: props.initial?.date ?? '', time: props.initial?.time ?? '',
  place: props.initial?.place ?? '', note: props.initial?.note ?? '' })
const error = ref('')
const count = (text: string) => Array.from(text).length

function submit(): void {
  if (props.busy) return
  const title = draft.title.trim()
  if (!title || count(title) > 80 || count(draft.place) > 120 || count(draft.note) > 2000) {
    error.value = '标题须为 1 到 80 个字符，地点最多 120 个字符，备注最多 2000 个字符。'
    return
  }
  if (draft.time && !draft.date) {
    error.value = '填写时间时，请同时选择日期。'
    return
  }
  error.value = ''
  emit('submit', { title, date: draft.date || null, time: draft.time || null, place: draft.place.trim() || null, note: draft.note.trim() || null })
}
</script>

<template>
  <form class="stack" data-test="plan-form" @submit.prevent="submit">
    <label class="field"><span class="field__label">标题</span>
      <input v-model="draft.title" class="field__input" :disabled="busy" required data-test="plan-title" />
    </label>
    <label class="field"><span class="field__label">日期（可选）</span>
      <input v-model="draft.date" class="field__input" type="date" :disabled="busy" data-test="plan-date" />
    </label>
    <label class="field"><span class="field__label">北京时间（可选）</span>
      <input v-model="draft.time" class="field__input" type="time" :disabled="busy" data-test="plan-time" />
    </label>
    <label class="field"><span class="field__label">地点（可选）</span>
      <input v-model="draft.place" class="field__input" :disabled="busy" />
    </label>
    <label class="field"><span class="field__label">备注（可选）</span>
      <textarea v-model="draft.note" class="field__input" rows="3" :disabled="busy"></textarea>
    </label>
    <p v-if="error" class="hint hint--error" role="alert">{{ error }}</p>
    <!-- D20: the create form puts its opt-in notice toggle here, above the save button. -->
    <slot />
    <button class="btn" type="submit" :disabled="busy">{{ label }}</button>
  </form>
</template>
