import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { getPlan, listPlans, sendPlan, type PlanBody, type PlanCommand, type SharedPlan } from '@/api/shared-plans'
import { isValidNotificationFacts } from '@/api/notifications'
import { newIdempotencyKey } from '@/api/tasks'
import { isUsableMemoryResponse } from './memories'
import { useSessionStore } from './session'

type Action = { command: PlanCommand; key: string; state: 'sending' | 'unknown' | 'not_sent'; error: string; uncertain: boolean }
type Detail = { plan: SharedPlan | null; loading: boolean; error: string }
type Result = { ok: true; id: string } | { ok: false }

export const isPlanId = (id: unknown): id is string =>
  typeof id === 'string' && /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/i.test(id)

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

function isBody(value: unknown): value is PlanBody {
  return isRecord(value) && typeof value.title === 'string' && value.title.trim().length > 0 &&
    [value.date, value.time, value.place, value.note].every(field => field === null || typeof field === 'string')
}

function isVersion(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value > 0
}

function isPlan(value: unknown): value is SharedPlan {
  if (!isRecord(value) || !isPlanId(value.id) || !isPlanId(value.creator_id) || typeof value.created_at !== 'string' ||
      !isBody(value.body) || !isVersion(value.body_version) || !isVersion(value.status_version) ||
      (value.status === 'want' ? value.status_version !== 1 :
        value.status === 'plan' ? value.status_version !== 2 : value.status !== 'finished' || value.status_version !== 3)) return false
  if (value.memory_id !== undefined && value.memory_id !== null && (!isPlanId(value.memory_id) || value.status !== 'finished')) return false
  if (value.memory_unavailable !== undefined && (typeof value.memory_unavailable !== 'boolean' ||
    (value.memory_unavailable && value.memory_id !== null))) return false
  const p = value.pending_proposal
  const d = value.latest_decision
  return (p === null || (isRecord(p) && isPlanId(p.id) && isPlanId(p.proposer_id) && p.proposer_id !== value.creator_id &&
      p.base_version === value.body_version && isBody(p.original) && isBody(p.suggested) && typeof p.created_at === 'string')) &&
    (d === null || (isRecord(d) && isPlanId(d.proposal_id) && isPlanId(d.actor_id) && typeof d.created_at === 'string' &&
      (d.outcome === 'approved' || d.outcome === 'rejected' || d.outcome === 'withdrawn')))
}

function sameBody(a: PlanBody, b: PlanBody): boolean {
  return a.title === b.title && a.date === b.date && a.time === b.time && a.place === b.place && a.note === b.note
}

function confirmPlan(response: unknown, command: Exclude<PlanCommand, { kind: 'link' | 'convert' }>, owner: string): SharedPlan | null {
  if (!isRecord(response) || typeof response.replayed !== 'boolean' || !isPlan(response.plan) ||
      !isRecord(response.mutation) || !isPlanId(response.mutation.id) ||
      response.mutation.operation !== command.kind) return null
  const { plan, mutation } = response
  if (command.kind !== 'create' && plan.id !== command.id) return null
  const matchesBody = (version: number, body: PlanBody) => mutation.body_version === version &&
    (plan.body_version > version || (plan.body_version === version && sameBody(plan.body, body)))
  switch (command.kind) {
    // D20: the original decision must agree with the captured choice; no_partner is a successful creation.
    case 'create': return plan.creator_id === owner && mutation.status === 'want' && mutation.status_version === 1 &&
      matchesBody(1, command.body) && isValidNotificationFacts(response.notification, command.notify, true) ? plan : null
    case 'edit': return matchesBody(command.version + 1, command.body) ? plan : null
    case 'propose': {
      const pending = plan.pending_proposal
      if (mutation.body_version !== null || plan.body_version < command.version) return null
      if (pending && pending.id === mutation.id) {
        return pending.proposer_id === owner && pending.base_version === command.version && sameBody(pending.suggested, command.body)
          ? plan : null
      }
      return response.replayed ? plan : null
    }
    case 'decide':
      if (mutation.proposal_id !== command.proposalId || mutation.outcome !== command.outcome || plan.body_version < command.baseVersion) return null
      return (command.outcome === 'approved' ? matchesBody(command.baseVersion + 1, command.suggested) : mutation.body_version === null)
        ? plan : null
    case 'advance': {
      const next = command.from === 'want' ? 'plan' : 'finished'
      return mutation.status === next && mutation.status_version === command.version + 1 &&
        (plan.status_version > command.version + 1 || plan.status === next) ? plan : null
    }
  }
}

