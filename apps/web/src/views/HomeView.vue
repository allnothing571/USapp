<script setup lang="ts">
/**
 * 首页（阶段 1 M10，D15 / C1 / C2；docs/phase1-m10-home.md）：四宫格。
 *
 * - 左上：自己的昵称、形象与小红花/阳光值；余额只来自 GET /api/v1/wallet，加载中或失败时不显示数字。
 * - 左下：今天可以做的、分配给我的任务（北京时间），以及任务页入口。
 * - 右上：纪念日卡片（阶段 2 P2-2）：服务端选出的置顶或最近一项，天数来自服务端；失败时显示重试，
 *   从不显示“没有纪念日”；点按打开详情（没有纪念日时打开“我们”）。
 * - 右下：对方最新的留言（阶段 2 P2-1）。
 * - 下方：由现有数据推导的待处理事项，分“需要我处理”与“等待对方”，各自链接到任务页或奖励页。
 *   任务变更读取进行中与历史两类任务（按提议 id 去重），今天的任务只来自进行中。
 *   覆盖范围与分页完整性是两回事：服务端历史只含最近 30 天，读完游标也不代表全局完整，
 *   所以任务变更的数字只说“已找到 N”（或“至少 N”），空结果只说“在可查看的范围内没有找到”，
 *   并始终显示范围说明；兑换记录读完全部页时才显示确切数字。
 *   读取有上限：没有读完全部页时只显示“至少”，从不把部分结果说成总数或“没有待办”。
 *
 * 只读：不发写请求、不自动重试写入、不持久化。首页状态在 home store，
 * 刷新不会影响任务页与奖励页的待确认动作、幂等键或分页。
 * 跨过北京午夜、回到前台、恢复联网时重新读取。
 */
import { computed, onBeforeUnmount, onMounted, watch } from 'vue'

import { anniversaryHeadline, anniversaryNextLine, KIND_LABEL } from '@/api/anniversary-rules'
import { messagePreview, messageTime } from '@/api/message-rules'
import { isRecurring, nextDailyRefreshDelay, periodLabel } from '@/api/task-rules'
import type { Task } from '@/api/types'
import { HOME_MAX_PAGES, HOME_PAGE_LIMIT, isTodayTask, pendingSummary, useHomeStore } from '@/stores/home'
import type { PendingCounts } from '@/stores/home'
import { useSessionStore } from '@/stores/session'
import { useWalletStore } from '@/stores/wallet'
import AvatarHome from '@/components/AvatarHome.vue'
import { useAvatarStore } from '@/stores/avatar'

const session = useSessionStore()
const walletStore = useWalletStore()
const home = useHomeStore()

const me = computed(() => session.user?.id ?? '')
const nickname = computed(() => session.user?.display_name?.trim() || '未设置昵称')

function refreshAll(): void {
  if (!session.isAuthenticated || !session.user) return
  void walletStore.load()
  void useAvatarStore().loadAppearance()
  void home.refresh()
}

// Also reload when the caller changes while this component remains mounted.
watch(() => session.user?.id, (id) => {
  if (id && session.isAuthenticated) refreshAll()
  else walletStore.reset()
}, { immediate: true })

// —— 跨日与回到前台：只重新读取，从不写入 ——
let alive = true
let dayTimer: number | undefined

function refreshOnReturn(): void {
  if (!alive || navigator.onLine === false || document.visibilityState === 'hidden') return
  refreshAll()
}

function scheduleDayRefresh(): void {
  dayTimer = window.setTimeout(() => {
    refreshOnReturn()
    if (alive) scheduleDayRefresh()
  }, nextDailyRefreshDelay())
}

onMounted(() => {
  scheduleDayRefresh()
  window.addEventListener('focus', refreshOnReturn)
  window.addEventListener('online', refreshOnReturn)
  document.addEventListener('visibilitychange', refreshOnReturn)
})

onBeforeUnmount(() => {
  alive = false
  window.clearTimeout(dayTimer)
  window.removeEventListener('focus', refreshOnReturn)
  window.removeEventListener('online', refreshOnReturn)
  document.removeEventListener('visibilitychange', refreshOnReturn)
})

