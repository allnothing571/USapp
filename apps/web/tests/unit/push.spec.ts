import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'

vi.mock('@/api/push', async (original) => ({
  ...(await original<typeof import('@/api/push')>()),
  getPushConfig: vi.fn(),
  listPushSubscriptions: vi.fn(),
  registerPushSubscription: vi.fn(),
  revokePushSubscription: vi.fn(),
}))
vi.mock('@/api/auth', () => ({ deleteSession: vi.fn() }))

import * as authApi from '@/api/auth'
import { ApiError, setCsrfToken } from '@/api/client'
import {
  getPushConfig, isValidRegistrationResponse, listPushSubscriptions, registerPushSubscription,
  revokePushSubscription, sha256Hex,
} from '@/api/push'
import type { PushGeneration, PushRegistrationBody, PushRegistrationResponse, PushRevocationResponse, PushSubscriptionList } from '@/api/push'
import PushDeviceCard from '@/components/PushDeviceCard.vue'
import { setPushEnvironmentForTests } from '@/pwa/push-env'
import type { PushEnvironment } from '@/pwa/push-env'
import { captureMarker, markerIdle, readMarker, setMarkerBackendForTests, writeMarker } from '@/pwa/push-marker'
import type { MarkerBackend, MarkerRecord } from '@/pwa/push-marker'
import { usePushStore } from '@/stores/push'
import { useSessionStore } from '@/stores/session'

const VAPID = 'B' + 'A'.repeat(86)
const G1 = '11111111-1111-4111-8111-111111111111'
const G3 = '33333333-3333-4333-8333-333333333333'
const USER = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
const BODY: PushRegistrationBody = {
  endpoint: 'https://updates.push.services.mozilla.com/wpush/v2/device-1',
  keys: { p256dh: 'B' + 'p'.repeat(86), auth: 'a'.repeat(22) },
}
const MARKER_1 = 'm'.repeat(22)
const MARKER_3 = 'n'.repeat(22)

// ---------------------------------------------------------------- fakes
interface Deferred<T> { promise: Promise<T>; resolve: (v: T) => void; reject: (e: unknown) => void }
function deferred<T>(): Deferred<T> {
  let resolve!: (v: T) => void, reject!: (e: unknown) => void
  const promise = new Promise<T>((res, rej) => { resolve = res; reject = rej })
  return { promise, resolve, reject }
}

class MemoryBackend implements MarkerBackend {
  value: MarkerRecord | null = null
  fenceValue = 0
  gate: Deferred<void> | null = null
  getGate: Deferred<void> | null = null
  puts = 0
  async fence(): Promise<number> { return this.fenceValue }
  async putIfFence(record: MarkerRecord, fence: number): Promise<boolean> {
    this.puts += 1
    if (this.gate) await this.gate.promise
    if (fence !== this.fenceValue) return false
    this.value = { ...record }
    return true
  }
  async clearAll(): Promise<void> { this.fenceValue += 1; this.value = null }
  async compareDelete(id: string): Promise<void> { if (this.value?.generationId === id) this.value = null }
  async get(): Promise<MarkerRecord | null> {
    if (this.getGate) await this.getGate.promise
    return this.value ? { ...this.value } : null
  }
}

type MockEnvironment = PushEnvironment & { requestPermission: ReturnType<typeof vi.fn>; subscribe: ReturnType<typeof vi.fn>; unsubscribe: ReturnType<typeof vi.fn>; closeMarkedNotifications: ReturnType<typeof vi.fn> }
function environment(overrides: Partial<PushEnvironment> = {}): MockEnvironment {
  return {
    supported: () => true,
    permission: () => 'granted',
    requestPermission: vi.fn(async () => 'granted' as NotificationPermission),
    workerCapable: async () => true,
    subscribe: vi.fn(async () => ({ endpoint: BODY.endpoint, keys: { ...BODY.keys } })),
    unsubscribe: vi.fn(async () => undefined),
    closeMarkedNotifications: vi.fn(async () => undefined),
    ...overrides,
  } as MockEnvironment
}

