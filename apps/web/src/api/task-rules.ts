/**
 * 任务发布规则的前端镜像（阶段 1 M3-B；D22/D22a/D23/D5）。
 *
 * 权威校验在服务端 TaskService；这里只做即时提示，并保证提交的是规范化后的值。
 * 两边规则必须一致：
 *   - 标题：首尾只裁剪 Unicode 空格分隔符（Zs）；控制字符与任何换行都拒绝；1～40 个码点。
 *   - 描述：CRLF 先归一为 LF；之后只允许 LF；单独的 CR 与其他控制字符拒绝；
 *           首尾裁剪 Zs 与 LF；最多 500 个码点。
 *   - 奖励：整数，小红花 0～999，阳光值 0～100000，至少一项大于 0。
 *   - 截止：可选的本地日期（YYYY-MM-DD）与可选的本地时间（HH:MM，Asia/Shanghai）。
 *     “必须在未来”只由服务端判断（服务器时钟），前端不臆测。
 * 计数单位是 Unicode 码点（`Array.from`），不是 `.length`。
 */
import type { CreateTaskRequest, RedemptionMode, RewardProductTerms, ScheduleType, Task } from './types'

export const TASK_TITLE_MAX_CODE_POINTS = 40
export const TASK_DESCRIPTION_MAX_CODE_POINTS = 500
export const TASK_MAX_REWARD_FLOWERS = 999
export const TASK_MAX_REWARD_SUNSHINE = 100000

function isTrimmableSpace(cp: number): boolean {
  return (
    cp === 0x20 ||
    cp === 0xa0 ||
    cp === 0x1680 ||
    (cp >= 0x2000 && cp <= 0x200a) ||
    cp === 0x202f ||
    cp === 0x205f ||
    cp === 0x3000
  )
}

function isLineOrControl(cp: number): boolean {
  return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp === 0x2028 || cp === 0x2029
}

function hasLoneSurrogate(cps: number[]): boolean {
  return cps.some((cp) => cp >= 0xd800 && cp <= 0xdfff)
}

export type TextCheck = { ok: true; value: string } | { ok: false; message: string }

export function checkTaskTitle(raw: string, maxCodePoints = TASK_TITLE_MAX_CODE_POINTS): TextCheck {
  const chars = Array.from(raw)
  const cps = chars.map((ch) => ch.codePointAt(0) ?? 0)
  if (hasLoneSurrogate(cps)) return { ok: false, message: '标题包含无法识别的字符' }
  if (cps.some(isLineOrControl)) return { ok: false, message: '标题不能包含换行或控制字符' }
  let first = 0
  let last = cps.length
  while (first < last && isTrimmableSpace(cps[first])) first += 1
  while (last > first && isTrimmableSpace(cps[last - 1])) last -= 1
  if (first === last) return { ok: false, message: '标题不能为空' }
  if (last - first > maxCodePoints) {
    return { ok: false, message: `标题最多 ${maxCodePoints} 个字符` }
  }
  return { ok: true, value: chars.slice(first, last).join('') }
}

export function normalizeTaskDescription(raw: string, maxCodePoints = TASK_DESCRIPTION_MAX_CODE_POINTS): TextCheck {
  const normalized = raw.replace(/\r\n/g, '\n')
  const chars = Array.from(normalized)
  const cps = chars.map((ch) => ch.codePointAt(0) ?? 0)
  if (hasLoneSurrogate(cps)) return { ok: false, message: '描述包含无法识别的字符' }
  if (cps.some((cp) => cp !== 0x0a && isLineOrControl(cp))) {
    return { ok: false, message: '描述只允许普通换行，不能包含其他控制字符' }
  }
  const trimmable = (cp: number) => cp === 0x0a || isTrimmableSpace(cp)
  let first = 0
  let last = cps.length
  while (first < last && trimmable(cps[first])) first += 1
  while (last > first && trimmable(cps[last - 1])) last -= 1
  if (last - first > maxCodePoints) {
    return { ok: false, message: `描述最多 ${maxCodePoints} 个字符` }
  }
  return { ok: true, value: chars.slice(first, last).join('') }
}

