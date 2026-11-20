/**
 * The upload queue for photo memories (P3-1c, docs/phase3-plan.md Section 23): grant → direct POST to staging → completion.
 *
 * An item can be attached only once the backend has answered READY. Each failure has its own state, because each has a different
 * safe retry, and every retry is a user click:
 *   grant_unknown  the grant may exist: same key and body, which replays it.
 *   interrupted    the POST did not finish: re-POST the same form while it is valid. Staging is allowed to change.
 *   unconfirmed    the POST finished but completion has no answer: complete the SAME media id again. No new grant, no new POST.
 *   expired        the form or grant has ended: only an explicit "upload again" takes a new grant, with a new key.
 *   rejected       terminal: the bytes were inspected and refused.
 *
 * What stays out of reach: the file, signed form and key live in a plain Map outside reactive state. So they cannot reach a
 * devtools snapshot, a store dump or storage, they are dropped as soon as READY or rejected is known, and nothing here logs.
 * There is no local preview, because nothing before READY is displayed.
 *
 * Late results: every step re-checks identity and that its item still exists. `reset()` (identity change) and `clearScope()`
 * (the editor left the route) abort the POST and forget the items, so a late answer changes nothing. An object left READY and
 * unattached that way is removed by the server after its unattached period.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { completeMediaUpload, createMediaUpload, isAbort, isMediaFacts, isUploadForm, postToStaging } from '@/api/media'
import { MEDIA_MAX_BYTES, MEDIA_TYPES } from '@/api/memory-rules'
import { newIdempotencyKey } from '@/api/tasks'
import type { MediaUploadForm } from '@/api/types'
import { useAlbumsStore } from './albums'
import { useMemoriesStore } from './memories'
import { useMediaCapabilityStore } from './media-capability'
import { useSessionStore } from './session'

export type UploadStatus =
  | 'queued' | 'granting' | 'grant_unknown' | 'uploading' | 'interrupted' | 'completing' | 'unconfirmed'
  | 'ready' | 'rejected' | 'expired' | 'failed' | 'disabled'

export interface UploadItem {
  id: string
  scope: string
  status: UploadStatus
  mediaId: string | null
  errorText: string
  /** Refused here, before anything was sent: there is nothing to retry. */
  local?: boolean
}

type Phase = 'grant' | 'post' | 'complete'

interface Transient {
  file: File
  key: string
  form: MediaUploadForm | null
  controller: AbortController | null
  next: Phase
}

export const MAX_CONCURRENT_UPLOADS = 3

const ACTIVE: ReadonlySet<UploadStatus> = new Set(['granting', 'uploading', 'completing'])

const REJECT_TEXT: Record<string, string> = {
  size_mismatch: '文件大小与声明不一致。',
  not_image: '这个文件不是可用的图片。',
  format_mismatch: '文件内容与图片格式不一致。',
  dimensions: '图片尺寸超出限制。',
  attempts_exhausted: '检查次数已用完。',
}

export function checkUploadFile(file: { type: string; size: number }): string | null {
  if (!MEDIA_TYPES.includes(file.type)) return '只支持 JPEG、PNG 或 WebP 图片。'
  if (file.size < 1 || file.size > MEDIA_MAX_BYTES) return '单张照片最大 20 MB。'
  return null
}

