/**
 * Read-only admin task inspection (docs/admin-task-inspection.md): the API paths and response validation, the store's scoping
 * by admin session, couple, filter and task with separate list and detail identities, late answers, authorization-loss
 * interleavings, loading-state settlement and no persistence, and the read-only view. fetch is stubbed or the API module is
 * mocked; nothing reaches a network.
 */
import { beforeEach, afterEach, describe, it, expect, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { flushPromises, mount } from '@vue/test-utils';
import { nextTick, reactive } from 'vue';

const api = vi.hoisted(() => ({ list: vi.fn(), detail: vi.fn() }));
vi.mock('../../src/api/tasks', async (original) => ({ ...(await original<typeof import('../../src/api/tasks')>()), taskAdminApi: api }));
const routeHolder = vi.hoisted(() => ({ current: { params: {} as Record<string, string> } }));
vi.mock('vue-router', async () => {
  const { defineComponent, h } = await import('vue');
  return {
    useRoute: () => routeHolder.current,
    RouterLink: defineComponent({
      props: { to: { type: Object, required: true } },
      setup(props, { slots }) {
        return () => h('a', { 'data-to': JSON.stringify(props.to) }, slots.default?.());
      },
    }),
  };
});

import { AdminApiError, AdminHttpClient } from '../../src/api/client';
import { isTaskDetail, isTaskPage, type KindFilter } from '../../src/api/tasks';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import { useCoupleTasksStore } from '../../src/stores/couple-tasks';
import View from '../../src/views/CoupleTasksView.vue';

const C1 = '11111111-1111-4111-8111-111111111111';
const C2 = '22222222-2222-4222-8222-222222222222';
const T1 = 'aaaaaaaa-0000-4000-8000-000000000001';
const T2 = 'aaaaaaaa-0000-4000-8000-000000000002';
const T3 = 'aaaaaaaa-0000-4000-8000-000000000003';
const UA = 'bbbbbbbb-0000-4000-8000-00000000000a';
const UB = 'bbbbbbbb-0000-4000-8000-00000000000b';
const P1 = 'cccccccc-0000-4000-8000-000000000001';
const NOW = '2026-11-11T08:00:00.000000Z';
const AUTH = '会话已失效';

const row = (id: string, over: Record<string, unknown> = {}) => ({
  id, kind: 'task', schedule_type: 'one_off', template_id: null, period_date: null, title: `T ${id.slice(-1)}`,
  creator_id: UA, assignee_id: UB, reward_flowers: '5', reward_sunshine: '100000', completion_mode: 'single', target_count: 1,
  progress_count: 0, status: 'open', start_date: null, deadline_at: null, created_at: NOW, completed_at: null, ...over,
});
const page = (couple: string, items: ReturnType<typeof row>[], next: string | null = null, kind: string | null = null) => ({
  space: { id: couple, status: 'active' }, kind, items, next_cursor: next, read_at: NOW,
});
const detail = (couple: string, task: string, over: Record<string, unknown> = {}) => ({
  space: { id: couple, status: 'active' },
  task: { ...row(task), description: '', timezone: 'Asia/Shanghai', deadline_local_date: null, deadline_local_time: null },
  completion: null,
  progress: { total: 0, items: [] },
  reaction: null,
  changes: { owner_task_id: task, total: 0, items: [] },
  closure: null,
  replaces: null,
  read_at: NOW,
  ...over,
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
  routeHolder.current = reactive({ params: {} as Record<string, string> });
  api.list.mockImplementation(async (couple: string, kind: KindFilter) => page(couple, [row(T1), row(T2)], null, kind || null));
  api.detail.mockImplementation(async (couple: string, task: string) => detail(couple, task));
});
afterEach(() => vi.unstubAllGlobals());

describe('admin task API and validation', () => {
  it('reads with GET on the admin base, encodes ids, sends kind only when filtered, and carries no CSRF', async () => {
    const fetchImpl = vi.fn(async () => new Response('{}', { status: 200, headers: { 'Content-Type': 'application/json' } }));
    const http = new AdminHttpClient({ baseUrl: '/api/admin/v1', fetchImpl });
    await http.request(`/couples/${encodeURIComponent(C1)}/tasks`, { query: { kind: undefined, limit: 50, cursor: 'c' } });
    await http.request(`/couples/${encodeURIComponent(C1)}/tasks/${encodeURIComponent(T1)}`, {});
    const [url, init] = fetchImpl.mock.calls[0] as unknown as [string, RequestInit];
    expect(url).toBe(`/api/admin/v1/couples/${C1}/tasks?limit=50&cursor=c`);
    expect(init.method).toBe('GET');
    expect((init.headers as Record<string, string>)['X-CSRF-Token']).toBeUndefined();
    expect((fetchImpl.mock.calls[1] as unknown as [string])[0]).toBe(`/api/admin/v1/couples/${C1}/tasks/${T1}`);
  });

  it('accepts only well-formed pages and details for the selected couple, filter and task', () => {
    expect(isTaskPage(page(C1, [row(T1)]), C1, '')).toBe(true);
    expect(isTaskPage(page(C1, [row(T1)]), C2, '')).toBe(false);                                     // another couple
    expect(isTaskPage(page(C1, [row(T1)], null, 'task'), C1, '')).toBe(false);                        // another filter
    expect(isTaskPage(page(C1, [row(T1)], null, 'period'), C1, 'period')).toBe(false);                // a row of another kind
    expect(isTaskPage(page(C1, [row(T1, { reward_flowers: 5 })]), C1, '')).toBe(false);               // numbers, never
    expect(isTaskPage(page(C1, [row(T1, { reward_sunshine: '-1' })]), C1, '')).toBe(false);
    expect(isTaskPage(page(C1, [row(T1, { status: 'deleted' })]), C1, '')).toBe(false);
    expect(isTaskPage(page(C1, Array.from({ length: 101 }, () => row(T1))), C1, '')).toBe(false);
    expect(isTaskDetail(detail(C1, T1), C1, T1)).toBe(true);
    expect(isTaskDetail(detail(C1, T1), C1, T2)).toBe(false);
    expect(isTaskDetail(detail(C1, T1), C2, T1)).toBe(false);
    expect(isTaskDetail(detail(C1, T1, { completion: { completed_by: UB, completed_at: NOW, reward_flowers: 5, reward_sunshine: '1' } }), C1, T1)).toBe(false);
    expect(isTaskDetail(detail(C1, T1, { changes: { owner_task_id: T1, total: 1, items: [{ id: P1, kind: 'cancel', proposer_id: UA, created_at: NOW, terms: { title: 'x' }, decision: null }] } }), C1, T1)).toBe(false);
    expect(isTaskDetail(detail(C1, T1, { changes: { owner_task_id: T1, total: 1, items: [{ id: P1, kind: 'cancel', proposer_id: UA, created_at: NOW, terms: null, decision: { outcome: 'approved', decided_by: UB, decided_at: NOW, successor_task_id: null } }] } }), C1, T1)).toBe(true);
  });
});

describe('admin task store', () => {
  it('pages forward and back with the server cursors, and a filter restarts from the newest row with its own kind', async () => {
    const store = useCoupleTasksStore();
    api.list.mockResolvedValueOnce(page(C1, [row(T1)], 'cur-2'));
    await store.select(C1);
    expect(store.items.map((t) => t.id)).toEqual([T1]);
    expect(store.hasNext).toBe(true);
    api.list.mockResolvedValueOnce(page(C1, [row(T2)], null));
    await store.next();
    expect(api.list).toHaveBeenLastCalledWith(C1, '', 'cur-2', 50);
    expect(store.items.map((t) => t.id)).toEqual([T2]);
    expect(store.hasPrev).toBe(true);
    api.list.mockResolvedValueOnce(page(C1, [row(T1)], 'cur-2'));
    await store.prev();
    expect(api.list).toHaveBeenLastCalledWith(C1, '', null, 50);
    api.list.mockResolvedValueOnce(page(C1, [row(T3, { kind: 'period', template_id: T1, period_date: '2026-11-11' })], null, 'period'));
    await store.selectKind('period');
    expect(api.list).toHaveBeenLastCalledWith(C1, 'period', null, 50);
    expect(store.items.map((t) => t.id)).toEqual([T3]);
    expect(store.pageIndex).toBe(0);
  });

  it('drops a late list answer after a couple change, a filter change or a reset', async () => {
    const store = useCoupleTasksStore();
    const slow = deferred<unknown>();
    api.list.mockReturnValueOnce(slow.promise);
    const first = store.select(C1);
    await store.select(C2);
    slow.resolve(page(C1, [row(T3)]));
    await first;
    expect(store.coupleId).toBe(C2);
    expect(store.items.map((t) => t.id)).toEqual([T1, T2]);
    const old = deferred<unknown>();
    api.list.mockReturnValueOnce(old.promise);
    const refreshing = store.refreshList();
    await store.selectKind('task');
    old.resolve(page(C2, [row(T3)]));
    await refreshing;
    expect(store.kind).toBe('task');
    expect(store.items.map((t) => t.id)).toEqual([T1, T2]);
    expect(store.listLoading).toBe(false);
    const pending = deferred<unknown>();
    api.list.mockReturnValueOnce(pending.promise);
    const again = store.refreshList();
    store.reset();
    pending.resolve(page(C2, [row(T3)], null, 'task'));
    await again;
    expect(store.items).toEqual([]);
    expect(store.listLoading).toBe(false);
  });

  it('opening another task drops the earlier detail; the latest selection wins', async () => {
    const store = useCoupleTasksStore();
    await store.select(C1);
    const a = deferred<unknown>();
    api.detail.mockReturnValueOnce(a.promise);
    const openA = store.openTask(T1);
    expect(store.detailLoading).toBe(true);
    await store.openTask(T2);
    a.resolve(detail(C1, T1));
    await openA;
    expect(store.detail?.task.id).toBe(T2);
    expect(store.detailLoading).toBe(false);
    await store.openTask(null);
    expect(store.detail).toBeNull();
    expect(store.taskId).toBeNull();
  });

  it('a filter change does not strand a pending detail, and a task selection does not strand a pending list', async () => {
    const store = useCoupleTasksStore();
    await store.select(C1);
    const slowDetail = deferred<unknown>();
    api.detail.mockReturnValueOnce(slowDetail.promise);
    const opening = store.openTask(T1);
    await store.selectKind('template');
    slowDetail.resolve(detail(C1, T1));
    await opening;
    expect(store.detail?.task.id).toBe(T1);
    expect(store.detailLoading).toBe(false);
    const slowList = deferred<unknown>();
    api.list.mockReturnValueOnce(slowList.promise);
    const listing = store.refreshList();
    await store.openTask(T2);
    expect(store.listLoading).toBe(true);
    slowList.resolve(page(C1, [], null, 'template'));
    await listing;
    expect(store.listLoading).toBe(false);
    expect(store.isEmpty).toBe(true);
    expect(store.detail?.task.id).toBe(T2);
  });

  it('list 401/403 first: a delayed detail 200 must not repopulate; both loaders settle; no retry', async () => {
    for (const status of [401, 403]) {
      setActivePinia(createPinia());
      signIn();
      const store = useCoupleTasksStore();
      await store.select(C1);
      const l = deferred<unknown>();
      const d = deferred<unknown>();
      api.list.mockReturnValueOnce(l.promise);
      api.detail.mockReturnValueOnce(d.promise);
      const listing = store.refreshList();
      const opening = store.openTask(T1);
      l.reject(new AdminApiError(status, 'UNAUTHENTICATED', 'x'));
      await flushPromises();
      expect(store.items).toEqual([]);
      expect(store.listError).toContain(AUTH);
      expect(store.listLoading).toBe(false);
      expect(store.detailLoading).toBe(false);
      d.resolve(detail(C1, T1));
      await Promise.all([listing, opening]);
      await flushPromises();
      expect(store.detail).toBeNull();
      expect(store.space).toBeNull();
      expect(store.detailError).toContain(AUTH);
    }
    expect(api.list).toHaveBeenCalledTimes(4); // two per round: select and the failed refresh; no automatic retry
  });

  it('detail 401/403 first: a delayed list 200 must not repopulate; both loaders settle', async () => {
    for (const status of [401, 403]) {
      setActivePinia(createPinia());
      signIn();
      const store = useCoupleTasksStore();
      const l = deferred<unknown>();
      const d = deferred<unknown>();
      api.list.mockReturnValueOnce(l.promise);
      api.detail.mockReturnValueOnce(d.promise);
      const listing = store.select(C1);
      const opening = store.openTask(T1);
      d.reject(new AdminApiError(status, 'FORBIDDEN', 'x'));
      await flushPromises();
      expect(store.detailError).toContain(AUTH);
      expect(store.detailLoading).toBe(false);
      expect(store.listLoading).toBe(false);
      l.resolve(page(C1, [row(T1)]));
      await Promise.all([listing, opening]);
      await flushPromises();
      expect(store.items).toEqual([]);
      expect(store.space).toBeNull();
      expect(store.listError).toContain(AUTH);
    }
  });

  it('data shown before an authorization loss is cleared; older page and detail answers cannot bring it back; a click recovers', async () => {
    const store = useCoupleTasksStore();
    api.list.mockResolvedValueOnce(page(C1, [row(T1)], 'cur-2'));
    await store.select(C1);
    await store.openTask(T1);
    expect(store.detail).not.toBeNull();
    const oldPage = deferred<unknown>();
    api.list.mockReturnValueOnce(oldPage.promise);
    const paging = store.next();
    const oldDetail = deferred<unknown>();
    api.detail.mockReturnValueOnce(oldDetail.promise);
    const reopening = store.refreshDetail();
    const fresh = deferred<unknown>();
    api.list.mockReturnValueOnce(fresh.promise);
    const refreshing = store.refreshList();
    fresh.reject(new AdminApiError(401, 'UNAUTHENTICATED', 'x'));
    await flushPromises();
    oldPage.resolve(page(C1, [row(T2)]));
    oldDetail.resolve(detail(C1, T1));
    await Promise.all([paging, reopening, refreshing]);
    await flushPromises();
    expect(store.items).toEqual([]);
    expect(store.detail).toBeNull();
    expect(store.listLoading).toBe(false);
    expect(store.detailLoading).toBe(false);
    await store.refreshList();
    await store.refreshDetail();
    expect(store.items.map((t) => t.id)).toEqual([T1, T2]);
    expect(store.detail?.task.id).toBe(T1);
  });

  it('logout and another admin clear everything and drop pending reads', async () => {
    const store = useCoupleTasksStore();
    await store.select(C1);
    const l = deferred<unknown>();
    const d = deferred<unknown>();
    api.list.mockReturnValueOnce(l.promise);
    api.detail.mockReturnValueOnce(d.promise);
    const listing = store.refreshList();
    const opening = store.openTask(T1);
    signIn('admin-2');
    expect(store.coupleId).toBeNull();
    expect(store.items).toEqual([]);
    expect(store.listLoading).toBe(false);
    expect(store.detailLoading).toBe(false);
    l.resolve(page(C1, [row(T3)]));
    d.resolve(detail(C1, T1));
    await Promise.all([listing, opening]);
    expect(store.items).toEqual([]);
    expect(store.detail).toBeNull();
    useAdminSessionStore().$patch({ authenticated: false, admin: null });
    await store.select(C1);
    expect(api.list).toHaveBeenCalledTimes(2); // signed out: no read at all
  });

  it('distinguishes an unknown space, an empty space and a task outside the space; nothing is persisted', async () => {
    const setItem = vi.spyOn(Storage.prototype, 'setItem');
    const store = useCoupleTasksStore();
    api.list.mockRejectedValueOnce(new AdminApiError(404, 'SPACE_NOT_FOUND', 'x'));
    await store.select(C1);
    expect(store.spaceNotFound).toBe(true);
    expect(store.isEmpty).toBe(false);
    api.list.mockResolvedValueOnce(page(C2, []));
    await store.select(C2);
    expect(store.spaceNotFound).toBe(false);
    expect(store.isEmpty).toBe(true);
    api.detail.mockRejectedValueOnce(new AdminApiError(404, 'TASK_NOT_FOUND', 'x'));
    await store.openTask(T3);
    expect(store.taskNotFound).toBe(true);
    expect(store.detailError).toContain('没有这个任务');
    expect(store.space?.id).toBe(C2); // the list stays
    api.detail.mockRejectedValueOnce(new AdminApiError(500, 'INTERNAL_ERROR', 'x'));
    await store.refreshDetail();
    expect(store.taskNotFound).toBe(false);
    expect(store.detailError).toContain('重试');
    expect(setItem).not.toHaveBeenCalled();
    setItem.mockRestore();
  });
});

describe('admin task view', () => {
  it('is read-only, shows exact decimal strings and the full history, and opens a task from the route', async () => {
    routeHolder.current.params = { coupleId: C1.toUpperCase() };
    api.list.mockResolvedValueOnce(page(C1, [row(T1, { reward_sunshine: '9007199254740993' }), row(T2, { status: 'superseded' })]));
    const wrapper = mount(View, { global: { stubs: { CursorPager: true } } });
    await flushPromises();
    expect(api.list).toHaveBeenCalledWith(C1, '', null, 50);
    expect(wrapper.find('[data-test="read-only"]').exists()).toBe(true);
    expect(wrapper.findAll('[data-test="task-row"]')).toHaveLength(2);
    expect(wrapper.text()).toContain('9007199254740993');
    expect(wrapper.find('[data-test="detail"]').exists()).toBe(false);
    api.detail.mockResolvedValueOnce(detail(C1, T2, {
      task: { ...row(T2, { status: 'superseded', reward_flowers: '8' }), description: 'line', timezone: 'Asia/Shanghai', deadline_local_date: null, deadline_local_time: null },
      changes: { owner_task_id: T2, total: 2, items: [
        { id: P1, kind: 'edit', proposer_id: UA, created_at: NOW, terms: { title: 'New', description: '', reward_flowers: '8', reward_sunshine: '0', completion_mode: 'single', target_count: 1, deadline_local_date: null, deadline_local_time: null, start_date: null }, decision: { outcome: 'approved', decided_by: UB, decided_at: NOW, successor_task_id: T3 } },
        { id: P1.replace('1', '2'), kind: 'cancel', proposer_id: UA, created_at: NOW, terms: null, decision: { outcome: 'rejected', decided_by: UB, decided_at: NOW, successor_task_id: null } },
      ] },
      closure: { owner_task_id: T2, reason: 'superseded', closed_at: NOW, proposal_id: P1, successor_task_id: T3, applies_to_this_task: true },
      progress: { total: 250, items: [{ id: P1, user_id: UB, progress_count: 250, target_count: 300, recorded_at: NOW, completes_task: false }] },
    }));
    routeHolder.current.params = { coupleId: C1, taskId: T2 };
    await nextTick();
    await flushPromises();
    expect(api.detail).toHaveBeenCalledWith(C1, T2);
    expect(api.list).toHaveBeenCalledTimes(1); // the list stays mounted
    expect(wrapper.find('[data-test="detail-status"]').text()).toBe('已被修改取代');
    expect(wrapper.find('[data-test="detail-reward"]').text()).toContain('小红花 8');
    expect(wrapper.findAll('[data-test="proposal-row"]')).toHaveLength(2);
    expect(wrapper.find('[data-test="progress-truncated"]').text()).toContain('250');
    expect(wrapper.find('[data-test="link-successor"]').attributes('data-to')).toContain(T3);
    expect(wrapper.find('[data-test="link-closure-successor"]').attributes('data-to')).toContain(C1);
    // Only filter, refresh and pager controls: no mutation button, enabled or disabled.
    const tests = wrapper.findAll('button').map((b) => b.attributes('data-test') ?? '');
    for (const t of tests) expect(/^(kind-(all|task|template|period)|refresh-list|refresh-detail)$/.test(t)).toBe(true);
    wrapper.unmount();
    expect(useCoupleTasksStore().coupleId).toBeNull();
  });

  it('names the template as the owner of a period\'s changes and links it', async () => {
    routeHolder.current.params = { coupleId: C1, taskId: T3 };
    api.detail.mockResolvedValueOnce(detail(C1, T3, {
      task: { ...row(T3, { kind: 'period', schedule_type: 'daily', template_id: T1, period_date: '2026-11-11', completion_mode: 'count', target_count: 2, progress_count: 1 }), description: '', timezone: 'Asia/Shanghai', deadline_local_date: '2026-11-11', deadline_local_time: null },
      changes: { owner_task_id: T1, total: 0, items: [] },
    }));
    const wrapper = mount(View, { global: { stubs: { CursorPager: true } } });
    await flushPromises();
    expect(wrapper.find('[data-test="changes-owner"]').exists()).toBe(true);
    expect(wrapper.find('[data-test="link-template"]').attributes('data-to')).toContain(T1);
    wrapper.unmount();
  });
});
