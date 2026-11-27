import { flushPromises, mount, RouterLinkStub } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/tasks', () => ({
  createTask: vi.fn(), listTasks: vi.fn(), getTask: vi.fn(),
  completeTask: vi.fn(), progressTask: vi.fn(),
  proposeTaskChange: vi.fn(), decideTaskChange: vi.fn(),
  listReminders: vi.fn().mockResolvedValue({ reminders: [] }),
  createReminder: vi.fn(), cancelReminder: vi.fn(),
  reactTask: vi.fn(),
}))
vi.mock('@/api/rewards', () => ({
  createRewardProduct: vi.fn(), listRewardProducts: vi.fn(), editRewardProduct: vi.fn(), archiveRewardProduct: vi.fn(),
  redeemReward: vi.fn(), listRedemptions: vi.fn(), fulfillRedemption: vi.fn(), transitionRedemption: vi.fn(),
}))
vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn(), createMessage: vi.fn(), getMessage: vi.fn(), replyMessage: vi.fn() }))
vi.mock('@/api/anniversaries', () => ({
  listAnniversaries: vi.fn(), getHomeAnniversary: vi.fn(),
  createAnniversary: vi.fn(), getAnniversary: vi.fn(), updateAnniversary: vi.fn(), pinAnniversary: vi.fn(),
}))
vi.mock('@/api/avatar', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/avatar')>()),
  getAvatarAppearance: vi.fn().mockResolvedValue({ avatar_preset: 'p1', equipped_items: {}, version: 1 }),
  listAvatarCatalog: vi.fn().mockResolvedValue({ items: [] }),
  listAvatarOwned: vi.fn().mockResolvedValue({ items: [] }),
}))
vi.mock('@/api/garden', () => ({
  getGardenState: vi.fn().mockResolvedValue({
    garden: { plant_code: 'sprout', stage: 'seedling', stage_label: '幼苗', progress: 0, target: 3, streak_days: 0, watered_today: false, ready_to_harvest: false, last_watered_date: null, total_harvests: 0 },
  }),
  waterGarden: vi.fn(), harvestGarden: vi.fn(),
}))
vi.mock('@/api/spaces', () => ({
  getSpace: vi.fn().mockResolvedValue({
    space: { id: 'sp-1', name: '暖屋', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-01T00:00:00Z', members: [] },
    available_invites: [],
  }),
}))
vi.mock('@/api/push', () => ({
  getPushStatus: vi.fn().mockResolvedValue({ supported: false, enabled: false, permission: 'default', subscriptions: [] }),
}))
vi.mock('@/api/notification-choices', () => ({ getNotificationChoices: vi.fn().mockResolvedValue({ choices: {} }) }))
vi.mock('vue-router', () => ({ useRouter: () => ({ replace: vi.fn(), push: vi.fn() }) }))

import * as anniversariesApi from '@/api/anniversaries'
import * as messagesApi from '@/api/messages'
import * as rewardsApi from '@/api/rewards'
import * as tasksApi from '@/api/tasks'
import * as walletApi from '@/api/wallet'
import { useSessionStore } from '@/stores/session'
import HomeView from '@/views/HomeView.vue'
import MeView from '@/views/MeView.vue'
import RewardsView from '@/views/RewardsView.vue'
import TasksView from '@/views/TasksView.vue'
import UsView from '@/views/UsView.vue'

function signIn() {
  useSessionStore().applyBootstrap({
    authenticated: true,
    csrf_token: 'csrf-1',
    user: { id: 'me', email: 'me@example.com', display_name: '小满', created_at: '2026-10-01T00:00:00Z' },
    space: {
      id: 'sp-1',
      name: '暖屋',
      timezone: 'Asia/Shanghai',
      member_count: 2,
      created_at: '2026-10-01T00:00:00Z',
      members: [
        { user_id: 'me', slot: 1, display_name: '小满', joined_at: '2026-10-01T00:00:00Z' },
        { user_id: 'partner', slot: 2, display_name: '阿橘', joined_at: '2026-10-01T00:00:00Z' },
      ],
    },
  })
}

let wrapper: ReturnType<typeof mount> | undefined
beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  signIn()
  vi.mocked(tasksApi.listTasks).mockResolvedValue({ tasks: [], next_cursor: null })
  vi.mocked(rewardsApi.listRewardProducts).mockResolvedValue({ products: [], next_cursor: null })
  vi.mocked(rewardsApi.listRedemptions).mockResolvedValue({ redemptions: [], next_cursor: null })
  vi.mocked(walletApi.getWallet).mockResolvedValue({ wallet: { flower: 12, sunshine: 8 } })
  vi.mocked(messagesApi.listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(anniversariesApi.listAnniversaries).mockResolvedValue({ as_of_date: '2026-10-20', anniversaries: [], next_cursor: null })
  vi.mocked(anniversariesApi.getHomeAnniversary).mockResolvedValue({ as_of_date: '2026-10-20', anniversary: null, source: 'none' })
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
})

