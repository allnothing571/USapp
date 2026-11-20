import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { eventLabel } from '@/api/notification-labels'
import { notificationLabel, notificationTarget } from '@/api/notifications'
import {
  destinationFor, FALLBACK_PATH, GENERIC_BODY, GENERIC_TAG, installPushHandlers, notificationFor, parsePushPayload,
} from '@/pwa/push-sw'
import type { PushPayload, PushScope, WindowClientLike } from '@/pwa/push-sw'
import type { MarkerRecord } from '@/pwa/push-marker'

const NID = '11111111-1111-4111-8111-111111111111'
const TARGET = '33333333-3333-4333-8333-333333333333'
const MARKER = 'm'.repeat(22)
const OTHER = 'n'.repeat(22)
const ORIGIN = 'https://usapp.test'
const payload = (over: Record<string, unknown> = {}): PushPayload => ({
  v: 1, nid: NID, event_type: 'message.created', target_kind: 'message', target_id: TARGET, marker: MARKER, ...over,
}) as PushPayload
const stored = (marker = MARKER): MarkerRecord => ({ generationId: 'g', marker })

describe('payload validation', () => {
  it('accepts exactly the agreed shape', () => {
    expect(parsePushPayload(JSON.stringify(payload()))).toEqual(payload())
  })

  it.each([
    ['null', null], ['empty', ''], ['not json', '{'], ['array', '[]'], ['number', '1'],
    ['oversized', JSON.stringify(payload({ pad: 'x'.repeat(2000) }))],
    ['a message field', JSON.stringify(payload({ message: 'secret words' }))],
    ['a title field', JSON.stringify(payload({ title: 'secret' }))],
    ['a body field', JSON.stringify(payload({ body: 'secret' }))],
    ['a missing field', JSON.stringify({ v: 1, nid: NID })],
    ['another version', JSON.stringify(payload({ v: 2 }))],
    ['a bad id', JSON.stringify(payload({ nid: 'x' }))],
    ['a path in the target id', JSON.stringify(payload({ target_id: '../../evil' }))],
    ['an uppercase id', JSON.stringify(payload({ target_id: TARGET.toUpperCase().replace('33', 'AB') }))],
    ['an unknown kind', JSON.stringify(payload({ target_kind: 'wallet' }))],
    ['an unknown event', JSON.stringify(payload({ event_type: 'wallet.drained' }))],
    ['a prototype event', JSON.stringify(payload({ event_type: 'constructor' }))],
    ['a bad marker', JSON.stringify(payload({ marker: 'short' }))],
  ])('refuses %s', (_name, text) => {
    expect(parsePushPayload(text)).toBeNull()
  })
})

describe('what a notification may show', () => {
  it('shows the fixed event label for this generation, with the tag as the notice id', () => {
    const plan = notificationFor(payload(), stored())
    expect(plan.options.body).toBe(eventLabel('message.created'))
    expect(plan.options.tag).toBe(NID)
    expect(plan.options.data).toEqual({ nid: NID, target: `/us/messages/${TARGET}`, marker: MARKER })
  })

  it.each([
    ['no payload', null, stored()],
    ['no stored marker', payload(), null],
    ['another generation', payload(), stored(OTHER)],
  ])('falls back to the generic notification with %s', (_name, p, s) => {
    const plan = notificationFor(p, s)
    expect(plan.options.body).toBe(GENERIC_BODY)
    expect(plan.options.tag).toBe(GENERIC_TAG)
    expect(plan.options.data).toEqual({ target: FALLBACK_PATH })
  })

  it('the inbox and the worker share one table and one target rule', () => {
    const item = { id: NID, event_type: 'task.published', actor_id: 'a', target_kind: 'anniversary', target_id: TARGET, created_at: '', read_at: null } as const
    expect(notificationLabel(item)).toBe(eventLabel('task.published'))
    expect(notificationLabel({ ...item, event_type: 'unknown.event' })).toBe('收到一条通知')
    expect(notificationTarget(item)).toBe(`/us/anniversaries/${TARGET}`)
  })
})

