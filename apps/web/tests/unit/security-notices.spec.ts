import {beforeEach,describe,it,expect,vi} from 'vitest'
import {createPinia,setActivePinia} from 'pinia'
import {mount,flushPromises} from '@vue/test-utils'
const route=vi.hoisted(()=>({name:'space-create'}))
vi.mock('vue-router',async original=>({...await original<typeof import('vue-router')>(),useRoute:()=>route,useRouter:()=>({push:vi.fn()})}))
vi.mock('@/api/notifications',async original=>({...await original<typeof import('@/api/notifications')>(),getUnreadCount:vi.fn(),listNotifications:vi.fn(),readNotification:vi.fn()}))
import {hasValidActor,notificationLabel,notificationTarget,getUnreadCount,listNotifications,readNotification,type Notification} from '@/api/notifications'
import {parsePushPayload,notificationFor,destinationFor} from '@/pwa/push-sw'
import {useSessionStore} from '@/stores/session'
import {useNotificationsStore} from '@/stores/notifications'
import Entry from '@/components/NotificationEntry.vue'
import Inbox from '@/views/NotificationsView.vue'
const U='11111111-1111-4111-8111-111111111111',N='22222222-2222-4222-8222-222222222222',MARKER='m'.repeat(22)
const events=['security.wallet_adjusted','security.redemption_cancelled','security.session_created']
const row=(event_type=events[2]):Notification=>({id:N,event_type,actor_id:null,target_kind:'account',target_id:U,created_at:'2026-11-15T00:00:00Z',read_at:null})
function login(){useSessionStore().applyBootstrap({authenticated:true,csrf_token:'csrf',user:{id:U,email:'synthetic@example.com',display_name:'Test',created_at:''},space:null})}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();login();vi.mocked(getUnreadCount).mockResolvedValue({unread_count:1});vi.mocked(listNotifications).mockResolvedValue({notifications:[row()],next_cursor:null});vi.mocked(readNotification).mockResolvedValue({receipt:{notification_id:N,read_at:'2026-11-15T01:00:00Z'}})})
describe('mandatory personal security notices',()=>{
 for(const event of events)it(`fixed private label and actor rules: ${event}`,()=>{const n=row(event);expect(hasValidActor(n)).toBe(true);expect(hasValidActor({...n,actor_id:U})).toBe(false);expect(notificationLabel(n)).not.toBe('收到一条通知');expect(notificationTarget(n)).toBe('/notifications');expect(notificationLabel(n)).not.toContain('异地');expect(notificationLabel(n)).not.toContain('冻结')})
 it('does not make unknown actorless families valid',()=>{expect(hasValidActor({...row(),event_type:'security.arbitrary'})).toBe(false);expect(hasValidActor({...row(),event_type:'task.published'})).toBe(false)})
 it('refuses a personal target belonging to another account or using a content target',async()=>{const store=useNotificationsStore();for(const patch of [{target_id:N},{target_kind:'reward' as const}]){vi.mocked(listNotifications).mockResolvedValueOnce({notifications:[{...row(),...patch}],next_cursor:null});await store.load();expect(store.status).toBe('error');expect(store.items).toHaveLength(0)}})
 it('displays a discoverable entry while authenticated without a space',async()=>{const view=mount(Entry,{global:{stubs:{RouterLink:{template:'<a><slot/></a>',props:['to']}}}});await flushPromises();expect(view.find('nav[aria-label="站内通知"]').exists()).toBe(true);expect(getUnreadCount).toHaveBeenCalledTimes(1);expect(view.text()).toContain('未读 1');view.unmount()})
 it('loads and acknowledges personal notice without inventing a partner or space',async()=>{const view=mount(Inbox,{global:{stubs:{RouterLink:true}}});await flushPromises();expect(view.text()).toContain('你的账号有新的登录会话');expect(view.text()).toContain('确认已读');expect(useNotificationsStore().status).toBe('ready');expect(await useNotificationsStore().markRead(N)).toBe(true);expect(readNotification).toHaveBeenCalledWith(N);view.unmount()})
 for(const event of events)it(`push accepts only a fixed exact payload and opens the inbox: ${event}`,()=>{const payload={v:1,nid:N,event_type:event,target_kind:'account',target_id:U,marker:MARKER};const parsed=parsePushPayload(JSON.stringify(payload));expect(parsed).not.toBeNull();const plan=notificationFor(parsed,{generationId:'g',marker:MARKER});expect(plan.options.body).toBe(notificationLabel(row(event)));expect(destinationFor(plan.options.data,{generationId:'g',marker:MARKER})).toBe('/notifications');expect(parsePushPayload(JSON.stringify({...payload,reason:'Private operator reason'}))).toBeNull();expect(parsePushPayload(JSON.stringify({...payload,session_id:N}))).toBeNull()})
})
