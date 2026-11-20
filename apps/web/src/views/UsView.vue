<script setup lang="ts">
/**
 * “我们”（阶段 2 P2-1，V1 §25 / §47）：双人留言板。不是聊天：一张张小纸条，按时间倒序。
 *
 * - 写给对方的纯文本留言（1～500 字，可换行）；结果未确认时保留原文与请求标识，只在点击“重试”时用同一标识重发。
 * - 历史留言按游标分页“加载更多”；每条显示时间、谁写的，以及回复状态（已回复 / 待我回复 / 等待对方回复）。
 * - 回复在留言详情页进行；只有收到留言的一方可以回复一次。
 * - 账号切换或注销时清空本页草稿与错误；旧账号的晚到结果不影响新账号（由 store 的会话代数保证）。
 * - 阶段 2 P2-2：留言板上方是纪念日区块（AnniversarySection）。
 */
import { computed, onMounted, ref, watch } from 'vue'

import { checkMessageBody, MESSAGE_MAX_CODE_POINTS, messageTime } from '@/api/message-rules'
import type { Message } from '@/api/types'
import AnniversarySection from '@/components/AnniversarySection.vue'
import NotifyPartnerToggle from '@/components/NotifyPartnerToggle.vue'
import { useMessagesStore } from '@/stores/messages'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const messages = useMessagesStore()

const me = computed(() => session.user?.id ?? '')
const draft = ref('')
const draftError = ref('')
const draftLength = computed(() => Array.from(draft.value).length)
const composing = computed(() => messages.actions.new)
// P2-3b: explicit notify_partner captured with the body; while an action exists the toggle shows
// (and retries resend) that action's captured choice.
const notify = ref(false)
const notifyKey = ref(0)
const shownNotify = computed(() => (composing.value ? composing.value.notify === true : notify.value))

function newNotifyForm(): void {
  notify.value = false
  notifyKey.value += 1
}

function clearLocal(): void {
  newNotifyForm()
  draft.value = ''
  draftError.value = ''
}

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id === previous) return
  clearLocal()
  if (id && session.isAuthenticated) void messages.load()
})

onMounted(() => {
  if (session.isAuthenticated) void messages.load()
})

async function submit(): Promise<void> {
  if (composing.value) return
  const checked = checkMessageBody(draft.value)
  if (!checked.ok) {
    draftError.value = checked.message
    return
  }
  draftError.value = ''
  const owner = me.value
  const result = await messages.create(checked.value, notify.value)
  // Only a confirmed send for the same account clears the draft; unknown keeps it for retry.
  if (result.outcome === 'done' && me.value === owner) {
    draft.value = ''
    newNotifyForm()
  }
}

async function retryCompose(): Promise<void> {
  const owner = me.value
  const result = await messages.retry('new')
  if (result.outcome === 'done' && me.value === owner) {
    draft.value = ''
    newNotifyForm()
  }
}

