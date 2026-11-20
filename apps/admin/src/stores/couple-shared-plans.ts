/**
 * Read-only shared-plan inspection of one selected couple/space (docs/admin-shared-plan-inspection.md). Memory only.
 * Identities:
 * - a list page belongs to the admin session (epoch and admin id), the selected couple and the list read;
 * - a detail belongs to the admin session, the selected couple and the selected redemption.
 * Changing the filter invalidates only list reads; selecting another redemption invalidates only detail reads, so neither strands
 * the other's loading indicator. A couple, session or unmount change invalidates both.
 * A 401/403 from any current read clears the private data, settles both loaders and invalidates every other read in flight,
 * so no delayed 200 can repopulate the page. No retry without a click.
 */
import { defineStore } from 'pinia';
import { computed, ref, watch } from 'vue';

import { AdminApiError } from '@/api/client';
import { isSharedPlanDetail, isSharedPlanPage, sharedPlanAdminApi as api, type SharedPlanDetail, type SharedPlanSpace, type SharedPlanSummary } from '@/api/shared-plans';
import { useAdminSessionStore } from './admin-session';

export const PAGE_SIZE = 50;

export const useCoupleSharedPlansStore = defineStore('couple-shared-plans', () => {
  const session = useAdminSessionStore();
  const coupleId = ref<string | null>(null);
  const space = ref<SharedPlanSpace | null>(null);
  const items = ref<SharedPlanSummary[]>([]);
  const readAt = ref<string | null>(null);
  const listLoading = ref(false);
  const listError = ref('');
  const spaceNotFound = ref(false);
  // cursors[i] loaded page i (null for the first page); nextCursor is the server's cursor for page i + 1.
  const cursors = ref<Array<string | null>>([null]);
  const pageIndex = ref(0);
  const nextCursor = ref<string | null>(null);
  const planId = ref<string | null>(null);
  const detail = ref<SharedPlanDetail | null>(null);
  const detailLoading = ref(false);
  const detailError = ref('');
  const planNotFound = ref(false);
  // coupleGeneration: a couple, session, unmount or authorization change (invalidates list and detail reads).
  // listGeneration: additionally a filter change (list only). detailGeneration: additionally a task change (detail only).
  let coupleGeneration = 0;
  let listGeneration = 0;
  let detailGeneration = 0;
  let listSeq = 0;
  let detailSeq = 0;

  const owner = () => (session.authenticated ? `${session.epoch}:${session.admin?.id ?? ''}` : '');

  function clearList(): void {
    space.value = null;
    items.value = [];
    readAt.value = null;
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
  }
  function clearDetail(): void {
    detail.value = null;
  }
  function reset(): void {
    coupleGeneration++;
    listGeneration++;
    detailGeneration++;
    listSeq++;
    detailSeq++;
    coupleId.value = null;
    planId.value = null;
    clearList();
    clearDetail();
    listLoading.value = detailLoading.value = false;
    listError.value = detailError.value = '';
    spaceNotFound.value = planNotFound.value = false;
  }
  // Logout, a new login or another admin: forget everything at once.
  watch(owner, () => reset(), { flush: 'sync' });

  type BaseToken = { owner: string; couple: string; coupleGeneration: number };
  type ListToken = BaseToken & { listGeneration: number };
  type DetailToken = BaseToken & { task: string; detailGeneration: number };
  const baseToken = (): BaseToken => ({ owner: owner(), couple: coupleId.value ?? '', coupleGeneration });
  const baseCurrent = (t: BaseToken) =>
    !!t.owner && t.owner === owner() && t.couple === (coupleId.value ?? '') && t.coupleGeneration === coupleGeneration;
  const listCurrent = (t: ListToken) => baseCurrent(t) && t.listGeneration === listGeneration;
  const detailCurrent = (t: DetailToken) => baseCurrent(t) && t.task === (planId.value ?? '') && t.detailGeneration === detailGeneration;

  const AUTH_LOST = '管理员会话已失效或无权访问，已清除显示的数据。请重新登录后再读取。';
  const isAuthLoss = (error: unknown) => error instanceof AdminApiError && (error.status === 401 || error.status === 403);
  // A current read lost authorization: clear the private data, settle both loaders, and make every read still in flight
  // (list or detail, any filter or task) unusable. Recovery is only an explicit refresh or a new selection.
  function authorizationLost(): void {
    coupleGeneration++;
    listGeneration++;
    detailGeneration++;
    listSeq++;
    detailSeq++;
    clearList();
    clearDetail();
    listLoading.value = detailLoading.value = false;
    listError.value = detailError.value = AUTH_LOST;
  }
  function failure(error: unknown, what: string): string {
    if (error instanceof AdminApiError && error.status === 400) return `${what}请求无效（${error.code}）。`;
    return `${what}读取失败，可点击重试。`;
  }

  async function loadPage(index: number, cursor: string | null): Promise<void> {
    const couple = coupleId.value;
    if (!owner() || !couple) return;
    const t: ListToken = { ...baseToken(), listGeneration };
    const s = ++listSeq;
    listLoading.value = true;
    listError.value = '';
    try {
      const r = await api.list(couple, cursor, PAGE_SIZE);
      if (!listCurrent(t) || s !== listSeq) return;
      if (!isSharedPlanPage(r, couple)) throw new Error('shape');
      space.value = r.space;
      items.value = r.items;
      readAt.value = r.read_at;
      pageIndex.value = index;
      cursors.value = [...cursors.value.slice(0, index), cursor];
      nextCursor.value = r.next_cursor;
    } catch (error) {
      if (!listCurrent(t) || s !== listSeq) return;
      if (isAuthLoss(error)) { authorizationLost(); return; }
      clearList();
      if (error instanceof AdminApiError && error.status === 404) {
        spaceNotFound.value = true;
        listError.value = '该空间不存在。';
      } else {
        listError.value = failure(error, '计划列表');
      }
    } finally {
      if (listCurrent(t) && s === listSeq) listLoading.value = false;
    }
  }

  async function loadDetail(): Promise<void> {
    const couple = coupleId.value;
    const task = planId.value;
    if (!owner() || !couple || !task) return;
    const t: DetailToken = { ...baseToken(), task, detailGeneration };
    const s = ++detailSeq;
    detailLoading.value = true;
    detailError.value = '';
    planNotFound.value = false;
    try {
      const r = await api.detail(couple, task);
      if (!detailCurrent(t) || s !== detailSeq) return;
      if (!isSharedPlanDetail(r, couple, task)) throw new Error('shape');
      detail.value = r;
    } catch (error) {
      if (!detailCurrent(t) || s !== detailSeq) return;
      if (isAuthLoss(error)) { authorizationLost(); return; }
      detail.value = null;
      if (error instanceof AdminApiError && error.status === 404) {
        if (error.code === 'SPACE_NOT_FOUND') {
          spaceNotFound.value = true;
          detailError.value = '该空间不存在。';
        } else {
          planNotFound.value = true;
          detailError.value = '该空间中没有这个计划。';
        }
      } else {
        detailError.value = failure(error, '计划详情');
      }
    } finally {
      if (detailCurrent(t) && s === detailSeq) detailLoading.value = false;
    }
  }

  /** Open a couple: clears everything shown for the previous one and reads its first page. */
  async function select(id: string): Promise<void> {
    if (!owner()) return;
    if (id === coupleId.value && (space.value || listLoading.value)) return;
    reset();
    coupleId.value = id;
    await loadPage(0, null);
  }
  /** Open one task of the selected couple (null closes the detail); the list is untouched. */
  async function openPlan(id: string | null): Promise<void> {
    if (!owner() || !coupleId.value) return;
    if (id === planId.value && (detail.value || detailLoading.value)) return;
    detailGeneration++;
    planId.value = id;
    clearDetail();
    detailError.value = '';
    planNotFound.value = false;
    detailLoading.value = false;
    if (id) await loadDetail();
  }
  async function next(): Promise<void> {
    if (!nextCursor.value || listLoading.value) return;
    await loadPage(pageIndex.value + 1, nextCursor.value);
  }
  async function prev(): Promise<void> {
    if (pageIndex.value === 0 || listLoading.value) return;
    await loadPage(pageIndex.value - 1, cursors.value[pageIndex.value - 1] ?? null);
  }
  /** Explicit refresh or retry of the list, from the newest row. */
  async function refreshList(): Promise<void> {
    if (!owner() || !coupleId.value) return;
    listGeneration++;
    cursors.value = [null];
    pageIndex.value = 0;
    nextCursor.value = null;
    await loadPage(0, null);
  }
  /** Explicit refresh or retry of the open detail. */
  async function refreshDetail(): Promise<void> {
    if (!owner() || !coupleId.value || !planId.value) return;
    detailGeneration++;
    await loadDetail();
  }

  const hasPrev = computed(() => pageIndex.value > 0);
  const hasNext = computed(() => nextCursor.value !== null);
  const isEmpty = computed(() => !listLoading.value && !listError.value && space.value !== null && items.value.length === 0);

  return {
    coupleId, space, items, readAt, listLoading, listError, spaceNotFound, pageIndex, planId, detail, detailLoading,
    detailError, planNotFound, hasPrev, hasNext, isEmpty, select, openPlan, next, prev, refreshList, refreshDetail,
    reset,
  };
});
