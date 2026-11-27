import { flushPromises, mount, RouterLinkStub } from '@vue/test-utils'
import { createPinia, type Pinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ApiError } from '@/api/client'
import { getWallet } from '@/api/wallet'
import { getAvatarAppearance } from '@/api/avatar'
import { listMessages } from '@/api/messages'
import {
  buyRoomFurniture,
  changeRoomPosition,
  connectSharedRoom,
  getRoomPositions,
  getSharedRoom,
  isSharedRoom,
  saveSharedRoom,
  setRoomAvatarSharing,
  type SharedRoom,
} from '@/api/shared-room'
import { DEFAULT_ITEM_SPOTS, mergeRoomLayout, ROOM_LAYOUT_PRESETS } from '@/shared-room-layout'
import { useSessionStore } from '@/stores/session'
import SharedRoomView from '@/views/SharedRoomView.vue'

vi.mock('@/api/wallet', () => ({ getWallet: vi.fn() }))
vi.mock('@/api/messages', () => ({ listMessages: vi.fn() }))

vi.mock('@/api/avatar', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/avatar')>(),
  getAvatarAppearance: vi.fn(),
}))
vi.mock('@/api/shared-room', async importOriginal => ({
  ...await importOriginal<typeof import('@/api/shared-room')>(),
  connectSharedRoom: vi.fn(),
  buyRoomFurniture: vi.fn(),
  changeRoomPosition: vi.fn(),
  getRoomPositions: vi.fn(),
  getSharedRoom: vi.fn(),
  saveSharedRoom: vi.fn(),
  setRoomAvatarSharing: vi.fn(),
}))

const empty: SharedRoom = { version: 0, layout: [], share_avatar: false, partner_appearance: null }
const stamp = '2026-11-20T00:00:00Z'
const user = { id: 'member-a', email: 'room-a@example.test', display_name: '甲', created_at: stamp }

function mountRoom(pinia: Pinia) {
  return mount(SharedRoomView, { global: { plugins: [pinia], stubs: { RouterLink: true, AvatarFigure: true } } })
}

async function setup() {
  const pinia = createPinia()
  const session = useSessionStore(pinia)
  session.user = user
  session.space = { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 2, created_at: stamp }
  const wrapper = mountRoom(pinia)
  await flushPromises()
  return { wrapper, session, pinia }
}

function button(wrapper: Awaited<ReturnType<typeof setup>>['wrapper'], label: string) {
  return wrapper.findAll('button').find(button => button.text() === label)!
}

beforeEach(() => {
  vi.resetAllMocks()
  window.localStorage.clear()
  vi.mocked(getAvatarAppearance).mockResolvedValue(null)
  vi.mocked(getSharedRoom).mockResolvedValue(empty)
  vi.mocked(saveSharedRoom).mockResolvedValue(1)
  vi.mocked(getWallet).mockResolvedValue({ wallet: { flower: 0, sunshine: 100 } })
  vi.mocked(listMessages).mockResolvedValue({ messages: [], next_cursor: null })
  vi.mocked(changeRoomPosition).mockResolvedValue(undefined)
  vi.mocked(getRoomPositions).mockResolvedValue({ side: 'left', paired: true, pending: null })
})

