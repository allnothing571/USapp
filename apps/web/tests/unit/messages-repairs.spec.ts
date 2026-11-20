/**
 * P2-1 review repairs:
 * 1. An unresolved send keeps its key, body and unknown state when a later retry is rejected before
 *    idempotency classification (session/CSRF/Origin/rate limit); only a confirmed answer resolves it.
 *    Unsent first attempts and definitive business rejections still end the action. An identity
 *    reset discards the in-memory action (documented limitation).
 * 2. The detail page clears its draft when the message changes, and a late result for message A
 *    never clears or alters message B's draft.
 * 3. A read started before a confirmed reply cannot hide that reply in the detail or the list.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/messages', () => ({
  createMessage: vi.fn(), listMessages: vi.fn(), getMessage: vi.fn(), replyMessage: vi.fn(),
}))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn() }))

import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { createMessage, getMessage, listMessages, replyMessage } from '@/api/messages'
import { newIdempotencyKey } from '@/api/tasks'
import type { Message, MessageListResponse, MessageMutationResponse, MessageResponse } from '@/api/types'
import { useMessagesStore } from '@/stores/messages'
import { useSessionStore } from '@/stores/session'
import MessageDetailView from '@/views/MessageDetailView.vue'

function msg(id: string, overrides: Partial<Message> = {}): Message {
  return { id, author_id: 'partner', recipient_id: 'self', body: `留言 ${id}`, created_at: '2026-10-27T02:00:00.000000Z',
    reply: null, ...overrides }
}
function created(m: Message): MessageMutationResponse {
  return { mutation: { action: 'create', message_id: m.id, author_id: m.author_id, recipient_id: m.recipient_id,
    body: m.body, created_at: m.created_at }, message: m,
    notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null } }
}
function replied(m: Message, body: string): MessageMutationResponse {
  const reply = { author_id: m.recipient_id, body, created_at: '2026-10-27T03:00:00.000000Z' }
  return { mutation: { action: 'reply', message_id: m.id, author_id: reply.author_id, body, created_at: reply.created_at },
    message: { ...m, reply }, notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null } }
}
function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-27T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-27T00:00:00Z', members: [] },
  })
  setCsrfToken(`csrf-${id}`)
}
function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

const stubs = { RouterLink: { template: '<a><slot /></a>' } }
let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
})

describe('unknown first attempt, rejected retry', () => {
  it.each([
    ['CSRF_TOKEN_INVALID', 403], ['UNAUTHENTICATED', 401], ['ORIGIN_NOT_ALLOWED', 403], ['RATE_LIMITED', 429],
  ])('a %s retry keeps the unresolved create with its key and body', async (code, status) => {
    const store = useMessagesStore()
    const m = msg('m1', { author_id: 'self', recipient_id: 'partner', body: '加油' })
    vi.mocked(createMessage)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(new ApiError(code, 'rejected', status))
      .mockResolvedValueOnce(created(m))
    expect((await store.create('加油')).outcome).toBe('unknown')
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', key: 'key-0000000000000001', body: '加油', unresolved: true })
    expect(store.actions.new.errorText).toContain('之前的发送结果仍未确认')
    expect(store.notices).toEqual([])
    // A later authorised retry resends the same key and body and resolves the original.
    expect((await store.retry('new')).outcome).toBe('done')
    const calls = vi.mocked(createMessage).mock.calls
    expect(calls).toHaveLength(3)
    for (const call of calls) expect(call).toEqual(['加油', 'key-0000000000000001'])
    expect(store.actions.new).toBeUndefined()
  })

  it('the same holds for a reply', async () => {
    const store = useMessagesStore()
    const m = msg('m2')
    vi.mocked(replyMessage)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockRejectedValueOnce(new ApiError('CSRF_TOKEN_INVALID', 'csrf', 403))
      .mockResolvedValueOnce(replied(m, '好呀'))
    expect((await store.reply('m2', '好呀')).outcome).toBe('unknown')
    expect((await store.retry('reply:m2')).outcome).toBe('unknown')
    expect(store.actions['reply:m2']).toMatchObject({ status: 'unknown', key: 'key-0000000000000001', body: '好呀' })
    expect((await store.retry('reply:m2')).outcome).toBe('done')
    for (const call of vi.mocked(replyMessage).mock.calls) expect(call).toEqual(['m2', '好呀', 'key-0000000000000001'])
  })

  it.each([
    new OfflineMutationBlockedError(),
    new ApiError('INTERNAL_ERROR', 'gateway response', 403),
    new ApiError('SPACE_NOT_FOUND', 'no space', 404),
  ])('keeps an earlier unknown create when a later retry fails: %s', async (failure) => {
    const store = useMessagesStore()
    const m = msg('m1', { author_id: 'self', recipient_id: 'partner', body: '加油' })
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(failure).mockResolvedValueOnce(created(m))
    await store.create('加油')
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', unresolved: true, body: '加油' })
    expect(store.actions.new.errorText).toContain('仍未确认')
    expect((await store.retry('new')).outcome).toBe('done')
    expect(vi.mocked(createMessage).mock.calls.map((call) => call[1])).toEqual([
      'key-0000000000000001', 'key-0000000000000001', 'key-0000000000000001',
    ])
  })

  it('keeps an earlier unknown create when CSRF preparation fails locally', async () => {
    const store = useMessagesStore()
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    await store.create('加油')
    const spy = vi.spyOn(useSessionStore(), 'ensureCsrfToken').mockRejectedValueOnce(new Error('token unavailable'))
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', unresolved: true, body: '加油' })
    expect(store.actions.new.errorText).toContain('仍未确认')
    expect(createMessage).toHaveBeenCalledTimes(1)
    spy.mockRestore()
  })

  it('does not confirm a reply when the success response omits the reply facts', async () => {
    const store = useMessagesStore()
    const m = msg('m2')
    const broken = replied(m, '好呀')
    vi.mocked(replyMessage).mockResolvedValueOnce({ ...broken, message: m })
      .mockResolvedValueOnce(broken)
    expect((await store.reply('m2', '好呀')).outcome).toBe('unknown')
    const key = store.actions['reply:m2'].key
    expect((await store.retry('reply:m2')).outcome).toBe('done')
    expect(vi.mocked(replyMessage).mock.calls.map((call) => call[2])).toEqual([key, key])
  })

  it('a business rejection after an unknown attempt keeps the original action', async () => {
    const store = useMessagesStore()
    vi.mocked(replyMessage)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(new ApiError('MESSAGE_ALREADY_REPLIED', 'x', 409))
    vi.mocked(getMessage).mockResolvedValue({ message: msg('m3') })
    await store.reply('m3', '好')
    expect(await store.retry('reply:m3')).toEqual({ outcome: 'unknown' })
    expect(store.actions['reply:m3']).toMatchObject({ status: 'unknown', unresolved: true })
    expect(store.notices).toEqual([])
  })

  it('a first attempt that was never sent is not unresolved: a CSRF rejection on retry ends it', async () => {
    const store = useMessagesStore()
    vi.mocked(createMessage)
      .mockRejectedValueOnce(new OfflineMutationBlockedError())
      .mockRejectedValueOnce(new ApiError('CSRF_TOKEN_INVALID', 'csrf', 403))
    expect((await store.create('离线时写的')).outcome).toBe('not_sent')
    expect(store.actions.new.unresolved).toBeUndefined()
    expect(await store.retry('new')).toEqual({ outcome: 'rejected', errorCode: 'CSRF_TOKEN_INVALID' })
    expect(store.actions.new).toBeUndefined()
  })

  it('a definitive first-attempt rejection ends the action as before', async () => {
    const store = useMessagesStore()
    vi.mocked(replyMessage).mockRejectedValueOnce(new ApiError('MESSAGE_REPLY_FORBIDDEN', 'x', 403))
    vi.mocked(getMessage).mockResolvedValue({ message: msg('m4') })
    expect(await store.reply('m4', '好')).toEqual({ outcome: 'rejected', errorCode: 'MESSAGE_REPLY_FORBIDDEN' })
    expect(store.actions['reply:m4']).toBeUndefined()
  })

  it('logout or an account switch discards an unresolved action (in memory only, never shown to another account)', async () => {
    const store = useMessagesStore()
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
    await store.create('未确认')
    expect(store.actions.new.status).toBe('unknown')
    signIn('other')
    expect(store.actions).toEqual({})
    expect(store.notices).toEqual([])
  })
})

describe('reads never hide a confirmed reply', () => {
  it('a detail read started before the reply resolves afterwards without overwriting it', async () => {
    const store = useMessagesStore()
    const m = msg('m5')
    const read = deferred<MessageResponse>()
    vi.mocked(getMessage).mockReturnValueOnce(read.promise)
    const pending = store.openDetail('m5')
    vi.mocked(replyMessage).mockResolvedValueOnce(replied(m, '收到'))
    expect((await store.reply('m5', '收到')).outcome).toBe('done')
    expect(store.detail.message?.reply?.body).toBe('收到')
    read.resolve({ message: m })  // stale copy: no reply yet
    await pending
    expect(store.detail.message?.reply?.body).toBe('收到')
  })

  it('a list read started before the reply resolves afterwards without overwriting it', async () => {
    const store = useMessagesStore()
    const m = msg('m6')
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [m], next_cursor: null })
    await store.load()
    const read = deferred<MessageListResponse>()
    vi.mocked(listMessages).mockReturnValueOnce(read.promise)
    const pending = store.load()
    vi.mocked(replyMessage).mockResolvedValueOnce(replied(m, '收到'))
    await store.reply('m6', '收到')
    read.resolve({ messages: [m], next_cursor: null })
    await pending
    expect(store.items[0].reply?.body).toBe('收到')
  })
})

describe('detail page target changes', () => {
  it('moving from message A to B clears the draft and error', async () => {
    vi.mocked(getMessage).mockImplementation(async (id) => ({ message: msg(id) }))
    wrapper = mount(MessageDetailView, { props: { id: 'a' }, global: { stubs } })
    await flushPromises()
    await wrapper.find('[data-test="reply-body"]').setValue('好'.repeat(101))
    await wrapper.find('[data-test="reply-form"]').trigger('submit')
    expect(wrapper.find('[data-test="reply-error"]').exists()).toBe(true)
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    expect((wrapper.find('[data-test="reply-body"]').element as HTMLInputElement).value).toBe('')
    expect(wrapper.find('[data-test="reply-error"]').exists()).toBe(false)
  })

  it("a late result for A neither clears nor alters B's draft", async () => {
    vi.mocked(getMessage).mockImplementation(async (id) => ({ message: msg(id) }))
    const late = deferred<MessageMutationResponse>()
    vi.mocked(replyMessage).mockReturnValueOnce(late.promise)
    wrapper = mount(MessageDetailView, { props: { id: 'a' }, global: { stubs } })
    await flushPromises()
    await wrapper.find('[data-test="reply-body"]').setValue('给 A 的回复')
    await wrapper.find('[data-test="reply-form"]').trigger('submit')
    await wrapper.setProps({ id: 'b' })
    await flushPromises()
    await wrapper.find('[data-test="reply-body"]').setValue('给 B 的草稿')
    late.resolve(replied(msg('a'), '给 A 的回复'))
    await flushPromises()
    expect((wrapper.find('[data-test="reply-body"]').element as HTMLInputElement).value).toBe('给 B 的草稿')
    expect(wrapper.find('[data-test="message-notice-reply:a"]').exists()).toBe(false)
    expect(vi.mocked(replyMessage).mock.calls[0].slice(0, 2)).toEqual(['a', '给 A 的回复'])
  })
})
