<script setup lang="ts">
import ReminderControls from '@/components/ReminderControls.vue'
import TaskReactionPanel from '@/components/TaskReactionPanel.vue'
/**
 * 任务页（阶段 1 M3-B，M3-C2）：给对方发布一次性任务；分页查看进行中与最近 30 天的任务；
 * 查看详情；接收者完成任务并获得奖励。
 *
 * - 接收者由服务端推导为空间里的另一成员，表单没有接收者字段。
 * - 本地校验只做即时提示（api/task-rules.ts），服务端是权威校验。
 * - 发布结果未确认时，只能重试原请求；修改后的草稿保留，待原发布确认后再提交。
 * - 发布成功只在表单仍是被提交的那份内容时才清空表单，较新的草稿保留。
 * - 账号切换或注销时清空本组件的表单、本地校验错误与提示；旧账号的晚到结果不触碰新账号的草稿。
 * - 完成（M3-C2）：只有“进行中且接收者是我”的任务显示“完成”按钮。完成动作保存在 tasks store 中，
 *   与列表成员无关；结果未确认的动作在单独的面板里保留“重试完成”（同一请求标识），
 *   即使刷新后任务已不在进行中列表。成功提示与列表/钱包刷新失败分开显示。
 * - 指定日期范围（M5-C）：单个不重复的计数任务；开始前显示“未开始”且没有 +1，范围内累计，结束后过期。
 * - 重复任务（M5-A 每天、M5-B 每周）：只支持计数，没有自定义截止；卡片、详情与提示显示周期
 *   （每周为周一至周日）。待确认动作保留原周期与请求标识，跨越北京时间 0 点后只由用户显式重试。
 * - 修改与取消（M6）：详情里发布者可提出修改，任一方可提出取消；另一方同意或拒绝，提出者可撤回。
 *   确认前原任务照常进行；不能同意的原因由服务端给出（仍可拒绝或撤回）。未确认的动作保留请求标识，只显式重试。
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'

import {
  TASK_DESCRIPTION_MAX_CODE_POINTS,
  TASK_MAX_REWARD_FLOWERS,
  TASK_MAX_REWARD_SUNSHINE,
  TASK_TITLE_MAX_CODE_POINTS,
  buildCreateTaskRequest,
  formFromTask,
  isRecurring,
  nextDailyRefreshDelay,
  periodLabel,
} from '@/api/task-rules'
import { codePointLength } from '@/api/display-name'
import NotifyPartnerToggle from '@/components/NotifyPartnerToggle.vue'
import type { ScheduleType, Task, TaskChangeAction, TaskChangeTerms, TaskPendingChange, TaskStatus, TaskView } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'
import { type CompletionNotice, useTasksStore } from '@/stores/tasks'

const session = useSessionStore()
const spaceStore = useSpaceStore()
const tasksStore = useTasksStore()

const me = computed(() => session.user?.id ?? null)

// —— 组件本地状态的账号隔离 ——
// viewGeneration 在会话身份变化（含变为匿名）时递增；await 之后只有代数未变且组件仍挂载，
// 才允许触碰表单与提示。store 的 reset 不会清空这些组件本地的 ref，所以这里自己清。
let viewGeneration = 0
let alive = true
let dailyTimer: number | undefined
onBeforeUnmount(() => {
  alive = false
  window.clearTimeout(dailyTimer)
  window.removeEventListener('focus', refreshDaily)
  window.removeEventListener('online', refreshDaily)
  document.removeEventListener('visibilitychange', refreshDaily)
})

function refreshDaily(): void {
  if (!alive || !session.isAuthenticated || navigator.onLine === false || document.visibilityState === 'hidden') return
  // Daily and weekly periods both roll over at a Beijing midnight (weeks on Monday).
  // Custom ranges also change state at a Beijing midnight (upcoming -> open -> expired).
  if (![...tasksStore.views.open.items, ...tasksStore.views.history.items].some(hasPeriod)) return
  void tasksStore.loadFirst('open')
  void tasksStore.loadFirst('history')
}

function scheduleDailyRefresh(): void {
  dailyTimer = window.setTimeout(() => {
    refreshDaily()
    if (alive) scheduleDailyRefresh()
  }, nextDailyRefreshDelay())
}

onMounted(() => {
  scheduleDailyRefresh()
  window.addEventListener('focus', refreshDaily)
  window.addEventListener('online', refreshDaily)
  document.addEventListener('visibilitychange', refreshDaily)
})

function emptyForm() {
  return { title: '', description: '', flowers: '', sunshine: '', date: '', time: '', startDate: '', endDate: '',
    completionMode: 'single' as 'single' | 'count', targetCount: '1',
    scheduleType: 'one_off' as ScheduleType }
}

const form = reactive(emptyForm())
watch(() => form.scheduleType, value => {
  if (value !== 'one_off') form.completionMode = 'count'
})
const formError = ref('')
const formNotice = ref('')

// P2-3b: the explicit notify_partner is part of the draft body; a new form takes the
// remembered default once (the toggle remounts via notifyKey).
const notifyPartner = ref(false)
const notifyKey = ref(0)

function newNotifyForm(): void {
  notifyPartner.value = false
  notifyKey.value += 1
}

function clearDraft(): void {
  Object.assign(form, emptyForm())
  formError.value = ''
  formNotice.value = ''
  newNotifyForm()
}

// 进入页面与切换账号时从第一页加载两个视图；身份变化（含注销）时先清空本地草稿。
watch(
  () => session.user?.id,
  (id, oldId) => {
    if (oldId !== undefined && id !== oldId) {
      viewGeneration += 1
      clearDraft()
    }
    if (id && session.isAuthenticated) {
      void tasksStore.loadFirst('open')
      void tasksStore.loadFirst('history')
    }
  },
  { immediate: true },
)

const openView = computed(() => tasksStore.views.open)
const historyView = computed(() => tasksStore.views.history)
const toComplete = computed(() => openView.value.items.filter((task) => task.assignee_id === me.value))
const publishedByMe = computed(() => openView.value.items.filter((task) => task.creator_id === me.value))

function memberName(userId: string): string {
  if (userId === me.value) return '我'
  const member = spaceStore.members.find((item) => item.user_id === userId)
  return member?.display_name?.trim() || '对方'
}

function rewardText(task: Task): string {
  const parts: string[] = []
  if (task.reward_flowers > 0) parts.push(`小红花 ${task.reward_flowers}`)
  if (task.reward_sunshine > 0) parts.push(`阳光值 ${task.reward_sunshine}`)
  return parts.join(' · ')
}

function deadlineText(task: Task): string {
  if (!task.deadline) return '无截止时间'
  const time = task.deadline.local_time ?? '当天结束'
  return `截止 ${task.deadline.local_date} ${time}（北京时间）`
}

const STATUS_LABEL: Record<TaskStatus, string> = {
  open: '进行中', completed: '已完成', expired: '已过期', upcoming: '未开始', cancelled: '已取消', superseded: '已替换',
}

/** Daily/weekly periods and custom ranges show a period/range label. */
function hasPeriod(task: Task): boolean {
  return isRecurring(task) || task.schedule_type === 'custom'
}

