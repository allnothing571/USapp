/**
 * 留言板 store（阶段 2 P2-1）：内存状态，不做浏览器持久化，没有离线写队列。
 *
 * - 会话代数 `sessionGeneration`：reset()（注销、切换账号、引导失败）时递增；在途请求的成功
 *   和失败落地时，只有代数未变且当前用户仍是发起者才生效，否则静默丢弃。
 * - 列表与详情各有请求序号（较新的读取胜出）。列表按 (created_at, id) 倒序游标分页。
 * - 写动作：发留言用键 'new'，回复用键 `reply:<留言>`，每个目标至多一个。网络错误、5xx
 *   （含 503 RESULT_UNKNOWN）或离线 / 本地缺少令牌时保留原请求标识与原文，只在用户点击“重试”时
 *   用同一个键重发。从不自动重放写请求。
 * - 一旦某次发送的结果未确认，后续错误不能证明原请求失败：保留原键、原文与“未确认”状态，
 *   直到收到可确认的同键成功响应。首次发送的明确 4xx 仍结束动作。
 *   注销或身份切换会清空内存中的动作（不持久化、不跨账号保留）：此时原请求是否已写入只能通过
 *   重新登录后查看留言历史确认；再次手动发送会用新键，可能产生一条重复留言。
 * - P2-1 修复：回复只会增加、不会消失。确认成功的回复（以及任何读取到的回复）记在内存里，
 *   之后落地的较早读取（列表、详情、单条刷新）不会把它覆盖成“未回复”。
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { createMessage, getMessage, listMessages, replyMessage } from '@/api/messages'
import { isValidNotificationFacts } from '@/api/notifications'
import { newIdempotencyKey } from '@/api/tasks'
import type { Message, MessageMutationResponse, MessageReply } from '@/api/types'
import { useNotificationChoicesStore } from './notification-choices'
import { useSessionStore } from './session'

export type MessageListStatus = 'idle' | 'loading' | 'ready' | 'error'
export type MessageActionStatus = 'submitting' | 'unknown' | 'not_sent'

export interface MessageAction {
  /** 'new' for a message, 'reply:<message id>' for a reply. */
  id: string
  kind: 'create' | 'reply'
  messageId?: string
  body: string
  /** P2-3b: the explicit notify_partner captured with the body and key (undefined: not sent). */
  notify?: boolean
  key: string
  status: MessageActionStatus
  errorText: string
  /** True once a send with this key may have reached the server without a confirmed answer. */
  unresolved?: boolean
}

export interface MessageNotice {
  id: string
  ok: boolean
  text: string
}

export type MessageResult =
  | { outcome: 'done'; response: MessageMutationResponse }
  | { outcome: 'unknown' }
  | { outcome: 'not_sent' }
  | { outcome: 'rejected'; errorCode: string | null }
  | { outcome: 'discarded' }
  | { outcome: 'busy' }

export interface MessageDetailState {
  id: string | null
  status: MessageListStatus
  message: Message | null
  errorText: string
}

function isUnconfirmedOrUnsent(error: unknown): boolean {
  return !(error instanceof ApiError && error.status >= 400 && error.status < 500)
}

function isUsableMutation(response: unknown, action: MessageAction, owner: string):
  response is MessageMutationResponse {
  if (!response || typeof response !== 'object') return false
  const r = response as Partial<MessageMutationResponse>
  if (r.mutation?.action !== action.kind || typeof r.message?.id !== 'string' || r.message.id.length === 0) return false
  if (r.mutation.message_id !== r.message.id) return false
  if (r.mutation.author_id !== owner || r.mutation.body !== action.body) return false
  // The notification facts are validated separately from the business mutation.
  if (!isValidNotificationFacts(r.notification, action.notify)) return false
  if (action.kind === 'create') return r.message.author_id === owner && r.message.body === action.body
  return r.message.id === action.messageId && !!r.message.reply &&
    r.message.reply.author_id === owner && r.message.reply.body === action.body
}

const DONE_TEXT: Record<MessageAction['kind'], string> = {
  create: '留言已发出，对方会在留言板和首页看到它。',
  reply: '回复已发出。',
}

