import { defineStore } from 'pinia'
import { ref } from 'vue'
import { ApiError, getCsrfToken, OfflineMutationBlockedError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { isRecovery, isRetained, listMemoryDeletions, listRetainedPhotos, sendMemoryDeletion, validMemoryDeleteResponse,
  type MemoryDeleteCommand, type MemoryRecovery, type RetainedPhoto } from '@/api/memory-deletions'
import { newIdempotencyKey } from '@/api/tasks'
import { useMemoriesStore } from './memories'
import { useSessionStore } from './session'
import { useSharedPlansStore } from './shared-plans'

interface Action { command: MemoryDeleteCommand; key: string; state: 'sending' | 'unknown' | 'not_sent'; error: string; uncertain: boolean }
export const useMemoryDeletionsStore = defineStore('memory-deletions', () => {
  const recovery = ref<MemoryRecovery[]>([])
  const retained = ref<RetainedPhoto[]>([])
  const cursor = ref<string | null>(null)
  const loading = ref(false)
  const error = ref('')
  const actions = ref<Record<string, Action>>({})
  const notice = ref('')
  const page = ref<'recovery' | 'retained' | null>(null)
  let generation = 0
  let read = 0
  function reset(): void {
    generation++
    read++
    recovery.value = []
    retained.value = []
    cursor.value = null
    page.value = null
    loading.value = false
    error.value = ''
    actions.value = {}
    notice.value = ''
  }
  function close(): void {
    read++
    page.value = null
    recovery.value = []
    retained.value = []
    cursor.value = null
    loading.value = false
  }
  async function load(kind: 'recovery' | 'retained', more = false): Promise<void> {
    if (more && (loading.value || !cursor.value || page.value !== kind)) return
    const session = useSessionStore()
    const owner = session.user?.id
    if (!owner) return
    const epoch = generation
    const seq = ++read
    const before = more ? cursor.value : null
    const current = () => epoch === generation && owner === session.user?.id && seq === read
    page.value = kind
    loading.value = true
    error.value = ''
    if (!more) { recovery.value = []; retained.value = [] }
    try {
      const result = kind === 'recovery' ? await listMemoryDeletions(before) : await listRetainedPhotos(before)
      if (!current()) return
      if (result.next_cursor !== null && (typeof result.next_cursor !== 'string' || !result.next_cursor || result.next_cursor === before)) {
        throw new Error('分页响应无效，请重新加载。')
      }
      if (kind === 'recovery') {
        if (!('deletions' in result) || !Array.isArray(result.deletions) || !result.deletions.every(isRecovery)) throw new Error('恢复列表响应无效。')
        const rows = more ? [...recovery.value, ...result.deletions] : result.deletions
        recovery.value = [...new Map(rows.map(row => [row.deletion_id, row])).values()]
      } else {
        if (!('photos' in result) || !Array.isArray(result.photos) || !result.photos.every(isRetained)) throw new Error('保留照片列表响应无效。')
        const rows = more ? [...retained.value, ...result.photos] : result.photos
        retained.value = [...new Map(rows.map(row => [row.media_id, row])).values()]
      }
      cursor.value = result.next_cursor
    } catch (cause) {
      if (current()) { error.value = describeError(cause); if (!more) cursor.value = null }
    } finally {
      if (current()) loading.value = false
    }
  }
  function refresh(memoryId: string, hide: boolean): void {
    useMemoriesStore().refreshAfterMemoryChange(memoryId, hide)
    const plans = useSharedPlansStore()
    for (const [id, detail] of Object.entries(plans.details)) {
      if (detail.plan?.memory_id === memoryId || detail.plan?.memory_unavailable) void plans.loadDetail(id)
    }
    if (page.value) void load(page.value)
  }
  async function execute(slot: string): Promise<void> {
    const action = actions.value[slot]
    const session = useSessionStore()
    const owner = session.user?.id
    const epoch = generation
    if (!action || action.state === 'sending' || !owner) return
    const current = () => epoch === generation && owner === session.user?.id
    action.state = 'sending'
    action.error = ''
    try {
      await session.ensureCsrfToken()
      if (!current()) return
      if (!getCsrfToken()) throw new OfflineMutationBlockedError('会话未确认，请重新登录后重试。')
      const result = await sendMemoryDeletion(action.command, action.key)
      if (!current()) return
      if (!validMemoryDeleteResponse(result, action.command, owner)) throw new Error('服务器响应无法确认。')
      delete actions.value[slot]
      notice.value = action.command.kind === 'request' ? '已确认这条删除申请，请以重新读取的状态为准。' :
        action.command.kind === 'undo' ? '已确认撤销该次删除。回忆及照片当前是否可用，请以重新读取为准。' :
        action.command.kind === 'decide' && action.command.outcome !== 'approve' ? '已确认处理这条删除申请。' :
        '已确认这次删除记录，可在“最近删除的回忆”中查看当前可撤销的轮次。'
      refresh(action.command.memoryId, action.command.kind === 'delete' ||
        (action.command.kind === 'decide' && action.command.outcome === 'approve'))
    } catch (cause) {
      if (!current()) return
      const code = cause instanceof ApiError ? cause.code : ''
      const retireDelete = action.command.kind === 'delete' && code === 'MEMORY_ALREADY_DELETED'
      const closedUndo = action.command.kind === 'undo' &&
        ['MEMORY_UNDO_CLOSED', 'MEMORY_DELETION_NOT_ACTIVE'].includes(code)
      if (retireDelete || closedUndo || (!action.uncertain && cause instanceof ApiError && cause.status >= 400 && cause.status < 500)) {
        delete actions.value[slot]
        notice.value = retireDelete ? '回忆目前已删除，已停止重试。若之后恢复，再次删除需要重新确认。' : describeError(cause)
        refresh(action.command.memoryId, retireDelete || code === 'MEMORY_NOT_FOUND')
      } else {
        if (!(cause instanceof OfflineMutationBlockedError)) action.uncertain = true
        action.state = action.uncertain ? 'unknown' : 'not_sent'
        action.error = action.uncertain ? '结果未确认，请保留原请求并点击重试。' : describeError(cause)
      }
    }
  }
  function submit(command: MemoryDeleteCommand): Promise<void> {
    const slot = command.memoryId
    if (!useSessionStore().user || actions.value[slot]) return Promise.resolve()
    actions.value[slot] = { command: JSON.parse(JSON.stringify(command)) as MemoryDeleteCommand, key: newIdempotencyKey(),
      state: 'not_sent', error: '', uncertain: false }
    return execute(slot)
  }
  return { recovery, retained, cursor, loading, error, actions, notice, page, reset, close, load, submit, retry: execute }
})
