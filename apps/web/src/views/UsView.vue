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
import { useSmartBack } from '@/router/back-navigation'

const props = defineProps<{ section?: 'hub' | 'messages' | 'anniversaries' | 'recovery' }>()
const back = useSmartBack(
  () => (props.section === 'messages' ? 'messages' : props.section === 'anniversaries' ? 'anniversaries' : 'us-recovery'),
  'us',
)

const session = useSessionStore()
const messages = useMessagesStore()

const isHub = computed(() => props.section === 'hub')
const pendingReplyCount = computed(() =>
  messages.items.filter((item) => !item.reply && item.recipient_id === me.value).length,
)
const SUBPAGE_TITLE: Record<'messages' | 'anniversaries' | 'recovery', string> = {
  messages: '留言小纸条',
  anniversaries: '纪念日与倒数日',
  recovery: '照片与回忆恢复',
}

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
  <section class="page us-page">
    <template v-if="isHub">
      <header class="page-heading">
        <h1 class="page__title">我们</h1>
      </header>
      <div class="card us-hero">
        <img src="/ui-art/us.jpg" alt="" class="us-hero__img">
      </div>
      <div class="hub-grid hub-grid--2col" data-test="us-hub">
        <RouterLink class="hub-card" :to="{ name: 'messages' }" data-test="hub-us-messages">
          <span class="hub-card__icon hub-card__icon--warm" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <path d="M4 6h16v11H7l-3 3V6Z" />
              <path d="M8 10h8" />
              <path d="M8 13.5h5" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">留言小纸条</span>
            <span class="hub-card__sub">写一张给对方的纸条或翻看往来</span>
          </span>
          <span class="hub-card__meta">
            <span v-if="pendingReplyCount > 0" class="hub-badge hub-badge--alert">待回复 {{ pendingReplyCount }}</span>
            <span class="hub-arrow" aria-hidden="true">›</span>
          </span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'anniversaries' }" data-test="hub-us-anniversaries">
          <span class="hub-card__icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <rect x="4" y="5" width="16" height="15" rx="2.5" />
              <path d="M8 3v4" />
              <path d="M16 3v4" />
              <path d="M4 10h16" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">纪念日与倒数日</span>
            <span class="hub-card__sub">记录和置顶重要的日子</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'memories' }" data-test="memories-link">
          <span class="hub-card__icon hub-card__icon--green" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <rect x="3.5" y="5.5" width="17" height="13" rx="2" />
              <circle cx="9" cy="10" r="1.5" />
              <path d="m20 15-4.5-4-4.5 4-2.5-2L4 17.5" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">照片回忆</span>
            <span class="hub-card__sub">按日期收好的照片与心情文字</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'albums' }" data-test="albums-link">
          <span class="hub-card__icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <rect x="4" y="6" width="13" height="14" rx="2" />
              <path d="M8 6V4h12v13h-3" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">共享相册</span>
            <span class="hub-card__sub">按主题一起整理的相册</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'shared-plans' }" data-test="shared-plans-link">
          <span class="hub-card__icon hub-card__icon--green" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <path d="M9 6h11" />
              <path d="M9 12h11" />
              <path d="M9 18h11" />
              <path d="M4.5 6h.01" />
              <path d="M4.5 12h.01" />
              <path d="M4.5 18h.01" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">共享计划</span>
            <span class="hub-card__sub">记下想一起去完成的小愿望</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'avatar-shop' }" data-test="avatar-shop-link">
          <span class="hub-card__icon hub-card__icon--warm" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <path d="M12 3a3 3 0 0 1 3 3c0 1.2-.8 2.1-3 3.2L4 14.5h16l-8-5.3" />
              <path d="M5 14.5 4 19h16l-1-4.5" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">我的装扮</span>
            <span class="hub-card__sub">用阳光挑选装扮，打理衣柜与站位</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'achievements' }" data-test="hub-us-achievements">
          <span class="hub-card__icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="9" r="5" />
              <path d="M9.5 13.5 8 21l4-2 4 2-1.5-7.5" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">我的成就</span>
            <span class="hub-card__sub">看看日常任务留下的足迹</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>

        <RouterLink class="hub-card" :to="{ name: 'us-recovery' }" data-test="hub-us-recovery">
          <span class="hub-card__icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">
              <path d="M3 12a9 9 0 1 0 3-6.7L3 8" />
              <path d="M3 3v5h5" />
            </svg>
          </span>
          <span class="hub-card__body">
            <span class="hub-card__title">照片与回忆恢复</span>
            <span class="hub-card__sub">7 天内可恢复删除的内容与保留照片</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>
      </div>
    </template>

    <template v-else-if="props.section === 'messages'">
      <header class="subpage-header">
        <RouterLink class="back-pill" :to="back.to" data-test="back-to-us">‹ 返回{{ back.label }}</RouterLink>
        <h1 class="subpage-header__title">{{ SUBPAGE_TITLE.messages }}</h1>
      </header>
      <form class="card card--note stack" data-test="compose" @submit.prevent="submit()">
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

      <div class="card card--note" data-test="message-history" aria-live="polite" :aria-busy="messages.status === 'loading'">
        <h2 class="card__title">往来纸条</h2>
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
    </template>

    <template v-else-if="props.section === 'anniversaries'">
      <header class="subpage-header">
        <RouterLink class="back-pill" :to="back.to" data-test="back-to-us">‹ 返回{{ back.label }}</RouterLink>
        <h1 class="subpage-header__title">{{ SUBPAGE_TITLE.anniversaries }}</h1>
      </header>
      <AnniversarySection />
    </template>

    <template v-else-if="props.section === 'recovery'">
      <header class="subpage-header">
        <RouterLink class="back-pill" :to="back.to" data-test="back-to-us">‹ 返回{{ back.label }}</RouterLink>
        <h1 class="subpage-header__title">{{ SUBPAGE_TITLE.recovery }}</h1>
      </header>
      <div class="hub-grid">
        <RouterLink class="hub-card" :to="{ name: 'memory-deletions' }">
          <span class="hub-card__body">
            <span class="hub-card__title">最近删除的回忆</span>
            <span class="hub-card__sub">撤回误删的回忆内容</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>
        <RouterLink class="hub-card" :to="{ name: 'retained-photos' }">
          <span class="hub-card__body">
            <span class="hub-card__title">保留的照片</span>
            <span class="hub-card__sub">查看被保留下来的照片记录</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>
        <RouterLink class="hub-card" :to="{ name: 'media-deletions' }" data-test="media-deletions-link">
          <span class="hub-card__body">
            <span class="hub-card__title">最近删除的照片</span>
            <span class="hub-card__sub">你删除的照片，7 天内可以在这里恢复</span>
          </span>
          <span class="hub-card__meta"><span class="hub-arrow" aria-hidden="true">›</span></span>
        </RouterLink>
      </div>
    </template>

    <template v-else>
      <header class="page-heading"><h1 class="page__title">我们</h1></header>
      <img src="/ui-art/us.jpg" alt="" class="page-illustration" decoding="async">
    <section class="us-section" aria-labelledby="us-message-title">
      <h2 id="us-message-title" class="section-title">留言</h2>
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

    <section class="us-section" aria-labelledby="us-memory-title">
      <h2 id="us-memory-title" class="section-title">回忆与相册</h2>
      <div class="us-link-grid">
        <RouterLink class="card message-link" :to="{ name: 'memories' }" data-test="memories-link">
              <h2 class="card__title">照片回忆</h2>
              <p class="card__hint">按日期收好的照片和文字。</p>
            </RouterLink>
        <RouterLink class="card message-link" :to="{ name: 'albums' }" data-test="albums-link">
              <h2 class="card__title">共享相册</h2>
              <p class="card__hint">按主题放在一起的照片，两个人都可以加。</p>
            </RouterLink>
      </div>
    </section>

    <section class="us-section" aria-labelledby="us-plan-title">
      <h2 id="us-plan-title" class="section-title">纪念日与计划</h2>
      <AnniversarySection />
      <RouterLink class="card message-link" :to="{ name: 'shared-plans' }" data-test="shared-plans-link">
            <h2 class="card__title">共享计划</h2>
            <p class="card__hint">记录想一起做的事，完成后可以关联回忆。</p>
          </RouterLink>
    </section>

    <section class="us-section" aria-labelledby="us-personal-title">
      <h2 id="us-personal-title" class="section-title">装扮与成就</h2>
      <div class="us-link-grid">
        <RouterLink class="card message-link" :to="{ name: 'avatar-shop' }" data-test="avatar-shop-link">
              <h2 class="card__title">我的装扮</h2>
              <p class="card__hint">用阳光挑选装扮，在衣柜保存自己的形象。</p>
            </RouterLink>
        <RouterLink class="card message-link" :to="{ name: 'achievements' }"><h2 class="card__title">我的成就</h2><p class="card__hint">看看任务完成留下的小小足迹。</p></RouterLink>
      </div>
    </section>

    <details class="card us-management">
      <summary>照片与回忆管理</summary>
      <div class="us-link-grid">
        <RouterLink class="btn btn--small btn--soft us-text-link" :to="{ name: 'memory-deletions' }">最近删除的回忆</RouterLink>
        <RouterLink class="btn btn--small btn--soft us-text-link" :to="{ name: 'retained-photos' }">保留的照片</RouterLink>
        <RouterLink class="card message-link" :to="{ name: 'media-deletions' }" data-test="media-deletions-link">
              <h2 class="card__title">最近删除的照片</h2>
              <p class="card__hint">你删除的照片，7 天内可以在这里恢复。</p>
            </RouterLink>
      </div>
    </details>
    </template>
  </section>
</template>