function generation(id: string, overrides: Partial<PushGeneration> = {}): PushGeneration {
  return {
    id, state: 'active', activated_at: '2026-10-30T01:00:00.000000Z', revoked_reason: null, revoked_at: null,
    current_session: true, provider: 'mozilla', user_agent_family: 'firefox', ...overrides,
  }
}

async function registration(id: string, marker: string | null, overrides: Partial<PushGeneration> = {},
  result: 'created' | 'already_active' = 'created'): Promise<PushRegistrationResponse> {
  return {
    command: { result, generation_id: id, recorded_at: '2026-10-30T01:00:00.000000Z' },
    generation: { ...generation(id, overrides), endpoint_hash: await sha256Hex(BODY.endpoint), vapid_key_id: await sha256Hex(VAPID) },
    marker,
  }
}

function revocation(id: string): PushRevocationResponse {
  const at = '2026-10-30T02:00:00.000000Z'
  return {
    revocation: { generation_id: id, reason: 'user', revoked_by: USER, revoked_at: at },
    generation: generation(id, { state: 'revoked', revoked_reason: 'user', revoked_at: at }),
  }
}

/** Unrevoked generations are slot-occupying devices; revoked ones are history. */
function list(...generations: PushGeneration[]): PushSubscriptionList {
  const devices = generations.filter((g) => g.state !== 'revoked')
  return { devices, history: generations.filter((g) => g.state === 'revoked'), occupied_slots: devices.length, device_limit: 10 }
}
function revokedGeneration(id: string): PushGeneration {
  return generation(id, { state: 'revoked', revoked_reason: 'user', revoked_at: '2026-10-30T02:00:00.000000Z' })
}

function login(id = USER): void {
  useSessionStore().applyBootstrap({
    authenticated: true, csrf_token: 'csrf',
    user: { id, email: `${id}@example.com`, display_name: 'x', created_at: '2026-10-30T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-30T00:00:00Z', members: [] },
  })
  setCsrfToken('csrf')
}

let backend: MemoryBackend

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  backend = new MemoryBackend()
  setMarkerBackendForTests(backend)
  setPushEnvironmentForTests(environment())
  vi.mocked(getPushConfig).mockResolvedValue({ subscribe_available: true, vapid_public_key: VAPID, device_limit: 10 })
  vi.mocked(listPushSubscriptions).mockResolvedValue(list())
  login()
})
afterEach(() => {
  setPushEnvironmentForTests(null)
  setMarkerBackendForTests(null)
})

