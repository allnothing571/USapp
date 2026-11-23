import { flushPromises, mount } from '@vue/test-utils'
import { createPinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ApiError } from '@/api/client'
import { getSharedGarden, waterSharedGarden, type SharedGarden } from '@/api/shared-garden'
import { useSessionStore } from '@/stores/session'
import SharedGardenView from '@/views/SharedGardenView.vue'

vi.mock('@/api/shared-garden', () => ({
  getSharedGarden: vi.fn(),
  waterSharedGarden: vi.fn(),
}))

const date = '2026-11-20'
const baseline: SharedGarden = {
  watering_date: date, total_waterings: 2, watered_today: false, partner_watered_today: true,
}
const stamp = date + 'T00:00:00Z'

async function setup() {
  const pinia = createPinia()
  const session = useSessionStore(pinia)
  session.user = { id: 'member-a', email: 'garden-a@example.test', display_name: '甲', created_at: stamp }
  session.space = { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 2, created_at: stamp }
  const wrapper = mount(SharedGardenView, { global: { plugins: [pinia], stubs: { RouterLink: true } } })
  await flushPromises()
  return { wrapper, session }
}

beforeEach(() => {
  vi.resetAllMocks()
  vi.mocked(getSharedGarden).mockResolvedValue(baseline)
  vi.mocked(waterSharedGarden).mockResolvedValue()
})

