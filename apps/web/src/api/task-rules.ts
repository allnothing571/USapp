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
import type { CreateTaskRequest } from './types'

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

export function checkTaskTitle(raw: string): TextCheck {
  const chars = Array.from(raw)
  const cps = chars.map((ch) => ch.codePointAt(0) ?? 0)
  if (hasLoneSurrogate(cps)) return { ok: false, message: '标题包含无法识别的字符' }
  if (cps.some(isLineOrControl)) return { ok: false, message: '标题不能包含换行或控制字符' }
  let first = 0
  let last = cps.length
  while (first < last && isTrimmableSpace(cps[first])) first += 1
  while (last > first && isTrimmableSpace(cps[last - 1])) last -= 1
  if (first === last) return { ok: false, message: '标题不能为空' }
  if (last - first > TASK_TITLE_MAX_CODE_POINTS) {
    return { ok: false, message: `标题最多 ${TASK_TITLE_MAX_CODE_POINTS} 个字符` }
  }
  return { ok: true, value: chars.slice(first, last).join('') }
}

export function normalizeTaskDescription(raw: string): TextCheck {
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
  if (last - first > TASK_DESCRIPTION_MAX_CODE_POINTS) {
    return { ok: false, message: `描述最多 ${TASK_DESCRIPTION_MAX_CODE_POINTS} 个字符` }
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
  title: string
  description: string
  flowers: string
  sunshine: string
  /** `<input type="date">` 的值：'' 或 YYYY-MM-DD */
  date: string
  /** `<input type="time">` 的值：'' 或 HH:MM */
  time: string
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
