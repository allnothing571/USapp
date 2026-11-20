/**
 * 提醒的本地规则与文案（P2-6 R3，docs/phase2-reminders.md 第 5、6 节）。这里只做即时提示，服务端是权威校验；
 * 所有时间都是北京时间，不使用设备时区，也不在前端计算下次提醒时间。
 */
import { isCivilDate } from './anniversary-rules'
import type { AnniversaryReminderRequest, ReminderRequest, ReminderRuleCurrent, ReminderRuleFacts, TaskReminderRequest } from './reminders'
import type { Task } from './types'

export const ANNIVERSARY_OFFSETS: readonly number[] = [0, 1, 3, 7, 14, 30]
export const ANNIVERSARY_MAX_OFFSETS = 3
export const ANNIVERSARY_DEFAULT_TIME = '09:00'

const TIME = /^([01][0-9]|2[0-3]):[0-5][0-9]$/

export type TaskReminderMode = 'before_deadline' | 'local_time' | 'absolute'

export interface TaskReminderBounds {
  modes: readonly TaskReminderMode[]
  beforeMax: number
  daysMax: number
}

type TaskShape = Pick<Task, 'schedule_type' | 'deadline'>

/** The schedule-specific bounds (Section 5.2), which keep an instant inside its own period. */
export function taskReminderBounds(task: TaskShape): TaskReminderBounds {
  switch (task.schedule_type) {
    case 'daily': return { modes: ['before_deadline', 'local_time'], beforeMax: 1440, daysMax: 0 }
    case 'weekly':
    case 'custom': return { modes: ['before_deadline', 'local_time'], beforeMax: 10080, daysMax: 6 }
    default:
      return task.deadline
        ? { modes: ['before_deadline', 'local_time'], beforeMax: 10080, daysMax: 7 }
        : { modes: ['absolute'], beforeMax: 0, daysMax: 0 }
  }
}

export interface TaskReminderAccess {
  /** Only the assignee ever sees the control. */
  visible: boolean
  canEnable: boolean
  /** Disabling is allowed after completion or expiry, and never on a closed root. */
  canDisable: boolean
  closedText: string
}

/** Mirrors the API's target rules; the server still decides. */
export function taskReminderAccess(task: Pick<Task, 'assignee_id' | 'schedule_type' | 'status' | 'closure'>, me: string): TaskReminderAccess {
  if (task.assignee_id !== me) return { visible: false, canEnable: false, canDisable: false, closedText: '' }
  if (task.closure?.reason === 'superseded' || task.status === 'superseded') {
    return { visible: true, canEnable: false, canDisable: false, closedText: '这个任务已被新任务替换，这里不能再修改提醒。你在这里的提醒设置（包括关闭状态）已原样带到新任务，可以在新任务上查看或修改。' }
  }
  if (task.closure?.reason === 'cancelled' || task.status === 'cancelled') {
    return { visible: true, canEnable: false, canDisable: false, closedText: '这个任务已取消，不能再设置提醒。' }
  }
  const recurring = task.schedule_type === 'daily' || task.schedule_type === 'weekly'
  // A recurring rule belongs to the root: one finished period does not end it.
  if (!recurring && task.status === 'completed') return { visible: true, canEnable: false, canDisable: true, closedText: '这个任务已完成，不会再提醒。' }
  if (!recurring && task.status === 'expired') return { visible: true, canEnable: false, canDisable: true, closedText: '这个任务已过截止时间，不会再提醒。' }
  return { visible: true, canEnable: true, canDisable: true, closedText: '' }
}

export interface TaskReminderDraft {
  mode: TaskReminderMode
  beforeMinutes: string | number
  daysBefore: string | number
  atTime: string
  atLocalDate: string
}

export type BuiltReminder<T> = { ok: true; request: T } | { ok: false; error: string }

/** A number input's v-model yields a number once it parses, and a string otherwise. */
function wholeNumber(value: string | number): number | null {
  const text = String(value).trim()
  return /^[0-9]{1,6}$/.test(text) ? Number(text) : null
}