describe('P2-5c explicit opt-in and this device only', () => {
  it('reaches the permission prompt synchronously from the user gesture', async () => {
    const env = environment({ permission: () => 'default' })
    setPushEnvironmentForTests(env)
    const s = usePushStore()
    await s.load()
    await flushPromises()
    env.requestPermission.mockClear()
    const pending = s.enable()
    // Not one microtask has run since the "click": the browser still sees the user activation.
    expect(env.requestPermission).toHaveBeenCalledTimes(1)
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    await pending
  })

  it('does not subscribe when the worker is not capable or the permission is refused', async () => {
    const closed = environment({ workerCapable: async () => false })
    setPushEnvironmentForTests(closed)
    await usePushStore().load()
    await usePushStore().enable()
    expect(closed.subscribe).not.toHaveBeenCalled()
    setActivePinia(createPinia())
    login()
    const refused = environment({ permission: () => 'default', requestPermission: vi.fn(async () => 'denied' as NotificationPermission) })
    setPushEnvironmentForTests(refused)
    await usePushStore().load()
    await usePushStore().enable()
    expect(refused.subscribe).not.toHaveBeenCalled()
    expect(registerPushSubscription).not.toHaveBeenCalled()
  })

  it('unsubscribes this browser only for a confirmed revoke of its own generation', async () => {
    const env = environment()
    setPushEnvironmentForTests(env)
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1), generation(G3, { current_session: false })))
    const s = usePushStore()
    await s.load()
    await s.enable()
    await flushPromises()
    // Another device's generation: this browser keeps its subscription and its marker.
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G3))
    await s.revoke(G3)
    await flushPromises()
    await markerIdle()
    expect(env.unsubscribe).not.toHaveBeenCalled()
    expect(backend.value?.generationId).toBe(G1)
    // An unconfirmed revoke of its own generation changes nothing locally.
    vi.mocked(revokePushSubscription).mockRejectedValueOnce(new Error('network'))
    await s.revoke(G1)
    await flushPromises()
    expect(env.unsubscribe).not.toHaveBeenCalled()
    // Confirmed.
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G1))
    await s.revoke(G1)
    await flushPromises()
    await markerIdle()
    expect(env.unsubscribe).toHaveBeenCalledTimes(1)
    expect(backend.value).toBeNull()
  })

  it('a failed unsubscribe is not reported as a failed revoke', async () => {
    setPushEnvironmentForTests(environment({ unsubscribe: async () => { throw new Error('browser') } }))
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G1))
    const s = usePushStore()
    await s.load()
    await s.enable()
    await flushPromises()
    await s.revoke(G1)
    await flushPromises()
    expect(s.notice).toBe('已关闭该设备的提醒。')
  })

  it('closes marked notifications at an identity boundary, best effort', async () => {
    const env = environment({ closeMarkedNotifications: vi.fn(async () => { throw new Error('no worker') }) })
    setPushEnvironmentForTests(env)
    usePushStore().reset()
    await flushPromises()
    expect(env.closeMarkedNotifications).toHaveBeenCalledTimes(1)
    usePushStore().reset(false)
    expect(env.closeMarkedNotifications).toHaveBeenCalledTimes(1)
  })
})

// ---------------------------------------------------------------- gate and states
describe('P2-5a closed gate and states', () => {
  it('stays closed without the server gate, never prompting on load', async () => {
    const env = environment()
    setPushEnvironmentForTests(env)
    vi.mocked(getPushConfig).mockResolvedValue({ subscribe_available: false, vapid_public_key: null, device_limit: 10 })
    const w = mount(PushDeviceCard)
    await flushPromises()
    expect(usePushStore().gate).toBe('closed')
    expect(w.get('[data-test="push-enable"]').attributes('disabled')).toBeDefined()
    expect(w.get('[data-test="push-gate"]').text()).toContain('尚未开放')
    await usePushStore().enable()
    expect(env.requestPermission).not.toHaveBeenCalled()
    expect(env.subscribe).not.toHaveBeenCalled()
    expect(registerPushSubscription).not.toHaveBeenCalled()
    w.unmount()
  })

  it('stays closed when no worker advertises push:v1 (the real P2-5a environment)', async () => {
    setPushEnvironmentForTests(environment({ workerCapable: async () => false }))
    const s = usePushStore()
    await s.load()
    expect(s.gate).toBe('closed')
  })

  it('reports unsupported, denied, offline and error without prompting', async () => {
    const env = environment({ supported: () => false })
    setPushEnvironmentForTests(env)
    const s = usePushStore()
    await s.load()
    expect(s.gate).toBe('unsupported')

    backend.value = { generationId: G1, marker: MARKER_1 }
    setPushEnvironmentForTests(environment({ permission: () => 'denied' }))
    await s.load()
    await markerIdle()
    expect(s.gate).toBe('denied')
    expect(backend.value).toBeNull()

    setPushEnvironmentForTests(environment())
    await s.load()
    s.setOnline(false)
    expect(s.gate).toBe('offline')
    s.setOnline(true)

    s.reset()
    vi.mocked(getPushConfig).mockRejectedValue(new ApiError('INTERNAL_ERROR', 'x', 500))
    await s.load()
    expect(s.gate).toBe('error')
    expect(env.requestPermission).not.toHaveBeenCalled()
  })

  it('renders the device list without endpoints and the count', async () => {
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1), generation(G3, { current_session: false, state: 'inactive' })))
    const w = mount(PushDeviceCard)
    await flushPromises()
    expect(w.findAll('[data-test="push-device"]')).toHaveLength(2)
    expect(w.text()).not.toContain('updates.push.services.mozilla.com')
    // The inactive old-key generation still occupies a slot.
    expect(w.get('[data-test="push-count"]').text()).toContain('已占用 2 / 10 个设备名额')
    w.unmount()
  })
})

