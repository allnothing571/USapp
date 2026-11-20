import { beforeEach, afterEach, describe, it, expect, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { flushPromises, mount } from '@vue/test-utils';
import { reactive } from 'vue';

const api = vi.hoisted(() => ({ list: vi.fn(), detail: vi.fn() }));
vi.mock('../../src/api/shared-plans', async (original) => ({ ...(await original<typeof import('../../src/api/shared-plans')>()), sharedPlanAdminApi: api }));
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

import { AdminApiError } from '../../src/api/client';
import { isSharedPlanDetail, isSharedPlanPage } from '../../src/api/shared-plans';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import { useCoupleSharedPlansStore } from '../../src/stores/couple-shared-plans';
import View from '../../src/views/CoupleSharedPlansView.vue';

const C1 = '11111111-1111-4111-8111-111111111111';
const C2 = '22222222-2222-4222-8222-222222222222';
const T1 = 'aaaaaaaa-0000-4000-8000-000000000001';
const T2 = 'aaaaaaaa-0000-4000-8000-000000000002';
const UA = 'bbbbbbbb-0000-4000-8000-00000000000a';
const P1 = 'cccccccc-0000-4000-8000-000000000001';
const NOW = '2026-11-11T08:00:00.000000Z';
const AUTH = '会话已失效';

const body=(title='Plan title')=>({title,date:null,time:null,place:null,note:'Independent plan note'});
const row=(id:string)=>({id,creator_id:UA,created_at:NOW,title:'Plan title',body_version:1,status:'want',status_version:1});
const page=(couple:string,items:ReturnType<typeof row>[]=[],cursor:string|null=null)=>({space:{id:couple,status:'active'},items,next_cursor:cursor,read_at:NOW});
const detail=(couple:string,id:string)=>({space:{id:couple,status:'active'},plan:{...row(id),body:body()},events:{total:'1',items:[{id:P1,seq:1,actor_id:UA,operation:'create',body_version:1,status_version:1,status:'want',proposal_id:null,outcome:null,created_at:NOW,body:body()}]},read_at:NOW});
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



beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();routeHolder.current=reactive({params:{}});api.list.mockResolvedValue(page(C1));api.detail.mockResolvedValue(detail(C1,T1));});
afterEach(()=>vi.restoreAllMocks());
describe('admin shared plan inspection',()=>{
  it('validates selected couple/plan, matching title, decimal totals and bounded event history',()=>{
    expect(isSharedPlanPage(page(C1,[row(T1)]),C1)).toBe(true);expect(isSharedPlanPage(page(C1),C2)).toBe(false);
    expect(isSharedPlanDetail(detail(C1,T1),C1,T1)).toBe(true);expect(isSharedPlanDetail(detail(C1,T1),C1,T2)).toBe(false);
    const changed=detail(C1,T1);changed.plan.body.title='unrelated';expect(isSharedPlanDetail(changed,C1,T1)).toBe(false);
    const over=detail(C1,T1);over.events.items=Array(51).fill(over.events.items[0]);expect(isSharedPlanDetail(over,C1,T1)).toBe(false);
    const bad=detail(C1,T1);bad.events.total='0';expect(isSharedPlanDetail(bad,C1,T1)).toBe(false);
  });
  it('paginates using server cursors and refresh starts from the newest page',async()=>{
    api.list.mockResolvedValueOnce(page(C1,[row(T1)],'p1-cursor')).mockResolvedValueOnce(page(C1,[row(T2)]));const s=useCoupleSharedPlansStore();
    await s.select(C1);await s.next();expect(api.list).toHaveBeenLastCalledWith(C1,'p1-cursor',50);await s.refreshList();expect(s.pageIndex).toBe(0);expect(api.list).toHaveBeenLastCalledWith(C1,null,50);
  });
  it('drops delayed responses from the previous couple',async()=>{
    const old=deferred<unknown>();api.list.mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(C2,[row(T2)]));const s=useCoupleSharedPlansStore();const p=s.select(C1);await s.select(C2);old.resolve(page(C1,[row(T1)]));await p;expect(s.items[0].id).toBe(T2);expect(s.listLoading).toBe(false);
  });
  it('opening another plan drops its earlier detail without stranding the list',async()=>{
    const s=useCoupleSharedPlansStore();await s.select(C1);const first=deferred<unknown>();api.detail.mockReturnValueOnce(first.promise).mockResolvedValueOnce(detail(C1,T2));
    const p=s.openPlan(T1);await s.openPlan(T2);first.resolve(detail(C1,T1));await p;expect(s.detail?.plan.id).toBe(T2);expect(s.detailLoading).toBe(false);
  });
  it.each([401,403])('list authorization loss %s clears private detail and rejects a delayed 200',async code=>{
    const a=deferred<unknown>(),b=deferred<unknown>();api.list.mockReturnValueOnce(a.promise);api.detail.mockReturnValueOnce(b.promise);const s=useCoupleSharedPlansStore();const x=s.select(C1),y=s.openPlan(T1);
    a.reject(new AdminApiError(code,'UNAUTHENTICATED','auth'));await x;b.resolve(detail(C1,T1));await y;expect(s.detail).toBeNull();expect(s.items).toEqual([]);expect(s.listLoading).toBe(false);expect(s.detailLoading).toBe(false);expect(s.listError).toContain(AUTH);
  });
  it('detail authorization loss clears and invalidates a delayed list read',async()=>{
    const a=deferred<unknown>(),b=deferred<unknown>();api.list.mockReturnValueOnce(a.promise);api.detail.mockReturnValueOnce(b.promise);const s=useCoupleSharedPlansStore();const x=s.select(C1),y=s.openPlan(T1);
    b.reject(new AdminApiError(403,'FORBIDDEN','auth'));await y;a.resolve(page(C1,[row(T1)]));await x;expect(s.items).toEqual([]);expect(s.detail).toBeNull();expect(s.detailError).toContain(AUTH);
  });
  it('logout/reset invalidates pending responses and unknown plan keeps the couple list',async()=>{
    const s=useCoupleSharedPlansStore();api.list.mockResolvedValue(page(C1,[row(T1)]));await s.select(C1);api.detail.mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','missing'));await s.openPlan(T1);expect(s.items.length).toBe(1);expect(s.planNotFound).toBe(true);
    const pending=deferred<unknown>();api.detail.mockReturnValueOnce(pending.promise);const p=s.refreshDetail();useAdminSessionStore().$patch({authenticated:false});pending.resolve(detail(C1,T1));await p;expect(s.detail).toBeNull();expect(s.coupleId).toBeNull();
  });
  it('renders read-only content/history and O12 note, opens direct route and clears on unmount',async()=>{
    api.list.mockResolvedValue(page(C1,[row(T1)]));routeHolder.current.params={coupleId:C1,planId:T1};const w=mount(View);await flushPromises();
    expect(w.text()).toContain('Independent plan note');expect(w.text()).toContain('历史请求不代表当前待办');expect(w.text()).toContain('不会因关联回忆被删除而清除');
    expect(w.findAll('button').some(b=>['同意','编辑','推进状态'].includes(b.text()))).toBe(false);w.unmount();expect(useCoupleSharedPlansStore().coupleId).toBeNull();
  });
});
