import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn(),detail:vi.fn()}));
vi.mock('../../src/api/anniversaries',async original=>({...await original<typeof import('../../src/api/anniversaries')>(),anniversaryAdminApi:api}));
const route=vi.hoisted(()=>({current:{params:{} as Record<string,string>}}));
vi.mock('vue-router',async()=>{const {defineComponent,h}=await import('vue');return {useRoute:()=>route.current,RouterLink:defineComponent({props:['to'],setup:(_p,{slots})=>()=>h('a',slots.default?.())})};});
import {AdminApiError} from '../../src/api/client';
import {isAnniversaryPage,isAnniversaryDetail,decisionText,type Decision} from '../../src/api/anniversaries';
import {useCoupleAnniversariesStore} from '../../src/stores/couple-anniversaries';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import View from '../../src/views/CoupleAnniversariesView.vue';
const C='11111111-1111-4111-8111-111111111111',D='22222222-2222-4222-8222-222222222222',A='aaaaaaaa-0000-4000-8000-000000000001',B='aaaaaaaa-0000-4000-8000-000000000002',U='bbbbbbbb-0000-4000-8000-000000000001',P='cccccccc-0000-4000-8000-000000000001',Q='cccccccc-0000-4000-8000-000000000002',NOW='2026-11-14T00:00:00.000000Z';
const space=(id=C)=>({id,status:'active'});
const anniversary=(id=A)=>({id,creator_id:U,created_at:NOW,updated_at:NOW,date:'2020-05-20',title:'在一起',kind:'anniversary',version:3});
const page=(c=C,rows=[anniversary()],cursor:string|null=null)=>({space:space(c),items:rows,next_cursor:cursor,read_at:NOW});
const pending=()=>({id:P,proposer_id:U,base_version:3,created_at:NOW,title:'请求标题',date:'2027-12-31',kind:'countdown',note:'请求备注'});
const decision=()=>({proposal_id:Q,proposer_id:U,base_version:1,outcome:'approved',decided_by:U,decided_at:NOW,approved_version:2 as number|null});
const detail=(c=C,id=A)=>({space:space(c),anniversary:{...anniversary(id),note:'当前备注'},pending_proposal:pending(),last_decision:decision(),read_at:NOW});
const deferred=<T>()=>{let resolve!:(v:T)=>void,reject!:(e:unknown)=>void;const promise=new Promise<T>((r,j)=>{resolve=r;reject=j;});return {promise,resolve,reject};};
function signIn(){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id:'admin-1',username:'test'},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();route.current=reactive({params:{}});api.list.mockImplementation(async(c:string)=>page(c));api.detail.mockImplementation(async(c:string,id:string)=>detail(c,id));});
describe('admin anniversary inspection',()=>{
 it('validates exact shapes, context and bound cursors without exposing operational fields',()=>{
  expect(isAnniversaryPage(page(),C)).toBe(true);expect(isAnniversaryDetail(detail(),C,A)).toBe(true);
  expect(isAnniversaryDetail({...detail(),pinned_by_me:true},C,A)).toBe(false);expect(isAnniversaryDetail(detail(C,B),C,A)).toBe(false);expect(isAnniversaryPage(page(D),C)).toBe(false);
  expect(isAnniversaryPage(page(C,[anniversary()],`an1.1.${A}.${C}`),C)).toBe(true);expect(isAnniversaryPage(page(C,[anniversary()],`an1.1.${A}.${D}`),C)).toBe(false);expect(isAnniversaryPage(page(C,[anniversary()],`an1.9223372036854775808.${A}.${C}`),C)).toBe(false);
  expect(isAnniversaryDetail({...detail(),anniversary:{...detail().anniversary,request_fingerprint:'private'}},C,A)).toBe(false);
 });
 it('checks dates, calendar kind, pending version and last-decision consistency',()=>{
  for(const date of ['2026-02-31','1899-12-31','2200-01-01'])expect(isAnniversaryPage(page(C,[{...anniversary(),date}]),C)).toBe(false);
  const d=detail();d.pending_proposal.base_version=2;expect(isAnniversaryDetail(d,C,A)).toBe(false);
  const a=detail();a.last_decision.approved_version=3;expect(isAnniversaryDetail(a,C,A)).toBe(false);
  const same=detail();same.last_decision.proposal_id=P;expect(isAnniversaryDetail(same,C,A)).toBe(false);
  expect(isAnniversaryDetail({...detail(),pending_proposal:null,last_decision:null},C,A)).toBe(true);
  expect(isAnniversaryDetail({...detail(),last_decision:{...decision(),outcome:'withdrawn',approved_version:null}},C,A)).toBe(true);
  expect(isAnniversaryDetail({...detail(),last_decision:{...decision(),outcome:'rejected'}},C,A)).toBe(false);
 });
 it('uses server cursors, previous page and newest-page refresh',async()=>{
  const cursor=`an1.1.${A}.${C}`;api.list.mockResolvedValueOnce(page(C,[anniversary()],cursor)).mockResolvedValueOnce(page(C,[anniversary(B)]));const s=useCoupleAnniversariesStore();await s.select(C);await s.next();expect(api.list).toHaveBeenLastCalledWith(C,cursor);expect(s.pageIndex).toBe(1);await s.prev();expect(s.pageIndex).toBe(0);await s.refreshList();expect(api.list).toHaveBeenLastCalledWith(C,null);
 });
 it('selecting another anniversary does not strand an in-flight same-couple list',async()=>{
  const p=deferred<unknown>();api.list.mockReturnValueOnce(p.promise);const s=useCoupleAnniversariesStore();const selecting=s.select(C);await s.openAnniversary(B);p.resolve(page());await selecting;expect(s.detail?.anniversary.id).toBe(B);expect(s.items[0].id).toBe(A);expect(s.listLoading).toBe(false);
 });
 it('list refresh does not discard a pending same-parent detail',async()=>{
  const p=deferred<unknown>();api.detail.mockReturnValueOnce(p.promise);const s=useCoupleAnniversariesStore();await s.select(C);const opening=s.openAnniversary(A);await s.refreshList();p.resolve(detail());await opening;expect(s.detail?.anniversary.id).toBe(A);expect(s.detailLoading).toBe(false);
 });
 it('drops an old detail after another selection without changing its loader',async()=>{
  const p=deferred<unknown>();api.detail.mockReturnValueOnce(p.promise);const s=useCoupleAnniversariesStore();await s.select(C);const old=s.openAnniversary(A);await s.openAnniversary(B);p.resolve(detail());await old;expect(s.detail?.anniversary.id).toBe(B);expect(s.detailLoading).toBe(false);
 });
 it('drops prior-couple responses and stale authorization errors',async()=>{
  const p=deferred<unknown>();api.list.mockReturnValueOnce(p.promise);const s=useCoupleAnniversariesStore();const old=s.select(C);await s.select(D);p.reject(new AdminApiError(401,'UNAUTHENTICATED','test'));await old;expect(s.space?.id).toBe(D);expect(s.listError).toBe('');
 });
 for(const status of [401,403])for(const source of ['list','detail'] as const)it(`${source} ${status} clears data and invalidates every delayed read`,async()=>{
  const s=useCoupleAnniversariesStore();await s.select(C);await s.openAnniversary(A);const l=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);const reads=[s.refreshList(),s.refreshDetail()];({list:l,detail:d})[source].reject(new AdminApiError(status,'UNAUTHENTICATED','test'));await flushPromises();if(source!=='list')l.resolve(page());if(source!=='detail')d.resolve(detail());await Promise.all(reads);expect(s.space).toBeNull();expect(s.items).toEqual([]);expect(s.detail).toBeNull();expect(s.listLoading||s.detailLoading).toBe(false);expect(s.listError).toContain('会话');
 });
 it('detail 404 clears content and invalidates a delayed list',async()=>{
  const l=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','test'));const s=useCoupleAnniversariesStore();const selecting=s.select(C);await s.openAnniversary(A);l.resolve(page());await selecting;expect(s.detail).toBeNull();expect(s.items).toEqual([]);expect(s.detailError).toContain('没有');
 });
 it('unknown space clears detail and rejects its pending success',async()=>{
  const s=useCoupleAnniversariesStore();await s.select(C);const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const read=s.openAnniversary(A);api.list.mockRejectedValueOnce(new AdminApiError(404,'SPACE_NOT_FOUND','test'));await s.refreshList();d.resolve(detail());await read;expect(s.detail).toBeNull();expect(s.space).toBeNull();expect(s.detailLoading).toBe(false);
 });
 it('rejects malformed server responses and supports explicit retry',async()=>{
  const s=useCoupleAnniversariesStore();await s.select(C);api.detail.mockResolvedValueOnce({...detail(),last_decision:{...decision(),approved_version:99}});await s.openAnniversary(A);expect(s.detail).toBeNull();expect(s.detailError).toContain('重试');await s.refreshDetail();expect(s.detail?.anniversary.id).toBe(A);expect(s.detailError).toBe('');
 });
 it('logout and unmount reset state and prevent late response repopulation',async()=>{
  const s=useCoupleAnniversariesStore();await s.select(C);const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const read=s.openAnniversary(A);useAdminSessionStore().$patch({authenticated:false});d.resolve(detail());await read;expect(s.coupleId).toBeNull();expect(s.detail).toBeNull();signIn();route.current.params={coupleId:C,anniversaryId:A};const view=mount(View);await flushPromises();expect(s.detail).not.toBeNull();view.unmount();expect(s.detail).toBeNull();
 });
 it('renders pending versus current terms and historical approval without write controls',async()=>{
  route.current.params={coupleId:C,anniversaryId:A};const view=mount(View);await flushPromises();expect(view.text()).toContain('当前备注');expect(view.text()).toContain('请求备注');expect(view.text()).toContain('历史批准为版本 2');expect(view.text()).toContain('不代表完整修改历史');expect(view.text()).toContain('倒计时');expect(view.findAll('button').some(b=>/批准|拒绝|删除|保存/.test(b.text()))).toBe(false);expect(view.find('img').exists()).toBe(false);view.unmount();
  expect(decisionText(decision() as Decision,2)).toContain('此详情快照的当前版本');
 });
});