/** 输入框里的奖励文本：空白视为 0；只接受十进制整数。 */
export function parseReward(raw: string, max: number): number | null {
  const text = raw.trim()
  if (text === '') return 0
  if (!/^\d{1,6}$/.test(text)) return null
  const value = Number(text)
  return value >= 0 && value <= max ? value : null
}

function validLocalDate(value: string): boolean {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value)
  if (!match) return false
  const year = Number(match[1])
  const month = Number(match[2])
  const day = Number(match[3])
  if (year < 1970 || year > 9998 || month < 1 || month > 12) return false
  const daysInMonth = new Date(Date.UTC(year, month, 0)).getUTCDate()
  return day >= 1 && day <= daysInMonth
}

function validLocalTime(value: string): boolean {
  return /^([01]\d|2[0-3]):[0-5]\d$/.test(value)
}

export interface TaskForm {
  scheduleType?: ScheduleType
  completionMode?: 'single' | 'count'
  targetCount?: string
  title: string
  description: string
  flowers: string
  sunshine: string
  /** `<input type="date">` 的值：'' 或 YYYY-MM-DD */
  date: string
  /** `<input type="time">` 的值：'' 或 HH:MM */
  time: string
  /** Custom range (M5-C): inclusive start/end dates, '' or YYYY-MM-DD. */
  startDate?: string
  endDate?: string
}

export type BuildResult = { ok: true; request: CreateTaskRequest } | { ok: false; message: string }

/** 把表单转成规范化的请求体（字段顺序固定，便于判断“是否同一次发布”）。 */
export function buildCreateTaskRequest(form: TaskForm): BuildResult {
  const title = checkTaskTitle(form.title)
  if (!title.ok) return title
  const description = normalizeTaskDescription(form.description)
  if (!description.ok) return description
  const flowers = parseReward(form.flowers, TASK_MAX_REWARD_FLOWERS)
  if (flowers === null) return { ok: false, message: `小红花奖励必须是 0 到 ${TASK_MAX_REWARD_FLOWERS} 之间的整数` }
  const sunshine = parseReward(form.sunshine, TASK_MAX_REWARD_SUNSHINE)
  if (sunshine === null) {
    return { ok: false, message: `阳光值奖励必须是 0 到 ${TASK_MAX_REWARD_SUNSHINE} 之间的整数` }
  }
  if (flowers === 0 && sunshine === 0) return { ok: false, message: '至少需要一项大于 0 的奖励' }

  const request: CreateTaskRequest = {
    title: title.value,
    description: description.value,
    reward_flowers: flowers,
    reward_sunshine: sunshine,
  }
  if (form.completionMode === 'count') {
    const raw = (form.targetCount ?? '').trim()
    const target = Number(raw)
    if (!/^\d{1,10}$/.test(raw) || !Number.isInteger(target) || target < 1 || target > 2147483647) {
      return { ok: false, message: '目标次数必须是 1 到 2147483647 的整数' }
    }
    request.completion_mode = 'count'
    request.target_count = target
  }
  if (form.scheduleType === 'daily' || form.scheduleType === 'weekly') {
    if (form.completionMode !== 'count') {
      return { ok: false, message: form.scheduleType === 'daily' ? '每天重复请选择累计计数' : '每周重复请选择累计计数' }
    }
    request.schedule_type = form.scheduleType
    return { ok: true, request }
  }
  if (form.scheduleType === 'custom') {
    if (form.completionMode !== 'count') return { ok: false, message: '指定日期范围请选择累计计数' }
    const start = (form.startDate ?? '').trim()
    const end = (form.endDate ?? '').trim()
    if (!validLocalDate(start) || !validLocalDate(end)) return { ok: false, message: '请选择有效的开始和结束日期' }
    // Same-width ISO dates compare lexically. "Not before today" is decided by the server clock.
    if (end < start) return { ok: false, message: '结束日期不能早于开始日期' }
    request.schedule_type = 'custom'
    request.start_date = start
    request.end_date = end
    return { ok: true, request }
  }
  const date = form.date.trim()
  const time = form.time.trim()
  if (date === '' && time !== '') return { ok: false, message: '设置截止时间前请先选择截止日期' }
  if (date !== '') {
    if (!validLocalDate(date)) return { ok: false, message: '截止日期格式应为 YYYY-MM-DD' }
    if (time !== '' && !validLocalTime(time)) return { ok: false, message: '截止时间格式应为 HH:MM' }
    request.deadline = time === '' ? { local_date: date } : { local_date: date, local_time: time }
  }
  return { ok: true, request }
}

