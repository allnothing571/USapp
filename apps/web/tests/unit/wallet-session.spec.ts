import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest'
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import * as authApi from '@/api/auth'
import { getWallet } from '@/api/wallet'
import type { SessionBootstrap, WalletResponse } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useWalletStore } from '@/stores/wallet'
import { getCsrfToken } from '@/api/csrf'

function snapshot(id = 'user-a'): SessionBootstrap {
  return { authenticated: true, csrf_token: 'csrf',
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-07T00:00:00Z' },
    space: null }
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
})
afterEach(() => vi.restoreAllMocks())

describe('wallet session boundaries', () => {
  it.each(['anonymous', 'user-change', 'bootstrap-failure', 'logout', 'logout-failure', 'refresh-user-change'])
    ('clears balances and invalidates a pending request on %s', async (boundary) => {
      const session = useSessionStore(), wallet = useWalletStore()
      session.applyBootstrap(snapshot())
      vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 5, sunshine: 6 } })
      await wallet.load()
      expect(wallet.balances).toEqual({ flower: 5, sunshine: 6 })
      let resolve!: (value: WalletResponse) => void
      vi.mocked(getWallet).mockReturnValueOnce(new Promise<WalletResponse>((yes) => { resolve = yes }))
      const pending = wallet.load()
      switch (boundary) {
        case 'anonymous':
          session.applyBootstrap({ authenticated: false, csrf_token: 'pre-auth-token' })
          expect(getCsrfToken()).toBe('pre-auth-token')
          break
        case 'user-change':
          session.applyBootstrap(snapshot('user-b'))
          break
        case 'bootstrap-failure':
          vi.spyOn(console, 'warn').mockImplementation(() => undefined)
          vi.mocked(authApi.bootstrapSession).mockRejectedValueOnce(new Error('offline'))
          await session.bootstrap(true)
          expect(session.status).toBe('anonymous')
          break
        case 'logout':
          vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
          await session.logout()
          break
        case 'logout-failure':
          vi.spyOn(console, 'warn').mockImplementation(() => undefined)
          vi.mocked(authApi.deleteSession).mockRejectedValueOnce(new Error('offline'))
          await session.logout()
          break
        case 'refresh-user-change': {
          const next = snapshot('user-b')
          vi.mocked(authApi.getMe).mockResolvedValueOnce({ user: next.user!, space: null })
          await session.refreshMe()
          break
        }
      }
      expect(wallet.balances).toBeNull()
      expect(wallet.status).toBe('idle')
      resolve({ wallet: { flower: 99, sunshine: 99 } })
      await pending
      expect(wallet.balances).toBeNull()
      expect(wallet.status).toBe('idle')
    })

  it('keeps the current wallet across same-user bootstrap and nickname update', async () => {
    const session = useSessionStore(), wallet = useWalletStore()
    const same = snapshot()
    session.applyBootstrap(same)
    vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 5, sunshine: 6 } })
    await wallet.load()
    session.applyBootstrap({ ...same, csrf_token: 'rotated-token' })
    vi.mocked(authApi.updateMe).mockResolvedValueOnce({ user: { ...same.user!, display_name: 'new name' }, space: null })
    await session.updateDisplayName('new name')
    expect(wallet.balances).toEqual({ flower: 5, sunshine: 6 })
    expect(wallet.status).toBe('ready')
  })
})