describe('click destination', () => {
  const data = { nid: NID, target: `/us/messages/${TARGET}`, marker: MARKER }

  it('follows the target only when the marker stored at click time still equals', () => {
    expect(destinationFor(data, stored())).toBe(`/us/messages/${TARGET}`)
    expect(destinationFor({ ...data, target: '/tasks' }, stored())).toBe('/tasks')
    expect(destinationFor({ ...data, target: '/rewards' }, stored())).toBe('/rewards')
    expect(destinationFor({ ...data, target: `/us/anniversaries/${TARGET}` }, stored())).toBe(`/us/anniversaries/${TARGET}`)
  })

  it('goes only to the inbox after an account change or logout', () => {
    expect(destinationFor(data, stored(OTHER))).toBe(FALLBACK_PATH)
    expect(destinationFor(data, null)).toBe(FALLBACK_PATH)
    expect(destinationFor({ target: FALLBACK_PATH }, stored())).toBe(FALLBACK_PATH)
    expect(destinationFor(null, stored())).toBe(FALLBACK_PATH)
  })

  it.each([
    'https://evil.example/', '//evil.example/x', '/\\evil.example', '/us/messages/../../admin', '/us/messages/not-a-uuid',
    `/us/messages/${TARGET}/x`, `/us/messages/${TARGET}?next=//evil`, '/admin', 'javascript:alert(1)', '',
  ])('never follows the target %s', (target) => {
    expect(destinationFor({ ...data, target }, stored())).toBe(FALLBACK_PATH)
  })
})