// ---------------------------------------------------------------- registration
describe('P2-5a registration', () => {
  it('captures key and body, validates facts, stores the marker', async () => {
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    const s = usePushStore()
    await s.load()
    // The server's complete device list after the write includes the new generation.
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    await s.enable()
    await markerIdle()
    const [body, key] = vi.mocked(registerPushSubscription).mock.calls[0]
    expect(body).toEqual(BODY)
    expect(key).toMatch(/^[A-Za-z0-9_-]{16,128}$/)
    expect(s.device?.generationId).toBe(G1)
    expect(backend.value).toMatchObject({ generationId: G1, marker: MARKER_1 })
    expect(s.registerAction).toBeNull()
  })

  it('asks for permission only from enable, and a refusal clears the marker', async () => {
    const env = environment({ permission: () => 'default', requestPermission: vi.fn(async () => 'denied' as NotificationPermission) })
    setPushEnvironmentForTests(env)
    backend.value = { generationId: G1, marker: MARKER_1 }
    const s = usePushStore()
    await s.load()
    expect(env.requestPermission).not.toHaveBeenCalled()
    await s.enable()
    await markerIdle()
    expect(env.requestPermission).toHaveBeenCalledTimes(1)
    expect(registerPushSubscription).not.toHaveBeenCalled()
    expect(backend.value).toBeNull()
    expect(s.gate).toBe('denied')
  })

  it('unknown results keep the exact key and body; only an explicit retry resends', async () => {
    vi.mocked(registerPushSubscription)
      .mockRejectedValueOnce(new Error('lost'))
      .mockRejectedValueOnce(new ApiError('RATE_LIMITED', 'x', 429))
      .mockResolvedValueOnce(await registration(G1, MARKER_1))
    const s = usePushStore()
    await s.load()
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    await s.enable()
    expect(s.registerAction?.status).toBe('unknown')
    expect(registerPushSubscription).toHaveBeenCalledTimes(1)  // no automatic retry
    await s.retryRegister()
    expect(s.registerAction?.status).toBe('unknown')  // a 4xx after unknown stays unknown
    await s.retryRegister()
    const calls = vi.mocked(registerPushSubscription).mock.calls
    expect(calls[1]).toEqual(calls[0])
    expect(calls[2]).toEqual(calls[0])
    expect(s.device?.generationId).toBe(G1)
  })

  it('a first definitive rejection ends the action with a fixed message', async () => {
    vi.mocked(registerPushSubscription).mockRejectedValue(new ApiError('PUSH_DEVICE_LIMIT', 'x', 409))
    const s = usePushStore()
    await s.load()
    await s.enable()
    expect(s.registerAction).toBeNull()
    expect(s.notice).toContain('10 台设备')
    expect(backend.puts).toBe(0)
  })

  it.each([
    ['wrong endpoint hash', async () => { const r = await registration(G1, MARKER_1); r.generation.endpoint_hash = '0'.repeat(64); return r }],
    ['wrong key id', async () => { const r = await registration(G1, MARKER_1); r.generation.vapid_key_id = '0'.repeat(64); return r }],
    ['generation differs from command', async () => { const r = await registration(G1, MARKER_1); r.generation.id = G3; return r }],
    ['active without a marker', async () => registration(G1, null)],
    ['revoked with a marker', async () => registration(G1, MARKER_1, { state: 'revoked', revoked_reason: 'user', revoked_at: 'x' })],
    ['revoked without a reason', async () => registration(G1, null, { state: 'revoked' })],
    ['another session', async () => registration(G1, MARKER_1, { current_session: false })],
    ['unknown result', async () => { const r = await registration(G1, MARKER_1); (r.command as unknown as Record<string, unknown>).result = 'restored'; return r }],
  ])('malformed success facts stay unknown: %s', async (_name, make) => {
    vi.mocked(registerPushSubscription).mockResolvedValue(await make())
    const s = usePushStore()
    await s.load()
    await s.enable()
    await markerIdle()
    expect(s.registerAction?.status).toBe('unknown')
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
  })

  it('a replay whose generation is revoked turns off only that generation', async () => {
    const replay = await registration(G1, null, { state: 'revoked', revoked_reason: 'user', revoked_at: '2026-10-30T02:00:00Z' }, 'already_active')
    expect(isValidRegistrationResponse(replay, { endpointHash: await sha256Hex(BODY.endpoint), vapidKeyId: await sha256Hex(VAPID) })).toBe(true)
    vi.mocked(registerPushSubscription).mockResolvedValueOnce(await registration(G3, MARKER_3)).mockResolvedValueOnce(replay)
    const s = usePushStore()
    await s.load()
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(replay.generation, generation(G3)))
    await s.enable()
    await s.enable()
    await flushPromises()
    await markerIdle()
    expect(s.device?.generationId).toBe(G3)
    expect(backend.value).toMatchObject({ generationId: G3, marker: MARKER_3 })
    expect([...s.devices, ...s.history].find((g) => g.id === G1)?.state).toBe('revoked')
  })
})

