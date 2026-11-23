import manifest from '../../src/assets/avatar-manifest.json'
import { emptySlots, type Appearance, type AvatarItem, type AvatarSlot, type AvatarPurchase, type LoadoutRequest, type LoadoutResponse } from '../../src/api/avatar'
export const id = (n: number) => `10000000-0000-4000-8000-${String(n).padStart(12, '0')}`
export const keys: Record<AvatarSlot, string> = { hair: 'hair.placeholder_bob.v1', clothes: 'clothes.placeholder_cream.v1',
  expression: 'expression.placeholder_smile.v1', hat: 'hat.placeholder_cap.v1', handheld: 'handheld.placeholder_flower.v1', accessory: 'accessory.placeholder_bow.v1' }
export function item(n = 1, category: AvatarSlot = 'hair', starter = true, version = 1): AvatarItem {
  const a = manifest.assets[keys[category] as keyof typeof manifest.assets]
  return { id: id(n), category, starter, version, name: `装扮${n}`, price_sunshine: starter ? null : 20,
    asset: { key: keys[category], format: a.format as AvatarItem['asset']['format'], layers: Object.keys(a.layers), covers_hair_top: a.covers_hair_top,
      accessory_position: a.accessory_position as AvatarItem['asset']['accessory_position'] } }
}
export const hair = () => item(1)
export const clothes = () => item(2, 'clothes')
export const expression = () => item(3, 'expression')
export const hat = () => item(4, 'hat', false)
export function appearance(version = 0): Appearance {
  return { source: version ? 'saved_loadout' : 'starter_defaults', complete: true, starter_version: 1, loadout_version: version,
    slots: { hair: hair(), clothes: clothes(), expression: expression(), hat: null, handheld: null, accessory: null } }
}
export function purchase(i = hat()): AvatarPurchase {
  return { id: id(100), item_id: i.id, item_version: i.version, name: i.name, category: i.category, asset_key: i.asset.key,
    price_sunshine: i.price_sunshine!, debited_sunshine: i.price_sunshine!, sunshine_balance_after: 80,
    operation_id: id(101), created_at: '2026-11-10T01:00:00.123456Z' }
}
export function receipt(i = hat()) {
  return { replayed: false, purchase: purchase(i), current: { sunshine_balance: 80, item: { ...i, listed: true } } }
}
export function loadoutReceipt(request: LoadoutRequest = { expected_version: 0, slots: emptySlots() }, current: Appearance | null = appearance(request.expected_version + 1)): LoadoutResponse {
  const slots = { ...request.slots, hair: request.slots.hair ?? id(1), clothes: request.slots.clothes ?? id(2), expression: request.slots.expression ?? id(3) }
  return { replayed: false, loadout: { id: id(200), version: request.expected_version + 1, slots, requested: { ...request.slots },
    ...(request.character_gender ? { character_gender: request.character_gender } : {}),
    ...(request.hair_hidden !== undefined ? { hair_hidden: request.hair_hidden } : {}),
    starter_version: ['hair', 'clothes', 'expression'].some(s => request.slots[s as AvatarSlot] === null) ? 1 : null,
    created_at: '2026-11-10T02:00:00.000000Z' }, current: current ? { appearance: current } : null }
}
