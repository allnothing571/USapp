<script setup lang="ts">
/**
 * 提醒设置（阶段 2 P2-6 R3）：任务详情（只有接收者，规则挂在根任务上）与纪念日详情（每位成员只管自己的）。
 *
 * - 默认关闭，不会自动创建或开启规则；打开页面只读取。读取未完成时不显示“未开启”。
 * - 只配置应用内提醒，与手机推送无关，不承诺手机一定收到。
 * - 显示内容只来自 store 的当前规则；保存的不可变事实不会覆盖它。
 * - 结果未确认的动作显示捕获的内容，只能用原请求重试。
 * - 换目标或换账号时清空草稿与提示；保留在 store 中的未确认动作回到该目标时仍然显示。
 * - 只有服务端明确说提醒生成已开启时才显示下次时间；关闭或未知时规则仍可查看和修改，但不暗示它会提醒。
 */
import { computed, reactive, ref, watch } from 'vue'

import {
  ANNIVERSARY_DEFAULT_TIME,
  ANNIVERSARY_MAX_OFFSETS,
  ANNIVERSARY_OFFSETS,
  buildAnniversaryReminder,
  buildTaskReminder,
  nextFireText,
  reminderText,
  taskReminderAccess,
  taskReminderBounds,
  type TaskReminderMode,
} from '@/api/reminder-rules'
import type { Task } from '@/api/types'
import { reminderId, useRemindersStore } from '@/stores/reminders'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ task?: Task; anniversaryId?: string }>()

const session = useSessionStore()
const store = useRemindersStore()

const kind = computed(() => (props.task ? 'task' : 'anniversary') as 'task' | 'anniversary')
// The task API's id is always the root's; period_date is what names a period.
const targetId = computed(() => props.task?.id ?? props.anniversaryId ?? '')
const id = computed(() => reminderId(kind.value, targetId.value))
const me = computed(() => session.user?.id ?? '')
const current = computed(() => store.rules[id.value])
const action = computed(() => store.actions[id.value])
const notice = computed(() => store.notices[id.value])
const generation = computed(() => store.generationStatus)
/** Only a complete read may say that no rule exists. */
const known = computed(() => !!current.value || store.status === 'ready')

const access = computed(() => (props.task
  ? taskReminderAccess(props.task, me.value)
  : { visible: true, canEnable: true, canDisable: true, closedText: '' }))
const bounds = computed(() => (props.task ? taskReminderBounds(props.task) : null))

const MODE_LABEL: Record<TaskReminderMode, string> = {
  before_deadline: '截止前多少分钟', local_time: '指定时间', absolute: '指定日期和时间',
}

const editing = ref(false)
const draftError = ref('')
const draft = reactive({ mode: 'before_deadline' as TaskReminderMode, beforeMinutes: '30', daysBefore: '0', atTime: '21:00', atLocalDate: '' })
const offsets = ref<number[]>([0])
const anniversaryTime = ref(ANNIVERSARY_DEFAULT_TIME)

function closeDraft(): void {
  editing.value = false
  draftError.value = ''
}

function startDraft(): void {
  const rule = current.value?.enabled ? current.value : undefined
  draftError.value = ''
  if (bounds.value) {
    // An inherited mode may not be offered under the new terms.
    const mode = rule?.mode as TaskReminderMode | undefined
    draft.mode = (mode && bounds.value.modes.includes(mode) ? mode : bounds.value.modes[0]) ?? 'absolute'
    draft.beforeMinutes = String(rule?.before_minutes ?? 30)
    draft.daysBefore = String(rule?.days_before ?? 0)
    draft.atTime = rule?.at_time ?? '21:00'
    draft.atLocalDate = rule?.at_local_date ?? ''
  } else {
    offsets.value = rule?.offsets ? [...rule.offsets] : [0]
    anniversaryTime.value = rule?.at_time ?? ANNIVERSARY_DEFAULT_TIME
  }
  editing.value = true
}

