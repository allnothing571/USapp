<script setup lang="ts">
/**
 * D20 opt-in “通知对方” for a new photo memory, a new shared plan and one album photo batch (docs/phase3-plan.md Section 30).
 *
 * - Off on every fresh form, and never a remembered value: this toggle reads and writes no preference.
 * - locked: while a send is in flight or its result is unconfirmed, the choice stays what was captured with the request.
 * - unavailable: with no partner it is disabled and off; the write still succeeds (the server records no_partner).
 * - The parent remounts it with a new key to start a new form.
 */
import { computed } from 'vue'

const props = withDefaults(defineProps<{
  modelValue: boolean
  locked?: boolean
  unavailable?: boolean
  name: string
}>(), { locked: false, unavailable: false })

const emit = defineEmits<{ 'update:modelValue': [value: boolean] }>()
const disabled = computed(() => props.locked || props.unavailable)

function onChange(event: Event): void {
  if (disabled.value) return
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
  </div>
</template>
