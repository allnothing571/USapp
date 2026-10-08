import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { getWallet } from '@/api/wallet'
import { ApiError, setCsrfToken } from '@/api/client'

let fetchMock: ReturnType<typeof vi.fn>
beforeEach(() => {
  fetchMock = vi.fn()
  vi.stubGlobal('fetch', fetchMock)
  setCsrfToken(null)
})
afterEach(() => {
  vi.unstubAllGlobals()
  Reflect.deleteProperty(window.navigator, 'onLine')
  setCsrfToken(null)
})

describe('wallet API uses the shared GET client', () => {
  it('uses cookie credentials and sends no CSRF header or body, even with a stored token', async () => {
    setCsrfToken('unused-for-get')
    fetchMock.mockResolvedValue({ ok: true, status: 200,
      text: async () => JSON.stringify({ wallet: { flower: 0, sunshine: 0 } }) })
    expect(await getWallet()).toEqual({ wallet: { flower: 0, sunshine: 0 } })
    expect(fetchMock).toHaveBeenCalledTimes(1)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/wallet')
    expect(init.method).toBe('GET')
    expect(init.credentials).toBe('same-origin')
    expect(init.body).toBeUndefined()
    expect(init.headers).not.toHaveProperty('X-CSRF-Token')
  })

  it('does not need a preloaded CSRF token', async () => {
    fetchMock.mockResolvedValue({ ok: true, status: 200,
      text: async () => JSON.stringify({ wallet: { flower: 1000000, sunshine: 1000000000 } }) })
    expect((await getWallet()).wallet.sunshine).toBe(1000000000)
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('an offline GET fails as a network error, with no write queue or automatic replay', async () => {
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => false })
    fetchMock.mockRejectedValue(new TypeError('offline'))
    await expect(getWallet()).rejects.toMatchObject({ code: 'NETWORK_ERROR' })
    expect(fetchMock).toHaveBeenCalledTimes(1)
    window.dispatchEvent(new Event('online'))
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('preserves the server unauthenticated error', async () => {
    fetchMock.mockResolvedValue({ ok: false, status: 401,
      json: async () => ({ error: { code: 'UNAUTHENTICATED', message: 'login required' } }) })
    await expect(getWallet()).rejects.toBeInstanceOf(ApiError)
    await expect(getWallet()).rejects.toMatchObject({ status: 401, code: 'UNAUTHENTICATED' })
  })
})
