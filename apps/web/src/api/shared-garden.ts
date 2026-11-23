import { request } from './client'

interface GardenWateringDay {
  watering_date: string
  watered: boolean
  partner_watered: boolean
}

export interface SharedGarden {
  watering_date: string
  total_waterings: number
  watered_today: boolean
  partner_watered_today: boolean
  recent_waterings?: GardenWateringDay[]
}

export interface SharedGardenMonth {
  month: string
  current_month: string
  days: GardenWateringDay[]
}

export function isGardenMonthKey(value: unknown): value is string {
  return typeof value === 'string' && /^\d{4}-(0[1-9]|1[0-2])$/.test(value) && !value.startsWith('0000')
}

function isWateringDay(value: unknown): value is GardenWateringDay {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false
  const day = value as Record<string, unknown>
  return typeof day.watering_date === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(day.watering_date) &&
    typeof day.watered === 'boolean' && typeof day.partner_watered === 'boolean'
}

function isGarden(value: unknown): value is SharedGarden {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false
  const garden = value as Record<string, unknown>
  return typeof garden.watering_date === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(garden.watering_date) &&
    typeof garden.total_waterings === 'number' && Number.isSafeInteger(garden.total_waterings) &&
    garden.total_waterings >= 0 && typeof garden.watered_today === 'boolean' &&
    typeof garden.partner_watered_today === 'boolean' &&
    (garden.recent_waterings === undefined ||
      (Array.isArray(garden.recent_waterings) && garden.recent_waterings.length === 7 &&
        garden.recent_waterings.every(isWateringDay) &&
        garden.recent_waterings[0].watering_date === garden.watering_date))
}

export async function getSharedGarden(signal?: AbortSignal): Promise<SharedGarden> {
  const result = await request<unknown>('/api/v1/shared-garden', { signal })
  if (!isGarden(result)) throw new Error('花园数据格式无效')
  return result
}

function isGardenMonth(value: unknown): value is SharedGardenMonth {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false
  const record = value as Record<string, unknown>
  if (!isGardenMonthKey(record.month) || !isGardenMonthKey(record.current_month) || record.month > record.current_month) return false
  return Array.isArray(record.days) && record.days.length >= 1 && record.days.length <= 31 &&
    record.days.every((day, index) =>
      isWateringDay(day) && day.watering_date === `${record.month}-${String(index + 1).padStart(2, '0')}`)
}

export async function getSharedGardenMonth(month?: string, signal?: AbortSignal): Promise<SharedGardenMonth> {
  const result = await request<unknown>('/api/v1/shared-garden/history', { query: { month }, signal })
  if (!isGardenMonth(result) || (month && result.month !== month)) throw new Error('浇水月历数据格式无效')
  return result
}

export async function waterSharedGarden(date: string): Promise<void> {
  const receipt = await request<{ watering_date?: string } | null>('/api/v1/shared-garden/watering', {
    method: 'PUT', body: { watering_date: date },
  })
  if (receipt?.watering_date !== date) throw new Error('浇水结果待确认')
}