// Reads once per opened target, never per task card. The pages key this component by account and target, so a
// change arrives as a new instance: the notice of whatever was shown before is cleared here as well.
watch([id, me], ([nextId, nextMe], previous) => {
  closeDraft()
  const previousId = previous?.[0]
  if (previousId) store.clearNotice(previousId)
  store.clearNotice(nextId)
  if (nextMe && session.isAuthenticated) void store.refresh()
}, { immediate: true })

function toggleOffset(day: number): void {
  offsets.value = offsets.value.includes(day) ? offsets.value.filter(d => d !== day) : [...offsets.value, day]
}

async function submit(): Promise<void> {
  const built = bounds.value ? buildTaskReminder(draft, bounds.value) : buildAnniversaryReminder(offsets.value, anniversaryTime.value)
  if (!built.ok) {
    draftError.value = built.error
    return
  }
  draftError.value = ''
  const target = id.value
  const owner = me.value
  const result = await store.save(kind.value, targetId.value, built.request)
  if (result.outcome === 'done' && id.value === target && me.value === owner) closeDraft()
}

async function disable(): Promise<void> {
  const target = id.value
  const owner = me.value
  const result = await store.save(kind.value, targetId.value, { enabled: false })
  if (result.outcome === 'done' && id.value === target && me.value === owner) closeDraft()
}

async function retry(): Promise<void> {
  const target = id.value
  const owner = me.value
  const result = await store.retry(target)
  if (result.outcome === 'done' && id.value === target && me.value === owner) closeDraft()
}
</script>

