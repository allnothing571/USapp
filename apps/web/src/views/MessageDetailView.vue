<script setup lang="ts">
/**
 * 留言详情（阶段 2 P2-1）：一条留言与它的回复。收到留言的一方在没有回复时可以回复一句话（1～100 字，一行）。
 * 结果未确认时保留原文与请求标识，只在点击“重试”时用同一标识重发；账号切换时清空回复草稿。
 */
import { computed, ref, watch } from 'vue'

import { checkReplyBody, messageTime, REPLY_MAX_CODE_POINTS } from '@/api/message-rules'
import { REACTION_EMOJIS } from '@/api/reaction-rules'
import NotifyPartnerToggle from '@/components/NotifyPartnerToggle.vue'
import { useMessagesStore } from '@/stores/messages'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ id: string }>()

const session = useSessionStore()
const messages = useMessagesStore()

const me = computed(() => session.user?.id ?? '')
const message = computed(() => (messages.detail.id === props.id ? messages.detail.message : null))
const actionId = computed(() => `reply:${props.id}`)
const replying = computed(() => messages.actions[actionId.value])
const canReply = computed(() => !!message.value && !message.value.reply && message.value.recipient_id === me.value)

const draft = ref('')
const draftError = ref('')
// P2-3b: explicit notify_partner captured with the reply; an existing action shows its own choice.
const notify = ref(false)
const notifyKey = ref(0)
const shownNotify = computed(() => (replying.value ? replying.value.notify === true : notify.value))

function newNotifyForm(): void {
  notify.value = false
  notifyKey.value += 1
}
const draftLength = computed(() => Array.from(draft.value).length)

// A different message or account starts with an empty draft; nothing typed for A is kept for B.
watch([() => props.id, () => session.user?.id ?? null], ([id, user], previous) => {
  if (previous && (previous[0] !== id || previous[1] !== user)) {
    draft.value = ''
    draftError.value = ''
    newNotifyForm()
  }
  if (user && session.isAuthenticated) void messages.openDetail(id)
}, { immediate: true })

/** True only while the page still shows the same message for the same account. */
function sameTarget(messageId: string, owner: string): boolean {
  return props.id === messageId && me.value === owner
}

async function submit(): Promise<void> {
  if (replying.value) return
  const checked = checkReplyBody(draft.value)
  if (!checked.ok) {
    draftError.value = checked.message
    return
  }
  draftError.value = ''
  const messageId = props.id
  const owner = me.value
  const result = await messages.reply(messageId, checked.value, notify.value)
  if (result.outcome === 'done' && sameTarget(messageId, owner)) {
    draft.value = ''
    newNotifyForm()
  }
}

async function retry(): Promise<void> {
  const messageId = props.id
  const owner = me.value
  const result = await messages.retry(`reply:${messageId}`)
  if (result.outcome === 'done' && sameTarget(messageId, owner)) {
    draft.value = ''
    newNotifyForm()
  }
}
</script>

<template>
  <section class="page">
    <header class="row">
      <RouterLink class="btn btn--small btn--ghost" :to="{ name: 'us' }" data-test="back-to-us">返回留言板</RouterLink>
    </header>

    <div class="card stack" data-test="message-detail" aria-live="polite">
      <template v-if="messages.detail.id === id && messages.detail.status === 'error'">
        <p class="hint hint--error" role="alert">{{ messages.detail.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="messages.openDetail(id)">重试</button>
      </template>
      <p v-else-if="!message" class="card__hint">加载中…</p>
      <template v-else>
        <div class="list__meta">{{ message.author_id === me ? '我写给对方' : '对方写给我' }} · {{ messageTime(message.created_at) }}</div>
        <div class="message-body" data-test="detail-body">{{ message.body }}</div>
        <div v-if="message.reply" class="card card--soft" data-test="detail-reply">
          <div class="list__meta">{{ message.reply.author_id === me ? '我的回复' : '对方的回复' }} · {{ messageTime(message.reply.created_at) }}</div>
          <div>{{ message.reply.body }}</div>
        </div>
        <p v-else-if="!canReply" class="card__hint" data-test="detail-waiting">等待对方回复。</p>
      </template>
    </div>

    <div v-for="notice in messages.notices.filter((n) => n.id === actionId)" :key="notice.id" class="card"
      aria-live="polite" :data-test="`message-notice-${notice.id}`">
      <p class="hint" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</p>
    </div>

    <form v-if="canReply || replying" class="card stack" data-test="reply-form" @submit.prevent="submit()">
      <p class="hint">表情也算一次回复；选好后点击“回复”才发送，发出后不能再回复。</p>
      <div class="row"><button v-for="(emoji, code) in REACTION_EMOJIS" :key="code" type="button" class="btn btn--soft" :disabled="!!replying" :data-test="`quick-reply-${code}`" @click="draft = emoji; draftError = ''">{{ emoji }}</button></div>
      <div class="field">
        <label class="field__label" for="reply-body">回复一句话（{{ draftLength }} / {{ REPLY_MAX_CODE_POINTS }}）</label>
        <input id="reply-body" v-model="draft" class="field__input" type="text" name="reply" :disabled="!!replying"
          data-test="reply-body" />
      </div>
      <NotifyPartnerToggle :key="notifyKey" :model-value="shownNotify" operation="message_reply" name="reply"
        :locked="!!replying" @update:model-value="notify = $event" />
      <p v-if="draftError" class="hint hint--error" role="alert" data-test="reply-error">{{ draftError }}</p>
      <template v-if="replying && replying.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="reply-pending">{{ replying.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="reply-retry" @click="retry()">重试回复</button>
      </template>
      <button v-else class="btn btn--block" type="submit" :disabled="!!replying" data-test="reply-submit">
        {{ replying ? '正在发送…' : '回复' }}
      </button>
    </form>
  </section>
</template>
