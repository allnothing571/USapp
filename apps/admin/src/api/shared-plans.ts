import {adminHttp} from './admin';
import {isDecimal} from './wallet';
export const planStatuses=['want','plan','finished'] as const;
export type PlanStatus=(typeof planStatuses)[number];
export const statusLabels:Record<PlanStatus,string>={want:'想做',plan:'计划中',finished:'已完成'};
export const operationLabels:Record<string,string>={create:'创建',edit:'直接编辑',propose:'提出修改',decide:'决定修改请求',advance:'推进状态'};
export const outcomeLabels:Record<string,string>={approved:'已同意',rejected:'已拒绝',withdrawn:'已撤回'};
export interface PlanBody {title:string;date:string|null;time:string|null;place:string|null;note:string|null}
export interface SharedPlanSummary {id:string;creator_id:string;created_at:string;title:string;body_version:number;status:PlanStatus;status_version:number}
export interface PlanEvent {id:string;seq:number;actor_id:string;operation:string;body_version:number|null;status_version:number|null;status:PlanStatus|null;proposal_id:string|null;outcome:string|null;created_at:string;body:PlanBody|null}
export interface SharedPlanSpace {id:string;status:'active'|'archived'}
export interface SharedPlanPage {space:SharedPlanSpace;items:SharedPlanSummary[];next_cursor:string|null;read_at:string}
export interface SharedPlanDetail {space:SharedPlanSpace;plan:SharedPlanSummary&{body:PlanBody};events:{total:string;items:PlanEvent[]};read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown)=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const integer=(v:unknown):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>0;
const optInteger=(v:unknown)=>v===null||integer(v);
function body(v:unknown):v is PlanBody {return obj(v)&&typeof v.title==='string'&&(v.date===null||(typeof v.date==='string'&&/^\d{4}-\d{2}-\d{2}$/.test(v.date)))&&
  (v.time===null||(typeof v.time==='string'&&/^\d{2}:\d{2}$/.test(v.time)))&&(v.place===null||typeof v.place==='string')&&(v.note===null||typeof v.note==='string');}
function space(v:unknown,id:string):v is SharedPlanSpace{return obj(v)&&v.id===id&&['active','archived'].includes(v.status as string);}
export function isSharedPlanSummary(v:unknown):v is SharedPlanSummary{return obj(v)&&uuid(v.id)&&uuid(v.creator_id)&&time(v.created_at)&&typeof v.title==='string'&&integer(v.body_version)&&integer(v.status_version)&&(planStatuses as readonly unknown[]).includes(v.status);}
function event(v:unknown):v is PlanEvent {return obj(v)&&uuid(v.id)&&integer(v.seq)&&uuid(v.actor_id)&&typeof v.operation==='string'&&Object.hasOwn(operationLabels,v.operation)&&
  optInteger(v.body_version)&&optInteger(v.status_version)&&(v.status===null||(planStatuses as readonly unknown[]).includes(v.status))&&(v.proposal_id===null||uuid(v.proposal_id))&&
  (v.outcome===null||(typeof v.outcome==='string'&&Object.hasOwn(outcomeLabels,v.outcome)))&&time(v.created_at)&&(v.body===null||body(v.body));}
export function isSharedPlanPage(v:unknown,couple:string):v is SharedPlanPage{return obj(v)&&space(v.space,couple)&&time(v.read_at)&&Array.isArray(v.items)&&v.items.length<=100&&v.items.every(isSharedPlanSummary)&&(v.next_cursor===null||(typeof v.next_cursor==='string'&&v.next_cursor.length<=160));}
export function isSharedPlanDetail(v:unknown,couple:string,id:string):v is SharedPlanDetail {
  if(!obj(v)||!space(v.space,couple)||!time(v.read_at)||!isSharedPlanSummary(v.plan)||v.plan.id!==id)return false;
  const p=v.plan as unknown as Record<string,unknown>,e=v.events;
  return body(p.body)&&p.body.title===p.title&&obj(e)&&isDecimal(e.total)&&!e.total.startsWith('-')&&Array.isArray(e.items)&&e.items.length<=50&&e.items.every(event)&&BigInt(e.total)>=BigInt(e.items.length);
}
export const sharedPlanAdminApi={
  list(couple:string,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(couple)}/shared-plans`,{query:{limit,cursor:cursor??undefined}});},
  detail(couple:string,id:string):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(couple)}/shared-plans/${encodeURIComponent(id)}`);},
};
