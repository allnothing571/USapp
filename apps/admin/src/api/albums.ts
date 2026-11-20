import {adminHttp} from './admin';
export type AlbumView='current'|'history';
export interface Space {id:string;status:'active'|'archived'}
export interface Album {id:string;creator_id:string;created_at:string;name:string;version:number;photo_count:string;cover_media_id:string|null}
export interface AlbumPage {space:Space;availability:'available'|'unavailable';items:Album[];next_cursor:string|null;read_at:string}
export interface AlbumDetail {space:Space;album:Album;history_count:string;read_at:string}
export interface AlbumItem {seq:number;media_id:string;uploader_id:string;added_by:string;added_at:string;width:number|null;height:number|null;removed_by:string|null;removed_at:string|null}
export interface ItemPage {space:Space;album_id:string;view:AlbumView;items:AlbumItem[];next_cursor:string|null;read_at:string}
const obj=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const exact=(v:unknown,keys:string[]):v is Record<string,unknown>=>obj(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown):v is string=>typeof v==='string'&&Number.isFinite(Date.parse(v));
const integer=(v:unknown):v is number=>typeof v==='number'&&Number.isSafeInteger(v)&&v>=1&&v<=2147483647;
const count=(v:unknown):v is string=>typeof v==='string'&&/^(0|[1-9]\d{0,18})$/.test(v)&&BigInt(v)<=9223372036854775807n;
const space=(v:unknown,id:string):v is Space=>exact(v,['id','status'])&&v.id===id&&['active','archived'].includes(v.status as string);
export function isAlbum(v:unknown):v is Album{return exact(v,['id','creator_id','created_at','name','version','photo_count','cover_media_id'])&&uuid(v.id)&&uuid(v.creator_id)&&time(v.created_at)&&typeof v.name==='string'&&v.name.length>0&&integer(v.version)&&count(v.photo_count)&&(v.cover_media_id===null||(uuid(v.cover_media_id)&&v.photo_count!=='0'));}
function albumCursor(v:unknown,couple:string,items:Album[]):boolean {
 if(v===null)return true;if(typeof v!=='string'||v.length>160||!items.length)return false;
 const m=/^aa1\.([1-9]\d{0,18})\.([a-f\d-]{36})\.([a-f\d-]{36})$/.exec(v);
 return !!m&&BigInt(m[1])<=9223372036854775807n&&m[2]===items.at(-1)?.id&&m[3]===couple;
}
export function isAlbumPage(v:unknown,couple:string):v is AlbumPage{return exact(v,['space','availability','items','next_cursor','read_at'])&&space(v.space,couple)&&['available','unavailable'].includes(v.availability as string)&&time(v.read_at)&&Array.isArray(v.items)&&v.items.length<=100&&v.items.every(isAlbum)&&new Set(v.items.map(i=>i.id)).size===v.items.length&&albumCursor(v.next_cursor,couple,v.items)&&(v.availability!=='unavailable'||(v.items.length===0&&v.next_cursor===null));}
export function isAlbumDetail(v:unknown,couple:string,id:string):v is AlbumDetail{return exact(v,['space','album','history_count','read_at'])&&space(v.space,couple)&&isAlbum(v.album)&&v.album.id===id&&count(v.history_count)&&BigInt(v.history_count)>=BigInt(v.album.photo_count)&&time(v.read_at);}
export function isAlbumItem(v:unknown):v is AlbumItem{return exact(v,['seq','media_id','uploader_id','added_by','added_at','width','height','removed_by','removed_at'])&&integer(v.seq)&&uuid(v.media_id)&&uuid(v.uploader_id)&&uuid(v.added_by)&&time(v.added_at)&&((v.width===null&&v.height===null)||(integer(v.width)&&integer(v.height)))&&((v.removed_by===null&&v.removed_at===null)||(uuid(v.removed_by)&&time(v.removed_at)));}
export function isItemPage(v:unknown,couple:string,album:string,view:AlbumView):v is ItemPage {
 if(!exact(v,['space','album_id','view','items','next_cursor','read_at'])||!space(v.space,couple)||v.album_id!==album||v.view!==view||!time(v.read_at)||!Array.isArray(v.items)||v.items.length>100||!v.items.every(isAlbumItem))return false;
 const items=v.items as AlbumItem[];
 if(items.some((p,i)=>(i>0&&p.seq>=items[i-1].seq)||(view==='current'&&p.removed_at!==null))||new Set(items.map(i=>i.media_id)).size!==items.length)return false;
 return v.next_cursor===null||(items.length>0&&v.next_cursor===`ai1.${view}.${items.at(-1)?.seq}.${album}.${couple}`);
}
export const albumAdminApi={
 list(couple:string,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(couple)}/albums`,{query:{cursor:cursor??undefined,limit}});},
 detail(couple:string,album:string):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(couple)}/albums/${encodeURIComponent(album)}`);},
 items(couple:string,album:string,view:AlbumView,cursor:string|null,limit=50):Promise<unknown>{return adminHttp.request(`/couples/${encodeURIComponent(couple)}/albums/${encodeURIComponent(album)}/items`,{query:{view,cursor:cursor??undefined,limit}});},
};
