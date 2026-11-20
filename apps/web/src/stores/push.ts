/**
 * P2-5a/c device push store (docs/phase2-web-push.md Sections 4.5, 7.1 and 16). Independent of every
 * other store; no automatic retries and no offline queue.
 *
 * Ordering rules (a late response never undoes newer state):
 * - Identity: every action and read captures the identity epoch and the account; a result that
 *   arrives after a reset or account change is discarded.
 * - Writes: one registration action at a time and at most one revoke action per generation.
 * - Reads: a list applies only if it is the newest read (checked again after the asynchronous
 *   marker read of reconciliation). Every confirmed write starts a new read immediately, so a
 *   list that was in flight during that write is superseded and never applied.
 * - Revocation dominates: a revocation is immutable, so every revocation the store confirms or
 *   observes (revoke response, revoked replay, list) is remembered per generation for the
 *   identity. A later registration response, list or reconciliation that shows that generation
 *   unrevoked is older than that knowledge and is overlaid as revoked: it never restores the
 *   device, its marker or an "enabled" notice. This does not depend on any follow-up refresh.
 * - Command facts vs state: a confirmed registration always ends its action (the command
 *   succeeded), while the device state comes only from the (overlaid) current generation.
 * - Generation targets: the device generation changes only on a confirmed ACTIVE, not-known-revoked
 *   registration of an operation not older than the one that set it; it turns off only for that
 *   exact generation, and the stored marker is only cleared for that exact generation
 *   (clearMarkerFor), so an old G1 revoke can never clear G3.
 * - List coverage: `devices` is the complete set of slot-occupying generations (server
 *   contract), so absence from it is authoritative for the device and the marker; `history` is
 *   bounded and never used as evidence.
 * - Marker writes capture the marker state at action start: a logout, an identity reset or a clear
 *   in any other tab or the service worker in between makes the write a no-op (push-marker.ts).
 * - Current device only: a confirmed revoke of THIS device's generation also unsubscribes this
 *   browser; revoking another device's generation never touches this browser's subscription.
 * Unknown results keep the exact original key and body; only an explicit retry resends them.
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import {
  getPushConfig, isValidConfig, isValidRegistrationResponse, isValidRevocationResponse, isValidSubscriptionList,
  listPushSubscriptions, registerPushSubscription, revokePushSubscription, sha256Hex,
} from '@/api/push'
import type { CapturedRegistration, PushConfig, PushGeneration, PushRegistrationBody } from '@/api/push'
import { newIdempotencyKey } from '@/api/tasks'
import { pushEnvironment } from '@/pwa/push-env'
import type { PermissionState } from '@/pwa/push-env'
import { captureMarker, clearMarker, clearMarkerFor, primeMarkerFence, readMarker, writeMarker } from '@/pwa/push-marker'
import type { MarkerCapture } from '@/pwa/push-marker'
import { useSessionStore } from './session'

type ActionStatus = 'submitting' | 'unknown' | 'not_sent'

interface RegisterAction {
  seq: number
  key: string
  body: PushRegistrationBody
  captured: CapturedRegistration
  owner: string
  identity: number
  capture: MarkerCapture
  status: ActionStatus
  error: string
  unresolved: boolean
}

interface RevokeAction {
  seq: number
  generationId: string
  owner: string
  identity: number
  status: ActionStatus
  error: string
  unresolved: boolean
}

export type PushGate = 'loading' | 'error' | 'unsupported' | 'closed' | 'denied' | 'offline' | 'available'

export const usePushStore = defineStore('push', () => {
  const config = ref<PushConfig | null>(null)
  const supported = ref(false)
  const permission = ref<PermissionState>('default')
  const workerReady = ref(false)
  const online = ref(typeof navigator === 'undefined' ? true : navigator.onLine !== false)
  const loadError = ref('')
  /** Complete: every generation occupying a cap slot. */
  const devices = ref<PushGeneration[]>([])
  /** Bounded: newest revoked or session-ended generations. */
  const history = ref<PushGeneration[]>([])
  const occupiedSlots = ref(0)
  const deviceLimit = ref(10)
  const listed = ref(false)
  const registerAction = ref<RegisterAction | null>(null)
  const revokeActions = ref<Record<string, RevokeAction>>({})
  const notice = ref('')
  /** The generation confirmed active for THIS device, and the operation that set it. */
  const device = ref<{ generationId: string; seq: number } | null>(null)

  let identity = 0
  let opSeq = 0
  let readSeq = 0
  let loadSeq = 0
  /** Immutable revocations confirmed or observed for this identity, by generation id. */
  let revokedKnown = new Map<string, { reason: PushGeneration['revoked_reason']; revokedAt: string | null }>()

  const gate = computed<PushGate>(() => {
    if (!supported.value) return 'unsupported'
    if (loadError.value && !config.value) return 'error'
    if (!config.value) return 'loading'
    if (!config.value.subscribe_available || !workerReady.value) return 'closed'
    if (permission.value === 'denied') return 'denied'
    if (!online.value) return 'offline'
    return 'available'
  })

  function owner(): string | null {
    return useSessionStore().user?.id ?? null
  }

  /** Identity boundary: drop everything; clear the device marker unless told this is a first load. */
  function reset(clearDevice = true): void {
    identity += 1
    config.value = null
    workerReady.value = false
    loadError.value = ''
    devices.value = []
    history.value = []
    occupiedSlots.value = 0
    revokedKnown = new Map()
    listed.value = false
    registerAction.value = null
    revokeActions.value = {}
    notice.value = ''
    device.value = null
    if (clearDevice) {
      void clearMarker()
      void pushEnvironment().closeMarkedNotifications().catch(() => undefined)
    }
  }

  function setOnline(value: boolean): void {
    online.value = value
  }

  /** Records a revocation, or overlays known revocation knowledge onto an older view. */
  function settle(g: PushGeneration): PushGeneration {
    const clean: PushGeneration = { ...g }
    delete clean.endpoint_hash
    delete clean.vapid_key_id
    if (clean.state === 'revoked') {
      if (!revokedKnown.has(clean.id)) revokedKnown.set(clean.id, { reason: clean.revoked_reason, revokedAt: clean.revoked_at })
      return clean
    }
    const known = revokedKnown.get(clean.id)
    return known ? { ...clean, state: 'revoked', revoked_reason: known.reason, revoked_at: known.revokedAt } : clean
  }

  function isRevokedKnown(id: string): boolean {
    return revokedKnown.has(id)
  }

  /** One confirmed generation from a write response: moves between devices and history. */
  function applyGeneration(g: PushGeneration): PushGeneration {
    const settled = settle(g)
    devices.value = devices.value.filter((item) => item.id !== settled.id)
    history.value = history.value.filter((item) => item.id !== settled.id)
    if (settled.state === 'revoked') history.value.unshift(settled)
    else devices.value.unshift(settled)
    occupiedSlots.value = devices.value.length
    return settled
  }

  /** Device generation turns off only for that exact generation. */
  function deviceOff(generationId: string): void {
    if (device.value?.generationId === generationId) device.value = null
    void clearMarkerFor(generationId)
  }

  async function load(): Promise<void> {
    const started = identity, seq = ++loadSeq, account = owner()
    const env = pushEnvironment()
    supported.value = env.supported()
    permission.value = supported.value ? env.permission() : 'unsupported'
    if (permission.value === 'denied') void clearMarker()
    loadError.value = ''
    try {
      const cfg = await getPushConfig()
      if (started !== identity || seq !== loadSeq || account !== owner()) return
      if (!isValidConfig(cfg)) throw new Error('malformed config')
      const capable = cfg.subscribe_available && supported.value ? await env.workerCapable() : false
      if (started !== identity || seq !== loadSeq || account !== owner()) return
      config.value = cfg
      workerReady.value = capable
    } catch (error) {
      if (started !== identity || seq !== loadSeq) return
      loadError.value = error instanceof ApiError || error instanceof OfflineMutationBlockedError
        ? describeError(error) : '暂时无法读取设备提醒设置。'
    }
    await refresh()
  }

  /** A generation this device may hold: occupying a slot, active, this session, not known revoked. */
  function holdable(id: string): boolean {
    const g = devices.value.find((item) => item.id === id)
    return !!g && g.state === 'active' && g.current_session && !isRevokedKnown(id)
  }

  async function refresh(): Promise<void> {
    const started = identity, seq = ++readSeq, account = owner()
    const fresh = () => started === identity && seq === readSeq && account === owner()
    primeMarkerFence()
    try {
      const list = await listPushSubscriptions()
      if (!fresh()) return
      if (!isValidSubscriptionList(list)) throw new Error('malformed list')
      const settled = [...list.devices, ...list.history].map(settle)
      devices.value = settled.filter((g, i) => i < list.devices.length && g.state !== 'revoked')
      history.value = settled.filter((g, i) => i >= list.devices.length || g.state === 'revoked')
      occupiedSlots.value = devices.value.length
      deviceLimit.value = list.device_limit
      listed.value = true
      // `devices` is complete, so a device generation missing from it occupies no slot: off.
      const dev = device.value
      if (dev && !holdable(dev.generationId)) deviceOff(dev.generationId)
      await reconcileMarker(fresh)
    } catch (error) {
      if (started !== identity || seq !== readSeq) return
      loadError.value = error instanceof ApiError || error instanceof OfflineMutationBlockedError
        ? describeError(error) : '暂时无法读取设备列表。'
    }
  }

  /**
   * The stored marker must belong to an active, slot-occupying generation of THIS session of THIS
   * account that is not known revoked; otherwise (another account's, revoked, ended session) it is
   * cleared for exactly that generation. The read sequence is checked again after the
   * asynchronous marker read, so a newer read or write is never undone by this one.
   */
  async function reconcileMarker(fresh: () => boolean): Promise<void> {
    const stored = await readMarker()
    if (!stored || !fresh()) return
    if (holdable(stored.generationId)) {
      if (!device.value) device.value = { generationId: stored.generationId, seq: 0 }
      return
    }
    deviceOff(stored.generationId)
  }

  async function sendRegister(action: RegisterAction): Promise<void> {
    const session = useSessionStore()
    const current = () => action.identity === identity && registerAction.value === action && session.user?.id === action.owner
    action.status = 'submitting'
    let sent = false
    try {
      await session.ensureCsrfToken()
      if (!current()) return
      if (!getCsrfToken()) throw new OfflineMutationBlockedError()
      sent = true
      const response = await registerPushSubscription(action.body, action.key)
      if (!current()) return
      if (!isValidRegistrationResponse(response, action.captured)) throw new Error('unconfirmed response')
      // The command is confirmed: the action ends whatever the generation's current state is.
      registerAction.value = null
      const g = applyGeneration(response.generation)
      if (g.state === 'active' && response.marker) {
        if (!device.value || device.value.seq <= action.seq) {
          device.value = { generationId: g.id, seq: action.seq }
          void writeMarker({ generationId: g.id, marker: response.marker, activatedAt: g.activated_at }, action.capture)
        }
        notice.value = response.command.result === 'created' ? '已在此设备开启提醒。' : '此设备的提醒已经开启。'
      } else {
        // Revoked (as reported, or already known locally) or inactive: only that generation.
        deviceOff(g.id)
        notice.value = '这次开启请求已记录，但对应的设备提醒已关闭，可以重新开启。'
      }
      void refresh()
    } catch (error) {
      if (!current()) return
      if (!action.unresolved && error instanceof ApiError && error.status >= 400 && error.status < 500) {
        registerAction.value = null
        notice.value = describeError(error)
        return
      }
      const unsent = !sent || error instanceof OfflineMutationBlockedError
      action.unresolved ||= !unsent
      action.status = action.unresolved ? 'unknown' : 'not_sent'
      action.error = action.unresolved ? '结果未确认，请用原请求重试。' : '尚未发送，请重试。'
    }
  }

  /** User action only: asks for permission (never on load), subscribes, captures, submits. */
  async function enable(): Promise<void> {
    if (registerAction.value || gate.value !== 'available' || !config.value?.vapid_public_key) return
    // Nothing is awaited before the prompt: the user's gesture must still count when it opens.
    const started = identity, account = owner(), capture = captureMarker(), vapid = config.value.vapid_public_key
    if (!account) return
    const env = pushEnvironment()
    const stale = () => started !== identity || account !== owner()
    notice.value = ''
    let granted = env.permission()
    if (granted === 'default') granted = await env.requestPermission()
    if (stale()) return
    permission.value = granted
    if (granted !== 'granted') {
      if (granted === 'denied') void clearMarker()
      notice.value = '没有获得通知权限，无法开启设备提醒。'
      return
    }
    let body: PushRegistrationBody
    let captured: CapturedRegistration
    try {
      body = await env.subscribe(vapid)
      captured = { endpointHash: await sha256Hex(body.endpoint), vapidKeyId: await sha256Hex(vapid) }
    } catch {
      if (!stale()) notice.value = '无法在此浏览器创建订阅。'
      return
    }
    if (stale() || registerAction.value) return
    const action: RegisterAction = {
      seq: ++opSeq, key: newIdempotencyKey(),
      body: { endpoint: body.endpoint, keys: { p256dh: body.keys.p256dh, auth: body.keys.auth } },
      captured, owner: account, identity: started, capture, status: 'submitting', error: '', unresolved: false,
    }
    registerAction.value = action
    // Compare and mutate through the reactive proxy the store holds.
    await sendRegister(registerAction.value!)
  }

  async function retryRegister(): Promise<void> {
    const action = registerAction.value
    if (action && action.status !== 'submitting') await sendRegister(action)
  }

  async function sendRevoke(action: RevokeAction): Promise<void> {
    const session = useSessionStore()
    const current = () => action.identity === identity && revokeActions.value[action.generationId] === action &&
      session.user?.id === action.owner
    action.status = 'submitting'
    let sent = false
    try {
      await session.ensureCsrfToken()
      if (!current()) return
      if (!getCsrfToken()) throw new OfflineMutationBlockedError()
      sent = true
      const response = await revokePushSubscription(action.generationId)
      if (!current()) return
      if (!isValidRevocationResponse(response, action.generationId)) throw new Error('unconfirmed response')
      delete revokeActions.value[action.generationId]
      const thisDevice = device.value?.generationId === action.generationId
      applyGeneration(response.generation)
      deviceOff(action.generationId)
      // Best effort: the server already refuses this generation whatever happens here.
      if (thisDevice) void pushEnvironment().unsubscribe().catch(() => undefined)
      notice.value = '已关闭该设备的提醒。'
      void refresh()
    } catch (error) {
      if (!current()) return
      if (!action.unresolved && error instanceof ApiError && error.status >= 400 && error.status < 500) {
        delete revokeActions.value[action.generationId]
        notice.value = describeError(error)
        return
      }
      const unsent = !sent || error instanceof OfflineMutationBlockedError
      action.unresolved ||= !unsent
      action.status = action.unresolved ? 'unknown' : 'not_sent'
      action.error = action.unresolved ? '结果未确认，请重试。' : '尚未发送，请重试。'
    }
  }

  async function revoke(generationId: string): Promise<void> {
    const account = owner()
    if (!account) return
    const existing = revokeActions.value[generationId]
    if (existing) {
      if (existing.status !== 'submitting') await sendRevoke(existing)
      return
    }
    const action: RevokeAction = {
      seq: ++opSeq, generationId, owner: account, identity, status: 'submitting', error: '', unresolved: false,
    }
    revokeActions.value[generationId] = action
    await sendRevoke(revokeActions.value[generationId])
  }

  return {
    config, supported, permission, workerReady, online, loadError, devices, history, occupiedSlots, deviceLimit,
    listed, registerAction, revokeActions, notice, device, gate,
    reset, setOnline, load, refresh, enable, retryRegister, revoke,
  }
})
