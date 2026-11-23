import { mount, flushPromises, RouterLinkStub, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/avatar', async original => ({ ...(await original<typeof import('@/api/avatar')>()), getAvatarAppearance: vi.fn() }))
vi.mock('@/api/tasks', () => ({ listTasks: vi.fn() }))
vi.mock('@/api/rewards', () => ({ listRedemptions: vi.fn() }))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn() }))
vi.mock('@/api/shared-garden', () => ({ getSharedGarden: vi.fn() }))
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
import { getSharedGarden, type SharedGarden } from '@/api/shared-garden'
import { useSessionStore } from '@/stores/session'
import HomeView from '@/views/HomeView.vue'
import { getAvatarAppearance } from '@/api/avatar'
import { appearance as avatarAppearance } from './avatar-fixtures'

let wrapper: VueWrapper | undefined
const garden: SharedGarden = {
  watering_date: '2026-11-20', total_waterings: 3, watered_today: false, partner_watered_today: true,
}
beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  vi.mocked(getAvatarAppearance).mockResolvedValue({ appearance: avatarAppearance() })
  vi.mocked(listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(getSharedGarden).mockResolvedValue(garden)
  vi.mocked(getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-29', source: 'none', anniversary: null })
  vi.mocked(listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-29', anniversaries: [], next_cursor: null })
  useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'token',
    user: { id: 'self', email: 'self@example.com', display_name: 'self', created_at: '2026-10-07T00:00:00Z' },
    space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2,
      created_at: '2026-10-07T00:00:00Z', members: [] } })
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('home garden card', () => {
  it('shows shared growth, server-date watering status and the garden link', async () => {
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: RouterLinkStub } } })
    await flushPromises()
    const card = wrapper.get('.home-garden')
    expect(card.text()).toContain('发芽')
    expect(card.text()).toContain('累计浇水 3 次')
    expect(card.text()).toContain('我待浇水')
    expect(card.text()).toContain('对方已浇水')
    expect(card.get('time').attributes('datetime')).toBe('2026-11-20')
    const destinations = wrapper.get('.home-destinations')
    const links = destinations.findAllComponents(RouterLinkStub)
    expect(links).toHaveLength(2)
    expect(links[0]!.props('to')).toEqual({ name: 'shared-room' })
    expect(links[1]!.props('to')).toEqual({ name: 'shared-garden' })
    expect(links[1]!.text()).toBe('去共享花园')
    expect(card.find('a').exists()).toBe(false)
  })

  it('retries a failed read independently, then rereads on an explicit home refresh', async () => {
    vi.mocked(getSharedGarden).mockRejectedValueOnce(new ApiError('NETWORK_ERROR', 'offline', 0))
    vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 2, sunshine: 7 } })
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: true } } })
    await flushPromises()
    const card = wrapper.get('.home-garden')
    expect(card.text()).toContain('暂时无法读取花园进度')
    expect(wrapper.get('[data-test="home-wallet"]').findAll('dd').map(item => item.text())).toEqual(['2', '7'])
    await card.get('button').trigger('click')
    await flushPromises()
    expect(card.text()).toContain('发芽')
    vi.mocked(getSharedGarden).mockResolvedValue({
      ...garden, watering_date: '2026-11-21', total_waterings: 8, partner_watered_today: false,
    })
    await wrapper.get('[data-test="home-refresh"]').trigger('click')
    await flushPromises()
    expect(card.text()).toContain('长叶')
    expect(card.text()).toContain('对方待浇水')
    expect(card.get('time').attributes('datetime')).toBe('2026-11-21')
  })

  it('cancels an outstanding garden read when the current space changes', async () => {
    let oldRead!: (state: SharedGarden) => void
    vi.mocked(getSharedGarden).mockReturnValueOnce(new Promise(resolve => { oldRead = resolve }))
    wrapper = mount(HomeView, { global: { stubs: { RouterLink: true } } })
    const signal = vi.mocked(getSharedGarden).mock.lastCall?.[0]
    const session = useSessionStore()
    vi.mocked(getSharedGarden).mockResolvedValue({ ...garden, total_waterings: 0, partner_watered_today: false })
    session.space = { ...session.space!, id: 'another-space', member_count: 1 }
    await flushPromises()
    oldRead({ ...garden, total_waterings: 14 })
    await flushPromises()
    expect(signal?.aborted).toBe(true)
    expect(wrapper.get('.home-garden').text()).toContain('种子')
    expect(wrapper.get('.home-garden').text()).toContain('对方尚未加入')
    expect(wrapper.get('.home-garden').text()).not.toContain('开花')
  })
})

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