/**
 * M6: an edit form prefilled from the task's current terms. The schedule type is fixed, and
 * the same publication rules build the proposed terms. A recurring row's deadline is its
 * period end, not a task term, so it is not copied.
 */
export function formFromTask(task: Task): Required<TaskForm> {
  const schedule = task.schedule_type ?? 'one_off'
  return {
    scheduleType: schedule,
    completionMode: task.completion_mode ?? 'single',
    targetCount: String(task.target_count ?? 1),
    title: task.title,
    description: task.description,
    flowers: String(task.reward_flowers),
    sunshine: String(task.reward_sunshine),
    date: schedule === 'one_off' ? task.deadline?.local_date ?? '' : '',
    time: schedule === 'one_off' ? task.deadline?.local_time ?? '' : '',
    startDate: schedule === 'custom' ? task.start_date ?? '' : '',
    endDate: schedule === 'custom' ? task.deadline?.local_date ?? '' : '',
  }
}

/** M7 reward form: same text rules as tasks; integer price 1-999 flowers; stored redemption mode. */
export interface RewardForm {
  title: string
  description: string
  price: string
  mode: RedemptionMode
}

export type RewardBuildResult = { ok: true; terms: RewardProductTerms } | { ok: false; message: string }

export function buildRewardTerms(form: RewardForm): RewardBuildResult {
  const title = checkTaskTitle(form.title)
  if (!title.ok) return { ok: false, message: title.message.replace('标题', '名称') }
  const description = normalizeTaskDescription(form.description)
  if (!description.ok) return description
  const raw = form.price.trim()
  const price = Number(raw)
  if (!/^\d{1,3}$/.test(raw) || price < 1 || price > 999) {
    return { ok: false, message: '价格必须是 1 到 999 之间的整数（小红花）' }
  }
  if (form.mode !== 'direct' && form.mode !== 'request') return { ok: false, message: '请选择兑换方式' }
  return { ok: true, terms: { title: title.value, description: description.value, price_flowers: price, redemption_mode: form.mode } }
}

export function isRecurring(task: Pick<Task, 'schedule_type'>): boolean {
  return task.schedule_type === 'daily' || task.schedule_type === 'weekly'
}

/**
 * Period label for cards, notices and details. A weekly period runs from its
 * Monday (period_date) to its Sunday (deadline.local_date), Beijing time.
 */
export function periodLabel(task: Pick<Task, 'schedule_type' | 'period_date' | 'deadline' | 'start_date'>): string {
  if (task.schedule_type === 'daily') return `每天重复 · ${task.period_date ?? ''}`
  if (task.schedule_type === 'weekly') {
    const end = task.deadline?.local_date
    return `每周重复 · ${task.period_date ?? ''}${end ? ` 至 ${end}` : ''}`
  }
  if (task.schedule_type === 'custom') {
    const end = task.deadline?.local_date
    return `日期范围 · ${task.start_date ?? ''}${end && end !== task.start_date ? ` 至 ${end}` : ''}`
  }
  return ''
}

/** One read refresh at the next Beijing midnight, not a polling mutation. Weeks also roll at a midnight. */
export function nextDailyRefreshDelay(now = Date.now()): number {
  const parts = new Intl.DateTimeFormat('en-US', {
    timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit',
  }).formatToParts(new Date(now))
  const value = (name: string) => Number(parts.find(part => part.type === name)?.value)
  const next = Date.UTC(value('year'), value('month') - 1, value('day') + 1) - 8 * 3600000
  return Math.max(1, Math.min(86400050, next - now + 50))
}
