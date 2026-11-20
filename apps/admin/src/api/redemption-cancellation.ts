import {adminHttp} from './admin';
import {seq} from './wallet-adjustment';
import type {RedemptionSummary} from './redemptions';
export interface CancellationCommand {expected_status:'pending_confirmation'|'pending_fulfillment';reason:string}
export interface CancellationCapture {adminId:string;coupleId:string;redemptionId:string;redeemerId:string;price:string;key:string;command:CancellationCommand}
export interface CancellationReceipt {id:string;admin_id:string;couple_id:string;redemption_id:string;redeemer_id:string;previous_status:string;refunded_flowers:string;refund_operation_id:string|null;refund_seq:string|null;balance_before:string|null;balance_after:string|null;reason:string;created_at:string}
export const cancellable=(r:RedemptionSummary)=>r.status==='pending_confirmation'||r.status==='pending_fulfillment';
export const cancellationApi={send(c:CancellationCapture):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(c.coupleId)}/redemptions/${encodeURIComponent(c.redemptionId)}/cancellations`,{method:'POST',headers:{'Idempotency-Key':c.key},body:c.command});}};
const object=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const exact=(v:unknown,keys:string[]):v is Record<string,unknown>=>object(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const uuid=(v:unknown)=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
export function parseCancellationReceipt(v:unknown,c:CancellationCapture):CancellationReceipt|null {
 if(!/^[1-9]\d{0,2}$/.test(c.price))return null;
 if(!exact(v,['cancellation']))return null;const r=v.cancellation;
 if(!exact(r,['id','admin_id','couple_id','redemption_id','redeemer_id','previous_status','refunded_flowers','refund_operation_id','refund_seq','balance_before','balance_after','reason','created_at'])||!uuid(r.id)||r.admin_id!==c.adminId||r.couple_id!==c.coupleId||r.redemption_id!==c.redemptionId||r.redeemer_id!==c.redeemerId||r.previous_status!==c.command.expected_status||r.reason!==c.command.reason||typeof r.created_at!=='string'||!Number.isFinite(Date.parse(r.created_at)))return null;
 const charged=c.command.expected_status==='pending_fulfillment';
 if(r.refunded_flowers!==(charged?c.price:'0'))return null;
 if(charged){
  if(!uuid(r.refund_operation_id)||!seq(r.refund_seq)||r.refund_seq==='0'||!seq(r.balance_before)||!seq(r.balance_after)||BigInt(r.balance_after)!==BigInt(r.balance_before)+BigInt(c.price)||BigInt(r.balance_after)>1000000n)return null;
 }else if(r.refund_operation_id!==null||r.refund_seq!==null||r.balance_before!==null||r.balance_after!==null)return null;
 return r as unknown as CancellationReceipt;
}
