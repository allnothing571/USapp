/**
 * Contract 0.48.0, admin surface only (docs/admin-task-inspection.md). Read-only inspection of the stored task rows of one
 * selected couple/space. Rewards are canonical decimal strings and are never converted to Number here.
 */
import { adminHttp } from './admin';
import { isDecimal } from './wallet';

export const taskKinds = ['task', 'template', 'period'] as const;
export type TaskKind = (typeof taskKinds)[number];
/** '' = every kind. */
export type KindFilter = TaskKind | '';
export const kindLabels: Record<TaskKind, string> = { task: '任务', template: '周期模板', period: '周期实例' };
export const taskStatuses = ['completed', 'cancelled', 'superseded', 'expired', 'upcoming', 'open'] as const;
export type TaskStatus = (typeof taskStatuses)[number];
export const statusLabels: Record<TaskStatus, string> = {
  completed: '已完成', cancelled: '已取消', superseded: '已被修改取代', expired: '已过期', upcoming: '未开始', open: '进行中',
};
export const scheduleLabels: Record<string, string> = { one_off: '一次性', custom: '自定义区间', daily: '每日', weekly: '每周' };

export interface TaskSpace { id: string; status: string }
export interface TaskSummary {
  id: string;
  kind: TaskKind;
  schedule_type: string;
  template_id: string | null;
  period_date: string | null;
  title: string;
  creator_id: string;
  assignee_id: string;
  reward_flowers: string;
  reward_sunshine: string;
  completion_mode: 'single' | 'count';
  target_count: number;
  progress_count: number;
  status: TaskStatus;
  start_date: string | null;
  deadline_at: string | null;
  created_at: string;
  completed_at: string | null;
}
export interface TaskPage { space: TaskSpace; kind: TaskKind | null; items: TaskSummary[]; next_cursor: string | null; read_at: string }
export interface TaskDetailRow extends TaskSummary {
  description: string;
  timezone: string;
  deadline_local_date: string | null;
  deadline_local_time: string | null;
}
export interface ProgressEvent { id: string; user_id: string; progress_count: number; target_count: number; recorded_at: string; completes_task: boolean }
export interface ProposalTerms {
  title: string; description: string; reward_flowers: string; reward_sunshine: string; completion_mode: string;
  target_count: number; deadline_local_date: string | null; deadline_local_time: string | null; start_date: string | null;
}
export interface Decision { outcome: 'approved' | 'rejected' | 'withdrawn'; decided_by: string; decided_at: string; successor_task_id: string | null }
export interface Proposal { id: string; kind: 'edit' | 'cancel'; proposer_id: string; created_at: string; terms: ProposalTerms | null; decision: Decision | null }
export interface Closure {
  owner_task_id: string; reason: 'cancelled' | 'superseded'; closed_at: string; proposal_id: string;
  successor_task_id: string | null; applies_to_this_task: boolean;
}
export interface TaskDetail {
  space: TaskSpace;
  task: TaskDetailRow;
  completion: { completed_by: string; completed_at: string; reward_flowers: string; reward_sunshine: string } | null;
  progress: { total: number; items: ProgressEvent[] };
  reaction: { id: string; author_id: string; kind: 'emoji' | 'text'; emoji_code: string | null; body: string | null; created_at: string } | null;
  changes: { owner_task_id: string; total: number; items: Proposal[] };
  closure: Closure | null;
  replaces: { task_id: string; proposal_id: string; decided_at: string } | null;
  read_at: string;
}

const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v);
const uuid = (v: unknown): v is string => typeof v === 'string' && /^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const optUuid = (v: unknown) => v === null || uuid(v);
const time = (v: unknown): v is string => typeof v === 'string' && Number.isFinite(Date.parse(v));
const optTime = (v: unknown) => v === null || time(v);
const day = (v: unknown) => typeof v === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(v);
const optDay = (v: unknown) => v === null || day(v);
const optClock = (v: unknown) => v === null || (typeof v === 'string' && /^\d{2}:\d{2}$/.test(v));
const count = (v: unknown, min: number) => typeof v === 'number' && Number.isSafeInteger(v) && v >= min;
const text = (v: unknown): v is string => typeof v === 'string';
const optText = (v: unknown) => v === null || text(v);
const reward = (v: unknown) => isDecimal(v) && !v.startsWith('-');

