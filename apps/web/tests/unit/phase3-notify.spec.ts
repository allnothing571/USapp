/**
 * D20 opt-in partner notices (contract 0.41.0, docs/phase3-plan.md Section 30): request bodies through the real API clients, the
 * shared-plan create page and store, and the generic labels and destinations the inbox and service worker share. Only the HTTP
 * `request` is faked; nothing here reaches a server.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/client', async (original) => ({
  ...(await original<typeof import('@/api/client')>()),
  request: vi.fn(),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(), createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
const routerPush = vi.fn()
vi.mock('vue-router', async (original) => ({
  ...(await original<typeof import('vue-router')>()),
  useRouter: () => ({ push: routerPush, currentRoute: { value: { name: 'shared-plans' } } }),
}))

import { addAlbumPhotos } from '@/api/albums'
import { request, setCsrfToken } from '@/api/client'
import { createMemory } from '@/api/memories'
import { eventLabel, targetPath } from '@/api/notification-labels'
import { notificationTarget, type Notification } from '@/api/notifications'
import { sendPlan, type PlanBody, type SharedPlan } from '@/api/shared-plans'
import { destinationFor, notificationFor, parsePushPayload, type PushPayload } from '@/pwa/push-sw'
import { useSessionStore } from '@/stores/session'
import { useSharedPlansStore } from '@/stores/shared-plans'
import SharedPlansView from '@/views/SharedPlansView.vue'

const SELF = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
const PLAN = '11111111-1111-4111-8111-111111111111'
const RECEIPT = '33333333-3333-4333-8333-333333333333'
const NID = '99999999-9999-4999-8999-999999999999'
const TARGET = '44444444-4444-4444-8444-444444444444'
const TIME = '2026-11-09T01:00:00Z'
const body: PlanBody = { title: '看海', date: null, time: null, place: null, note: null }
const terms = { date: '2026-03-01', title: '海边', description: '', media_ids: [TARGET] }
const delivered = { requested: true, created: true, reason: 'delivered_to_inbox', notification_id: NID }
const quiet = { requested: false, created: false, reason: 'not_requested', notification_id: null }

function plan(): SharedPlan {
  return { id: PLAN, creator_id: SELF, created_at: TIME, body, body_version: 1, status: 'want', status_version: 1,
    pending_proposal: null, latest_decision: null, memory_id: null }
}
function createdPlan(notification: unknown) {
  return { replayed: false, plan: plan(), notification,
    mutation: { id: RECEIPT, operation: 'create', body_version: 1, status: 'want', status_version: 1, proposal_id: null, outcome: null } }
}
function signIn(members = 2): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: 'csrf',
    user: { id: SELF, email: 'self@example.com', display_name: 'self', created_at: TIME },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: members, created_at: TIME, members: [] },
  })
  setCsrfToken('csrf')
}
/** Bodies of POSTs to the plan collection, in order. */
function planPosts(): unknown[] {
  return vi.mocked(request).mock.calls.filter(([url, init]) => url === '/api/v1/shared-plans' && (init as { method?: string })?.method === 'POST')
    .map(([, init]) => (init as { body?: unknown }).body)
}
function planKeys(): string[] {
  return vi.mocked(request).mock.calls.filter(([url, init]) => url === '/api/v1/shared-plans' && (init as { method?: string })?.method === 'POST')
    .map(([, init]) => (init as { headers: Record<string, string> }).headers['Idempotency-Key'])
}
function answerPlans(post: () => Promise<unknown>): void {
  vi.mocked(request).mockImplementation(((url: string, init?: { method?: string }) =>
    url === '/api/v1/shared-plans' && init?.method === 'POST' ? post() : Promise.resolve({ plans: [], next_cursor: null })) as never)
}

const mounted: VueWrapper[] = []
function page(): VueWrapper {
  const wrapper = mount(SharedPlansView, { global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } }) as VueWrapper
  mounted.push(wrapper)
  return wrapper
}
const box = (wrapper: VueWrapper) => wrapper.get('[data-test="plan-create-notify-input"]').element as HTMLInputElement
async function fillAndSave(wrapper: VueWrapper): Promise<void> {
  await wrapper.get('[data-test="plan-title"]').setValue('看海')
  await wrapper.get('form').trigger('submit')
  await flushPromises()
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn()
})
afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
})

describe('D20 request bodies', () => {
  it('memory create sends notify_partner only when a choice was captured', async () => {
    vi.mocked(request).mockResolvedValue({})
    await createMemory(terms, 'k1')
    await createMemory(terms, 'k2', false)
    await createMemory(terms, 'k3', true)
    const bodies = vi.mocked(request).mock.calls.map(([, init]) => (init as { body: Record<string, unknown> }).body)
    expect(bodies[0]).toEqual(terms)
    expect(bodies[1]).toEqual({ ...terms, notify_partner: false })
    expect(bodies[2]).toEqual({ ...terms, notify_partner: true })
  })

  it('an album batch sends one notify_partner for the whole batch, never per photo', async () => {
    vi.mocked(request).mockResolvedValue({})
    await addAlbumPhotos('album-1', [TARGET, NID, RECEIPT], 'k1', true)
    await addAlbumPhotos('album-1', [TARGET], 'k2')
    const bodies = vi.mocked(request).mock.calls.map(([, init]) => (init as { body: Record<string, unknown> }).body)
    expect(bodies[0]).toEqual({ media_ids: [TARGET, NID, RECEIPT], notify_partner: true })
    expect(bodies[1]).toEqual({ media_ids: [TARGET] })
  })

  it('only plan creation can carry the field; edits, requests and conversion never do', async () => {
    vi.mocked(request).mockResolvedValue({})
    await sendPlan({ kind: 'create', body, notify: true }, 'k1')
    await sendPlan({ kind: 'create', body }, 'k2')
    await sendPlan({ kind: 'edit', id: PLAN, version: 1, body }, 'k3')
    await sendPlan({ kind: 'propose', id: PLAN, version: 1, body }, 'k4')
    await sendPlan({ kind: 'convert', id: PLAN, terms }, 'k5')
    const bodies = vi.mocked(request).mock.calls.map(([, init]) => (init as { body: Record<string, unknown> }).body)
    expect(bodies[0]).toEqual({ body, notify_partner: true })
    expect(bodies[1]).toEqual({ body })
    for (const later of bodies.slice(2)) expect(later).not.toHaveProperty('notify_partner')
  })
})

