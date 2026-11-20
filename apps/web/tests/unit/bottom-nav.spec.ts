/**
 * M10 navigation: Home / Tasks / Rewards / Me with an accessible current page, shown only to an
 * authenticated member on those four pages; the router keeps its authentication and space guards for
 * /me; the app shell keeps its offline, update and install notices next to the navigation.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

vi.mock('@/pwa/register-sw', () => ({
  setupServiceWorker: () => ({ needRefresh: ref(true), offlineReady: ref(false), update: vi.fn(), dismiss: vi.fn() }),
}))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))

import * as authApi from '@/api/auth'
import type { SessionBootstrap } from '@/api/types'
import App from '@/App.vue'
import BottomNav from '@/components/BottomNav.vue'
import appRouter from '@/router'
import { useSessionStore } from '@/stores/session'

const Blank = { template: '<div class="blank" />' }

function memberSnapshot(withSpace = true): SessionBootstrap {
  return {
    authenticated: true, csrf_token: 'csrf',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-26T00:00:00Z' },
    space: withSpace
      ? { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-26T00:00:00Z', members: [] }
      : null,
  }
}

function testRouter(): Router {
  return createRouter({
    history: createMemoryHistory(),
    routes: [
      ...['home', 'tasks', 'rewards', 'us', 'me', 'login', 'space-create', 'notifications'].map((name) => ({ path: `/${name}`, name, component: Blank })),
      { path: '/us/messages/:id', name: 'message-detail', component: Blank },
      { path: '/us/anniversaries/:id', name: 'anniversary-detail', component: Blank },
    ],
  })
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

async function mountNav(path: string) {
  const router = testRouter()
  await router.push(path)
  wrapper = mount(BottomNav, { global: { plugins: [router] } })
  await flushPromises()
  return { router, w: wrapper }
}

describe('bottom navigation', () => {
  it('links the five destinations and marks the current one', async () => {
    useSessionStore().applyBootstrap(memberSnapshot())
    const { router, w } = await mountNav('/home')
    const nav = w.find('nav[aria-label="主导航"]')
    expect(nav.findAll('a').map((a) => a.text())).toEqual(['首页', '任务', '奖励', '我们', '我的'])
    expect(nav.findAll('a').map((a) => a.attributes('href'))).toEqual(['/home', '/tasks', '/rewards', '/us', '/me'])
    expect(w.find('[data-test="nav-home"]').attributes('aria-current')).toBe('page')
    await router.push('/me')
    await flushPromises()
    expect(w.find('[data-test="nav-home"]').attributes('aria-current')).toBeUndefined()
    expect(w.find('[data-test="nav-me"]').attributes('aria-current')).toBe('page')
    expect(w.findAll('[aria-current="page"]')).toHaveLength(1)
  })

  it.each([
    ['anonymous', '/home'],
    ['no-space', '/home'],
    ['member', '/login'],
    ['member', '/space-create'],
  ])('is hidden for %s on %s', async (who, path) => {
    if (who === 'member') useSessionStore().applyBootstrap(memberSnapshot())
    if (who === 'no-space') useSessionStore().applyBootstrap(memberSnapshot(false))
    if (who === 'anonymous') useSessionStore().applyBootstrap({ authenticated: false, csrf_token: 'pre' })
    const { w } = await mountNav(path)
    expect(w.find('nav').exists()).toBe(false)
  })

  it('disappears on logout', async () => {
    useSessionStore().applyBootstrap(memberSnapshot())
    const { w } = await mountNav('/tasks')
    expect(w.find('nav').exists()).toBe(true)
    vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
    await useSessionStore().logout()
    await flushPromises()
    expect(w.find('nav').exists()).toBe(false)
  })
})

describe('P2-1 Us destination', () => {
  it('stays visible on a message detail page and marks 我们 as the only current item', async () => {
    useSessionStore().applyBootstrap(memberSnapshot())
    const { router, w } = await mountNav('/us')
    expect(w.find('[data-test="nav-us"]').attributes('aria-current')).toBe('page')
    await router.push('/us/messages/m1')
    await flushPromises()
    expect(w.find('nav').exists()).toBe(true)
    expect(w.find('[data-test="nav-us"]').attributes('aria-current')).toBe('page')
    expect(w.find('[data-test="nav-us"]').classes()).toContain('bottom-nav__item--active')
    expect(w.findAll('[aria-current="page"]')).toHaveLength(1)
    // P2-2: the anniversary detail also belongs to Us.
    await router.push('/us/anniversaries/a1')
    await flushPromises()
    expect(w.find('nav').exists()).toBe(true)
    expect(w.find('[data-test="nav-us"]').attributes('aria-current')).toBe('page')
    expect(w.findAll('[aria-current="page"]')).toHaveLength(1)
  })
})

describe('router guards for /me', () => {
  it('anonymous users go to login; members without a space go to space creation', async () => {
    const scroll = vi.spyOn(window, 'scrollTo').mockImplementation(() => undefined)  // jsdom lacks scrollTo
    useSessionStore().applyBootstrap({ authenticated: false, csrf_token: 'pre' })
    await appRouter.push('/me')
    expect(appRouter.currentRoute.value.name).toBe('login')
    expect(appRouter.currentRoute.value.query.redirect).toBe('/me')
    useSessionStore().applyBootstrap(memberSnapshot(false))
    await appRouter.push('/me')
    expect(appRouter.currentRoute.value.name).toBe('space-create')
    useSessionStore().applyBootstrap(memberSnapshot())
    await appRouter.push('/me')
    expect(appRouter.currentRoute.value.name).toBe('me')
    // P2-1: the Us routes use the same global guards.
    useSessionStore().applyBootstrap({ authenticated: false, csrf_token: 'pre' })
    await appRouter.push('/us/messages/m1')
    expect(appRouter.currentRoute.value.name).toBe('login')
    await appRouter.push('/us/anniversaries/a1')
    expect(appRouter.currentRoute.value.name).toBe('login')
    useSessionStore().applyBootstrap(memberSnapshot(false))
    await appRouter.push('/us')
    expect(appRouter.currentRoute.value.name).toBe('space-create')
    await appRouter.push('/notifications')
    expect(appRouter.currentRoute.value.name).toBe('notifications')
    await new Promise((resolve) => setTimeout(resolve, 0))  // the router scrolls after navigation settles
    scroll.mockRestore()
  })
})

describe('app shell', () => {
  it('keeps the offline banner, update notice and install prompt alongside the navigation', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(memberSnapshot())
    useSessionStore().applyBootstrap(memberSnapshot())
    const router = testRouter()
    await router.push('/rewards')
    wrapper = mount(App, { global: { plugins: [router] } })
    await flushPromises()
    expect(wrapper.text()).toContain('有新版本可用')
    expect(wrapper.find('nav[aria-label="主导航"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="nav-rewards"]').attributes('aria-current')).toBe('page')
    const html = wrapper.html()
    expect(html.indexOf('app-main')).toBeLessThan(html.indexOf('bottom-nav'))
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => false })
    window.dispatchEvent(new Event('offline'))
    await flushPromises()
    expect(wrapper.find('.offline-banner').exists()).toBe(true)
    Reflect.deleteProperty(window.navigator, 'onLine')
  })
})
