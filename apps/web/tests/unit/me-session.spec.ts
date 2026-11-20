/**
 * M10 review repair: identity isolation for nickname updates, space/invitation reads, logout and the
 * Me page. Every case resolves (success) or rejects (failure) an old request after the identity changed.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const replace = vi.fn()
vi.mock('vue-router', () => ({ useRouter: () => ({ replace }) }))
vi.mock('@/api/space', () => ({ listSpaceInvites: vi.fn(), getCurrentSpace: vi.fn() }))
vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(), deleteSession: vi.fn(), getMe: vi.fn(), updateMe: vi.fn(),
  createSession: vi.fn(), requestEmailCode: vi.fn(),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))

import { ApiError, getCsrfToken } from '@/api/client'
import * as authApi from '@/api/auth'
import { getCurrentSpace, listSpaceInvites } from '@/api/space'
import type { InviteListResponse, MeResponse, SessionBootstrap, Space, SpaceResponse } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'
import { StaleSessionError } from '@/stores/stale'
import MeView from '@/views/MeView.vue'

function space(id: string, name: string): Space {
  return { id, name, timezone: 'Asia/Shanghai', member_count: 1, created_at: '2026-10-26T00:00:00Z',
    members: [{ user_id: 'x', slot: 1, display_name: name, joined_at: '2026-10-26T00:00:00Z' }] }
}

function snapshot(id: string, withSpace = true): SessionBootstrap {
  return { authenticated: true, csrf_token: `csrf-${id}`,
    user: { id, email: `${id}@example.com`, display_name: `昵称${id}`, created_at: '2026-10-26T00:00:00Z' },
    space: withSpace ? space(`space-${id}`, `空间${id}`) : null }
}

function me(id: string, name: string): MeResponse {
  return { user: { id, email: `${id}@example.com`, display_name: name, created_at: '2026-10-26T00:00:00Z' },
    space: space(`space-${id}`, `空间${id}`) }
}

function invites(id: string): InviteListResponse {
  return { invites: [{ id, expires_at: '2099-01-01T00:00:00Z', created_at: '2026-10-26T00:00:00Z', max_uses: 1, used_count: 0 }] }
}

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(listSpaceInvites).mockResolvedValue({ invites: [] })
  useSessionStore().applyBootstrap(snapshot('a'))
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.restoreAllMocks()
})

describe('nickname update across identity changes', () => {
  it.each([
    ['account switch', 'success'], ['account switch', 'failure'], ['logout', 'success'], ['logout', 'failure'],
  ])('a late %s %s never restores the previous user', async (boundary, outcome) => {
    const session = useSessionStore()
    const request = deferred<MeResponse>()
    vi.mocked(authApi.updateMe).mockReturnValueOnce(request.promise)
    const pending = session.updateDisplayName('旧草稿')
    await flushPromises()
    expect(authApi.updateMe).toHaveBeenCalledTimes(1)
    if (boundary === 'logout') {
      vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
      await session.logout()
    } else {
      session.applyBootstrap(snapshot('b'))
    }
    if (outcome === 'success') request.resolve(me('a', '旧草稿'))
    else request.reject(new ApiError('VALIDATION_ERROR', 'bad', 400))
    await expect(pending).rejects.toBeInstanceOf(StaleSessionError)
    if (boundary === 'logout') {
      expect(session.user).toBeNull()
      expect(session.isAuthenticated).toBe(false)
      expect(useSpaceStore().space).toBeNull()
    } else {
      expect(session.user?.id).toBe('b')
      expect(session.user?.display_name).toBe('昵称b')
      expect(useSpaceStore().space?.id).toBe('space-b')
    }
  })

  it.each(['new account', 'failure'])('an identity change while waiting for CSRF never submits the old draft (%s)', async (kind) => {
    const session = useSessionStore()
    session.csrfToken = null
    const boot = deferred<SessionBootstrap>()
    vi.mocked(authApi.bootstrapSession).mockReturnValueOnce(boot.promise)
    vi.spyOn(console, 'warn').mockImplementation(() => undefined)
    const pending = session.updateDisplayName('旧草稿')
    if (kind === 'new account') boot.resolve(snapshot('b'))
    else boot.reject(new Error('offline'))  // bootstrap failure resets to anonymous
    await expect(pending).rejects.toBeInstanceOf(StaleSessionError)
    expect(authApi.updateMe).not.toHaveBeenCalled()
    expect(session.user?.display_name ?? null).toBe(kind === 'new account' ? '昵称b' : null)
  })

  it('still updates the same user after fetching a CSRF token (existing behaviour)', async () => {
    const session = useSessionStore()
    session.csrfToken = null
    vi.mocked(authApi.bootstrapSession).mockResolvedValueOnce(snapshot('a'))
    vi.mocked(authApi.updateMe).mockResolvedValueOnce(me('a', '新昵称'))
    await session.updateDisplayName('新昵称')
    expect(authApi.updateMe).toHaveBeenCalledWith({ display_name: '新昵称' })
    expect(session.user?.display_name).toBe('新昵称')
  })
})

describe('a stale bootstrap started by a nickname update', () => {
  it.each([
    ['account switch', 'resolve A'], ['account switch', 'reject'], ['logout', 'resolve A'], ['logout', 'reject'],
  ])('after an independent %s, the old bootstrap (%s) changes nothing', async (boundary, outcome) => {
    const session = useSessionStore()
    const spaceStore = useSpaceStore()
    session.csrfToken = null
    const oldBoot = deferred<SessionBootstrap>()
    vi.mocked(authApi.bootstrapSession).mockReturnValueOnce(oldBoot.promise)
    vi.spyOn(console, 'warn').mockImplementation(() => undefined)
    const pending = session.updateDisplayName('旧草稿')  // A, no CSRF: waits on the old bootstrap
    await flushPromises()
    expect(authApi.bootstrapSession).toHaveBeenCalledTimes(1)

    if (boundary === 'logout') {
      vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
      await session.logout()
    } else {
      session.applyBootstrap(snapshot('b'))  // e.g. B logged in independently
      vi.mocked(listSpaceInvites).mockResolvedValueOnce(invites('b-invite'))
      await spaceStore.refreshInvites()
    }
    const before = {
      status: session.status, user: session.user, space: session.space, storeSpace: spaceStore.space,
      invites: [...spaceStore.invites], csrf: session.csrfToken, clientCsrf: getCsrfToken(), bootstrapped: session.bootstrapped,
    }

    if (outcome === 'resolve A') oldBoot.resolve(snapshot('a'))
    else oldBoot.reject(new Error('offline'))
    await expect(pending).rejects.toBeInstanceOf(StaleSessionError)
    await flushPromises()

    expect(authApi.updateMe).not.toHaveBeenCalled()
    expect({
      status: session.status, user: session.user, space: session.space, storeSpace: spaceStore.space,
      invites: [...spaceStore.invites], csrf: session.csrfToken, clientCsrf: getCsrfToken(), bootstrapped: session.bootstrapped,
    }).toEqual(before)
    if (boundary === 'logout') {
      expect(before).toMatchObject({ status: 'anonymous', user: null, space: null, storeSpace: null, invites: [], csrf: null })
    } else {
      expect(before).toMatchObject({ status: 'authenticated', csrf: 'csrf-b', clientCsrf: 'csrf-b' })
      expect(before.user?.id).toBe('b')
      expect(before.invites.map((invite) => invite.id)).toEqual(['b-invite'])
    }
  })

  it('a stale bootstrap is not reused and its completion cannot clear a newer in-flight bootstrap', async () => {
    const session = useSessionStore()
    session.csrfToken = null
    const oldBoot = deferred<SessionBootstrap>()
    const newBoot = deferred<SessionBootstrap>()
    vi.mocked(authApi.bootstrapSession).mockReturnValueOnce(oldBoot.promise).mockReturnValueOnce(newBoot.promise)
    const pending = session.updateDisplayName('旧草稿')
    await flushPromises()
    vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
    await session.logout()
    const newer = session.bootstrap(true)  // anonymous now: must start a fresh request, not reuse A's
    expect(authApi.bootstrapSession).toHaveBeenCalledTimes(2)
    oldBoot.resolve(snapshot('a'))
    await expect(pending).rejects.toBeInstanceOf(StaleSessionError)
    // The old finally did not clear the newer in-flight entry: another caller joins it (no third request)
    // and does not settle before it. (Pinia wraps action promises, so identity is not comparable.)
    let joined = false
    const again = session.bootstrap().then(() => { joined = true })
    await flushPromises()
    expect(authApi.bootstrapSession).toHaveBeenCalledTimes(2)
    expect(joined).toBe(false)
    expect(session.isAuthenticated).toBe(false)
    newBoot.resolve(snapshot('b'))
    await newer
    await again
    expect(joined).toBe(true)
    expect(session.user?.id).toBe('b')
    expect(session.bootstrapped).toBe(true)
    expect(authApi.updateMe).not.toHaveBeenCalled()
  })

  it('normal bootstrap and concurrent callers still share one request', async () => {
    setActivePinia(createPinia())
    const session = useSessionStore()
    vi.mocked(authApi.bootstrapSession).mockResolvedValueOnce(snapshot('a'))
    const first = session.bootstrap()
    const second = session.bootstrap()
    await Promise.all([first, second])
    expect(authApi.bootstrapSession).toHaveBeenCalledTimes(1)
    expect(session.user?.id).toBe('a')
    expect(session.bootstrapped).toBe(true)
    await session.bootstrap()
    expect(authApi.bootstrapSession).toHaveBeenCalledTimes(1)
  })
})

describe('space and invitations across identity changes', () => {
  it('an account switch clears old invitations', async () => {
    const spaceStore = useSpaceStore()
    vi.mocked(listSpaceInvites).mockResolvedValueOnce(invites('old-invite'))
    await spaceStore.refreshInvites()
    expect(spaceStore.invites).toHaveLength(1)
    useSessionStore().applyBootstrap(snapshot('b'))
    expect(spaceStore.invites).toEqual([])
    expect(spaceStore.space?.id).toBe('space-b')
  })

  it.each([['account switch', 'success'], ['account switch', 'failure'], ['logout', 'success'], ['logout', 'failure']])(
    'late %s %s of space and invitation reads cannot repopulate state', async (boundary, outcome) => {
      const spaceStore = useSpaceStore()
      const current = deferred<SpaceResponse>()
      const list = deferred<InviteListResponse>()
      vi.mocked(getCurrentSpace).mockReturnValueOnce(current.promise)
      vi.mocked(listSpaceInvites).mockReturnValueOnce(list.promise)
      const loading = spaceStore.loadCurrent()
      const refreshing = spaceStore.refreshInvites()
      if (boundary === 'logout') {
        vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
        await useSessionStore().logout()
      } else {
        useSessionStore().applyBootstrap(snapshot('b'))
      }
      if (outcome === 'success') {
        current.resolve({ space: space('space-a', '旧空间') })
        list.resolve(invites('old-invite'))
      } else {
        current.reject(new ApiError('INTERNAL_ERROR', 'down', 500))
        list.reject(new ApiError('INTERNAL_ERROR', 'down', 500))
      }
      await expect(loading).rejects.toBeInstanceOf(StaleSessionError)
      await expect(refreshing).rejects.toBeInstanceOf(StaleSessionError)
      expect(spaceStore.invites).toEqual([])
      expect(spaceStore.errorCode).toBeNull()
      expect(spaceStore.loading).toBe(false)
      expect(spaceStore.space?.id ?? null).toBe(boundary === 'logout' ? null : 'space-b')
    })
})

describe('me page across identity changes', () => {
  function mountMe() {
    wrapper = mount(MeView, { global: { stubs: { RouterLink: true } } })
    return wrapper
  }

  it('clears the nickname draft and error on an identity change', async () => {
    const w = mountMe()
    await flushPromises()
    await w.find('[data-test="edit-name"]').trigger('click')
    await w.find('#me-display-name').setValue('   ')
    await w.find('form').trigger('submit')
    expect(w.find('[role="alert"]').exists()).toBe(true)
    useSessionStore().applyBootstrap(snapshot('b'))
    await flushPromises()
    expect(w.find('form').exists()).toBe(false)
    expect(w.find('[role="alert"]').exists()).toBe(false)
    await w.find('[data-test="edit-name"]').trigger('click')
    expect((w.find('#me-display-name').element as HTMLInputElement).value).toBe('昵称b')
    expect(listSpaceInvites).toHaveBeenCalledTimes(2)  // re-read for the new identity
  })

  it.each(['success', 'failure'])('a late nickname %s after an account switch changes nothing on the page', async (outcome) => {
    const request = deferred<MeResponse>()
    vi.mocked(authApi.updateMe).mockReturnValueOnce(request.promise)
    const w = mountMe()
    await flushPromises()
    await w.find('[data-test="edit-name"]').trigger('click')
    await w.find('#me-display-name').setValue('旧草稿')
    await w.find('form').trigger('submit')
    await flushPromises()
    useSessionStore().applyBootstrap(snapshot('b'))
    await flushPromises()
    if (outcome === 'success') request.resolve(me('a', '旧草稿'))
    else request.reject(new ApiError('VALIDATION_ERROR', '昵称不合法', 400))
    await flushPromises()
    expect(w.find('[role="alert"]').exists()).toBe(false)
    expect(w.find('form').exists()).toBe(false)
    expect(w.text()).toContain('昵称b')
    expect(w.text()).not.toContain('旧草稿')
  })

  it('an old space read failure is not shown to the new account', async () => {
    const current = deferred<SpaceResponse>()
    useSessionStore().applyBootstrap(snapshot('a', false))
    vi.mocked(getCurrentSpace).mockReturnValueOnce(current.promise)
    const w = mountMe()
    await flushPromises()
    useSessionStore().applyBootstrap(snapshot('b'))
    current.reject(new ApiError('INTERNAL_ERROR', 'down', 500))
    await flushPromises()
    expect(w.find('[role="alert"]').exists()).toBe(false)
    expect(w.text()).toContain('空间b')
  })

  it.each(['success', 'failure'])('an old logout %s cannot clear or navigate a newer session', async (outcome) => {
    const request = deferred<void>()
    vi.mocked(authApi.deleteSession).mockReturnValueOnce(request.promise)
    vi.spyOn(console, 'warn').mockImplementation(() => undefined)
    const w = mountMe()
    await flushPromises()
    await w.find('[data-test="logout"]').trigger('click')
    useSessionStore().applyBootstrap(snapshot('b'))  // a newer session became current meanwhile
    if (outcome === 'success') request.resolve()
    else request.reject(new Error('offline'))
    await flushPromises()
    const session = useSessionStore()
    expect(session.isAuthenticated).toBe(true)
    expect(session.user?.id).toBe('b')
    expect(replace).not.toHaveBeenCalled()
    expect(w.find('[data-test="logout"]').attributes('disabled')).toBeUndefined()
  })

  it('a current logout still clears the session and goes to login', async () => {
    vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
    const w = mountMe()
    await flushPromises()
    await w.find('[data-test="logout"]').trigger('click')
    await flushPromises()
    expect(useSessionStore().isAuthenticated).toBe(false)
    expect(replace).toHaveBeenCalledWith({ name: 'login' })
  })
})