describe('shared room live and purchases', () => {
  it('merges non-overlapping concurrent placements beyond 4 items and flags overlapping non-rug items', () => {
    const base = ROOM_LAYOUT_PRESETS[0].layout
    const mine = [...base, { item: 'bookshelf', x: 760, y: 240 }] as SharedRoom['layout']
    const theirs = [...base, { item: 'armchair', x: 700, y: 560 }] as SharedRoom['layout']
    expect(mergeRoomLayout(base, mine, theirs).layout).toEqual([
      { item: 'table', x: 500, y: 600 },
      { item: 'lamp', x: 210, y: 280 },
      { item: 'rug', x: 500, y: 640 },
      { item: 'bookshelf', x: 760, y: 240 },
      { item: 'armchair', x: 700, y: 560 },
    ])

    const closeDraft = [...base, { item: 'bookshelf', x: 700, y: 560 }] as SharedRoom['layout']
    expect(mergeRoomLayout(base, closeDraft, theirs).conflicts).toEqual([
      { kind: 'overlap', first: { item: 'bookshelf', x: 700, y: 560 }, second: { item: 'armchair', x: 700, y: 560 } },
    ])
  })

  it('shares presence only on opt-in, syncs updates, and saves with the current version', async () => {
    const initial = { onmessage: null as ((event: { data: string }) => void) | null, close: vi.fn() }
    const optedIn = { onmessage: null as ((event: { data: string }) => void) | null, close: vi.fn() }
    vi.mocked(connectSharedRoom).mockReturnValueOnce(initial as unknown as WebSocket)
      .mockReturnValueOnce(optedIn as unknown as WebSocket)
    const visibility = vi.spyOn(document, 'hidden', 'get').mockReturnValue(false)
    const { wrapper } = await setup()
    try {
      expect(connectSharedRoom).toHaveBeenCalledWith(false)
      initial.onmessage?.({ data: 'ready' })
      await flushPromises()
      await wrapper.get('section[aria-label="房间协作"] input').setValue(true)
      expect(initial.close).toHaveBeenCalledOnce()
      expect(connectSharedRoom).toHaveBeenLastCalledWith(true)
      const remote: SharedRoom = { ...empty, version: 1, layout: [{ item: 'rug', x: 500, y: 640 }] }
      vi.mocked(getSharedRoom).mockResolvedValue(remote)
      optedIn.onmessage?.({ data: 'ready' })
      optedIn.onmessage?.({ data: 'peer-online' })
      await flushPromises()
      expect(wrapper.text()).toContain('对方在房间')
      expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
      await wrapper.findAll('section[aria-label="房间协作"] input')[1].setValue(true)
      vi.mocked(saveSharedRoom).mockResolvedValue(2)
      await button(wrapper, '小桌').trigger('click')
      await flushPromises()
      expect(saveSharedRoom).toHaveBeenCalledWith(1, [...remote.layout, { item: 'table', ...DEFAULT_ITEM_SPOTS.table }])
      visibility.mockReturnValue(true)
      document.dispatchEvent(new Event('visibilitychange'))
      expect(optedIn.close).toHaveBeenCalledOnce()
    } finally {
      wrapper.unmount()
      visibility.mockRestore()
    }
  })

  it('confirms the price and resolves an unconfirmed purchase with the original key', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, furniture: [{ item: 'bookshelf', price: 20, owned: false }] })
    vi.mocked(buyRoomFurniture).mockRejectedValueOnce(new Error('network')).mockResolvedValueOnce(undefined)
    const { wrapper } = await setup()
    try {
      expect(wrapper.get('#room-layout-panel').isVisible()).toBe(false)
      expect(wrapper.get('#room-shop-panel').isVisible()).toBe(false)
      await button(wrapper, '商店').trigger('click')
      await flushPromises()
      expect(wrapper.get('#room-shop-panel').text()).toContain('我的阳光 100')
      await button(wrapper, '书架 · 20 阳光').trigger('click')
      expect(wrapper.find('.room-preview[data-item="bookshelf"]').exists()).toBe(true)
      expect(saveSharedRoom).not.toHaveBeenCalled()
      expect(buyRoomFurniture).not.toHaveBeenCalled()
      expect(wrapper.text()).toContain('用我的 20 阳光购买书架')
      await button(wrapper, '确认购买').trigger('click')
      await flushPromises()
      const key = vi.mocked(buyRoomFurniture).mock.calls[0][1]
      await button(wrapper, '确认购买结果').trigger('click')
      await flushPromises()
      expect(buyRoomFurniture).toHaveBeenLastCalledWith('bookshelf', key)
      expect(button(wrapper, '书架')).toBeDefined()
      expect(wrapper.text()).toContain('空间已拥有')
    } finally {
      wrapper.unmount()
    }
  })
})

