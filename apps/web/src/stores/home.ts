/**
 * 首页只读状态（阶段 1 M10，docs/phase1-m10-home.md）：内存状态，不做浏览器持久化，从不写入。
 *
 * - 与 tasks / rewards store 分开：首页刷新只调用读取接口，不触碰任务页与奖励页的
 *   列表、分页、待确认动作或幂等键。
 * - 有界扫描：进行中任务、历史任务与兑换记录各按游标最多读取 HOME_MAX_PAGES 页（每页 HOME_PAGE_LIMIT 条），
 *   不对单条记录发详情请求。今天的任务只来自进行中；待决定的任务变更同时来自进行中与历史
 *   （已完成或已到期的任务上未决定的变更仍可拒绝或撤回），按提议 id 去重。
 *   complete 只表示游标读完，不表示覆盖完整：服务端历史只含最近 30 天，
 *   所以任务变更的结果只代表“进行中 + 最近 30 天”这一可见范围，不是全局总数。读完全部页时 complete = true；仍有下一页时 complete = false，
 *   界面只能显示“至少 N 项”，不能当作精确总数或“没有待办”。任一页失败时整张卡片显示错误，
 *   不保留部分结果。
 * - 两层失效：会话代数（reset() 递增，注销、切换账号、引导失败时由 session store 调用）
 *   与每类读取自己的请求序号（较新的读取胜出）。在途请求的成功和失败落地时，
 *   只有序号、代数与发起时的用户都未变才生效，否则静默丢弃。
 * - “今天”按 Asia/Shanghai 日期，以读取完成时刻（asOf）计算；跨过北京午夜由页面重新读取。
 * - P2-2 纪念日卡片：一次 `GET /api/v1/anniversaries/home`（服务端在完整数据上选择），有自己的序号，
 *   与其他卡片互不影响；天数全部来自服务端的 `as_of_date`。
 */
import { defineStore } from 'pinia'
import { reactive } from 'vue'

import { getHomeAnniversary } from '@/api/anniversaries'
import { describeError } from '@/api/error-messages'
import { listMessages } from '@/api/messages'
import { listRedemptions } from '@/api/rewards'
import { listTasks } from '@/api/tasks'
import type { Anniversary, AnniversaryHomeSource, CivilDate, Message, Redemption, Task } from '@/api/types'
import { useSessionStore } from './session'

export const HOME_PAGE_LIMIT = 100
export const HOME_MAX_PAGES = 5

export type ScanStatus = 'idle' | 'loading' | 'ready' | 'error'

export interface Scan<T> {
  status: ScanStatus
  items: T[]
  /** True only when every page was read (no next cursor remained). */
  complete: boolean
  pages: number
  errorText: string
  /** Epoch milliseconds when the last successful scan finished; the reference for "today". */
  asOf: number
}

function emptyScan<T>(): Scan<T> {
  return { status: 'idle', items: [], complete: false, pages: 0, errorText: '', asOf: 0 }
}

/** Beijing calendar date (YYYY-MM-DD) of an instant. */
export function beijingDate(now: number): string {
  const parts = new Intl.DateTimeFormat('en-US', {
    timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit',
  }).formatToParts(new Date(now))
  const value = (name: string) => parts.find((part) => part.type === name)?.value ?? ''
  return `${value('year')}-${value('month')}-${value('day')}`
}

/**
 * Today's actionable task for `me` at `now`: assigned to me, server status open, and applicable on
 * the Beijing date — undated or not-yet-due one-offs, the current daily period, the weekly period
 * covering today, or a custom range covering today. Upcoming, expired, completed, cancelled and
 * superseded tasks are excluded by status; stale periods are excluded by date.
 */
export function isTodayTask(task: Task, me: string, now: number): boolean {
  if (task.assignee_id !== me || task.status !== 'open') return false
  const today = beijingDate(now)
  const end = task.deadline?.local_date ?? ''
  switch (task.schedule_type ?? 'one_off') {
    case 'daily':
      return task.period_date === today
    case 'weekly':
      return !!task.period_date && task.period_date <= today && end >= today
    case 'custom':
      return !!task.start_date && task.start_date <= today && end >= today
    default:
      return task.deadline === null || Date.parse(task.deadline.at) > now
  }
}

