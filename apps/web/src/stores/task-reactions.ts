// P2-4: independent actions; no wallet writes or interference with task completion retries.
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { newIdempotencyKey, reactToTask } from '@/api/tasks'
import type { Task, TaskReaction, TaskReactionInput, TaskReactionResponse } from '@/api/types'
import { useSessionStore } from './session'
export function reactionTarget(task: Pick<Task, 'id' | 'period_date'>): string { return `${task.id}:${task.period_date ?? ''}` }
interface Action { taskId: string; payload: TaskReactionInput; key: string; status: 'submitting' | 'unknown' | 'not_sent'; error: string; unresolved: boolean }
export const useTaskReactionsStore = defineStore('task-reactions', () => {
  const actions = ref<Record<string, Action>>({})
  const confirmed = ref<Record<string, TaskReaction>>({})
  const notices = ref<Record<string, string>>({})
  let generation = 0
  function reset(): void { generation++; actions.value = {}; confirmed.value = {}; notices.value = {} }
  async function send(target: string): Promise<void> {
    const action = actions.value[target]
    if (!action) return
    const session = useSessionStore(), owner = session.user?.id, gen = generation
    const current = () => gen === generation && owner === session.user?.id && actions.value[target] === action
    action.status = 'submitting'
    let sent = false
    try {
      await session.ensureCsrfToken()
      if (!current()) return
      if (!getCsrfToken()) throw new OfflineMutationBlockedError()
      sent = true
      const r: TaskReactionResponse = await reactToTask(action.taskId, action.payload, action.key)
      if (!current()) return
      const m = r?.mutation, t = r?.task, period = action.payload.period_date ?? null
      if (!m || !t || typeof m.id !== 'string' || m.id.length === 0 || typeof m.created_at !== 'string' || m.created_at.length === 0 || m.author_id !== owner || m.task_id !== action.taskId || m.period_date !== period ||
        m.kind !== action.payload.kind || t.id !== action.taskId || (t.period_date ?? null) !== period ||
        t.reaction?.id !== m.id || ['task_id', 'period_date', 'author_id', 'kind', 'emoji_code', 'body', 'created_at'].some(k => (t.reaction as unknown as Record<string, unknown>)[k] !== (m as unknown as Record<string, unknown>)[k]) ||
        (action.payload.kind === 'emoji' ? m.emoji_code !== action.payload.emoji_code || m.body !== null : m.body !== action.payload.body || m.emoji_code !== null)) {
        throw new Error('unconfirmed response')
      }
      confirmed.value[target] = m
      delete actions.value[target]
      notices.value[target] = '回应已发出。'
    } catch (error) {
      if (!current()) return
      if (!action.unresolved && error instanceof ApiError && error.status >= 400 && error.status < 500) {
        delete actions.value[target]; notices.value[target] = describeError(error); return
      }
      const unsent = !sent || error instanceof OfflineMutationBlockedError
      action.unresolved ||= !unsent
      action.status = action.unresolved ? 'unknown' : 'not_sent'
      action.error = action.unresolved ? '结果未确认，请用原请求重试。' : '尚未发送，请重试。'
    }
  }
  async function start(task: Task, payload: TaskReactionInput): Promise<void> {
    const target = reactionTarget(task)
    if (actions.value[target]) return
    const captured = { ...payload }
    if (task.period_date) captured.period_date = task.period_date
    actions.value[target] = { taskId: task.id, payload: captured, key: newIdempotencyKey(), status: 'submitting', error: '', unresolved: false }
    delete notices.value[target]
    await send(target)
  }
  async function retry(target: string): Promise<void> { if (actions.value[target]?.status !== 'submitting') await send(target) }
  return { actions, confirmed, notices, start, retry, reset }
})
