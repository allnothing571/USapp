/**
 * Whether new photo uploads are on (contract 0.40.0): memory only, nothing persisted. The one place pickers ask.
 *
 * - `uploadsEnabled` is true or false as the newest applicable answer said, and null while unknown, loading or after a failed read; every
 *   picker treats anything but true as "cannot start a new upload". Choosing already uploaded photos does not depend on it.
 * - Sources, each a snapshot: this store's own read of /api/v1/media/capabilities, the `uploads_enabled` a timeline or album-list page
 *   carries (`observe`), and a real operation's MEDIA_DISABLED (`markDisabled`).
 * - Ordering: every read takes a token when it starts. `load`, `markDisabled` and `reset` move the epoch, so an answer that started before
 *   them is dropped. A MEDIA_DISABLED therefore outranks an enabled answer still in flight, and only a read started after it can turn
 *   uploads back on.
 * - Identity: `reset()` (from the session store) also raises the generation, and an answer lands only for the user who asked.
 * - Nothing here cancels an upload, a READY photo or a captured write: it only decides whether a new file can be picked.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { describeError } from '@/api/error-messages'
import { getMediaCapabilities, isMediaCapabilities } from '@/api/media-capability'
import { useSessionStore } from './session'

export type CapabilityStatus = 'idle' | 'loading' | 'ready' | 'error'

export interface CapabilityToken {
  generation: number
  epoch: number
  owner: string | null
}

const MALFORMED = '服务器的响应无法确认，请重试。'

export const useMediaCapabilityStore = defineStore('media-capability', () => {
  const uploadsEnabled = ref<boolean | null>(null)
  const status = ref<CapabilityStatus>('idle')
  const errorText = ref('')

  let generation = 0
  let epoch = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function token(): CapabilityToken {
    return { generation, epoch, owner: currentUserId() }
  }

  function current(t: CapabilityToken): boolean {
    return t.generation === generation && t.epoch === epoch && t.owner !== null && currentUserId() === t.owner
  }

  function reset(): void {
    generation += 1
    epoch += 1
    uploadsEnabled.value = null
    status.value = 'idle'
    errorText.value = ''
  }

  /** A list page's `uploads_enabled`, applied only if nothing newer happened since that page was requested. */
  function observe(value: unknown, started: CapabilityToken): void {
    if (!current(started) || typeof value !== 'boolean' || status.value === 'loading') return
    uploadsEnabled.value = value
    status.value = 'ready'
    errorText.value = ''
  }

  /** A real operation answered MEDIA_DISABLED: a fact newer than any answer still in flight. */
  function markDisabled(): void {
    epoch += 1
    uploadsEnabled.value = false
    status.value = 'ready'
    errorText.value = ''
  }

  async function load(): Promise<void> {
    epoch += 1
    const started = token()
    uploadsEnabled.value = null
    status.value = 'loading'
    errorText.value = ''
    try {
      const response = await getMediaCapabilities()
      if (!current(started)) return
      if (!isMediaCapabilities(response)) throw new Error(MALFORMED)
      uploadsEnabled.value = response.uploads_enabled
      status.value = 'ready'
    } catch (error) {
      if (!current(started)) return
      uploadsEnabled.value = null
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  /** Read once if nothing is known yet; a page opened directly calls this instead of loading an unrelated list. */
  function ensure(): void {
    if (status.value === 'idle' && currentUserId()) void load()
  }

  return { uploadsEnabled, status, errorText, token, observe, markDisabled, reset, load, ensure }
})