describe('D20 shared-plan create', () => {
  it('is off on a fresh form, sends false unless chosen, and a later form starts off again', async () => {
    answerPlans(() => Promise.resolve(createdPlan(quiet)))
    const wrapper = page()
    await flushPromises()
    await wrapper.get('[data-test="new-plan"]').trigger('click')
    expect(box(wrapper).checked).toBe(false)
    await fillAndSave(wrapper)
    expect(planPosts()).toEqual([{ body, notify_partner: false }])
    expect(routerPush).toHaveBeenCalledWith({ name: 'shared-plan-detail', params: { id: PLAN } })
  })

  it('captures true, refuses disagreeing facts, keeps the choice locked and retries the original request', async () => {
    let answer: unknown = createdPlan(quiet)
    answerPlans(() => Promise.resolve(answer))
    const wrapper = page()
    await flushPromises()
    await wrapper.get('[data-test="new-plan"]').trigger('click')
    await wrapper.get('[data-test="plan-create-notify-input"]').setValue(true)
    await fillAndSave(wrapper)
    expect(useSharedPlansStore().actions.new?.state).toBe('unknown')
    expect(box(wrapper).checked).toBe(true)
    expect(box(wrapper).disabled).toBe(true)
    expect(routerPush).not.toHaveBeenCalled()
    answer = createdPlan(delivered)
    await useSharedPlansStore().retry('new')
    await flushPromises()
    expect(planPosts()).toEqual([{ body, notify_partner: true }, { body, notify_partner: true }])
    const keys = planKeys()
    expect(keys[1]).toBe(keys[0])
    expect(useSharedPlansStore().actions.new).toBeUndefined()
  })

  it('accepts no_partner facts for a requested create and shows the choice unavailable with no partner', async () => {
    signIn(1)
    answerPlans(() => Promise.resolve(createdPlan(quiet)))
    const wrapper = page()
    await flushPromises()
    await wrapper.get('[data-test="new-plan"]').trigger('click')
    expect(box(wrapper).disabled).toBe(true)
    expect(wrapper.find('[data-test="plan-create-notify-unavailable"]').exists()).toBe(true)
    await fillAndSave(wrapper)
    expect(planPosts()).toEqual([{ body, notify_partner: false }])
    // The store's own check: a requested create may come back with no partner.
    answerPlans(() => Promise.resolve(createdPlan({ requested: true, created: false, reason: 'no_partner', notification_id: null })))
    expect(await useSharedPlansStore().submit({ kind: 'create', body, notify: true })).toEqual({ ok: true, id: PLAN })
  })
})

describe('D20 labels and destinations', () => {
  const kinds = [['memory', 'memory.created', `/us/memories/${TARGET}`], ['shared_plan', 'shared_plan.created', `/us/plans/${TARGET}`],
    ['album', 'album.photos_added', `/us/albums/${TARGET}`]] as const

  it.each(kinds)('%s notices have a fixed generic label and open the page, which checks access itself', (kind, event, path) => {
    const label = eventLabel(event)
    expect(label).toBeTruthy()
    expect(label).not.toMatch(/[{}%$]/)
    expect(targetPath(kind, TARGET)).toBe(path)
    const item: Notification = { id: NID, event_type: event, actor_id: SELF, target_kind: kind, target_id: TARGET, created_at: TIME, read_at: null }
    expect(notificationTarget(item)).toBe(path)
  })

  it.each(kinds)('the service worker accepts %s pushes and allows only their page', (kind, event, path) => {
    const payload = { v: 1, nid: NID, event_type: event, target_kind: kind, target_id: TARGET, marker: 'm'.repeat(22) }
    const parsed = parsePushPayload(JSON.stringify(payload)) as PushPayload
    expect(parsed).toEqual(payload)
    const plan = notificationFor(parsed, { generationId: 'g', marker: 'm'.repeat(22) })
    expect(plan.options.data.target).toBe(path)
    const current = { generationId: 'g', marker: 'm'.repeat(22) }
    expect(destinationFor({ target: path, marker: current.marker }, current)).toBe(path)
    expect(destinationFor({ target: `${path}/edit`, marker: current.marker }, current)).toBe('/notifications')
  })

  it('a near-miss kind is still refused', () => {
    for (const kind of ['memories', 'plan', 'albums', 'album_batch']) {
      expect(parsePushPayload(JSON.stringify({ v: 1, nid: NID, event_type: 'memory.created', target_kind: kind, target_id: TARGET, marker: 'm'.repeat(22) }))).toBeNull()
    }
  })

  it('the D20 toggle reads and writes no remembered preference', () => {
    const source = readFileSync(resolve(__dirname, '../../src/components/PartnerNoticeToggle.vue'), 'utf8')
    expect(source).not.toMatch(/notification-choices|useNotificationChoicesStore|getNotificationChoices/)
  })
})