<template>
  <section v-if="access.visible" class="stack" data-test="reminder-controls" aria-live="polite">
    <h3 class="card__title">提醒我</h3>
    <p class="card__hint" data-test="reminder-scope">只在应用内的通知里提醒你，时间按北京时间。这里的设置与手机推送无关，不保证手机会收到。只有你自己能看到。</p>

    <p v-if="!known && store.status === 'loading'" class="card__hint" data-test="reminder-loading">正在读取提醒设置…</p>
    <template v-else-if="!known">
      <p class="hint hint--error" role="alert" data-test="reminder-read-error">{{ store.errorText || '提醒设置未读取。' }}</p>
      <button class="btn btn--small btn--soft" type="button" data-test="reminder-read-retry" @click="store.refresh()">重新读取</button>
    </template>
    <template v-else>
      <p v-if="generation === 'disabled'" class="card__hint" data-test="reminder-generation-disabled">提醒生成目前未开启，不会按这里的设置发出提醒。规则仍可查看和修改。</p>
      <p v-else-if="generation === 'unknown'" class="card__hint" data-test="reminder-generation-unknown">暂时无法确认提醒生成是否开启，因此不显示下次提醒时间，保存规则也不代表一定会提醒。</p>
      <p class="kv__value" data-test="reminder-current">当前：{{ current?.enabled ? reminderText(current) : '未开启' }}</p>
      <p v-if="current?.origin === 'successor'" class="card__hint" data-test="reminder-inherited">这条设置是从修改前的任务原样带过来的。</p>
      <p v-if="current?.needs_reset" class="hint hint--error" role="alert" data-test="reminder-needs-reset">需要重新设置：任务修改后，这条提醒不再适用，或时间已经过去，不会按它提醒。你原来的设置没有被改动。</p>
      <p v-if="current?.enabled && generation === 'enabled'" class="card__hint" data-test="reminder-next">{{ nextFireText(current) }}</p>
      <p v-if="current?.fired_this_period" class="card__hint" data-test="reminder-fired">这一期的提醒已经发出。更改时间不会让它再提醒一次。</p>
      <p v-if="current?.enabled && generation === 'enabled'" class="card__hint" data-test="reminder-no-repeat">{{ kind === 'task' ? '每个任务周期只提醒一次' : '同一个日子的同一个提前天数只提醒一次' }}，已经提醒过的不会因为更改时间而重复。</p>
      <p v-if="store.status === 'error'" class="hint hint--error" role="alert" data-test="reminder-read-error">{{ store.errorText }}</p>
    </template>
    <p v-if="access.closedText" class="card__hint" data-test="reminder-closed">{{ access.closedText }}</p>

    <template v-if="action && action.status !== 'submitting'">
      <p class="list__meta" data-test="reminder-pending-terms">这次提交：{{ reminderText(action.payload) }}</p>
      <p class="hint hint--error" role="alert" data-test="reminder-pending">{{ action.errorText }}</p>
      <button class="btn btn--small" type="button" data-test="reminder-retry" @click="retry()">用原请求重试</button>
    </template>
    <p v-else-if="action" class="card__hint" data-test="reminder-saving">正在保存…</p>

    <template v-else-if="known">
      <form v-if="editing && access.canEnable" class="stack" data-test="reminder-form" @submit.prevent="submit">
        <template v-if="bounds">
          <div v-if="bounds.modes.length > 1" class="field">
            <label class="field__label" for="reminder-mode">提醒方式</label>
            <select id="reminder-mode" v-model="draft.mode" class="field__input" data-test="reminder-mode">
              <option v-for="mode in bounds.modes" :key="mode" :value="mode">{{ MODE_LABEL[mode] }}</option>
            </select>
          </div>
          <div v-if="draft.mode === 'before_deadline'" class="field">
            <label class="field__label" for="reminder-before">截止前多少分钟（1～{{ bounds.beforeMax }}）</label>
            <input id="reminder-before" v-model="draft.beforeMinutes" class="field__input" type="number" inputmode="numeric" min="1" :max="bounds.beforeMax" data-test="reminder-before" />
          </div>
          <div v-if="draft.mode === 'local_time' && bounds.daysMax > 0" class="field">
            <label class="field__label" for="reminder-days">提前几天（0～{{ bounds.daysMax }}，从截止日算起）</label>
            <input id="reminder-days" v-model="draft.daysBefore" class="field__input" type="number" inputmode="numeric" min="0" :max="bounds.daysMax" data-test="reminder-days" />
          </div>
          <div v-if="draft.mode === 'absolute'" class="field">
            <label class="field__label" for="reminder-date">日期（北京时间）</label>
            <input id="reminder-date" v-model="draft.atLocalDate" class="field__input" type="date" data-test="reminder-date" />
          </div>
          <div v-if="draft.mode !== 'before_deadline'" class="field">
            <label class="field__label" for="reminder-time">时间（北京时间）</label>
            <input id="reminder-time" v-model="draft.atTime" class="field__input" type="time" data-test="reminder-time" />
          </div>
        </template>
        <template v-else>
          <fieldset class="field">
            <legend class="field__label">提醒日（最多 {{ ANNIVERSARY_MAX_OFFSETS }} 个）</legend>
            <label v-for="day in ANNIVERSARY_OFFSETS" :key="day">
              <input type="checkbox" :checked="offsets.includes(day)" :data-test="`reminder-offset-${day}`" @change="toggleOffset(day)" />
              {{ day === 0 ? '当天' : `提前 ${day} 天` }}
            </label>
          </fieldset>
          <div class="field">
            <label class="field__label" for="reminder-anniversary-time">时间（北京时间）</label>
            <input id="reminder-anniversary-time" v-model="anniversaryTime" class="field__input" type="time" data-test="reminder-time" />
          </div>
        </template>
        <p v-if="draftError" class="hint hint--error" role="alert" data-test="reminder-draft-error">{{ draftError }}</p>
        <div class="row">
          <button class="btn btn--small" type="submit" data-test="reminder-save">保存提醒</button>
          <button class="btn btn--small btn--ghost" type="button" @click="closeDraft()">取消</button>
        </div>
      </form>
      <div v-else class="row">
        <button v-if="access.canEnable" class="btn btn--small btn--soft" type="button" data-test="reminder-edit" @click="startDraft()">
          {{ current?.needs_reset ? '重新设置' : current?.enabled ? '修改提醒' : '设置提醒' }}
        </button>
        <button v-if="access.canDisable && current?.enabled" class="btn btn--small btn--ghost" type="button" data-test="reminder-disable" @click="disable()">关闭提醒</button>
      </div>
    </template>

    <p v-if="notice" :class="notice.ok ? 'hint hint--ok' : 'hint hint--error'" :role="notice.ok ? 'status' : 'alert'" data-test="reminder-notice">{{ notice.text }}</p>
  </section>
</template>
