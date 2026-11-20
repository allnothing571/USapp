import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn(),detail:vi.fn(),proposals:vi.fn(),comparison:vi.fn()}));
vi.mock('../../src/api/memories',async original=>({...await original<typeof import('../../src/api/memories')>(),memoryAdminApi:api}));
const route=vi.hoisted(()=>({current:{params:{} as Record<string,string>}}));
vi.mock('vue-router',async()=>{const {defineComponent,h}=await import('vue');return {useRoute:()=>route.current,RouterLink:defineComponent({props:['to'],setup:(_p,{slots})=>()=>h('a',slots.default?.())})};});
import {AdminApiError} from '../../src/api/client';
import {isMemoryPage,isMemoryDetail,isProposalPage,isComparison,decisionText,type Proposal} from '../../src/api/memories';
import {useCoupleMemoriesStore} from '../../src/stores/couple-memories';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import View from '../../src/views/CoupleMemoriesView.vue';
const C='11111111-1111-4111-8111-111111111111',D='22222222-2222-4222-8222-222222222222',A='aaaaaaaa-0000-4000-8000-000000000001',B='aaaaaaaa-0000-4000-8000-000000000002',U='bbbbbbbb-0000-4000-8000-000000000001',P='cccccccc-0000-4000-8000-000000000001',Q='cccccccc-0000-4000-8000-000000000002',NOW='2026-11-14T00:00:00.000000Z';
const space=(id=C)=>({id,status:'active'});
const memory=(id=A)=>({id,creator_id:U,created_at:NOW,updated_at:NOW,date:'2026-03-03',title:'当前标题',version:3,photo_count:0});
const page=(c=C,rows=[memory()],cursor:string|null=null)=>({space:space(c),availability:'available',items:rows,next_cursor:cursor,read_at:NOW});
const detail=(c=C,id=A)=>({space:space(c),memory:{...memory(id),description:'当前正文',photos:[{position:1,available:false}]},read_at:NOW});
const proposal=(id=P,m=A)=>({id,memory_id:m,proposer_id:U,base_version:1,created_at:NOW,status:'approved',decided_by:U,decided_at:NOW,approved_version:2});
const history=(c=C,id=A,rows=[proposal(P,id)],cursor:string|null=null)=>({space:space(c),memory_id:id,current_version:3,items:rows,next_cursor:cursor,read_at:NOW});
const comparison=(c=C,id=A,pid=P)=>({space:space(c),memory_id:id,current_version:3,comparison:{proposal:proposal(pid,id),original:{version:1,date:'2026-03-01',title:'原始标题',description:'原始正文',photos:[{position:1,available:false}]},proposed:{date:'2026-03-02',title:'请求标题',description:'请求正文',photos:[{position:1,available:false}]}},read_at:NOW});
const deferred=<T>()=>{let resolve!:(v:T)=>void,reject!:(e:unknown)=>void;const promise=new Promise<T>((r,j)=>{resolve=r;reject=j;});return {promise,resolve,reject};};
function signIn(){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id:'admin-1',username:'test'},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();route.current=reactive({params:{}});api.list.mockImplementation(async(c:string)=>page(c));api.detail.mockImplementation(async(c:string,id:string)=>detail(c,id));api.proposals.mockImplementation(async(c:string,id:string)=>history(c,id));api.comparison.mockImplementation(async(c:string,id:string,p:string)=>comparison(c,id,p));});
describe('admin memory inspection',()=>{
 it('accepts current and comparison shapes, and rejects secrets and foreign parents',()=>{
  expect(isMemoryPage(page(),C)).toBe(true);expect(isMemoryDetail(detail(),C,A)).toBe(true);expect(isProposalPage(history(),C,A)).toBe(true);expect(isComparison(comparison(),C,A,P)).toBe(true);
  expect(isMemoryDetail({...detail(),object_key:'secret'},C,A)).toBe(false);expect(isComparison(comparison(C,B),C,A,P)).toBe(false);expect(isComparison(comparison(),C,A,Q)).toBe(false);
  expect(isMemoryPage(page(C,[memory()],`am1.1.${A}.${D}`),C)).toBe(false);expect(isProposalPage(history(C,A,[proposal()],`mp1.1.${P}.${B}.${C}`),C,A)).toBe(false);
 });
 it('withheld photos carry only position and availability, across all three snapshots',()=>{
  const d=detail(),c=comparison();expect(isMemoryDetail(d,C,A)).toBe(true);
  for(const photos of [d.memory.photos,c.comparison.original.photos,c.comparison.proposed.photos])Object.assign(photos[0],{media_id:P});
  expect(isMemoryDetail(d,C,A)).toBe(false);expect(isComparison(c,C,A,P)).toBe(false);
 });
 it('rejects inconsistent decisions, original versions, counts and date normalization',()=>{
  const h=history();h.items[0].approved_version=3;expect(isProposalPage(h,C,A)).toBe(false);
  const c=comparison();c.comparison.original.version=2;expect(isComparison(c,C,A,P)).toBe(false);
  const d=detail();d.memory.photo_count=1;expect(isMemoryDetail(d,C,A)).toBe(false);const p=page();p.items[0].date='2026-02-31';expect(isMemoryPage(p,C)).toBe(false);
 });
 it('uses server cursors independently for parent and history pages',async()=>{
  const s=useCoupleMemoriesStore(),mc=`am1.1.${A}.${C}`,pc=`mp1.1.${P}.${A}.${C}`;api.list.mockResolvedValueOnce(page(C,[memory()],mc));await s.select(C);await s.next();expect(api.list).toHaveBeenLastCalledWith(C,mc);await s.prev();expect(s.pageIndex).toBe(0);
  api.proposals.mockResolvedValueOnce(history(C,A,[proposal()],pc));await s.openMemory(A);await s.nextItems();expect(api.proposals).toHaveBeenLastCalledWith(C,A,pc);await s.prevItems();expect(s.itemPageIndex).toBe(0);
 });
 it('history refresh cannot discard a pending same-memory detail',async()=>{
  const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const s=useCoupleMemoriesStore();await s.select(C);const open=s.openMemory(A);await s.refreshItems();d.resolve(detail());await open;expect(s.detail?.memory.id).toBe(A);expect(s.detailLoading).toBe(false);
 });
 it('switching memory drops old detail, history and comparison',async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);const d=deferred<unknown>(),h=deferred<unknown>(),c=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);api.proposals.mockReturnValueOnce(h.promise);api.comparison.mockReturnValueOnce(c.promise);const dr=s.refreshDetail(),hr=s.refreshItems(),cr=s.openProposal(P);await s.openMemory(B);d.resolve(detail());h.resolve(history());c.resolve(comparison());await Promise.all([dr,hr,cr]);expect(s.detail?.memory.id).toBe(B);expect(s.history?.memory_id).toBe(B);expect(s.comparison).toBeNull();
 });
 it('switching proposal drops prior comparison without stranding its loader',async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);const old=deferred<unknown>();api.comparison.mockReturnValueOnce(old.promise);const read=s.openProposal(P);await s.openProposal(Q);old.resolve(comparison());await read;expect(s.comparison?.comparison.proposal.id).toBe(Q);expect(s.comparisonLoading).toBe(false);
 });
 it('drops stale authorization failures after a couple switch',async()=>{
  const old=deferred<unknown>();api.list.mockReturnValueOnce(old.promise);const s=useCoupleMemoriesStore();const read=s.select(C);await s.select(D);old.reject(new AdminApiError(401,'UNAUTHENTICATED','test'));await read;expect(s.space?.id).toBe(D);expect(s.listError).toBe('');
 });
 for(const status of [401,403])for(const source of ['list','detail','proposals','comparison'] as const)it(`${source} ${status} invalidates all four reads`,async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);await s.openProposal(P);
  const l=deferred<unknown>(),d=deferred<unknown>(),h=deferred<unknown>(),c=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);api.proposals.mockReturnValueOnce(h.promise);api.comparison.mockReturnValueOnce(c.promise);
  const reads=[s.refreshList(),s.refreshDetail(),s.refreshItems(),s.openProposal(P)];({list:l,detail:d,proposals:h,comparison:c})[source].reject(new AdminApiError(status,'UNAUTHENTICATED','test'));await flushPromises();
  if(source!=='list')l.resolve(page());if(source!=='detail')d.resolve(detail());if(source!=='proposals')h.resolve(history());if(source!=='comparison')c.resolve(comparison());await Promise.all(reads);
  expect(s.memories).toEqual([]);expect(s.space).toBeNull();expect(s.detail).toBeNull();expect(s.history).toBeNull();expect(s.comparison).toBeNull();expect(s.listLoading||s.detailLoading||s.itemsLoading||s.comparisonLoading).toBe(false);expect(s.listError).toContain('会话');
 });
 for(const source of ['detail','proposals','comparison'] as const)it(`${source} 404 invalidates late parent list and selected content`,async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);await s.openProposal(P);const l=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api[source].mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','test'));const lr=s.refreshList();await (source==='detail'?s.refreshDetail():source==='proposals'?s.refreshItems():s.openProposal(P));l.resolve(page());await lr;expect(s.memories).toEqual([]);expect(s.history).toBeNull();expect(s.comparison).toBeNull();expect(s.detailError).toContain('不可用');
 });
 it('recovery hold clears selected contents and rejects a late comparison',async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);const c=deferred<unknown>();api.comparison.mockReturnValueOnce(c.promise);const read=s.openProposal(P);api.list.mockResolvedValueOnce({...page(C,[]),availability:'unavailable'});await s.refreshList();c.resolve(comparison());await read;expect(s.detail).toBeNull();expect(s.history).toBeNull();expect(s.comparison).toBeNull();expect(s.availability).toBe('unavailable');
 });
 it('logout invalidates all delayed responses and clears every display',async()=>{
  const s=useCoupleMemoriesStore();await s.select(C);await s.openMemory(A);const c=deferred<unknown>();api.comparison.mockReturnValueOnce(c.promise);const read=s.openProposal(P);useAdminSessionStore().$patch({authenticated:false});c.resolve(comparison());await read;expect(s.coupleId).toBeNull();expect(s.detail).toBeNull();expect(s.comparison).toBeNull();
 });
 it('shows historical approval using the history snapshot rather than a racing detail version, offers no write controls',async()=>{
  route.current.params={coupleId:C,memoryId:A};api.detail.mockResolvedValueOnce({...detail(),memory:{...detail().memory,version:2}});const wrapper=mount(View);await flushPromises();expect(wrapper.text()).toContain('历史批准为版本 2');expect(wrapper.text()).toContain('当前正文');expect(wrapper.text()).toContain('照片 1：不可用');const button=wrapper.findAll('button').find(b=>b.text()==='查看对比')!;await button.trigger('click');await flushPromises();expect(wrapper.text()).toContain('原始正文');expect(wrapper.text()).toContain('请求正文');expect(wrapper.find('img').exists()).toBe(false);expect(wrapper.text()).not.toContain('批准请求');wrapper.unmount();expect(useCoupleMemoriesStore().detail).toBeNull();
  expect(decisionText(proposal() as Proposal,2)).toContain('此快照的当前版本');
 });
});
