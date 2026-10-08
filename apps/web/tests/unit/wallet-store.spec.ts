import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { getWallet } from '@/api/wallet'
import { ApiError } from '@/api/client'
import type { WalletResponse } from '@/api/types'
import { useWalletStore } from '@/stores/wallet'

function deferred() {
  let resolve!: (value: WalletResponse) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<WalletResponse>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.mocked(getWallet).mockReset()
})

describe('memory-only wallet store', () => {
  it('shows loading then ready with actual zero balances', async () => {
    const reply = deferred()
    vi.mocked(getWallet).mockReturnValue(reply.promise)
    const store = useWalletStore()
    expect(store.status).toBe('idle')
    expect(store.balances).toBeNull()
    const pending = store.load()
    expect(store.status).toBe('loading')
    expect(store.balances).toBeNull()
    reply.resolve({ wallet: { flower: 0, sunshine: 0 } })
    await pending
    expect(store.status).toBe('ready')
    expect(store.balances).toEqual({ flower: 0, sunshine: 0 })
  })

  it('clears old numbers on loading/error, and a retry can recover', async () => {
    const store = useWalletStore()
    vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 8, sunshine: 9 } })
    await store.load()
    vi.mocked(getWallet).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    const pending = store.load()
    expect(store.balances).toBeNull()
    await pending
    expect(store.status).toBe('error')
    expect(store.errorCode).toBe('NETWORK_ERROR')
    expect(store.errorText).toBeTruthy()
    expect(store.balances).toBeNull()
    vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 1, sunshine: 2 } })
    await store.load()
    expect(store.status).toBe('ready')
    expect(store.errorCode).toBeNull()
    expect(store.errorText).toBe('')
  })

  it.each(['success', 'failure'])('reset discards a late %s response', async (kind) => {
    const reply = deferred()
    vi.mocked(getWallet).mockReturnValue(reply.promise)
    const store = useWalletStore()
    const pending = store.load()
    store.reset()
    if (kind === 'success') reply.resolve({ wallet: { flower: 8, sunshine: 9 } })
    else reply.reject(new ApiError('NETWORK_ERROR', 'late failure', 0))
    await pending
    expect(store.status).toBe('idle')
    expect(store.balances).toBeNull()
    expect(store.errorCode).toBeNull()
    expect(store.errorText).toBe('')
  })

  it.each(['success', 'failure'])('a newer load wins over an older %s response', async (kind) => {
    const older = deferred(), newer = deferred()
    vi.mocked(getWallet).mockReturnValueOnce(older.promise).mockReturnValueOnce(newer.promise)
    const store = useWalletStore()
    const oldPending = store.load(), newPending = store.load()
    newer.resolve({ wallet: { flower: 2, sunshine: 3 } })
    await newPending
    if (kind === 'success') older.resolve({ wallet: { flower: 99, sunshine: 99 } })
    else older.reject(new ApiError('NETWORK_ERROR', 'old failure', 0))
    await oldPending
    expect(store.status).toBe('ready')
    expect(store.balances).toEqual({ flower: 2, sunshine: 3 })
    expect(store.errorCode).toBeNull()
  })
})
