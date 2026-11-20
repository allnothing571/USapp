/**
 * E1 user-data export (docs/user-data-export-design.md Section 8.2): the raw download helper keeps request()'s guards,
 * the store saves through a revoked object URL, never persists anything, never retries by itself, and drops late answers
 * after an identity change or leaving the page. fetch is stubbed; nothing reaches a network.
 */
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('vue-router', () => ({ useRouter: () => ({ replace: vi.fn() }) }))
vi.mock('@/api/space', () => ({ listSpaceInvites: vi.fn().mockResolvedValue({ invites: [] }), getCurrentSpace: vi.fn() }))

import { ApiError, MAX_DOWNLOAD_BYTES, OfflineMutationBlockedError, request, requestDownload, setCsrfToken } from '@/api/client'
import { USER_EXPORT_PATH } from '@/api/export'
import { isCacheableRequest, isSensitivePath } from '@/pwa/cache-policy'
import { useExportStore } from '@/stores/export'
import { useSessionStore } from '@/stores/session'
import MeView from '@/views/MeView.vue'

const USER = { id: 'self', email: 'self@example.com', display_name: '我', created_at: '2026-10-07T00:00:00Z' }
const SPACE = { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-07T00:00:00Z', members: [] }
const DOC = '{"export":{"format":"usapp-user-export","schema_version":1}}'

/**
 * A body stream that records what the application consumed: each read, and whether it cancelled. Elements are byte chunks,
 * or an Error that makes that read reject. After cancel() every read reports done and counts as consumption after refusal.
 */
class FakeBody {
  reads = 0
  readsAfterCancel = 0
  cancelled = 0
  private next = 0
  constructor(private readonly chunks: Array<Uint8Array | Error>, private readonly gate?: Promise<void>) {}
  getReader() {
    return {
      read: async (): Promise<{ done: boolean; value?: Uint8Array }> => {
        this.reads += 1
        if (this.cancelled) { this.readsAfterCancel += 1; return { done: true } }
        if (this.gate && this.next === 1) await this.gate
        if (this.next >= this.chunks.length) return { done: true }
        const item = this.chunks[this.next++]
        if (item instanceof Error) throw item
        return { done: false, value: item }
      },
      cancel: async () => { this.cancelled += 1 },
      releaseLock: () => undefined,
    }
  }
  async cancel() { this.cancelled += 1 }
}
const encode = (text: string) => new TextEncoder().encode(text)
function chunksOf(text: string, size = 7): Uint8Array[] {
  const bytes = encode(text)
  const out: Uint8Array[] = []
  for (let at = 0; at < bytes.length; at += size) out.push(bytes.slice(at, at + size))
  return out
}

interface Fake { status?: number; body?: string; headers?: Record<string, string>; stream?: FakeBody | null }
function fakeResponse(init: Fake = {}): Response {
  const status = init.status ?? 200
  const text = init.body ?? ''
  return {
    ok: status >= 200 && status < 300,
    status,
    headers: new Headers(init.headers ?? {}),
    body: init.stream === undefined ? new FakeBody(chunksOf(text)) : init.stream,
    json: async () => JSON.parse(text) as unknown,
    text: async () => text,
    // The download path must never buffer the whole body itself.
    blob: async () => { throw new Error('blob() must not be used') },
  } as unknown as Response
}
async function textOf(blob: Blob): Promise<string> {
  return new Promise<string>((resolve) => {
    const reader = new FileReader()
    reader.onload = () => resolve(String(reader.result))
    reader.readAsText(blob)
  })
}
function okExport(body = DOC): Response {
  return fakeResponse({ body, headers: {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Disposition': 'attachment; filename="usapp-export-20261110T080000Z.json"',
    'Content-Length': String(new Blob([body]).size),
  } })
}
function stubOnline(value: boolean): void {
  Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => value })
}
function signIn(id = USER.id): void {
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'csrf-token', user: { ...USER, id }, space: SPACE } as never)
}

