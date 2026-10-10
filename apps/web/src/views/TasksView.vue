<script setup lang="ts">
/**
 * 任务页（阶段 1 M3-B）：给对方发布一次性任务；分页查看进行中与最近 30 天的任务；查看详情。
 *
 * - 接收者由服务端推导为空间里的另一成员，表单没有接收者字段。
 * - 本地校验只做即时提示（api/task-rules.ts），服务端是权威校验。
 * - 发布结果未确认（网络错误、5xx、503 RESULT_UNKNOWN）时：表单的规范化内容与原发布相同，
 *   才提供“重试发布”（发送原请求体与原请求标识，不会重复发布）；内容一旦修改，
 *   就明确作为一次新的发布（新的请求标识），不再称为重试。不自动重试，离线时不提交也不排队。
 * - 发布成功只在表单仍是被提交的那份内容时才清空表单，较新的草稿保留。
 * - 账号切换或注销时清空本组件的表单、本地校验错误与提示；旧账号的晚到结果不触碰新账号的草稿。
 * - **没有**完成按钮：完成与奖励结算属于 M3-C，尚未开放。
 */
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue'

import {
  TASK_DESCRIPTION_MAX_CODE_POINTS,
  TASK_MAX_REWARD_FLOWERS,
  TASK_MAX_REWARD_SUNSHINE,
  TASK_TITLE_MAX_CODE_POINTS,
  buildCreateTaskRequest,
} from '@/api/task-rules'
import { codePointLength } from '@/api/display-name'
import type { Task, TaskStatus, TaskView } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'
import { useTasksStore } from '@/stores/tasks'

const session = useSessionStore()
const spaceStore = useSpaceStore()
const tasksStore = useTasksStore()

const me = computed(() => session.user?.id ?? null)

// —— 组件本地状态的账号隔离 ——
// viewGeneration 在会话身份变化（含变为匿名）时递增；await 之后只有代数未变且组件仍挂载，
// 才允许触碰表单与提示。store 的 reset 不会清空这些组件本地的 ref，所以这里自己清。
let viewGeneration = 0
let alive = true
onBeforeUnmount(() => {
  alive = false
})

function emptyForm() {
  return { title: '', description: '', flowers: '', sunshine: '', date: '', time: '' }
}

const form = reactive(emptyForm())
const formError = ref('')
const formNotice = ref('')

function clearDraft(): void {
  Object.assign(form, emptyForm())
  formError.value = ''
  formNotice.value = ''
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

const STATUS_LABEL: Record<TaskStatus, string> = { open: '进行中', completed: '已完成', expired: '已过期' }

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
  return built.ok ? JSON.stringify(built.request) : null
})
const unconfirmed = computed(() => tasksStore.publishStatus === 'unknown' && tasksStore.pendingPublishBody !== null)
/** 结果未确认，且表单仍是原发布的内容：只提供“重试发布”。 */
const draftIsPending = computed(() => unconfirmed.value && draftBody.value === tasksStore.pendingPublishBody)
/** 结果未确认，但表单已修改：再提交是一次新的发布，不是重试。 */
const draftChanged = computed(() => unconfirmed.value && !draftIsPending.value)
const titleLength = computed(() => codePointLength(form.title.trim()))
const descriptionLength = computed(() => codePointLength(form.description.replace(/\r\n/g, '\n').trim()))

/** 成功后的收尾：只有组件仍挂载、身份未变时才处理；表单仍是被提交的内容才清空。 */
function settlePublished(generation: number, submittedBody: string, notice: string): void {
  if (!alive || generation !== viewGeneration) return
  if (draftBody.value === submittedBody) {
    Object.assign(form, emptyForm())
    formNotice.value = notice
  } else {
    formNotice.value = `${notice}表单中较新的草稿已保留。`
  }
}

async function submit(): Promise<void> {
  if (submitting.value) return
  // 内容未改：这就是对原发布的重试，走同一个显式入口（原请求体、原请求标识）。
  if (draftIsPending.value) return retryPublish()
  formNotice.value = ''
  const built = buildCreateTaskRequest(form)
  if (!built.ok) {
    formError.value = built.message
    return
  }
  formError.value = ''
  const generation = viewGeneration
  const submittedBody = JSON.stringify(built.request)
  const result = await tasksStore.publish(built.request)
  if (result.outcome === 'published') settlePublished(generation, submittedBody, '已发布，对方会在任务列表里看到它。')
}

