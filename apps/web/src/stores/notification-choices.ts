/**
 * 通知偏好（阶段 2 P2-3b）：调用者自己在五类普通操作上“上一次明确的选择”，只读，内存状态。
 *
 * - 默认关闭；读取失败时保持关闭并给出重试，绝不悄悄开启。
 * - 会话代数：reset()（注销、切换账号、引导失败）后，旧账号的晚到结果被丢弃。
 * - 每次读取有序号；只有最新开始的读取的结果会被采用（较早开始的晚到响应被丢弃）。
 * - load() 合并并发读取；refresh()（确认写入之后）作废进行中的旧读取并开始新的读取。
 * - 新表单通过 mark() 要求一次在表单开始时或之后才完成的读取，从不使用早先缓存的默认值
 *   （其他设备的更改因此也会被新表单看到）。
 * - 这里只提供“新表单的默认值”。表单在开始时取一次默认值；之后的读取不会改动用户已经碰过的
 *   开关、正在编辑的草稿或提交中 / 结果未确认的动作（见 components/NotifyPartnerToggle.vue）。
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { describeError } from '@/api/error-messages'
import {
  CHOICE_OPERATIONS,
  getNotificationChoices,
  type ChoiceOperation,
  type NotificationChoice,
} from '@/api/notifications'
import { useSessionStore } from './session'

export type ChoicesStatus = 'idle' | 'loading' | 'ready' | 'error'

function isValidChoices(value: unknown): value is Record<ChoiceOperation, NotificationChoice> {
  if (!value || typeof value !== 'object') return false
  const choices = value as Record<string, unknown>
  return CHOICE_OPERATIONS.every((op) => {
    const item = choices[op] as Partial<NotificationChoice> | undefined
    if (!item || typeof item.enabled !== 'boolean') return false
    if (item.seq === null) return item.enabled === false
    return Number.isSafeInteger(item.seq) && (item.seq as number) >= 1
  })
}

export const useNotificationChoicesStore = defineStore('notification-choices', () => {
  const status = ref<ChoicesStatus>('idle')
  const choices = ref<Record<ChoiceOperation, NotificationChoice> | null>(null)
  const errorText = ref('')
  /** The account the loaded choices belong to. */
  const owner = ref<string | null>(null)

  let generation = 0
  let seq = 0
  let inFlight: Promise<void> | null = null
  let inFlightSeq = 0
  /** Sequence of the read whose result is currently applied (0: none for this account). */
  const readySeq = ref(0)

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function reset(): void {
    generation += 1
    seq += 1
    inFlight = null
    inFlightSeq = 0
    readySeq.value = 0
    status.value = 'idle'
    choices.value = null
    errorText.value = ''
    owner.value = null
  }

  async function run(s: number): Promise<void> {
    const g = generation
    const user = currentUserId()
    if (!user) return
    if (status.value !== 'ready') status.value = 'loading'
    errorText.value = ''
    try {
      const response = await getNotificationChoices()
      if (g !== generation || s !== seq || currentUserId() !== user) return
      if (!isValidChoices(response?.choices)) throw new Error('通知偏好响应无效')
      choices.value = response.choices
      owner.value = user
      readySeq.value = s
      status.value = 'ready'
    } catch (error) {
      if (g !== generation || s !== seq || currentUserId() !== user) return
      // Keep earlier defaults of the same account; never invent "on".
      if (owner.value !== user) {
        choices.value = null
        status.value = 'error'
      } else {
        status.value = 'ready'
      }
      errorText.value = describeError(error)
    }
  }

  function start(): number {
    const g = generation
    const s = ++seq
    const p = run(s).finally(() => {
      if (g === generation && inFlight === p) {
        inFlight = null
        inFlightSeq = 0
      }
    })
    inFlight = p
    inFlightSeq = s
    return s
  }

  /** Coalesces concurrent requests; a new call after completion reads again. */
  function load(): Promise<void> {
    if (!inFlight) start()
    return inFlight as Promise<void>
  }

  /**
   * After a confirmed write: reads started before it are superseded (their late responses are
   * discarded) and a fresh read starts. Never reuses the older in-flight promise.
   */
  function refresh(): Promise<void> {
    inFlight = null
    start()
    return inFlight as unknown as Promise<void>
  }

  /**
   * A new form asks for defaults no older than its own start: joins a read already in flight
   * (it completes after this moment) or starts one. Returns the sequence the form must wait for.
   */
  function mark(): number {
    if (inFlight) return inFlightSeq
    return start()
  }

  /** The remembered default for the CURRENT account, or off when unknown. */
  function enabledFor(op: ChoiceOperation): boolean {
    const user = currentUserId()
    if (status.value !== 'ready' || !choices.value || owner.value !== user) return false
    return choices.value[op].enabled
  }

  /** True once this account's defaults are known (so a form may take its initial value). */
  function readyFor(): boolean {
    return status.value === 'ready' && owner.value !== null && owner.value === currentUserId()
  }

  return { status, choices, errorText, readySeq, reset, load, refresh, mark, enabledFor, readyFor }
})