export const useMemoryUploadsStore = defineStore('memoryUploads', () => {
  const items = ref<UploadItem[]>([])
  const transients = new Map<string, Transient>()
  let generation = 0
  let nextId = 0

  function owner(): string | null {
    return useSessionStore().user?.id ?? null
  }

  function patch(id: string, change: Partial<UploadItem>): void {
    items.value = items.value.map((item) => (item.id === id ? { ...item, ...change } : item))
  }

  function find(id: string): UploadItem | undefined {
    return items.value.find((item) => item.id === id)
  }

  function forget(id: string): void {
    const transient = transients.get(id)
    transient?.controller?.abort()
    transients.delete(id)
  }

  function reset(): void {
    generation += 1
    for (const id of [...transients.keys()]) forget(id)
    items.value = []
  }

  function clearScope(scope: string): void {
    for (const item of items.value) if (item.scope === scope) forget(item.id)
    items.value = items.value.filter((item) => item.scope !== scope)
    pump()
  }

  function remove(id: string): void {
    forget(id)
    items.value = items.value.filter((item) => item.id !== id)
    pump()
  }

  /** At most three transfers at a time; no offline queue. */
  function pump(): void {
    let active = items.value.filter((item) => ACTIVE.has(item.status)).length
    for (const item of items.value) {
      if (active >= MAX_CONCURRENT_UPLOADS) return
      if (item.status !== 'queued') continue
      active += 1
      // Marked active now, before the first await, so the count stays right.
      const phase = transients.get(item.id)?.next ?? 'grant'
      patch(item.id, { status: phase === 'grant' ? 'granting' : phase === 'post' ? 'uploading' : 'completing', errorText: '' })
      void run(item.id)
    }
  }

  function settle(id: string, status: UploadStatus, errorText: string, next?: Phase): void {
    const transient = transients.get(id)
    if (transient && next) transient.next = next
    patch(id, { status, errorText })
  }

  /** Bytes and the form are no longer needed once the outcome is decided. */
  function dropBytes(id: string): void {
    transients.delete(id)
  }

  async function run(id: string): Promise<void> {
    const startedGeneration = generation
    const startedOwner = owner()
    const live = () => generation === startedGeneration && startedOwner !== null && owner() === startedOwner && transients.has(id)
    try {
      const transient = transients.get(id)
      if (!transient) return
      if (transient.next === 'grant') await grant(id, transient, live)
      if (live() && transient.next === 'post' && find(id)?.status === 'uploading') await post(id, transient, live)
      if (live() && transient.next === 'complete' && find(id)?.status === 'completing') await complete(id, live)
    } finally {
      if (generation === startedGeneration) pump()
    }
  }

  /** A fact about the server, so every place that offers a picker learns it, ahead of any capability answer still in flight. */
  function markDisabled(): void {
    useMediaCapabilityStore().markDisabled()
    useMemoriesStore().markUploadsDisabled()
    useAlbumsStore().markUploadsDisabled()
  }

  async function ensureCsrf(): Promise<void> {
    await useSessionStore().ensureCsrfToken()
    if (!getCsrfToken()) throw new ApiError('CSRF_TOKEN_INVALID', '缺少安全令牌，请求未发出。', 0)
  }

  async function grant(id: string, transient: Transient, live: () => boolean): Promise<void> {
    const unknown = (text: string) => settle(id, 'grant_unknown', text, 'grant')
    try {
      await ensureCsrf()
      if (!live()) return
      const response = await createMediaUpload(transient.file.type, transient.file.size, transient.key)
      if (!live()) return
      if (!response || !isMediaFacts(response.media)) {
        unknown('服务器的响应无法确认，可以重试，不会重复申请。')
        return
      }
      const media = response.media
      patch(id, { mediaId: media.media_id })
      if (media.state === 'ready') {
        dropBytes(id)
        settle(id, 'ready', '')
      } else if (media.state === 'rejected') {
        dropBytes(id)
        settle(id, 'rejected', REJECT_TEXT[media.reason ?? ''] ?? '这张照片没有通过检查。')
      } else if (media.state === 'granted' && isUploadForm(response.upload)) {
        transient.form = response.upload
        transient.next = 'post'
        patch(id, { status: 'uploading' })
      } else if (media.state === 'granted' && response.upload !== null) {
        unknown('服务器的响应无法确认，可以重试，不会重复申请。')
      } else {
        settle(id, 'expired', '这次上传已结束，需要重新上传。', 'grant')
      }
    } catch (error) {
      if (!live()) return
      if (error instanceof ApiError && error.code === 'MEDIA_DISABLED') {
        markDisabled()
        settle(id, 'disabled', describeError(error), 'grant')
      } else if (error instanceof ApiError && error.status >= 400 && error.status < 500) {
        settle(id, 'failed', describeError(error), 'grant')
      } else {
        unknown(error instanceof OfflineMutationBlockedError ? describeError(error) : '上传申请的结果未确认，可以重试，不会重复申请。')
      }
    }
  }

  async function post(id: string, transient: Transient, live: () => boolean): Promise<void> {
    const form = transient.form
    if (!form || Date.parse(form.expires_at) <= Date.now()) {
      settle(id, 'expired', '这次上传已过期，需要重新上传。', 'grant')
      return
    }
    transient.controller = new AbortController()
    try {
      await postToStaging(form, transient.file, transient.controller.signal)
      if (!live()) return
      transient.next = 'complete'
      patch(id, { status: 'completing' })
    } catch (error) {
      if (!live() || isAbort(error)) return
      settle(id, 'interrupted', '上传中断了，照片还没有保存。可以继续上传。', 'post')
    } finally {
      transient.controller = null
    }
  }

  async function complete(id: string, live: () => boolean): Promise<void> {
    const mediaId = find(id)?.mediaId ?? ''
    const unconfirmed = (text: string) => settle(id, 'unconfirmed', text, 'complete')
    try {
      await ensureCsrf()
      if (!live()) return
      const response = await completeMediaUpload(mediaId)
      if (!live()) return
      if (!response || !isMediaFacts(response.media, mediaId)) {
        unconfirmed('照片已上传，但服务器的响应无法确认。可以重试确认，不需要重新上传。')
      } else if (response.media.state === 'ready') {
        dropBytes(id)
        settle(id, 'ready', '')
      } else if (response.media.state === 'rejected') {
        dropBytes(id)
        settle(id, 'rejected', REJECT_TEXT[response.media.reason ?? ''] ?? '这张照片没有通过检查。')
      } else {
        unconfirmed('照片已上传，但检查结果还没有确认。可以重试确认，不需要重新上传。')
      }
    } catch (error) {
      if (!live()) return
      const code = error instanceof ApiError ? error.code : ''
      if (code === 'MEDIA_NOT_UPLOADED') {
        settle(id, 'interrupted', '存储没有收到这张照片。可以继续上传。', 'post')
      } else if (code === 'MEDIA_GRANT_EXPIRED') {
        settle(id, 'expired', '这次上传已过期，需要重新上传。', 'grant')
      } else if (code === 'MEDIA_DISABLED') {
        markDisabled()
        unconfirmed(describeError(error))
      } else if (code === 'MEDIA_NOT_FOUND') {
        settle(id, 'failed', describeError(error), 'grant')
      } else {
        unconfirmed(error instanceof ApiError && error.status > 0 && error.code !== 'RESULT_UNKNOWN' && error.status < 500
          ? `${describeError(error)} 照片已上传，可以重试确认。`
          : '照片已上传，但确认结果未知。可以重试确认，不需要重新上传。')
      }
    }
  }

  /** Files already checked by the editor for count; each is checked here for type and size before anything is sent. */
  function add(scope: string, files: File[]): UploadItem[] {
    if (!owner()) return []
    const added: UploadItem[] = []
    for (const file of files) {
      nextId += 1
      const id = `u${nextId}`
      const problem = checkUploadFile(file)
      const item: UploadItem = { id, scope, status: problem ? 'failed' : 'queued', mediaId: null, errorText: problem ?? '', local: !!problem }
      if (!problem) transients.set(id, { file, key: newIdempotencyKey(), form: null, controller: null, next: 'grant' })
      added.push(item)
    }
    items.value = [...items.value, ...added]
    pump()
    return added
  }

  /** The state's own safe retry, on the same key, form or media id. */
  function retry(id: string): void {
    const item = find(id)
    if (!item || !transients.has(id)) return
    if (item.status !== 'grant_unknown' && item.status !== 'interrupted' && item.status !== 'unconfirmed' && item.status !== 'failed') return
    patch(id, { status: 'queued', errorText: '' })
    pump()
  }

  /** Explicit only: an ended grant needs a new one, with a new key, for the same file. */
  function uploadAgain(id: string): void {
    const item = find(id)
    const transient = transients.get(id)
    if (!item || !transient || item.status !== 'expired') return
    transient.key = newIdempotencyKey()
    transient.form = null
    transient.next = 'grant'
    patch(id, { status: 'queued', mediaId: null, errorText: '' })
    pump()
  }

  return { items, reset, clearScope, remove, add, retry, uploadAgain }
})