describe('shared room automatic sync', () => {
  const shared: SharedRoom = {
    ...empty,
    partner_appearance: {
      source: 'starter_defaults', complete: false, starter_version: null, loadout_version: 0,
      slots: { hair: null, clothes: null, expression: null, hat: null, handheld: null, accessory: null },
    },
  }
  const latest: SharedRoom = { ...empty, version: 1, layout: [{ item: 'rug', x: 500, y: 650 }] }
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

  it('updates the idle room at the next interval without fetching the caller appearance again', async () => {
    const { wrapper } = await setup()
    view = wrapper
    await button(wrapper, '小桌').trigger('click')
    await button(wrapper, '撤销').trigger('click')
    expect(button(wrapper, '重做').element.disabled).toBe(false)
    vi.mocked(getSharedRoom).mockResolvedValue(latest)
    await vi.advanceTimersByTimeAsync(15_000)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(button(wrapper, '重做').element.disabled).toBe(true)
    expect(wrapper.get('[role="status"]').text()).toBe('已同步最新布局。')
    expect(getAvatarAppearance).toHaveBeenCalledTimes(1)
    expect(saveSharedRoom).not.toHaveBeenCalled()
  })

  it('invalidates conflict choices when polling observes a newer shared version', async () => {
    const { wrapper } = await setup()
    view = wrapper
    await button(wrapper, '小桌').trigger('click')
    const table = { item: 'table', x: 240, y: 300 } as const
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 1, layout: [table] })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    await button(wrapper, '共享布局 · 后排偏左').trigger('click')
    expect(button(wrapper, '应用到草稿').element.disabled).toBe(false)
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 2, layout: [{ item: 'rug', x: 500, y: 650 }] })
    await vi.advanceTimersByTimeAsync(15_000)
    expect(wrapper.find('.room-resolution').exists()).toBe(false)
    expect(wrapper.get('[role="alert"]').text()).toContain('共享布局再次更新')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    expect(saveSharedRoom).not.toHaveBeenCalled()
  })

  it('keeps the draft and requires the existing merge action before saving a known stale version', async () => {
    const { wrapper } = await setup()
    view = wrapper
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(getSharedRoom).mockResolvedValue(latest)
    await vi.advanceTimersByTimeAsync(15_000)
    expect(wrapper.get('[role="status"]').text()).toContain('有新的共享布局')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(false)
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(button(wrapper, '保存布局').element.disabled).toBe(false)
    expect(saveSharedRoom).not.toHaveBeenCalled()
  })

  it('pauses while hidden and resumes with a fresh consent read', async () => {
    vi.mocked(getSharedRoom).mockResolvedValueOnce(shared)
    const { wrapper } = await setup()
    view = wrapper
    expect(wrapper.find('[label="对方的形象"]').exists()).toBe(true)
    hidden = true
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(30_000)
    expect(getSharedRoom).toHaveBeenCalledTimes(1)
    expect(wrapper.find('[label="对方的形象"]').exists()).toBe(false)
    vi.mocked(getSharedRoom).mockResolvedValue(latest)
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expect(getSharedRoom).toHaveBeenCalledTimes(2)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(wrapper.find('[label="对方的形象"]').exists()).toBe(false)
  })

  it('cancels a pending poll when saving and ignores a late response', async () => {
    const { wrapper } = await setup()
    view = wrapper
    let complete!: (room: SharedRoom) => void
    vi.mocked(getSharedRoom).mockReturnValueOnce(new Promise(resolve => { complete = resolve }))
    await vi.advanceTimersByTimeAsync(15_000)
    const signal = vi.mocked(getSharedRoom).mock.lastCall?.[0]
    await button(wrapper, '小桌').trigger('click')
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(signal?.aborted).toBe(true)
    expect(saveSharedRoom).toHaveBeenCalledWith(0, [{ item: 'table', ...DEFAULT_ITEM_SPOTS.table }])
    complete(latest)
    await flushPromises()
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
  })

  it('times out an unanswered poll and retries on the next interval', async () => {
    vi.mocked(getSharedRoom).mockResolvedValueOnce(shared)
    const { wrapper } = await setup()
    view = wrapper
    vi.mocked(getSharedRoom).mockReturnValueOnce(new Promise(() => {}))
    await vi.advanceTimersByTimeAsync(15_000)
    const signal = vi.mocked(getSharedRoom).mock.lastCall?.[0]
    await vi.advanceTimersByTimeAsync(8_000)
    expect(signal?.aborted).toBe(true)
    expect(wrapper.get('[role="status"]').text()).toContain('暂时无法自动同步')
    expect(wrapper.find('[label="对方的形象"]').exists()).toBe(false)
    vi.mocked(getSharedRoom).mockResolvedValue(latest)
    await vi.advanceTimersByTimeAsync(7_000)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
  })

  it('clears the room and stops polling after access is lost', async () => {
    const { wrapper } = await setup()
    view = wrapper
    vi.mocked(getSharedRoom).mockRejectedValueOnce(new ApiError('SPACE_NOT_FOUND', 'no space', 404))
    await vi.advanceTimersByTimeAsync(30_000)
    expect(wrapper.find('[aria-label="共享房间"]').exists()).toBe(false)
    expect(wrapper.find('[role="alert"]').exists()).toBe(true)
    expect(getSharedRoom).toHaveBeenCalledTimes(2)
  })
})