export const useSharedPlansStore = defineStore('shared-plans', () => {
  const items = ref<SharedPlan[]>([])
  const cursor = ref<string | null>(null)
  const listLoading = ref(false)
  const listError = ref('')
  const details = ref<Record<string, Detail>>({})
  const actions = ref<Record<string, Action>>({})
  const errors = ref<Record<string, string>>({})
  let generation = 0
  let listRead = 0
  const detailReads = new Map<string, number>()

  function reset(): void {
    generation++
    items.value = []
    cursor.value = null
    listLoading.value = false
    listError.value = ''
    details.value = {}
    actions.value = {}
    errors.value = {}
    detailReads.clear()
  }

  function identity() {
    const session = useSessionStore()
    const owner = session.user?.id
    const started = generation
    return { owner, current: () => !!owner && generation === started && session.user?.id === owner }
  }

  async function loadList(more = false): Promise<void> {
    if (more && (listLoading.value || !cursor.value)) return
    const who = identity()
    if (!who.owner) return
    const seq = ++listRead
    const before = more ? cursor.value : null
    listLoading.value = true
    listError.value = ''
    try {
      const result = await listPlans(before)
      if (!who.current() || seq !== listRead) return
      if (!Array.isArray(result?.plans) || !result.plans.every(isPlan) ||
        (result.next_cursor !== null && (typeof result.next_cursor !== 'string' || !result.next_cursor || result.next_cursor === before))) {
        throw new Error('Invalid plan page')
      }
      const previous = more ? items.value : []
      items.value = [...new Map([...previous, ...result.plans].map(plan => [plan.id, plan])).values()]
      cursor.value = result.next_cursor
    } catch (error) {
      if (who.current() && seq === listRead) listError.value = describeError(error)
    } finally {
      if (who.current() && seq === listRead) listLoading.value = false
    }
  }

  function absorb(plan: SharedPlan): void {
    const previous = details.value[plan.id]?.plan
    if (previous && (plan.body_version < previous.body_version || plan.status_version < previous.status_version)) return
    const memory = plan.memory_unavailable === true ? null : plan.memory_id ?? previous?.memory_id
    const unavailable = plan.memory_unavailable === undefined ? previous?.memory_unavailable : plan.memory_unavailable
    details.value[plan.id] = { plan: { ...plan, memory_id: memory, memory_unavailable: unavailable }, loading: false, error: '' }
  }

  async function loadDetail(id: string): Promise<void> {
    if (!isPlanId(id)) return
    const who = identity()
    if (!who.owner) return
    const seq = (detailReads.get(id) ?? 0) + 1
    detailReads.set(id, seq)
    const current = () => who.current() && detailReads.get(id) === seq
    details.value[id] = { plan: details.value[id]?.plan ?? null, loading: true, error: '' }
    try {
      const result = await getPlan(id)
      if (!current()) return
      if (!isPlan(result) || result.id !== id || result.memory_id === undefined) throw new Error('Invalid plan detail')
      absorb(result)
    } catch (error) {
      if (current()) {
        const prior = details.value[id]?.plan ?? null
        details.value[id] = { plan: error instanceof ApiError && error.status === 404 ? null : prior, loading: false, error: describeError(error) }
      }
    } finally {
      if (current() && details.value[id]?.loading) details.value[id].loading = false
    }
  }

  async function execute(slot: string, action: Action): Promise<Result> {
    const who = identity()
    if (!who.owner) return { ok: false }
    action.state = 'sending'
    action.error = ''
    errors.value[slot] = ''
    const { command, key } = action
    try {
      if (!getCsrfToken()) {
        action.state = action.uncertain ? 'unknown' : 'not_sent'
        action.error = '请重新确认会话，然后使用原请求重试。'
        return { ok: false }
      }
      const response = await sendPlan(command, key)
      if (!who.current()) return { ok: false }
      let plan: SharedPlan | null = null
      let memoryId: string | undefined
      if (command.kind === 'link') {
        if (isRecord(response) && response.plan_id === command.id && response.memory_id === command.memoryId &&
          typeof response.replayed === 'boolean' && isPlanId(response.memory_id)) memoryId = command.memoryId
      } else if (command.kind === 'convert') {
        if (isUsableMemoryResponse(response, { id: slot, kind: 'create', terms: command.terms, key,
          status: 'submitting', errorText: '' }, who.owner) && isPlanId(response.memory.id)) memoryId = response.memory.id
      } else {
        plan = confirmPlan(response, command, who.owner)
      }
      if (!plan && !memoryId) throw new Error('Invalid mutation receipt')
      const id = command.kind === 'create' ? plan!.id : command.id
      detailReads.set(id, (detailReads.get(id) ?? 0) + 1)
      if (plan) absorb(plan)
      else {
        const detail = details.value[id]?.plan
        if (detail) absorb({ ...detail, memory_id: memoryId })
      }
      delete actions.value[slot]
      if (command.kind !== 'create') void loadDetail(id)
      return { ok: true, id }
    } catch (error) {
      if (!who.current()) return { ok: false }
      const definite = error instanceof ApiError && error.status >= 400 && error.status < 500
      if (!action.uncertain && definite) {
        delete actions.value[slot]
        errors.value[slot] = describeError(error)
      } else {
        if (!(error instanceof OfflineMutationBlockedError)) action.uncertain = true
        action.state = action.uncertain ? 'unknown' : 'not_sent'
        action.error = action.uncertain ? '结果未确认，请使用原请求重试。' : describeError(error)
      }
      return { ok: false }
    }
  }

  function submit(command: PlanCommand): Promise<Result> {
    const slot = command.kind === 'create' ? 'new' : command.id
    if (!identity().owner || actions.value[slot]) return Promise.resolve({ ok: false })
    if (command.kind !== 'create' && !isPlanId(command.id)) return Promise.resolve({ ok: false })
    // Commands contain only JSON fields. Copy nested form data out of Vue's proxies.
    const captured = JSON.parse(JSON.stringify(command)) as PlanCommand
    actions.value[slot] = { command: captured, key: newIdempotencyKey(), state: 'sending', error: '', uncertain: false }
    return execute(slot, actions.value[slot])
  }

  function retry(slot: string): Promise<Result> {
    const action = actions.value[slot]
    return action && action.state !== 'sending' ? execute(slot, action) : Promise.resolve({ ok: false })
  }

  return { items, cursor, listLoading, listError, details, actions, errors, reset, loadList, loadDetail, submit, retry }
})
