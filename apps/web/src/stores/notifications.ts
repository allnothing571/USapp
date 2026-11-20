import { defineStore } from 'pinia'
import { ref } from 'vue'
import { getUnreadCount, isNotificationForUser, listNotifications, readNotification, type Notification } from '@/api/notifications'
import { describeError } from '@/api/error-messages'
import { useSessionStore } from './session'

export const useNotificationsStore = defineStore('notifications', () => {
  const items = ref<Notification[]>([])
  const status = ref<'idle' | 'loading' | 'ready' | 'error'>('idle')
  const errorText = ref('')
  const nextCursor = ref<string | null>(null)
  const loadingMore = ref(false)
  const moreError = ref('')
  const unreadOnly = ref(false)
  const unreadCount = ref<number | null>(null)
  const countError = ref('')
  const reading = ref<Record<string, { status: 'submitting' | 'unknown'; error: string }>>({})
  let generation = 0, listSeq = 0, countSeq = 0, receiptEpoch = 0
  const confirmed = new Map<string, string>()
  function owner(): string | null { return useSessionStore().user?.id ?? null }
  function current(g: number, u: string | null): boolean { return u !== null && g === generation && owner() === u }
  function reset(): void {
    ++generation; ++listSeq; ++countSeq; ++receiptEpoch
    items.value = []; status.value = 'idle'; errorText.value = ''; nextCursor.value = null
    loadingMore.value = false; moreError.value = ''; unreadOnly.value = false
    unreadCount.value = null; countError.value = ''; reading.value = {}; confirmed.clear()
  }
  function merge(rows: Notification[]): Notification[] {
    return rows.map(n => ({ ...n, read_at: confirmed.get(n.id) ?? n.read_at }))
      .filter(n => !unreadOnly.value || !n.read_at)
  }
  async function loadCount(): Promise<void> {
    const g = generation, u = owner(), seq = ++countSeq, epoch = receiptEpoch
    if (!u) return
    try {
      const result = await getUnreadCount()
      if (!current(g, u) || seq !== countSeq || epoch !== receiptEpoch) return
      if (!Number.isSafeInteger(result.unread_count) || result.unread_count < 0) throw new Error('通知计数响应无效')
      unreadCount.value = result.unread_count; countError.value = ''
    } catch (e) {
      if (!current(g, u) || seq !== countSeq || epoch !== receiptEpoch) return
      unreadCount.value = null; countError.value = describeError(e)
    }
  }
  async function load(onlyUnread = unreadOnly.value): Promise<void> {
    const g = generation, u = owner(), seq = ++listSeq
    if (!u) return
    unreadOnly.value = onlyUnread; status.value = 'loading'; errorText.value = ''
    items.value = []; nextCursor.value = null; loadingMore.value = false; moreError.value = ''
    try {
      const result = await listNotifications(null, onlyUnread)
      if (!current(g, u) || seq !== listSeq) return
      if (!result.notifications.every(n => isNotificationForUser(n, u!))) throw new Error('通知响应无效')
      items.value = merge(result.notifications); nextCursor.value = result.next_cursor; status.value = 'ready'
    } catch (e) {
      if (!current(g, u) || seq !== listSeq) return
      status.value = 'error'; errorText.value = describeError(e)
    }
  }
  async function loadMore(): Promise<void> {
    if (!nextCursor.value || loadingMore.value || status.value !== 'ready') return
    const g = generation, u = owner(), seq = listSeq, cursor = nextCursor.value, filter = unreadOnly.value
    loadingMore.value = true; moreError.value = ''
    try {
      const result = await listNotifications(cursor, filter)
      if (!current(g, u) || seq !== listSeq) return
      if (!result.notifications.every(n => isNotificationForUser(n, u!))) throw new Error('通知响应无效')
      const known = new Set(items.value.map(n => n.id))
      items.value = merge([...items.value, ...result.notifications.filter(n => !known.has(n.id))])
      nextCursor.value = result.next_cursor
    } catch (e) {
      if (current(g, u) && seq === listSeq) moreError.value = describeError(e)
    } finally { if (current(g, u) && seq === listSeq) loadingMore.value = false }
  }
  async function markRead(id: string): Promise<boolean> {
    if (reading.value[id]?.status === 'submitting') return false
    const g = generation, u = owner()
    if (!u) return false
    reading.value = { ...reading.value, [id]: { status: 'submitting', error: '' } }
    try {
      await useSessionStore().ensureCsrfToken()
      if (!current(g, u)) return false
      const result = await readNotification(id)
      if (!current(g, u)) return false
      if (result.receipt?.notification_id !== id || typeof result.receipt.read_at !== 'string' || !result.receipt.read_at)
        throw new Error('已读结果无法确认')
      confirmed.set(id, result.receipt.read_at)
      const pending = { ...reading.value }; delete pending[id]; reading.value = pending
      items.value = merge(items.value)
      ++receiptEpoch; ++countSeq; unreadCount.value = null
      void loadCount()
      return true
    } catch (e) {
      if (current(g, u)) reading.value = { ...reading.value, [id]: { status: 'unknown', error: describeError(e) } }
      return false
    }
  }
  return { items, status, errorText, nextCursor, loadingMore, moreError, unreadOnly, unreadCount,
    countError, reading, reset, load, loadMore, loadCount, markRead }
})
