import { adminHttp } from './admin';
import { isDecimal } from './wallet';
export const statuses = ['pending_confirmation','pending_fulfillment','fulfilled','cancelled'] as const;
export type Status = (typeof statuses)[number];
export type StatusFilter = Status | '';
export const statusLabels: Record<Status,string> = { pending_confirmation:'待确认', pending_fulfillment:'待履约', fulfilled:'已履约', cancelled:'已取消' };
export const actionLabels: Record<string,string> = {confirm:'确认兑换',reject:'拒绝申请',withdraw:'撤回申请',cancel_propose:'提出取消',cancel_approve:'同意取消并退款',cancel_reject:'拒绝取消',cancel_withdraw:'撤回取消'};
export interface RedemptionSpace { id:string; status:string }
export interface RedemptionSummary {
  id:string; product_id:string; product_version:number; creator_id:string; redeemer_id:string;
  title:string; price_flowers:string; redemption_mode:'direct'|'request'; status:Status; created_at:string;
  fulfilled_at:string|null; fulfilled_by:string|null; cancel_reason:'rejected'|'withdrawn'|'mutual'|'admin'|null;
  cancelled_at:string|null; cancellation_id:string|null; cancellation_by:string|null;
}
export interface RedemptionEvent {id:string; actor_id:string; action:string; proposal_id:string|null; created_at:string}
export interface AdminCancellation {id:string;admin_id:string;reason:string;refunded_flowers:string;created_at:string}
export interface RedemptionDetail {admin_cancellation?:AdminCancellation|null; space:RedemptionSpace; redemption:RedemptionSummary & {description:string}; events:{total:number;items:RedemptionEvent[]}; read_at:string }
export interface RedemptionPage {space:RedemptionSpace; status:Status|null; items:RedemptionSummary[];next_cursor:string|null;read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const optId=(v:unknown)=>v===null||uuid(v);
const time=(v:unknown)=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const optTime=(v:unknown)=>v===null||time(v);
const count=(v:unknown,min:number):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>=min;
const space=(v:unknown,id:string)=>obj(v)&&v.id===id&&(v.status==='active'||v.status==='archived');
export function isRedemptionSummary(v:unknown):v is RedemptionSummary {
  return obj(v)&&uuid(v.id)&&uuid(v.product_id)&&count(v.product_version,1)&&uuid(v.creator_id)&&uuid(v.redeemer_id)&&
    typeof v.title==='string'&&isDecimal(v.price_flowers)&&!v.price_flowers.startsWith('-')&&
    (v.redemption_mode==='direct'||v.redemption_mode==='request')&&statuses.includes(v.status as Status)&&time(v.created_at)&&
    optTime(v.fulfilled_at)&&optId(v.fulfilled_by)&&optTime(v.cancelled_at)&&
    (v.cancel_reason===null||['rejected','withdrawn','mutual','admin'].includes(v.cancel_reason as string))&&optId(v.cancellation_id)&&optId(v.cancellation_by);
}
export function isRedemptionPage(v:unknown,couple:string,status:StatusFilter):v is RedemptionPage {
  return obj(v)&&space(v.space,couple)&&v.status===(status||null)&&time(v.read_at)&&Array.isArray(v.items)&&v.items.length<=100&&
    v.items.every(isRedemptionSummary)&&(status===''||v.items.every(i=>i.status===status))&&
    (v.next_cursor===null||(typeof v.next_cursor==='string'&&v.next_cursor.length<=160));
}
export function isRedemptionDetail(v:unknown,couple:string,id:string):v is RedemptionDetail {
  if(!obj(v)||!space(v.space,couple)||!time(v.read_at)||!isRedemptionSummary(v.redemption)||v.redemption.id!==id||
    typeof (v.redemption as unknown as Record<string,unknown>).description!=='string')return false;
  const ac=v.admin_cancellation;
  if(ac!==undefined&&ac!==null&&(!obj(ac)||!uuid(ac.id)||!uuid(ac.admin_id)||typeof ac.reason!=='string'||!isDecimal(ac.refunded_flowers)||!time(ac.created_at)))return false;
  const e=v.events;
  return obj(e)&&count(e.total,0)&&Array.isArray(e.items)&&e.items.length<=100&&e.total>=e.items.length&&e.items.every(x=>
    obj(x)&&uuid(x.id)&&uuid(x.actor_id)&&typeof x.action==='string'&&Object.hasOwn(actionLabels,x.action)&&optId(x.proposal_id)&&time(x.created_at));
}
export const redemptionAdminApi={
  list(couple:string,status:StatusFilter,cursor:string|null,limit=50,signal?:AbortSignal):Promise<unknown>{
    return adminHttp.request(`/couples/${encodeURIComponent(couple)}/redemptions`,{query:{status:status||undefined,limit,cursor:cursor??undefined},signal});
  },
  detail(couple:string,id:string,signal?:AbortSignal):Promise<unknown>{
    return adminHttp.request(`/couples/${encodeURIComponent(couple)}/redemptions/${encodeURIComponent(id)}`,{signal});
  },
};