export interface PendingCounts {
  taskDecisions: number
  redemptionConfirmations: number
  cancellationDecisions: number
  fulfillments: number
}

export interface PendingSummary {
  needsMe: PendingCounts
  waiting: PendingCounts
}

function zeroCounts(): PendingCounts {
  return { taskDecisions: 0, redemptionConfirmations: 0, cancellationDecisions: 0, fulfillments: 0 }
}

/**
 * Role-appropriate pending work. Needs me: decide the partner's task change; confirm or reject the
 * partner's redemption request (creator); decide the partner's cancellation proposal; fulfil a paid
 * redemption of my product. Waiting: the same items from the other side.
 */
export function pendingSummary(tasks: Task[], redemptions: Redemption[], me: string): PendingSummary {
  const needsMe = zeroCounts()
  const waiting = zeroCounts()
  // One proposal can appear on several rows (open and history, or several periods of one recurring root).
  const seenChanges = new Set<string>()
  for (const task of tasks) {
    const change = task.pending_change
    if (!change || (task.creator_id !== me && task.assignee_id !== me) || seenChanges.has(change.id)) continue
    seenChanges.add(change.id)
    if (change.proposer_id === me) waiting.taskDecisions += 1
    else needsMe.taskDecisions += 1
  }
  for (const item of redemptions) {
    const creator = item.creator_id === me
    const redeemer = item.redeemer_id === me
    if (!creator && !redeemer) continue
    if (item.status === 'pending_confirmation') {
      if (creator) needsMe.redemptionConfirmations += 1
      else waiting.redemptionConfirmations += 1
    } else if (item.status === 'pending_fulfillment') {
      if (creator) needsMe.fulfillments += 1
      else waiting.fulfillments += 1
      const proposal = item.pending_cancellation
      if (proposal) {
        if (proposal.proposed_by === me) waiting.cancellationDecisions += 1
        else needsMe.cancellationDecisions += 1
      }
    }
  }
  return { needsMe, waiting }
}

/** P2-1: the partner's latest message for Home (one bounded read, independent of the scans). */
export interface LatestMessageState {
  status: ScanStatus
  message: Message | null
  errorText: string
}

/** P2-2: the Home anniversary card (one read; the server selects over the complete dataset). */
export interface HomeAnniversaryState {
  status: ScanStatus
  source: AnniversaryHomeSource | null
  anniversary: Anniversary | null
  asOfDate: CivilDate
  errorText: string
}

function emptyAnniversary(): HomeAnniversaryState {
  return { status: 'idle', source: null, anniversary: null, asOfDate: '', errorText: '' }
}

type PageFetcher<T> = (cursor: string | null) => Promise<{ items: T[]; next: string | null }>

/** A row identity: recurring tasks share the public root id across periods. */
function taskRowKey(task: Task): string {
  return `${task.id}:${task.period_date ?? ''}`
}

