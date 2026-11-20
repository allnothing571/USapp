import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {AdminApiError} from '@/api/client';
import {userSessionApi,isUserSessionPage,type UserSessionPage,type UserSession} from '@/api/user-sessions';
import {useAdminSessionStore} from './admin-session';

// Read-only, memory-only; every page belongs to the current admin session and selected user.
export const useUserSessionsStore=defineStore('user-sessions',()=>{
  const session=useAdminSessionStore();
  const userId=ref<string|null>(null),user=ref<UserSessionPage['user']|null>(null),items=ref<UserSession[]>([]);
  const loading=ref(false),error=ref(''),readAt=ref<string|null>(null),notFound=ref(false);
  const pageIndex=ref(0),nextCursor=ref<string|null>(null),cursors=ref<Array<string|null>>([null]);
  let generation=0,sequence=0;
  const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
  function clear(){user.value=null;items.value=[];readAt.value=null;pageIndex.value=0;nextCursor.value=null;cursors.value=[null];}
  function reset(){generation++;sequence++;userId.value=null;clear();loading.value=false;error.value='';notFound.value=false;}
  watch(owner,reset,{flush:'sync'});
  async function load(index:number,cursor:string|null){
    const id=userId.value,identity=owner(),g=generation,s=++sequence;
    if(!id||!identity)return;
    const current=()=>g===generation&&s===sequence&&id===userId.value&&identity===owner();
    loading.value=true;error.value='';notFound.value=false;
    try{
      const response=await userSessionApi.list(id,cursor,50);
      if(!current())return;
      if(!isUserSessionPage(response,id))throw new Error('shape');
      user.value=response.user;items.value=response.items;readAt.value=response.read_at;
      pageIndex.value=index;cursors.value=[...cursors.value.slice(0,index),cursor];nextCursor.value=response.next_cursor;
    }catch(e){
      if(!current())return;
      clear();
      if(e instanceof AdminApiError&&(e.status===401||e.status===403)){
        generation++;sequence++;loading.value=false;
        error.value='管理员会话已失效或无权访问，已清除显示的数据。请重新登录后再读取。';return;
      }
      if(e instanceof AdminApiError&&e.status===404){notFound.value=true;error.value='用户不存在。';}
      else if(e instanceof AdminApiError&&e.status===400)error.value='用户或游标无效，请刷新后重试。';
      else error.value='会话列表读取失败，可点击重试。';
    }finally{if(current())loading.value=false;}
  }
  async function select(id:string){if(!owner())return;if(id===userId.value&&(user.value||loading.value))return;reset();userId.value=id;await load(0,null);}
  async function refresh(){if(!owner()||!userId.value)return;generation++;await load(0,null);}
  async function next(){if(loading.value||!nextCursor.value)return;await load(pageIndex.value+1,nextCursor.value);}
  async function prev(){if(loading.value||pageIndex.value===0)return;await load(pageIndex.value-1,cursors.value[pageIndex.value-1]??null);}
  const hasPrev=computed(()=>pageIndex.value>0),hasNext=computed(()=>nextCursor.value!==null);
  return{userId,user,items,loading,error,readAt,notFound,pageIndex,hasPrev,hasNext,select,refresh,next,prev,reset};
});
