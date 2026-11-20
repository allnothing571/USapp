/**
 * P2-3b optional notifications (web): facts validation against the captured choice (legacy null
 * accepted, malformed kept unknown); remembered defaults per account with late/stale reads
 * discarded; the toggle takes a default once and never overrides a touched or locked toggle;
 * stores capture notify_partner with the payload and key and resend exactly that snapshot after
 * an unknown result, across later errors and changed preferences; inbox labels/targets.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/notifications', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/notifications')>()),
  getNotificationChoices: vi.fn(),
}))
vi.mock('@/api/messages', () => ({
  createMessage: vi.fn(), listMessages: vi.fn(), getMessage: vi.fn(), replyMessage: vi.fn(),
}))
vi.mock('@/api/anniversaries', () => ({
  createAnniversary: vi.fn(), listAnniversaries: vi.fn(), getAnniversary: vi.fn(), editAnniversary: vi.fn(),
  proposeAnniversaryEdit: vi.fn(), decideAnniversaryProposal: vi.fn(), getHomeAnniversary: vi.fn(),
  setHomeAnniversaryPin: vi.fn(),
}))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn(), listTasks: vi.fn(), createTask: vi.fn() }))

import { createAnniversary, editAnniversary } from '@/api/anniversaries'
import { ApiError, setCsrfToken } from '@/api/client'
import { createMessage } from '@/api/messages'
import {
  getNotificationChoices,
  isValidNotificationFacts,
  notificationLabel,
  notificationTarget,
  type Notification,
  type NotificationChoices,
} from '@/api/notifications'
import { createTask, newIdempotencyKey } from '@/api/tasks'
import type { Anniversary, AnniversaryMutationResponse, Message, MessageMutationResponse } from '@/api/types'
import NotifyPartnerToggle from '@/components/NotifyPartnerToggle.vue'
import { useAnniversariesStore } from '@/stores/anniversaries'
import { useMessagesStore } from '@/stores/messages'
import { useNotificationChoicesStore } from '@/stores/notification-choices'
import { useSessionStore } from '@/stores/session'
import { useTasksStore } from '@/stores/tasks'

const DELIVERED = { requested: true, created: true, reason: 'delivered_to_inbox' as const, notification_id: 'n1' }
const NOT_REQUESTED = { requested: false, created: false, reason: 'not_requested' as const, notification_id: null }
const NO_PARTNER = { requested: true, created: false, reason: 'no_partner' as const, notification_id: null }

function choices(overrides: Partial<NotificationChoices['choices']> = {}): NotificationChoices {
  const off = { enabled: false, seq: null }
  return { choices: { task_publish: off, anniversary_create: off, anniversary_edit: off, message_create: off,
    message_reply: off, ...overrides } }
}
function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-30T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-30T00:00:00Z', members: [] },
  })
  setCsrfToken(`csrf-${id}`)
}
function deferred<T>() {
  let resolve!: (value: T) => void
  const promise = new Promise<T>((yes) => { resolve = yes })
  return { promise, resolve }
}
function sent(body: string, notification: unknown): MessageMutationResponse {
  const m: Message = { id: 'm1', author_id: 'self', recipient_id: 'partner', body, created_at: '2026-10-30T01:00:00.000000Z',
    reply: null }
  return { mutation: { action: 'create', message_id: 'm1', author_id: 'self', recipient_id: 'partner', body,
    created_at: m.created_at }, message: m, notification } as MessageMutationResponse
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  let counter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  vi.mocked(getNotificationChoices).mockResolvedValue(choices())
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
})

describe('notification facts validation', () => {
  it('accepts legacy null and consistent facts; rejects missing, mismatched or inconsistent facts', () => {
    // A captured true has its own fingerprint and can never replay a pre-intent legacy write.
    expect(isValidNotificationFacts(null, true)).toBe(false)
    expect(isValidNotificationFacts(null, true, true)).toBe(false)
    expect(isValidNotificationFacts(null, false)).toBe(true)
    expect(isValidNotificationFacts(null, undefined)).toBe(true)
    expect(isValidNotificationFacts(DELIVERED, true)).toBe(true)
    // no_partner exists only for partnerless anniversary writes.
    expect(isValidNotificationFacts(NO_PARTNER, true)).toBe(false)
    expect(isValidNotificationFacts(NO_PARTNER, true, true)).toBe(true)
    expect(isValidNotificationFacts(NOT_REQUESTED, false)).toBe(true)
    expect(isValidNotificationFacts(NOT_REQUESTED, undefined)).toBe(true)
    expect(isValidNotificationFacts(undefined, undefined)).toBe(true)
    expect(isValidNotificationFacts(undefined, false)).toBe(false)  // a new server always sends the field
    expect(isValidNotificationFacts(DELIVERED, false)).toBe(false)  // not this request's choice
    expect(isValidNotificationFacts(NOT_REQUESTED, true)).toBe(false)
    expect(isValidNotificationFacts({ ...DELIVERED, notification_id: null }, true)).toBe(false)
    expect(isValidNotificationFacts({ ...NO_PARTNER, created: true }, true)).toBe(false)
    expect(isValidNotificationFacts({ ...DELIVERED, reason: 'pushed' }, true)).toBe(false)
    expect(isValidNotificationFacts('yes', true)).toBe(false)
  })

  it('labels and targets optional events without any message or note text', () => {
    const item = (event_type: string, target_kind: Notification['target_kind']): Notification =>
      ({ id: 'n', event_type, actor_id: 'p', target_kind, target_id: 'x1', created_at: '', read_at: null })
    expect(notificationLabel(item('message.created', 'message'))).toBe('对方给你写了一张留言')
    expect(notificationTarget(item('message.replied', 'message'))).toBe('/us/messages/x1')
    expect(notificationLabel(item('anniversary.edited', 'anniversary'))).toBe('对方修改了纪念日')
    expect(notificationTarget(item('task.published', 'task'))).toBe('/tasks')
  })
})

describe('remembered choices', () => {
  it('belong to the current account; late reads after an account switch are discarded', async () => {
    const store = useNotificationChoicesStore()
    const late = deferred<NotificationChoices>()
    vi.mocked(getNotificationChoices).mockReturnValueOnce(late.promise)
    const loading = store.load()
    signIn('other')
    late.resolve(choices({ message_create: { enabled: true, seq: 3 } }))
    await loading
    expect(store.readyFor()).toBe(false)
    expect(store.enabledFor('message_create')).toBe(false)
    vi.mocked(getNotificationChoices).mockResolvedValueOnce(choices({ message_create: { enabled: true, seq: 1 } }))
    await store.load()
    expect(store.enabledFor('message_create')).toBe(true)
  })

  it('coalesces concurrent reads and treats malformed or failed reads as off', async () => {
    const store = useNotificationChoicesStore()
    const gate = deferred<NotificationChoices>()
    vi.mocked(getNotificationChoices).mockReturnValueOnce(gate.promise)
    const a = store.load()
    const b = store.load()
    gate.resolve({ choices: { task_publish: { enabled: true, seq: null } } } as unknown as NotificationChoices)
    await Promise.all([a, b])
    expect(getNotificationChoices).toHaveBeenCalledTimes(1)
    expect(store.status).toBe('error')
    expect(store.enabledFor('task_publish')).toBe(false)
    vi.mocked(getNotificationChoices).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    await store.load()
    expect(store.enabledFor('task_publish')).toBe(false)
  })
})

describe('preference freshness', () => {
  it('a confirmed write supersedes an older in-flight read; the old response cannot apply', async () => {
    const store = useNotificationChoicesStore()
    const old = deferred<NotificationChoices>()
    const fresh = deferred<NotificationChoices>()
    vi.mocked(getNotificationChoices).mockReturnValueOnce(old.promise).mockReturnValueOnce(fresh.promise)
    const oldRead = store.load()
    // A new explicit choice (off) commits while the old read is still in flight.
    vi.mocked(createMessage).mockResolvedValueOnce(sent('新选择', NOT_REQUESTED))
    expect((await useMessagesStore().create('新选择', false)).outcome).toBe('done')
    expect(getNotificationChoices).toHaveBeenCalledTimes(2)  // a fresh read, not the old promise
    old.resolve(choices({ message_create: { enabled: true, seq: 1 } }))  // stale: before the write
    await oldRead
    expect(store.readyFor()).toBe(false)
    expect(store.enabledFor('message_create')).toBe(false)
    fresh.resolve(choices({ message_create: { enabled: false, seq: 2 } }))
    await flushPromises()
    expect(store.enabledFor('message_create')).toBe(false)
    expect(store.choices?.message_create.seq).toBe(2)
  })

  it('a new form reads current defaults, including a change made on another device', async () => {
    const store = useNotificationChoicesStore()
    vi.mocked(getNotificationChoices).mockResolvedValueOnce(choices())
    await store.load()
    expect(store.enabledFor('message_create')).toBe(false)
    // Another device remembered "on" meanwhile; a new form must not use the cached "off".
    vi.mocked(getNotificationChoices).mockResolvedValueOnce(choices({ message_create: { enabled: true, seq: 4 } }))
    let value = false
    wrapper = mount(NotifyPartnerToggle, {
      props: { modelValue: false, operation: 'message_create', name: 'f',
        'onUpdate:modelValue': (v: boolean) => { value = v } },
    })
    expect(value).toBe(false)  // nothing applied from the cached read
    await flushPromises()
    expect(getNotificationChoices).toHaveBeenCalledTimes(2)
    expect(value).toBe(true)
  })

  it('a form waiting for its read keeps off when that read fails, and a retry applies', async () => {
    vi.mocked(getNotificationChoices).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockResolvedValueOnce(choices({ message_create: { enabled: true, seq: 1 } }))
    let value = false
    wrapper = mount(NotifyPartnerToggle, {
      props: { modelValue: false, operation: 'message_create', name: 'r',
        'onUpdate:modelValue': (v: boolean) => { value = v } },
    })
    await flushPromises()
    expect(value).toBe(false)
    await wrapper.find('[data-test="r-notify-error"] button').trigger('click')
    await flushPromises()
    expect(value).toBe(true)
  })
})

describe('notify toggle', () => {
  function mountToggle(props: Record<string, unknown> = {}) {
    let value = false
    wrapper = mount(NotifyPartnerToggle, {
      props: { modelValue: false, operation: 'message_create', name: 't', ...props,
        'onUpdate:modelValue': (v: boolean) => { value = v; void wrapper?.setProps({ modelValue: v }) } },
    })
    return () => value
  }

  it('takes the remembered default once, when untouched', async () => {
    vi.mocked(getNotificationChoices).mockResolvedValue(choices({ message_create: { enabled: true, seq: 2 } }))
    const value = mountToggle()
    await flushPromises()
    expect(value()).toBe(true)
    expect((wrapper!.find('[data-test="t-notify-input"]').element as HTMLInputElement).checked).toBe(true)
  })

  it('a late default never overrides a toggle the user already touched', async () => {
    const gate = deferred<NotificationChoices>()
    vi.mocked(getNotificationChoices).mockReturnValueOnce(gate.promise)
    const value = mountToggle()
    await wrapper!.find('[data-test="t-notify-input"]').setValue(true)
    await wrapper!.find('[data-test="t-notify-input"]').setValue(false)
    gate.resolve(choices({ message_create: { enabled: true, seq: 5 } }))
    await flushPromises()
    expect(value()).toBe(false)
  })

  it('a locked toggle (active or unknown action) is not changed by a late default', async () => {
    const gate = deferred<NotificationChoices>()
    vi.mocked(getNotificationChoices).mockReturnValueOnce(gate.promise)
    const value = mountToggle({ locked: true })
    gate.resolve(choices({ message_create: { enabled: true, seq: 5 } }))
    await flushPromises()
    expect(value()).toBe(false)
    expect((wrapper!.find('[data-test="t-notify-input"]').element as HTMLInputElement).disabled).toBe(true)
  })

  it('a failed read shows off with a retry; without a partner the toggle is disabled and off', async () => {
    vi.mocked(getNotificationChoices).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    mountToggle()
    await flushPromises()
    expect(wrapper!.find('[data-test="t-notify-error"]').exists()).toBe(true)
    expect((wrapper!.find('[data-test="t-notify-input"]').element as HTMLInputElement).checked).toBe(false)
    wrapper!.unmount()
    vi.mocked(getNotificationChoices).mockResolvedValue(choices({ message_create: { enabled: true, seq: 1 } }))
    mountToggle({ unavailable: true, modelValue: true })
    await flushPromises()
    const input = wrapper!.find('[data-test="t-notify-input"]').element as HTMLInputElement
    expect(input.disabled).toBe(true)
    expect(input.checked).toBe(false)
  })
})

describe('captured choice on unknown results', () => {
  it('a message keeps its key, body and choice across later errors and changed preferences', async () => {
    const store = useMessagesStore()
    vi.mocked(createMessage)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(new ApiError('CSRF_TOKEN_INVALID', 'rejected', 403))
      .mockResolvedValueOnce(sent('加油', { ...DELIVERED, requested: false }))  // malformed for this request
      .mockResolvedValueOnce(sent('加油', DELIVERED))
    expect((await store.create('加油', true)).outcome).toBe('unknown')
    // Remembered defaults change meanwhile; the retry must still send the captured snapshot.
    vi.mocked(getNotificationChoices).mockResolvedValue(choices())
    await useNotificationChoicesStore().load()
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', key: 'key-0000000000000001', body: '加油', notify: true })
    expect((await store.retry('new')).outcome).toBe('done')
    for (const call of vi.mocked(createMessage).mock.calls) expect(call).toEqual(['加油', 'key-0000000000000001', true])
  })

  it.each([
    ['legacy null with true', null],
    ['no_partner for a message', NO_PARTNER],
  ])('%s stays unknown with its key and choice', async (_label, facts) => {
    const store = useMessagesStore()
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(sent('真', facts)).mockResolvedValueOnce(sent('真', DELIVERED))
    await store.create('真', true)
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ key: 'key-0000000000000001', body: '真', notify: true, unresolved: true })
    expect((await store.retry('new')).outcome).toBe('done')
  })

  it('task publication rejects no_partner and legacy null for true as unconfirmed', async () => {
    const store = useTasksStore()
    const payload = { title: '浇花', description: '', reward_flowers: 1, reward_sunshine: 0, notify_partner: true }
    vi.mocked(createTask)
      .mockResolvedValueOnce({ task: { id: 't1' }, notification: NO_PARTNER } as never)
      .mockResolvedValueOnce({ task: { id: 't1' }, notification: null } as never)
      .mockResolvedValueOnce({ task: { id: 't1' }, notification: DELIVERED } as never)
    expect((await store.publish(payload)).outcome).toBe('unknown')
    expect((await store.retryPublish()).outcome).toBe('unknown')
    expect((await store.retryPublish()).outcome).toBe('published')
    for (const call of vi.mocked(createTask).mock.calls) expect(call).toEqual([payload, 'key-0000000000000001'])
  })

  it('a valid legacy replay with notification null resolves the business action', async () => {
    const store = useMessagesStore()
    vi.mocked(createMessage).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(sent('旧', null))
    await store.create('旧', false)
    expect((await store.retry('new')).outcome).toBe('done')
  })

  it('anniversary legacy null for false resolves; null for true stays unknown', async () => {
    const store = useAnniversariesStore()
    const terms = { title: '旧', date: '2020-05-20', kind: 'anniversary' as const, note: '' }
    const response = (notification: unknown): AnniversaryMutationResponse => ({
      mutation: { action: 'create', anniversary_id: 'a9', version: 1, actor_id: 'self', ...terms },
      as_of_date: '2026-10-30', notification,
      anniversary: { id: 'a9', creator_id: 'self', version: 1, ...terms, created_at: '', updated_at: '',
        pinned_by_me: false, computed: { days_since: 1, next: null, days_until: null }, pending_proposal: null,
        last_decision: null },
    } as AnniversaryMutationResponse)
    vi.mocked(createAnniversary).mockResolvedValueOnce(response(null))
    expect((await store.create(terms, false)).outcome).toBe('done')
    vi.mocked(createAnniversary).mockResolvedValueOnce(response(null))
    expect((await store.create(terms, true)).outcome).toBe('unknown')
  })

  it('anniversary create and edit capture the choice; a missing facts field stays unknown', async () => {
    const store = useAnniversariesStore()
    const terms = { title: '在一起', date: '2020-05-20', kind: 'anniversary' as const, note: '' }
    const item = (version: number): Anniversary => ({ id: 'a1', creator_id: 'self', version, ...terms, created_at: '',
      updated_at: '', pinned_by_me: false, computed: { days_since: 1, next: null, days_until: null },
      pending_proposal: null, last_decision: null })
    const revision = (version: number, notification: unknown): AnniversaryMutationResponse => ({
      mutation: { action: version === 1 ? 'create' : 'edit', anniversary_id: 'a1', version, actor_id: 'self', ...terms },
      as_of_date: '2026-10-30', anniversary: item(version), notification } as AnniversaryMutationResponse)
    vi.mocked(createAnniversary).mockResolvedValueOnce(revision(1, undefined)).mockResolvedValueOnce(revision(1, NO_PARTNER))
    expect((await store.create(terms, true)).outcome).toBe('unknown')
    expect((await store.retry('new')).outcome).toBe('done')
    expect(vi.mocked(createAnniversary).mock.calls).toEqual([[terms, 'key-0000000000000001', true],
      [terms, 'key-0000000000000001', true]])
    vi.mocked(editAnniversary).mockResolvedValueOnce(revision(2, NOT_REQUESTED))
    expect((await store.edit('a1', 1, terms, false)).outcome).toBe('done')
    expect(vi.mocked(editAnniversary).mock.calls[0]).toEqual(['a1', 1, terms, 'key-0000000000000002', false])
  })

  it('a task publication keeps notify_partner inside the captured body for its retry', async () => {
    const store = useTasksStore()
    const payload = { title: '浇花', description: '', reward_flowers: 1, reward_sunshine: 0, notify_partner: true }
    const task = { id: 't1' }
    vi.mocked(createTask)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce({ task, notification: NOT_REQUESTED } as never)
      .mockResolvedValueOnce({ task, notification: DELIVERED } as never)
    expect((await store.publish(payload)).outcome).toBe('unknown')
    // A different draft (toggle off) cannot replace the unresolved publication.
    expect((await store.publish({ ...payload, notify_partner: false })).outcome).toBe('unknown')
    expect((await store.retryPublish()).outcome).toBe('unknown')  // facts do not match the captured choice
    expect((await store.retryPublish()).outcome).toBe('published')
    for (const call of vi.mocked(createTask).mock.calls) expect(call).toEqual([payload, 'key-0000000000000001'])
  })
})
