import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn()}));
vi.mock('../../src/api/user-sessions',async original=>({...await original<typeof import('../../src/api/user-sessions')>(),userSessionApi:api}));
const route=vi.hoisted(()=>({value:{params:{} as Record<string,string>}}));
vi.mock('vue-router',()=>({useRoute:()=>route.value}));
import {AdminApiError} from '../../src/api/client';
import {isUserSessionPage} from '../../src/api/user-sessions';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import {useUserSessionsStore} from '../../src/stores/user-sessions';
import View from '../../src/views/UserSessionsView.vue';
const U1='11111111-1111-4111-8111-111111111111',U2='22222222-2222-4222-8222-222222222222';
const S1='aaaaaaaa-0000-4000-8000-000000000001',S2='aaaaaaaa-0000-4000-8000-000000000002';
const NOW='2026-11-12T08:00:00.123456Z';
const row=(id=S1)=>({id,created_at:NOW,last_seen_at:NOW,expires_at:'2030-01-01T00:00:00Z',revoked_at:null,state:'valid'});
const page=(user=U1,items=[row()],cursor:string|null=null)=>({user:{id:user,status:'active'},items,next_cursor:cursor,read_at:NOW});
function deferred(){let resolve!:(v:unknown)=>void,reject!:(e:unknown)=>void;const promise=new Promise<unknown>((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};}
function login(id='admin-1'){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id,username:id},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();login();api.list.mockResolvedValue(page());route.value=reactive({params:{}});});
describe('user session metadata',()=>{
  it('validates user scope, timestamps, cap and revocation state without truncating microseconds',()=>{
    expect(isUserSessionPage(page(),U1)).toBe(true);expect(isUserSessionPage(page(),U2)).toBe(false);
    const malformed=page();(malformed.items[0] as any).expires_at='not-a-time';expect(isUserSessionPage(malformed,U1)).toBe(false);
    const revoked=page();(revoked.items[0] as any).state='revoked';expect(isUserSessionPage(revoked,U1)).toBe(false);
    const expired=page();expired.items[0].state='expired';expect(isUserSessionPage(expired,U1)).toBe(true);
    expect(isUserSessionPage(page(U1,Array(101).fill(row())),U1)).toBe(false);
  });
  it('paginates using exact server cursors and starts refresh at page zero',async()=>{
    api.list.mockResolvedValueOnce(page(U1,[row()],'s1-cursor')).mockResolvedValueOnce(page(U1,[row(S2)]));
    const s=useUserSessionsStore();await s.select(U1);await s.next();expect(api.list).toHaveBeenLastCalledWith(U1,'s1-cursor',50);expect(s.pageIndex).toBe(1);
    await s.prev();expect(api.list).toHaveBeenLastCalledWith(U1,null,50);await s.refresh();expect(s.pageIndex).toBe(0);
  });
  it('drops an old user response without stranding the new loader',async()=>{
    const old=deferred();api.list.mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(U2,[row(S2)]));const s=useUserSessionsStore();
    const p=s.select(U1);await s.select(U2);old.resolve(page());await p;expect(s.user?.id).toBe(U2);expect(s.items[0].id).toBe(S2);expect(s.loading).toBe(false);
  });
  it.each([401,403])('current %s clears shown data and invalidates an older read in flight',async code=>{
    const s=useUserSessionsStore();await s.select(U1);const old=deferred(),current=deferred();api.list.mockReturnValueOnce(old.promise).mockReturnValueOnce(current.promise);
    const a=s.refresh(),b=s.refresh();current.reject(new AdminApiError(code,'UNAUTHENTICATED','auth'));await b;
    old.resolve(page());await a;expect(s.items).toEqual([]);expect(s.user).toBeNull();expect(s.loading).toBe(false);expect(s.error).toContain('会话已失效');expect(api.list).toHaveBeenCalledTimes(3);
  });
  it('logout, a replacement admin and reset invalidate delayed reads',async()=>{
    const old=deferred();api.list.mockReturnValueOnce(old.promise);const s=useUserSessionsStore();const p=s.select(U1);useAdminSessionStore().$patch({authenticated:false});
    old.resolve(page());await p;expect(s.userId).toBeNull();expect(s.items).toEqual([]);
    login('admin-2');api.list.mockResolvedValue(page(U2));await s.select(U2);s.reset();expect(s.user).toBeNull();expect(s.loading).toBe(false);
  });
  it('distinguishes an empty known user, unknown user and malformed response',async()=>{
    const s=useUserSessionsStore();api.list.mockResolvedValueOnce(page(U1,[]));await s.select(U1);expect(s.items).toEqual([]);expect(s.error).toBe('');
    api.list.mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','missing'));await s.refresh();expect(s.notFound).toBe(true);
    api.list.mockResolvedValueOnce(page(U2));await s.refresh();expect(s.items).toEqual([]);expect(s.notFound).toBe(false);expect(s.error).toContain('读取失败');
  });
  it('shows disabled account separately from time-valid session; only refresh and paging controls exist',async()=>{
    const data=page();data.user.status='disabled';api.list.mockResolvedValue(data);route.value.params={userId:U1};
    const w=mount(View);await flushPromises();expect(w.text()).toContain('已禁用');expect(w.text()).toContain('未过期且未撤销');expect(w.text()).toContain('不保证当前可以登录');
    expect(w.findAll('button').map(b=>b.text()).some(label=>['强制退出','撤销','续期'].includes(label))).toBe(false);
    w.unmount();expect(useUserSessionsStore().userId).toBeNull();
  });
});