describe('installed handlers', () => {
  type Listener = (event: never) => void
  let listeners: Record<string, Listener>
  let waits: Promise<unknown>[]
  let show: ReturnType<typeof vi.fn>
  let openWindow: ReturnType<typeof vi.fn>
  let windows: WindowClientLike[]
  let marker: MarkerRecord | null
  let clear: ReturnType<typeof vi.fn>
  const fetchSpy = vi.fn()

  beforeEach(() => {
    listeners = {}
    waits = []
    show = vi.fn(async () => undefined)
    openWindow = vi.fn(async () => undefined)
    windows = []
    marker = stored()
    clear = vi.fn(async () => { marker = null })
    vi.stubGlobal('fetch', fetchSpy)
    fetchSpy.mockClear()
    const scope: PushScope = {
      location: { origin: ORIGIN },
      registration: { showNotification: show as never },
      clients: { matchAll: async () => windows, openWindow: openWindow as never },
      addEventListener: (type, listener) => { listeners[type] = listener },
    }
    installPushHandlers(scope, { readMarker: async () => marker, clearMarker: clear as never })
  })
  afterEach(() => { vi.unstubAllGlobals() })

  const extendable = <T extends object>(extra: T) => ({ waitUntil: (p: Promise<unknown>) => { waits.push(p) }, ...extra })
  const settle = () => Promise.all(waits)
  const fire = async (type: string, event: object) => {
    listeners[type]?.(extendable(event) as never)
    await settle()
  }
  const pushWith = (text: string | null) => fire('push', { data: text === null ? null : { text: () => text } })
  const client = (url: string, overrides: Partial<WindowClientLike> = {}): WindowClientLike => {
    const self: WindowClientLike = { url, focus: vi.fn(async () => self), navigate: vi.fn(async () => self), ...overrides }
    return self
  }

  it('always shows a notification, and only a fixed label', async () => {
    await pushWith(JSON.stringify(payload()))
    expect(show).toHaveBeenLastCalledWith('USapp', expect.objectContaining({ body: eventLabel('message.created'), tag: NID }))
    await pushWith(null)
    expect(show).toHaveBeenLastCalledWith('USapp', expect.objectContaining({ body: GENERIC_BODY, tag: GENERIC_TAG }))
    await pushWith(JSON.stringify(payload({ message: 'secret words' })))
    expect(show).toHaveBeenLastCalledWith('USapp', expect.objectContaining({ body: GENERIC_BODY }))
    expect(JSON.stringify(show.mock.calls)).not.toContain('secret words')
    expect(show).toHaveBeenCalledTimes(3)
  })

  it('shows the generic text after logout, and a duplicate carries the same tag', async () => {
    await pushWith(JSON.stringify(payload()))
    await pushWith(JSON.stringify(payload()))
    expect(show.mock.calls[0]?.[1].tag).toBe(show.mock.calls[1]?.[1].tag)
    marker = null
    await pushWith(JSON.stringify(payload()))
    expect(show).toHaveBeenLastCalledWith('USapp', expect.objectContaining({ body: GENERIC_BODY }))
  })

  it('treats unreadable storage and an unreadable body as no marker and no payload', async () => {
    await fire('push', { data: { text: () => { throw new Error('bad') } } })
    expect(show).toHaveBeenLastCalledWith('USapp', expect.objectContaining({ body: GENERIC_BODY }))
  })

  const clickEvent = (data: unknown) => ({ notification: { data, close: vi.fn() } })

  it('rechecks the marker at click time, and opens a window when none is open', async () => {
    const data = { nid: NID, target: `/us/messages/${TARGET}`, marker: MARKER }
    const event = clickEvent(data)
    await fire('notificationclick', event)
    expect(event.notification.close).toHaveBeenCalled()
    expect(openWindow).toHaveBeenLastCalledWith(`${ORIGIN}/us/messages/${TARGET}`)
    marker = stored(OTHER)
    await fire('notificationclick', clickEvent(data))
    expect(openWindow).toHaveBeenLastCalledWith(`${ORIGIN}/notifications`)
  })

  it('navigates a same-origin window, whatever account it showed, and ignores other origins', async () => {
    const other = client('https://other.example/x')
    const own = client(`${ORIGIN}/home`)
    windows = [other, own]
    await fire('notificationclick', clickEvent({ target: '/tasks', marker: MARKER }))
    expect(other.navigate).not.toHaveBeenCalled()
    expect(own.focus).toHaveBeenCalled()
    expect(own.navigate).toHaveBeenCalledWith(`${ORIGIN}/tasks`)
    expect(openWindow).not.toHaveBeenCalled()
  })

  it('opens a window when a client cannot be navigated', async () => {
    windows = [client(`${ORIGIN}/home`, { navigate: vi.fn(async () => { throw new TypeError('uncontrolled') }) })]
    await fire('notificationclick', clickEvent({ target: '/tasks', marker: MARKER }))
    expect(openWindow).toHaveBeenCalledWith(`${ORIGIN}/tasks`)
  })

  it('pushsubscriptionchange clears the association and nothing else', async () => {
    await fire('pushsubscriptionchange', {})
    expect(clear).toHaveBeenCalledTimes(1)
    expect(marker).toBeNull()
    expect(show).not.toHaveBeenCalled()
    expect(openWindow).not.toHaveBeenCalled()
  })

  it('makes no network request in any handler: no business write and no automatic mark-read', async () => {
    await pushWith(JSON.stringify(payload()))
    await fire('notificationclick', clickEvent({ target: '/tasks', marker: MARKER }))
    await fire('pushsubscriptionchange', {})
    expect(fetchSpy).not.toHaveBeenCalled()
  })

  it('answers only a capability request', () => {
    const port = { postMessage: vi.fn() }
    listeners.message?.({ data: { type: 'CAPABILITIES' }, ports: [port] } as never)
    expect(port.postMessage).toHaveBeenCalledWith({ type: 'CAPABILITIES', capabilities: ['push:v1'] })
    port.postMessage.mockClear()
    listeners.message?.({ data: { type: 'SKIP_WAITING' }, ports: [port] } as never)
    listeners.message?.({ data: null, ports: [port] } as never)
    expect(port.postMessage).not.toHaveBeenCalled()
  })
})
