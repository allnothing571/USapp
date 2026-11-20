/**
 * M10 Me page: the nickname editing, space/member/invitation display and logout that used to live
 * on the home page keep working after the move.
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

import * as authApi from '@/api/auth'
import { listSpaceInvites } from '@/api/space'
import { getWallet } from '@/api/wallet'
import { useSessionStore } from '@/stores/session'
import MeView from '@/views/MeView.vue'

const USER = { id: 'self', email: 'self@example.com', display_name: '旧昵称', created_at: '2026-10-07T00:00:00Z' }

let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(listSpaceInvites).mockResolvedValue({
    invites: [{ id: 'abcdef12-0000-0000-0000-000000000000', expires_at: '2099-01-01T00:00:00Z', used_count: 0,
      max_uses: 1, revoked_at: null, created_at: '2026-10-26T00:00:00Z' }],
  } as never)
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'token', user: USER,
    space: { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 1, created_at: '2026-10-07T00:00:00Z',
      members: [{ user_id: 'self', display_name: '旧昵称', slot: 1 }] } as never })
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('me page', () => {
  it('shows the account, space slots and available invitations without reading the wallet', async () => {
    wrapper = mount(MeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.text()).toContain('旧昵称')
    expect(wrapper.text()).toContain('self@example.com')
    expect(wrapper.text()).toContain('我们')
    expect(wrapper.text()).toContain('空位')
    expect(wrapper.find('[data-test="invites"]').text()).toContain('邀请 abcdef12')
    expect(listSpaceInvites).toHaveBeenCalledTimes(1)
    expect(getWallet).not.toHaveBeenCalled()
  })

  it('edits the nickname through the session store', async () => {
    vi.mocked(authApi.updateMe).mockResolvedValueOnce({ user: { ...USER, display_name: '新昵称' }, space: null })
    wrapper = mount(MeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="edit-name"]').trigger('click')
    await wrapper.find('#me-display-name').setValue('  新昵称 ')
    await wrapper.find('form').trigger('submit')
    await flushPromises()
    expect(authApi.updateMe).toHaveBeenCalledWith({ display_name: '新昵称' })
    expect(useSessionStore().user?.display_name).toBe('新昵称')
    expect(wrapper.find('form').exists()).toBe(false)
  })

  it('rejects an invalid nickname locally', async () => {
    wrapper = mount(MeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="edit-name"]').trigger('click')
    await wrapper.find('#me-display-name').setValue('   ')
    await wrapper.find('form').trigger('submit')
    expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    expect(authApi.updateMe).not.toHaveBeenCalled()
  })

  it('logs out and returns to the login page', async () => {
    vi.mocked(authApi.deleteSession).mockResolvedValueOnce(undefined)
    wrapper = mount(MeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    await wrapper.find('[data-test="logout"]').trigger('click')
    await flushPromises()
    expect(authApi.deleteSession).toHaveBeenCalledTimes(1)
    expect(useSessionStore().isAuthenticated).toBe(false)
    expect(replace).toHaveBeenCalledWith({ name: 'login' })
  })
})
