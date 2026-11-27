import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type RouteLocationNormalized, type Router } from 'vue-router'
import { ref } from 'vue'

vi.mock('@/pwa/register-sw', () => ({
  setupServiceWorker: () => ({ needRefresh: ref(false), offlineReady: ref(false), update: vi.fn(), dismiss: vi.fn() }),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(),
  deleteSession: vi.fn(),
  getMe: vi.fn(),
  updateMe: vi.fn(),
  createSession: vi.fn(),
  requestEmailCode: vi.fn(),
}))
vi.mock('@/api/notifications', () => ({
  unreadNotificationCount: vi.fn().mockResolvedValue(0),
  listNotifications: vi.fn().mockResolvedValue({ notifications: [], next_cursor: null }),
  markNotificationsRead: vi.fn(),
}))

import * as authApi from '@/api/auth'
import type { SessionBootstrap } from '@/api/types'
import App from '@/App.vue'
import {
  prepareRouteTransition,
  recordBackNavigation,
  resetBackNavigation,
  routeTransitionKey,
  useRouteTransition,
} from '@/router/back-navigation'
import { useSessionStore } from '@/stores/session'

function fakeRoute(name: string, path: string, matchedCount = 1): RouteLocationNormalized {
  return {
    name,
    path,
    fullPath: path,
    hash: '',
    query: {},
    params: {},
    matched: Array.from({ length: matchedCount }, () => ({} as never)),
    meta: {},
    redirectedFrom: undefined,
  }
}

const Blank = { template: '<div class="view-stub">{{ $route.name }}</div>' }

function buildRouter(): Router {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/login', name: 'login', component: Blank },
      { path: '/notifications', name: 'notifications', component: Blank },
      { path: '/home', name: 'home', component: Blank },
      { path: '/tasks', name: 'tasks', component: Blank },
      { path: '/tasks/open', name: 'tasks-open', component: Blank },
      { path: '/rewards', name: 'rewards', component: Blank },
      { path: '/us', name: 'us', component: Blank },
      { path: '/us/messages', name: 'messages', component: Blank },
      { path: '/us/messages/:id', name: 'message-detail', component: Blank },
      { path: '/shared-room', name: 'shared-room', component: Blank },
      { path: '/shared-garden', name: 'shared-garden', component: Blank },
      { path: '/shared-garden/history', name: 'shared-garden-history', component: Blank },
      { path: '/avatar', name: 'avatar-shop', component: Blank },
      { path: '/avatar/wardrobe', name: 'avatar-wardrobe', component: Blank },
      { path: '/me', name: 'me', component: Blank },
    ],
  })
  router.beforeResolve((to, from) => {
    const session = useSessionStore()
    prepareRouteTransition(to, from, session.user?.id ?? null)
  })
  router.afterEach((to, from, failure) => {
    if (failure) return
    const session = useSessionStore()
    recordBackNavigation(to, from, session.user?.id ?? null)
  })
  return router
}

