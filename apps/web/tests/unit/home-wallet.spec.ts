import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/avatar', async original => ({ ...(await original<typeof import('@/api/avatar')>()), getAvatarAppearance: vi.fn() }))
vi.mock('@/api/tasks', () => ({ listTasks: vi.fn() }))
vi.mock('@/api/rewards', () => ({ listRedemptions: vi.fn() }))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn() }))
vi.mock('@/api/anniversaries', () => ({
  createAnniversary: vi.fn(), listAnniversaries: vi.fn(), getAnniversary: vi.fn(), editAnniversary: vi.fn(),
  proposeAnniversaryEdit: vi.fn(), decideAnniversaryProposal: vi.fn(), getHomeAnniversary: vi.fn(),
  setHomeAnniversaryPin: vi.fn(),
}))

import { getHomeAnniversary, listAnniversaries } from '@/api/anniversaries'
import { getWallet } from '@/api/wallet'
import { listTasks } from '@/api/tasks'
import { listRedemptions } from '@/api/rewards'
import { listMessages } from '@/api/messages'
import { ApiError } from '@/api/client'
import { useSessionStore } from '@/stores/session'
import HomeView from '@/views/HomeView.vue'
import { getAvatarAppearance } from '@/api/avatar'
import { appearance as avatarAppearance } from './avatar-fixtures'

// M10: the own-wallet card now sits in the top-left home quadrant; its protections are unchanged.
let wrapper: VueWrapper | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: avatarAppearance() })
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
  vi.mocked(listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-29', anniversaries: [], next_cursor: null })
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'token',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-07T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2,
      created_at: '2026-10-07T00:00:00Z', members: [] } })
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('home wallet card', () => {
  it('shows loading without balances, then the two own balances', async () => {
    let resolve!: (value: { wallet: { flower: number; sunshine: number } }) => void
    vi.mocked(getWallet).mockReturnValue(new Promise((yes) => { resolve = yes }))
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: true } } })
    const card = wrapper.find('[data-test="home-wallet"]')
    expect(card.text()).toContain('加载中')
    expect(card.text()).not.toContain('小红花')
    resolve({ wallet: { flower: 0, sunshine: 7 } })
    await flushPromises()
    expect(card.text()).toContain('小红花')
    expect(card.text()).toContain('阳光值')
    expect(card.findAll('dd').map((entry) => entry.text())).toEqual(['0', '7'])
  })

  it('shows an error without numbers and a working retry button', async () => {
    vi.mocked(getWallet).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const card = wrapper.find('[data-test="home-wallet"]')
    expect(card.find('[role="alert"]').exists()).toBe(true)
    expect(card.findAll('dd')).toHaveLength(0)
    vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 1, sunshine: 2 } })
    await card.find('button').trigger('click')
    await flushPromises()
    expect(card.findAll('dd').map((entry) => entry.text())).toEqual(['1', '2'])
    expect(card.find('[role="alert"]').exists()).toBe(false)
  })

  it('refreshing a previously ready card while offline removes the old numbers', async () => {
    vi.mocked(getWallet).mockResolvedValueOnce({ wallet: { flower: 3, sunshine: 4 } })
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const card = wrapper.find('[data-test="home-wallet"]')
    expect(card.findAll('dd').map((entry) => entry.text())).toEqual(['3', '4'])
    vi.mocked(getWallet).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    await wrapper.find('[data-test="home-refresh"]').trigger('click')
    await flushPromises()
    expect(card.findAll('dd')).toHaveLength(0)
    expect(card.find('[role="alert"]').exists()).toBe(true)
    expect(card.find('button').text()).toBe('重试')
    expect(getWallet).toHaveBeenCalledTimes(2)
  })
})