let fetchMock: ReturnType<typeof vi.fn>
let createUrl: ReturnType<typeof vi.fn>
let revokeUrl: ReturnType<typeof vi.fn>
let clickSpy: ReturnType<typeof vi.spyOn>

beforeEach(() => {
  setActivePinia(createPinia())
  fetchMock = vi.fn()
  vi.stubGlobal('fetch', fetchMock)
  createUrl = vi.fn(() => 'blob:export')
  revokeUrl = vi.fn()
  Object.defineProperty(URL, 'createObjectURL', { configurable: true, value: createUrl })
  Object.defineProperty(URL, 'revokeObjectURL', { configurable: true, value: revokeUrl })
  clickSpy = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => undefined)
  stubOnline(true)
  setCsrfToken('csrf-token')
})
afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
  clickSpy.mockRestore()
  setCsrfToken(null as never)
})

describe('requestDownload', () => {
  it('posts {} with the CSRF header and same-origin credentials, and returns the raw bytes with the attachment name', async () => {
    fetchMock.mockResolvedValueOnce(okExport())
    const file = await requestDownload(USER_EXPORT_PATH, { body: {} })
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/me/export')
    expect(init.method).toBe('POST')
    expect(init.credentials).toBe('same-origin')
    expect(init.body).toBe('{}')
    expect((init.headers as Record<string, string>)['X-CSRF-Token']).toBe('csrf-token')
    expect(file.filename).toBe('usapp-export-20261110T080000Z.json')
    // jsdom's Blob has no text(); FileReader reads the exact bytes returned, assembled from several chunks.
    expect(await textOf(file.blob)).toBe(DOC)
    expect(file.bytes).toBe(encode(DOC).byteLength)
  })

  it('refuses offline and without a CSRF token before any request', async () => {
    stubOnline(false)
    await expect(requestDownload(USER_EXPORT_PATH, { body: {} })).rejects.toBeInstanceOf(OfflineMutationBlockedError)
    stubOnline(true)
    setCsrfToken(null as never)
    await expect(requestDownload(USER_EXPORT_PATH, { body: {} })).rejects.toMatchObject({ code: 'CSRF_TOKEN_INVALID' })
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('parses error envelopes, carries Retry-After and maps network failures', async () => {
    fetchMock.mockResolvedValueOnce(fakeResponse({ status: 413, body: '{"error":{"code":"EXPORT_TOO_LARGE","message":"x"}}' }))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'EXPORT_TOO_LARGE', status: 413 })
    fetchMock.mockResolvedValueOnce(fakeResponse({ status: 429, body: '{"error":{"code":"RATE_LIMITED","message":"x"}}', headers: { 'Retry-After': '42' } }))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'RATE_LIMITED', details: { retry_after_seconds: 42 } })
    fetchMock.mockRejectedValueOnce(new TypeError('down'))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'NETWORK_ERROR' })
  })

  it('accepts exactly the cap, with or without a Content-Length', async () => {
    const exact = 'x'.repeat(40)
    fetchMock.mockResolvedValueOnce(fakeResponse({ body: exact, headers: { 'Content-Length': '40' } }))
    const declared = await requestDownload(USER_EXPORT_PATH, { maxBytes: 40 })
    expect(declared.bytes).toBe(40)
    expect(await textOf(declared.blob)).toBe(exact)
    const missing = new FakeBody(chunksOf(exact, 9))
    fetchMock.mockResolvedValueOnce(fakeResponse({ stream: missing }))
    const undeclared = await requestDownload(USER_EXPORT_PATH, { maxBytes: 40 })
    expect(undeclared.bytes).toBe(40)
    expect(missing.cancelled).toBe(0)
  })

  it('refuses an oversized declared length without reading and cancels the body', async () => {
    const body = new FakeBody(chunksOf(DOC))
    fetchMock.mockResolvedValueOnce(fakeResponse({ stream: body, headers: { 'Content-Length': String(MAX_DOWNLOAD_BYTES + 1) } }))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'EXPORT_TOO_LARGE' })
    expect(body.reads).toBe(0)
    expect(body.cancelled).toBe(1)
  })

  it('counts actual bytes: a missing or misleadingly small Content-Length cannot let more through', async () => {
    for (const headers of [{}, { 'Content-Length': '5' }] as Array<Record<string, string>>) {
      const body = new FakeBody(chunksOf('y'.repeat(50), 8))
      fetchMock.mockResolvedValueOnce(fakeResponse({ stream: body, headers }))
      await expect(requestDownload(USER_EXPORT_PATH, { maxBytes: 30 })).rejects.toMatchObject({ code: 'EXPORT_TOO_LARGE' })
      // 8+8+8 = 24 kept; the fourth chunk would cross 30: refused there, cancelled, and nothing read afterwards.
      expect(body.reads).toBe(4)
      expect(body.cancelled).toBe(1)
      expect(body.readsAfterCancel).toBe(0)
    }
  })

  it('a read failure cancels and fails without returning a partial file', async () => {
    const body = new FakeBody([encode('{"a":'), new TypeError('reset')])
    fetchMock.mockResolvedValueOnce(fakeResponse({ stream: body }))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'NETWORK_ERROR' })
    expect(body.cancelled).toBe(1)
  })

  it('an abort between chunks stops reading, cancels and rethrows AbortError', async () => {
    let open: () => void = () => undefined
    const gate = new Promise<void>((resolve) => { open = resolve })
    const body = new FakeBody(chunksOf(DOC, 4), gate)
    fetchMock.mockResolvedValueOnce(fakeResponse({ stream: body }))
    const controller = new AbortController()
    const pending = requestDownload(USER_EXPORT_PATH, { signal: controller.signal })
    await flushPromises()
    controller.abort()
    open()
    await expect(pending).rejects.toMatchObject({ name: 'AbortError' })
    expect(body.cancelled).toBe(1)
    expect(body.reads).toBeLessThan(chunksOf(DOC, 4).length)
  })

  it('fails clearly, with no buffered fallback, when the body is not a readable stream', async () => {
    fetchMock.mockResolvedValueOnce(fakeResponse({ body: DOC, stream: null }))
    await expect(requestDownload(USER_EXPORT_PATH)).rejects.toMatchObject({ code: 'DOWNLOAD_STREAM_UNSUPPORTED' })
  })

  it('ignores an unsafe attachment name and leaves request() parsing JSON as before', async () => {
    fetchMock.mockResolvedValueOnce(fakeResponse({ body: DOC, headers: { 'Content-Disposition': 'attachment; filename="../../x.json"' } }))
    expect((await requestDownload(USER_EXPORT_PATH)).filename).toBeNull()
    fetchMock.mockResolvedValueOnce(fakeResponse({ body: DOC }))
    expect(await request('/api/v1/anything')).toEqual(JSON.parse(DOC))
  })
})