function replyState(message: Message): { text: string; tone: 'accent' | 'muted' } {
  if (message.reply) return { text: '已回复', tone: 'muted' }
  if (message.recipient_id === me.value) return { text: '待我回复', tone: 'accent' }
  return { text: '等待对方回复', tone: 'muted' }
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">我们</h1>
      <p class="page__subtitle">纪念日，以及写给对方的小纸条。每条留言对方可以回复一句话。</p>
    </header>

    <AnniversarySection />

    <RouterLink class="card message-link" :to="{ name: 'memories' }" data-test="memories-link">
      <h2 class="card__title">照片回忆</h2>
      <p class="card__hint">按日期收好的照片和文字。</p>
    </RouterLink>

    <RouterLink class="card message-link" :to="{ name: 'albums' }" data-test="albums-link">
      <h2 class="card__title">共享相册</h2>
      <p class="card__hint">按主题放在一起的照片，两个人都可以加。</p>
    </RouterLink>

    <RouterLink class="card message-link" :to="{ name: 'achievements' }"><h2 class="card__title">我的成就</h2><p class="card__hint">看看任务完成留下的小小足迹。</p></RouterLink>
    <RouterLink class="card message-link" :to="{ name: 'avatar-shop' }" data-test="avatar-shop-link">
      <h2 class="card__title">我的装扮</h2>
      <p class="card__hint">用阳光挑选装扮，在衣柜保存自己的形象。</p>
    </RouterLink>

    <RouterLink class="card message-link" :to="{ name: 'media-deletions' }" data-test="media-deletions-link">
      <h2 class="card__title">最近删除的照片</h2>
      <p class="card__hint">你删除的照片，7 天内可以在这里恢复。</p>
    </RouterLink>

    <RouterLink class="card message-link" :to="{ name: 'shared-plans' }" data-test="shared-plans-link">
      <h2 class="card__title">共享计划</h2>
      <p class="card__hint">记录想一起做的事，完成后可以关联回忆。</p>
    </RouterLink>

    <div class="card stack">
      <RouterLink :to="{ name: 'memory-deletions' }">最近删除的回忆</RouterLink>
      <RouterLink :to="{ name: 'retained-photos' }">保留的照片</RouterLink>
    </div>
    <form class="card stack" data-test="compose" @submit.prevent="submit()">
      <div class="field">
        <label class="field__label" for="message-body">写一张小纸条（{{ draftLength }} / {{ MESSAGE_MAX_CODE_POINTS }}）</label>
        <textarea id="message-body" v-model="draft" class="field__input" name="message" rows="3"
          :disabled="!!composing" data-test="compose-body"></textarea>
      </div>
      <NotifyPartnerToggle :key="notifyKey" :model-value="shownNotify" operation="message_create" name="compose"
        :locked="!!composing" @update:model-value="notify = $event" />
      <p v-if="draftError" class="hint hint--error" role="alert" data-test="compose-error">{{ draftError }}</p>
      <template v-if="composing && composing.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="compose-pending">{{ composing.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="compose-retry" @click="retryCompose()">重试发送</button>
      </template>
      <button v-else class="btn btn--block" type="submit" :disabled="!!composing" data-test="compose-submit">
        {{ composing ? '正在发送…' : '发给对方' }}
      </button>
    </form>

    <div v-if="messages.notices.length" class="card" aria-live="polite" data-test="message-notices">
      <ul class="list">
        <li v-for="notice in messages.notices" :key="notice.id" class="list__item">
          <div class="list__main">
            <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'" :data-test="`message-notice-${notice.id}`">
              {{ notice.text }}
            </div>
          </div>
          <button class="btn btn--small btn--ghost" type="button" @click="messages.dismissNotice(notice.id)">知道了</button>
        </li>
      </ul>
    </div>

    <div class="card" data-test="message-history" aria-live="polite" :aria-busy="messages.status === 'loading'">
      <h2 class="card__title">留言历史</h2>
      <template v-if="messages.status === 'error'">
        <p class="hint hint--error" role="alert">{{ messages.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="messages.load()">重试</button>
      </template>
      <p v-else-if="messages.status !== 'ready'" class="card__hint">加载中…</p>
      <p v-else-if="messages.items.length === 0" class="card__hint" data-test="history-empty">还没有留言，写下第一张小纸条吧。</p>
      <ul v-else class="list">
        <li v-for="item in messages.items" :key="item.id" class="list__item" :data-test="`message-${item.id}`">
          <RouterLink class="list__main message-link" :to="{ name: 'message-detail', params: { id: item.id } }">
            <div class="list__meta">{{ item.author_id === me ? '我写的' : '对方写的' }} · {{ messageTime(item.created_at) }}</div>
            <div class="message-body">{{ item.body }}</div>
            <div v-if="item.reply" class="list__meta" data-test="message-reply">回复：{{ item.reply.body }}</div>
          </RouterLink>
          <span class="badge" :class="replyState(item).tone === 'accent' ? 'badge--accent' : 'badge--muted'">
            {{ replyState(item).text }}
          </span>
        </li>
      </ul>
      <p v-if="messages.moreErrorText" class="hint hint--error" role="alert">{{ messages.moreErrorText }}</p>
      <button v-if="messages.status === 'ready' && messages.nextCursor" class="btn btn--small btn--soft" type="button"
        data-test="history-more" :disabled="messages.loadingMore" @click="messages.loadMore()">
        {{ messages.loadingMore ? '加载中…' : '加载更多' }}
      </button>
    </div>
  </section>
</template>
