import { request } from './client'
import type { WalletResponse } from './types'

/** Own balances only; GET uses the session cookie without a CSRF header. */
export function getWallet(): Promise<WalletResponse> {
  return request<WalletResponse>('/api/v1/wallet', { method: 'GET' })
}
