/**
 * Read-only admin wallet inspection (docs/admin-wallet-ledger.md): the API paths and exact-decimal validation, the store's
 * scoping by admin session and selection, paging, failures and no persistence, and the read-only view. fetch is stubbed or the
 * API module is mocked; nothing reaches a network.
 */
import { beforeEach, afterEach, describe, it, expect, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { flushPromises, mount } from '@vue/test-utils';

const api = vi.hoisted(() => ({ summary: vi.fn(), ledger: vi.fn() }));
vi.mock('../../src/api/wallet', async (original) => ({ ...(await original<typeof import('../../src/api/wallet')>()), walletAdminApi: api }));
vi.mock('vue-router', () => ({ useRoute: () => route }));
const route = vi.hoisted(() => ({ params: { userId: '' } as Record<string, string> }));

import { AdminApiError, AdminHttpClient } from '../../src/api/client';
import { isDecimal, isPage, isSummary } from '../../src/api/wallet';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import { useUserWalletStore } from '../../src/stores/user-wallet';
import View from '../../src/views/UserWalletView.vue';

const U1 = '11111111-1111-4111-8111-111111111111';
const U2 = '22222222-2222-4222-8222-222222222222';
const OP = '33333333-3333-4333-8333-333333333333';
const COUPLE = '44444444-4444-4444-8444-444444444444';
const NOW = '2026-11-11T08:00:00.000000Z';
const BIG = '9007199254740993'; // 2^53 + 1: not representable as a JavaScript number

const summary = (user = U1, flower = '12', sunshine = BIG) => ({
  user: { id: user, status: 'active' },
  space: { couple_id: COUPLE, slot: 2 },
  balances: [
    { asset: 'flower', account_exists: true, balance: flower, latest_seq: '3' },
    { asset: 'sunshine', account_exists: true, balance: sunshine, latest_seq: BIG },
  ],
  read_at: NOW,
});
const entry = (seq: string, amount: string, after: string) => ({
  seq, amount, balance_after: after, recorded_at: NOW,
  operation: { id: OP, type: 'TASK_REWARD', source_type: 'task', source_id: OP },
});
const page = (user: string, asset: 'flower' | 'sunshine', items: ReturnType<typeof entry>[], next: string | null = null) => ({
  user_id: user, asset, account_exists: true, balance: items[0]?.balance_after ?? '0', latest_seq: items[0]?.seq ?? '0',
  read_at: NOW, items, next_cursor: next,
});
const deferred = <T>() => {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((r, j) => { resolve = r; reject = j; });
  return { promise, resolve, reject };
};
function signIn(id = 'admin-1'): void {
  const s = useAdminSessionStore();
  s.$patch({ authenticated: true, admin: { id, username: 'test-admin' }, epoch: s.epoch + 1 });
}

beforeEach(() => {
  setActivePinia(createPinia());
  vi.clearAllMocks();
  signIn();
  api.summary.mockResolvedValue(summary());
  api.ledger.mockResolvedValue(page(U1, 'flower', [entry('3', '-4', '12'), entry('2', '6', '16')]));
});
afterEach(() => vi.unstubAllGlobals());

describe('admin wallet API and validation', () => {
  it('reads with GET on the admin base, encodes the user, and sends asset, limit and cursor; no CSRF or write marker', async () => {
    const fetchImpl = vi.fn(async () => new Response('{}', { status: 200, headers: { 'Content-Type': 'application/json' } }));
    const http = new AdminHttpClient({ baseUrl: '/api/admin/v1', fetchImpl });
    await http.request(`/users/${encodeURIComponent(U1)}/wallet/ledger`, { query: { asset: 'sunshine', limit: 50, cursor: 'w1.sunshine.9.' + U1 } });
    const [url, init] = fetchImpl.mock.calls[0] as unknown as [string, RequestInit];
    expect(url).toBe(`/api/admin/v1/users/${U1}/wallet/ledger?asset=sunshine&limit=50&cursor=w1.sunshine.9.${U1}`);
    expect(init.method).toBe('GET');
    expect(init.credentials).toBe('same-origin');
    expect((init.headers as Record<string, string>)['X-CSRF-Token']).toBeUndefined();
  });

  it('accepts only canonical signed 64-bit decimal strings, never numbers', () => {
    for (const ok of ['0', '-1', '999999999', BIG, '9223372036854775807', '-9223372036854775808']) expect(isDecimal(ok)).toBe(true);
    for (const bad of ['9223372036854775808', '-9223372036854775809', '01', '-0', '1.5', '', ' 1', 1, 12n, null]) expect(isDecimal(bad)).toBe(false);
  });

  it('checks the subject: the summary must be the requested user, the page the requested user and asset', () => {
    expect(isSummary(summary(), U1)).toBe(true);
    expect(isSummary(summary(U2), U1)).toBe(false);
    expect(isSummary({ ...summary(), balances: [summary().balances[1], summary().balances[0]] }, U1)).toBe(false);
    expect(isSummary({ ...summary(), balances: [{ ...summary().balances[0], balance: 12 }, summary().balances[1]] }, U1)).toBe(false);
    const p = page(U1, 'flower', [entry('1', '5', '5')]);
    expect(isPage(p, U1, 'flower')).toBe(true);
    expect(isPage(p, U1, 'sunshine')).toBe(false);
    expect(isPage(p, U2, 'flower')).toBe(false);
  });
});

describe('admin wallet store', () => {
  it('keeps exact values beyond 2^53 and both balances', async () => {
    const store = useUserWalletStore();
    await store.select(U1);
    expect(store.summary?.balances[1].balance).toBe(BIG);
    expect(store.items.map((e) => e.amount)).toEqual(['-4', '6']);
    expect(api.ledger).toHaveBeenCalledWith(U1, 'flower', null, 50);
  });

  it('pages forward with the server cursor and back to the first page', async () => {
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry('3', '1', '3')], `w1.flower.3.${U1}`));
    const store = useUserWalletStore();
    await store.select(U1);
    expect(store.hasNext).toBe(true);
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry('2', '1', '2')], null));
    await store.next();
    expect(api.ledger).toHaveBeenLastCalledWith(U1, 'flower', `w1.flower.3.${U1}`, 50);
    expect(store.pageIndex).toBe(1);
    expect(store.hasNext).toBe(false);
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry('3', '1', '3')], `w1.flower.3.${U1}`));
    await store.prev();
    expect(api.ledger).toHaveBeenLastCalledWith(U1, 'flower', null, 50);
    expect(store.pageIndex).toBe(0);
  });

  it('switching asset restarts from the newest entry and drops a late answer for the old asset', async () => {
    const store = useUserWalletStore();
    await store.select(U1);
    const late = deferred<unknown>();
    api.ledger.mockReturnValueOnce(late.promise);
    const pending = store.next === undefined ? null : store.refresh();
    api.ledger.mockResolvedValueOnce(page(U1, 'sunshine', [entry(BIG, '100000', '7')]));
    await store.selectAsset('sunshine');
    late.resolve(page(U1, 'flower', [entry('9', '9', '9')]));
    await pending;
    await flushPromises();
    expect(store.asset).toBe('sunshine');
    expect(store.items.map((e) => e.seq)).toEqual([BIG]);
    expect(api.ledger).toHaveBeenLastCalledWith(U1, 'sunshine', null, 50);
  });

  it('a new user selection clears the old data and ignores the old user\'s late answers', async () => {
    const slowSummary = deferred<unknown>();
    const slowLedger = deferred<unknown>();
    api.summary.mockReturnValueOnce(slowSummary.promise);
    api.ledger.mockReturnValueOnce(slowLedger.promise);
    const store = useUserWalletStore();
    const first = store.select(U1);
    api.summary.mockResolvedValueOnce(summary(U2, '1', '2'));
    api.ledger.mockResolvedValueOnce(page(U2, 'flower', [entry('1', '1', '1')]));
    await store.select(U2);
    slowSummary.resolve(summary(U1));
    slowLedger.resolve(page(U1, 'flower', [entry('3', '-4', '12')]));
    await first;
    expect(store.userId).toBe(U2);
    expect(store.summary?.user.id).toBe(U2);
    expect(store.items.map((e) => e.seq)).toEqual(['1']);
  });

  it('logout or another admin clears everything and drops late answers', async () => {
    const store = useUserWalletStore();
    await store.select(U1);
    expect(store.summary).not.toBeNull();
    const late = deferred<unknown>();
    api.summary.mockReturnValueOnce(late.promise);
    api.ledger.mockReturnValueOnce(deferred<unknown>().promise);
    const pending = store.refresh();
    useAdminSessionStore().$patch({ authenticated: false, admin: null, epoch: useAdminSessionStore().epoch + 1 });
    expect(store.userId).toBeNull();
    expect(store.summary).toBeNull();
    expect(store.items).toEqual([]);
    late.resolve(summary(U1));
    await flushPromises();
    expect(store.summary).toBeNull();
    void pending;
    signIn('admin-2');
    expect(store.userId).toBeNull();
  });

  it('a lost authorization clears the shown data; a missing user and other failures are reported; retry is explicit', async () => {
    const store = useUserWalletStore();
    await store.select(U1);
    api.summary.mockRejectedValueOnce(new AdminApiError(401, 'UNAUTHENTICATED', 'x'));
    api.ledger.mockRejectedValueOnce(new AdminApiError(401, 'UNAUTHENTICATED', 'x'));
    await store.refresh();
    expect(store.summary).toBeNull();
    expect(store.items).toEqual([]);
    expect(store.summaryError).toContain('会话已失效');

    api.summary.mockRejectedValueOnce(new AdminApiError(404, 'NOT_FOUND', 'x'));
    api.ledger.mockRejectedValueOnce(new AdminApiError(404, 'NOT_FOUND', 'x'));
    await store.select(U2);
    expect(store.notFound).toBe(true);
    expect(store.summaryError).toBe('用户不存在。');

    api.summary.mockRejectedValueOnce(new AdminApiError(500, 'INTERNAL_ERROR', 'x'));
    await store.refresh();
    expect(store.summaryError).toContain('可点击重试');
    const calls = api.summary.mock.calls.length;
    await flushPromises();
    expect(api.summary.mock.calls.length).toBe(calls); // no automatic retry
    await store.refresh();
    expect(api.summary.mock.calls.length).toBe(calls + 1);
  });

  it('rejects a response for another subject or with rounded numbers', async () => {
    api.summary.mockResolvedValueOnce(summary(U2));
    api.ledger.mockResolvedValueOnce({ ...page(U1, 'flower', []), items: [{ ...entry('1', '1', '1'), amount: 1 }] });
    const store = useUserWalletStore();
    await store.select(U1);
    expect(store.summary).toBeNull();
    expect(store.summaryError).not.toBe('');
    expect(store.items).toEqual([]);
    expect(store.ledgerError).not.toBe('');
  });

  it('persists nothing', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem');
    const store = useUserWalletStore();
    await store.select(U1);
    await store.selectAsset('sunshine');
    expect(setItem).not.toHaveBeenCalled();
    setItem.mockRestore();
  });
});

