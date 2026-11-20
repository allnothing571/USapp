/**
 * Shared plan -> memory (upload-capability correction): choosing an existing memory loads the memory list; creating a new photo memory
 * asks only for upload capability and loads no list; switching from convert to link still loads the list. All data is fake.
 */
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/shared-plans', async (original) => ({
  ...(await original<typeof import('@/api/shared-plans')>()),
  getPlan: vi.fn(), listPlans: vi.fn(), sendPlan: vi.fn(),
}))
vi.mock('@/api/memories', () => ({
  MEMORY_PAGE_SIZE: 20,
  createMemory: vi.fn(), listMemories: vi.fn(), getMemory: vi.fn(), editMemory: vi.fn(), proposeMemoryEdit: vi.fn(),
  getMemoryProposal: vi.fn(), decideMemoryProposal: vi.fn(), listMemoryProposals: vi.fn(),
}))
vi.mock('@/api/media-capability', async (original) => ({
  ...(await original<typeof import('@/api/media-capability')>()),
  getMediaCapabilities: vi.fn(),
}))
vi.mock('@/api/unattached', async (original) => ({
  ...(await original<typeof import('@/api/unattached')>()),
  listUnattachedPhotos: vi.fn(),
}))

import { setCsrfToken } from '@/api/client'
import { getMediaCapabilities } from '@/api/media-capability'
import { listMemories } from '@/api/memories'
import { getPlan, type SharedPlan } from '@/api/shared-plans'
import type { Memory } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import SharedPlanDetailView from '@/views/SharedPlanDetailView.vue'

const self = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
const id = '11111111-1111-4111-8111-111111111111'
const memoryId = '44444444-4444-4444-8444-444444444444'
const time = '2026-11-04T01:00:00Z'

function finished(): SharedPlan {
  return { id, creator_id: self, created_at: time, body: { title: '看海', date: null, time: null, place: null, note: null }, body_version: 1,
    status: 'finished', status_version: 3, pending_proposal: null, latest_decision: null, memory_id: null }
}
function memory(): Memory {
  return { id: memoryId, creator_id: self, version: 1, date: '2026-03-01', title: '海边', description: '', created_at: time, updated_at: time,
    photos: [], pending_proposal: null }
}

const mounted: VueWrapper[] = []
async function open(): Promise<VueWrapper> {
  const wrapper = mount(SharedPlanDetailView as never, { props: { id } as never, global: { stubs: ['RouterLink'] } }) as VueWrapper
  mounted.push(wrapper)
  await flushPromises()
  return wrapper
}
async function press(wrapper: VueWrapper, label: string): Promise<void> {
  const button = wrapper.findAll('button').find((b) => b.text() === label)
  if (!button) throw new Error(`no button ${label}`)
  await button.trigger('click')
  await flushPromises()
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.resetAllMocks()
  setCsrfToken('csrf')
  useSessionStore().user = { id: self, email: 'test@example.test', nickname: null } as never
  vi.mocked(getPlan).mockResolvedValue(finished())
  vi.mocked(listMemories).mockResolvedValue({ memories: [memory()], next_cursor: null, uploads_enabled: true } as never)
  vi.mocked(getMediaCapabilities).mockResolvedValue({ uploads_enabled: true })
})

afterEach(() => {
  while (mounted.length) mounted.pop()?.unmount()
})

describe('opening the memory choices of a finished plan', () => {
  it('choosing an existing memory loads the memory list and nothing about uploads', async () => {
    const wrapper = await open()
    await press(wrapper, '选择已有回忆')
    expect(listMemories).toHaveBeenCalledTimes(1)
    expect(getMediaCapabilities).not.toHaveBeenCalled()
    expect(wrapper.get('[data-test="plan-memory-select"]').text()).toContain('海边')
    expect(wrapper.find('[data-test="plan-memory-create-form"]').exists()).toBe(false)
  })

  it('creating a new photo memory asks only for upload capability and loads no list', async () => {
    const wrapper = await open()
    await press(wrapper, '新建照片回忆')
    expect(listMemories).not.toHaveBeenCalled()
    expect(getMediaCapabilities).toHaveBeenCalledTimes(1)
    expect(wrapper.find('[data-test="plan-memory-create-form"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="plan-memory-create-files"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="plan-memory-select"]').exists()).toBe(false)
  })

  it('switching from convert to link loads the list normally, and the capability is not read again', async () => {
    const wrapper = await open()
    await press(wrapper, '新建照片回忆')
    expect(listMemories).not.toHaveBeenCalled()
    await press(wrapper, '选择已有回忆')
    expect(listMemories).toHaveBeenCalledTimes(1)
    expect(wrapper.get('[data-test="plan-memory-select"]').text()).toContain('海边')
    await press(wrapper, '新建照片回忆')
    expect(getMediaCapabilities).toHaveBeenCalledTimes(1)
    expect(listMemories).toHaveBeenCalledTimes(1)
  })
})
