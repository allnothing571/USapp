import {adminHttp} from './admin';
export type UserSessionState='valid'|'expired'|'revoked';
export interface UserSession {id:string;created_at:string;last_seen_at:string;expires_at:string;revoked_at:string|null;state:UserSessionState}
export interface UserSessionPage {user:{id:string;status:'active'|'disabled'};items:UserSession[];next_cursor:string|null;read_at:string}
const object=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const uuid=(v:unknown):v is string=>typeof v==='string'&&/^[a-f\d]{8}(?:-[a-f\d]{4}){3}-[a-f\d]{12}$/.test(v);
const time=(v:unknown)=>typeof v==='string'&&Number.isFinite(Date.parse(v));
export function isUserSession(v:unknown):v is UserSession {
  return object(v)&&uuid(v.id)&&time(v.created_at)&&time(v.last_seen_at)&&time(v.expires_at)&&
    (v.revoked_at===null||time(v.revoked_at))&&['valid','expired','revoked'].includes(v.state as string)&&
    ((v.state==='revoked')===(v.revoked_at!==null));
}
export function isUserSessionPage(v:unknown,userId:string):v is UserSessionPage {
  return object(v)&&object(v.user)&&v.user.id===userId&&['active','disabled'].includes(v.user.status as string)&&
    Array.isArray(v.items)&&v.items.length<=100&&v.items.every(isUserSession)&&time(v.read_at)&&
    (v.next_cursor===null||(typeof v.next_cursor==='string'&&v.next_cursor.length<=160));
}
export const userSessionApi={
  list(userId:string,cursor:string|null,limit=50):Promise<unknown>{
    return adminHttp.request(`/users/${encodeURIComponent(userId)}/sessions`,{query:{limit,cursor:cursor??undefined}});
  },
};
