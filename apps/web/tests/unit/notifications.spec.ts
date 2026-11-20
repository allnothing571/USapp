import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
vi.mock('@/api/notifications', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/notifications')>(),
  getUnreadCount: vi.fn(), listNotifications: vi.fn(), readNotification: vi.fn(),
}))
vi.mock('vue-router', () => ({ useRouter: () => ({ push: vi.fn() }) }))
vi.mock('@/api/auth', () => ({ bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn() }))
import { getUnreadCount, listNotifications, readNotification, notificationTarget, type Notification } from '@/api/notifications'
import { setCsrfToken } from '@/api/client'
import { useSessionStore } from '@/stores/session'
import { useNotificationsStore } from '@/stores/notifications'
import NotificationsView from '@/views/NotificationsView.vue'

function deferred<T>() { let resolve!: (v: T) => void; let reject!: (e: unknown) => void
  const promise = new Promise<T>((a, b) => { resolve = a; reject = b }); return { promise, resolve, reject } }
const row: Notification = { id: 'n1', event_type: 'redemption.requested', actor_id: 'other', target_kind: 'reward',
  target_id: 'r1', created_at: '2026-10-29T00:00:00Z', read_at: null }
function login(id = 'self') {
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: id, created_at: '' },
    space: { id: `space-${id}`, name: 'test', timezone: 'Asia/Shanghai', members: [], member_count: 2, created_at: '' } })
  setCsrfToken(`csrf-${id}`)
}
beforeEach(() => {
  setActivePinia(createPinia()); vi.resetAllMocks(); login()
  vi.mocked(listNotifications).mockResolvedValue({ notifications: [row], next_cursor: null })
  vi.mocked(getUnreadCount).mockResolvedValue({ unread_count: 0 })
  vi.mocked(readNotification).mockResolvedValue({ receipt: { notification_id: 'n1', read_at: '2026-10-29T01:00:00Z' } })
})
describe('notification isolation and read races', () => {
  it('a stale count cannot land after a confirmed receipt', async () => {
    const old = deferred<{ unread_count: number }>(), fresh = deferred<{ unread_count: number }>()
    vi.mocked(getUnreadCount).mockReturnValueOnce(old.promise).mockReturnValueOnce(fresh.promise)
    const store = useNotificationsStore(), pending = store.loadCount()
    await store.markRead('n1'); fresh.resolve({ unread_count: 0 }); await flushPromises()
    old.resolve({ unread_count: 12 }); await pending
    expect(store.unreadCount).toBe(0)
  })
  it('a stale list cannot hide a confirmed read', async () => {
    const old = deferred<{ notifications: Notification[]; next_cursor: null }>()
    vi.mocked(listNotifications).mockReturnValueOnce(old.promise)
    const store = useNotificationsStore(), pending = store.load()
    await store.markRead('n1'); old.resolve({ notifications: [row], next_cursor: null }); await pending
    expect(store.items[0]?.read_at).toBe('2026-10-29T01:00:00Z')
  })
  it('an unknown read retries the same notification, even after a later rejection', async () => {
    vi.mocked(readNotification).mockRejectedValueOnce(new Error('network')).mockRejectedValueOnce(new Error('403'))
      .mockResolvedValueOnce({ receipt: { notification_id: 'n1', read_at: '2026-10-29T01:00:00Z' } })
    const store = useNotificationsStore()
    expect(await store.markRead('n1')).toBe(false); expect(await store.markRead('n1')).toBe(false)
    expect(store.reading.n1?.status).toBe('unknown'); expect(await store.markRead('n1')).toBe(true)
    expect(vi.mocked(readNotification).mock.calls.map(args => args[0])).toEqual(['n1','n1','n1'])
  })
  it('malformed receipts never mark the row read', async () => {
    const store = useNotificationsStore(); await store.load()
    vi.mocked(readNotification).mockResolvedValue({ receipt: { notification_id: 'another', read_at: 'x' } })
    expect(await store.markRead('n1')).toBe(false); expect(store.items[0]?.read_at).toBeNull()
  })
  it.each(['resolve', 'reject'])('late list and count %s cannot affect another account', async outcome => {
    const list = deferred<{ notifications: Notification[]; next_cursor: null }>(), count = deferred<{ unread_count: number }>()
    vi.mocked(listNotifications).mockReturnValueOnce(list.promise); vi.mocked(getUnreadCount).mockReturnValueOnce(count.promise)
    const store = useNotificationsStore(), a = store.load(), b = store.loadCount(); login('B')
    if (outcome === 'resolve') { list.resolve({ notifications: [row], next_cursor: null }); count.resolve({ unread_count: 3 }) }
    else { list.reject(new Error('old')); count.reject(new Error('old')) }
    await Promise.all([a,b]); expect(store.items).toEqual([]); expect(store.unreadCount).toBeNull()
    expect(store.countError).toBe(''); expect(store.errorText).toBe('')
  })
  it('late receipt after switching accounts is discarded', async () => {
    const late = deferred<{ receipt: { notification_id: string; read_at: string } }>()
    vi.mocked(readNotification).mockReturnValueOnce(late.promise)
    const store = useNotificationsStore(), p = store.markRead('n1'); await flushPromises(); login('B')
    late.resolve({ receipt: { notification_id: 'n1', read_at: 'x' } }); expect(await p).toBe(false)
    expect(store.reading).toEqual({}); expect(getUnreadCount).not.toHaveBeenCalled()
  })
  it('a list refresh invalidates in-flight load-more and deduplicates pages', async () => {
    vi.mocked(listNotifications).mockResolvedValueOnce({ notifications: [row], next_cursor: 'page2' })
      .mockResolvedValueOnce({ notifications: [row, { ...row, id: 'n2' }], next_cursor: 'page3' })
    const store = useNotificationsStore(); await store.load(); await store.loadMore()
    expect(store.items.map(n => n.id)).toEqual(['n1','n2'])
    const late = deferred<{ notifications: Notification[]; next_cursor: null }>()
    vi.mocked(listNotifications).mockReturnValueOnce(late.promise)
    const p = store.loadMore(); await store.load(); late.resolve({ notifications: [{ ...row, id: 'old' }], next_cursor: null }); await p
    expect(store.items.map(n => n.id)).toEqual(['n1'])
  })
  it('count failure is not zero, and partial list failure is not empty', async () => {
    vi.mocked(getUnreadCount).mockRejectedValueOnce(new Error('offline'))
    vi.mocked(listNotifications).mockRejectedValueOnce(new Error('offline'))
    const store = useNotificationsStore(); await store.loadCount(); await store.load()
    expect(store.unreadCount).toBeNull(); expect(store.countError).not.toBe(''); expect(store.status).toBe('error')
  })
  it('unread-filter results merge locally confirmed receipts', async () => {
    await useNotificationsStore().markRead('n1'); await useNotificationsStore().load(true)
    expect(useNotificationsStore().items).toEqual([])
  })
  it('targets are real existing list/detail routes', () => {
    expect(notificationTarget(row)).toBe('/rewards')
    expect(notificationTarget({ ...row, target_kind: 'task' })).toBe('/tasks')
    expect(notificationTarget({ ...row, target_kind: 'anniversary' })).toBe('/us/anniversaries/r1')
  })
  it('view error does not render an empty inbox', async () => {
    vi.mocked(listNotifications).mockRejectedValueOnce(new Error('offline'))
    const wrapper = mount(NotificationsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises(); expect(wrapper.text()).not.toContain('当前列表没有通知'); expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    wrapper.unmount()
  })
  it('view coalesces focus/online refreshes and stops refreshing after unmount', async () => {
    const wrapper = mount(NotificationsView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(listNotifications).toHaveBeenCalledTimes(1)
    window.dispatchEvent(new Event('focus')); window.dispatchEvent(new Event('online'))
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expect(listNotifications).toHaveBeenCalledTimes(2)
    expect(getUnreadCount).toHaveBeenCalledTimes(2)
    wrapper.unmount(); window.dispatchEvent(new Event('focus')); await flushPromises()
    expect(listNotifications).toHaveBeenCalledTimes(2)
  })
})
