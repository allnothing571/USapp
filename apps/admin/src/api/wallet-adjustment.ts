import {adminHttp} from './admin';
import type {Asset} from './wallet';
export interface AdjustmentCommand {asset:Asset;delta:string;expected_seq:string;reason:string}
export interface AdjustmentCapture {adminId:string;userId:string;coupleId:string;balance:string;key:string;command:AdjustmentCommand}
export interface AdjustmentReceipt {id:string;admin_id:string;user_id:string;couple_id:string;asset:Asset;delta:string;expected_seq:string;balance_before:string;balance_after:string;operation_count:number;last_seq:string;reason:string;created_at:string}
export const adjustmentApi={send(c:AdjustmentCapture):Promise<unknown>{return adminHttp.request(`/users/${encodeURIComponent(c.userId)}/wallet/adjustments`,{method:'POST',headers:{'Idempotency-Key':c.key},body:c.command});}};
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const exact=(v:unknown,keys:string[]):v is Record<string,unknown>=>obj(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const uuid=(v:unknown)=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
export const seq=(s:unknown):s is string=>typeof s==='string'&&/^(0|[1-9]\d{0,18})$/.test(s)&&BigInt(s)<=9223372036854775807n;
export const validDelta=(s:string)=>/^-?[1-9]\d{0,3}$/.test(s);
export function parseAdjustmentReceipt(v:unknown,c:AdjustmentCapture):AdjustmentReceipt|null {
 if(!exact(v,['adjustment']))return null;const r=v.adjustment;
 if(!exact(r,['id','admin_id','user_id','couple_id','asset','delta','expected_seq','balance_before','balance_after','operation_count','last_seq','reason','created_at'])||!uuid(r.id)||r.admin_id!==c.adminId||r.user_id!==c.userId||r.couple_id!==c.coupleId||r.asset!==c.command.asset||r.delta!==c.command.delta||r.expected_seq!==c.command.expected_seq||r.reason!==c.command.reason||r.balance_before!==c.balance||!seq(r.balance_after)||!seq(r.last_seq)||typeof r.created_at!=='string'||!Number.isFinite(Date.parse(r.created_at))||typeof r.operation_count!=='number'||!Number.isInteger(r.operation_count)||r.operation_count<1||r.operation_count>11)return null;
 const delta=BigInt(c.command.delta),abs=delta<0n?-delta:delta,n=c.command.asset==='flower'?(abs+998n)/999n:1n;
 if(BigInt(r.operation_count)!==n||BigInt(r.last_seq)!==BigInt(c.command.expected_seq)+n||BigInt(r.balance_after)!==BigInt(c.balance)+delta)return null;
 return r as unknown as AdjustmentReceipt;
}
