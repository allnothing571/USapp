import {adminHttp} from './admin';
export interface Space {id:string;status:'active'|'archived'}
export interface Terms {title:string;date:string;kind:'anniversary'|'countdown';note:string}
export interface Anniversary extends Omit<Terms,'note'> {id:string;creator_id:string;version:number;created_at:string;updated_at:string}
export interface Pending extends Terms {id:string;proposer_id:string;base_version:number;created_at:string}
export interface Decision {proposal_id:string;proposer_id:string;base_version:number;outcome:'approved'|'rejected'|'withdrawn';decided_by:string;decided_at:string;approved_version:number|null}
export interface AnniversaryPage {space:Space;items:Anniversary[];next_cursor:string|null;read_at:string}
export interface AnniversaryDetail {space:Space;anniversary:Anniversary&{note:string};pending_proposal:Pending|null;last_decision:Decision|null;read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const exact=(v:unknown,keys:string[]):v is Record<string,unknown>=>obj(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown):v is string=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const integer=(v:unknown):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>=1&&v<=2147483647;
const space=(v:unknown,id:string):v is Space=>exact(v,['id','status'])&&v.id===id&&['active','archived'].includes(v.status as string);
const date=(v:unknown):v is string=>typeof v==='string'&&/^\d{4}-\d{2}-\d{2}$/.test(v)&&v>='1900-01-01'&&v<='2199-12-31'&&Number.isFinite(Date.parse(v))&&new Date(v).toISOString().slice(0,10)===v;
const terms=(v:Record<string,unknown>,note=false)=>typeof v.title==='string'&&[...v.title].length>=1&&[...v.title].length<=40&&date(v.date)&&['anniversary','countdown'].includes(v.kind as string)&&(!note||(typeof v.note==='string'&&[...v.note].length<=500));
const fields=['id','creator_id','version','title','date','kind','created_at','updated_at'];
function anniversary(v:unknown,note=false):v is AnniversaryDetail['anniversary'] {return exact(v,note?[...fields,'note']:fields)&&uuid(v.id)&&uuid(v.creator_id)&&integer(v.version)&&time(v.created_at)&&time(v.updated_at)&&terms(v,note);}
function cursor(v:unknown,couple:string,rows:Anniversary[]):boolean {if(v===null)return true;if(typeof v!=='string'||v.length>160||!rows.length)return false;const p=v.split('.');return p.length===4&&p[0]==='an1'&&/^[1-9]\d{0,18}$/.test(p[1]??'')&&BigInt(p[1])<=9223372036854775807n&&p[2]===rows.at(-1)?.id&&p[3]===couple;}
export function isAnniversaryPage(v:unknown,couple:string):v is AnniversaryPage {return exact(v,['space','items','next_cursor','read_at'])&&space(v.space,couple)&&time(v.read_at)&&Array.isArray(v.items)&&v.items.length<=100&&v.items.every(a=>anniversary(a))&&new Set(v.items.map(a=>a.id)).size===v.items.length&&cursor(v.next_cursor,couple,v.items);}
export function isAnniversaryDetail(v:unknown,couple:string,id:string):v is AnniversaryDetail {
 if(!exact(v,['space','anniversary','pending_proposal','last_decision','read_at'])||!space(v.space,couple)||!time(v.read_at)||!anniversary(v.anniversary,true)||v.anniversary.id!==id)return false;
 const version=v.anniversary.version,p=v.pending_proposal,d=v.last_decision;
 if(p!==null&&!(exact(p,['id','proposer_id','base_version','title','date','kind','note','created_at'])&&uuid(p.id)&&uuid(p.proposer_id)&&p.base_version===version&&time(p.created_at)&&terms(p,true)))return false;
 if(d!==null&&!(exact(d,['proposal_id','proposer_id','base_version','outcome','decided_by','decided_at','approved_version'])&&uuid(d.proposal_id)&&uuid(d.proposer_id)&&integer(d.base_version)&&d.base_version<=version&&uuid(d.decided_by)&&time(d.decided_at)&&['approved','rejected','withdrawn'].includes(d.outcome as string)&&(d.outcome==='approved'?(d.approved_version===d.base_version+1&&d.approved_version<=version):d.approved_version===null)))return false;
 return !(p!==null&&d!==null&&obj(p)&&obj(d)&&p.id===d.proposal_id);
}
export function decisionText(d:Decision,current:number):string {if(d.outcome==='approved')return d.approved_version===current?`已批准为版本 ${d.approved_version}（此详情快照的当前版本）`:`历史批准为版本 ${d.approved_version}（此详情快照已到版本 ${current}）`;return d.outcome==='rejected'?'已拒绝':'已撤回';}
const root=(couple:string)=>`/couples/${encodeURIComponent(couple)}/anniversaries`;
export const anniversaryAdminApi={list(couple:string,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(root(couple),{query:{cursor:cursor??undefined,limit}});},detail(couple:string,id:string):Promise<unknown>{return adminHttp.request(`${root(couple)}/${encodeURIComponent(id)}`);}};
