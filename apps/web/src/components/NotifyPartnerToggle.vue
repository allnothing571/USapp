<script setup lang="ts">
/**
 * “通知对方”开关（阶段 2 P2-3b）。v-model 是表单提交时捕获的明确布尔值。
 *
 * - 初始关闭；本表单只在“尚未被触碰、没有被锁定”时取一次记住的默认值，而且只接受在表单开始时或
 *   之后才完成的读取（mark()），从不使用早先缓存的默认值。取过一次后，晚到的读取不会再改它。
 * - locked：提交中或结果未确认时，开关不可改动（原动作保留原来的选择，重试只发原请求）。
 * - unavailable：没有对方时禁用并保持关闭（纪念日在没有对方时仍可保存）。
 * - 父组件用新的 key 重新挂载本组件，表示开始一张新表单。
 */
import { computed, ref, watch } from 'vue'

import type { ChoiceOperation } from '@/api/notifications'
import { useNotificationChoicesStore } from '@/stores/notification-choices'

const props = withDefaults(defineProps<{
  modelValue: boolean
  operation: ChoiceOperation
  locked?: boolean
  unavailable?: boolean
  name: string
}>(), { locked: false, unavailable: false })

const emit = defineEmits<{ 'update:modelValue': [value: boolean] }>()

const choices = useNotificationChoicesStore()
const touched = ref(false)
const applied = ref(false)
// The read this form needs: one in flight now or started now, never an earlier cached result.
const neededSeq = choices.mark()

function applyDefault(): void {
  if (applied.value || touched.value || props.locked || props.unavailable || !choices.readyFor()) return
  if (choices.readySeq < neededSeq) return
  applied.value = true
  const value = choices.enabledFor(props.operation)
  if (value !== props.modelValue) emit('update:modelValue', value)
}

watch(() => [choices.status, choices.readySeq, props.locked, props.unavailable], applyDefault, { immediate: true })

const disabled = computed(() => props.locked || props.unavailable)

function onChange(event: Event): void {
  if (disabled.value) return
  touched.value = true
  emit('update:modelValue', (event.target as HTMLInputElement).checked)
}
</script>

<template>
  <div class="field" :data-test="`${name}-notify`">
    <label>
      <input type="checkbox" :checked="modelValue && !unavailable" :disabled="disabled"
        :data-test="`${name}-notify-input`" @change="onChange" />
      🔔 通知对方
    </label>
    <p v-if="unavailable" class="card__hint" :data-test="`${name}-notify-unavailable`">对方加入空间后才能通知。</p>
    <p v-else-if="!applied && !touched && choices.errorText" class="card__hint" :data-test="`${name}-notify-error`">
      读取通知偏好失败，默认不通知。
      <button class="btn btn--small btn--ghost" type="button" @click="choices.load()">重试</button>
    </p>
  </div>
</template>
