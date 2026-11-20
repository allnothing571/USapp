/**
 * P2-5c service worker push behaviour (docs/phase2-web-push.md Sections 7.2 and 16). No workbox and no
 * business API here: this code only shows fixed labels and opens same-origin pages. It never marks
 * anything read and never writes to the API. Every push shows a notification (userVisibleOnly).
 *
 * Duplicates are possible (at-least-once delivery). The tag makes them collapse where the browser
 * honours it, which is best effort, never exactly-once.
 */
import { eventLabel, targetPath, type NotificationTargetKind } from '../api/notification-labels'
import type { MarkerRecord } from './push-marker'

export const PUSH_CAPABILITY = 'push:v1'
export const GENERIC_TITLE = 'USapp'
export const GENERIC_BODY = '你有一条新通知'
export const GENERIC_TAG = 'usapp-generic'
export const FALLBACK_PATH = '/notifications'
const MAX_PAYLOAD_BYTES = 1024

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
const MARKER = /^[A-Za-z0-9_-]{22}$/
const ALLOWED_PATH = new RegExp(`^(/tasks|/rewards|/notifications|/us/(messages|anniversaries|memories|plans|albums)/${UUID.source.slice(1, -1)})$`)
const KINDS: ReadonlySet<string> = new Set(['task', 'reward', 'anniversary', 'message', 'memory', 'shared_plan', 'album', 'account'])
const FIELDS = ['event_type', 'marker', 'nid', 'target_id', 'target_kind', 'v']

export interface PushPayload {
  v: 1
  nid: string
  event_type: string
  target_kind: NotificationTargetKind
  target_id: string
  marker: string
}

export interface NotificationPlan {
  title: string
  options: { body: string; tag: string; data: { nid?: string; target: string; marker?: string } }
}

/** The exact payload, or null. Anything else (more fields included) never reaches a label. */
export function parsePushPayload(text: string | null | undefined): PushPayload | null {
  if (typeof text !== 'string' || new TextEncoder().encode(text).length > MAX_PAYLOAD_BYTES) return null
  let value: unknown
  try {
    value = JSON.parse(text)
  } catch {
    return null
  }
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return null
  const p = value as Record<string, unknown>
  if (Object.keys(p).sort().join() !== FIELDS.join()) return null
  if (p.v !== 1 || typeof p.nid !== 'string' || !UUID.test(p.nid) || typeof p.target_id !== 'string' || !UUID.test(p.target_id)) return null
  if (typeof p.target_kind !== 'string' || !KINDS.has(p.target_kind)) return null
  if (typeof p.event_type !== 'string' || eventLabel(p.event_type) === null) return null
  if (typeof p.marker !== 'string' || !MARKER.test(p.marker)) return null
  return p as unknown as PushPayload
}

const GENERIC: NotificationPlan = {
  title: GENERIC_TITLE,
  options: { body: GENERIC_BODY, tag: GENERIC_TAG, data: { target: FALLBACK_PATH } },
}

/** A label only for this device's current generation; everything else is the generic text. */
export function notificationFor(payload: PushPayload | null, stored: MarkerRecord | null): NotificationPlan {
  if (!payload || !stored || stored.marker !== payload.marker) return GENERIC
  const label = eventLabel(payload.event_type)
  if (label === null) return GENERIC
  return {
    title: GENERIC_TITLE,
    options: {
      body: label,
      tag: payload.nid,
      data: { nid: payload.nid, target: targetPath(payload.target_kind, payload.target_id), marker: payload.marker },
    },
  }
}

/** Decided at click time against the marker stored NOW; only allowlisted same-origin paths. */
export function destinationFor(data: unknown, current: MarkerRecord | null): string {
  if (typeof data !== 'object' || data === null) return FALLBACK_PATH
  const d = data as { target?: unknown; marker?: unknown }
  if (typeof d.marker !== 'string' || !current || current.marker !== d.marker) return FALLBACK_PATH
  return typeof d.target === 'string' && ALLOWED_PATH.test(d.target) ? d.target : FALLBACK_PATH
}

// ---------------------------------------------------------------- the minimal worker shape
export interface WindowClientLike {
  readonly url: string
  focus(): Promise<WindowClientLike>
  navigate(url: string): Promise<WindowClientLike | null>
}
interface ExtendableEventLike {
  waitUntil(promise: Promise<unknown>): void
}
export interface PushScope {
  readonly location: { readonly origin: string }
  readonly registration: { showNotification(title: string, options: NotificationPlan['options']): Promise<void> }
  readonly clients: {
    matchAll(options: { type: 'window'; includeUncontrolled: boolean }): Promise<readonly WindowClientLike[]>
    openWindow(url: string): Promise<unknown>
  }
  addEventListener(type: string, listener: (event: never) => void): void
}
export interface PushWorkerDeps {
  readMarker(): Promise<MarkerRecord | null>
  clearMarker(): Promise<void>
}

async function storedMarker(deps: PushWorkerDeps): Promise<MarkerRecord | null> {
  try {
    return await deps.readMarker()
  } catch {
    return null  // unreadable storage is "no marker": the generic text
  }
}

/** Focuses a same-origin window and navigates it, so that the page establishes its own session. */
async function openDestination(scope: PushScope, path: string): Promise<void> {
  const url = new URL(path, scope.location.origin).href
  const windows = await scope.clients.matchAll({ type: 'window', includeUncontrolled: true })
  for (const client of windows) {
    if (new URL(client.url).origin !== scope.location.origin) continue
    try {
      const focused = await client.focus()
      if (await focused.navigate(url)) return
    } catch {
      // An uncontrolled client cannot be navigated; try the next, then a new window.
    }
  }
  await scope.clients.openWindow(url)
}

export function installPushHandlers(scope: PushScope, deps: PushWorkerDeps): void {
  scope.addEventListener('push', (event: ExtendableEventLike & { data?: { text(): string } | null }) => {
    event.waitUntil((async () => {
      let text: string | null = null
      try {
        text = event.data ? event.data.text() : null
      } catch {
        text = null
      }
      const plan = notificationFor(parsePushPayload(text), await storedMarker(deps))
      await scope.registration.showNotification(plan.title, plan.options)
    })())
  })

  scope.addEventListener('notificationclick', (event: ExtendableEventLike & { notification: { data: unknown; close(): void } }) => {
    event.notification.close()
    event.waitUntil((async () => {
      await openDestination(scope, destinationFor(event.notification.data, await storedMarker(deps)))
    })())
  })

  // The old association is stale. No prompt and no background registration write: the user re-enables.
  scope.addEventListener('pushsubscriptionchange', (event: ExtendableEventLike) => {
    event.waitUntil(deps.clearMarker())
  })

  scope.addEventListener('message', (event: { data: unknown; ports?: readonly { postMessage(message: unknown): void }[] }) => {
    const data = event.data
    if (data && typeof data === 'object' && (data as { type?: unknown }).type === 'CAPABILITIES') {
      event.ports?.[0]?.postMessage({ type: 'CAPABILITIES', capabilities: [PUSH_CAPABILITY] })
    }
  })
}