describe('shared garden', () => {
  it('waters the displayed date once and renders the confirmed shared progress', async () => {
    const { wrapper } = await setup()
    expect(wrapper.get('#garden-stage').text()).toBe('种子')
    expect(wrapper.find('.garden-recent').exists()).toBe(false)
    expect(wrapper.get('.garden-members').text()).toContain('对方已浇水')
    vi.mocked(getSharedGarden).mockResolvedValue({ ...baseline, total_waterings: 3, watered_today: true })
    await wrapper.get('.garden-today button').trigger('click')
    await flushPromises()
    expect(waterSharedGarden).toHaveBeenCalledExactlyOnceWith(date)
    expect(wrapper.get('#garden-stage').text()).toBe('发芽')
    expect(wrapper.get('[role="status"]').text()).toBe('今天一起浇过水了')
    expect(wrapper.find('.garden-drops').exists()).toBe(true)
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeDefined()
    wrapper.unmount()
  })

  it('shows watering feedback without changing the stage below the next threshold', async () => {
    vi.mocked(getSharedGarden).mockResolvedValue({ ...baseline, total_waterings: 4, partner_watered_today: false })
    const { wrapper } = await setup()
    expect(wrapper.get('.garden-plant').attributes('data-stage')).toBe('1')
    vi.mocked(getSharedGarden).mockResolvedValue({ ...baseline, total_waterings: 5, partner_watered_today: false, watered_today: true })
    await wrapper.get('.garden-today button').trigger('click')
    await flushPromises()
    expect(wrapper.get('.garden-plant').attributes('data-stage')).toBe('1')
    expect(wrapper.find('.garden-drops').exists()).toBe(true)
    expect(wrapper.get('[role="status"]').text()).toBe('浇水已记录。')
    wrapper.unmount()
  })

  it('shows the seven server dates in order with separate watering statuses', async () => {
    const recent = Array.from({ length: 7 }, (_, index) => ({
      watering_date: '2026-11-' + (20 - index),
      watered: index === 3,
      partner_watered: index === 0,
    }))
    vi.mocked(getSharedGarden).mockResolvedValueOnce({ ...baseline, recent_waterings: recent })
    const { wrapper } = await setup()
    const rows = wrapper.findAll('.garden-recent tbody tr')
    expect(rows.map(row => row.get('time').attributes('datetime'))).toEqual(recent.map(day => day.watering_date))
    expect(rows[0]?.findAll('td').map(cell => cell.text())).toEqual(['未浇水', '已浇水'])
    expect(rows[3]?.findAll('td').map(cell => cell.text())).toEqual(['已浇水', '未浇水'])
    expect(rows[6]?.findAll('td').map(cell => cell.text())).toEqual(['未浇水', '未浇水'])
    wrapper.unmount()
  })

  it('loads the new day without repeating a watering request across midnight', async () => {
    const { wrapper } = await setup()
    vi.mocked(waterSharedGarden).mockRejectedValueOnce(new ApiError('SHARED_GARDEN_DATE_CHANGED', 'expired', 409))
    vi.mocked(getSharedGarden).mockResolvedValue({ ...baseline, watering_date: '2026-11-21', partner_watered_today: false })
    await wrapper.get('.garden-today button').trigger('click')
    await flushPromises()
    expect(waterSharedGarden).toHaveBeenCalledExactlyOnceWith(date)
    expect(wrapper.get('time').attributes('datetime')).toBe('2026-11-21')
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeUndefined()
    expect(wrapper.get('[role="status"]').text()).toContain('日期已更新')
    expect(wrapper.find('.garden-drops').exists()).toBe(false)
    wrapper.unmount()
  })

  it('checks the server after an unknown result and never adds progress locally', async () => {
    const { wrapper } = await setup()
    vi.mocked(waterSharedGarden).mockRejectedValueOnce(new ApiError('RESULT_UNKNOWN', 'check', 503))
    await wrapper.get('.garden-today button').trigger('click')
    await flushPromises()
    expect(wrapper.find('.garden-today').exists()).toBe(false)
    expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    vi.mocked(getSharedGarden).mockResolvedValue({ ...baseline, total_waterings: 3, watered_today: true })
    await wrapper.get('.garden-heading button').trigger('click')
    await flushPromises()
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeDefined()
    expect(waterSharedGarden).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('discards a delayed read after the current session is cleared', async () => {
    const { wrapper, session } = await setup()
    let resolve!: (garden: SharedGarden) => void
    vi.mocked(getSharedGarden).mockReturnValueOnce(new Promise(done => { resolve = done }))
    await wrapper.get('.garden-heading button').trigger('click')
    session.user = null
    resolve({ ...baseline, total_waterings: 14, watered_today: true })
    await flushPromises()
    expect(wrapper.find('.garden-today').exists()).toBe(false)
    expect(wrapper.find('.garden-scene').exists()).toBe(false)
    wrapper.unmount()
  })
})

describe('shared garden automatic sync', () => {
  let view: Awaited<ReturnType<typeof setup>>['wrapper'] | undefined
  let hidden: boolean

  beforeEach(() => {
    hidden = false
    vi.spyOn(document, 'hidden', 'get').mockImplementation(() => hidden)
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })
  })

  afterEach(() => {
    view?.unmount()
    view = undefined
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  it('updates progress and daily watering controls using the server date without writing', async () => {
    vi.mocked(getSharedGarden).mockResolvedValueOnce({
      ...baseline, watered_today: true, partner_watered_today: false,
    })
    const { wrapper } = await setup()
    view = wrapper
    vi.mocked(getSharedGarden).mockResolvedValueOnce({
      ...baseline, total_waterings: 3, watered_today: true,
    })
    await vi.advanceTimersByTimeAsync(15_000)
    expect(wrapper.get('#garden-stage').text()).toBe('发芽')
    expect(wrapper.get('.garden-members').text()).toContain('对方已浇水')
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeDefined()
    vi.mocked(getSharedGarden).mockResolvedValueOnce({
      ...baseline, watering_date: '2026-11-21', total_waterings: 3, partner_watered_today: false,
    })
    await vi.advanceTimersByTimeAsync(15_000)
    expect(wrapper.get('time').attributes('datetime')).toBe('2026-11-21')
    expect(wrapper.get('#garden-stage').text()).toBe('发芽')
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeUndefined()
    expect(waterSharedGarden).not.toHaveBeenCalled()
  })

  it('aborts a hidden-page read and refreshes once on returning', async () => {
    const { wrapper } = await setup()
    view = wrapper
    let finish!: (snapshot: SharedGarden) => void
    vi.mocked(getSharedGarden).mockReturnValueOnce(new Promise(resolve => { finish = resolve }))
    await vi.advanceTimersByTimeAsync(15_000)
    const signal = vi.mocked(getSharedGarden).mock.lastCall?.[0]
    hidden = true
    document.dispatchEvent(new Event('visibilitychange'))
    expect(signal?.aborted).toBe(true)
    await vi.advanceTimersByTimeAsync(30_000)
    expect(getSharedGarden).toHaveBeenCalledTimes(2)
    vi.mocked(getSharedGarden).mockResolvedValueOnce({ ...baseline, total_waterings: 3 })
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    finish(baseline)
    await flushPromises()
    expect(getSharedGarden).toHaveBeenCalledTimes(3)
    expect(wrapper.get('#garden-stage').text()).toBe('发芽')
  })

  it('prioritizes watering over a pending read and discards the late snapshot', async () => {
    const { wrapper } = await setup()
    view = wrapper
    let finish!: (snapshot: SharedGarden) => void
    vi.mocked(getSharedGarden).mockReturnValueOnce(new Promise(resolve => { finish = resolve }))
    await vi.advanceTimersByTimeAsync(15_000)
    const signal = vi.mocked(getSharedGarden).mock.lastCall?.[0]
    vi.mocked(getSharedGarden).mockResolvedValueOnce({ ...baseline, total_waterings: 3, watered_today: true })
    await wrapper.get('.garden-today button').trigger('click')
    await flushPromises()
    expect(signal?.aborted).toBe(true)
    expect(waterSharedGarden).toHaveBeenCalledExactlyOnceWith(date)
    finish(baseline)
    await flushPromises()
    expect(wrapper.get('.garden-today button').attributes('disabled')).toBeDefined()
    expect(wrapper.get('#garden-stage').text()).toBe('发芽')
  })

  it('preserves progress on a timed-out poll and clears it if the next read loses access', async () => {
    const { wrapper } = await setup()
    view = wrapper
    vi.mocked(getSharedGarden).mockReturnValueOnce(new Promise(() => {}))
    await vi.advanceTimersByTimeAsync(15_000)
    const signal = vi.mocked(getSharedGarden).mock.lastCall?.[0]
    await vi.advanceTimersByTimeAsync(8_000)
    expect(signal?.aborted).toBe(true)
    expect(wrapper.get('[role="status"]').text()).toContain('显示上次读取的进度')
    expect(wrapper.get('#garden-stage').text()).toBe('种子')
    vi.mocked(getSharedGarden).mockRejectedValueOnce(new ApiError('SPACE_NOT_FOUND', 'no space', 404))
    await vi.advanceTimersByTimeAsync(7_000)
    expect(wrapper.find('#garden-stage').exists()).toBe(false)
    expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    await vi.advanceTimersByTimeAsync(15_000)
    expect(getSharedGarden).toHaveBeenCalledTimes(3)
  })
})