function formatInstant(value: string | null): string {
  if (!value) return '—'
  const time = new Date(value)
  if (Number.isNaN(time.getTime())) return value
  return time.toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', hour12: false })
}

// —— 发布表单 ——
const submitting = computed(() => tasksStore.publishStatus === 'submitting')

/** 当前表单的规范化请求体（JSON 串），表单不合法时为 null。与 store 的比较口径一致。 */
const draftBody = computed(() => {
  const built = buildCreateTaskRequest(form)
  return built.ok ? JSON.stringify({ ...built.request, notify_partner: notifyPartner.value }) : null
})
const unconfirmed = computed(() => tasksStore.publishStatus === 'unknown' && tasksStore.pendingPublishBody !== null)
/** 结果未确认，且表单仍是原发布的内容：只提供“重试发布”。 */
const draftIsPending = computed(() => unconfirmed.value && draftBody.value === tasksStore.pendingPublishBody)
/** 结果未确认，但表单已修改：先确认原发布，较新的草稿保持不变。 */
const draftChanged = computed(() => unconfirmed.value && !draftIsPending.value)
const titleLength = computed(() => codePointLength(form.title.trim()))
const descriptionLength = computed(() => codePointLength(form.description.replace(/\r\n/g, '\n').trim()))

/** 成功后的收尾：只有组件仍挂载、身份未变时才处理；表单仍是被提交的内容才清空。 */
function settlePublished(generation: number, submittedBody: string, notice: string): void {
  if (!alive || generation !== viewGeneration) return
  if (draftBody.value === submittedBody) {
    Object.assign(form, emptyForm())
    newNotifyForm()
    formNotice.value = notice
  } else {
    formNotice.value = `${notice}表单中较新的草稿已保留。`
  }
}

async function submit(): Promise<void> {
  if (submitting.value) return
  if (unconfirmed.value) return retryPublish()
  formNotice.value = ''
  const built = buildCreateTaskRequest(form)
  if (!built.ok) {
    formError.value = built.message
    return
  }
  formError.value = ''
  const generation = viewGeneration
  // The explicit choice is captured with the payload and key; a retry resends this snapshot.
  const request = { ...built.request, notify_partner: notifyPartner.value }
  const submittedBody = JSON.stringify(request)
  const result = await tasksStore.publish(request)
  if (result.outcome === 'published') settlePublished(generation, submittedBody, '已发布，对方会在任务列表里看到它。')
}

