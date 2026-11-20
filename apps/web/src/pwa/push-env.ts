/**
 * P2-5a/c browser push environment. The only place that touches `Notification`, `PushManager`
 * and `navigator.serviceWorker`; tests replace it with mocks (setPushEnvironmentForTests).
 *
 * Gate: real opt-in needs an ACTIVE service worker that answers with the `push:v1` capability. A
 * waiting worker does not count, and the server gate is separate. The permission prompt is only
 * ever shown from an explicit user action, never on page load.
 */
import type { PushRegistrationBody } from '@/api/push'
import { PUSH_CAPABILITY } from './push-sw'

export type PermissionState = NotificationPermission | 'unsupported'

export interface PushEnvironment {
  supported(): boolean
  permission(): PermissionState
  requestPermission(): Promise<NotificationPermission>
  workerCapable(): Promise<boolean>
  subscribe(vapidPublicKey: string): Promise<PushRegistrationBody>
  /** This browser's subscription only. A revoke of another device never calls this. */
  unsubscribe(): Promise<void>
  /** Best effort at a session's end: closes shown notifications that carry a marker. */
  closeMarkedNotifications(): Promise<void>
}

const CAPABILITY_TIMEOUT_MS = 1500

function base64UrlToBytes(text: string): Uint8Array {
  const padded = text.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (text.length % 4)) % 4)
  const raw = atob(padded)
  return Uint8Array.from(raw, (c) => c.charCodeAt(0))
}

/** Asks the ACTIVE worker. No answer in time, or any other answer, is "not capable". */
function askCapabilities(worker: ServiceWorker): Promise<boolean> {
  return new Promise((resolve) => {
    const channel = new MessageChannel()
    const timer = setTimeout(() => { channel.port1.close(); resolve(false) }, CAPABILITY_TIMEOUT_MS)
    channel.port1.onmessage = (event: MessageEvent) => {
      clearTimeout(timer)
      channel.port1.close()
      const data = event.data as { type?: unknown; capabilities?: unknown } | null
      resolve(data?.type === 'CAPABILITIES' && Array.isArray(data.capabilities) && data.capabilities.includes(PUSH_CAPABILITY))
    }
    worker.postMessage({ type: 'CAPABILITIES' }, [channel.port2])
  })
}

const browserEnvironment: PushEnvironment = {
  supported: () =>
    typeof window !== 'undefined' && 'Notification' in window && 'PushManager' in window &&
    typeof navigator !== 'undefined' && 'serviceWorker' in navigator,
  permission: () => (typeof Notification === 'undefined' ? 'unsupported' : Notification.permission),
  requestPermission: () => Notification.requestPermission(),
  workerCapable: async () => {
    const registration = await navigator.serviceWorker.getRegistration()
    return registration?.active ? askCapabilities(registration.active) : false
  },
  subscribe: async (vapidPublicKey) => {
    const registration = await navigator.serviceWorker.ready
    const subscription = await registration.pushManager.subscribe({
      userVisibleOnly: true,
      applicationServerKey: base64UrlToBytes(vapidPublicKey) as BufferSource,
    })
    const json = subscription.toJSON()
    if (!json.endpoint || !json.keys?.p256dh || !json.keys?.auth) throw new Error('incomplete subscription')
    return { endpoint: json.endpoint, keys: { p256dh: json.keys.p256dh, auth: json.keys.auth } }
  },
  unsubscribe: async () => {
    const registration = await navigator.serviceWorker.getRegistration()
    const subscription = await registration?.pushManager.getSubscription()
    await subscription?.unsubscribe()
  },
  closeMarkedNotifications: async () => {
    const registration = await navigator.serviceWorker.getRegistration()
    const shown = (await registration?.getNotifications()) ?? []
    for (const notification of shown) {
      if ((notification.data as { marker?: unknown } | null)?.marker) notification.close()
    }
  },
}

let environment: PushEnvironment = browserEnvironment

export function pushEnvironment(): PushEnvironment {
  return environment
}

/** Tests only: inject mocked browser APIs (null restores the real environment). */
export function setPushEnvironmentForTests(next: PushEnvironment | null): void {
  environment = next ?? browserEnvironment
}