describe('cache policy', () => {
  it('never caches the export route, by method or by path', () => {
    expect(isCacheableRequest(USER_EXPORT_PATH, 'POST', { destination: '' })).toBe(false)
    expect(isCacheableRequest(USER_EXPORT_PATH, 'GET', { destination: 'document' })).toBe(false)
    expect(isSensitivePath(USER_EXPORT_PATH)).toBe(true)
  })
})

describe('export store', () => {
  it('saves through an anchor and revokes the object URL; nothing is persisted', async () => {
    vi.useFakeTimers()
    signIn()
    const setItem = vi.spyOn(Storage.prototype, 'setItem')
    fetchMock.mockResolvedValueOnce(okExport())
    const store = useExportStore()
    await store.start()
    expect(store.status).toBe('done')
    expect(store.savedName).toBe('usapp-export-20261110T080000Z.json')
    expect(createUrl).toHaveBeenCalledTimes(1)
    expect(clickSpy).toHaveBeenCalledTimes(1)
    expect(document.querySelector('a[download]')).toBeNull()
    expect(revokeUrl).not.toHaveBeenCalled()
    vi.runAllTimers()
    expect(revokeUrl).toHaveBeenCalledWith('blob:export')
    expect(setItem).not.toHaveBeenCalled()
    setItem.mockRestore()
  })

  it('reports a failure without retrying by itself; a click retries', async () => {
    vi.useFakeTimers()
    signIn()
    fetchMock.mockResolvedValueOnce(fakeResponse({ status: 503, body: '{"error":{"code":"EXPORT_UNAVAILABLE","message":"x"}}' }))
    const store = useExportStore()
    await store.start()
    expect(store.status).toBe('failed')
    expect(store.message).toContain('没有生成任何文件')
    vi.advanceTimersByTime(600_000)
    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(createUrl).not.toHaveBeenCalled()
    fetchMock.mockResolvedValueOnce(fakeResponse({ status: 429, body: '{"error":{"code":"RATE_LIMITED","message":"x"}}', headers: { 'Retry-After': '30' } }))
    await store.start()
    expect(fetchMock).toHaveBeenCalledTimes(2)
    expect(store.retryAfterSeconds).toBe(30)
  })

  it('drops a late answer after an identity change and aborts the request', async () => {
    signIn()
    let resolve: (r: Response) => void = () => undefined
    fetchMock.mockImplementationOnce(() => new Promise<Response>((r) => { resolve = r }))
    const store = useExportStore()
    const running = store.start()
    expect(store.status).toBe('working')
    const signal = (fetchMock.mock.calls[0][1] as RequestInit).signal as AbortSignal
    signIn('someone-else')
    expect(signal.aborted).toBe(true)
    resolve(okExport())
    await running
    expect(store.status).toBe('idle')
    expect(createUrl).not.toHaveBeenCalled()
  })

  it('a refused oversized stream never creates an object URL or starts a download', async () => {
    signIn()
    const body = new FakeBody(chunksOf('z'.repeat(64), 16))
    fetchMock.mockResolvedValueOnce(fakeResponse({ stream: body, headers: { 'Content-Length': String(MAX_DOWNLOAD_BYTES + 1) } }))
    const store = useExportStore()
    await store.start()
    expect(store.status).toBe('failed')
    expect(createUrl).not.toHaveBeenCalled()
    expect(clickSpy).not.toHaveBeenCalled()
    expect(body.cancelled).toBe(1)
  })

  it('cancel() on leaving the page ignores the answer; an already-saved file is not affected', async () => {
    signIn()
    let resolve: (r: Response) => void = () => undefined
    fetchMock.mockImplementationOnce(() => new Promise<Response>((r) => { resolve = r }))
    const store = useExportStore()
    const running = store.start()
    store.cancel()
    resolve(okExport())
    await running
    expect(store.status).toBe('idle')
    expect(createUrl).not.toHaveBeenCalled()
    expect(ApiError).toBeDefined()
  })
})

describe('me page export card', () => {
  it('explains scope, photos and non-revocation, starts on click and cancels on unmount', async () => {
    signIn()
    let resolve: (r: Response) => void = () => undefined
    fetchMock.mockImplementation((url: string) => {
      if (url === '/api/v1/me/export') return new Promise<Response>((r) => { resolve = r })
      return Promise.resolve(fakeResponse({ body: '{"invites":[]}' }))
    })
    const wrapper = mount(MeView, { global: { stubs: { RouterLink: true, PushDeviceCard: true } } })
    await flushPromises()
    const card = wrapper.find('[data-test="user-export"]')
    expect(card.text()).toContain('不包含对方的私人数据')
    expect(card.text()).toContain('照片文件本身')
    expect(card.text()).toContain('不会撤回已下载的文件')
    await wrapper.find('[data-test="user-export-start"]').trigger('click')
    expect(useExportStore().status).toBe('working')
    expect((wrapper.find('[data-test="user-export-start"]').element as HTMLButtonElement).disabled).toBe(true)
    wrapper.unmount()
    resolve(okExport())
    await flushPromises()
    expect(useExportStore().status).toBe('idle')
    expect(createUrl).not.toHaveBeenCalled()
  })
})