// ---------------------------------------------------------------- ordering and identity
describe('P2-5a ordering and identity boundaries', () => {
  it('a late register result after an account change is discarded and writes no marker', async () => {
    const pending = deferred<PushRegistrationResponse>()
    vi.mocked(registerPushSubscription).mockReturnValue(pending.promise)
    const s = usePushStore()
    await s.load()
    const run = s.enable()
    await flushPromises()
    login('bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb')
    pending.resolve(await registration(G1, MARKER_1))
    await run
    await markerIdle()
    expect(s.device).toBeNull()
    expect(s.registerAction).toBeNull()
    expect(backend.puts).toBe(0)
  })

  it('a pending marker write followed by an identity reset ends with no marker', async () => {
    backend.gate = deferred<void>()
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    const s = usePushStore()
    await s.load()
    await s.enable()
    await flushPromises()
    expect(backend.puts).toBe(1)  // the write started and is blocked in storage
    useSessionStore().resetBusinessState()
    backend.gate.resolve()
    await markerIdle()
    expect(backend.value).toBeNull()
    expect(s.device).toBeNull()
  })

  it('a queued (not yet started) marker write captured before a clear is skipped', async () => {
    const gate = deferred<void>()
    backend.gate = gate
    const capture = captureMarker()
    await Promise.all([
      writeMarker({ generationId: G1, marker: MARKER_1 }, capture).then(() => undefined),
      (async () => {
        await flushPromises()
        backend.gate = null
        const late = writeMarker({ generationId: G3, marker: MARKER_3 }, capture)
        useSessionStore().resetBusinessState()
        gate.resolve()
        expect(await late).toBe(false)
      })(),
    ])
    await markerIdle()
    expect(backend.value).toBeNull()
  })

  it('logout clears the marker before the server call', async () => {
    backend.value = { generationId: G1, marker: MARKER_1 }
    let seenAtCall: MarkerRecord | null | undefined
    vi.mocked(authApi.deleteSession).mockImplementation(async () => { seenAtCall = await readMarker() })
    await useSessionStore().logout()
    expect(seenAtCall).toBeNull()
  })

  it('a late old revoke does not turn off or clear a newer generation', async () => {
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1, { current_session: false })))
    const pendingRevoke = deferred<PushRevocationResponse>()
    vi.mocked(revokePushSubscription).mockReturnValue(pendingRevoke.promise)
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G3, MARKER_3))
    const s = usePushStore()
    await s.load()
    const revoking = s.revoke(G1)
    await flushPromises()
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1, { current_session: false }), generation(G3)))
    await s.enable()
    await markerIdle()
    expect(s.device?.generationId).toBe(G3)
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(revocation(G1).generation, generation(G3)))
    pendingRevoke.resolve(revocation(G1))
    await revoking
    await flushPromises()
    await markerIdle()
    expect(s.device?.generationId).toBe(G3)
    expect(backend.value).toMatchObject({ generationId: G3, marker: MARKER_3 })
    expect([...s.devices, ...s.history].find((g) => g.id === G1)?.state).toBe('revoked')
  })

  it('revoking this device clears exactly its marker', async () => {
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G1, MARKER_1))
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G1))
    const s = usePushStore()
    await s.load()
    await s.enable()
    await markerIdle()
    await s.revoke(G1)
    await markerIdle()
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
  })

  it('a malformed revoke success stays unknown and changes nothing', async () => {
    const bad = revocation(G1)
    bad.revocation.generation_id = G3
    vi.mocked(revokePushSubscription).mockRejectedValueOnce(new Error('lost')).mockResolvedValueOnce(bad)
    const s = usePushStore()
    await s.load()
    await s.revoke(G1)
    expect(s.revokeActions[G1].status).toBe('unknown')
    await s.revoke(G1)
    expect(s.revokeActions[G1].status).toBe('unknown')
    expect(vi.mocked(revokePushSubscription).mock.calls).toEqual([[G1], [G1]])
  })

  it('a list that started before a confirmed write is discarded, not applied', async () => {
    const s = usePushStore()
    await s.load()
    const stale = deferred<PushSubscriptionList>()
    vi.mocked(listPushSubscriptions).mockReturnValueOnce(stale.promise)
    const reading = s.refresh()
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G3, MARKER_3))
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G3)))
    await s.enable()
    // The old list (G3 absent, G1 shown inactive) arrives after the write was confirmed.
    stale.resolve(list(generation(G1, { state: 'inactive' })))
    await reading
    await flushPromises()
    await markerIdle()
    expect(s.device?.generationId).toBe(G3)
    expect(s.devices.map((g) => g.id)).toEqual([G3])
    expect(backend.value).toMatchObject({ generationId: G3, marker: MARKER_3 })
  })

  it('an older list read never overwrites a newer one', async () => {
    const s = usePushStore()
    await s.load()
    const older = deferred<PushSubscriptionList>()
    vi.mocked(listPushSubscriptions).mockReturnValueOnce(older.promise).mockResolvedValueOnce(list(generation(G3)))
    const first = s.refresh()
    await s.refresh()
    older.resolve(list(generation(G1)))
    await first
    expect(s.devices.map((g) => g.id)).toEqual([G3])
  })

  it('a first load keeps a matching marker and clears a foreign one by generation', async () => {
    backend.value = { generationId: G1, marker: MARKER_1 }
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    const s = usePushStore()
    await s.load()
    await markerIdle()
    expect(backend.value).toMatchObject({ generationId: G1, marker: MARKER_1 })
    expect(s.device?.generationId).toBe(G1)

    s.reset(false)
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G3)))
    await s.load()
    await markerIdle()
    expect(backend.value).toBeNull()
  })

  it('a fresh authenticated bootstrap keeps the marker; an account switch clears it', async () => {
    setActivePinia(createPinia())
    backend.value = { generationId: G1, marker: MARKER_1 }
    login()
    await markerIdle()
    expect(backend.value).toMatchObject({ generationId: G1, marker: MARKER_1 })
    login('bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb')
    await markerIdle()
    expect(backend.value).toBeNull()
  })
})

