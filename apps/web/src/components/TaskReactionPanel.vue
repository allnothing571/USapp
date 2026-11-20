<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import type { ReactionEmoji, Task } from '@/api/types'
import { REACTION_EMOJIS, reactionText, validReactionText } from '@/api/reaction-rules'
import { reactionTarget, useTaskReactionsStore } from '@/stores/task-reactions'
import { useSessionStore } from '@/stores/session'
const props = defineProps<{ task: Task }>()
const session = useSessionStore(), store = useTaskReactionsStore()
const target = computed(() => reactionTarget(props.task))
const action = computed(() => store.actions[target.value])
const reaction = computed(() => store.confirmed[target.value] ?? props.task.reaction)
const mode = ref<'emoji' | 'text'>('emoji'), emoji = ref<ReactionEmoji>('heart'), draft = ref(''), error = ref('')
watch([target, () => session.user?.id], () => { draft.value = ''; error.value = ''; mode.value = 'emoji'; emoji.value = 'heart' })
async function submit(): Promise<void> {
  const text = draft.value.trim()
  if (mode.value === 'text' && !validReactionText(text)) { error.value = '请输入 1～40 个字符的单行回应。'; return }
  error.value = ''
  await store.start(props.task, mode.value === 'emoji' ? { kind: 'emoji', emoji_code: emoji.value } : { kind: 'text', body: text })
}
</script>
<template>
  <section v-if="task.status === 'completed' || action || reaction" class="card stack" data-test="task-reaction">
    <h3>完成回应</h3>
    <p v-if="reaction" data-test="reaction-fact">{{ reaction.author_id === session.user?.id ? '我的回应' : '对方的回应' }}：{{ reactionText(reaction) }}</p>
    <form v-if="action || (!reaction && task.creator_id === session.user?.id)" class="stack" @submit.prevent="submit">
      <p class="hint">每次完成只能回应一次，发出后不能修改，不影响奖励。</p>
      <fieldset :disabled="!!action"><legend>选择回应</legend>
        <label><input v-model="mode" type="radio" value="emoji" /> 表情</label>
        <label><input v-model="mode" type="radio" value="text" /> 一句话</label>
        <div v-if="mode === 'emoji'" class="row">
          <button v-for="(label, code) in REACTION_EMOJIS" :key="code" type="button" class="btn btn--soft" :aria-pressed="emoji === code" @click="emoji = code">{{ label }}</button>
        </div>
        <label v-else>一句话（{{ Array.from(draft).length }} / 40）<input v-model="draft" class="field__input" data-test="reaction-body" /></label>
      </fieldset>
      <p v-if="error" role="alert">{{ error }}</p>
      <p v-if="action?.error" role="alert">{{ action.error }}</p>
      <button v-if="action && action.status !== 'submitting'" type="button" class="btn" @click="store.retry(target)">用原请求重试</button>
      <button v-else class="btn" type="submit" :disabled="!!action">{{ action ? '发送中…' : '确认发出回应' }}</button>
    </form>
    <p v-else-if="!reaction" class="hint">等待对方回应。</p>
    <p v-if="store.notices[target]" role="status">{{ store.notices[target] }}</p>
  </section>
</template>
