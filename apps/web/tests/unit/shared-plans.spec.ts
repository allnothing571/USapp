import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/shared-plans', async original => ({
  ...(await original<typeof import('@/api/shared-plans')>()),
  getPlan: vi.fn(), listPlans: vi.fn(), sendPlan: vi.fn(),
}))

import { ApiError, setCsrfToken } from '@/api/client'
import { getPlan, listPlans, sendPlan, type SharedPlan } from '@/api/shared-plans'
import SharedPlanForm from '@/components/SharedPlanForm.vue'
import { useSessionStore } from '@/stores/session'
import { useSharedPlansStore } from '@/stores/shared-plans'
import SharedPlanDetailView from '@/views/SharedPlanDetailView.vue'

const self = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
const partner = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb'
const id = '11111111-1111-4111-8111-111111111111'
const proposal = '22222222-2222-4222-8222-222222222222'
const receipt = '33333333-3333-4333-8333-333333333333'
const memoryId = '44444444-4444-4444-8444-444444444444'
const photo = '55555555-5555-4555-8555-555555555555'
const time = '2026-11-04T01:00:00Z'
const body = { title: '看海', date: null, time: null, place: null, note: null }
const suggested = { ...body, title: '去公园' }

function item(overrides: Partial<SharedPlan> = {}): SharedPlan {
  return { id, creator_id: self, created_at: time, body, body_version: 1, status: 'want', status_version: 1,
    pending_proposal: null, latest_decision: null, memory_id: null, ...overrides }
}

function advanced(plan: SharedPlan = item({ status: 'plan', status_version: 2 })) {
  return { replayed: false, plan, mutation: { id: receipt, operation: 'advance', body_version: null,
    status: 'plan', status_version: 2, proposal_id: null, outcome: null } }
}

function deferred<T>() {
  let resolve!: (value: T) => void
  const promise = new Promise<T>(done => { resolve = done })
  return { promise, resolve }
}

function owner(value = self): void {
  useSessionStore().user = { id: value, email: 'test@example.test', nickname: null } as never
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  setCsrfToken('csrf')
  owner()
  vi.mocked(getPlan).mockResolvedValue(item())
})

