/**
 * Contract 0.47.0, admin surface only (docs/admin-wallet-ledger.md). Read-only inspection of one selected user's wallet.
 * Amounts and sequences are canonical decimal strings and are never converted to Number here: values beyond 2^53 stay exact.
 */
import { adminHttp } from './admin';

export const assets = ['flower', 'sunshine'] as const;
export type Asset = (typeof assets)[number];
export const assetLabels: Record<Asset, string> = { flower: '小红花', sunshine: '阳光值' };

export interface WalletBalance { asset: Asset; account_exists: boolean; balance: string; latest_seq: string }
export interface WalletSummary {
  user: { id: string; status: string };
  space: { couple_id: string; slot: number } | null;
  balances: WalletBalance[];
  read_at: string;
}
export interface LedgerEntry {
  seq: string;
  amount: string;
  balance_after: string;
  recorded_at: string;
  operation: { id: string; type: string; source_type: string; source_id: string };
}
export interface LedgerPage {
  user_id: string;
  asset: Asset;
  account_exists: boolean;
  balance: string;
  latest_seq: string;
  read_at: string;
  items: LedgerEntry[];
  next_cursor: string | null;
}

const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v);
/** A signed 64-bit integer as a canonical decimal string. */
export const isDecimal = (v: unknown): v is string => typeof v === 'string' && /^(0|-?[1-9][0-9]{0,18})$/.test(v) &&
  (v.startsWith('-') ? BigInt(v) >= -(2n ** 63n) : BigInt(v) <= 2n ** 63n - 1n);
const uuid = (v: unknown): v is string => typeof v === 'string' && /^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const date = (v: unknown): v is string => typeof v === 'string' && Number.isFinite(Date.parse(v));
const asset = (v: unknown): v is Asset => assets.includes(v as Asset);

function isBalance(v: unknown): v is WalletBalance {
  return object(v) && asset(v.asset) && typeof v.account_exists === 'boolean' && isDecimal(v.balance) && isDecimal(v.latest_seq);
}
export function isSummary(v: unknown, userId: string): v is WalletSummary {
  if (!object(v) || !object(v.user) || v.user.id !== userId || typeof v.user.status !== 'string' || !date(v.read_at)) return false;
  if (!(v.space === null || (object(v.space) && uuid(v.space.couple_id) && (v.space.slot === 1 || v.space.slot === 2)))) return false;
  return Array.isArray(v.balances) && v.balances.length === 2 && v.balances.every(isBalance) &&
    (v.balances[0] as WalletBalance).asset === 'flower' && (v.balances[1] as WalletBalance).asset === 'sunshine';
}
function isEntry(v: unknown): v is LedgerEntry {
  return object(v) && isDecimal(v.seq) && isDecimal(v.amount) && isDecimal(v.balance_after) && date(v.recorded_at) &&
    object(v.operation) && uuid(v.operation.id) && typeof v.operation.type === 'string' &&
    typeof v.operation.source_type === 'string' && typeof v.operation.source_id === 'string';
}
export function isPage(v: unknown, userId: string, wanted: Asset): v is LedgerPage {
  return object(v) && v.user_id === userId && v.asset === wanted && typeof v.account_exists === 'boolean' && isDecimal(v.balance) &&
    isDecimal(v.latest_seq) && date(v.read_at) && Array.isArray(v.items) && v.items.length <= 100 && v.items.every(isEntry) &&
    (v.next_cursor === null || (typeof v.next_cursor === 'string' && v.next_cursor.length <= 128));
}

export const walletAdminApi = {
  summary(userId: string, signal?: AbortSignal): Promise<unknown> {
    return adminHttp.request<unknown>(`/users/${encodeURIComponent(userId)}/wallet`, { signal });
  },
  ledger(userId: string, which: Asset, cursor: string | null, limit = 50, signal?: AbortSignal): Promise<unknown> {
    return adminHttp.request<unknown>(`/users/${encodeURIComponent(userId)}/wallet/ledger`, {
      query: { asset: which, limit, cursor: cursor ?? undefined },
      signal,
    });
  },
};