// ---------------------------------------------------------------- review repairs R1 and R2
describe('P2-5a review R1: a confirmed revocation dominates late results', () => {
  async function activeDevice(): Promise<ReturnType<typeof usePushStore>> {
    backend.value = { generationId: G1, marker: MARKER_1 }
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    const s = usePushStore()
    await s.load()
    await markerIdle()
    expect(s.device?.generationId).toBe(G1)
    return s
  }

  it.each(['fails', 'stays pending'])('delayed active registration -> confirmed revoke -> late registration (refresh %s)', async (mode) => {
    const s = await activeDevice()
    const late = deferred<PushRegistrationResponse>()
    vi.mocked(registerPushSubscription).mockReturnValue(late.promise)
    const enabling = s.enable()
    await flushPromises()
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G1))
    if (mode === 'fails') vi.mocked(listPushSubscriptions).mockRejectedValue(new ApiError('INTERNAL_ERROR', 'x', 500))
    else vi.mocked(listPushSubscriptions).mockReturnValue(new Promise<PushSubscriptionList>(() => {}))
    await s.revoke(G1)
    await markerIdle()
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
    // The no-op registration's response (active G1 with its marker) arrives after the revoke.
    late.resolve(await registration(G1, MARKER_1, {}, 'already_active'))
    await enabling
    await flushPromises()
    await markerIdle()
    expect(s.registerAction).toBeNull()  // the command itself was confirmed
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
    expect(s.devices.find((g) => g.id === G1)).toBeUndefined()
    expect(s.history.find((g) => g.id === G1)?.state).toBe('revoked')
    expect(s.occupiedSlots).toBe(0)
    expect(s.notice).not.toContain('已在此设备开启提醒')
    expect(s.notice).not.toContain('此设备的提醒已经开启')
  })

  it('a later list or reconciliation showing the revoked generation unrevoked does not restore it', async () => {
    const s = await activeDevice()
    vi.mocked(revokePushSubscription).mockResolvedValue(revocation(G1))
    vi.mocked(listPushSubscriptions).mockReturnValue(new Promise<PushSubscriptionList>(() => {}))
    await s.revoke(G1)
    await markerIdle()
    // A stale view (and a marker that landed late, e.g. from another tab) claim G1 is active here.
    backend.value = { generationId: G1, marker: MARKER_1 }
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    await s.refresh()
    await markerIdle()
    expect(s.devices.find((g) => g.id === G1)).toBeUndefined()
    expect(s.history.find((g) => g.id === G1)?.state).toBe('revoked')
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
  })

  it('a reconciliation whose read was superseded during the marker read applies nothing', async () => {
    backend.value = { generationId: G1, marker: MARKER_1 }
    backend.getGate = deferred<void>()
    vi.mocked(listPushSubscriptions).mockResolvedValueOnce(list(generation(G1)))
    const s = usePushStore()
    const loading = s.load()
    await flushPromises()  // the load's list applied; its reconciliation waits on the marker read
    vi.mocked(listPushSubscriptions).mockResolvedValueOnce(list())  // newer read: G1 no longer occupies a slot
    const newer = s.refresh()
    await flushPromises()
    backend.getGate.resolve()
    backend.getGate = null
    await loading
    await newer
    await markerIdle()
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
  })

  it('an old G1 revoke still never clears G3 (generation-targeted)', async () => {
    const s = await activeDevice()
    const pendingRevoke = deferred<PushRevocationResponse>()
    vi.mocked(revokePushSubscription).mockReturnValue(pendingRevoke.promise)
    const revoking = s.revoke(G1)
    await flushPromises()
    vi.mocked(registerPushSubscription).mockResolvedValue(await registration(G3, MARKER_3))
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1), generation(G3)))
    await s.enable()
    await markerIdle()
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(revokedGeneration(G1), generation(G3)))
    pendingRevoke.resolve(revocation(G1))
    await revoking
    await flushPromises()
    await markerIdle()
    expect(s.device?.generationId).toBe(G3)
    expect(backend.value).toMatchObject({ generationId: G3, marker: MARKER_3 })
  })
})

