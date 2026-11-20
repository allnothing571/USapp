import { request } from './client'
import type { MemoryTerms } from './types'

export const PLAN_STATUS_LABEL = { want: '想做', plan: '计划中', finished: '已完成' } as const
export const PLAN_DECISION_LABEL = { approved: '已同意', rejected: '已拒绝', withdrawn: '已撤回' } as const
export type PlanStatus = keyof typeof PLAN_STATUS_LABEL
export type PlanDecision = keyof typeof PLAN_DECISION_LABEL

export interface PlanBody {
  title: string
  date: string | null
  time: string | null
  place: string | null
  note: string | null
}

export interface PlanProposal {
  id: string
  proposer_id: string
  base_version: number
  original: PlanBody
  suggested: PlanBody
  created_at: string
}

export interface SharedPlan {
  id: string
  creator_id: string
  created_at: string
  body: PlanBody
  body_version: number
  status: PlanStatus
  status_version: number
  pending_proposal: PlanProposal | null
  latest_decision: { proposal_id: string; actor_id: string; outcome: PlanDecision; created_at: string } | null
  memory_id?: string | null
  memory_unavailable?: boolean
}

export type PlanCommand =
  /** D20: `notify` is the explicit opt-in captured with the request; undefined sends no field. */
  | { kind: 'create'; body: PlanBody; notify?: boolean }
  | { kind: 'edit' | 'propose'; id: string; version: number; body: PlanBody }
  | { kind: 'decide'; id: string; proposalId: string; outcome: PlanDecision; baseVersion: number; suggested: PlanBody }
  | { kind: 'advance'; id: string; from: Exclude<PlanStatus, 'finished'>; version: number }
  | { kind: 'link'; id: string; memoryId: string }
  | { kind: 'convert'; id: string; terms: MemoryTerms }

const BASE = '/api/v1/shared-plans'
const path = (id: string) => BASE + '/' + encodeURIComponent(id)

export function listPlans(cursor?: string | null): Promise<{ plans: SharedPlan[]; next_cursor: string | null }> {
  return request(BASE, { query: { cursor: cursor ?? undefined, limit: 20 } })
}

export function getPlan(id: string): Promise<SharedPlan> {
  return request(path(id))
}

export function sendPlan(command: PlanCommand, key: string): Promise<unknown> {
  const post = (url: string, body: object) => request(url, { method: 'POST', body, headers: { 'Idempotency-Key': key } })
  switch (command.kind) {
    case 'create': return post(BASE, command.notify === undefined ? { body: command.body } : { body: command.body, notify_partner: command.notify })
    case 'edit': return post(path(command.id) + '/revisions', { body: command.body, expected_version: command.version })
    case 'propose': return post(path(command.id) + '/proposals', { body: command.body, expected_version: command.version })
    case 'decide': return post(path(command.id) + '/proposals/' + encodeURIComponent(command.proposalId) + '/decision', { outcome: command.outcome })
    case 'advance': return post(path(command.id) + '/status', { expected_status: command.from })
    case 'link': return post(path(command.id) + '/memory-link', { memory_id: command.memoryId })
    case 'convert': return post(path(command.id) + '/memory', command.terms)
  }
}