export function buildTaskReminder(draft: TaskReminderDraft, bounds: TaskReminderBounds): BuiltReminder<TaskReminderRequest> {
  if (!bounds.modes.includes(draft.mode)) return { ok: false, error: '这个任务不能使用这种提醒方式。' }
  if (draft.mode === 'before_deadline') {
    const minutes = wholeNumber(draft.beforeMinutes)
    if (minutes === null || minutes < 1 || minutes > bounds.beforeMax) {
      return { ok: false, error: `提前分钟数需要在 1 到 ${bounds.beforeMax} 之间。` }
    }
    return { ok: true, request: { enabled: true, mode: 'before_deadline', before_minutes: minutes } }
  }
  if (!TIME.test(draft.atTime)) return { ok: false, error: '请填写提醒时间（北京时间，时:分）。' }
  if (draft.mode === 'local_time') {
    const days = wholeNumber(draft.daysBefore)
    if (days === null || days > bounds.daysMax) {
      return { ok: false, error: bounds.daysMax === 0 ? '每天重复的任务只能在当天提醒。' : `提前天数需要在 0 到 ${bounds.daysMax} 之间。` }
    }
    return { ok: true, request: { enabled: true, mode: 'local_time', days_before: days, at_time: draft.atTime } }
  }
  if (!isCivilDate(draft.atLocalDate)) return { ok: false, error: '请填写提醒日期（北京时间）。' }
  return { ok: true, request: { enabled: true, mode: 'absolute', at_local_date: draft.atLocalDate, at_time: draft.atTime } }
}

/** Ascending and distinct, which is how the API stores a set. */
export function buildAnniversaryReminder(offsets: readonly number[], atTime: string): BuiltReminder<AnniversaryReminderRequest> {
  const sorted = [...new Set(offsets)].sort((a, b) => a - b)
  if (sorted.length < 1 || sorted.length > ANNIVERSARY_MAX_OFFSETS) return { ok: false, error: `请选择 1 到 ${ANNIVERSARY_MAX_OFFSETS} 个提醒日。` }
  if (!sorted.every(day => ANNIVERSARY_OFFSETS.includes(day))) return { ok: false, error: '提前天数只能从给定的选项中选择。' }
  if (!TIME.test(atTime)) return { ok: false, error: '请填写提醒时间（北京时间，时:分）。' }
  return { ok: true, request: { enabled: true, days_before: sorted, at_time: atTime } }
}

function offsetText(day: number): string {
  return day === 0 ? '当天' : `提前 ${day} 天`
}

/** The settings of a request or a stored rule, in Beijing-time wording. */
export function reminderText(rule: ReminderRequest | ReminderRuleFacts): string {
  if (!rule.enabled) return '关闭'
  if ('offsets' in rule && rule.offsets) return `${rule.offsets.map(offsetText).join('、')}，北京时间 ${rule.at_time}`
  if (!('mode' in rule)) {
    const days = (rule as { days_before: number[] }).days_before
    return `${days.map(offsetText).join('、')}，北京时间 ${(rule as { at_time: string }).at_time}`
  }
  switch (rule.mode) {
    case 'before_deadline': return `截止前 ${rule.before_minutes} 分钟`
    case 'local_time': return `${rule.days_before === 0 ? '截止当天' : `截止前 ${rule.days_before} 天`}，北京时间 ${rule.at_time}`
    case 'absolute': return `北京时间 ${rule.at_local_date} ${rule.at_time}`
    default: return '已开启'
  }
}

/** A server instant in Beijing time, or null if it is not one. Never derived on the device. */
export function beijingInstant(iso: string | null): string | null {
  if (!iso) return null
  const time = new Date(iso)
  if (Number.isNaN(time.getTime())) return null
  return time.toLocaleString('zh-CN', {
    timeZone: 'Asia/Shanghai', year: 'numeric', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false,
  })
}

/** What the current rule honestly says about the next reminder. null next_fire_at invents nothing. */
export function nextFireText(current: ReminderRuleCurrent | undefined): string {
  if (!current || !current.enabled) return '没有开启提醒。'
  if (current.needs_reset) return '需要重新设置后才会提醒。'
  const at = beijingInstant(current.next_fire_at)
  return at ? `按当前设置计算的下次时间：${at}（北京时间）。` : '按当前设置，没有下一次提醒时间。'
}