describe('admin wallet view', () => {
  it('keeps balance/ledger tables read-only with exact amounts, excludes unrelated writes and resets read state on leave', async () => {
    route.params.userId = U1;
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry(BIG, '-100000', '0'), entry('2', '6', '100000')]));
    const wrapper = mount(View, { global: { stubs: { CursorPager: true } } });
    await flushPromises();
    expect(wrapper.find('[data-test="read-only"]').text()).toBe('只读');
    expect(wrapper.find('[data-test="balance-sunshine"]').text()).toContain(BIG);
    const rows = wrapper.findAll('[data-test="ledger-row"]');
    expect(rows[0].text()).toContain(BIG);
    expect(rows[0].text()).toContain('-100000');
    expect(rows[1].text()).toContain('+6');
    expect(wrapper.findAll('table a')).toHaveLength(0);
    for (const word of ['修正', '发放', '退款', '转账', '导出']) {
      expect(wrapper.findAll('button').some((b) => b.text().includes(word))).toBe(false);
    }
    wrapper.unmount();
    expect(useUserWalletStore().userId).toBeNull();
  });

  it('shows an empty state for a user without postings', async () => {
    route.params.userId = U2;
    api.summary.mockResolvedValueOnce({ ...summary(U2, '0', '0'), space: null,
      balances: summary(U2, '0', '0').balances.map((b) => ({ ...b, account_exists: false, latest_seq: '0' })) });
    api.ledger.mockResolvedValueOnce({ ...page(U2, 'flower', []), account_exists: false });
    const wrapper = mount(View, { global: { stubs: { CursorPager: true } } });
    await flushPromises();
    expect(wrapper.find('[data-test="ledger-empty"]').exists()).toBe(true);
    expect(wrapper.text()).toContain('未加入空间');
    expect(wrapper.text()).toContain('尚无账户');
    wrapper.unmount();
  });
});

