/**
 * Two browsing contexts: separately loaded module instances sharing one store, each operation atomic
 * as one IndexedDB transaction is. The real IndexedDB path is exercised separately in a real browser
 * with same-origin frames (docs/phase2-web-push.md Section 16).
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'

import type { MarkerBackend, MarkerRecord } from '@/pwa/push-marker'

class SharedStore implements MarkerBackend {
  value: MarkerRecord | null = null
  fenceValue = 0
  cleared: string[] = []
  async fence() { return this.fenceValue }
  async putIfFence(record: MarkerRecord, fence: number) {
    const later = this.value?.activatedAt && record.activatedAt && Date.parse(this.value.activatedAt) > Date.parse(record.activatedAt)
    if (fence !== this.fenceValue || this.cleared.includes(record.generationId) || later) return false
    this.value = { ...record }
    return true
  }
  async clearAll() { this.fenceValue += 1; this.value = null }
  async compareDelete(id: string) {
    if (!this.cleared.includes(id)) this.cleared.push(id)
    if (this.value?.generationId === id) this.value = null
  }
  async get() { return this.value ? { ...this.value } : null }
}

async function context(store: SharedStore) {
  vi.resetModules()
  const module = await import('@/pwa/push-marker')
  module.setMarkerBackendForTests(store)
  return module
}

const G1 = { generationId: 'g1', marker: 'a'.repeat(22), activatedAt: '2026-11-01T00:00:00.000Z' }
const G2 = { generationId: 'g2', marker: 'b'.repeat(22), activatedAt: '2026-11-02T00:00:00.000Z' }

describe('marker across contexts', () => {
  let store: SharedStore
  beforeEach(() => { store = new SharedStore() })

  it('a write delayed in tab A cannot restore a marker after tab B cleared it', async () => {
    const a = await context(store)
    const b = await context(store)
    expect(a).not.toBe(b)
    a.primeMarkerFence()
    const capture = a.captureMarker()
    await a.markerIdle()
    await b.clearMarker()
    expect(await a.writeMarker(G1, capture)).toBe(false)
    expect(store.value).toBeNull()
  })

  it('a clear by the service worker fences a delayed write of a tab the same way', async () => {
    const tab = await context(store)
    const worker = await context(store)
    await tab.writeMarker(G1, tab.captureMarker())
    const capture = tab.captureMarker()
    await tab.markerIdle()
    await worker.clearMarker()
    expect(await tab.writeMarker(G2, capture)).toBe(false)
    expect(store.value).toBeNull()
  })

  it('an old targeted clear never erases a newer tab\'s marker', async () => {
    const a = await context(store)
    const b = await context(store)
    await b.writeMarker(G2, b.captureMarker())
    await a.clearMarkerFor('g1')
    expect(store.value?.generationId).toBe('g2')
    await a.clearMarkerFor('g2')
    expect(store.value).toBeNull()
  })

  it('a write delayed past that generation\'s revoke in another tab cannot restore it', async () => {
    const a = await context(store)
    const b = await context(store)
    const capture = a.captureMarker()
    await a.markerIdle()
    await b.clearMarkerFor('g1')
    expect(await a.writeMarker(G1, capture)).toBe(false)
    expect(store.value).toBeNull()
  })

  it('an older generation written late never replaces a newer one', async () => {
    const a = await context(store)
    const b = await context(store)
    const capture = a.captureMarker()
    await a.markerIdle()
    await b.writeMarker(G2, b.captureMarker())
    expect(await a.writeMarker(G1, capture)).toBe(false)
    expect(store.value?.generationId).toBe('g2')
    expect(await a.writeMarker({ ...G2 }, a.captureMarker())).toBe(true)
  })

  it('captures synchronously: no await stands between a gesture and the prompt', async () => {
    const a = await context(store)
    const capture = a.captureMarker()
    expect(capture.epoch).toBe(0)
    expect(capture.fence).toBeInstanceOf(Promise)
  })
})