describe('shared room', () => {
  it('resolves a contested item while keeping both members’ compatible edits', async () => {
    vi.mocked(getSharedRoom).mockResolvedValueOnce({ ...empty, version: 1, layout: [{ item: 'table', x: 200, y: 220 }] })
    const { wrapper } = await setup()
    await button(wrapper, '布置').trigger('click')
    await wrapper.get('[data-test="room-item-table"]').trigger('click')
    await wrapper.get('[data-test="room-floor"]').trigger('click', { clientX: 400, clientY: 320 })
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 40%')
    await button(wrapper, '盆栽').trigger('click')
    const lamp = { item: 'lamp', x: 830, y: 820 } as const
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty, version: 2, layout: [{ item: 'table', x: 720, y: 260 }, lamp],
    })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    expect(button(wrapper, '应用到草稿').element.disabled).toBe(true)
    await button(wrapper, '我的布置 · 后排居中').trigger('click')
    await button(wrapper, '应用到草稿').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-lamp"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    await button(wrapper, '撤销').trigger('click')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 72%')
    await button(wrapper, '重做').trigger('click')
    vi.mocked(saveSharedRoom).mockResolvedValueOnce(3)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledExactlyOnceWith(2, [
      { item: 'table', x: 400, y: 320 }, lamp, { item: 'plant', ...DEFAULT_ITEM_SPOTS.plant },
    ])
    wrapper.unmount()
  })

  it('keeps apply disabled while two non-rug items overlap until one moves or hides', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty, version: 1, layout: [{ item: 'plant', x: 520, y: 605 }],
    })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    const plantFieldset = wrapper.findAll('.room-resolution fieldset').find(el => el.text().includes('盆栽'))!
    const tableFieldset = wrapper.findAll('.room-resolution fieldset').find(el => el.text().includes('小桌'))!
    await tableFieldset.findAll('button').find(btn => btn.text() === '我的布置 · 中景居中')!.trigger('click')
    await plantFieldset.findAll('button').find(btn => btn.text() === '共享布局 · 中景居中')!.trigger('click')
    expect(button(wrapper, '应用到草稿').element.disabled).toBe(true)
    expect(wrapper.get('.room-resolution [role="status"]').text()).toContain('小桌和盆栽位置重叠了')
    await tableFieldset.findAll('button').find(btn => btn.text() === '共享布局 · 不摆放')!.trigger('click')
    await button(wrapper, '应用到草稿').trigger('click')
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    wrapper.unmount()
  })

  it('uses valid room coordinates and unique furniture for every template', () => {
    expect(ROOM_LAYOUT_PRESETS).toHaveLength(3)
    for (const preset of ROOM_LAYOUT_PRESETS) {
      expect(isSharedRoom({ ...empty, layout: preset.layout }), preset.name).toBe(true)
    }
  })

  it('applies a template as one draft step and only sends it on explicit save', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '盆栽').trigger('click')
    await button(wrapper, '套用茶角').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(false)
    expect(button(wrapper, '套用茶角').element.disabled).toBe(true)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    await button(wrapper, '撤销').trigger('click')
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(false)
    await button(wrapper, '重做').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledExactlyOnceWith(0, ROOM_LAYOUT_PRESETS[0]!.layout)
    wrapper.unmount()
  })

  it('recovers a draft and its undo and redo history after leaving the room', async () => {
    const { wrapper, pinia } = await setup()
    await button(wrapper, '小桌').trigger('click')
    await wrapper.get('[data-test="room-floor"]').trigger('click', { clientX: 640, clientY: 500 })
    await button(wrapper, '撤销').trigger('click')
    wrapper.unmount()

    const returned = mountRoom(pinia)
    expect(returned.find('[aria-label="共享房间"]').exists()).toBe(false)
    await flushPromises()
    expect(returned.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 50%')
    expect(button(returned, '撤销').element.disabled).toBe(false)
    await button(returned, '重做').trigger('click')
    expect(returned.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 64%')
    expect(saveSharedRoom).not.toHaveBeenCalled()
    returned.unmount()
  })

  it('keeps a restored draft against its original version when a partner placement overlaps', async () => {
    const { wrapper, pinia } = await setup()
    await button(wrapper, '小桌').trigger('click')
    wrapper.unmount()
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 1, layout: [{ item: 'plant', x: 510, y: 610 }] })
    const returned = mountRoom(pinia)
    await flushPromises()
    expect(returned.get('[role="alert"]').text()).toContain('小桌和盆栽位置重叠了')
    expect(returned.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(button(returned, '保存布局').element.disabled).toBe(true)
    await button(returned, '放弃草稿并采用最新布局').trigger('click')
    await flushPromises()
    expect(returned.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect(button(returned, '撤销').element.disabled).toBe(true)
    returned.unmount()
  })

  it('clears a remembered draft on session reset while away from the room', async () => {
    const { wrapper, session, pinia } = await setup()
    const oldSpace = session.space
    await button(wrapper, '小桌').trigger('click')
    wrapper.unmount()
    session.resetBusinessState()
    session.user = user
    session.space = oldSpace
    const returned = mountRoom(pinia)
    await flushPromises()
    expect(returned.find('[data-test="room-item-table"]').exists()).toBe(false)
    expect(button(returned, '撤销').element.disabled).toBe(true)
    returned.unmount()
  })

  it('undoes placing, moving and removing, then redoes the same draft steps', async () => {
    const { wrapper } = await setup()
    expect(button(wrapper, '撤销').element.disabled).toBe(true)
    await button(wrapper, '小桌').trigger('click')
    await wrapper.get('[data-test="room-floor"]').trigger('click', { clientX: 660, clientY: 520 })
    await wrapper.get('[data-test="room-item-remove"]').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(false)
    await button(wrapper, '撤销').trigger('click')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 66%')
    await button(wrapper, '撤销').trigger('click')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 50%')
    await button(wrapper, '撤销').trigger('click')
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    await button(wrapper, '重做').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    await button(wrapper, '重做').trigger('click')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 66%')
    await button(wrapper, '重做').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(false)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    wrapper.unmount()
  })

  it('drops redo after a new edit and clears undo after a successful save', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    await button(wrapper, '撤销').trigger('click')
    await button(wrapper, '盆栽').trigger('click')
    expect(button(wrapper, '重做').element.disabled).toBe(true)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledExactlyOnceWith(0, [{ item: 'plant', ...DEFAULT_ITEM_SPOTS.plant }])
    expect(button(wrapper, '撤销').element.disabled).toBe(true)
    wrapper.unmount()
  })

  it('undoes a reconciled draft to the latest shared base without removing the partner edit', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty, version: 1, layout: [{ item: 'rug', x: 500, y: 650 }],
    })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    await button(wrapper, '撤销').trigger('click')
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(false)
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    await button(wrapper, '重做').trigger('click')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    wrapper.unmount()
  })

  it('places furniture freely, saves its version and reads a later layout', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledWith(0, [{ item: 'table', ...DEFAULT_ITEM_SPOTS.table }])
    expect(wrapper.text()).toContain('已保存布局')
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 2, layout: [{ item: 'rug', x: 500, y: 650 }] })
    await button(wrapper, '读取最新布局').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    wrapper.unmount()
  })

  it('keeps a conflicting draft visible and refuses a blind retry', async () => {
    vi.mocked(saveSharedRoom).mockRejectedValue(new ApiError('SHARED_ROOM_VERSION_CONFLICT', 'changed', 409))
    const { wrapper } = await setup()
    await button(wrapper, '盆栽').trigger('click')
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toContain('草稿仍在这里')
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect((button(wrapper, '保存布局').element as HTMLButtonElement).disabled).toBe(true)
    expect(saveSharedRoom).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('merges nonoverlapping placements and requires explicit save with the latest version', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(saveSharedRoom).mockRejectedValueOnce(new ApiError('SHARED_ROOM_VERSION_CONFLICT', 'changed', 409))
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    const rug = { item: 'rug', x: 500, y: 650 } as const
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 3, layout: [rug] })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-item-rug"]').exists()).toBe(true)
    expect(saveSharedRoom).toHaveBeenCalledTimes(1)
    vi.mocked(saveSharedRoom).mockResolvedValueOnce(4)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenLastCalledWith(3, [{ item: 'table', ...DEFAULT_ITEM_SPOTS.table }, rug])
    wrapper.unmount()
  })

  it('keeps the original draft on an overlap conflict until choosing the latest layout', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 1, layout: [{ item: 'plant', x: 510, y: 610 }] })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toContain('小桌和盆栽位置重叠了')
    expect(wrapper.find('[data-test="room-item-table"]').exists()).toBe(true)
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    await button(wrapper, '放弃草稿并采用最新布局').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="room-item-plant"]').exists()).toBe(true)
    expect(wrapper.find('[role="alert"]').exists()).toBe(false)
    expect(saveSharedRoom).not.toHaveBeenCalled()
    wrapper.unmount()
  })

  it('recognizes a save that succeeded despite a lost response', async () => {
    const { wrapper } = await setup()
    await button(wrapper, '小桌').trigger('click')
    vi.mocked(saveSharedRoom).mockRejectedValueOnce(new Error('network interruption'))
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    vi.mocked(getSharedRoom).mockResolvedValue({ ...empty, version: 1, layout: [{ item: 'table', ...DEFAULT_ITEM_SPOTS.table }] })
    await button(wrapper, '读取最新布局，保留草稿').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role="status"]').text()).toBe('最新布局已包含你的摆放。')
    expect(button(wrapper, '保存布局').element.disabled).toBe(true)
    expect(saveSharedRoom).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('requires a decision for moving versus removing the same furniture', () => {
    const table = { item: 'table', x: 240, y: 300 } as const
    expect(mergeRoomLayout([table], [], [{ ...table, x: 480 }])).toEqual({
      conflicts: [{ kind: 'item', item: 'table' }],
    })
    expect(mergeRoomLayout([table], [], [table])).toEqual({ layout: [] })
  })

  it('confirms sharing before rendering the enabled setting and clears the room on identity loss', async () => {
    vi.mocked(setRoomAvatarSharing).mockResolvedValue(true)
    const { wrapper, session } = await setup()
    expect(wrapper.find('[label="对方的形象"]').exists()).toBe(false)
    await wrapper.get('#room-avatar-sharing').trigger('click')
    await flushPromises()
    expect(setRoomAvatarSharing).toHaveBeenCalledWith(true)
    expect((wrapper.get('#room-avatar-sharing').element as HTMLInputElement).checked).toBe(true)
    session.user = null
    await flushPromises()
    expect(wrapper.find('[aria-label="共享房间"]').exists()).toBe(false)
    wrapper.unmount()
  })
})

