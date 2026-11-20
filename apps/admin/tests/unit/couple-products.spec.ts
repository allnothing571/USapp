import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn(),detail:vi.fn()}));
vi.mock('../../src/api/products',async original=>({...await original<typeof import('../../src/api/products')>(),productAdminApi:api}));
const route=vi.hoisted(()=>({current:{params:{} as Record<string,string>}}));
vi.mock('vue-router',async()=>{const {defineComponent,h}=await import('vue');return {useRoute:()=>route.current,RouterLink:defineComponent({props:['to'],setup:(_p,{slots})=>()=>h('a',slots.default?.())})};});
import {AdminApiError} from '../../src/api/client';
import {isProductPage,isProductDetail} from '../../src/api/products';
import {useCoupleProductsStore} from '../../src/stores/couple-products';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import View from '../../src/views/CoupleProductsView.vue';
const C='11111111-1111-4111-8111-111111111111',D='22222222-2222-4222-8222-222222222222',A='aaaaaaaa-0000-4000-8000-000000000001',B='aaaaaaaa-0000-4000-8000-000000000002',U='bbbbbbbb-0000-4000-8000-000000000001',V='bbbbbbbb-0000-4000-8000-000000000002',NOW='2026-11-14T00:00:00.000000Z';
const space=(id=C)=>({id,status:'active'});
const product=(id=A)=>({id,creator_id:U,owner_id:V,created_at:NOW,updated_at:NOW,title:'奖励名称',price_flowers:'999',redemption_mode:'request',status:'archived',version:3});
const page=(c=C,rows=[product()],cursor:string|null=null)=>({space:space(c),items:rows,next_cursor:cursor,read_at:NOW});
const detail=(c=C,id=A)=>({space:space(c),product:{...product(id),description:'当前商品说明'},read_at:NOW});
const deferred=<T>()=>{let resolve!:(v:T)=>void,reject!:(e:unknown)=>void;const promise=new Promise<T>((r,j)=>{resolve=r;reject=j;});return {promise,resolve,reject};};
function signIn(){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id:'admin-1',username:'test'},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();route.current=reactive({params:{}});api.list.mockImplementation(async(c:string)=>page(c));api.detail.mockImplementation(async(c:string,id:string)=>detail(c,id));});
describe('admin reward product inspection',()=>{
 it('accepts active and archived current metadata with exact decimal prices and distinct creator/owner',()=>{
  expect(isProductPage(page(),C)).toBe(true);expect(isProductDetail(detail(),C,A)).toBe(true);
  expect(isProductDetail({...detail(),product:{...detail().product,status:'active',price_flowers:'1',redemption_mode:'direct'}},C,A)).toBe(true);
  for(const price_flowers of ['0','-0','01','1.0','1000','9007199254740993'])expect(isProductDetail({...detail(),product:{...detail().product,price_flowers}},C,A)).toBe(false);
  expect(isProductDetail({...detail(),product:{...detail().product,price_flowers:999}},C,A)).toBe(false);expect(isProductDetail({...detail(),product:{...detail().product,owner_id:U}},C,A)).toBe(false);
 });
 it('rejects secrets, foreign identities, duplicate rows, invalid modes and wrong cursor kind or scope',()=>{
  expect(isProductDetail({...detail(),operation_id:'private'},C,A)).toBe(false);expect(isProductDetail({...detail(),product:{...detail().product,request_fingerprint:'private'}},C,A)).toBe(false);expect(isProductDetail(detail(C,B),C,A)).toBe(false);expect(isProductPage(page(D),C)).toBe(false);expect(isProductPage(page(C,[product(),product()]),C)).toBe(false);
  expect(isProductPage(page(C,[product()],`rp1.1.${A}.${C}`),C)).toBe(true);for(const cursor of [`rp1.1.${A}.${D}`,`an1.1.${A}.${C}`,`rp1.9223372036854775808.${A}.${C}`,`rp1.01.${A}.${C}`])expect(isProductPage(page(C,[product()],cursor),C)).toBe(false);
  expect(isProductDetail({...detail(),product:{...detail().product,redemption_mode:'sunshine'}},C,A)).toBe(false);
 });
 it('uses server cursors, previous page and newest-page refresh',async()=>{
  const cursor=`rp1.1.${A}.${C}`;api.list.mockResolvedValueOnce(page(C,[product()],cursor)).mockResolvedValueOnce(page(C,[product(B)]));const s=useCoupleProductsStore();await s.select(C);await s.next();expect(api.list).toHaveBeenLastCalledWith(C,cursor);expect(s.pageIndex).toBe(1);await s.prev();expect(s.pageIndex).toBe(0);await s.refreshList();expect(api.list).toHaveBeenLastCalledWith(C,null);
 });
 it('another product selection does not discard or strand a pending same-couple list',async()=>{
  const p=deferred<unknown>();api.list.mockReturnValueOnce(p.promise);const s=useCoupleProductsStore();const selecting=s.select(C);await s.openProduct(B);p.resolve(page());await selecting;expect(s.detail?.product.id).toBe(B);expect(s.items[0].id).toBe(A);expect(s.listLoading).toBe(false);
 });
 it('list refresh cannot discard a pending same-product detail',async()=>{
  const p=deferred<unknown>();api.detail.mockReturnValueOnce(p.promise);const s=useCoupleProductsStore();await s.select(C);const opening=s.openProduct(A);await s.refreshList();p.resolve(detail());await opening;expect(s.detail?.product.id).toBe(A);expect(s.detailLoading).toBe(false);
 });
 it('drops the old product response after another selection',async()=>{
  const p=deferred<unknown>();api.detail.mockReturnValueOnce(p.promise);const s=useCoupleProductsStore();await s.select(C);const old=s.openProduct(A);await s.openProduct(B);p.resolve(detail());await old;expect(s.detail?.product.id).toBe(B);expect(s.detailLoading).toBe(false);
 });
 it('drops prior-couple authorization errors',async()=>{
  const p=deferred<unknown>();api.list.mockReturnValueOnce(p.promise);const s=useCoupleProductsStore();const old=s.select(C);await s.select(D);p.reject(new AdminApiError(401,'UNAUTHENTICATED','test'));await old;expect(s.space?.id).toBe(D);expect(s.listError).toBe('');
 });
 for(const status of [401,403])for(const source of ['list','detail'] as const)it(`${source} ${status} invalidates all delayed replies and clears all private data`,async()=>{
  const s=useCoupleProductsStore();await s.select(C);await s.openProduct(A);const l=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);const reads=[s.refreshList(),s.refreshDetail()];({list:l,detail:d})[source].reject(new AdminApiError(status,'UNAUTHENTICATED','test'));await flushPromises();if(source!=='list')l.resolve(page());if(source!=='detail')d.resolve(detail());await Promise.all(reads);expect(s.space).toBeNull();expect(s.items).toEqual([]);expect(s.detail).toBeNull();expect(s.listLoading||s.detailLoading).toBe(false);expect(s.listError).toContain('会话');
 });
 it('product 404 invalidates the old list and selected content',async()=>{
  const l=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','test'));const s=useCoupleProductsStore();const selecting=s.select(C);await s.openProduct(A);l.resolve(page());await selecting;expect(s.detail).toBeNull();expect(s.items).toEqual([]);expect(s.detailError).toContain('没有');
 });
 it('a missing couple clears selected content and drops a pending detail',async()=>{
  const s=useCoupleProductsStore();await s.select(C);const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const read=s.openProduct(A);api.list.mockRejectedValueOnce(new AdminApiError(404,'SPACE_NOT_FOUND','test'));await s.refreshList();d.resolve(detail());await read;expect(s.detail).toBeNull();expect(s.space).toBeNull();expect(s.detailLoading).toBe(false);
 });
 it('rejects malformed responses and offers explicit read retry',async()=>{
  const s=useCoupleProductsStore();await s.select(C);api.detail.mockResolvedValueOnce({...detail(),product:{...detail().product,price_flowers:'-1'}});await s.openProduct(A);expect(s.detail).toBeNull();expect(s.detailError).toContain('重试');await s.refreshDetail();expect(s.detail?.product.id).toBe(A);expect(s.detailError).toBe('');
 });
 it('logout and unmount clear state and invalidate pending successes',async()=>{
  const s=useCoupleProductsStore();await s.select(C);const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const read=s.openProduct(A);useAdminSessionStore().$patch({authenticated:false});d.resolve(detail());await read;expect(s.coupleId).toBeNull();expect(s.detail).toBeNull();signIn();route.current.params={coupleId:C,productId:A};const view=mount(View);await flushPromises();expect(s.detail).not.toBeNull();view.unmount();expect(s.detail).toBeNull();
 });
 it('shows archived metadata with current-price versus historical-receipt copy and no write controls',async()=>{
  route.current.params={coupleId:C,productId:A};const view=mount(View);await flushPromises();expect(view.text()).toContain('当前商品说明');expect(view.text()).toContain('999 朵小红花');expect(view.text()).toContain('已归档下架');expect(view.text()).toContain('不是历史兑换成交快照');expect(view.text()).toContain('不恢复普通领取者');expect(view.findAll('button').some(b=>/兑换|退款|修改|下架|保存/.test(b.text()))).toBe(false);view.unmount();
 });
});