// —— 今天的任务 ——
const todayTasks = computed(() =>
  home.tasks.status === 'ready' ? home.tasks.items.filter((task) => isTodayTask(task, me.value, home.tasks.asOf)) : [],
)

function rewardText(task: Task): string {
  const parts: string[] = []
  if (task.reward_flowers > 0) parts.push(`小红花 ${task.reward_flowers}`)
  if (task.reward_sunshine > 0) parts.push(`阳光值 ${task.reward_sunshine}`)
  return parts.join(' · ')
}

function taskMeta(task: Task): string {
  const parts: string[] = []
  const period = isRecurring(task) || task.schedule_type === 'custom' ? periodLabel(task) : ''
  if (period) parts.push(period)
  else if (task.deadline) parts.push(`截止 ${task.deadline.local_date}${task.deadline.local_time ? ` ${task.deadline.local_time}` : ''}`)
  if (task.completion_mode === 'count') parts.push(`进度 ${task.progress_count ?? 0}/${task.target_count ?? 1}`)
  const reward = rewardText(task)
  if (reward) parts.push(reward)
  return parts.join(' · ')
}

// —— 待处理事项 ——
interface PendingRow {
  key: keyof PendingCounts
  source: 'tasks' | 'redemptions'
  needsMe: string
  waiting: string
  to: 'tasks' | 'rewards'
}

const PENDING_ROWS: PendingRow[] = [
  { key: 'taskDecisions', source: 'tasks', needsMe: '任务变更等我决定', waiting: '任务变更等对方决定', to: 'tasks' },
  { key: 'redemptionConfirmations', source: 'redemptions', needsMe: '兑换申请等我确认', waiting: '兑换申请等对方确认', to: 'rewards' },
  { key: 'cancellationDecisions', source: 'redemptions', needsMe: '取消申请等我决定', waiting: '取消申请等对方决定', to: 'rewards' },
  { key: 'fulfillments', source: 'redemptions', needsMe: '兑换等我履约', waiting: '兑换等对方履约', to: 'rewards' },
]

/** Task-change pending work needs both the open and the history scan. */
const taskSources = computed(() => {
  const scans = [home.tasks, home.history]
  return {
    status: scans.some((scan) => scan.status === 'error') ? 'error'
      : scans.every((scan) => scan.status === 'ready') ? 'ready' : 'loading',
    complete: scans.every((scan) => scan.complete),
  }
})

const summary = computed(() =>
  pendingSummary(
    taskSources.value.status === 'ready' ? [...home.tasks.items, ...home.history.items] : [],
    home.redemptions.status === 'ready' ? home.redemptions.items : [],
    me.value,
  ),
)

function scanOf(source: PendingRow['source']): { status: string; complete: boolean } {
  return source === 'tasks' ? taskSources.value : home.redemptions
}

/**
 * Redemptions: exact once every page was read (the list covers all of the caller's redemptions).
 * Task changes: never an exact global total; open + last-30-days history is the visible scope.
 */
function countText(row: PendingRow, count: number): string {
  if (!scanOf(row.source).complete) return `至少 ${count}`
  return row.source === 'tasks' ? `已找到 ${count}` : String(count)
}

function rowsFor(side: 'needsMe' | 'waiting') {
  return PENDING_ROWS
    .filter((row) => scanOf(row.source).status === 'ready' && summary.value[side][row.key] > 0)
    .map((row) => ({ ...row, label: row[side], count: countText(row, summary.value[side][row.key]) }))
}

