import type { Asset, Item, Starter } from '../../src/api/avatar';
export const id = (n: number) => `10000000-0000-4000-8000-${String(n).padStart(12, '0')}`;
export const date = '2026-11-10T00:00:00.000Z';
export const asset: Asset = { key: 'hair.test.v1', family: 'hair.test', category: 'hair', revision: 1, format: 'svg', layers: { back: 'a'.repeat(64), front: 'b'.repeat(64), front_under_hat: 'c'.repeat(64) }, covers_hair_top: null, accessory_position: null, item_id: null, created_at: date };
export const item: Item = { id: id(1), category: 'hair', starter: false, version: 1, name: '测试头发', price_sunshine: 20, asset: { key: asset.key, format: 'svg', layers: Object.keys(asset.layers), covers_hair_top: null, accessory_position: null }, listed: true, family: asset.family, owner_count: 2, created_at: date, revised_at: date };
export const starter: Starter = { version: 1, hair: id(2), clothes: id(3), expression: id(4), admin_user_id: id(99), created_at: date };
