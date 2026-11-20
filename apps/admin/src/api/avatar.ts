/** Contract 0.44.0, admin surface only. No dependency on the user client. */
import { adminHttp } from './admin';
export const categories = ['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory'] as const;
export type Category = typeof categories[number];
export const labels: Record<Category, string> = { hair: '头发', clothes: '衣服', expression: '表情', hat: '帽子', handheld: '手持物', accessory: '配饰' };
export interface Asset {
  key: string; category: Category; family: string; revision: number; format: 'svg' | 'webp';
  layers: Record<string, string>; covers_hair_top: boolean | null;
  accessory_position: string | null; item_id: string | null; created_at: string;
}
export interface Item {
  id: string; category: Category; starter: boolean; version: number; name: string; price_sunshine: number | null;
  asset: { key: string; format: string; layers: string[]; covers_hair_top: boolean | null; accessory_position: string | null };
  listed: boolean; family: string; created_at: string; revised_at: string; owner_count: number | null;
}
export interface Revision { version: number; name: string; asset_key: string; price_sunshine: number | null; listed: boolean; admin_user_id: string; created_at: string }
export interface Starter { version: number; hair: string; clothes: string; expression: string; admin_user_id: string; created_at: string }
export interface Terms { name: string; asset_key: string; price_sunshine: number | null; listed: boolean }
export type Operation = { kind: 'create'; body: Terms & { category: Category; starter: boolean } } |
  { kind: 'revise'; target: string; base: Item; body: Terms & { expected_version: number } } |
  { kind: 'starter'; body: { expected_version: number; hair: string; clothes: string; expression: string } };
export const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v);
const int = (v: unknown, min = 0): v is number => Number.isSafeInteger(v) && (v as number) >= min;
const uuid = (v: unknown): v is string => typeof v === 'string' && /^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/i.test(v);
const date = (v: unknown) => typeof v === 'string' && Number.isFinite(Date.parse(v));
const category = (v: unknown): v is Category => categories.includes(v as Category);
const price = (v: unknown) => v === null || (int(v, 1) && v <= 9999);
export function isAsset(v: unknown): v is Asset {
  if (!object(v) || !category(v.category) || typeof v.family !== 'string' || !int(v.revision, 1) ||
    v.key !== `${v.family}.v${v.revision}` || !v.family.startsWith(`${v.category}.`) ||
    !['svg', 'webp'].includes(v.format as string) || !object(v.layers) || !date(v.created_at) || !(v.item_id === null || uuid(v.item_id))) return false;
  const layers = v.category === 'hair' ? ['back', 'front', 'front_under_hat'] : ['main'];
  return Object.keys(v.layers).length === layers.length && layers.every(k => typeof (v.layers as Record<string, unknown>)[k] === 'string' && /^[a-f\d]{64}$/.test((v.layers as Record<string, string>)[k]!)) &&
    (v.category === 'hat' ? typeof v.covers_hair_top === 'boolean' : v.covers_hair_top === null) &&
    (v.category === 'accessory' ? ['behind_body', 'neck', 'face', 'above_hat'].includes(v.accessory_position as string) : v.accessory_position === null);
}
export function isItem(v: unknown): v is Item {
  if (!object(v) || !uuid(v.id) || !category(v.category) || typeof v.starter !== 'boolean' || !int(v.version, 1) ||
    typeof v.name !== 'string' || !v.name.trim() || [...v.name].length > 20 || !price(v.price_sunshine) ||
    (v.starter ? v.price_sunshine !== null : v.price_sunshine === null) || typeof v.listed !== 'boolean' ||
    typeof v.family !== 'string' || !v.family.startsWith(`${v.category}.`) || !object(v.asset) ||
    typeof v.asset.key !== 'string' || !new RegExp(`^${v.family.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\.v[1-9][0-9]*$`).test(v.asset.key) ||
    !date(v.created_at) || !date(v.revised_at) || !(v.starter ? v.owner_count === null : int(v.owner_count))) return false;
  const layers = v.category === 'hair' ? ['back', 'front', 'front_under_hat'] : ['main'];
  return ['svg', 'webp'].includes(v.asset.format as string) && Array.isArray(v.asset.layers) &&
    v.asset.layers.length === layers.length && layers.every(k => (v.asset as Item['asset']).layers.includes(k)) &&
    (v.category === 'hat' ? typeof v.asset.covers_hair_top === 'boolean' : v.asset.covers_hair_top === null) &&
    (v.category === 'accessory' ? ['behind_body', 'neck', 'face', 'above_hat'].includes(v.asset.accessory_position as string) : v.asset.accessory_position === null);
}
export function isStarter(v: unknown): v is Starter {
  return object(v) && int(v.version, 1) && ['hair', 'clothes', 'expression', 'admin_user_id'].every(k => uuid(v[k])) && date(v.created_at);
}
export function isRevision(v: unknown): v is Revision {
  return object(v) && int(v.version, 1) && typeof v.name === 'string' && typeof v.asset_key === 'string' && price(v.price_sunshine) && typeof v.listed === 'boolean' && uuid(v.admin_user_id) && date(v.created_at);
}
export function validReceipt(v: unknown, op: Operation): boolean {
  if (!object(v) || typeof v.replayed !== 'boolean') return false;
  if (op.kind === 'starter') return isStarter(v.starter) && v.starter.version === op.body.expected_version + 1 &&
    ['hair', 'clothes', 'expression'].every(k => (v.starter as unknown as Record<string, unknown>)[k] === (op.body as unknown as Record<string, unknown>)[k]);
  const expected = op.kind === 'create' ? 1 : op.body.expected_version + 1;
  if (v.revision_version !== expected || !isItem(v.item) || v.item.version < expected) return false;
  if (op.kind === 'revise' && (v.item.id !== op.target || v.item.family !== op.base.family || v.item.category !== op.base.category || v.item.starter !== op.base.starter)) return false;
  if (op.kind === 'create' && (v.item.category !== op.body.category || v.item.starter !== op.body.starter || !v.item.asset.key.startsWith(op.body.asset_key.replace(/\.v\d+$/, '') + '.v'))) return false;
  return v.item.version > expected || (v.item.name === op.body.name && v.item.asset.key === op.body.asset_key && v.item.price_sunshine === op.body.price_sunshine && v.item.listed === op.body.listed);
}
export const avatarAdminApi = {
  assets: () => adminHttp.request<unknown>('/avatar/assets'),
  items: () => adminHttp.request<unknown>('/avatar/items'),
  starter: () => adminHttp.request<unknown>('/avatar/starter'),
  detail: (id: string) => adminHttp.request<unknown>(`/avatar/items/${encodeURIComponent(id)}`),
  write: (op: Operation, key: string) => adminHttp.request<unknown>(op.kind === 'starter' ? '/avatar/starter' : op.kind === 'create' ? '/avatar/items' : `/avatar/items/${encodeURIComponent(op.target)}/revisions`,
    { method: 'POST', headers: { 'Idempotency-Key': key }, body: op.body }),
};
