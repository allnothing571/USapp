import { flushPromises, mount } from '@vue/test-utils'
import { createPinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { getSharedGardenMonth, type SharedGardenMonth } from '@/api/shared-garden'
import { useSessionStore } from '@/stores/session'
import GardenHistoryView from '@/views/GardenHistoryView.vue'

vi.mock('@/api/shared-garden', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/shared-garden')>(),
  getSharedGardenMonth: vi.fn(),
}))

function month(month: string, current: string, days: number): SharedGardenMonth {
  return {
    month, current_month: current,
    days: Array.from({ length: days }, (_, index) => ({
      watering_date: `${month}-${String(index + 1).padStart(2, '0')}`,
      watered: index === 0, partner_watered: index < 2,
    })),
  }
}

async function setup() {
  const pinia = createPinia()
  const session = useSessionStore(pinia)
  session.user = { id: 'a', email: 'garden-a@example.test', display_name: '甲', created_at: '' }
  session.space = { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 2, created_at: '' }
  const wrapper = mount(GardenHistoryView, { global: { plugins: [pinia], stubs: { RouterLink: true } } })
  await flushPromises()
  const button = (name: string) => wrapper.findAll('button').find(item => item.text() === name)!
  return { wrapper, session, button }
}

beforeEach(() => {
  vi.resetAllMocks()
  vi.mocked(getSharedGardenMonth).mockResolvedValue(month('2026-11', '2026-11', 20))
})

describe('garden month calendar', () => {
  it('uses the server month and day count, positions the first weekday and counts joint days', async () => {
    const { wrapper, button } = await setup()
    const days = wrapper.findAll('.month-days li')
    expect(days).toHaveLength(20)
    expect(days[0]?.attributes('style')).toContain('grid-column-start: 7')
    expect(days[0]?.get('button').attributes('aria-label')).toContain('我已浇水，对方已浇水')
    expect(days[19]?.get('button').attributes('aria-current')).toBe('date')
    expect(wrapper.get('.month-summary').text()).toContain('两人都浇水 1 天')
    expect(button('下个月').element.disabled).toBe(true)
    wrapper.unmount()
  })

  it('jumps directly to a valid past month and rejects future or empty values locally', async () => {
    const { wrapper } = await setup()
    const input = wrapper.get('#month-jump-input')
    expect(input.attributes('max')).toBe('2026-11')
    await input.setValue('2026-12')
    await wrapper.get('form').trigger('submit')
    expect(wrapper.get('#month-jump-error').text()).toContain('当前月份或更早')
    await input.setValue('')
    await wrapper.get('form').trigger('submit')
    expect(wrapper.get('#month-jump-error').text()).toContain('YYYY-MM')
    expect(getSharedGardenMonth).toHaveBeenCalledTimes(1)
    vi.mocked(getSharedGardenMonth).mockResolvedValueOnce(month('2024-02', '2026-11', 29))
    await input.setValue('2024-02')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(vi.mocked(getSharedGardenMonth).mock.lastCall?.[0]).toBe('2024-02')
    expect(wrapper.findAll('.month-days li')).toHaveLength(29)
    expect(wrapper.find('#month-jump-error').exists()).toBe(false)
    expect(wrapper.find('[aria-current="date"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('reveals daily details without reading again and clears the selection when changing months', async () => {
    const { wrapper, button } = await setup()
    const days = wrapper.findAll('.month-day')
    await days[1]!.trigger('click')
    expect(days[1]?.attributes('aria-pressed')).toBe('true')
    expect(wrapper.get('.month-day-detail').text()).toContain('2026-11-02')
    expect(wrapper.get('.month-day-detail').text()).toContain('我未浇水 · 对方已浇水')
    expect(getSharedGardenMonth).toHaveBeenCalledTimes(1)
    vi.mocked(getSharedGardenMonth).mockResolvedValueOnce(month('2026-10', '2026-11', 31))
    await button('上个月').trigger('click')
    await flushPromises()
    expect(wrapper.find('.month-day-detail').exists()).toBe(false)
    wrapper.unmount()
  })

  it('navigates across the year boundary and returns to the current server month', async () => {
    vi.mocked(getSharedGardenMonth).mockResolvedValue(month('2027-01', '2027-01', 2))
    const { wrapper, button } = await setup()
    vi.mocked(getSharedGardenMonth).mockResolvedValueOnce(month('2026-12', '2027-01', 31))
    await button('上个月').trigger('click')
    await flushPromises()
    expect(vi.mocked(getSharedGardenMonth).mock.lastCall?.[0]).toBe('2026-12')
    expect(wrapper.findAll('.month-days time')).toHaveLength(31)
    expect(button('下个月').element.disabled).toBe(false)
    await button('回到本月').trigger('click')
    await flushPromises()
    expect(vi.mocked(getSharedGardenMonth).mock.lastCall?.[0]).toBeUndefined()
    expect(wrapper.findAll('.month-days time')).toHaveLength(2)
    wrapper.unmount()
  })

  it('retries an older month without showing the previous results under the wrong heading', async () => {
    const { wrapper, button } = await setup()
    vi.mocked(getSharedGardenMonth).mockRejectedValueOnce(new Error('offline'))
    await button('上个月').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('2026-10')
    expect(wrapper.find('.month-days').exists()).toBe(false)
    expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    vi.mocked(getSharedGardenMonth).mockResolvedValueOnce(month('2026-10', '2026-11', 31))
    await button('重试').trigger('click')
    await flushPromises()
    expect(vi.mocked(getSharedGardenMonth).mock.lastCall?.[0]).toBe('2026-10')
    expect(wrapper.findAll('.month-days time')).toHaveLength(31)
    wrapper.unmount()
  })
})