/** 发送原请求体与原请求标识，确认后仍保留修改后的草稿。 */
async function retryPublish(): Promise<void> {
  const original = tasksStore.pendingPublishBody
  if (submitting.value || !unconfirmed.value || original === null) return
  formNotice.value = ''
  formError.value = ''
  const generation = viewGeneration
  const result = await tasksStore.retryPublish()
  if (result.outcome === 'published') settlePublished(generation, original, '已确认发布（不会重复发布）。')
}

// —— 完成（M3-C2）——
const pendingActions = computed(() => Object.values(tasksStore.completionActions))

function canComplete(task: Task): boolean {
  return task.status === 'open' && task.assignee_id === me.value && !tasksStore.completionActions[task.id]
}

function completeTask(task: Task): void {
  if (task.completion_mode === 'count') void tasksStore.progress(task)
  else void tasksStore.complete(task)
}

function retryCompletion(taskId: string): void {
  void tasksStore.retryCompletion(taskId)
}

function noticeText(notice: CompletionNotice): string {
  const period = notice.periodLabel ?? notice.periodDate
  const title = period ? `${notice.title}（${period}）` : notice.title
  if (notice.kind !== 'completed') return `「${title}」：${notice.text}`
  const parts: string[] = []
  if (notice.flower > 0) parts.push(`+${notice.flower} 小红花`)
  if (notice.sunshine > 0) parts.push(`+${notice.sunshine} 阳光值`)
  return `已完成「${title}」${parts.length ? '：' + parts.join(' · ') : ''}`
}

// —— 修改与取消（M6）：一方提出，另一方同意或拒绝；提出者可以撤回。服务端是权威判断 ——
const editing = ref(false)
const editForm = reactive(emptyForm())
const editError = ref('')
const pendingChanges = computed(() => Object.values(tasksStore.changeActions))

const BLOCKED_TEXT: Record<NonNullable<TaskPendingChange['blocked_reason']>, string> = {
  progress: '任务（或本周期）已有进度，暂不能同意修改；可以拒绝或撤回。',
  completed: '任务已完成，不能同意；可以拒绝或撤回。',
  expired: '任务已过期，不能同意；可以拒绝或撤回。',
  closed: '任务已关闭。',
}

/** No pending change or action, not closed; one-off/custom only while open or upcoming. */
function canChange(task: Task): boolean {
  if (tasksStore.changeActions[task.id] || task.pending_change || task.closure) return false
  return isRecurring(task) || task.status === 'open' || task.status === 'upcoming'
}

function canEdit(task: Task): boolean {
  return canChange(task) && task.creator_id === me.value
}

function startEdit(task: Task): void {
  Object.assign(editForm, formFromTask(task))
  editError.value = ''
  editing.value = true
}

async function submitEdit(task: Task): Promise<void> {
  const built = buildCreateTaskRequest(editForm)
  if (!built.ok) {
    editError.value = built.message
    return
  }
  editError.value = ''
  editing.value = false
  await tasksStore.proposeChange(task, { kind: 'edit', terms: built.request })
}

function proposeCancel(task: Task): void {
  editing.value = false
  void tasksStore.proposeChange(task, { kind: 'cancel' })
}

function decideChange(task: Task, action: TaskChangeAction): void {
  void tasksStore.decideChange(task, action)
}

function termsText(terms: TaskChangeTerms): string {
  const parts = [terms.title]
  const reward: string[] = []
  if (terms.reward_flowers > 0) reward.push(`小红花 ${terms.reward_flowers}`)
  if (terms.reward_sunshine > 0) reward.push(`阳光值 ${terms.reward_sunshine}`)
  parts.push(reward.join(' · '))
  if (terms.completion_mode === 'count') parts.push(`目标 ${terms.target_count} 次`)
  if (terms.start_date) parts.push(`${terms.start_date} 至 ${terms.deadline?.local_date ?? ''}`)
  else if (terms.deadline) parts.push(`截止 ${terms.deadline.local_date} ${terms.deadline.local_time ?? '当天结束'}`)
  return parts.join(' · ')
}

function openTask(taskId: string | null | undefined): void {
  if (!taskId) return
  editing.value = false
  void tasksStore.openDetail(taskId)
}

// Detail actions read the store when clicked (template handlers do not keep v-if narrowing).
function withDetail(run: (task: Task) => void): void {
  const task = tasksStore.detail
  if (task) run(task)
}
const decideDetail = (action: TaskChangeAction) => withDetail((task) => decideChange(task, action))
const startEditDetail = () => withDetail(startEdit)
const proposeCancelDetail = () => withDetail(proposeCancel)
const submitEditDetail = () => withDetail((task) => { void submitEdit(task) })