export const useHomeStore = defineStore('home', () => {
  /** view=open: today's tasks and open-task change proposals. */
  const tasks = reactive<Scan<Task>>(emptyScan<Task>())
  /** view=history: change proposals left on completed or expired tasks. */
  const history = reactive<Scan<Task>>(emptyScan<Task>())
  const redemptions = reactive<Scan<Redemption>>(emptyScan<Redemption>())
  const latestMessage = reactive<LatestMessageState>({ status: 'idle', message: null, errorText: '' })
  const anniversary = reactive<HomeAnniversaryState>(emptyAnniversary())

  let sessionGeneration = 0
  let latestSeq = 0
  let anniversarySeq = 0
  let taskSeq = 0
  let historySeq = 0
  let redemptionSeq = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function clear<T>(scan: Scan<T>): void {
    Object.assign(scan, emptyScan<T>())
  }

  function reset(): void {
    sessionGeneration += 1
    taskSeq += 1
    historySeq += 1
    redemptionSeq += 1
    latestSeq += 1
    anniversarySeq += 1
    clear(tasks)
    clear(history)
    clear(redemptions)
    Object.assign(latestMessage, { status: 'idle', message: null, errorText: '' })
    Object.assign(anniversary, emptyAnniversary())
  }

  async function scan<T>(target: Scan<T>, fetchPage: PageFetcher<T>, isLatest: () => boolean,
    keyOf: (item: T) => string): Promise<void> {
    const generation = sessionGeneration
    const owner = currentUserId()
    const current = () => isLatest() && stillCurrent(generation, owner)
    target.status = 'loading'
    target.errorText = ''
    const seen = new Set<string>()
    const items: T[] = []
    let cursor: string | null = null
    let pages = 0
    let complete = false
    try {
      while (pages < HOME_MAX_PAGES) {
        const page = await fetchPage(cursor)
        if (!current()) return
        pages += 1
        for (const item of page.items) {
          const key = keyOf(item)
          if (!seen.has(key)) {
            seen.add(key)
            items.push(item)
          }
        }
        cursor = page.next
        if (!cursor) {
          complete = true
          break
        }
      }
    } catch (error) {
      if (!current()) return
      Object.assign(target, emptyScan<T>(), { status: 'error', errorText: describeError(error) })
      return
    }
    Object.assign(target, { status: 'ready', items, complete, pages, errorText: '', asOf: Date.now() })
  }

  function loadTasks(): Promise<void> {
    const seq = ++taskSeq
    return scan(tasks, async (cursor) => {
      const page = await listTasks('open', cursor, HOME_PAGE_LIMIT)
      return { items: page.tasks, next: page.next_cursor }
    }, () => seq === taskSeq, taskRowKey)
  }

  function loadHistory(): Promise<void> {
    const seq = ++historySeq
    return scan(history, async (cursor) => {
      const page = await listTasks('history', cursor, HOME_PAGE_LIMIT)
      return { items: page.tasks, next: page.next_cursor }
    }, () => seq === historySeq, taskRowKey)
  }

  function loadRedemptions(): Promise<void> {
    const seq = ++redemptionSeq
    return scan(redemptions, async (cursor) => {
      const page = await listRedemptions(cursor, HOME_PAGE_LIMIT)
      return { items: page.redemptions, next: page.next_cursor }
    }, () => seq === redemptionSeq, (item) => item.id)
  }

  /** `from=partner&limit=1`: the newest message written to the caller, or none. */
  async function loadLatestMessage(): Promise<void> {
    const seq = ++latestSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const current = () => seq === latestSeq && stillCurrent(generation, owner)
    latestMessage.status = 'loading'
    latestMessage.errorText = ''
    try {
      const page = await listMessages(null, 1, 'partner')
      if (!current()) return
      Object.assign(latestMessage, { status: 'ready', message: page.messages[0] ?? null, errorText: '' })
    } catch (error) {
      if (!current()) return
      Object.assign(latestMessage, { status: 'error', message: null, errorText: describeError(error) })
    }
  }

  /** One request; an error never shows the empty state. */
  async function loadAnniversary(): Promise<void> {
    const seq = ++anniversarySeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const current = () => seq === anniversarySeq && stillCurrent(generation, owner)
    anniversary.status = 'loading'
    anniversary.errorText = ''
    try {
      const response = await getHomeAnniversary()
      if (!current()) return
      Object.assign(anniversary, {
        status: 'ready', source: response.source, anniversary: response.anniversary, asOfDate: response.as_of_date,
        errorText: '',
      })
    } catch (error) {
      if (!current()) return
      Object.assign(anniversary, emptyAnniversary(), { status: 'error', errorText: describeError(error) })
    }
  }

  /** Reads only; each card fails independently. */
  function refresh(): Promise<void> {
    return Promise.all([loadTasks(), loadHistory(), loadRedemptions(), loadLatestMessage(), loadAnniversary()])
      .then(() => undefined)
  }

  return {
    tasks, history, redemptions, latestMessage, anniversary, reset, loadTasks, loadHistory, loadRedemptions,
    loadLatestMessage, loadAnniversary, refresh,
  }
})