/** 只在表单内容与原发布相同时可用：发送原请求体与原请求标识。 */
async function retryPublish(): Promise<void> {
  const original = tasksStore.pendingPublishBody
  if (submitting.value || !draftIsPending.value || original === null) return
  formNotice.value = ''
  formError.value = ''
  const generation = viewGeneration
  const result = await tasksStore.retryPublish()
  if (result.outcome === 'published') settlePublished(generation, original, '已确认发布（不会重复发布）。')
}

// —— 详情 ——
function showDetail(task: Task): void {
  void tasksStore.openDetail(task.id)
}

function reload(view: TaskView): void {
  void tasksStore.loadFirst(view)
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">任务</h1>
      <p class="page__subtitle">给对方发布一次性任务。完成任务与领取奖励将在后续版本开放。</p>
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
        <div class="row">
          <div class="field">
            <label class="field__label" for="task-date">截止日期（可选，北京时间）</label>
            <input id="task-date" v-model="form.date" class="field__input" type="date" name="date" :disabled="submitting" />
          </div>
          <div class="field">
            <label class="field__label" for="task-time">截止时间（可选）</label>
            <input id="task-time" v-model="form.time" class="field__input" type="time" name="time" :disabled="submitting" />
          </div>
        </div>
        <p class="card__hint">只选日期时，任务在当天结束时（次日 0 点）到期。至少需要一项大于 0 的奖励。</p>
        <p v-if="formError" class="hint hint--error" role="alert">{{ formError }}</p>
        <template v-if="draftIsPending">
          <p class="hint hint--error" role="alert">{{ tasksStore.publishErrorText }}</p>
          <button class="btn btn--block btn--soft" type="button" data-test="publish-retry" @click="retryPublish()">
            重试发布（同一请求，不会重复发布）
          </button>
        </template>
        <p v-else-if="draftChanged" class="hint hint--error" role="alert" data-test="draft-changed">
          上一次发布的结果仍未确认，可以刷新“进行中”列表查看。表单内容已修改：再次提交会作为一次新的发布，
          不是对上一次的重试；改回原内容才能重试原发布。
        </p>
        <p v-else-if="tasksStore.publishStatus === 'error'" class="hint hint--error" role="alert">
          {{ tasksStore.publishErrorText }}
        </p>
        <p v-if="formNotice" class="hint hint--ok">{{ formNotice }}</p>
        <button v-if="!draftIsPending" class="btn btn--block" type="submit" data-test="publish-submit" :disabled="submitting">
          {{ submitting ? '正在发布…' : draftChanged ? '发布修改后的内容（新的发布）' : '发布任务' }}
        </button>
      </form>
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
        <dt class="kv__key">截止</dt>
        <dd class="kv__value">{{ deadlineText(tasksStore.detail) }}</dd>
        <dt class="kv__key">状态</dt>
        <dd class="kv__value">{{ STATUS_LABEL[tasksStore.detail.status] }}</dd>
        <dt class="kv__key">发布于</dt>
        <dd class="kv__value">{{ formatInstant(tasksStore.detail.created_at) }}</dd>
        <template v-if="tasksStore.detail.completed_at">
          <dt class="kv__key">完成于</dt>
          <dd class="kv__value">{{ formatInstant(tasksStore.detail.completed_at) }}</dd>
        </template>
      </dl>
      <button class="btn btn--small btn--ghost" type="button" @click="tasksStore.closeDetail()">关闭</button>
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
            <li v-for="task in toComplete" :key="task.id" class="list__item" @click="showDetail(task)">
              <div class="list__main">
                <div class="list__title">{{ task.title }}</div>
                <div class="list__meta">{{ rewardText(task) }} · {{ deadlineText(task) }}</div>
              </div>
              <span class="badge badge--accent">{{ STATUS_LABEL[task.status] }}</span>
            </li>
          </ul>
          <h3 class="list__meta">我发布的（{{ publishedByMe.length }}）</h3>
          <ul class="list">
            <li v-for="task in publishedByMe" :key="task.id" class="list__item" @click="showDetail(task)">
              <div class="list__main">
                <div class="list__title">{{ task.title }}</div>
                <div class="list__meta">{{ rewardText(task) }} · {{ deadlineText(task) }}</div>
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
          <li v-for="task in historyView.items" :key="task.id" class="list__item list__item--muted" @click="showDetail(task)">
            <div class="list__main">
              <div class="list__title">{{ task.title }}</div>
              <div class="list__meta">
                {{ memberName(task.creator_id) }} → {{ memberName(task.assignee_id) }} · {{ rewardText(task) }}
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
