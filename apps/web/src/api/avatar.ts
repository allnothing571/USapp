/** Avatar contract 0.44.0. Private responses and writes remain in memory only. */
import { request } from './client'

export const AVATAR_SLOTS = ['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory'] as const
export type AvatarSlot = typeof AVATAR_SLOTS[number]
export type CharacterGender = 'female' | 'male'
export const isCharacterGender = (v: unknown): v is CharacterGender => v === 'female' || v === 'male'
export type SlotIds = Record<AvatarSlot, string | null>
export interface AvatarAsset {
  key: string; format: 'svg' | 'webp'; layers: string[]; covers_hair_top: boolean | null
  accessory_position: 'behind_body' | 'neck' | 'face' | 'above_hat' | null
}
export interface AvatarItem {
  id: string; category: AvatarSlot; starter: boolean; version: number; name: string
  price_sunshine: number | null; asset: AvatarAsset; listed?: boolean
}
export interface Appearance {
  character_gender?: CharacterGender
  hair_hidden?: boolean
  source: 'starter_defaults' | 'saved_loadout'; complete: boolean; starter_version: number | null
  loadout_version: number; slots: Record<AvatarSlot, AvatarItem | null>
}
export interface AvatarPurchase {
  id: string; item_id: string; item_version: number; name: string; category: AvatarSlot; asset_key: string
  price_sunshine: number; debited_sunshine: number; sunshine_balance_after: number; operation_id: string; created_at: string
}
export interface OwnedAvatar { purchase: AvatarPurchase; current_item: AvatarItem & { listed: boolean } }
export interface PurchaseResponse {
  replayed: boolean; purchase: AvatarPurchase
  current: { sunshine_balance: number; item: AvatarItem & { listed: boolean } }
}
export interface LoadoutRequest { expected_version: number; slots: SlotIds; character_gender?: CharacterGender; hair_hidden?: boolean }
export interface LoadoutResponse {
  replayed: boolean
  loadout: { id: string; version: number; slots: SlotIds; requested: SlotIds; starter_version: number | null; created_at: string; character_gender?: CharacterGender; hair_hidden?: boolean }
  current: { appearance: Appearance } | null
}
export const emptySlots = (): SlotIds => ({ hair: null, clothes: null, expression: null, hat: null, handheld: null, accessory: null })
const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v)
const integer = (v: unknown, min = 0, max = 1_000_000_000): v is number => Number.isSafeInteger(v) && (v as number) >= min && (v as number) <= max
const uuid = (v: unknown): v is string => typeof v === 'string' && /^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/i.test(v)
const date = (v: unknown): v is string => typeof v === 'string' && /^\d{4}-\d\d-\d\dT.*Z$/.test(v) && Number.isFinite(Date.parse(v))
export const isAvatarSlot = (v: unknown): v is AvatarSlot => AVATAR_SLOTS.includes(v as AvatarSlot)
export function isSlotIds(v: unknown): v is SlotIds {
  return object(v) && Object.keys(v).length === 6 && AVATAR_SLOTS.every(s => v[s] === null || uuid(v[s]))
}
export const sameSlots = (a: SlotIds, b: SlotIds): boolean => AVATAR_SLOTS.every(s => a[s] === b[s])
export function isAvatarItem(v: unknown, listed = false): v is AvatarItem {
  if (!object(v) || !uuid(v.id) || !isAvatarSlot(v.category) || typeof v.starter !== 'boolean' || !integer(v.version, 1) ||
    typeof v.name !== 'string' || !v.name.trim() || Array.from(v.name).length > 20 ||
    !(v.starter ? v.price_sunshine === null : integer(v.price_sunshine, 1, 9999)) || !object(v.asset)) return false
  const a = v.asset
  const receivedLayers = a.layers
  const layers = v.category === 'hair' ? ['back', 'front', 'front_under_hat'] : ['main']
  return (!listed || typeof v.listed === 'boolean') && typeof a.key === 'string' &&
    new RegExp(`^${v.category}\\.[a-z0-9_]{1,40}\\.v[1-9][0-9]{0,2}$`).test(a.key) &&
    (a.format === 'svg' || a.format === 'webp') && Array.isArray(receivedLayers) &&
    receivedLayers.length === layers.length && layers.every(l => receivedLayers.includes(l)) &&
    (v.category === 'hat' ? typeof a.covers_hair_top === 'boolean' : a.covers_hair_top === null) &&
    (v.category === 'accessory' ? ['behind_body', 'neck', 'face', 'above_hat'].includes(a.accessory_position as string) : a.accessory_position === null)
}
export function isAppearance(v: unknown): v is Appearance {
  if (!object(v) || (v.source !== 'starter_defaults' && v.source !== 'saved_loadout') || typeof v.complete !== 'boolean' ||
    (v.character_gender !== undefined && !isCharacterGender(v.character_gender)) ||
    (v.hair_hidden !== undefined && typeof v.hair_hidden !== 'boolean') ||
    !integer(v.loadout_version) || !(v.starter_version === null || integer(v.starter_version, 1)) || !object(v.slots) || Object.keys(v.slots).length !== 6) return false
  const slots = v.slots
  if (!AVATAR_SLOTS.every(s => slots[s] === null || (isAvatarItem(slots[s]) && slots[s].category === s))) return false
  if (v.source === 'saved_loadout') return v.complete && v.loadout_version > 0 && ['hair', 'clothes', 'expression'].every(s => slots[s] !== null)
  return v.loadout_version === 0 && (v.complete
    ? v.starter_version !== null && ['hair', 'clothes', 'expression'].every(s => isAvatarItem(slots[s]) && slots[s].starter) && ['hat', 'handheld', 'accessory'].every(s => slots[s] === null)
    : v.starter_version === null && AVATAR_SLOTS.every(s => slots[s] === null))
}
export function isAvatarPurchase(v: unknown): v is AvatarPurchase {
  return object(v) && uuid(v.id) && uuid(v.item_id) && integer(v.item_version, 1) && typeof v.name === 'string' &&
    isAvatarSlot(v.category) && typeof v.asset_key === 'string' && integer(v.price_sunshine, 1, 9999) &&
    v.debited_sunshine === v.price_sunshine && integer(v.sunshine_balance_after) && uuid(v.operation_id) && date(v.created_at)
}
export function isOwnedAvatar(v: unknown): v is OwnedAvatar {
  return object(v) && isAvatarPurchase(v.purchase) && isAvatarItem(v.current_item, true) && !v.current_item.starter &&
    v.current_item.id === v.purchase.item_id && v.current_item.category === v.purchase.category && v.current_item.version >= v.purchase.item_version
}
export function isPurchaseResponse(v: unknown, captured: AvatarItem): v is PurchaseResponse {
  if (!object(v) || typeof v.replayed !== 'boolean' || !isAvatarPurchase(v.purchase) || !object(v.current) ||
    !integer(v.current.sunshine_balance) || !isOwnedAvatar({ purchase: v.purchase, current_item: v.current.item })) return false
  const p = v.purchase
  return p.item_id === captured.id && p.item_version === captured.version && p.category === captured.category &&
    p.name === captured.name && p.asset_key === captured.asset.key && p.price_sunshine === captured.price_sunshine
}
export function isLoadoutResponse(v: unknown, captured: LoadoutRequest): v is LoadoutResponse {
  if (!object(v) || typeof v.replayed !== 'boolean' || !object(v.loadout)) return false
  const l = v.loadout
  if (!uuid(l.id) || l.version !== captured.expected_version + 1 || !isSlotIds(l.slots) || !isSlotIds(l.requested) ||
    !sameSlots(l.requested, captured.slots) || l.character_gender !== captured.character_gender || l.hair_hidden !== captured.hair_hidden ||
    !date(l.created_at) || !(l.starter_version === null || integer(l.starter_version, 1))) return false
  const resolved = l.slots
  if (!AVATAR_SLOTS.every(s => captured.slots[s] === null && ['hair', 'clothes', 'expression'].includes(s)
    ? uuid(resolved[s]) : resolved[s] === captured.slots[s])) return false
  const defaults = ['hair', 'clothes', 'expression'].some(s => captured.slots[s as AvatarSlot] === null)
  if (defaults !== (l.starter_version !== null)) return false
  return v.current === null || (object(v.current) && isAppearance(v.current.appearance) && v.current.appearance.loadout_version >= (l.version as number))
}
export function isCatalog(v: unknown): v is { items: AvatarItem[] } {
  return object(v) && Array.isArray(v.items) && v.items.every(i => isAvatarItem(i)) && new Set(v.items.map(i => i.id)).size === v.items.length
}
export function isOwnedPage(v: unknown): v is { items: OwnedAvatar[]; next_cursor: string | null } {
  return object(v) && Array.isArray(v.items) && v.items.every(isOwnedAvatar) &&
    (v.next_cursor === null || (typeof v.next_cursor === 'string' && v.next_cursor.length > 0 && v.next_cursor.length <= 512))
}
export const getAvatarCatalog = () => request<unknown>('/api/v1/avatar/catalog')
export const getAvatarAppearance = () => request<unknown>('/api/v1/avatar')
export const getOwnedAvatars = (cursor?: string | null) => request<unknown>('/api/v1/avatar/owned', { query: { limit: 20, cursor: cursor ?? undefined } })
export const purchaseAvatar = (item: { item_id: string; expected_version: number }, key: string) =>
  request<unknown>('/api/v1/avatar/purchases', { method: 'POST', body: item, headers: { 'Idempotency-Key': key } })
export const saveAvatarLoadout = (body: LoadoutRequest, key: string) =>
  request<unknown>('/api/v1/avatar/loadout', { method: 'POST', body, headers: { 'Idempotency-Key': key } })
