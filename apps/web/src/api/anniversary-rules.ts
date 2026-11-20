/**
 * 纪念日与倒数日的前端规则镜像（阶段 2 P2-2）。权威校验与所有日期计算都在服务端
 * （AnniversaryService / AnniversaryCalendar，按北京时间日期 `as_of_date`）；这里只做即时提示，
 * 并提交规范化后的值：
 *   - 标题：与任务标题相同的字符规则，1～40 个码点；
 *   - 备注：与任务描述相同的字符规则，0～500 个码点；
 *   - 日期：`YYYY-MM-DD`，1900-01-01～2199-12-31，只按字符串与数字校验；
 *   - 显示：只用服务端计算好的天数，从不 `new Date('YYYY-MM-DD')`（那是 UTC 午夜），
 *     也从不用 `toISOString().slice(0, 10)` 取日期。
 */
import { checkTaskTitle, normalizeTaskDescription } from './task-rules'
import type { Anniversary, AnniversaryKind, AnniversaryTerms, CivilDate } from './types'

export const ANNIVERSARY_TITLE_MAX = 40
export const ANNIVERSARY_NOTE_MAX = 500
export const ANNIVERSARY_MIN_DATE: CivilDate = '1900-01-01'
export const ANNIVERSARY_MAX_DATE: CivilDate = '2199-12-31'

export const KIND_LABEL: Record<AnniversaryKind, string> = { anniversary: '纪念日', countdown: '倒数日' }

/** A real calendar day as `YYYY-MM-DD`, checked with integer arithmetic only. */
export function isCivilDate(value: string): boolean {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value)
  if (!match) return false
  const year = Number(match[1])
  const month = Number(match[2])
  const day = Number(match[3])
  if (year < 1 || month < 1 || month > 12 || day < 1) return false
  const leap = (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0
  const days = [31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1]
  return day <= days
}

export type TermsCheck = { ok: true; terms: AnniversaryTerms } | { ok: false; field: keyof AnniversaryTerms; message: string }

export interface TermsDraft {
  title: string
  date: string
  kind: AnniversaryKind | ''
  note: string
}

export function checkAnniversaryTerms(draft: TermsDraft): TermsCheck {
  const title = checkTaskTitle(draft.title)
  if (!title.ok) {
    const message = title.message.includes('最多') ? `标题最多 ${ANNIVERSARY_TITLE_MAX} 个字符` : title.message
    return { ok: false, field: 'title', message }
  }
  if (Array.from(title.value).length > ANNIVERSARY_TITLE_MAX) {
    return { ok: false, field: 'title', message: `标题最多 ${ANNIVERSARY_TITLE_MAX} 个字符` }
  }
  // Fixed-width YYYY-MM-DD strings compare in calendar order.
  if (!isCivilDate(draft.date) || draft.date < ANNIVERSARY_MIN_DATE || draft.date > ANNIVERSARY_MAX_DATE) {
    return { ok: false, field: 'date', message: '请选择 1900-01-01 到 2199-12-31 之间的有效日期' }
  }
  if (draft.kind !== 'anniversary' && draft.kind !== 'countdown') {
    return { ok: false, field: 'kind', message: '请选择纪念日或倒数日' }
  }
  const note = normalizeTaskDescription(draft.note)
  if (!note.ok) {
    const message = note.message.includes('最多') ? `备注最多 ${ANNIVERSARY_NOTE_MAX} 个字符` : note.message.replace('描述', '备注')
    return { ok: false, field: 'note', message }
  }
  return { ok: true, terms: { title: title.value, date: draft.date, kind: draft.kind, note: note.value } }
}

export function sameTerms(a: AnniversaryTerms, b: AnniversaryTerms): boolean {
  return a.title === b.title && a.date === b.date && a.kind === b.kind && a.note === b.note
}

/** `2026-10-29` → `2026年10月29日`, from the string itself (no Date object). */
export function civilDateLabel(date: CivilDate): string {
  const match = /^(\d{4,})-(\d{2})-(\d{2})$/.exec(date)
  if (!match) return date
  return `${Number(match[1])}年${Number(match[2])}月${Number(match[3])}日`
}

/** The main line for an item, from the server-computed values only (owner choice O2: N = T − D). */
export function anniversaryHeadline(item: Pick<Anniversary, 'kind' | 'computed'>): string {
  const c = item.computed
  if (item.kind === 'countdown') {
    const n = c.days_until ?? 0
    if (n > 0) return `还有 ${n} 天`
    if (n === 0) return '就是今天'
    return `已过 ${-n} 天`
  }
  const since = c.days_since ?? 0
  if (since < 0) return `还有 ${-since} 天`
  return `已经 ${since} 天`
}

/** The secondary line for an anniversary's next yearly occurrence (empty for countdowns). */
export function anniversaryNextLine(item: Pick<Anniversary, 'kind' | 'computed'>): string {
  const next = item.computed.next
  if (item.kind !== 'anniversary' || !next) return ''
  if (next.days_until === 0) return next.years >= 1 ? `今天是 ${next.years} 周年` : '就是今天'
  if (next.years >= 1) return `距 ${next.years} 周年还有 ${next.days_until} 天（${civilDateLabel(next.date)}）`
  return ''
}
