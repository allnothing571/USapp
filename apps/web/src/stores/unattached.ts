/**
 * The caller's previously uploaded, still unattached photos (contract 0.39.0): memory only, nothing persisted.
 *
 * - Identity: `reset()` (from the session store on any identity change) raises the generation; a late page lands only if the generation
 *   and the user are unchanged. Each read has a sequence, so the newer read wins.
 * - A snapshot: being listed or chosen reserves nothing and renews nothing. The attachment that follows decides; a refusal means the list
 *   may have changed, and `load()` reads it again.
 * - Chosen ids live in the form that chose them (MemoryEditor, AlbumPhotoAdder), never here, so a pending write's captured ids cannot be
 *   altered from this store.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { describeError } from '@/api/error-messages'
import { isUnattachedPage, listUnattachedPhotos, type UnattachedPhoto } from '@/api/unattached'
import { useSessionStore } from './session'

export type UnattachedStatus = 'idle' | 'loading' | 'ready' | 'error'

const MALFORMED = '服务器的响应无法确认，请重试。'

export const useUnattachedStore = defineStore('unattached', () => {
  const items = ref<UnattachedPhoto[]>([])
  const status = ref<UnattachedStatus>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreErrorText = ref('')

  let generation = 0
  let seq = 0

  function currentUserId(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function reset(): void {
    generation += 1
    seq += 1
    items.value = []
    status.value = 'idle'
    errorText.value = ''
    nextCursor.value = null
    loadingMore.value = false
    moreErrorText.value = ''
  }

  async function load(): Promise<void> {
    const mine = ++seq
    const gen = generation
    const owner = currentUserId()
    status.value = 'loading'
    errorText.value = ''
    loadingMore.value = false
    moreErrorText.value = ''
    try {
      const response = await listUnattachedPhotos()
      if (mine !== seq || gen !== generation || !owner || currentUserId() !== owner) return
      if (!isUnattachedPage(response)) throw new Error(MALFORMED)
      items.value = response.photos
      nextCursor.value = response.next_cursor
      status.value = 'ready'
    } catch (error) {
      if (mine !== seq || gen !== generation || !owner || currentUserId() !== owner) return
      items.value = []
      nextCursor.value = null
      status.value = 'error'
      errorText.value = describeError(error)
    }
  }

  async function loadMore(): Promise<void> {
    if (status.value !== 'ready' || !nextCursor.value || loadingMore.value) return
    const mine = ++seq
    const gen = generation
    const owner = currentUserId()
    loadingMore.value = true
    moreErrorText.value = ''
    try {
      const response = await listUnattachedPhotos(nextCursor.value)
      if (mine !== seq || gen !== generation || !owner || currentUserId() !== owner) return
      if (!isUnattachedPage(response)) throw new Error(MALFORMED)
      const seen = new Set(items.value.map((p) => p.media_id))
      items.value = [...items.value, ...response.photos.filter((p) => !seen.has(p.media_id))]
      nextCursor.value = response.next_cursor
      loadingMore.value = false
    } catch (error) {
      if (mine !== seq || gen !== generation || !owner || currentUserId() !== owner) return
      loadingMore.value = false
      moreErrorText.value = describeError(error)
    }
  }

  return { items, status, errorText, nextCursor, loadingMore, moreErrorText, reset, load, loadMore }
})
