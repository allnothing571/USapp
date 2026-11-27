import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type RouteLocationNormalized, type Router } from 'vue-router'

vi.mock('@/api/messages', () => ({
  listMessages: vi.fn().mockResolvedValue({ messages: [], next_cursor: null }),
  createMessage: vi.fn(),
  getMessage: vi.fn().mockResolvedValue({
    message: {
      id: 'letter-0', author_id: 'partner', recipient_id: 'me',
      body: '早安', created_at: '2026-10-20T00:00:00Z', reply: null,
    },
  }),
  replyMessage: vi.fn(),
}))
vi.mock('@/api/notification-choices', () => ({ getNotificationChoices: vi.fn().mockResolvedValue({ choices: {} }) }))
vi.mock('@/api/avatar', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/avatar')>()),
  getAvatarAppearance: vi.fn().mockResolvedValue({ avatar_preset: 'p1', equipped_items: {}, version: 1, slots: {} }),
  listAvatarCatalog: vi.fn().mockResolvedValue({ items: [] }),
  listAvatarOwned: vi.fn().mockResolvedValue({ items: [] }),
}))
vi.mock('@/api/shared-room', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/shared-room')>()),
  getRoomPositions: vi.fn().mockResolvedValue({ side: 'left', paired: true, pending: null }),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn().mockResolvedValue({ wallet: { flower: 5, sunshine: 10 } }) }))
vi.mock('@/api/shared-garden', () => ({
  getSharedGarden: vi.fn().mockResolvedValue({
    watering_date: '2026-11-21', total_waterings: 10, watered_today: false, partner_watered_today: false, recent_waterings: [],
  }),
}))

import { recordBackNavigation, resetBackNavigation, useSmartBack } from '@/router/back-navigation'
import { useSessionStore } from '@/stores/session'
import AvatarView from '@/views/AvatarView.vue'
import MessageDetailView from '@/views/MessageDetailView.vue'
import SharedGardenView from '@/views/SharedGardenView.vue'

function fakeRoute(name: string, fullPath: string): RouteLocationNormalized {
  return {
    name,
    fullPath,
    path: fullPath.split('?')[0] ?? fullPath,
    hash: '',
    query: {},
    params: {},
    matched: [{} as never],
    meta: {},
    redirectedFrom: undefined,
  }
}

const Blank = { template: '<div />' }
function buildTestRouter(): Router {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/home', name: 'home', component: Blank },
      { path: '/tasks', name: 'tasks', component: Blank },
      { path: '/us', name: 'us', component: Blank },
      { path: '/us/messages', name: 'messages', component: Blank },
      { path: '/us/messages/:id', name: 'message-detail', component: MessageDetailView, props: true },
      { path: '/avatar', name: 'avatar-shop', component: AvatarView, props: { tab: 'shop' } },
      { path: '/avatar/wardrobe', name: 'avatar-wardrobe', component: AvatarView, props: { tab: 'wardrobe' } },
      { path: '/shared-room', name: 'shared-room', component: Blank },
      { path: '/shared-garden', name: 'shared-garden', component: SharedGardenView },
      { path: '/shared-garden/history', name: 'shared-garden-history', component: Blank },
      { path: '/notifications', name: 'notifications', component: Blank },
    ],
  })
  router.afterEach((to, from, failure) => {
    if (failure) return
    const session = useSessionStore()
    recordBackNavigation(to, from, session.user?.id ?? null)
  })
  return router
}

function signIn(id = 'me') {
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: '测试', created_at: '2026-10-01T00:00:00Z' },
    space: {
      id: 'sp-1',
      name: '暖屋',
      timezone: 'Asia/Shanghai',
      member_count: 2,
      created_at: '2026-10-01T00:00:00Z',
      members: [],
    },
  })
}

let wrapper: ReturnType<typeof mount> | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  resetBackNavigation()
  vi.clearAllMocks()
  signIn()
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  resetBackNavigation()
})

