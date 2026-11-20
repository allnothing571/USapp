import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { getAvatarAppearance, getAvatarCatalog, getOwnedAvatars, purchaseAvatar, saveAvatarLoadout, emptySlots } from '@/api/avatar'
import { setCsrfToken } from '@/api/client'
import { id } from './avatar-fixtures'
const fetchMock = vi.fn()
beforeEach(() => { fetchMock.mockReset().mockImplementation(async () => new Response('{}')); vi.stubGlobal('fetch', fetchMock); setCsrfToken('synthetic-csrf') })
afterEach(() => { vi.unstubAllGlobals(); setCsrfToken(null) })
describe('avatar real API client with fake fetch', () => {
  it('uses the authenticated same-origin client for reads without a user-id parameter', async () => {
    await getAvatarCatalog(); await getAvatarAppearance(); await getOwnedAvatars('next-cursor')
    expect(fetchMock.mock.calls.map(([url]) => url)).toEqual(['/api/v1/avatar/catalog', '/api/v1/avatar', '/api/v1/avatar/owned?limit=20&cursor=next-cursor'])
    for (const [, init] of fetchMock.mock.calls) expect(init.credentials).toBe('same-origin')
  })
  it('sends only item and expected version, with CSRF and the captured idempotency key', async () => {
    await purchaseAvatar({ item_id: id(4), expected_version: 2 }, 'captured-avatar-key')
    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/avatar/purchases')
    expect(init.headers).toMatchObject({ 'X-CSRF-Token': 'synthetic-csrf', 'Idempotency-Key': 'captured-avatar-key' })
    expect(JSON.parse(init.body)).toEqual({ item_id: id(4), expected_version: 2 })
  })
  it('sends the complete six-slot request and blocks writes without a CSRF token', async () => {
    await saveAvatarLoadout({ expected_version: 0, slots: emptySlots() }, 'captured-loadout-key')
    expect(JSON.parse(fetchMock.mock.calls[0][1].body)).toEqual({ expected_version: 0, slots: emptySlots() })
    setCsrfToken(null)
    await expect(purchaseAvatar({ item_id: id(4), expected_version: 1 }, 'captured-avatar-key')).rejects.toBeDefined()
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })
})
