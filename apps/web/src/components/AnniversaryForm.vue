<script setup lang="ts">
/**
 * 纪念日表单（阶段 2 P2-2）：标题、日期、类型、备注。只做即时提示并提交规范化后的完整内容；
 * 权威校验在服务端。日期用 `<input type="date">` 的字符串，不转换成 Date。
 * 草稿属于父页面：父页面在目标或账号变化时用新的 `key` 重新挂载本组件来清空它。
 * P2-3b：创建与创建者直接修改带“通知对方”开关（修改申请没有，它是强制通知）；提交时捕获明确的布尔值。
 * 没有对方时开关禁用，提交 false。已有动作（提交中或结果未确认）时显示该动作捕获的选择。
 */
import { computed, reactive, ref } from 'vue'

import {
  ANNIVERSARY_MAX_DATE,
  ANNIVERSARY_MIN_DATE,
  ANNIVERSARY_NOTE_MAX,
  ANNIVERSARY_TITLE_MAX,
  checkAnniversaryTerms,
  type TermsDraft,
} from '@/api/anniversary-rules'
import type { AnniversaryTerms } from '@/api/types'
import NotifyPartnerToggle from '@/components/NotifyPartnerToggle.vue'

const props = withDefaults(defineProps<{
  initial?: AnniversaryTerms | null
  submitLabel: string
  busy?: boolean
  /** Prefix for data-test attributes and input ids. */
  name: string
  /** P2-3b: create or creator edit only; absent for proposals. */
  notifyOperation?: 'anniversary_create' | 'anniversary_edit' | null
  partnerPresent?: boolean
  /** The choice captured by an existing (submitting / unknown) action. */
  capturedNotify?: boolean | null
}>(), { initial: null, busy: false, notifyOperation: null, partnerPresent: true, capturedNotify: null })

const emit = defineEmits<{ submit: [terms: AnniversaryTerms, notifyPartner: boolean | undefined] }>()

const notify = ref(false)
const shownNotify = computed(() => props.capturedNotify ?? notify.value)

const draft = reactive<TermsDraft>({
  title: props.initial?.title ?? '',
  date: props.initial?.date ?? '',
  kind: props.initial?.kind ?? 'anniversary',
  note: props.initial?.note ?? '',
})
const fieldError = ref('')
const titleLength = computed(() => Array.from(draft.title).length)
const noteLength = computed(() => Array.from(draft.note).length)

function submit(): void {
  if (props.busy) return
  const checked = checkAnniversaryTerms(draft)
  if (!checked.ok) {
    fieldError.value = checked.message
    return
  }
  fieldError.value = ''
  emit('submit', checked.terms, props.notifyOperation ? notify.value && props.partnerPresent : undefined)
}

function clear(): void {
  Object.assign(draft, { title: '', date: '', kind: 'anniversary', note: '' })
  fieldError.value = ''
}

defineExpose({ clear })
</script>

<template>
  <form class="stack" :data-test="`${name}-form`" @submit.prevent="submit()">
    <div class="field">
      <label class="field__label" :for="`${name}-title`">标题（{{ titleLength }} / {{ ANNIVERSARY_TITLE_MAX }}）</label>
      <input :id="`${name}-title`" v-model="draft.title" class="field__input" type="text" :disabled="busy"
        :data-test="`${name}-title`" />
    </div>
    <div class="field">
      <label class="field__label" :for="`${name}-date`">日期</label>
      <input :id="`${name}-date`" v-model="draft.date" class="field__input" type="date" :min="ANNIVERSARY_MIN_DATE"
        :max="ANNIVERSARY_MAX_DATE" :disabled="busy" :data-test="`${name}-date`" />
    </div>
    <fieldset class="field" :disabled="busy">
      <legend class="field__label">类型</legend>
      <label><input v-model="draft.kind" type="radio" value="anniversary" :data-test="`${name}-kind-anniversary`" />
        纪念日（每年重复）</label>
      <label><input v-model="draft.kind" type="radio" value="countdown" :data-test="`${name}-kind-countdown`" />
        倒数日（不重复）</label>
    </fieldset>
    <div class="field">
      <label class="field__label" :for="`${name}-note`">备注（可选，{{ noteLength }} / {{ ANNIVERSARY_NOTE_MAX }}）</label>
      <textarea :id="`${name}-note`" v-model="draft.note" class="field__input" rows="2" :disabled="busy"
        :data-test="`${name}-note`"></textarea>
    </div>
    <NotifyPartnerToggle v-if="notifyOperation" :model-value="shownNotify" :operation="notifyOperation" :name="name"
      :locked="busy" :unavailable="!partnerPresent" @update:model-value="notify = $event" />
    <p v-if="fieldError" class="hint hint--error" role="alert" :data-test="`${name}-error`">{{ fieldError }}</p>
    <button class="btn btn--block" type="submit" :disabled="busy" :data-test="`${name}-submit`">{{ submitLabel }}</button>
  </form>
</template>