export const useMessagesStore = defineStore('messages', () => {
  const items = ref<Message[]>([])
  const status = ref<MessageListStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')
  const detail = ref<MessageDetailState>({ id: null, status: 'idle', message: null, errorText: '' })
  const actions = ref<Record<string, MessageAction>>({})
  const notices = ref<MessageNotice[]>([])

  let sessionGeneration = 0
  let listSeq = 0
  let detailSeq = 0
  /** Replies only appear, never disappear: remembered so an older read cannot hide them. */
  const knownReplies = new Map<string, MessageReply>()

  function withKnownReply(message: Message): Message {
    if (message.reply) {
      knownReplies.set(message.id, message.reply)
      return message
    }
    const reply = knownReplies.get(message.id)
    return reply ? { ...message, reply } : message
  }

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function stillCurrent(generation: number, owner: string | null): boolean {
    return generation === sessionGeneration && owner !== null && currentUserId() === owner
  }

  function reset(): void {
    sessionGeneration += 1
    listSeq += 1
    detailSeq += 1
    knownReplies.clear()
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
    detail.value = { id: null, status: 'idle', message: null, errorText: '' }
    actions.value = {}
    notices.value = []
  }

  /** Replaces a known message in the list and the open detail with a newer server copy. */
  function absorb(received: Message): void {
    const message = withKnownReply(received)
    items.value = items.value.map((m) => (m.id === message.id ? message : m))
    if (detail.value.id === message.id) detail.value = { ...detail.value, status: 'ready', message, errorText: '' }
  }

  async function load(): Promise<void> {
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    status.value = 'loading'
    errorText.value = ''
    loadingMore.value = false
    moreErrorText.value = ''
    try {
      const response = await listMessages()
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      items.value = response.messages.map(withKnownReply)
      nextCursor.value = response.next_cursor
      status.value = 'ready'
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      items.value = []
      nextCursor.value = null
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  async function loadMore(): Promise<void> {
    if (status.value !== 'ready' || !nextCursor.value || loadingMore.value) return
    const seq = ++listSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    loadingMore.value = true
    moreErrorText.value = ''
    try {
      const response = await listMessages(nextCursor.value)
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      const seen = new Set(items.value.map((m) => m.id))
      for (const message of response.messages) if (!seen.has(message.id)) items.value.push(withKnownReply(message))
      nextCursor.value = response.next_cursor
      loadingMore.value = false
    } catch (error) {
      if (seq !== listSeq || !stillCurrent(generation, owner)) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  async function openDetail(id: string): Promise<void> {
    const seq = ++detailSeq
    const generation = sessionGeneration
    const owner = currentUserId()
    const known = items.value.find((m) => m.id === id) ?? null
    detail.value = { id, status: 'loading', message: known, errorText: '' }
    try {
      const response = await getMessage(id)
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'ready', message: withKnownReply(response.message), errorText: '' }
    } catch (error) {
      if (seq !== detailSeq || !stillCurrent(generation, owner)) return
      detail.value = { id, status: 'error', message: null, errorText: describeError(error) }
    }
  }

  function update(id: string, patch: Partial<MessageAction>): void {
    const current = actions.value[id]
    if (current) actions.value = { ...actions.value, [id]: { ...current, ...patch } }
  }

  function end(id: string): void {
    const next = { ...actions.value }
    delete next[id]
    actions.value = next
  }

  function notify(notice: MessageNotice): void {
    notices.value = [notice, ...notices.value.filter((n) => n.id !== notice.id)]
  }

  function dismissNotice(id: string): void {
    notices.value = notices.value.filter((n) => n.id !== id)
  }

  async function send(id: string): Promise<MessageResult> {
    const action = actions.value[id]
    if (!action) return { outcome: 'discarded' }
    if (action.status === 'submitting') return { outcome: 'busy' }
    const session = useSessionStore()
    const owner = currentUserId()
    const generation = sessionGeneration
    if (!owner) return { outcome: 'discarded' }
    update(id, { status: 'submitting', errorText: '' })
    try {
      await session.ensureCsrfToken()
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (action.unresolved) {
        update(id, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的发送结果仍未确认，请保留原请求重试。` })
        return { outcome: 'unknown' }
      }
      update(id, { status: 'not_sent', errorText: describeError(error) })
      return { outcome: 'not_sent' }
    }
    let response: MessageMutationResponse
    try {
      // The captured snapshot is resent unchanged; the choice is never re-read from preferences.
      const received = action.kind === 'create'
        ? (action.notify === undefined ? await createMessage(action.body, action.key)
          : await createMessage(action.body, action.key, action.notify))
        : (action.notify === undefined ? await replyMessage(action.messageId ?? '', action.body, action.key)
          : await replyMessage(action.messageId ?? '', action.body, action.key, action.notify))
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (!isUsableMutation(received, action, owner)) throw new Error('留言响应无法确认。')
      response = received
    } catch (error) {
      if (!stillCurrent(generation, owner)) return { outcome: 'discarded' }
      if (error instanceof OfflineMutationBlockedError) {
        if (action.unresolved) {
          update(id, { status: 'unknown', errorText: `本次重试未发出（${describeError(error)}）。之前的发送结果仍未确认，请保留原请求重试。` })
          return { outcome: 'unknown' }
        }
        update(id, { status: 'not_sent', errorText: describeError(error) })
        return { outcome: 'not_sent' }
      }
      if (!isUnconfirmedOrUnsent(error)) {
        if (action.unresolved) {
          // A retry error alone does not prove what happened to an earlier request.
          update(id, { status: 'unknown',
            errorText: `本次重试未确认原结果（${describeError(error)}）。之前的发送结果仍未确认，恢复后可用同一请求重试。` })
          return { outcome: 'unknown' }
        }
        end(id)
        notify({ id, ok: false, text: describeError(error) })
        // A definite rejection (for example already replied) may mean the list is stale.
        if (action.kind === 'reply' && action.messageId) void refreshOne(action.messageId)
        return { outcome: 'rejected', errorCode: error instanceof ApiError ? error.code : null }
      }
      update(id, { status: 'unknown', unresolved: true, errorText: '结果未确认。可以重试，重试不会重复发出。' })
      return { outcome: 'unknown' }
    }
    end(id)
    notify({ id, ok: true, text: DONE_TEXT[action.kind] })
    // Remembered defaults for FUTURE forms only.
    if (action.notify !== undefined) void useNotificationChoicesStore().refresh()
    if (action.kind === 'create') {
      void load()
    } else {
      absorb(response.message)
    }
    return { outcome: 'done', response }
  }

  /** Re-reads one message after a rejected reply (detail and list copies only). */
  async function refreshOne(id: string): Promise<void> {
    const generation = sessionGeneration
    const owner = currentUserId()
    try {
      const response = await getMessage(id)
      if (!stillCurrent(generation, owner)) return
      absorb(response.message)
    } catch {
      // The notice already reports the rejection; a failed refresh changes nothing.
    }
  }

  function register(action: Omit<MessageAction, 'key' | 'status' | 'errorText'>): Promise<MessageResult> {
    if (actions.value[action.id]) return Promise.resolve({ outcome: 'busy' })
    if (!currentUserId()) return Promise.resolve({ outcome: 'discarded' })
    // Registered synchronously (status not_sent) so a double click cannot start a second action.
    actions.value = { ...actions.value, [action.id]: { ...action, key: newIdempotencyKey(), status: 'not_sent', errorText: '' } }
    return send(action.id)
  }

  /** The body is normalised by the view; the server re-validates it. */
  function create(body: string, notifyPartner?: boolean): Promise<MessageResult> {
    return register({ id: 'new', kind: 'create', body, notify: notifyPartner })
  }

  function reply(messageId: string, body: string, notifyPartner?: boolean): Promise<MessageResult> {
    return register({ id: `reply:${messageId}`, kind: 'reply', messageId, body, notify: notifyPartner })
  }

  /** Resend an unknown or unsent action with its original key and body (user click only). */
  function retry(id: string): Promise<MessageResult> {
    return send(id)
  }

  return {
    items, status, errorText, nextCursor, loadingMore, moreErrorText, detail, actions, notices,
    reset, load, loadMore, openDetail, create, reply, retry, dismissNotice,
  }
})
