import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn(),detail:vi.fn(),items:vi.fn()}));
vi.mock('../../src/api/albums',async original=>({...await original<typeof import('../../src/api/albums')>(),albumAdminApi:api}));
const route=vi.hoisted(()=>({current:{params:{} as Record<string,string>}}));
vi.mock('vue-router',async()=>{const {defineComponent,h}=await import('vue');return {useRoute:()=>route.current,RouterLink:defineComponent({props:['to'],setup:(_p,{slots})=>()=>h('a',slots.default?.())})};});
import {AdminApiError} from '../../src/api/client';
import {isAlbumPage,isAlbumDetail,isItemPage,type AlbumView} from '../../src/api/albums';
import {useCoupleAlbumsStore} from '../../src/stores/couple-albums';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import View from '../../src/views/CoupleAlbumsView.vue';
const C='11111111-1111-4111-8111-111111111111',D='22222222-2222-4222-8222-222222222222',A='aaaaaaaa-0000-4000-8000-000000000001',B='aaaaaaaa-0000-4000-8000-000000000002',U='bbbbbbbb-0000-4000-8000-000000000001',P='cccccccc-0000-4000-8000-000000000001',NOW='2026-11-14T00:00:00.000000Z';
const space=(id=C)=>({id,status:'active'});
const album=(id=A)=>({id,creator_id:U,created_at:NOW,name:'相册名称',version:1,photo_count:'0',cover_media_id:null});
const page=(c=C,rows=[album()],cursor:string|null=null)=>({space:space(c),availability:'available',items:rows,next_cursor:cursor,read_at:NOW});
const detail=(c=C,id=A)=>({space:space(c),album:album(id),history_count:'0',read_at:NOW});
const photo=(seq=2)=>({seq,media_id:P,uploader_id:U,added_by:U,added_at:NOW,width:4,height:4,removed_by:null as string|null,removed_at:null as string|null});
const photos=(c=C,id=A,view:AlbumView='current',rows=[photo()],cursor:string|null=null)=>({space:space(c),album_id:id,view,items:rows,next_cursor:cursor,read_at:NOW});
const deferred=<T>()=>{let resolve!:(v:T)=>void,reject!:(e:unknown)=>void;const promise=new Promise<T>((r,j)=>{resolve=r;reject=j;});return {promise,resolve,reject};};
function signIn(){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id:'admin-1',username:'test'},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();route.current=reactive({params:{}});api.list.mockImplementation(async(c:string)=>page(c));api.detail.mockImplementation(async(c:string,id:string)=>detail(c,id));api.items.mockImplementation(async(c:string,id:string,v:AlbumView)=>photos(c,id,v));});
describe('admin album metadata inspection',()=>{
 it('keeps exact counts, rejects unknown fields, invalid totals and foreign cursors',()=>{
  expect(isAlbumPage(page(),C)).toBe(true);expect(isAlbumDetail(detail(),C,A)).toBe(true);
  const big=detail();big.album.photo_count=big.history_count='9007199254740993';expect(isAlbumDetail(big,C,A)).toBe(true);
  expect(isAlbumDetail({...detail(),history_count:'-0'},C,A)).toBe(false);expect(isAlbumPage({...page(),staging_key:'private'},C)).toBe(false);
  expect(isAlbumPage(page(C,[album()],`aa1.1.${A}.${D}`),C)).toBe(false);expect(isAlbumDetail(detail(),C,B)).toBe(false);
 });
 it('binds photo pages to couple, album and view and checks removals/order',()=>{
  expect(isItemPage(photos(),C,A,'current')).toBe(true);const removed=photo();removed.removed_by=U;removed.removed_at=NOW;
  expect(isItemPage(photos(C,A,'current',[removed]),C,A,'current')).toBe(false);expect(isItemPage(photos(C,A,'history',[removed]),C,A,'history')).toBe(true);
  expect(isItemPage(photos(C,B),C,A,'current')).toBe(false);expect(isItemPage(photos(C,A,'history'),C,A,'current')).toBe(false);
  expect(isItemPage(photos(C,A,'current',[photo()],`ai1.history.2.${A}.${C}`),C,A,'current')).toBe(false);
  expect(isItemPage(photos(C,A,'history',[photo(),photo()]),C,A,'history')).toBe(false);
 });
 it('uses server list cursors, previous pages and newest-page refresh',async()=>{
  const cursor=`aa1.1.${A}.${C}`;api.list.mockResolvedValueOnce(page(C,[album()],cursor)).mockResolvedValueOnce(page(C,[album(B)]));const s=useCoupleAlbumsStore();await s.select(C);await s.next();expect(api.list).toHaveBeenLastCalledWith(C,cursor);expect(s.pageIndex).toBe(1);await s.prev();expect(s.pageIndex).toBe(0);await s.refreshList();expect(api.list).toHaveBeenLastCalledWith(C,null);
 });
 it('paginates photo items independently and view switches start at the newest item',async()=>{
  const s=useCoupleAlbumsStore();await s.select(C);api.items.mockResolvedValueOnce(photos(C,A,'current',[photo()],`ai1.current.2.${A}.${C}`));await s.openAlbum(A);await s.nextItems();expect(api.items).toHaveBeenLastCalledWith(C,A,'current',`ai1.current.2.${A}.${C}`);await s.setView('history');expect(s.itemPageIndex).toBe(0);expect(api.items).toHaveBeenLastCalledWith(C,A,'history',null);expect(s.detail?.album.id).toBe(A);
 });
 it('view change does not discard or strand a pending same-album detail',async()=>{
  const p=deferred<unknown>();api.detail.mockReturnValueOnce(p.promise);const s=useCoupleAlbumsStore();await s.select(C);const opening=s.openAlbum(A);await s.setView('history');p.resolve(detail());await opening;expect(s.detail?.album.id).toBe(A);expect(s.detailLoading).toBe(false);expect(s.photos?.view).toBe('history');
 });
 it('drops prior view responses without changing the current item loader',async()=>{
  const old=deferred<unknown>();api.items.mockReturnValueOnce(old.promise);const s=useCoupleAlbumsStore();await s.select(C);const opening=s.openAlbum(A);await s.setView('history');old.resolve(photos());await opening;expect(s.photos?.view).toBe('history');expect(s.itemsLoading).toBe(false);
 });
 it('drops old album detail and items after a new selection',async()=>{
  const d=deferred<unknown>(),p=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);api.items.mockReturnValueOnce(p.promise);const s=useCoupleAlbumsStore();await s.select(C);const old=s.openAlbum(A);await s.openAlbum(B);d.resolve(detail());p.resolve(photos());await old;expect(s.detail?.album.id).toBe(B);expect(s.photos?.album_id).toBe(B);
 });
 it('drops all prior couple responses including a stale authorization error',async()=>{
  const old=deferred<unknown>();api.list.mockReturnValueOnce(old.promise);const s=useCoupleAlbumsStore();const a=s.select(C);await s.select(D);old.reject(new AdminApiError(401,'UNAUTHENTICATED','test'));await a;expect(s.space?.id).toBe(D);expect(s.listError).toBe('');
 });
 for(const status of [401,403])for(const source of ['list','detail','items'] as const)it(`${source} ${status} invalidates every in-flight read`,async()=>{
  const s=useCoupleAlbumsStore();await s.select(C);const l=deferred<unknown>(),d=deferred<unknown>(),p=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);api.items.mockReturnValueOnce(p.promise);const selecting=s.refreshList(),opening=s.openAlbum(A);
  ({list:l,detail:d,items:p})[source].reject(new AdminApiError(status,'UNAUTHENTICATED','test'));await flushPromises();
  if(source!=='list')l.resolve(page());if(source!=='detail')d.resolve(detail());if(source!=='items')p.resolve(photos());await Promise.all([selecting,opening]);
  expect(s.albums).toEqual([]);expect(s.detail).toBeNull();expect(s.photos).toBeNull();expect(s.listLoading||s.detailLoading||s.itemsLoading).toBe(false);expect(s.listError).toContain('会话');expect(s.space).toBeNull();
 });
 it('a 404 clears shown items and invalidates a late list response',async()=>{
  const l=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);const s=useCoupleAlbumsStore();const selecting=s.select(C),opening=s.openAlbum(A);await flushPromises();expect(s.photos).not.toBeNull();d.reject(new AdminApiError(404,'NOT_FOUND','test'));await opening;l.resolve(page());await selecting;expect(s.photos).toBeNull();expect(s.detail).toBeNull();expect(s.albums).toEqual([]);expect(s.detailError).toContain('不可用');
 });
 it('a recovery hold in the list clears all selected content and drops late detail',async()=>{
  const l=deferred<unknown>(),d=deferred<unknown>();api.list.mockReturnValueOnce(l.promise);api.detail.mockReturnValueOnce(d.promise);const s=useCoupleAlbumsStore();const selecting=s.select(C),opening=s.openAlbum(A);l.resolve({...page(C,[]),availability:'unavailable'});await selecting;d.resolve(detail());await opening;expect(s.availability).toBe('unavailable');expect(s.detail).toBeNull();expect(s.photos).toBeNull();expect(s.detailLoading||s.itemsLoading).toBe(false);
 });
 it('rejects malformed pages and allows explicit retry',async()=>{
  api.list.mockResolvedValueOnce({...page(),items:[{...album(),object_key:'no'}]});const s=useCoupleAlbumsStore();await s.select(C);expect(s.albums).toEqual([]);expect(s.listError).toBeTruthy();await s.refreshList();expect(s.albums).toHaveLength(1);
 });
 it('logout and reset discard late private content',async()=>{
  const d=deferred<unknown>();api.detail.mockReturnValueOnce(d.promise);const s=useCoupleAlbumsStore();await s.select(C);const opening=s.openAlbum(A);useAdminSessionStore().$patch({authenticated:false});d.resolve(detail());await opening;expect(s.coupleId).toBeNull();expect(s.detail).toBeNull();expect(s.photos).toBeNull();
 });
 it('renders the established admin table, switches history and clears content on unmount',async()=>{
  route.current=reactive({params:{coupleId:C,albumId:A}});const wrapper=mount(View);await flushPromises();expect(wrapper.text()).toContain('相册名称');expect(wrapper.text()).toContain('照片条目');
  const button=wrapper.findAll('button').find(b=>b.text()==='加入／移除历史');await button!.trigger('click');await flushPromises();expect(api.items).toHaveBeenLastCalledWith(C,A,'history',null);wrapper.unmount();expect(useCoupleAlbumsStore().detail).toBeNull();
 });
});
