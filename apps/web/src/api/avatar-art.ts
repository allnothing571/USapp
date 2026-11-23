import manifestJson from '@/assets/avatar-manifest.json'
import { AVATAR_SLOTS, type AvatarItem, type AvatarSlot, type CharacterGender } from './avatar'
import type { Space } from './types'

interface File { file: string; sha256: string }
interface Art { category: string; format: string; covers_hair_top: boolean | null; accessory_position: string | null; layers: Record<string, File> }
const manifest = manifestJson as {
  base: File
  bases?: Partial<Record<CharacterGender, File>>
  assets: Record<string, Art>
}
export type FigureSlots = Record<AvatarSlot, AvatarItem | null>
export type RoomCharacter = 'left' | 'right'

export function roomCharacter(space: Space | null, userId?: string): RoomCharacter {
  return space?.members?.find(member => member.user_id === userId)?.slot === 2 ? 'right' : 'left'
}
export const originalGender = (side: RoomCharacter): CharacterGender => side === 'left' ? 'female' : 'male'
export const emptyFigureSlots = (): FigureSlots => ({ hair: null, clothes: null, expression: null, hat: null, handheld: null, accessory: null })
export const SLOT_LABEL: Record<AvatarSlot, string> = { hair: '头发', clothes: '衣服', expression: '表情', hat: '帽子', handheld: '手持物', accessory: '配饰' }
export function knownArt(item: AvatarItem | null): Art | null {
  if (!item) return null
  const art = manifest.assets[item.asset.key]
  return art && art.category === item.category && art.format === item.asset.format &&
    art.covers_hair_top === item.asset.covers_hair_top && art.accessory_position === item.asset.accessory_position &&
    Object.keys(art.layers).length === item.asset.layers.length && item.asset.layers.every(l => !!art.layers[l]) ? art : null
}
export function itemThumbnail(item: AvatarItem): string | null {
  const a = knownArt(item)
  const file = a?.layers[item.category === 'hair' ? 'front' : 'main']
  return file ? '/avatars/' + file.file : null
}
export function figureLayers(slots: FigureSlots, gender?: CharacterGender): { layers: { key: string; src: string }[]; missing: AvatarSlot[] } {
  const layers: { key: string; src: string }[] = []
  const missing = AVATAR_SLOTS.filter(s => slots[s] !== null && !knownArt(slots[s]))
  function add(slot: AvatarSlot, layer = 'main') {
    const item = slots[slot]
    const art = knownArt(item)
    const file = art?.layers[layer]
    if (file && item) layers.push({ key: slot + ':' + layer, src: '/avatars/' + file.file })
  }
  const position = knownArt(slots.accessory)?.accessory_position
  if (position === 'behind_body') add('accessory')
  add('hair', 'back')
  const base = (gender ? manifest.bases?.[gender] : undefined) ?? manifest.base
  layers.push({ key: 'body', src: '/avatars/' + base.file })
  add('clothes'); add('expression')
  if (position === 'neck') add('accessory')
  add('hair', knownArt(slots.hat)?.covers_hair_top ? 'front_under_hat' : 'front')
  add('hat')
  if (position === 'face') add('accessory')
  add('handheld')
  if (position === 'above_hat') add('accessory')
  return { layers, missing }
}
