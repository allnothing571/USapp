/**
 * P2-1 message board (web): compose and reply keep their key while the result is unknown and resend
 * the same key and body only on an explicit retry; definite rejections end the action; pages load
 * more without duplicates; the reply form is only for the unreplied recipient; Home shows the
 * partner's latest message (or a non-zero error state); late results after an account switch or
 * logout change nothing for the new account.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/messages', () => ({
  createMessage: vi.fn(), listMessages: vi.fn(), getMessage: vi.fn(), replyMessage: vi.fn(),
}))
vi.mock('@/api/anniversaries', () => ({
  createAnniversary: vi.fn(), listAnniversaries: vi.fn(), getAnniversary: vi.fn(), editAnniversary: vi.fn(),
  proposeAnniversaryEdit: vi.fn(), decideAnniversaryProposal: vi.fn(), getHomeAnniversary: vi.fn(),
  setHomeAnniversaryPin: vi.fn(),
}))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn(), listTasks: vi.fn() }))
vi.mock('@/api/rewards', () => ({ listRedemptions: vi.fn() }))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))

import { getHomeAnniversary, listAnniversaries } from '@/api/anniversaries'
import { ApiError, setCsrfToken } from '@/api/client'
import * as authApi from '@/api/auth'
import { createMessage, getMessage, listMessages, replyMessage } from '@/api/messages'
import { checkMessageBody, checkReplyBody, messagePreview } from '@/api/message-rules'
import { listRedemptions } from '@/api/rewards'
import { listTasks, newIdempotencyKey } from '@/api/tasks'
import type { Message, MessageListResponse, MessageMutationResponse } from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useHomeStore } from '@/stores/home'
import { useMessagesStore } from '@/stores/messages'
import { useSessionStore } from '@/stores/session'
import HomeView from '@/views/HomeView.vue'
import MessageDetailView from '@/views/MessageDetailView.vue'
import UsView from '@/views/UsView.vue'

function msg(id: string, overrides: Partial<Message> = {}): Message {
  return {
    id, author_id: 'partner', recipient_id: 'self', body: `留言 ${id}`, created_at: '2026-10-27T02:00:00.000000Z',
    reply: null, ...overrides,
  }
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

const stubs = { RouterLink: { props: ['to'], template: '<a :data-to="JSON.stringify(to)"><slot /></a>' } }

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
  vi.mocked(listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-29', anniversaries: [], next_cursor: null })
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 1, sunshine: 2 } })
  let counter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
})

describe('text rules', () => {
  it('mirrors the server: message 1-500 with line breaks, reply one line of 1-100', () => {
    expect(checkMessageBody('  加油\r\n不许紧张 \n')).toEqual({ ok: true, value: '加油\n不许紧张' })
    expect(checkMessageBody('😀'.repeat(500)).ok).toBe(true)
    expect(checkMessageBody('😀'.repeat(501))).toEqual({ ok: false, message: '留言最多 500 个字符' })
    expect(checkMessageBody('  \n ')).toEqual({ ok: false, message: '留言不能为空' })
    expect(checkReplyBody('好'.repeat(100)).ok).toBe(true)
    expect(checkReplyBody('好'.repeat(101))).toEqual({ ok: false, message: '回复最多 100 个字符' })
    expect(checkReplyBody('a\nb')).toEqual({ ok: false, message: '回复只能是一行' })
    expect(messagePreview('第一行\n第二行')).toBe('第一行')
    expect(messagePreview('长'.repeat(61))).toBe(`${'长'.repeat(60)}…`)
  })
})

describe('store actions', () => {
  it('an unknown create keeps its key; retry resends the same key and body; success reloads the list', async () => {
    const store = useMessagesStore()
    const m = msg('m1', { author_id: 'self', recipient_id: 'partner', body: '加油' })
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(created(m))
    expect((await store.create('加油')).outcome).toBe('unknown')
    expect(store.actions.new.status).toBe('unknown')
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [m], next_cursor: null })
    expect((await store.retry('new')).outcome).toBe('done')
    await flushPromises()
    const calls = vi.mocked(createMessage).mock.calls
    expect(calls[1]).toEqual(calls[0])
    expect(calls[0]).toEqual(['加油', 'key-0000000000000001'])
    expect(store.actions.new).toBeUndefined()
    expect(store.items.map((x) => x.id)).toEqual(['m1'])
    expect(store.notices[0]).toMatchObject({ id: 'new', ok: true })
  })

  it('a definite reply rejection ends the action and refreshes that message', async () => {
    const store = useMessagesStore()
    const m = msg('m2')
    vi.mocked(replyMessage).mockRejectedValueOnce(new ApiError('MESSAGE_ALREADY_REPLIED', 'x', 409))
    vi.mocked(getMessage).mockResolvedValueOnce({ message: { ...m, reply: { author_id: 'self', body: '早就回了', created_at: '2026-10-27T03:00:00Z' } } })
    await store.openDetail('m2')
    vi.mocked(getMessage).mockResolvedValueOnce({ message: { ...m, reply: { author_id: 'self', body: '早就回了', created_at: '2026-10-27T03:00:00Z' } } })
    expect(await store.reply('m2', '再回一次')).toEqual({ outcome: 'rejected', errorCode: 'MESSAGE_ALREADY_REPLIED' })
    await flushPromises()
    expect(store.actions['reply:m2']).toBeUndefined()
    expect(store.notices[0]).toMatchObject({ id: 'reply:m2', ok: false })
    expect(store.detail.message?.reply?.body).toBe('早就回了')
  })

  it('load more appends without duplicates; a newer first-page load wins over an in-flight page', async () => {
    const store = useMessagesStore()
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [msg('a'), msg('b')], next_cursor: 'c1' })
    await store.load()
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [msg('b'), msg('c')], next_cursor: null })
    await store.loadMore()
    expect(vi.mocked(listMessages).mock.calls[1][0]).toBe('c1')
    expect(store.items.map((m) => m.id)).toEqual(['a', 'b', 'c'])
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [msg('a')], next_cursor: 'c2' })
    await store.load()
    const more = deferred<MessageListResponse>()
    vi.mocked(listMessages).mockReturnValueOnce(more.promise).mockResolvedValueOnce({ messages: [msg('z')], next_cursor: null })
    const pending = store.loadMore()
    await store.load()
    more.resolve({ messages: [msg('old')], next_cursor: 'c3' })
    await pending
    expect(store.items.map((m) => m.id)).toEqual(['z'])
  })

  it.each(['account switch', 'logout'])('late create success and reply failure are discarded after %s', async (boundary) => {
    const store = useMessagesStore()
    const create = deferred<MessageMutationResponse>()
    const reply = deferred<MessageMutationResponse>()
    vi.mocked(createMessage).mockReturnValueOnce(create.promise)
    vi.mocked(replyMessage).mockReturnValueOnce(reply.promise)
    const a = store.create('旧账号的留言')
    const b = store.reply('m9', '旧账号的回复')
    await flushPromises()
    if (boundary === 'logout') {
      vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
      await useSessionStore().logout()
    } else {
      signIn('other')
    }
    create.resolve(created(msg('old', { author_id: 'self' })))
    reply.reject(new ApiError('MESSAGE_ALREADY_REPLIED', 'x', 409))
    expect((await a).outcome).toBe('discarded')
    expect((await b).outcome).toBe('discarded')
    expect(store.actions).toEqual({})
    expect(store.notices).toEqual([])
    expect(store.items).toEqual([])
  })
})

describe('Us page', () => {
  it('validates locally, keeps the draft while unknown, and clears it after a confirmed send', async () => {
    vi.mocked(listMessages).mockResolvedValue({ messages: [
      msg('in', { body: '给我的' }),
      msg('out', { author_id: 'self', recipient_id: 'partner', body: '我写的' }),
      msg('done', { reply: { author_id: 'self', body: '回好了', created_at: '2026-10-27T03:00:00Z' } }),
    ], next_cursor: null })
    wrapper = mount(UsView, { global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="message-in"]').text()).toContain('待我回复')
    expect(wrapper.find('[data-test="message-out"]').text()).toContain('等待对方回复')
    expect(wrapper.find('[data-test="message-done"]').text()).toContain('已回复')
    expect(wrapper.find('[data-test="message-done"] [data-test="message-reply"]').text()).toContain('回好了')

    await wrapper.find('[data-test="compose"]').trigger('submit')
    expect(wrapper.find('[data-test="compose-error"]').text()).toContain('不能为空')
    expect(createMessage).not.toHaveBeenCalled()

    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    await wrapper.find('[data-test="compose-body"]').setValue('今晚早点睡')
    await wrapper.find('[data-test="compose"]').trigger('submit')
    await flushPromises()
    expect(wrapper.find('[data-test="compose-retry"]').exists()).toBe(true)
    expect((wrapper.find('[data-test="compose-body"]').element as HTMLTextAreaElement).value).toBe('今晚早点睡')

    vi.mocked(createMessage).mockResolvedValueOnce(created(msg('new', { author_id: 'self', recipient_id: 'partner', body: '今晚早点睡' })))
    await wrapper.find('[data-test="compose-retry"]').trigger('click')
    await flushPromises()
    expect(vi.mocked(createMessage).mock.calls[1]).toEqual(vi.mocked(createMessage).mock.calls[0])
    expect((wrapper.find('[data-test="compose-body"]').element as HTMLTextAreaElement).value).toBe('')
  })

  it('an account switch clears the draft and reloads for the new account', async () => {
    wrapper = mount(UsView, { global: { stubs } })
    await flushPromises()
    await wrapper.find('[data-test="compose-body"]').setValue('旧草稿')
    await wrapper.find('[data-test="compose"]').trigger('submit')
    signIn('other')
    await flushPromises()
    expect((wrapper.find('[data-test="compose-body"]').element as HTMLTextAreaElement).value).toBe('')
    expect(wrapper.find('[data-test="compose-error"]').exists()).toBe(false)
    expect(listMessages).toHaveBeenCalledTimes(2)
  })
})

describe('message detail', () => {
  it('quick emoji selects a draft; only explicit submit uses the one-reply endpoint', async () => {
    const m = msg('quick')
    vi.mocked(getMessage).mockResolvedValueOnce({ message: m })
    wrapper = mount(MessageDetailView, { props: { id: 'quick' }, global: { stubs } })
    await flushPromises()
    await wrapper.get('[data-test="quick-reply-clap"]').trigger('click')
    expect(replyMessage).not.toHaveBeenCalled()
    expect((wrapper.get('[data-test="reply-body"]').element as HTMLInputElement).value).toBe('👏')
    vi.mocked(replyMessage).mockResolvedValueOnce(replied(m, '👏'))
    await wrapper.get('[data-test="reply-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(replyMessage).mock.calls[0].slice(0, 2)).toEqual(['quick', '👏'])
    expect(wrapper.find('[data-test="reply-form"]').exists()).toBe(false)
  })

  it('only the unreplied recipient gets a reply form; a confirmed reply shows in place', async () => {
    const m = msg('m1', { body: '晚上一起散步吗' })
    vi.mocked(getMessage).mockResolvedValueOnce({ message: m })
    wrapper = mount(MessageDetailView, { props: { id: 'm1' }, global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="detail-body"]').text()).toBe('晚上一起散步吗')
    await wrapper.find('[data-test="reply-body"]').setValue('好'.repeat(101))
    await wrapper.find('[data-test="reply-form"]').trigger('submit')
    expect(wrapper.find('[data-test="reply-error"]').text()).toContain('最多 100')
    expect(replyMessage).not.toHaveBeenCalled()
    vi.mocked(replyMessage).mockResolvedValueOnce(replied(m, '好呀'))
    await wrapper.find('[data-test="reply-body"]').setValue('好呀')
    await wrapper.find('[data-test="reply-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(replyMessage).mock.calls[0].slice(0, 2)).toEqual(['m1', '好呀'])
    expect(wrapper.find('[data-test="detail-reply"]').text()).toContain('好呀')
    expect(wrapper.find('[data-test="reply-form"]').exists()).toBe(false)
  })

  it('the author sees no reply form', async () => {
    vi.mocked(getMessage).mockResolvedValueOnce({ message: msg('m2', { author_id: 'self', recipient_id: 'partner' }) })
    wrapper = mount(MessageDetailView, { props: { id: 'm2' }, global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="reply-form"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="detail-waiting"]').exists()).toBe(true)
  })
})

describe('Home latest message', () => {
  it('shows the partner latest message linking to its detail, or an explicit empty state', async () => {
    vi.mocked(listMessages).mockResolvedValueOnce({ messages: [msg('p1', { body: '今天面试加油\n不许紧张' })], next_cursor: null })
    wrapper = mount(HomeView, { global: { stubs } })
    await flushPromises()
    expect(listMessages).toHaveBeenCalledWith(null, 1, 'partner')
    expect(wrapper.find('[data-test="latest-message-preview"]').text()).toBe('今天面试加油')
    expect(wrapper.find('[data-test="latest-message-link"]').attributes('data-to')).toContain('"id":"p1"')
    expect(wrapper.find('[data-test="home-latest-message"]').text()).toContain('待我回复')
    wrapper.unmount()
    wrapper = mount(HomeView, { global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="latest-message-none"]').exists()).toBe(true)
  })

  it('an error is shown with a retry, never as "no messages"', async () => {
    vi.mocked(listMessages).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    wrapper = mount(HomeView, { global: { stubs } })
    await flushPromises()
    const card = wrapper.find('[data-test="home-latest-message"]')
    expect(card.find('[role="alert"]').exists()).toBe(true)
    expect(card.find('[data-test="latest-message-none"]').exists()).toBe(false)
  })

  it('a late latest-message result for the old account is discarded', async () => {
    const home = useHomeStore()
    const late = deferred<MessageListResponse>()
    vi.mocked(listMessages).mockReturnValueOnce(late.promise)
    const pending = home.loadLatestMessage()
    signIn('other')
    late.resolve({ messages: [msg('old')], next_cursor: null })
    await pending
    expect(home.latestMessage).toMatchObject({ status: 'idle', message: null })
  })
})