describe('shared plan writes', () => {
  it('validates the create receipt and refuses a mismatched body', async () => {
    const store = useSharedPlansStore()
    vi.mocked(sendPlan).mockResolvedValue({ replayed: false, plan: item({ body: suggested }),
      mutation: { id: receipt, operation: 'create', body_version: 1, status: 'want', status_version: 1 } })
    expect(await store.submit({ kind: 'create', body })).toEqual({ ok: false })
    expect(store.actions.new?.state).toBe('unknown')
    expect(store.details[id]).toBeUndefined()
  })

  it('keeps an uncertain request across a 409 and retries the original source status and key', async () => {
    const store = useSharedPlansStore()
    vi.mocked(sendPlan).mockRejectedValueOnce(new Error('lost')).mockRejectedValueOnce(new ApiError('PLAN_CONFLICT', '', 409))
      .mockResolvedValueOnce({ ...advanced(item({ status: 'finished', status_version: 3 })), replayed: true })
    const command = { kind: 'advance', id, from: 'want', version: 1 } as const
    expect(await store.submit(command)).toEqual({ ok: false })
    expect(await store.retry(id)).toEqual({ ok: false })
    expect(store.actions[id]?.state).toBe('unknown')
    expect(await store.retry(id)).toEqual({ ok: true, id })
    const key = vi.mocked(sendPlan).mock.calls[0]?.[1]
    expect(vi.mocked(sendPlan).mock.calls).toEqual([[command, key], [command, key], [command, key]])
    expect(store.details[id]?.plan?.status).toBe('finished')
  })

  it('copies nested form data and blocks another write while a receipt is outstanding', async () => {
    const store = useSharedPlansStore()
    const wait = deferred<unknown>()
    vi.mocked(sendPlan).mockReturnValueOnce(wait.promise)
    const draft = { ...body }
    const first = store.submit({ kind: 'edit', id, version: 1, body: draft })
    draft.title = 'changed locally'
    expect(await store.submit({ kind: 'advance', id, from: 'want', version: 1 })).toEqual({ ok: false })
    expect(vi.mocked(sendPlan).mock.calls[0]?.[0]).toMatchObject({ body })
    wait.resolve({ replayed: false, plan: item({ body_version: 2 }), mutation: { id: receipt, operation: 'edit', body_version: 2 } })
    expect(await first).toEqual({ ok: true, id })
  })

  it('discards a response from a previous account', async () => {
    const store = useSharedPlansStore()
    const wait = deferred<unknown>()
    vi.mocked(sendPlan).mockReturnValueOnce(wait.promise)
    const first = store.submit({ kind: 'advance', id, from: 'want', version: 1 })
    store.reset()
    owner(partner)
    wait.resolve(advanced())
    expect(await first).toEqual({ ok: false })
    expect(store.details).toEqual({})
    expect(store.actions).toEqual({})
  })

  it('does not let an in-flight detail read undo a completed write', async () => {
    const store = useSharedPlansStore()
    const stale = deferred<SharedPlan>()
    vi.mocked(getPlan).mockReturnValueOnce(stale.promise)
      .mockResolvedValueOnce(item({ status: 'plan', status_version: 2 }))
    vi.mocked(sendPlan).mockResolvedValueOnce(advanced())
    const before = store.loadDetail(id)
    await store.submit({ kind: 'advance', id, from: 'want', version: 1 })
    stale.resolve(item())
    await before
    expect(store.details[id]?.plan?.status).toBe('plan')
  })

  it('checks the exact proposal and approved body', async () => {
    const store = useSharedPlansStore()
    vi.mocked(sendPlan).mockResolvedValue({ replayed: false, plan: item({ body_version: 2, body: suggested }),
      mutation: { id: receipt, operation: 'decide', proposal_id: proposal, outcome: 'approved', body_version: 2 } })
    expect(await store.submit({ kind: 'decide', id, proposalId: proposal, outcome: 'approved', baseVersion: 1, suggested }))
      .toEqual({ ok: true, id })
    expect(store.details[id]?.plan?.body.title).toBe(suggested.title)
  })

  it('checks the exact memory link and preserves it over an older refresh', async () => {
    const store = useSharedPlansStore()
    vi.mocked(getPlan).mockResolvedValue(item({ status: 'finished', status_version: 3 }))
    await store.loadDetail(id)
    vi.mocked(sendPlan).mockResolvedValue({ plan_id: id, memory_id: memoryId, replayed: false })
    expect(await store.submit({ kind: 'link', id, memoryId })).toEqual({ ok: true, id })
    await flushPromises()
    expect(store.details[id]?.plan?.memory_id).toBe(memoryId)
  })

  it('validates the memory receipt from a conversion', async () => {
    const store = useSharedPlansStore()
    vi.mocked(getPlan).mockResolvedValue(item({ status: 'finished', status_version: 3 }))
    await store.loadDetail(id)
    const terms = { title: '看海', date: '2026-11-04', description: '', media_ids: [photo] }
    vi.mocked(sendPlan).mockResolvedValue({ mutation: { action: 'create', memory_id: memoryId, actor_id: self, version: 1 },
      memory: { id: memoryId, creator_id: self, version: 1, title: terms.title, date: terms.date,
        description: terms.description, photos: [{ media_id: photo, position: 1 }], pending_proposal: null } })
    expect(await store.submit({ kind: 'convert', id, terms })).toEqual({ ok: true, id })
    expect(store.details[id]?.plan?.memory_id).toBe(memoryId)
  })

  it('clears a known link when the server marks its memory unavailable, and restores it on undo', async () => {
    const store = useSharedPlansStore()
    vi.mocked(getPlan).mockResolvedValueOnce(item({ status: 'finished', status_version: 3, memory_id: memoryId, memory_unavailable: false }))
      .mockResolvedValueOnce(item({ status: 'finished', status_version: 3, memory_id: null, memory_unavailable: true }))
      .mockResolvedValueOnce(item({ status: 'finished', status_version: 3, memory_id: memoryId, memory_unavailable: false }))
    await store.loadDetail(id)
    expect(store.details[id]?.plan?.memory_id).toBe(memoryId)
    await store.loadDetail(id)
    expect(store.details[id]?.plan?.memory_id).toBeNull()
    expect(store.details[id]?.plan?.memory_unavailable).toBe(true)
    await store.loadDetail(id)
    expect(store.details[id]?.plan?.memory_id).toBe(memoryId)
  })

  it('passes the next cursor unchanged and does not duplicate items', async () => {
    const store = useSharedPlansStore()
    vi.mocked(listPlans).mockResolvedValueOnce({ plans: [item()], next_cursor: 'page-2' })
      .mockResolvedValueOnce({ plans: [item(), item({ id: receipt })], next_cursor: null })
    await store.loadList()
    await store.loadList(true)
    expect(listPlans).toHaveBeenLastCalledWith('page-2')
    expect(store.items.map(plan => plan.id)).toEqual([id, receipt])
  })
})

describe('shared plan views', () => {
  it('requires a date when entering a time', async () => {
    const wrapper = mount(SharedPlanForm, { props: { label: '保存' } })
    await wrapper.get('[data-test="plan-title"]').setValue('  看海  ')
    await wrapper.get('[data-test="plan-time"]').setValue('09:30')
    await wrapper.get('form').trigger('submit')
    expect(wrapper.emitted('submit')).toBeUndefined()
    await wrapper.get('[data-test="plan-date"]').setValue('2026-11-05')
    await wrapper.get('form').trigger('submit')
    expect(wrapper.emitted('submit')?.[0]?.[0]).toEqual({ ...body, date: '2026-11-05', time: '09:30' })
    wrapper.unmount()
  })

  it.each([[self, 'plan-approve', 'plan-withdraw'], [partner, 'plan-withdraw', 'plan-approve']])(
    'shows only the allowed proposal actions and leaves advance available for %s', async (who, allowed, absent) => {
      owner(who)
      vi.mocked(getPlan).mockResolvedValue(item({ pending_proposal: { id: proposal, proposer_id: partner,
        base_version: 1, original: body, suggested, created_at: time } }))
      const wrapper = mount(SharedPlanDetailView, { props: { id }, global: { stubs: ['RouterLink'] } })
      await flushPromises()
      expect(wrapper.find('[data-test="' + allowed + '"]').exists()).toBe(true)
      expect(wrapper.find('[data-test="' + absent + '"]').exists()).toBe(false)
      expect(wrapper.find('[data-test="plan-edit"]').exists()).toBe(false)
      expect(wrapper.get('[data-test="plan-advance"]').attributes('disabled')).toBeUndefined()
      expect(wrapper.get('[data-test="plan-proposal"]').text()).toContain(suggested.title)
      wrapper.unmount()
    },
  )
})
