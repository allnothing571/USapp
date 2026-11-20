/**
 * Read-only wallet inspection of one selected user (docs/admin-wallet-ledger.md Section 5). Memory only.
 * Identities (correction 2026-11-11): a summary belongs to the admin session (epoch and admin id) and the selected user; a
 * ledger page additionally to the selected asset. Changing the asset invalidates only ledger reads, so a pending same-user
 * summary still lands and its loading indicator settles. A user, session or unmount change invalidates both.
 * A 401/403 from any current read clears the private data, settles both loaders and invalidates every other read in flight,
 * so no delayed 200 can repopulate the page. No retry without a click.
 */
import { defineStore } from 'pinia';
import { computed, ref, watch } from 'vue';

import { AdminApiError } from '@/api/client';
import { isPage, isSummary, walletAdminApi as api, type Asset, type LedgerEntry, type WalletSummary } from '@/api/wallet';
import { useAdminSessionStore } from './admin-session';

export const PAGE_SIZE = 50;

export const useUserWalletStore = defineStore('user-wallet', () => {
  const session = useAdminSessionStore();
  const userId = ref<string | null>(null);
  const asset = ref<Asset>('flower');
  const summary = ref<WalletSummary | null>(null);
  const items = ref<LedgerEntry[]>([]);
  const pageInfo = ref<{ balance: string; latestSeq: string; accountExists: boolean; readAt: string } | null>(null);
  const summaryLoading = ref(false);
  const ledgerLoading = ref(false);
  const summaryError = ref('');
  const ledgerError = ref('');
  const notFound = ref(false);
  // cursors[i] is the cursor that loaded page i (null for the first page); next is the server's cursor for page i + 1.
  const cursors = ref<Array<string | null>>([null]);
  const pageIndex = ref(0);
  const nextCursor = ref<string | null>(null);
  // userGeneration: bumped by a user, session, unmount or authorization change (invalidates summary and ledger reads).
  // ledgerGeneration: bumped additionally by an asset change (invalidates ledger reads only).
  let userGeneration = 0;
  let ledgerGeneration = 0;
  let summarySeq = 0;
  let ledgerSeq = 0;

  const owner = () => (session.authenticated ? `${session.epoch}:${session.admin?.id ?? ''}` : '');

  // What was shown for the subject; errors are kept (an authorization loss clears data but reports why).
  function clearShown(): void {
    summary.value = null;
    items.value = [];
    pageInfo.value = null;
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
  }
  function clearData(): void {
    clearShown();
    summaryError.value = ledgerError.value = '';
    notFound.value = false;
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
    summaryLoading.value = ledgerLoading.value = false;
  }
  function reset(): void {
    userGeneration++;
    ledgerGeneration++;
    summarySeq++;
    ledgerSeq++;
    userId.value = null;
    asset.value = 'flower';
    clearData();
  }
  // Logout, a new login or another admin: forget everything at once.
  watch(owner, () => reset(), { flush: 'sync' });

  type SummaryToken = { owner: string; user: string; userGeneration: number };
  type LedgerToken = SummaryToken & { asset: Asset; ledgerGeneration: number };
  const summaryToken = (): SummaryToken => ({ owner: owner(), user: userId.value ?? '', userGeneration });
  const ledgerToken = (): LedgerToken => ({ ...summaryToken(), asset: asset.value, ledgerGeneration });
  const summaryCurrent = (t: SummaryToken) =>
    !!t.owner && t.owner === owner() && t.user === (userId.value ?? '') && t.userGeneration === userGeneration;
  const ledgerCurrent = (t: LedgerToken) => summaryCurrent(t) && t.asset === asset.value && t.ledgerGeneration === ledgerGeneration;

  const AUTH_LOST = '管理员会话已失效或无权访问，已清除显示的数据。请重新登录后再读取。';
  const isAuthLoss = (error: unknown) => error instanceof AdminApiError && (error.status === 401 || error.status === 403);
  // A current read lost authorization: clear the private data, settle both loaders, and make every read still in flight
  // (summary or ledger, any asset) unusable. Recovery is only an explicit refresh or a new selection.
  function authorizationLost(): void {
    userGeneration++;
    ledgerGeneration++;
    summarySeq++;
    ledgerSeq++;
    clearShown();
    summaryLoading.value = ledgerLoading.value = false;
    summaryError.value = ledgerError.value = AUTH_LOST;
  }

  function failure(error: unknown, what: string): string {
    if (error instanceof AdminApiError && error.status === 404) {
      notFound.value = true;
      return '用户不存在。';
    }
    if (error instanceof AdminApiError && error.status === 400) return `${what}请求无效（${error.code}）。`;
    return `${what}读取失败，可点击重试。`;
  }

  async function loadSummary(): Promise<void> {
    const id = userId.value;
    if (!owner() || !id) return;
    const t = summaryToken();
    const s = ++summarySeq;
    summaryLoading.value = true;
    summaryError.value = '';
    try {
      const r = await api.summary(id);
      if (!summaryCurrent(t) || s !== summarySeq) return;
      if (!isSummary(r, id)) throw new Error('shape');
      summary.value = r;
    } catch (error) {
      if (!summaryCurrent(t) || s !== summarySeq) return;
      if (isAuthLoss(error)) { authorizationLost(); return; }
      summary.value = null;
      summaryError.value = failure(error, '余额');
    } finally {
      if (summaryCurrent(t) && s === summarySeq) summaryLoading.value = false;
    }
  }

  async function loadPage(index: number, cursor: string | null): Promise<void> {
    const id = userId.value;
    if (!owner() || !id) return;
    const t = ledgerToken();
    const s = ++ledgerSeq;
    const which = asset.value;
    ledgerLoading.value = true;
    ledgerError.value = '';
    try {
      const r = await api.ledger(id, which, cursor, PAGE_SIZE);
      if (!ledgerCurrent(t) || s !== ledgerSeq) return;
      if (!isPage(r, id, which)) throw new Error('shape');
      items.value = r.items;
      pageInfo.value = { balance: r.balance, latestSeq: r.latest_seq, accountExists: r.account_exists, readAt: r.read_at };
      pageIndex.value = index;
      cursors.value = [...cursors.value.slice(0, index), cursor];
      nextCursor.value = r.next_cursor;
    } catch (error) {
      if (!ledgerCurrent(t) || s !== ledgerSeq) return;
      if (isAuthLoss(error)) { authorizationLost(); return; }
      items.value = [];
      pageInfo.value = null;
      ledgerError.value = failure(error, '流水');
    } finally {
      if (ledgerCurrent(t) && s === ledgerSeq) ledgerLoading.value = false;
    }
  }

  /** Open a user: clears everything shown for the previous subject. */
  async function select(id: string): Promise<void> {
    if (!owner()) return;
    if (id === userId.value && (summary.value || summaryLoading.value)) return;
    userGeneration++;
    ledgerGeneration++;
    userId.value = id;
    asset.value = 'flower';
    clearData();
    await Promise.all([loadSummary(), loadPage(0, null)]);
  }
  /** Switch asset: the ledger restarts from the newest entry. */
  async function selectAsset(next: Asset): Promise<void> {
    if (!owner() || !userId.value || next === asset.value) return;
    // Ledger identity only: a pending same-user summary stays valid and settles its own loading indicator.
    ledgerGeneration++;
    asset.value = next;
    items.value = [];
    pageInfo.value = null;
    ledgerError.value = '';
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
    await loadPage(0, null);
  }
  async function next(): Promise<void> {
    if (!nextCursor.value || ledgerLoading.value) return;
    await loadPage(pageIndex.value + 1, nextCursor.value);
  }
  async function prev(): Promise<void> {
    if (pageIndex.value === 0 || ledgerLoading.value) return;
    await loadPage(pageIndex.value - 1, cursors.value[pageIndex.value - 1] ?? null);
  }
  /** Explicit refresh or retry: both reads again, the ledger from the newest entry. */
  async function refresh(): Promise<void> {
    if (!owner() || !userId.value) return;
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
    await Promise.all([loadSummary(), loadPage(0, null)]);
  }

  const hasPrev = computed(() => pageIndex.value > 0);
  const hasNext = computed(() => nextCursor.value !== null);
  const isEmpty = computed(() => !ledgerLoading.value && !ledgerError.value && pageInfo.value !== null && items.value.length === 0);

  return {
    userId, asset, summary, items, pageInfo, summaryLoading, ledgerLoading, summaryError, ledgerError, notFound, pageIndex,
    hasPrev, hasNext, isEmpty, select, selectAsset, next, prev, refresh, reset,
  };
});
