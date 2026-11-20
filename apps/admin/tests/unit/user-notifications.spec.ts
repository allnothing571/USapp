import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {flushPromises,mount} from '@vue/test-utils';
import {reactive} from 'vue';
const api=vi.hoisted(()=>({list:vi.fn()}));
vi.mock('../../src/api/notifications',async original=>({...await original<typeof import('../../src/api/notifications')>(),notificationAdminApi:api}));
const route=vi.hoisted(()=>({value:{params:{} as Record<string,string>}}));
vi.mock('vue-router',()=>({useRoute:()=>route.value}));
import {AdminApiError} from '../../src/api/client';
import {isNotificationPage} from '../../src/api/notifications';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import {useUserNotificationsStore} from '../../src/stores/user-notifications';
import View from '../../src/views/UserNotificationsView.vue';
const U1='11111111-1111-4111-8111-111111111111',U2='22222222-2222-4222-8222-222222222222';
const S1='aaaaaaaa-0000-4000-8000-000000000001',S2='aaaaaaaa-0000-4000-8000-000000000002';
const NOW='2026-11-12T08:00:00.123456Z';
const delivery=()=>({generation_id:S2,created_at:NOW,final_state:'accepted',decided_at:NOW,latest_attempt:{attempt_no:1,claimed_at:NOW,lease_until:NOW,expired_at:null,observation:{class:'provider',outcome:'accepted',http_status:201,retry_after_s:null,observed_at:NOW}}});
const row=(id=S1)=>({id,event_type:'message_created',created_at:NOW,read_at:null,push:{total:'1',items:[delivery()]}});
const page=(user=U1,items=[row()],cursor:string|null=null)=>({user:{id:user,status:'active'},items,next_cursor:cursor,read_at:NOW});
function deferred(){let resolve!:(v:unknown)=>void,reject!:(e:unknown)=>void;const promise=new Promise<unknown>((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};}
function login(id='admin-1'){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id,username:id},epoch:s.epoch+1});}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();login();api.list.mockResolvedValue(page());route.value=reactive({params:{}});});
describe('user notification metadata',()=>{
  it('validates recipient, delivery cap, exact decimal totals, outcome class and final-state facts',()=>{
    expect(isNotificationPage(page(),U1)).toBe(true);expect(isNotificationPage(page(),U2)).toBe(false);
    const tooMany=page();tooMany.items[0].push.items=Array(6).fill(delivery());expect(isNotificationPage(tooMany,U1)).toBe(false);
    const wrongClass=page();wrongClass.items[0].push.items[0].latest_attempt.observation.class='internal';expect(isNotificationPage(wrongClass,U1)).toBe(false);
    const badStatus=page();badStatus.items[0].push.items[0].latest_attempt.observation.http_status=999;expect(isNotificationPage(badStatus,U1)).toBe(false);
    const badTotal=page();badTotal.items[0].push.total='0';expect(isNotificationPage(badTotal,U1)).toBe(false);
    expect(isNotificationPage(page(U1,Array(101).fill(row())),U1)).toBe(false);
  });
  it('allows absent results and internal skip codes without inventing success or failure',()=>{
    const pending=page();(pending.items[0].push.items[0] as any).final_state=null;(pending.items[0].push.items[0] as any).decided_at=null;
    (pending.items[0].push.items[0].latest_attempt as any).observation=null;expect(isNotificationPage(pending,U1)).toBe(true);
    (pending.items[0].push.items[0] as any).latest_attempt=null;expect(isNotificationPage(pending,U1)).toBe(true);
    const skip=page();const d=skip.items[0].push.items[0];(d as any).final_state=null;(d as any).decided_at=null;
    d.latest_attempt.observation.class='internal';d.latest_attempt.observation.outcome='notice_ahead_skipped';expect(isNotificationPage(skip,U1)).toBe(true);
  });
  it('paginates using exact server cursors and starts refresh at page zero',async()=>{
    api.list.mockResolvedValueOnce(page(U1,[row()],'s1-cursor')).mockResolvedValueOnce(page(U1,[row(S2)]));
    const s=useUserNotificationsStore();await s.select(U1);await s.next();expect(api.list).toHaveBeenLastCalledWith(U1,'s1-cursor',50);expect(s.pageIndex).toBe(1);
    await s.prev();expect(api.list).toHaveBeenLastCalledWith(U1,null,50);await s.refresh();expect(s.pageIndex).toBe(0);
  });
  it('drops an old user response without stranding the new loader',async()=>{
    const old=deferred();api.list.mockReturnValueOnce(old.promise).mockResolvedValueOnce(page(U2,[row(S2)]));const s=useUserNotificationsStore();
    const p=s.select(U1);await s.select(U2);old.resolve(page());await p;expect(s.user?.id).toBe(U2);expect(s.items[0].id).toBe(S2);expect(s.loading).toBe(false);
  });
  it.each([401,403])('current %s clears shown data and invalidates an older read in flight',async code=>{
    const s=useUserNotificationsStore();await s.select(U1);const old=deferred(),current=deferred();api.list.mockReturnValueOnce(old.promise).mockReturnValueOnce(current.promise);
    const a=s.refresh(),b=s.refresh();current.reject(new AdminApiError(code,'UNAUTHENTICATED','auth'));await b;
    old.resolve(page());await a;expect(s.items).toEqual([]);expect(s.user).toBeNull();expect(s.loading).toBe(false);expect(s.error).toContain('会话已失效');expect(api.list).toHaveBeenCalledTimes(3);
  });
  it('logout, a replacement admin and reset invalidate delayed reads',async()=>{
    const old=deferred();api.list.mockReturnValueOnce(old.promise);const s=useUserNotificationsStore();const p=s.select(U1);useAdminSessionStore().$patch({authenticated:false});
    old.resolve(page());await p;expect(s.userId).toBeNull();expect(s.items).toEqual([]);
    login('admin-2');api.list.mockResolvedValue(page(U2));await s.select(U2);s.reset();expect(s.user).toBeNull();expect(s.loading).toBe(false);
  });
  it('distinguishes an empty known user, unknown user and malformed response',async()=>{
    const s=useUserNotificationsStore();api.list.mockResolvedValueOnce(page(U1,[]));await s.select(U1);expect(s.items).toEqual([]);expect(s.error).toBe('');
    api.list.mockRejectedValueOnce(new AdminApiError(404,'NOT_FOUND','missing'));await s.refresh();expect(s.notFound).toBe(true);
    api.list.mockResolvedValueOnce(page(U2));await s.refresh();expect(s.items).toEqual([]);expect(s.notFound).toBe(false);expect(s.error).toContain('读取失败');
  });
  it('shows provider acceptance without promising device display, with no send or read mutation controls',async()=>{
    const data=page();data.user.status='disabled';api.list.mockResolvedValue(data);route.value.params={userId:U1};
    const w=mount(View);await flushPromises();expect(w.text()).toContain('已禁用');expect(w.text()).toContain('推送服务已接收');expect(w.text()).toContain('推送服务接收不代表设备已展示');
    expect(w.findAll('button').map(b=>b.text()).some(label=>['发送','重试推送','标为已读'].includes(label))).toBe(false);
    w.unmount();expect(useUserNotificationsStore().userId).toBeNull();
  });
});
