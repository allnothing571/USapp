import { beforeEach, afterEach, describe, it, expect, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { flushPromises, mount } from '@vue/test-utils';
import { reactive } from 'vue';

const api = vi.hoisted(() => ({ list: vi.fn(), detail: vi.fn() }));
vi.mock('../../src/api/redemptions', async (original) => ({ ...(await original<typeof import('../../src/api/redemptions')>()), redemptionAdminApi: api }));
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
import { isRedemptionDetail, isRedemptionPage } from '../../src/api/redemptions';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import { useCoupleRedemptionsStore } from '../../src/stores/couple-redemptions';
import View from '../../src/views/CoupleRedemptionsView.vue';

const C1 = '11111111-1111-4111-8111-111111111111';
const C2 = '22222222-2222-4222-8222-222222222222';
const T1 = 'aaaaaaaa-0000-4000-8000-000000000001';
const T2 = 'aaaaaaaa-0000-4000-8000-000000000002';
const UA = 'bbbbbbbb-0000-4000-8000-00000000000a';
const UB = 'bbbbbbbb-0000-4000-8000-00000000000b';
const P1 = 'cccccccc-0000-4000-8000-000000000001';
const NOW = '2026-11-11T08:00:00.000000Z';
const AUTH = '会话已失效';

const row=(id:string)=>({id,product_id:P1,product_version:1,creator_id:UA,redeemer_id:UB,title:'商品快照',price_flowers:'9007199254740993',redemption_mode:'request',status:'pending_confirmation',created_at:NOW,fulfilled_at:null,fulfilled_by:null,cancel_reason:null,cancelled_at:null,cancellation_id:null,cancellation_by:null});
const page=(couple:string,items:ReturnType<typeof row>[]=[],cursor:string|null=null,status:string|null=null)=>({space:{id:couple,status:'active'},status,items,next_cursor:cursor,read_at:NOW});
const detail=(couple:string,id:string)=>({space:{id:couple,status:'active'},redemption:{...row(id),description:'原始描述'},events:{total:0,items:[]},read_at:NOW});
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


beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();routeHolder.current=reactive({params:{} as Record<string,string>});api.list.mockResolvedValue(page(C1));api.detail.mockResolvedValue(detail(C1,T1));});
afterEach(()=>{vi.restoreAllMocks();});
describe('admin redemption inspection',()=>{
  it('validates scope, decimal strings, status and bounded event histories',()=>{
    expect(isRedemptionPage(page(C1,[row(T1)]),C1,'')).toBe(true);
    expect(isRedemptionPage(page(C1,[row(T1)]),C2,'')).toBe(false);
    expect(isRedemptionPage(page(C1,[row(T1)]),C1,'fulfilled')).toBe(false);
    expect(isRedemptionDetail(detail(C1,T1),C1,T1)).toBe(true);
    expect(isRedemptionDetail(detail(C1,T1),C1,T2)).toBe(false);
    const broken=detail(C1,T1);(broken.redemption as any).price_flowers=7;
    expect(isRedemptionDetail(broken,C1,T1)).toBe(false);
    const events=detail(C1,T1);(events.events as any).items=Array(101).fill({});
    expect(isRedemptionDetail(events,C1,T1)).toBe(false);
  });
  it('pages with server cursors and resets only list identity on filter change',async()=>{
    const store=useCoupleRedemptionsStore();api.list.mockResolvedValueOnce(page(C1,[row(T1)],'r1-cursor')).mockResolvedValueOnce(page(C1,[row(T2)]));
    await store.select(C1);await store.next();expect(api.list).toHaveBeenLastCalledWith(C1,'','r1-cursor',50);
    api.list.mockResolvedValue(page(C1,[],null,'fulfilled'));await store.selectStatus('fulfilled');
    expect(store.pageIndex).toBe(0);expect(store.items).toEqual([]);expect(api.list).toHaveBeenLastCalledWith(C1,'fulfilled',null,50);
  });
  it('drops old couple answers and settles new reads',async()=>{
    const old=deferred<unknown>();api.list.mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(C2,[row(T2)]));
    const store=useCoupleRedemptionsStore();const p=store.select(C1);await store.select(C2);old.resolve(page(C1,[row(T1)]));await p;
    expect(store.coupleId).toBe(C2);expect(store.items.map(i=>i.id)).toEqual([T2]);expect(store.listLoading).toBe(false);
  });
  it('filter change does not discard a pending detail or strand its loader',async()=>{
    const store=useCoupleRedemptionsStore();await store.select(C1);const slow=deferred<unknown>();api.detail.mockReturnValueOnce(slow.promise);
    const d=store.openRedemption(T1);api.list.mockResolvedValue(page(C1,[],null,'fulfilled'));await store.selectStatus('fulfilled');slow.resolve(detail(C1,T1));await d;
    expect(store.detail?.redemption.id).toBe(T1);expect(store.detailLoading).toBe(false);
  });
  it.each([401,403])('list auth loss %s invalidates delayed detail 200 and clears both loaders',async(code)=>{
    const list=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(list.promise);api.detail.mockReturnValueOnce(d.promise);
    const store=useCoupleRedemptionsStore();const a=store.select(C1),b=store.openRedemption(T1);
    list.reject(new AdminApiError(code,'UNAUTHENTICATED','auth'));await a;d.resolve(detail(C1,T1));await b;
    expect(store.detail).toBeNull();expect(store.items).toEqual([]);expect(store.listLoading).toBe(false);expect(store.detailLoading).toBe(false);expect(store.listError).toContain(AUTH);
  });
  it('detail auth loss invalidates delayed list 200',async()=>{
    const list=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(list.promise);api.detail.mockReturnValueOnce(d.promise);
    const store=useCoupleRedemptionsStore();const a=store.select(C1),b=store.openRedemption(T1);d.reject(new AdminApiError(403,'FORBIDDEN','auth'));await b;
    list.resolve(page(C1,[row(T1)]));await a;expect(store.items).toEqual([]);expect(store.detail).toBeNull();expect(store.detailError).toContain(AUTH);
  });
  it('logout and unmount reset invalidate pending reads',async()=>{
    const p=deferred<unknown>();api.list.mockReturnValueOnce(p.promise);const store=useCoupleRedemptionsStore();const a=store.select(C1);
    useAdminSessionStore().$patch({authenticated:false});p.resolve(page(C1,[row(T1)]));await a;expect(store.coupleId).toBeNull();expect(store.items).toEqual([]);
    signIn();await store.select(C1);store.reset();expect(store.coupleId).toBeNull();expect(store.detail).toBeNull();
  });
  it('unknown space differs from a foreign/missing redemption',async()=>{
    const store=useCoupleRedemptionsStore();await store.select(C1);api.detail.mockRejectedValueOnce(new AdminApiError(404,'REDEMPTION_NOT_FOUND','missing'));
    await store.openRedemption(T1);expect(store.redemptionNotFound).toBe(true);expect(store.spaceNotFound).toBe(false);
    api.list.mockRejectedValueOnce(new AdminApiError(404,'SPACE_NOT_FOUND','missing'));await store.refreshList();expect(store.spaceNotFound).toBe(true);
  });
  it('renders read-only snapshot and exact price, opens from route and clears on unmount',async()=>{
    api.list.mockResolvedValue(page(C1,[row(T1)]));routeHolder.current.params={coupleId:C1,redemptionId:T1};
    const w=mount(View);await flushPromises();
    expect(w.text()).toContain('9007199254740993');expect(w.text()).toContain('原始描述');expect(w.text()).toContain('只读');
    expect(w.findAll('button').map(b=>b.text()).some(x=>['退款','确认履约','同意取消'].includes(x))).toBe(false);
    w.unmount();expect(useCoupleRedemptionsStore().coupleId).toBeNull();
  });
});