const needsMeRows = computed(() => rowsFor('needsMe'))
const waitingRows = computed(() => rowsFor('waiting'))
const pendingReady = computed(() => taskSources.value.status === 'ready' && home.redemptions.status === 'ready')
const pendingComplete = computed(() => pendingReady.value && taskSources.value.complete && home.redemptions.complete)
const pendingLoading = computed(() =>
  [home.tasks.status, home.history.status, home.redemptions.status].some((status) => status === 'loading' || status === 'idle'),
)
const scanLimit = HOME_MAX_PAGES * HOME_PAGE_LIMIT
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">小空间</h1>
        <p class="page__subtitle">今天的任务、我的资产与待处理事项。</p>
      </div>
      <button class="btn btn--small btn--soft" type="button" data-test="home-refresh" @click="refreshAll()">刷新</button>
    </header>

    <div class="home-grid">
      <!-- 左上：昵称与自己的资产 -->
      <div class="card home-grid__cell" data-test="home-me">
        <h2 class="card__title">{{ nickname }}</h2>
        <AvatarHome />
        <div aria-live="polite" :aria-busy="walletStore.status === 'loading'" data-test="home-wallet">
          <dl v-if="walletStore.status === 'ready' && walletStore.balances" class="kv">
            <dt class="kv__key">小红花</dt>
            <dd class="kv__value">{{ walletStore.balances.flower }}</dd>
            <dt class="kv__key">阳光值</dt>
            <dd class="kv__value">{{ walletStore.balances.sunshine }}</dd>
          </dl>
          <template v-else-if="walletStore.status === 'error'">
            <p class="hint hint--error" role="alert">{{ walletStore.errorText }}</p>
            <button class="btn btn--small btn--soft" type="button" @click="walletStore.load()">重试</button>
          </template>
          <p v-else class="card__hint">加载中…</p>
        </div>
      </div>

      <!-- 右上：纪念日（P2-2，V1 §19）。加载中不显示内容；失败时显示重试，从不说“还没有纪念日”。 -->
      <div class="card home-grid__cell" data-test="home-anniversary" aria-live="polite"
        :aria-busy="home.anniversary.status === 'loading'">
        <h2 class="card__title">纪念日</h2>
        <template v-if="home.anniversary.status === 'error'">
          <p class="hint hint--error" role="alert">{{ home.anniversary.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" data-test="home-anniversary-retry"
            @click="home.loadAnniversary()">重试</button>
        </template>
        <p v-else-if="home.anniversary.status !== 'ready'" class="card__hint">加载中…</p>
        <RouterLink v-else-if="home.anniversary.anniversary" class="message-link" data-test="home-anniversary-link"
          :to="{ name: 'anniversary-detail', params: { id: home.anniversary.anniversary.id } }">
          <div class="list__title">{{ home.anniversary.anniversary.title }}</div>
          <div class="list__meta">{{ KIND_LABEL[home.anniversary.anniversary.kind] }}
            <template v-if="home.anniversary.source === 'pinned'"> · 已置顶</template></div>
          <div class="kv__value" data-test="home-anniversary-headline">{{ anniversaryHeadline(home.anniversary.anniversary) }}</div>
          <div v-if="anniversaryNextLine(home.anniversary.anniversary)" class="list__meta">
            {{ anniversaryNextLine(home.anniversary.anniversary) }}</div>
        </RouterLink>
        <RouterLink v-else class="card__hint" :to="{ name: 'us' }" data-test="home-anniversary-none">还没有纪念日，去添加</RouterLink>
      </div>

      <!-- 左下：今天的任务 -->
      <div class="card home-grid__cell" data-test="home-today" aria-live="polite" :aria-busy="home.tasks.status === 'loading'">
        <h2 class="card__title">今天的任务</h2>
        <template v-if="home.tasks.status === 'error'">
          <p class="hint hint--error" role="alert">{{ home.tasks.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" @click="home.loadTasks()">重试</button>
        </template>
        <p v-else-if="home.tasks.status !== 'ready'" class="card__hint">加载中…</p>
        <template v-else>
          <ul v-if="todayTasks.length" class="list">
            <li v-for="task in todayTasks" :key="`${task.id}:${task.period_date ?? ''}`" class="list__item"
              :data-test="`today-${task.id}`">
              <div class="list__main">
                <div class="list__title">{{ task.title }}</div>
                <div class="list__meta">{{ taskMeta(task) }}</div>
              </div>
            </li>
          </ul>
          <p v-else-if="home.tasks.complete" class="card__hint" data-test="today-none">今天没有要做的任务。</p>
          <p v-if="!home.tasks.complete" class="card__hint" data-test="today-incomplete">
            只检查了前 {{ scanLimit }} 条进行中的任务，可能还有更多，请到任务页查看。
          </p>
        </template>
        <RouterLink class="btn btn--small btn--soft" :to="{ name: 'tasks' }" data-test="home-tasks-link">全部任务</RouterLink>
      </div>

      <!-- 右下：对方最新留言（P2-1，V1 §19）。失败时显示错误，从不说“没有留言”。 -->
      <div class="card home-grid__cell" data-test="home-latest-message" aria-live="polite"
        :aria-busy="home.latestMessage.status === 'loading'">
        <h2 class="card__title">对方的留言</h2>
        <template v-if="home.latestMessage.status === 'error'">
          <p class="hint hint--error" role="alert">{{ home.latestMessage.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" @click="home.loadLatestMessage()">重试</button>
        </template>
        <p v-else-if="home.latestMessage.status !== 'ready'" class="card__hint">加载中…</p>
        <RouterLink v-else-if="home.latestMessage.message" class="message-link" data-test="latest-message-link"
          :to="{ name: 'message-detail', params: { id: home.latestMessage.message.id } }">
          <div class="list__meta">{{ messageTime(home.latestMessage.message.created_at) }}
            · {{ home.latestMessage.message.reply ? '已回复' : '待我回复' }}</div>
          <div class="message-preview" data-test="latest-message-preview">{{ messagePreview(home.latestMessage.message.body) }}</div>
        </RouterLink>
        <p v-else class="card__hint" data-test="latest-message-none">对方还没有留言。</p>
        <RouterLink class="btn btn--small btn--soft" :to="{ name: 'us' }" data-test="home-us-link">留言板</RouterLink>
      </div>
    </div>

    <!-- 待处理事项（应用内提示，C2；不是推送通知） -->
    <div class="card" data-test="home-pending" aria-live="polite">
      <h2 class="card__title">待处理</h2>
      <p v-if="home.tasks.status === 'error'" class="hint hint--error" role="alert" data-test="pending-tasks-error">
        任务读取失败：{{ home.tasks.errorText }}
      </p>
      <p v-if="home.history.status === 'error'" class="hint hint--error" role="alert" data-test="pending-history-error">
        历史任务读取失败：{{ home.history.errorText }}
      </p>
      <p v-if="home.redemptions.status === 'error'" class="hint hint--error" role="alert" data-test="pending-redemptions-error">
        兑换记录读取失败：{{ home.redemptions.errorText }}
      </p>
      <button v-if="[home.tasks, home.history, home.redemptions].some((scan) => scan.status === 'error')" class="btn btn--small btn--soft"
        type="button" data-test="pending-retry" @click="home.refresh()">重试</button>

      <div class="stack">
        <div data-test="pending-needs-me">
          <h3 class="home-pending__heading">需要我处理</h3>
          <ul v-if="needsMeRows.length" class="list">
            <li v-for="row in needsMeRows" :key="row.key" class="list__item" :data-test="`needs-me-${row.key}`">
              <div class="list__main"><div class="list__title">{{ row.label }}</div></div>
              <span class="badge badge--accent">{{ row.count }}</span>
              <RouterLink class="btn btn--small btn--soft" :to="{ name: row.to }">去处理</RouterLink>
            </li>
          </ul>
        </div>
        <div data-test="pending-waiting">
          <h3 class="home-pending__heading">等待对方</h3>
          <ul v-if="waitingRows.length" class="list">
            <li v-for="row in waitingRows" :key="row.key" class="list__item" :data-test="`waiting-${row.key}`">
              <div class="list__main"><div class="list__title">{{ row.label }}</div></div>
              <span class="badge badge--muted">{{ row.count }}</span>
              <RouterLink class="btn btn--small btn--ghost" :to="{ name: row.to }">查看</RouterLink>
            </li>
          </ul>
        </div>
      </div>

      <p class="card__hint" data-test="pending-task-scope">
        任务变更只检查进行中的任务与最近 30 天的历史任务；更早任务上未决定的变更无法通过现有列表查到，不会在这里显示。
      </p>
      <p v-if="pendingLoading" class="card__hint">加载中…</p>
      <p v-else-if="pendingComplete && !needsMeRows.length && !waitingRows.length" class="card__hint" data-test="pending-none">
        在可查看的范围内没有找到待处理事项（任务变更只含进行中与最近 30 天的任务）。
      </p>
      <p v-if="pendingReady && !pendingComplete" class="card__hint" data-test="pending-incomplete">
        记录较多，只检查了部分记录（进行中任务、历史任务与兑换记录每类最多 {{ scanLimit }} 条），实际可能更多，请到任务页与奖励页查看。
      </p>
    </div>
  </section>
</template>
