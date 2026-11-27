import { isAppearance, type Appearance } from './avatar'
import { request } from './client'

export const ROOM_ITEMS = {
  table: '小桌', lamp: '落地灯', plant: '盆栽', rug: '地毯',
  bookshelf: '书架', armchair: '扶手椅', vase: '花瓶',
} as const
export type RoomItem = keyof typeof ROOM_ITEMS
export const FREE_ROOM_ITEMS = ['table', 'lamp', 'plant', 'rug'] as const
const PAID_ROOM_ITEMS = ['bookshelf', 'armchair', 'vase'] as const
export type PaidRoomItem = typeof PAID_ROOM_ITEMS[number]
export const MAX_ROOM_FURNITURE = Object.keys(ROOM_ITEMS).length
export interface RoomFurniture { item: PaidRoomItem; price: number; owned: boolean }
export interface Placement { item: RoomItem; x: number; y: number; flip?: boolean }
export type RoomSide = 'left' | 'right'

export function isLegacyGridCoord(placement: Pick<Placement, 'x' | 'y'>): boolean {
  return Number.isInteger(placement.x) && Number.isInteger(placement.y) &&
    placement.x >= 0 && placement.x <= 3 && placement.y >= 0 && placement.y <= 2
}

export function renderCoords(placement: Pick<Placement, 'x' | 'y'>): { x: number; y: number } {
  return isLegacyGridCoord(placement)
    ? { x: 125 + placement.x * 250, y: 220 + placement.y * 320 }
    : { x: placement.x, y: placement.y }
}
export type PositionAction = 'request' | 'accept' | 'decline' | 'cancel'
export interface RoomPositions { side: RoomSide; paired: boolean; pending: { id: string; incoming: boolean } | null }
export const oppositeSide = (side: RoomSide): RoomSide => side === 'left' ? 'right' : 'left'
export const sideLabel = (side: RoomSide): string => side === 'left' ? '左侧' : '右侧'
export const isRoomSide = (side: unknown): side is RoomSide => side === 'left' || side === 'right'
export interface SharedRoom {
  version: number
  layout: Placement[]
  share_avatar: boolean
  partner_appearance: Appearance | null
  furniture?: RoomFurniture[]
  positions?: RoomPositions
}

function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
}
function integer(value: unknown, max: number): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= max
}
function validPlacement(value: unknown): value is Placement {
  if (!object(value)) return false
  const keys = Object.keys(value)
  if (!keys.every(key => key === 'item' || key === 'x' || key === 'y' || key === 'flip')) return false
  if (value.flip !== undefined && typeof value.flip !== 'boolean') return false
  return typeof value.item === 'string' && Object.hasOwn(ROOM_ITEMS, value.item) &&
    integer(value.x, 1000) && integer(value.y, 1000)
}
function paidItem(value: unknown): value is PaidRoomItem {
  return PAID_ROOM_ITEMS.some(item => value === item)
}

function validFurniture(value: unknown): value is RoomFurniture {
  return object(value) && paidItem(value.item) && integer(value.price, 9999) && value.price > 0 && typeof value.owned === 'boolean'
}
export function isRoomPositions(value: unknown): value is RoomPositions {
  return object(value) && isRoomSide(value.side) && typeof value.paired === 'boolean' &&
    (value.pending === null || (value.paired && object(value.pending) &&
      typeof value.pending.id === 'string' && /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/i.test(value.pending.id) &&
      typeof value.pending.incoming === 'boolean'))
}
export async function getRoomPositions(signal?: AbortSignal): Promise<RoomPositions> {
  const body = await request<unknown>('/api/v1/shared-room/positions', { signal })
  if (!isRoomPositions(body)) throw new Error('站位数据格式无效')
  return body
}
export async function changeRoomPosition(id: string, action: PositionAction): Promise<void> {
  const body = await request<unknown>('/api/v1/shared-room/positions', { method: 'POST', body: { id, action } })
  if (!object(body) || body.id !== id || body.action !== action) throw new Error('换位操作待确认')
}

export function isSharedRoom(value: unknown): value is SharedRoom {
  if (!object(value) || !integer(value.version, 1_000_000_000) ||
    !Array.isArray(value.layout) || value.layout.length > MAX_ROOM_FURNITURE || !value.layout.every(validPlacement) ||
    new Set(value.layout.map(item => item.item)).size !== value.layout.length) return false
  const nonRug = value.layout.filter(entry => entry.item !== 'rug')
  return new Set(nonRug.map(item => item.x + ':' + item.y)).size === nonRug.length &&
    (value.furniture === undefined || (Array.isArray(value.furniture) && value.furniture.length <= PAID_ROOM_ITEMS.length &&
      value.furniture.every(validFurniture) && new Set(value.furniture.map(entry => entry.item)).size === value.furniture.length)) &&
    typeof value.share_avatar === 'boolean' &&
    (value.positions === undefined || isRoomPositions(value.positions)) &&
    (value.partner_appearance === null || isAppearance(value.partner_appearance))
}
export function connectSharedRoom(sharePresence: boolean): WebSocket {
  const url = new URL('/api/v1/shared-room/live', window.location.href)
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'
  if (sharePresence) url.searchParams.set('presence', '1')
  return new WebSocket(url)
}

export async function getSharedRoom(signal?: AbortSignal): Promise<SharedRoom> {
  const body = await request<unknown>('/api/v1/shared-room', { signal })
  if (!isSharedRoom(body)) throw new Error('房间数据格式无效')
  return body
}
export async function buyRoomFurniture(item: PaidRoomItem, key: string): Promise<void> {
  const body = await request<unknown>('/api/v1/shared-room/furniture/purchases', {
    method: 'POST', headers: { 'Idempotency-Key': key }, body: { item },
  })
  if (!object(body) || body.item !== item || body.owned !== true) throw new Error('家具购买结果待确认')
}

export async function saveSharedRoom(expected: number, layout: Placement[]): Promise<number> {
  const body = await request<unknown>('/api/v1/shared-room', { method: 'PUT', body: { expected_version: expected, layout } })
  if (!object(body) || body.version !== expected + 1) throw new Error('房间保存结果待确认')
  return expected + 1
}
export async function setRoomAvatarSharing(shared: boolean): Promise<boolean> {
  const body = await request<unknown>('/api/v1/shared-room/avatar-sharing', { method: 'PUT', body: { share_avatar: shared } })
  if (!object(body) || body.share_avatar !== shared) throw new Error('形象展示设置待确认')
  return shared
}