describe('Origin-aware smart back navigation', () => {
  it('falls back to canonical parent on cold load without stack', () => {
    const msgBack = useSmartBack('message-detail', 'messages')
    expect(msgBack.value).toEqual({
      to: { name: 'messages' }, group: 'messages', routeName: 'messages', label: '留言',
    })
    const gardenBack = useSmartBack('shared-garden', 'home')
    expect(gardenBack.value).toEqual({
      to: { name: 'home' }, group: 'home', routeName: 'home', label: '小空间',
    })
  })

  it('tracks Home -> MessageDetail back to /home and shows list shortcut, while Us -> Messages -> MessageDetail returns to Messages', async () => {
    const router = buildTestRouter()
    await router.push('/home')
    await router.push('/us/messages/letter-0')
    await flushPromises()

    wrapper = mount(MessageDetailView, {
      props: { id: 'letter-0' },
      global: { plugins: [router] },
    })
    await flushPromises()
    expect(wrapper.get('[data-test="back-to-us"]').text()).toBe('‹ 返回小空间')
    expect(wrapper.get('[data-test="back-to-us"]').attributes('href')).toBe('/home')
    expect(wrapper.find('[data-test="all-messages-link"]').exists()).toBe(true)
    wrapper.unmount()

    await router.push('/us')
    await router.push('/us/messages')
    await router.push('/us/messages/letter-0')
    await flushPromises()

    wrapper = mount(MessageDetailView, {
      props: { id: 'letter-0' },
      global: { plugins: [router] },
    })
    await flushPromises()
    expect(wrapper.get('[data-test="back-to-us"]').text()).toBe('‹ 返回留言')
    expect(wrapper.get('[data-test="back-to-us"]').attributes('href')).toBe('/us/messages')
    expect(wrapper.find('[data-test="all-messages-link"]').exists()).toBe(false)

    // Returning to Messages pops MessageDetail and preserves Us as parent (no ping-pong loop).
    await router.push('/us/messages')
    expect(useSmartBack('messages', 'us').value.routeName).toBe('us')
  })

  it('replaces detail frame when detouring via 全部留言 so Messages still returns to Home', () => {
    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('login', '/login'), 'u1')
    recordBackNavigation(fakeRoute('message-detail', '/us/messages/letter-0'), fakeRoute('home', '/home'), 'u1')
    expect(useSmartBack('message-detail', 'messages').value.routeName).toBe('home')

    recordBackNavigation(fakeRoute('messages', '/us/messages'), fakeRoute('message-detail', '/us/messages/letter-0'), 'u1')
    const back = useSmartBack('messages', 'us').value
    expect(back.routeName).toBe('home')
    expect(back.label).toBe('小空间')
  })

  it('preserves original entry across sibling avatar tab switches and removes redundant home button', async () => {
    const router = buildTestRouter()
    await router.push('/home')
    await router.push('/avatar/wardrobe')
    await router.push('/avatar')
    await flushPromises()

    wrapper = mount(AvatarView, {
      props: { tab: 'shop' },
      global: { plugins: [router] },
    })
    await flushPromises()
    expect(wrapper.get('[data-test="avatar-back"]').text()).toBe('‹ 返回小空间')
    expect(wrapper.get('[data-test="avatar-back"]').attributes('href')).toBe('/home')
    expect(wrapper.findAll('.avatar-nav a')).toHaveLength(2)
    wrapper.unmount()

    await router.push('/home')
    await router.push('/shared-room')
    await router.push('/avatar/wardrobe')
    await router.push('/avatar')
    await flushPromises()

    wrapper = mount(AvatarView, {
      props: { tab: 'shop' },
      global: { plugins: [router] },
    })
    await flushPromises()
    expect(wrapper.get('[data-test="avatar-back"]').text()).toBe('‹ 返回小房间')
    expect(wrapper.get('[data-test="avatar-back"]').attributes('href')).toBe('/shared-room')
  })

  it('dynamically sets SharedGarden back target between Home, SharedRoom, and GardenHistory', async () => {
    const router = buildTestRouter()
    await router.push('/home')
    await router.push('/shared-garden')
    await router.push('/shared-garden/history')
    await router.push('/shared-garden')
    await flushPromises()

    wrapper = mount(SharedGardenView, { global: { plugins: [router] } })
    await flushPromises()
    expect(wrapper.get('[data-test="garden-back"]').attributes('href')).toBe('/home')
    expect(wrapper.get('[data-test="garden-back"]').attributes('aria-label')).toBe('返回小空间')
    wrapper.unmount()

    await router.push('/home')
    await router.push('/shared-room')
    await router.push('/shared-garden')
    await flushPromises()

    wrapper = mount(SharedGardenView, { global: { plugins: [router] } })
    await flushPromises()
    expect(wrapper.get('[data-test="garden-back"]').attributes('href')).toBe('/shared-room')
    expect(wrapper.get('[data-test="garden-back"]').attributes('aria-label')).toBe('返回小房间')
  })

  it('tracks Tasks -> Notifications -> AnniversaryDetail -> Notifications cleanly', () => {
    recordBackNavigation(fakeRoute('tasks', '/tasks'), fakeRoute('home', '/home'), 'u1')
    recordBackNavigation(fakeRoute('notifications', '/notifications'), fakeRoute('tasks', '/tasks'), 'u1')
    expect(useSmartBack('notifications', 'home').value.label).toBe('任务')

    recordBackNavigation(fakeRoute('anniversary-detail', '/us/anniversaries/a1'), fakeRoute('notifications', '/notifications'), 'u1')
    expect(useSmartBack('anniversary-detail', 'anniversaries').value.label).toBe('通知')

    recordBackNavigation(fakeRoute('notifications', '/notifications'), fakeRoute('anniversary-detail', '/us/anniversaries/a1'), 'u1')
    expect(useSmartBack('notifications', 'home').value.label).toBe('任务')
  })

  it('replaces transient memory-new route so newly created MemoryDetail returns to Memories', () => {
    recordBackNavigation(fakeRoute('us', '/us'), fakeRoute('home', '/home'), 'u1')
    recordBackNavigation(fakeRoute('memories', '/us/memories'), fakeRoute('us', '/us'), 'u1')
    recordBackNavigation(fakeRoute('memory-new', '/us/memories/new'), fakeRoute('memories', '/us/memories'), 'u1')
    recordBackNavigation(fakeRoute('memory-detail', '/us/memories/mem-1'), fakeRoute('memory-new', '/us/memories/new'), 'u1')

    const back = useSmartBack('memory-detail', 'memories').value
    expect(back.routeName).toBe('memories')
    expect(back.label).toBe('照片回忆')
  })

  it('resets navigation stack on account switch or logout', () => {
    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('login', '/login'), 'u1')
    recordBackNavigation(fakeRoute('message-detail', '/us/messages/letter-0'), fakeRoute('home', '/home'), 'u1')
    expect(useSmartBack('message-detail', 'messages').value.routeName).toBe('home')

    recordBackNavigation(fakeRoute('message-detail', '/us/messages/letter-0'), fakeRoute('home', '/home'), 'u2')
    expect(useSmartBack('message-detail', 'messages').value.routeName).toBe('messages')
  })
})