describe('shared room free arrangement and night lighting', () => {
  it('removes grid cells, supports smooth pointer dragging and flipping, and saves continuous coordinates', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      version: 1,
      layout: [{ item: 'table', x: 300, y: 400 }, { item: 'plant', x: 170, y: 290 }],
    })
    const { wrapper } = await setup()
    await button(wrapper, '布置').trigger('click')
    expect(wrapper.find('.room-cell').exists()).toBe(false)
    expect(wrapper.get('[data-test="room-capacity-badge"]').text()).toContain('2 / 4')

    const tableBtn = wrapper.get('[data-test="room-item-table"]')
    await tableBtn.trigger('pointerdown', { clientX: 100, clientY: 100, button: 0 })
    window.dispatchEvent(new MouseEvent('pointermove', { clientX: 180, clientY: 140 }))
    window.dispatchEvent(new MouseEvent('pointerup', { clientX: 180, clientY: 140 }))
    await flushPromises()

    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('left: 50%')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('style')).toContain('top: 60%')
    expect(wrapper.find('[data-test="room-item-toolbar"]').exists()).toBe(true)

    await wrapper.get('[data-test="room-item-flip"]').trigger('click')
    expect(wrapper.get('[data-test="room-item-table"]').attributes('data-flipped')).toBe('true')

    vi.mocked(saveSharedRoom).mockResolvedValueOnce(2)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledExactlyOnceWith(1, [
      { item: 'plant', x: 170, y: 290 },
      { item: 'table', x: 500, y: 600, flip: true },
    ])
    wrapper.unmount()
  })

  it('places more than 4 furniture items and layers items directly on top of the rug', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      version: 1,
      furniture: [
        { item: 'bookshelf', price: 20, owned: true },
        { item: 'armchair', price: 30, owned: true },
      ],
      layout: [
        { item: 'rug', x: 500, y: 600 },
        { item: 'plant', x: 170, y: 300 },
        { item: 'lamp', x: 830, y: 300 },
        { item: 'bookshelf', x: 500, y: 235 },
        { item: 'armchair', x: 700, y: 560 },
      ],
    })
    const { wrapper } = await setup()
    await button(wrapper, '布置').trigger('click')
    await button(wrapper, '小桌').trigger('click')
    expect(wrapper.get('[data-test="room-capacity-badge"]').text()).toContain('6 / 6')

    vi.mocked(saveSharedRoom).mockResolvedValueOnce(2)
    await button(wrapper, '保存布局').trigger('click')
    await flushPromises()
    expect(saveSharedRoom).toHaveBeenCalledWith(1, expect.arrayContaining([
      { item: 'rug', x: 500, y: 600 },
      { item: 'table', x: 500, y: 600 },
      { item: 'bookshelf', x: 500, y: 235 },
      { item: 'armchair', x: 700, y: 560 },
    ]))
    wrapper.unmount()
  })

  it('normalizes legacy 4x3 grid coordinates seamlessly on render', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      version: 1,
      layout: [{ item: 'table', x: 1, y: 1 }],
    })
    const { wrapper } = await setup()
    const style = wrapper.get('[data-test="room-item-table"]').attributes('style')
    expect(style).toContain('left: 37.5%')
    expect(style).toContain('top: 54%')
    wrapper.unmount()
  })

  it('switches to dedicated night scenery, auto-lights the placed floor lamp, and lets user toggle lamp on/off', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      version: 1,
      layout: [{ item: 'lamp', x: 820, y: 290 }],
    })
    const { wrapper } = await setup()
    expect(wrapper.find('.room-scene--night').exists()).toBe(false)
    expect(wrapper.get('[data-test="room-light-toggle"]').text()).toContain('晴昼')

    await wrapper.get('[data-test="room-light-toggle"]').trigger('click')
    expect(wrapper.find('.room-scene--night').exists()).toBe(true)
    expect(wrapper.find('.room-scene--lamp-lit').exists()).toBe(true)
    expect(wrapper.find('.room-item--lamp-lit').exists()).toBe(true)
    expect(wrapper.find('.room-lamp-bulb').exists()).toBe(true)
    expect(wrapper.get('[data-test="room-light-toggle"]').text()).toContain('暖夜')

    // Clicking the floor lamp in night mode toggles it off into calm moonlit night
    await wrapper.get('[data-test="room-item-lamp"]').trigger('click')
    expect(wrapper.find('.room-scene--night').exists()).toBe(true)
    expect(wrapper.find('.room-scene--moonlit').exists()).toBe(true)
    expect(wrapper.find('.room-item--lamp-lit').exists()).toBe(false)
    expect(wrapper.get('[data-test="room-light-toggle"]').text()).toContain('月夜')

    // Clicking the floor lamp again relights it
    await wrapper.get('[data-test="room-item-lamp"]').trigger('click')
    expect(wrapper.find('.room-scene--lamp-lit').exists()).toBe(true)
    expect(wrapper.get('[data-test="room-light-toggle"]').text()).toContain('暖夜')

    // HUD toggle turns night back to sunlit day
    await wrapper.get('[data-test="room-light-toggle"]').trigger('click')
    expect(wrapper.find('.room-scene--night').exists()).toBe(false)
    wrapper.unmount()
  })

  it('lazily loads partner latest note on partner tap and lets self request a room swap inline', async () => {
    vi.mocked(listMessages).mockResolvedValue({
      messages: [{
        id: 'm-partner-1', author_id: 'member-b', recipient_id: 'member-a',
        body: '晚上买个草莓蛋糕好不好', created_at: stamp, reply: null,
      }],
      next_cursor: null,
    })
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      positions: { side: 'left', paired: true, pending: null },
      partner_appearance: {
        source: 'starter_defaults', complete: true, starter_version: 1, loadout_version: 0,
        slots: { hair: null, clothes: null, expression: null, hat: null, handheld: null, accessory: null },
      },
    })
    const { wrapper } = await setup()
    expect(listMessages).not.toHaveBeenCalled()

    await wrapper.get('[data-test="interact-partner"]').trigger('click')
    await flushPromises()
    expect(listMessages).toHaveBeenCalledWith(null, 1, 'partner')
    expect(wrapper.get('[data-test="partner-bubble"]').text()).toContain('晚上买个草莓蛋糕好不好')
    expect(wrapper.find('[data-test="room-partner-read-note"]').exists()).toBe(true)

    await wrapper.get('[data-test="interact-self"]').trigger('click')
    expect(wrapper.find('[data-test="self-bubble"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="room-self-wardrobe"]').exists()).toBe(true)

    vi.mocked(getRoomPositions).mockResolvedValueOnce({
      side: 'left', paired: true, pending: { id: 'f0000000-0000-4000-8000-000000000099', incoming: false },
    })
    await wrapper.get('[data-test="room-swap-request"]').trigger('click')
    await flushPromises()
    expect(changeRoomPosition).toHaveBeenCalledWith(expect.any(String), 'request')
    expect(wrapper.get('[data-test="self-bubble"]').text()).toContain('换位申请已发出')
    expect(wrapper.find('[data-test="room-swap-cancel"]').exists()).toBe(true)
    wrapper.unmount()
  })

  it('displays the 4-button action dock for layout, shop, wardrobe and garden', async () => {
    vi.mocked(getSharedRoom).mockResolvedValue({
      ...empty,
      furniture: [{ item: 'armchair', price: 30, owned: false }],
    })
    const pinia = createPinia()
    const session = useSessionStore(pinia)
    session.user = user
    session.space = { id: 'space', name: '我们', timezone: 'Asia/Shanghai', member_count: 2, created_at: stamp }
    const wrapper = mount(SharedRoomView, {
      global: { plugins: [pinia], stubs: { RouterLink: RouterLinkStub, AvatarFigure: true } },
    })
    await flushPromises()
    const dock = wrapper.get('.room-dock')
    expect(dock.text()).toContain('布置')
    expect(dock.text()).toContain('商店')
    expect(dock.find('[data-test="room-dock-wardrobe"]').text()).toContain('衣柜')
    expect(dock.find('[data-test="room-dock-garden"]').text()).toContain('花园')
    wrapper.unmount()
  })
})