function signIn(id = 'u1') {
  const snapshot: SessionBootstrap = {
    authenticated: true,
    csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.test`, display_name: '甲', created_at: '2026-10-01T00:00:00Z' },
    space: {
      id: 'sp-1',
      name: '暖屋',
      timezone: 'Asia/Shanghai',
      member_count: 2,
      created_at: '2026-10-01T00:00:00Z',
      members: [],
    },
  }
  vi.mocked(authApi.bootstrapSession).mockResolvedValue(snapshot)
  useSessionStore().applyBootstrap(snapshot)
}

beforeEach(() => {
  setActivePinia(createPinia())
  resetBackNavigation()
  vi.clearAllMocks()
  signIn()
})

afterEach(() => {
  resetBackNavigation()
})

describe('Global route transition classifier', () => {
  it('returns route-none on cold load or auth routes and route-tab across level-1 hubs', () => {
    const transition = useRouteTransition()
    expect(transition.value).toBe('route-none')

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('', '/', 0), 'u1')
    expect(transition.value).toBe('route-none')

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('login', '/login'), 'u1')
    expect(transition.value).toBe('route-none')

    recordBackNavigation(fakeRoute('tasks', '/tasks'), fakeRoute('home', '/home'), 'u1')
    expect(transition.value).toBe('route-tab')

    recordBackNavigation(fakeRoute('me', '/me'), fakeRoute('tasks', '/tasks'), 'u1')
    expect(transition.value).toBe('route-tab')
  })

  it('distinguishes forward drill-down and backward return across multi-level stacks', () => {
    const transition = useRouteTransition()

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('login', '/login'), 'u1')
    recordBackNavigation(fakeRoute('shared-room', '/shared-room'), fakeRoute('home', '/home'), 'u1')
    expect(transition.value).toBe('route-forward')

    recordBackNavigation(fakeRoute('shared-garden', '/shared-garden'), fakeRoute('shared-room', '/shared-room'), 'u1')
    expect(transition.value).toBe('route-forward')

    recordBackNavigation(
      fakeRoute('shared-garden-history', '/shared-garden/history'),
      fakeRoute('shared-garden', '/shared-garden'),
      'u1',
    )
    expect(transition.value).toBe('route-forward')

    recordBackNavigation(
      fakeRoute('shared-garden', '/shared-garden'),
      fakeRoute('shared-garden-history', '/shared-garden/history'),
      'u1',
    )
    expect(transition.value).toBe('route-back')

    recordBackNavigation(fakeRoute('shared-room', '/shared-room'), fakeRoute('shared-garden', '/shared-garden'), 'u1')
    expect(transition.value).toBe('route-back')

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('shared-room', '/shared-room'), 'u1')
    expect(transition.value).toBe('route-back')
  })

  it('uses route-back when stepping from deep-linked detail to its list parent or hub', () => {
    const transition = useRouteTransition()

    recordBackNavigation(fakeRoute('message-detail', '/us/messages/m1'), fakeRoute('', '/', 0), 'u1')
    expect(transition.value).toBe('route-none')

    recordBackNavigation(fakeRoute('messages', '/us/messages'), fakeRoute('message-detail', '/us/messages/m1'), 'u1')
    expect(transition.value).toBe('route-back')

    recordBackNavigation(fakeRoute('us', '/us'), fakeRoute('messages', '/us/messages'), 'u1')
    expect(transition.value).toBe('route-back')
  })

  it('preserves AvatarView instance key and uses route-tab between shop and wardrobe', () => {
    const transition = useRouteTransition()
    expect(routeTransitionKey(fakeRoute('avatar-shop', '/avatar'))).toBe('avatar')
    expect(routeTransitionKey(fakeRoute('avatar-wardrobe', '/avatar/wardrobe'))).toBe('avatar')
    expect(routeTransitionKey(fakeRoute('tasks-open', '/tasks/open'))).toBe('/tasks/open')

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('login', '/login'), 'u1')
    recordBackNavigation(fakeRoute('avatar-wardrobe', '/avatar/wardrobe'), fakeRoute('home', '/home'), 'u1')
    expect(transition.value).toBe('route-forward')

    recordBackNavigation(fakeRoute('avatar-shop', '/avatar'), fakeRoute('avatar-wardrobe', '/avatar/wardrobe'), 'u1')
    expect(transition.value).toBe('route-tab')

    recordBackNavigation(fakeRoute('home', '/home'), fakeRoute('avatar-shop', '/avatar'), 'u1')
    expect(transition.value).toBe('route-back')
  })

  it('drives :data-transition on App.vue route-stage as routes navigate forward, across tabs, and back', async () => {
    const router = buildRouter()
    await router.push('/home')
    const wrapper = mount(App, {
      global: {
        plugins: [router],
      },
    })
    await flushPromises()
    expect(wrapper.get('.route-stage').attributes('data-transition')).toBe('route-none')

    await router.push('/tasks')
    await flushPromises()
    expect(wrapper.get('.route-stage').attributes('data-transition')).toBe('route-tab')

    await router.push('/tasks/open')
    await flushPromises()
    expect(wrapper.get('.route-stage').attributes('data-transition')).toBe('route-forward')
    expect(wrapper.find('[data-test="bottom-nav"]').exists()).toBe(false)

    await router.push('/tasks')
    await flushPromises()
    expect(wrapper.get('.route-stage').attributes('data-transition')).toBe('route-back')
    expect(wrapper.find('[data-test="bottom-nav"]').exists()).toBe(true)

    wrapper.unmount()
  })
})
