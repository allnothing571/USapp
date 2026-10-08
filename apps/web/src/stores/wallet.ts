import { defineStore } from 'pinia'
import { ref } from 'vue'

import { getWallet } from '@/api/wallet'
import type { Wallet } from '@/api/types'
import { describeError, errorCodeOf } from '@/api/error-messages'

/** Memory only. Each load/reset invalidates every older response, including errors. */
export const useWalletStore = defineStore('wallet', () => {
  const balances = ref<Wallet | null>(null)
  const status = ref<'idle' | 'loading' | 'ready' | 'error'>('idle')
  const errorCode = ref<string | null>(null)
  const errorText = ref('')
  let generation = 0

  function reset(): void {
    ++generation
    balances.value = null
    status.value = 'idle'
    errorCode.value = null
    errorText.value = ''
  }

  async function load(): Promise<void> {
    const requestGeneration = ++generation
    balances.value = null
    status.value = 'loading'
    errorCode.value = null
    errorText.value = ''
    try {
      const response = await getWallet()
      if (requestGeneration !== generation) return
      balances.value = { ...response.wallet }
      status.value = 'ready'
    } catch (error) {
      if (requestGeneration !== generation) return
      balances.value = null
      status.value = 'error'
      errorCode.value = errorCodeOf(error)
      errorText.value = describeError(error)
    }
  }

  return { balances, status, errorCode, errorText, load, reset }
})