const globalStubs = { stubs: { RouterLink: RouterLinkStub } }

describe('Level-1 Hub + Dedicated Sub-page views', () => {
  it('TasksView renders only 3 clean entry links on hub and isolates each sub-page', async () => {
    vi.mocked(tasksApi.listTasks).mockResolvedValueOnce({
      tasks: [{
        id: 't1', creator_id: 'partner', assignee_id: 'me', title: '买牛奶', description: '',
        reward_flowers: 2, reward_sunshine: 1, completion_mode: 'single', target_count: 1,
        progress_count: 0, schedule_type: 'one_off',
        deadline: null, status: 'open', created_at: '2026-10-20T00:00:00Z', completed_at: null,
      }],
      next_cursor: null,
    }).mockResolvedValueOnce({ tasks: [], next_cursor: null })

    wrapper = mount(TasksView, { props: { section: 'hub' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="tasks-hub"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="hub-tasks-open"]').text()).toContain('待做 1')
    expect(wrapper.find('#task-title').exists()).toBe(false)
    expect(wrapper.find('[data-test="open-view"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="history-view"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(TasksView, { props: { section: 'publish' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-tasks"]').exists()).toBe(true)
    expect(wrapper.find('#task-title').exists()).toBe(true)
    expect(wrapper.find('[data-test="open-view"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(TasksView, { props: { section: 'open' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-tasks"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="open-view"]').exists()).toBe(true)
    expect(wrapper.find('#task-title').exists()).toBe(false)
    expect(wrapper.find('[data-test="history-view"]').exists()).toBe(false)
  })

  it('RewardsView shows flower balance + 4 hub links and isolates shop vs create vs mine vs records', async () => {
    vi.mocked(rewardsApi.listRedemptions).mockResolvedValueOnce({
      redemptions: [{
        id: 'rd-1', product_id: 'p1', creator_id: 'me', redeemer_id: 'partner',
        title: '按摩', description: '', price_flowers: 5, redemption_mode: 'direct', product_version: 1,
        operation_id: null, status: 'pending_fulfillment', created_at: '2026-10-20T00:00:00Z',
        fulfilled_at: null, fulfilled_by: null, cancelled_at: null, cancel_reason: null, pending_cancellation: null,
      }],
      next_cursor: null,
    })

    wrapper = mount(RewardsView, { props: { section: 'hub' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="rewards-wallet"]').text()).toContain('小红花 12')
    expect(wrapper.find('[data-test="rewards-hub"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="hub-rewards-records"]').text()).toContain('待处理 1')
    expect(wrapper.find('[data-test="for-me"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="create-form"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(RewardsView, { props: { section: 'new' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-rewards"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="create-form"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="for-me"]').exists()).toBe(false)
  })

  it('HomeView renders concise brief card on hub and isolates today vs pending sub-pages', async () => {
    wrapper = mount(HomeView, { props: { section: 'hub' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="home-brief"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-wallet"]').text()).toContain('小红花 12')
    expect(wrapper.findAll('[data-test="avatar-home"] a.btn.btn--soft')).toHaveLength(2)
    expect(wrapper.find('[data-test="home-hub-actions"] [data-test="brief-today"].hub-card').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-hub-actions"] [data-test="brief-anniversary"].hub-card').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-hub-actions"] [data-test="brief-message"].hub-card').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-hub-actions"] [data-test="brief-garden"].hub-card').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-today"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="home-pending"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(HomeView, { props: { section: 'today' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-home"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-today"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="home-pending"]').exists()).toBe(false)
  })

  it('UsView renders 8 clean hub cards with pending reply badge and isolates messages sub-page', async () => {
    vi.mocked(messagesApi.listMessages).mockResolvedValue({
      messages: [{
        id: 'm1', author_id: 'partner', recipient_id: 'me', body: '今晚散步吗',
        created_at: '2026-10-20T12:00:00Z', reply: null,
      }],
      next_cursor: null,
    })

    wrapper = mount(UsView, { props: { section: 'hub' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="us-hub"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="hub-us-messages"]').text()).toContain('待回复 1')
    expect(wrapper.find('[data-test="compose"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(UsView, { props: { section: 'messages' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-us"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="compose"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="message-history"]').exists()).toBe(true)
  })

  it('MeView renders profile/space card + 3 links on hub and isolates space/export sub-pages', async () => {
    wrapper = mount(MeView, { props: { section: 'hub' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="me-hub"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="logout"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="user-export"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="invites"]').exists()).toBe(false)
    wrapper.unmount()

    wrapper = mount(MeView, { props: { section: 'export' }, global: globalStubs })
    await flushPromises()
    expect(wrapper.find('[data-test="back-to-me"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="user-export"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="logout"]').exists()).toBe(false)
  })
})
