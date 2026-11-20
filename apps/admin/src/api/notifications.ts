import {adminHttp} from './admin';
import {isDecimal} from './wallet';
export const providerOutcomes=['accepted','gone','rejected','transient','unknown','provider_auth_failed'] as const;
export const internalOutcomes=['worker_auth_failed','transport_unavailable','busy','inactive_skipped','ttl_skipped','cutover_skipped','control_skipped','terminal_skipped','lease_lost_skipped','source_stale_skipped','notice_ahead_skipped'] as const;
export const finalStates=['accepted','gone','rejected','inactive','expired','exhausted','cutover_skipped','source_stale'] as const;
export const outcomeLabels:Record<string,string>={accepted:'推送服务已接收',gone:'订阅已失效',rejected:'推送服务拒绝',transient:'暂时失败',unknown:'结果未知',provider_auth_failed:'推送服务认证失败',worker_auth_failed:'工作进程认证失败',transport_unavailable:'传输不可用',busy:'内部繁忙',inactive_skipped:'订阅不活跃，已跳过',ttl_skipped:'有效期检查跳过',cutover_skipped:'启用边界检查跳过',control_skipped:'控制状态检查跳过',terminal_skipped:'已有最终记录，跳过',lease_lost_skipped:'租约丢失，跳过',source_stale_skipped:'来源已失效，跳过',notice_ahead_skipped:'通知时间领先，暂缓',inactive:'订阅不活跃',expired:'已到期',exhausted:'尝试预算已用尽',source_stale:'来源已失效'};
export interface PushObservation {class:'provider'|'internal';outcome:string;http_status:number|null;retry_after_s:number|null;observed_at:string}
export interface PushAttempt {attempt_no:number;claimed_at:string;lease_until:string;expired_at:string|null;observation:PushObservation|null}
export interface PushDelivery {generation_id:string;created_at:string;final_state:string|null;decided_at:string|null;latest_attempt:PushAttempt|null}
export interface AdminNotification {id:string;event_type:string;created_at:string;read_at:string|null;push:{total:string;items:PushDelivery[]}}
export interface NotificationPage {user:{id:string;status:'active'|'disabled'};items:AdminNotification[];next_cursor:string|null;read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown)=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const optTime=(v:unknown)=>v===null||time(v);
const integer=(v:unknown,min:number,max=Number.MAX_SAFE_INTEGER):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>=min&&v<=max;
function observation(v:unknown):v is PushObservation {
  return obj(v)&&time(v.observed_at)&&typeof v.outcome==='string'&&
    ((v.class==='provider'&&(providerOutcomes as readonly string[]).includes(v.outcome))||(v.class==='internal'&&(internalOutcomes as readonly string[]).includes(v.outcome)))&&
    (v.http_status===null||integer(v.http_status,100,599))&&(v.retry_after_s===null||integer(v.retry_after_s,0,86400));
}
function attempt(v:unknown):v is PushAttempt {return obj(v)&&integer(v.attempt_no,1)&&time(v.claimed_at)&&time(v.lease_until)&&optTime(v.expired_at)&&(v.observation===null||observation(v.observation));}
function delivery(v:unknown):v is PushDelivery {return obj(v)&&uuid(v.generation_id)&&time(v.created_at)&&optTime(v.decided_at)&&
  (v.final_state===null||(typeof v.final_state==='string'&&(finalStates as readonly string[]).includes(v.final_state)))&&
  ((v.final_state===null)===(v.decided_at===null))&&(v.latest_attempt===null||attempt(v.latest_attempt));}
export function isAdminNotification(v:unknown):v is AdminNotification {
  if(!obj(v)||!uuid(v.id)||typeof v.event_type!=='string'||!time(v.created_at)||!optTime(v.read_at)||!obj(v.push))return false;
  const p=v.push;
  return isDecimal(p.total)&&!p.total.startsWith('-')&&Array.isArray(p.items)&&p.items.length<=5&&p.items.every(delivery)&&BigInt(p.total)>=BigInt(p.items.length);
}
export function isNotificationPage(v:unknown,id:string):v is NotificationPage {return obj(v)&&obj(v.user)&&v.user.id===id&&['active','disabled'].includes(v.user.status as string)&&
  Array.isArray(v.items)&&v.items.length<=100&&v.items.every(isAdminNotification)&&time(v.read_at)&&(v.next_cursor===null||(typeof v.next_cursor==='string'&&v.next_cursor.length<=160));}
export const notificationAdminApi={list(id:string,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(`/users/${encodeURIComponent(id)}/notifications`,{query:{limit,cursor:cursor??undefined}});}};