watch(() => session.user?.id, () => { editing.value = false })

// —— 详情 ——
function showDetail(task: Task): void {
  editing.value = false
  void tasksStore.openDetail(task.id, task.period_date)
}

function reload(view: TaskView): void {
  void tasksStore.loadFirst(view)
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">任务</h1>
      <p class="page__subtitle">给对方发布一次性、每天或每周重复、或指定日期范围的任务；完成发给你的任务即可获得奖励。</p>
    </header>

    <RouterLink class="btn btn--small btn--ghost" :to="{ name: 'home' }">返回首页</RouterLink>

    <!-- 发布 -->
    <div class="card">
      <h2 class="card__title">发布给对方</h2>
      <form class="stack" @submit.prevent="submit()">
        <div class="field">
          <label class="field__label" for="task-title">标题（{{ titleLength }} / {{ TASK_TITLE_MAX_CODE_POINTS }}）</label>
          <input id="task-title" v-model="form.title" class="field__input" type="text" name="title" :disabled="submitting" />
        </div>
        <div class="field">
          <label class="field__label" for="task-description">
            描述（可选，{{ descriptionLength }} / {{ TASK_DESCRIPTION_MAX_CODE_POINTS }}）
          </label>
          <textarea
            id="task-description"
            v-model="form.description"
            class="field__input"
            name="description"
            rows="3"
            :disabled="submitting"
          ></textarea>
        </div>
        <div class="field">
          <label class="field__label" for="task-schedule">重复方式</label>
          <select id="task-schedule" v-model="form.scheduleType" class="field__input" :disabled="submitting">
            <option value="one_off">仅一次</option>
            <option value="daily">每天重复</option>
            <option value="weekly">每周重复</option>
            <option value="custom">指定日期范围</option>
          </select>
          <p v-if="form.scheduleType === 'daily'" class="card__hint">
            每天北京时间 0 点开始新一轮；次数不结转，每天达到目标后领取一次奖励。
          </p>
          <p v-else-if="form.scheduleType === 'weekly'" class="card__hint" data-test="weekly-hint">
            每周一北京时间 0 点开始新一轮（周一至周日）；次数不结转，每周达到目标后领取一次奖励。
          </p>
          <p v-else-if="form.scheduleType === 'custom'" class="card__hint" data-test="custom-hint">
            从开始日期 0 点到结束日期当天结束（北京时间）；不重复，期间累计计数，达到目标后领取一次奖励。开始日期不能早于今天。
          </p>
        </div>
        <div class="row">
          <div class="field">
            <label class="field__label" for="task-completion-mode">完成方式</label>
            <select id="task-completion-mode" v-model="form.completionMode" class="field__input" :disabled="submitting">
              <option value="single" :disabled="form.scheduleType !== 'one_off'">单次完成</option>
              <option value="count">累计计数</option>
            </select>
          </div>
          <div v-if="form.completionMode === 'count'" class="field">
            <label class="field__label" for="task-target">目标次数</label>
            <input id="task-target" v-model="form.targetCount" class="field__input" type="text"
              inputmode="numeric" :disabled="submitting" />
          </div>
        </div>
        <p v-if="form.completionMode === 'count'" class="card__hint">每次 +1，达到目标后一次性领取下面的奖励。</p>
        <div class="row">
          <div class="field">
            <label class="field__label" for="task-flowers">小红花（0–{{ TASK_MAX_REWARD_FLOWERS }}）</label>
            <input id="task-flowers" v-model="form.flowers" class="field__input" type="text" inputmode="numeric" name="flowers" :disabled="submitting" />
          </div>
          <div class="field">
            <label class="field__label" for="task-sunshine">阳光值（0–{{ TASK_MAX_REWARD_SUNSHINE }}）</label>
            <input id="task-sunshine" v-model="form.sunshine" class="field__input" type="text" inputmode="numeric" name="sunshine" :disabled="submitting" />
          </div>
        </div>
        <div v-if="form.scheduleType === 'one_off'" class="row">
          <div class="field">
            <label class="field__label" for="task-date">截止日期（可选，北京时间）</label>
            <input id="task-date" v-model="form.date" class="field__input" type="date" name="date" :disabled="submitting" />
          </div>
          <div class="field">
            <label class="field__label" for="task-time">截止时间（可选）</label>
            <input id="task-time" v-model="form.time" class="field__input" type="time" name="time" :disabled="submitting" />
          </div>
        </div>
        <div v-if="form.scheduleType === 'custom'" class="row">
          <div class="field">
            <label class="field__label" for="task-start">开始日期（北京时间）</label>
            <input id="task-start" v-model="form.startDate" class="field__input" type="date" name="start" :disabled="submitting" />
          </div>
          <div class="field">
            <label class="field__label" for="task-end">结束日期（含当天）</label>
            <input id="task-end" v-model="form.endDate" class="field__input" type="date" name="end" :disabled="submitting" />
          </div>
        </div>
        <p v-if="form.scheduleType === 'one_off' && form.date && !form.time" class="card__hint" data-test="date-only-hint">
          只选日期时，任务在当天结束时（次日 0 点）到期。
        </p>
        <p class="card__hint">至少需要一项大于 0 的奖励。</p>
        <p v-if="formError" class="hint hint--error" role="alert">{{ formError }}</p>
        <template v-if="unconfirmed">
          <p class="hint hint--error" role="alert">{{ tasksStore.publishErrorText }}</p>
          <p v-if="draftChanged" class="hint" data-test="draft-changed">
            表单已修改，新草稿会保留。请先确认上一次发布，再提交新草稿。
          </p>
          <button class="btn btn--block btn--soft" type="button" data-test="publish-retry" @click="retryPublish()">
            重试上一次发布（同一请求）
          </button>
        </template>
        <p v-else-if="tasksStore.publishStatus === 'error'" class="hint hint--error" role="alert">
          {{ tasksStore.publishErrorText }}
        </p>
        <NotifyPartnerToggle :key="notifyKey" v-model="notifyPartner" operation="task_publish" name="publish"
          :locked="submitting" />
        <p v-if="formNotice" class="hint hint--ok">{{ formNotice }}</p>
        <button v-if="!unconfirmed" class="btn btn--block" type="submit" data-test="publish-submit" :disabled="submitting">
          {{ submitting ? '正在发布…' : '发布任务' }}
        </button>
      </form>
    </div>

    <!-- 完成：结果未确认的动作与完成提示（与列表成员无关） -->
    <div
      v-if="pendingActions.length || tasksStore.completionNotices.length"
      class="card"
      aria-live="polite"
      data-test="completion-panel"
    >
      <h2 class="card__title">完成任务</h2>
      <ul v-if="pendingActions.length" class="list" data-test="completion-pending">
        <li v-for="action in pendingActions" :key="action.taskId" class="list__item">
          <div class="list__main">
            <div class="list__title">{{ action.title }} {{ action.periodLabel ?? action.periodDate ?? '' }}</div>
            <div class="list__meta">{{ action.status === 'submitting' ? '正在提交…' : action.errorText }}</div>
          </div>
          <button
            v-if="action.status !== 'submitting'"
            class="btn btn--small btn--soft"
            type="button"
            :data-test="`completion-retry-${action.taskId}`"
            @click="retryCompletion(action.taskId)"
          >
            {{ action.kind === 'progress' ? '重试 +1' : '重试完成' }}
          </button>
        </li>
      </ul>
      <ul v-if="tasksStore.completionNotices.length" class="list" data-test="completion-notices">
        <li v-for="notice in tasksStore.completionNotices" :key="notice.taskId" class="list__item">
          <div class="list__main">
            <div class="list__title" :class="notice.kind === 'rejected' ? 'hint hint--error' : 'hint hint--ok'">
              {{ noticeText(notice) }}
            </div>
          </div>
          <button class="btn btn--small btn--ghost" type="button" @click="tasksStore.dismissNotice(notice.taskId)">
            知道了
          </button>
        </li>
      </ul>
    </div>

    <!-- 详情 -->
    <div v-if="tasksStore.detailStatus !== 'idle'" class="card" aria-live="polite" data-test="task-detail">
      <h2 class="card__title">任务详情</h2>
      <p v-if="tasksStore.detailStatus === 'loading'" class="card__hint">加载中…</p>
      <p v-else-if="tasksStore.detailStatus === 'error'" class="hint hint--error" role="alert">
        {{ tasksStore.detailErrorText }}
      </p>
      <dl v-else-if="tasksStore.detail" class="kv">
        <dt class="kv__key">标题</dt>
        <dd class="kv__value">{{ tasksStore.detail.title }}</dd>
        <dt class="kv__key">描述</dt>
        <dd class="kv__value task-text">{{ tasksStore.detail.description || '—' }}</dd>
        <dt class="kv__key">发布者</dt>
        <dd class="kv__value">{{ memberName(tasksStore.detail.creator_id) }}</dd>
        <dt class="kv__key">接收者</dt>
        <dd class="kv__value">{{ memberName(tasksStore.detail.assignee_id) }}</dd>
        <dt class="kv__key">奖励</dt>
        <dd class="kv__value">{{ rewardText(tasksStore.detail) }}</dd>
        <template v-if="hasPeriod(tasksStore.detail)">
          <dt class="kv__key">周期</dt>
          <dd class="kv__value">{{ periodLabel(tasksStore.detail) }}</dd>
        </template>
        <template v-if="tasksStore.detail.completion_mode === 'count'">
          <dt class="kv__key">进度</dt>
          <dd class="kv__value">{{ tasksStore.detail.progress_count ?? 0 }} / {{ tasksStore.detail.target_count }}</dd>
        </template>
        <dt class="kv__key">截止</dt>
        <dd class="kv__value">{{ deadlineText(tasksStore.detail) }}</dd>
        <dt class="kv__key">状态</dt>
        <dd class="kv__value">{{ STATUS_LABEL[tasksStore.detail.status] }}</dd>
        <dt class="kv__key">{{ isRecurring(tasksStore.detail) ? '周期开始' : '发布于' }}</dt>
        <dd class="kv__value">{{ formatInstant(tasksStore.detail.created_at) }}</dd>
        <template v-if="tasksStore.detail.completed_at">
          <dt class="kv__key">完成于</dt>
          <dd class="kv__value">{{ formatInstant(tasksStore.detail.completed_at) }}</dd>
        </template>
      </dl>
      <TaskReactionPanel v-if="tasksStore.detailStatus === 'ready' && tasksStore.detail" :key="`${tasksStore.detail.id}:${tasksStore.detail.period_date ?? ''}`" :task="tasksStore.detail" />
      <ReminderControls v-if="tasksStore.detailStatus === 'ready' && tasksStore.detail" :key="`${me}:${tasksStore.detail.id}`" :task="tasksStore.detail" />
      <div v-if="tasksStore.detailStatus === 'ready' && tasksStore.detail" class="stack" data-test="task-change">
        <p v-if="tasksStore.detail.closure" class="card__hint" data-test="task-closure">
          {{ tasksStore.detail.closure.reason === 'cancelled' ? '双方已确认取消这项任务' : '双方已确认修改，原任务由新任务替换' }}
          （{{ formatInstant(tasksStore.detail.closure.closed_at) }}）。已有进度、完成与奖励不会改变。
          <button v-if="tasksStore.detail.closure.replaced_by" class="btn btn--small btn--ghost" type="button"
            data-test="open-successor" @click="openTask(tasksStore.detail?.closure?.replaced_by)">查看新任务</button>
        </p>
        <p v-if="tasksStore.detail.replaces" class="card__hint" data-test="task-replaces">
          这是确认修改后的新任务。
          <button class="btn btn--small btn--ghost" type="button" @click="openTask(tasksStore.detail?.replaces)">查看原任务</button>
        </p>
        <template v-if="tasksStore.detail.pending_change">
          <p class="list__title" data-test="pending-change">
            待确认的{{ tasksStore.detail.pending_change.kind === 'edit' ? '修改' : '取消' }}
            （{{ memberName(tasksStore.detail.pending_change.proposer_id) }}提出）
          </p>
          <p v-if="tasksStore.detail.pending_change.terms" class="list__meta">
            {{ termsText(tasksStore.detail.pending_change.terms) }}
          </p>
          <p v-if="tasksStore.detail.pending_change.blocked_reason" class="hint hint--error" data-test="change-blocked">
            {{ BLOCKED_TEXT[tasksStore.detail.pending_change.blocked_reason] }}
          </p>
          <div v-if="!tasksStore.changeActions[tasksStore.detail.id]" class="row">
            <button v-if="tasksStore.detail.pending_change.proposer_id === me" class="btn btn--small btn--soft"
              type="button" data-test="change-withdraw" @click="decideDetail('withdraw')">撤回</button>
            <template v-else>
              <button class="btn btn--small" type="button" data-test="change-approve"
                :disabled="tasksStore.detail.pending_change.blocked_reason !== null"
                @click="decideDetail('approve')">同意</button>
              <button class="btn btn--small btn--soft" type="button" data-test="change-reject"
                @click="decideDetail('reject')">拒绝</button>
            </template>
          </div>
        </template>
        <div v-else-if="canChange(tasksStore.detail)" class="row">
          <button v-if="canEdit(tasksStore.detail)" class="btn btn--small btn--soft" type="button" data-test="change-edit"
            @click="startEditDetail()">提出修改</button>
          <button class="btn btn--small btn--ghost" type="button" data-test="change-cancel"
            @click="proposeCancelDetail()">提出取消</button>
        </div>
        <form v-if="editing && canEdit(tasksStore.detail)" class="stack" data-test="change-form"
          @submit.prevent="submitEditDetail()">
          <p class="card__hint">修改需要对方确认；确认前原任务照常进行。重复方式不能修改。</p>
          <div class="field">
            <label class="field__label" for="edit-title">标题</label>
            <input id="edit-title" v-model="editForm.title" class="field__input" type="text" />
          </div>
          <div class="field">
            <label class="field__label" for="edit-description">描述（可选）</label>
            <textarea id="edit-description" v-model="editForm.description" class="field__input" rows="3"></textarea>
          </div>
          <div class="row">
            <div v-if="editForm.scheduleType === 'one_off'" class="field">
              <label class="field__label" for="edit-mode">完成方式</label>
              <select id="edit-mode" v-model="editForm.completionMode" class="field__input">
                <option value="single">单次完成</option>
                <option value="count">累计计数</option>
              </select>
            </div>
            <div v-if="editForm.completionMode === 'count'" class="field">
              <label class="field__label" for="edit-target">目标次数</label>
              <input id="edit-target" v-model="editForm.targetCount" class="field__input" type="text" inputmode="numeric" />
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label class="field__label" for="edit-flowers">小红花</label>
              <input id="edit-flowers" v-model="editForm.flowers" class="field__input" type="text" inputmode="numeric" />
            </div>
            <div class="field">
              <label class="field__label" for="edit-sunshine">阳光值</label>
              <input id="edit-sunshine" v-model="editForm.sunshine" class="field__input" type="text" inputmode="numeric" />
            </div>
          </div>
          <div v-if="editForm.scheduleType === 'one_off'" class="row">
            <div class="field">
              <label class="field__label" for="edit-date">截止日期（可选）</label>
              <input id="edit-date" v-model="editForm.date" class="field__input" type="date" />
            </div>
            <div class="field">
              <label class="field__label" for="edit-time">截止时间（可选）</label>
              <input id="edit-time" v-model="editForm.time" class="field__input" type="time" />
            </div>
          </div>
          <div v-if="editForm.scheduleType === 'custom'" class="row">
            <div class="field">
              <label class="field__label" for="edit-start">开始日期</label>
              <input id="edit-start" v-model="editForm.startDate" class="field__input" type="date" />
            </div>
            <div class="field">
              <label class="field__label" for="edit-end">结束日期（含当天）</label>
              <input id="edit-end" v-model="editForm.endDate" class="field__input" type="date" />
            </div>
          </div>
          <p v-if="editForm.scheduleType === 'custom'" class="card__hint">
            已开始的范围可以保留原开始日期；改动后的开始日期不能早于今天。
          </p>
          <p v-if="editError" class="hint hint--error" role="alert">{{ editError }}</p>
          <div class="row">
            <button class="btn btn--small" type="submit" data-test="change-submit">提交修改，等待对方确认</button>
            <button class="btn btn--small btn--ghost" type="button" @click="editing = false">放弃</button>
          </div>
        </form>
      </div>
      <button class="btn btn--small btn--ghost" type="button" @click="tasksStore.closeDetail()">关闭</button>
    </div>

    <!-- 修改与取消：结果未确认的动作与提示（与列表成员无关） -->
    <div v-if="pendingChanges.length || tasksStore.changeNotices.length" class="card" aria-live="polite"
      data-test="change-panel">
      <h2 class="card__title">修改与取消</h2>
      <ul v-if="pendingChanges.length" class="list">
        <li v-for="action in pendingChanges" :key="action.taskId" class="list__item">
          <div class="list__main">
            <div class="list__title">{{ action.title }}</div>
            <div class="list__meta">{{ action.status === 'submitting' ? '正在提交…' : action.errorText }}</div>
          </div>
          <button v-if="action.status !== 'submitting'" class="btn btn--small btn--soft" type="button"
            :data-test="`change-retry-${action.taskId}`" @click="tasksStore.retryChange(action.taskId)">重试</button>
        </li>
      </ul>
      <ul v-if="tasksStore.changeNotices.length" class="list">
        <li v-for="notice in tasksStore.changeNotices" :key="notice.taskId" class="list__item">
          <div class="list__main">
            <div class="list__title" :class="notice.ok ? 'hint hint--ok' : 'hint hint--error'">
              「{{ notice.title }}」：{{ notice.text }}
            </div>
          </div>
          <button class="btn btn--small btn--ghost" type="button" @click="tasksStore.dismissChangeNotice(notice.taskId)">
            知道了
          </button>
        </li>
      </ul>
    </div>

    <!-- 进行中 -->
    <div class="card" :aria-busy="openView.status === 'loading'" data-test="open-view">
      <h2 class="card__title">进行中</h2>
      <p v-if="openView.status === 'loading' || openView.status === 'idle'" class="card__hint">加载中…</p>
      <template v-else-if="openView.status === 'error'">
        <p class="hint hint--error" role="alert">{{ openView.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="reload('open')">重试</button>
      </template>
      <template v-else>
        <p v-if="openView.items.length === 0" class="card__hint">还没有进行中的任务。</p>
        <template v-else>
          <h3 class="list__meta">待我完成（{{ toComplete.length }}）</h3>
          <ul class="list">
            <li v-for="task in toComplete" :key="`${task.id}:${task.period_date ?? ''}`" class="list__item" @click="showDetail(task)">
              <div class="list__main">
                <div class="list__title">{{ task.title }}</div>
                <div v-if="hasPeriod(task)" class="list__meta">{{ periodLabel(task) }}</div>
                <div class="list__meta">{{ rewardText(task) }} · {{ deadlineText(task) }}</div>
                <div v-if="task.completion_mode === 'count'" class="list__meta">
                  {{ task.progress_count ?? 0 }} / {{ task.target_count }}
                </div>
                <div v-if="task.pending_change?.kind === 'edit'" class="list__meta hint hint--error"
                  :data-test="`edit-pending-${task.id}`">
                  有待你确认的修改；继续{{ task.completion_mode === 'count' ? ' +1' : '完成' }}后修改将无法同意。
                </div>
              </div>
              <button
                v-if="canComplete(task)"
                class="btn btn--small"
                type="button"
                :data-test="`${task.completion_mode === 'count' ? 'progress' : 'complete'}-${task.id}`"
                @click.stop="completeTask(task)"
              >
                {{ task.completion_mode === 'count' ? '+1' : '完成' }}
              </button>
              <span v-else class="badge badge--accent">{{ STATUS_LABEL[task.status] }}</span>
            </li>
          </ul>
          <h3 class="list__meta">我发布的（{{ publishedByMe.length }}）</h3>
          <ul class="list">
            <li v-for="task in publishedByMe" :key="`${task.id}:${task.period_date ?? ''}`" class="list__item" @click="showDetail(task)">
              <div class="list__main">
                <div class="list__title">{{ task.title }}</div>
                <div v-if="hasPeriod(task)" class="list__meta">{{ periodLabel(task) }}</div>
                <div class="list__meta">{{ rewardText(task) }} · {{ deadlineText(task) }}</div>
              <div v-if="task.completion_mode === 'count'" class="list__meta">
                {{ task.progress_count ?? 0 }} / {{ task.target_count }}
              </div>
              </div>
              <span class="badge badge--muted">{{ STATUS_LABEL[task.status] }}</span>
            </li>
          </ul>
        </template>
        <p v-if="openView.moreErrorText" class="hint hint--error" role="alert">{{ openView.moreErrorText }}</p>
        <div class="row">
          <button
            v-if="openView.nextCursor"
            class="btn btn--small btn--soft"
            type="button"
            data-test="open-more"
            :disabled="openView.loadingMore"
            @click="tasksStore.loadMore('open')"
          >
            {{ openView.loadingMore ? '加载中…' : '加载更多' }}
          </button>
          <button class="btn btn--small btn--ghost" type="button" @click="reload('open')">刷新</button>
        </div>
      </template>
    </div>

    <!-- 最近 30 天 -->
    <div class="card" :aria-busy="historyView.status === 'loading'" data-test="history-view">
      <h2 class="card__title">最近 30 天</h2>
      <p v-if="historyView.status === 'loading' || historyView.status === 'idle'" class="card__hint">加载中…</p>
      <template v-else-if="historyView.status === 'error'">
        <p class="hint hint--error" role="alert">{{ historyView.errorText }}</p>
        <button class="btn btn--small btn--soft" type="button" @click="reload('history')">重试</button>
      </template>
      <template v-else>
        <p v-if="historyView.items.length === 0" class="card__hint">最近 30 天没有已完成或已过期的任务。</p>
        <ul v-else class="list">
          <li v-for="task in historyView.items" :key="`${task.id}:${task.period_date ?? ''}`" class="list__item list__item--muted" @click="showDetail(task)">
            <div class="list__main">
              <div class="list__title">{{ task.title }}</div>
              <div class="list__meta">
                {{ memberName(task.creator_id) }} → {{ memberName(task.assignee_id) }} · {{ rewardText(task) }}
                <span v-if="hasPeriod(task)"> · {{ periodLabel(task) }}</span>
              </div>
              <div v-if="task.completion_mode === 'count'" class="list__meta">
                {{ task.progress_count ?? 0 }} / {{ task.target_count }}
              </div>
            </div>
            <span class="badge badge--muted">{{ STATUS_LABEL[task.status] }}</span>
          </li>
        </ul>
        <p v-if="historyView.moreErrorText" class="hint hint--error" role="alert">{{ historyView.moreErrorText }}</p>
        <div class="row">
          <button
            v-if="historyView.nextCursor"
            class="btn btn--small btn--soft"
            type="button"
            data-test="history-more"
            :disabled="historyView.loadingMore"
            @click="tasksStore.loadMore('history')"
          >
            {{ historyView.loadingMore ? '加载中…' : '加载更多' }}
          </button>
          <button class="btn btn--small btn--ghost" type="button" @click="reload('history')">刷新</button>
        </div>
      </template>
    </div>
  </section>
</template>

<style scoped>
.task-text {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
</style>