describe('P2-5a review R2: complete device coverage', () => {
  const OLD = '44444444-4444-4444-8444-444444444444'
  function longHistory(): PushGeneration[] {
    return Array.from({ length: 20 }, (_, i) =>
      revokedGeneration(`${(0x50 + i).toString(16).padStart(8, '0')}-5555-4555-8555-555555555555`))
  }

  it('renders every slot-occupying device behind long history and keeps a matching marker', async () => {
    const history = longHistory()
    backend.value = { generationId: G1, marker: MARKER_1 }
    vi.mocked(listPushSubscriptions).mockResolvedValue({
      devices: [generation(G1), generation(OLD, { state: 'inactive', current_session: false })],
      history, occupied_slots: 2, device_limit: 10,
    })
    const w = mount(PushDeviceCard)
    await flushPromises()
    await markerIdle()
    const s = usePushStore()
    expect(s.listed).toBe(true)
    expect(w.findAll('[data-test="push-device"]')).toHaveLength(2)
    expect(w.findAll('[data-test="push-revoke"]')).toHaveLength(2)
    expect(w.get('[data-test="push-count"]').text()).toContain('已占用 2 / 10 个设备名额')
    expect(w.findAll('[data-test="push-history"] li')).toHaveLength(20)
    expect(w.find('[data-test="push-empty"]').exists()).toBe(false)
    expect(backend.value).toMatchObject({ generationId: G1, marker: MARKER_1 })
    expect(s.device?.generationId).toBe(G1)
    w.unmount()
  })

  it('shows no empty state when the list could not be read', async () => {
    vi.mocked(listPushSubscriptions).mockRejectedValue(new ApiError('INTERNAL_ERROR', 'x', 500))
    const w = mount(PushDeviceCard)
    await flushPromises()
    expect(w.find('[data-test="push-empty"]').exists()).toBe(false)
    expect(w.find('[data-test="push-count"]').exists()).toBe(false)
    w.unmount()
  })

  it('rejects malformed list contracts', async () => {
    const { isValidSubscriptionList } = await import('@/api/push')
    expect(isValidSubscriptionList(list(generation(G1)))).toBe(true)
    expect(isValidSubscriptionList({ ...list(generation(G1)), occupied_slots: 0 })).toBe(false)
    expect(isValidSubscriptionList({ devices: [revokedGeneration(G1)], history: [], occupied_slots: 1, device_limit: 10 })).toBe(false)
    expect(isValidSubscriptionList({ devices: [generation(G1)], history: [revokedGeneration(G1)], occupied_slots: 1, device_limit: 10 })).toBe(false)
    expect(isValidSubscriptionList({ generations: [], counted_devices: 0, device_limit: 10 })).toBe(false)
  })
})

