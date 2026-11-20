import {adminHttp} from './admin';
export interface Space {id:string;status:'active'|'archived'}
export interface Product {id:string;creator_id:string;owner_id:string;version:number;created_at:string;updated_at:string;title:string;price_flowers:string;redemption_mode:'direct'|'request';status:'active'|'archived'}
export interface ProductPage {space:Space;items:Product[];next_cursor:string|null;read_at:string}
export interface ProductDetail {space:Space;product:Product&{description:string};read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const exact=(v:unknown,keys:string[]):v is Record<string,unknown>=>obj(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown):v is string=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const integer=(v:unknown):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>=1&&v<=2147483647;
const space=(v:unknown,id:string):v is Space=>exact(v,['id','status'])&&v.id===id&&['active','archived'].includes(v.status as string);
const fields=['id','creator_id','owner_id','version','title','price_flowers','redemption_mode','status','created_at','updated_at'];
function product(v:unknown,description=false):v is ProductDetail['product'] {return exact(v,description?[...fields,'description']:fields)&&uuid(v.id)&&uuid(v.creator_id)&&uuid(v.owner_id)&&v.creator_id!==v.owner_id&&integer(v.version)&&time(v.created_at)&&time(v.updated_at)&&typeof v.title==='string'&&v.title.length>0&&typeof v.price_flowers==='string'&&/^[1-9]\d{0,2}$/.test(v.price_flowers)&&['direct','request'].includes(v.redemption_mode as string)&&['active','archived'].includes(v.status as string)&&(!description||typeof v.description==='string');}
function cursor(v:unknown,couple:string,rows:Product[]):boolean {if(v===null)return true;if(typeof v!=='string'||v.length>160||!rows.length)return false;const p=v.split('.');return p.length===4&&p[0]==='rp1'&&/^[1-9]\d{0,18}$/.test(p[1]??'')&&BigInt(p[1])<=9223372036854775807n&&p[2]===rows.at(-1)?.id&&p[3]===couple;}
export function isProductPage(v:unknown,couple:string):v is ProductPage {return exact(v,['space','items','next_cursor','read_at'])&&space(v.space,couple)&&time(v.read_at)&&Array.isArray(v.items)&&v.items.length<=100&&v.items.every(a=>product(a))&&new Set(v.items.map(a=>a.id)).size===v.items.length&&cursor(v.next_cursor,couple,v.items);}
export function isProductDetail(v:unknown,couple:string,id:string):v is ProductDetail {return exact(v,['space','product','read_at'])&&space(v.space,couple)&&time(v.read_at)&&product(v.product,true)&&v.product.id===id;}
const root=(couple:string)=>`/couples/${encodeURIComponent(couple)}/products`;
export const productAdminApi={list(couple:string,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(root(couple),{query:{cursor:cursor??undefined,limit}});},detail(couple:string,id:string):Promise<unknown>{return adminHttp.request(`${root(couple)}/${encodeURIComponent(id)}`);}};