describe('admin wallet races (correction 2026-11-11)', () => {
  const AUTH = '会话已失效';

  it('summary 401/403 first: a delayed ledger 200 must not repopulate the page; both loaders settle', async () => {
    for (const status of [401, 403]) {
      setActivePinia(createPinia());
      signIn();
      const s = deferred<unknown>();
      const l = deferred<unknown>();
      api.summary.mockReturnValueOnce(s.promise);
      api.ledger.mockReturnValueOnce(l.promise);
      const store = useUserWalletStore();
      const opened = store.select(U1);
      const summaryCalls = api.summary.mock.calls.length;
      s.reject(new AdminApiError(status, 'UNAUTHENTICATED', 'x'));
      await flushPromises();
      expect(store.summary).toBeNull();
      expect(store.summaryError).toContain(AUTH);
      expect(store.summaryLoading).toBe(false);
      l.resolve(page(U1, 'flower', [entry('3', '-4', '12')]));
      await opened;
      await flushPromises();
      expect(store.items).toEqual([]);
      expect(store.pageInfo).toBeNull();
      expect(store.ledgerLoading).toBe(false);
      expect(store.summaryError).toContain(AUTH);
      expect(api.summary.mock.calls.length).toBe(summaryCalls); // no automatic retry
    }
  });

  it('ledger 401/403 first: a delayed summary 200 must not repopulate the page; both loaders settle', async () => {
    for (const status of [401, 403]) {
      setActivePinia(createPinia());
      signIn();
      const s = deferred<unknown>();
      const l = deferred<unknown>();
      api.summary.mockReturnValueOnce(s.promise);
      api.ledger.mockReturnValueOnce(l.promise);
      const store = useUserWalletStore();
      const opened = store.select(U1);
      l.reject(new AdminApiError(status, 'FORBIDDEN', 'x'));
      await flushPromises();
      expect(store.items).toEqual([]);
      expect(store.ledgerError).toContain(AUTH);
      expect(store.ledgerLoading).toBe(false);
      s.resolve(summary(U1));
      await opened;
      await flushPromises();
      expect(store.summary).toBeNull();
      expect(store.summaryLoading).toBe(false);
      expect(store.ledgerError).toContain(AUTH);
    }
  });

  it('data shown before a later authorization loss is cleared, and stale answers from before and after cannot bring it back', async () => {
    const store = useUserWalletStore();
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry('3', '1', '3')], `w1.flower.3.${U1}`));
    await store.select(U1);
    expect(store.summary).not.toBeNull();
    // An older page request still in flight, then a refresh whose summary loses authorization.
    const oldPage = deferred<unknown>();
    api.ledger.mockReturnValueOnce(oldPage.promise);
    const paging = store.next();
    const s = deferred<unknown>();
    const l = deferred<unknown>();
    api.summary.mockReturnValueOnce(s.promise);
    api.ledger.mockReturnValueOnce(l.promise);
    const refreshing = store.refresh();
    s.reject(new AdminApiError(401, 'UNAUTHENTICATED', 'x'));
    await flushPromises();
    expect(store.summary).toBeNull();
    expect(store.items).toEqual([]);
    l.resolve(page(U1, 'flower', [entry('3', '1', '3')]));
    oldPage.resolve(page(U1, 'flower', [entry('2', '1', '2')]));
    await Promise.all([paging, refreshing]);
    await flushPromises();
    expect(store.summary).toBeNull();
    expect(store.items).toEqual([]);
    expect(store.pageInfo).toBeNull();
    expect(store.summaryLoading).toBe(false);
    expect(store.ledgerLoading).toBe(false);
    expect(store.summaryError).toContain(AUTH);
    // Recovery only by an explicit click.
    await store.refresh();
    expect(store.summary?.user.id).toBe(U1);
  });

  it('switching asset while the same user\'s summary is pending keeps that summary valid; loading ends and refresh is usable', async () => {
    route.params.userId = U1;
    const slow = deferred<unknown>();
    api.summary.mockReturnValueOnce(slow.promise);
    api.ledger.mockResolvedValueOnce(page(U1, 'flower', [entry('3', '-4', '12')]));
    const wrapper = mount(View, { global: { stubs: { CursorPager: true } } });
    await flushPromises();
    const store = useUserWalletStore();
    expect(store.summaryLoading).toBe(true);
    expect(store.ledgerLoading).toBe(false);
    api.ledger.mockResolvedValueOnce(page(U1, 'sunshine', [entry('7', '100000', BIG)]));
    await wrapper.find('[data-test="asset-sunshine"]').trigger('click');
    await flushPromises();
    expect(store.asset).toBe('sunshine');
    slow.resolve(summary(U1));
    await flushPromises();
    expect(store.summary?.user.id).toBe(U1);
    expect(store.summaryLoading).toBe(false);
    expect(wrapper.find('[data-test="balance-sunshine"]').text()).toContain(BIG);
    expect((wrapper.find('[data-test="refresh"]').element as HTMLButtonElement).disabled).toBe(false);
    // Old-asset ledger answers stay ignored.
    const late = deferred<unknown>();
    api.ledger.mockReturnValueOnce(late.promise);
    await wrapper.find('[data-test="asset-flower"]').trigger('click');
    // The view disables asset buttons while a ledger read is pending; the store must still ignore the old asset's answer.
    expect((wrapper.find('[data-test="asset-sunshine"]').element as HTMLButtonElement).disabled).toBe(true);
    api.ledger.mockResolvedValueOnce(page(U1, 'sunshine', [entry('8', '1', '1')]));
    await store.selectAsset('sunshine');
    late.resolve(page(U1, 'flower', [entry('9', '9', '9')]));
    await flushPromises();
    expect(store.asset).toBe('sunshine');
    expect(store.items.map((e) => e.seq)).toEqual(['8']);
    wrapper.unmount();
  });

  it('a user change and unmount still invalidate a pending summary', async () => {
    const slow = deferred<unknown>();
    api.summary.mockReturnValueOnce(slow.promise);
    const store = useUserWalletStore();
    const first = store.select(U1);
    api.summary.mockResolvedValueOnce(summary(U2, '1', '2'));
    api.ledger.mockResolvedValueOnce(page(U2, 'flower', []));
    await store.select(U2);
    slow.resolve(summary(U1));
    await first;
    expect(store.summary?.user.id).toBe(U2);
    const pending = deferred<unknown>();
    api.summary.mockReturnValueOnce(pending.promise);
    const again = store.refresh();
    store.reset();
    pending.resolve(summary(U2));
    await again;
    expect(store.summary).toBeNull();
    expect(store.summaryLoading).toBe(false);
  });
});
