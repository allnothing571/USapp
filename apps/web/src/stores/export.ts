/**
 * E1 user-data export (docs/user-data-export-design.md Section 8.2). Memory only: a status and a message, never the file.
 * - One export per explicit click; no automatic retry and no background export.
 * - Identity-scoped: reset() on any identity change aborts the request and ignores a late answer.
 * - cancel() on leaving the page does the same. Bytes already received or saved cannot be recalled.
 * - Saving uses a temporary object URL, revoked on the next task; nothing goes to localStorage, IndexedDB or caches.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { downloadUserDataExport } from '@/api/export'
import { useSessionStore } from './session'

export type ExportStatus = 'idle' | 'working' | 'done' | 'failed'

function saveFile(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob)
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = filename
  anchor.rel = 'noopener'
  anchor.style.display = 'none'
  document.body.appendChild(anchor)
  try {
    anchor.click()
  } finally {
    anchor.remove()
    setTimeout(() => URL.revokeObjectURL(url), 0)
  }
}

export const useExportStore = defineStore('export', () => {
  const status = ref<ExportStatus>('idle')
  const message = ref('')
  const retryAfterSeconds = ref<number | null>(null)
  const savedName = ref('')
  let generation = 0
  let controller: AbortController | null = null

  const owner = (): string | null => {
    const session = useSessionStore()
    return session.isAuthenticated && session.user ? session.user.id : null
  }

  function abort(): void {
    controller?.abort()
    controller = null
  }

  function reset(): void {
    generation += 1
    abort()
    status.value = 'idle'
    message.value = ''
    retryAfterSeconds.value = null
    savedName.value = ''
  }

  /** Leaving the page: stop waiting and forget the outcome. */
  function cancel(): void {
    reset()
  }

  async function start(): Promise<void> {
    if (status.value === 'working') return
    const who = owner()
    if (!who) return
    abort()
    const gen = ++generation
    const own = new AbortController()
    controller = own
    status.value = 'working'
    message.value = ''
    retryAfterSeconds.value = null
    savedName.value = ''
    const current = () => gen === generation && who === owner() && !own.signal.aborted
    try {
      const file = await downloadUserDataExport(own.signal)
      if (!current()) return
      const name = file.filename ?? 'usapp-export.json'
      saveFile(file.blob, name)
      savedName.value = name
      status.value = 'done'
    } catch (error) {
      if (!current()) return
      if (error instanceof Error && error.name === 'AbortError') return
      status.value = 'failed'
      message.value = describeError(error)
      const retry = error instanceof ApiError ? Number(error.details?.retry_after_seconds) : NaN
      retryAfterSeconds.value = Number.isFinite(retry) && retry > 0 ? retry : null
    } finally {
      if (controller === own) controller = null
    }
  }

  return { status, message, retryAfterSeconds, savedName, start, cancel, reset }
})
