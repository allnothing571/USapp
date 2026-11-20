/**
 * P2-2 anniversaries (web): civil-date rules and labels independent of the runtime time zone; the
 * store's unknown-result retry matrix (P2-1 Section 12 rules) with target validation; captured
 * versions; account isolation; the Home card states and independence; the detail page roles,
 * version-conflict draft retention and per-target draft reset.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/anniversaries', () => ({
  createAnniversary: vi.fn(), listAnniversaries: vi.fn(), getAnniversary: vi.fn(), editAnniversary: vi.fn(),
  proposeAnniversaryEdit: vi.fn(), decideAnniversaryProposal: vi.fn(), getHomeAnniversary: vi.fn(),
  setHomeAnniversaryPin: vi.fn(),
}))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn() }))
vi.mock('@/api/tasks', () => ({ newIdempotencyKey: vi.fn(), listTasks: vi.fn() }))
vi.mock('@/api/rewards', () => ({ listRedemptions: vi.fn() }))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))

import {
  createAnniversary,
  decideAnniversaryProposal,
  editAnniversary,
  getAnniversary,
  getHomeAnniversary,
  listAnniversaries,
  setHomeAnniversaryPin,
} from '@/api/anniversaries'
import {
  anniversaryHeadline,
  anniversaryNextLine,
  checkAnniversaryTerms,
  civilDateLabel,
  isCivilDate,
} from '@/api/anniversary-rules'
import { ApiError, OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { listMessages } from '@/api/messages'
import { listRedemptions } from '@/api/rewards'
import { listTasks, newIdempotencyKey } from '@/api/tasks'
import type {
  Anniversary,
  AnniversaryMutationResponse,
  AnniversaryPinResponse,
  AnniversaryTerms,
} from '@/api/types'
import { getWallet } from '@/api/wallet'
import { useAnniversariesStore } from '@/stores/anniversaries'
import { useHomeStore } from '@/stores/home'
import { useSessionStore } from '@/stores/session'
import AnniversaryDetailView from '@/views/AnniversaryDetailView.vue'
import HomeView from '@/views/HomeView.vue'

const TERMS: AnniversaryTerms = { title: '在一起', date: '2020-05-20', kind: 'anniversary', note: '' }

function item(id: string, overrides: Partial<Anniversary> = {}): Anniversary {
  return {
    id, creator_id: 'self', version: 1, ...TERMS, created_at: '2026-10-29T01:00:00.000000Z',
    updated_at: '2026-10-29T01:00:00.000000Z', pinned_by_me: false,
    computed: { days_since: 2353, next: { date: '2027-05-20', years: 7, days_until: 203 }, days_until: null },
    pending_proposal: null, last_decision: null, ...overrides,
  }
}

function revision(a: Anniversary, action: 'create' | 'edit', actor = 'self'): AnniversaryMutationResponse {
  return {
    mutation: { action, anniversary_id: a.id, version: a.version, actor_id: actor, proposal_id: null, title: a.title,
      date: a.date, kind: a.kind, note: a.note, created_at: a.updated_at },
    as_of_date: '2026-10-29', anniversary: a,
    notification: { requested: false, created: false, reason: 'not_requested' as const, notification_id: null },
  }
}

function signIn(id: string): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-29T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-29T00:00:00Z', members: [] },
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
const originalTz = process.env.TZ
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  let counter = 0
  vi.mocked(newIdempotencyKey).mockImplementation(() => `key-${String(++counter).padStart(16, '0')}`)
  vi.mocked(listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-29', anniversaries: [], next_cursor: null })
  vi.mocked(getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 1, sunshine: 2 } })
  signIn('self')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  process.env.TZ = originalTz
})

describe('rules and labels', () => {
  it('validates terms like the server: title 1-40, civil date 1900-2199, kind, note 0-500', () => {
    expect(checkAnniversaryTerms({ title: ' 在一起 ', date: '2020-05-20', kind: 'anniversary', note: ' 见面\r\n' }))
      .toEqual({ ok: true, terms: { title: '在一起', date: '2020-05-20', kind: 'anniversary', note: '见面' } })
    expect(checkAnniversaryTerms({ title: '天'.repeat(41), date: '2020-05-20', kind: 'anniversary', note: '' }).ok).toBe(false)
    expect(checkAnniversaryTerms({ title: '天'.repeat(40), date: '2020-05-20', kind: 'anniversary', note: '' }).ok).toBe(true)
    for (const date of ['1899-12-31', '2200-01-01', '2023-02-29', '2020-5-20', '']) {
      expect(checkAnniversaryTerms({ title: 'a', date, kind: 'countdown', note: '' }).ok).toBe(false)
    }
    expect(checkAnniversaryTerms({ title: 'a', date: '2024-02-29', kind: 'countdown', note: '' }).ok).toBe(true)
    expect(checkAnniversaryTerms({ title: 'a', date: '2020-01-01', kind: '', note: '' }).ok).toBe(false)
    expect(checkAnniversaryTerms({ title: 'a', date: '2020-01-01', kind: 'countdown', note: 'x'.repeat(501) }).ok).toBe(false)
    expect(isCivilDate('2100-02-29')).toBe(false)
    expect(isCivilDate('2000-02-29')).toBe(true)
  })

  it('labels use only server-computed days (O2: N = T − D)', () => {
    const a = item('a')
    expect(anniversaryHeadline(a)).toBe('已经 2353 天')
    expect(anniversaryNextLine(a)).toBe('距 7 周年还有 203 天（2027年5月20日）')
    expect(anniversaryHeadline(item('b', { computed: { days_since: -3, next: { date: '2026-11-01', years: 0, days_until: 3 },
      days_until: null } }))).toBe('还有 3 天')
    expect(anniversaryNextLine(item('c', { computed: { days_since: 365, next: { date: '2026-10-29', years: 1, days_until: 0 },
      days_until: null } }))).toBe('今天是 1 周年')
    const countdown = (n: number) => item('d', { kind: 'countdown', computed: { days_since: null, next: null, days_until: n } })
    expect(anniversaryHeadline(countdown(4))).toBe('还有 4 天')
    expect(anniversaryHeadline(countdown(0))).toBe('就是今天')
    expect(anniversaryHeadline(countdown(-2))).toBe('已过 2 天')
    expect(anniversaryNextLine(countdown(4))).toBe('')
  })

  it.each(['America/Los_Angeles', 'Asia/Shanghai', 'UTC'])('date labels do not shift in TZ=%s', (tz) => {
    process.env.TZ = tz
    // The runtime zone really changed: a naive Date parse is UTC midnight and moves in negative zones.
    const naiveDay = new Date('2026-10-29').getDate()
    expect(naiveDay).toBe(tz === 'America/Los_Angeles' ? 28 : 29)
    expect(civilDateLabel('2026-10-29')).toBe('2026年10月29日')
    expect(civilDateLabel('2200-01-01')).toBe('2200年1月1日')
    expect(anniversaryNextLine(item('a'))).toBe('距 7 周年还有 203 天（2027年5月20日）')
    expect(checkAnniversaryTerms({ title: 'a', date: '1900-01-01', kind: 'anniversary', note: '' }).ok).toBe(true)
  })
})

describe('store retry rules', () => {
  it.each([
    ['CSRF_TOKEN_INVALID', 403], ['UNAUTHENTICATED', 401], ['ORIGIN_NOT_ALLOWED', 403], ['RATE_LIMITED', 429],
    ['ANNIVERSARY_VERSION_CONFLICT', 409], ['VALIDATION_ERROR', 400],
  ])('after an unknown create, a %s retry keeps the same key and terms until a matching success', async (code, status) => {
    const store = useAnniversariesStore()
    const a = item('a1')
    vi.mocked(createAnniversary)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockRejectedValueOnce(new ApiError(code, 'rejected', status))
      .mockResolvedValueOnce(revision(a, 'create'))
    expect((await store.create(TERMS)).outcome).toBe('unknown')
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', key: 'key-0000000000000001', terms: TERMS, unresolved: true })
    expect((await store.retry('new')).outcome).toBe('done')
    expect(vi.mocked(createAnniversary).mock.calls).toEqual([
      [TERMS, 'key-0000000000000001'], [TERMS, 'key-0000000000000001'], [TERMS, 'key-0000000000000001'],
    ])
    expect(store.actions.new).toBeUndefined()
  })

  it('after an unknown result, offline and missing-token retries stay unknown without sending', async () => {
    const store = useAnniversariesStore()
    vi.mocked(createAnniversary)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockRejectedValueOnce(new OfflineMutationBlockedError())
    await store.create(TERMS)
    expect((await store.retry('new')).outcome).toBe('unknown')
    const session = useSessionStore()
    vi.spyOn(session, 'ensureCsrfToken').mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    expect((await store.retry('new')).outcome).toBe('unknown')
    expect(store.actions.new).toMatchObject({ status: 'unknown', key: 'key-0000000000000001' })
  })

  it('a first-attempt 4xx ends the action; an unsent first attempt is not_sent', async () => {
    const store = useAnniversariesStore()
    vi.mocked(createAnniversary).mockRejectedValueOnce(new ApiError('VALIDATION_ERROR', 'bad', 400))
    expect(await store.create(TERMS)).toEqual({ outcome: 'rejected', errorCode: 'VALIDATION_ERROR' })
    expect(store.actions.new).toBeUndefined()
    vi.mocked(createAnniversary).mockRejectedValueOnce(new OfflineMutationBlockedError())
    expect((await store.create(TERMS)).outcome).toBe('not_sent')
    expect(store.actions.new).toMatchObject({ status: 'not_sent' })
  })

  it('a success that does not match the captured target stays unknown', async () => {
    const store = useAnniversariesStore()
    // Edit captured version 3: a response with version 5, another item, or other terms is not a confirmation.
    const wrongVersion = revision(item('a1', { version: 5 }), 'edit')
    const otherItem = revision(item('a2', { version: 4 }), 'edit')
    const otherTerms = revision(item('a1', { version: 4, title: '别的' }), 'edit')
    vi.mocked(editAnniversary)
      .mockResolvedValueOnce(wrongVersion).mockResolvedValueOnce(otherItem).mockResolvedValueOnce(otherTerms)
      .mockResolvedValueOnce(revision(item('a1', { version: 4 }), 'edit'))
    expect((await store.edit('a1', 3, TERMS)).outcome).toBe('unknown')
    expect((await store.retry('edit:a1')).outcome).toBe('unknown')
    expect((await store.retry('edit:a1')).outcome).toBe('unknown')
    expect((await store.retry('edit:a1')).outcome).toBe('done')
    for (const call of vi.mocked(editAnniversary).mock.calls) expect(call).toEqual(['a1', 3, TERMS, 'key-0000000000000001'])
  })

  it('decisions and pins validate proposal, outcome and pin target', async () => {
    const store = useAnniversariesStore()
    const decided = (outcome: 'approved' | 'rejected', version: number | null): AnniversaryMutationResponse => ({
      mutation: { action: 'decide', anniversary_id: 'a1', proposal_id: 'p1', proposer_id: 'partner', outcome,
        decided_by: 'self', base_version: 1, version, decided_at: '' },
      as_of_date: '2026-10-29', anniversary: item('a1', { version: version ?? 1 }),
    })
    vi.mocked(decideAnniversaryProposal).mockResolvedValueOnce(decided('rejected', null))
      .mockResolvedValueOnce(decided('approved', 2))
    expect((await store.decide('a1', 'p1', 'approve')).outcome).toBe('unknown')
    expect((await store.retry('decide:a1:p1')).outcome).toBe('done')

    const pinned = (target: string | null): AnniversaryPinResponse => ({
      mutation: { action: 'pin', user_id: 'self', seq: 2, anniversary_id: target, created_at: '' },
      pin: { anniversary_id: target, seq: 2, updated_at: '2026-10-29T02:00:00.000000Z' },
    })
    vi.mocked(setHomeAnniversaryPin).mockResolvedValueOnce(pinned('a2')).mockResolvedValueOnce(pinned('a1'))
    expect((await store.pin('a1')).outcome).toBe('unknown')
    expect((await store.retry('pin')).outcome).toBe('done')
    expect(vi.mocked(setHomeAnniversaryPin).mock.calls.map((call) => call[0])).toEqual(['a1', 'a1'])
  })

  it.each(['account switch', 'logout'])('late results are discarded after %s and actions are cleared', async (boundary) => {
    const store = useAnniversariesStore()
    const late = deferred<AnniversaryMutationResponse>()
    vi.mocked(createAnniversary).mockReturnValueOnce(late.promise)
    const pending = store.create(TERMS)
    await flushPromises()
    if (boundary === 'logout') await useSessionStore().logout()
    else signIn('other')
    late.resolve(revision(item('a1'), 'create'))
    expect((await pending).outcome).toBe('discarded')
    expect(store.actions).toEqual({})
    expect(store.notices).toEqual([])
  })
})

describe('pin replay applies the current pin, not the replayed target', () => {
  const T1 = '2026-10-29T01:00:00.000000Z'
  const T2 = '2026-10-29T02:00:00.000000Z'
  function replay(current: unknown): AnniversaryPinResponse {
    // Immutable facts of the original request (pin A, seq 1) plus the caller's pin at read time.
    return { mutation: { action: 'pin', user_id: 'self', seq: 1, anniversary_id: 'A', created_at: T1 },
      pin: current } as AnniversaryPinResponse
  }
  async function loadedStore(pinnedA = false) {
    vi.mocked(listAnniversaries).mockResolvedValueOnce({ as_of_date: '2026-10-29', next_cursor: null,
      anniversaries: [item('A', { pinned_by_me: pinnedA }), item('B')] })
    vi.mocked(getAnniversary).mockResolvedValueOnce({ as_of_date: '2026-10-29', anniversary: item('A', { pinned_by_me: pinnedA }) })
    const store = useAnniversariesStore()
    await store.load()
    await store.openDetail('A')
    return store
  }
  const pinnedIds = (store: ReturnType<typeof useAnniversariesStore>) =>
    store.items.filter((a) => a.pinned_by_me).map((a) => a.id)

  it('an old replay with mutation A and current pin B keeps B selected', async () => {
    const store = await loadedStore()
    vi.mocked(setHomeAnniversaryPin)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(replay({ anniversary_id: 'B', seq: 2, updated_at: T2 }))
    expect((await store.pin('A')).outcome).toBe('unknown')
    expect((await store.retry('pin')).outcome).toBe('done')
    expect(vi.mocked(setHomeAnniversaryPin).mock.calls).toEqual([['A', 'key-0000000000000001'], ['A', 'key-0000000000000001']])
    expect(pinnedIds(store)).toEqual(['B'])
    expect(store.detail.item?.pinned_by_me).toBe(false)
    expect(store.notices[0].text).toContain('最新的首页设置')
    expect(store.actions.pin).toBeUndefined()
  })

  it('an old replay with a currently cleared pin leaves nothing selected', async () => {
    const store = await loadedStore(true)
    vi.mocked(setHomeAnniversaryPin)
      .mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
      .mockResolvedValueOnce(replay({ anniversary_id: null, seq: 3, updated_at: T2 }))
    await store.pin('A')
    expect((await store.retry('pin')).outcome).toBe('done')
    expect(pinnedIds(store)).toEqual([])
    expect(store.detail.item?.pinned_by_me).toBe(false)
  })

  it.each([
    ['missing pin', undefined],
    ['pin not an object', 'B'],
    ['numeric target', { anniversary_id: 5, seq: 2, updated_at: T2 }],
    ['empty target', { anniversary_id: '', seq: 2, updated_at: T2 }],
    ['seq older than the mutation', { anniversary_id: 'B', seq: 0, updated_at: T2 }],
    ['fractional seq', { anniversary_id: 'B', seq: 1.5, updated_at: T2 }],
    ['missing seq', { anniversary_id: 'B', updated_at: T2 }],
    ['bad updated_at', { anniversary_id: 'B', seq: 2, updated_at: 7 }],
  ])('malformed current pin (%s) does not resolve an unknown action', async (_label, current) => {
    const store = await loadedStore()
    vi.mocked(setHomeAnniversaryPin)
      .mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'unknown', 503))
      .mockResolvedValueOnce(replay(current))
      .mockRejectedValueOnce(new ApiError('CSRF_TOKEN_INVALID', 'rejected', 403))
      .mockResolvedValueOnce(replay({ anniversary_id: 'A', seq: 1, updated_at: T1 }))
    await store.pin('A')
    expect((await store.retry('pin')).outcome).toBe('unknown')
    expect(store.actions.pin).toMatchObject({ status: 'unknown', key: 'key-0000000000000001', pinTarget: 'A', unresolved: true })
    expect(pinnedIds(store)).toEqual([])
    // A later rejected retry still keeps the unresolved action; only a valid same-key answer resolves it.
    expect((await store.retry('pin')).outcome).toBe('unknown')
    expect((await store.retry('pin')).outcome).toBe('done')
    expect(pinnedIds(store)).toEqual(['A'])
    for (const call of vi.mocked(setHomeAnniversaryPin).mock.calls) expect(call).toEqual(['A', 'key-0000000000000001'])
  })

  it('a replayed mutation for another target or user stays unknown', async () => {
    const store = await loadedStore()
    const other = replay({ anniversary_id: 'B', seq: 2, updated_at: T2 })
    vi.mocked(setHomeAnniversaryPin)
      .mockResolvedValueOnce({ ...other, mutation: { ...other.mutation, anniversary_id: 'B' } })
      .mockResolvedValueOnce({ ...other, mutation: { ...other.mutation, user_id: 'partner' } })
    expect((await store.pin('A')).outcome).toBe('unknown')
    expect((await store.retry('pin')).outcome).toBe('unknown')
    expect(pinnedIds(store)).toEqual([])
  })

  it('a late pin replay after an account switch changes nothing for the new account', async () => {
    const store = await loadedStore()
    const late = deferred<AnniversaryPinResponse>()
    vi.mocked(setHomeAnniversaryPin).mockReturnValueOnce(late.promise)
    const pending = store.pin('A')
    await flushPromises()
    signIn('other')
    late.resolve(replay({ anniversary_id: 'B', seq: 2, updated_at: T2 }))
    expect((await pending).outcome).toBe('discarded')
    expect(store.items).toEqual([])
    expect(store.actions).toEqual({})
  })
})

describe('Home anniversary card', () => {
  it('shows nothing while loading, then the selected item linking to its detail', async () => {
    const gate = deferred<{ as_of_date: string; source: 'pinned'; anniversary: Anniversary }>()
    vi.mocked(getHomeAnniversary).mockReturnValueOnce(gate.promise)
    wrapper = mount(HomeView, { global: { stubs } })
    await flushPromises()
    const card = () => wrapper!.find('[data-test="home-anniversary"]')
    expect(card().text()).toContain('加载中')
    expect(card().find('[data-test="home-anniversary-none"]').exists()).toBe(false)
    gate.resolve({ as_of_date: '2026-10-29', source: 'pinned', anniversary: item('a1') })
    await flushPromises()
    expect(card().find('[data-test="home-anniversary-headline"]').text()).toBe('已经 2353 天')
    expect(card().text()).toContain('已置顶')
    expect(card().find('[data-test="home-anniversary-link"]').attributes('data-to'))
      .toBe(JSON.stringify({ name: 'anniversary-detail', params: { id: 'a1' } }))
  })

  it('an error shows a retry, never the empty state; other cards are unaffected and vice versa', async () => {
    vi.mocked(getHomeAnniversary).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    vi.mocked(listMessages).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    wrapper = mount(HomeView, { global: { stubs } })
    await flushPromises()
    const card = wrapper.find('[data-test="home-anniversary"]')
    expect(card.find('[data-test="home-anniversary-retry"]').exists()).toBe(true)
    expect(card.find('[data-test="home-anniversary-none"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="home-today"]').text()).toContain('今天没有要做的任务')
    vi.mocked(getHomeAnniversary).mockResolvedValueOnce({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
    await card.find('[data-test="home-anniversary-retry"]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="home-anniversary-none"]').exists()).toBe(true)
    // The message card failed on its own and still shows its own error.
    expect(wrapper.find('[data-test="home-latest-message"]').find('[role="alert"]').exists()).toBe(true)
    expect(getHomeAnniversary).toHaveBeenCalledTimes(2)
  })

  it('a late Home anniversary result for the old account is discarded', async () => {
    const home = useHomeStore()
    const late = deferred<{ as_of_date: string; source: 'auto'; anniversary: Anniversary }>()
    vi.mocked(getHomeAnniversary).mockReturnValueOnce(late.promise)
    const loading = home.loadAnniversary()
    signIn('other')
    late.resolve({ as_of_date: '2026-10-29', source: 'auto', anniversary: item('old') })
    await loading
    expect(home.anniversary.anniversary).toBeNull()
    expect(home.anniversary.status).toBe('idle')
  })
})

describe('anniversary detail', () => {
  it('the creator edits with the captured version; a conflict keeps the draft until the user continues', async () => {
    vi.mocked(getAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', anniversary: item('a1', { version: 2 }) })
    vi.mocked(editAnniversary).mockRejectedValueOnce(new ApiError('ANNIVERSARY_VERSION_CONFLICT', 'conflict', 409))
    wrapper = mount(AnniversaryDetailView, { props: { id: 'a1' }, global: { stubs } })
    await flushPromises()
    await wrapper.find('[data-test="anniversary-start-write"]').trigger('click')
    await wrapper.find('[data-test="anniversary-write-title"]').setValue('在一起（改）')
    // The server copy moves to version 3 meanwhile.
    vi.mocked(getAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', anniversary: item('a1', { version: 3 }) })
    await wrapper.find('[data-test="anniversary-write-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editAnniversary).mock.calls[0].slice(0, 2)).toEqual(['a1', 2])
    expect(wrapper.find('[data-test="anniversary-conflict"]').exists()).toBe(true)
    expect((wrapper.find('[data-test="anniversary-write-title"]').element as HTMLInputElement).value).toBe('在一起（改）')
    vi.mocked(editAnniversary).mockResolvedValueOnce(revision(item('a1', { version: 4, title: '在一起（改）' }), 'edit'))
    await wrapper.find('[data-test="anniversary-use-latest"]').trigger('click')
    await wrapper.find('[data-test="anniversary-write-form"]').trigger('submit')
    await flushPromises()
    expect(vi.mocked(editAnniversary).mock.calls[1].slice(0, 3))
      .toEqual(['a1', 3, { ...TERMS, title: '在一起（改）' }])
    expect(wrapper.find('[data-test="anniversary-write-form"]').exists()).toBe(false)
  })

  it('the partner proposes instead of editing; the creator approves or rejects a pending proposal', async () => {
    const proposal = { id: 'p1', proposer_id: 'partner', base_version: 1, ...TERMS, title: '改', created_at: '' }
    vi.mocked(getAnniversary).mockResolvedValue({ as_of_date: '2026-10-29',
      anniversary: item('a1', { creator_id: 'partner' }) })
    wrapper = mount(AnniversaryDetailView, { props: { id: 'a1' }, global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="anniversary-start-write"]').text()).toBe('申请修改')
    wrapper.unmount()
    vi.mocked(getAnniversary).mockResolvedValue({ as_of_date: '2026-10-29',
      anniversary: item('a2', { pending_proposal: proposal }) })
    wrapper = mount(AnniversaryDetailView, { props: { id: 'a2' }, global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="anniversary-approve"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="anniversary-withdraw"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="anniversary-start-write"]').exists()).toBe(false)
  })

  it('the proposer sees withdraw; changing the item clears the draft and a late result keeps the new form', async () => {
    const proposal = { id: 'p1', proposer_id: 'self', base_version: 1, ...TERMS, title: '改', created_at: '' }
    vi.mocked(getAnniversary).mockImplementation(async (id: string) => ({ as_of_date: '2026-10-29',
      anniversary: id === 'a1' ? item('a1', { creator_id: 'partner', pending_proposal: proposal })
        : item(id, { creator_id: 'partner' }) }))
    wrapper = mount(AnniversaryDetailView, { props: { id: 'a1' }, global: { stubs } })
    await flushPromises()
    expect(wrapper.find('[data-test="anniversary-withdraw"]').exists()).toBe(true)
    await wrapper.setProps({ id: 'b1' })
    await flushPromises()
    await wrapper.find('[data-test="anniversary-start-write"]').trigger('click')
    await wrapper.find('[data-test="anniversary-write-title"]').setValue('b 的草稿')
    await wrapper.setProps({ id: 'c1' })
    await flushPromises()
    expect(wrapper.find('[data-test="anniversary-write-title"]').exists()).toBe(false)
    await wrapper.find('[data-test="anniversary-start-write"]').trigger('click')
    expect((wrapper.find('[data-test="anniversary-write-title"]').element as HTMLInputElement).value).toBe(TERMS.title)
  })
})