describe('P2-5b: provider gone is not a user action', () => {
  const at = '2026-10-30T02:00:00.000000Z'
  function goneResponse(): PushRevocationResponse {
    return {
      revocation: { generation_id: G1, reason: 'gone', revoked_by: null, revoked_at: at },
      generation: generation(G1, { state: 'revoked', revoked_reason: 'gone', revoked_at: at }),
    }
  }

  it('a gone revocation has no human actor, and a human reason must have one', async () => {
    const { isValidRevocationResponse } = await import('@/api/push')
    expect(isValidRevocationResponse(goneResponse(), G1)).toBe(true)
    const withActor = goneResponse()
    withActor.revocation.revoked_by = USER
    expect(isValidRevocationResponse(withActor, G1)).toBe(false)
    const noActor = revocation(G1)
    noActor.revocation.revoked_by = null
    expect(isValidRevocationResponse(noActor, G1)).toBe(false)
  })

  it('a revoke retry that finds gone turns the device off and keeps it off', async () => {
    backend.value = { generationId: G1, marker: MARKER_1 }
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(generation(G1)))
    const s = usePushStore()
    await s.load()
    await markerIdle()
    expect(s.device?.generationId).toBe(G1)
    vi.mocked(revokePushSubscription).mockResolvedValue(goneResponse())
    vi.mocked(listPushSubscriptions).mockReturnValue(new Promise<PushSubscriptionList>(() => {}))
    await s.revoke(G1)
    await markerIdle()
    expect(s.device).toBeNull()
    expect(backend.value).toBeNull()
    expect(s.history.find((g) => g.id === G1)?.revoked_reason).toBe('gone')
  })

  it('a listed gone generation is shown as a provider report, not as closed by the user', async () => {
    vi.mocked(listPushSubscriptions).mockResolvedValue(list(goneResponse().generation))
    const w = mount(PushDeviceCard)
    await flushPromises()
    expect(w.get('[data-test="push-history"]').text()).toContain('推送服务报告此订阅已失效')
    expect(w.find('[data-test="push-device"]').exists()).toBe(false)
    w.unmount()
  })
})