function isSpace(v: unknown, coupleId: string): v is TaskSpace {
  return object(v) && v.id === coupleId && text(v.status);
}
export function isTaskSummary(v: unknown): v is TaskSummary {
  return object(v) && uuid(v.id) && taskKinds.includes(v.kind as TaskKind) && text(v.schedule_type) && optUuid(v.template_id) &&
    optDay(v.period_date) && text(v.title) && uuid(v.creator_id) && uuid(v.assignee_id) && reward(v.reward_flowers) &&
    reward(v.reward_sunshine) && (v.completion_mode === 'single' || v.completion_mode === 'count') && count(v.target_count, 1) &&
    count(v.progress_count, 0) && taskStatuses.includes(v.status as TaskStatus) && optDay(v.start_date) && optTime(v.deadline_at) &&
    time(v.created_at) && optTime(v.completed_at);
}
export function isTaskPage(v: unknown, coupleId: string, kind: KindFilter): v is TaskPage {
  return object(v) && isSpace(v.space, coupleId) && v.kind === (kind === '' ? null : kind) && time(v.read_at) &&
    Array.isArray(v.items) && v.items.length <= 100 && v.items.every(isTaskSummary) &&
    (kind === '' || v.items.every((i) => (i as TaskSummary).kind === kind)) &&
    (v.next_cursor === null || (text(v.next_cursor) && v.next_cursor.length <= 160));
}
function isEvent(v: unknown): v is ProgressEvent {
  return object(v) && uuid(v.id) && uuid(v.user_id) && count(v.progress_count, 1) && count(v.target_count, 1) && time(v.recorded_at) &&
    typeof v.completes_task === 'boolean';
}
function isTerms(v: unknown): v is ProposalTerms {
  return object(v) && text(v.title) && text(v.description) && reward(v.reward_flowers) && reward(v.reward_sunshine) &&
    text(v.completion_mode) && count(v.target_count, 1) && optDay(v.deadline_local_date) && optClock(v.deadline_local_time) &&
    optDay(v.start_date);
}
function isDecision(v: unknown): v is Decision {
  return object(v) && ['approved', 'rejected', 'withdrawn'].includes(v.outcome as string) && uuid(v.decided_by) && time(v.decided_at) &&
    optUuid(v.successor_task_id);
}
function isProposal(v: unknown): v is Proposal {
  return object(v) && uuid(v.id) && (v.kind === 'edit' || v.kind === 'cancel') && uuid(v.proposer_id) && time(v.created_at) &&
    (v.kind === 'edit' ? isTerms(v.terms) : v.terms === null) && (v.decision === null || isDecision(v.decision));
}
export function isTaskDetail(v: unknown, coupleId: string, taskId: string): v is TaskDetail {
  if (!object(v) || !isSpace(v.space, coupleId) || !time(v.read_at)) return false;
  const t = v.task;
  if (!isTaskSummary(t) || t.id !== taskId) return false;
  const row = t as unknown as Record<string, unknown>;
  if (!text(row.description) || !text(row.timezone) || !optDay(row.deadline_local_date) || !optClock(row.deadline_local_time)) return false;
  const c = v.completion;
  if (!(c === null || (object(c) && uuid(c.completed_by) && time(c.completed_at) && reward(c.reward_flowers) && reward(c.reward_sunshine)))) return false;
  const p = v.progress;
  if (!object(p) || !count(p.total, 0) || !Array.isArray(p.items) || p.items.length > 200 || !p.items.every(isEvent)) return false;
  const r = v.reaction;
  if (!(r === null || (object(r) && uuid(r.id) && uuid(r.author_id) && (r.kind === 'emoji' || r.kind === 'text') &&
    optText(r.emoji_code) && optText(r.body) && time(r.created_at)))) return false;
  const ch = v.changes;
  if (!object(ch) || !uuid(ch.owner_task_id) || !count(ch.total, 0) || !Array.isArray(ch.items) || ch.items.length > 100 ||
    !ch.items.every(isProposal)) return false;
  const k = v.closure;
  if (!(k === null || (object(k) && uuid(k.owner_task_id) && (k.reason === 'cancelled' || k.reason === 'superseded') &&
    time(k.closed_at) && uuid(k.proposal_id) && optUuid(k.successor_task_id) && typeof k.applies_to_this_task === 'boolean'))) return false;
  const x = v.replaces;
  return x === null || (object(x) && uuid(x.task_id) && uuid(x.proposal_id) && time(x.decided_at));
}

export const taskAdminApi = {
  list(coupleId: string, kind: KindFilter, cursor: string | null, limit = 50, signal?: AbortSignal): Promise<unknown> {
    return adminHttp.request<unknown>(`/couples/${encodeURIComponent(coupleId)}/tasks`, {
      query: { kind: kind || undefined, limit, cursor: cursor ?? undefined },
      signal,
    });
  },
  detail(coupleId: string, taskId: string, signal?: AbortSignal): Promise<unknown> {
    return adminHttp.request<unknown>(`/couples/${encodeURIComponent(coupleId)}/tasks/${encodeURIComponent(taskId)}`, { signal });
  },
};
