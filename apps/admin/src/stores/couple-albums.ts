import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {useAdminSessionStore} from './admin-session';
import {AdminApiError} from '@/api/client';
import {albumAdminApi as api,isAlbumPage,isAlbumDetail,isItemPage,type Album,type AlbumDetail,type AlbumView,type ItemPage,type Space} from '@/api/albums';
export const useCoupleAlbumsStore=defineStore('couple-albums',()=>{
 const session=useAdminSessionStore();
 const coupleId=ref<string|null>(null),albumId=ref<string|null>(null),space=ref<Space|null>(null);
 const availability=ref<'available'|'unavailable'>('available'),albums=ref<Album[]>([]),detail=ref<AlbumDetail|null>(null),photos=ref<ItemPage|null>(null),view=ref<AlbumView>('current');
 const listLoading=ref(false),detailLoading=ref(false),itemsLoading=ref(false),listError=ref(''),detailError=ref(''),itemsError=ref('');
 const pageIndex=ref(0),itemPageIndex=ref(0),nextCursor=ref<string|null>(null),itemNextCursor=ref<string|null>(null);
 let cursors:(string|null)[]=[null],itemCursors:(string|null)[]=[null];
 let root=0,target=0,itemGeneration=0,listSeq=0,detailSeq=0,itemSeq=0;
 const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
 const token=()=>({owner:owner(),root,couple:coupleId.value,album:albumId.value,target,view:view.value,itemGeneration});
 type Token=ReturnType<typeof token>;
 const base=(t:Token)=>!!t.owner&&t.owner===owner()&&t.root===root&&t.couple===coupleId.value;
 const selected=(t:Token)=>base(t)&&t.album===albumId.value&&t.target===target;
 const photoCurrent=(t:Token)=>selected(t)&&t.view===view.value&&t.itemGeneration===itemGeneration;
 function clearList(){space.value=null;albums.value=[];nextCursor.value=null;pageIndex.value=0;cursors=[null];}
 function clearItems(){photos.value=null;itemNextCursor.value=null;itemPageIndex.value=0;itemCursors=[null];}
 function invalidate(){root++;target++;itemGeneration++;listSeq++;detailSeq++;itemSeq++;listLoading.value=detailLoading.value=itemsLoading.value=false;clearList();detail.value=null;clearItems();}
 function reset(){invalidate();coupleId.value=albumId.value=null;space.value=null;availability.value='available';view.value='current';listError.value=detailError.value=itemsError.value='';}
 watch(owner,reset,{flush:'sync'});
 const auth=(e:unknown)=>e instanceof AdminApiError&&(e.status===401||e.status===403);
 function authLost(){invalidate();listError.value=detailError.value=itemsError.value='管理员会话已失效或无权访问，已清除显示的数据。请重新登录。';}
 function gone(){invalidate();listError.value='相册信息已变化，请刷新列表。';detailError.value='相册不存在或当前不可用。';itemsError.value='';}
 async function loadList(index=0,cursor:string|null=null){
  if(!owner()||!coupleId.value)return;const t=token(),s=++listSeq;listLoading.value=true;listError.value='';
  try{const r=await api.list(t.couple!,cursor);if(!base(t)||s!==listSeq)return;if(!isAlbumPage(r,t.couple!))throw new Error('shape');
   space.value=r.space;availability.value=r.availability;albums.value=r.items;nextCursor.value=r.next_cursor;pageIndex.value=index;cursors=[...cursors.slice(0,index),cursor];
   if(r.availability==='unavailable'){target++;itemGeneration++;detailSeq++;itemSeq++;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value='相册内容当前不可用。';}
  }catch(e){if(!base(t)||s!==listSeq)return;if(auth(e)){authLost();return;}clearList();listError.value=e instanceof AdminApiError&&e.status===404?'该空间不存在。':'相册列表读取失败，请重试。';
   if(e instanceof AdminApiError&&e.status===404){target++;itemGeneration++;detailSeq++;itemSeq++;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value=listError.value;}
  }finally{if(base(t)&&s===listSeq)listLoading.value=false;}
 }
 async function loadDetail(){
  if(!owner()||!coupleId.value||!albumId.value)return;const t=token(),s=++detailSeq;detailLoading.value=true;detailError.value='';
  try{const r=await api.detail(t.couple!,t.album!);if(!selected(t)||s!==detailSeq)return;if(!isAlbumDetail(r,t.couple!,t.album!))throw new Error('shape');detail.value=r;
  }catch(e){if(!selected(t)||s!==detailSeq)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}detail.value=null;detailError.value='相册详情读取失败，请重试。';
  }finally{if(selected(t)&&s===detailSeq)detailLoading.value=false;}
 }
 async function loadItems(index=0,cursor:string|null=null){
  if(!owner()||!coupleId.value||!albumId.value)return;const t=token(),s=++itemSeq;itemsLoading.value=true;itemsError.value='';
  try{const r=await api.items(t.couple!,t.album!,t.view,cursor);if(!photoCurrent(t)||s!==itemSeq)return;if(!isItemPage(r,t.couple!,t.album!,t.view))throw new Error('shape');photos.value=r;itemNextCursor.value=r.next_cursor;itemPageIndex.value=index;itemCursors=[...itemCursors.slice(0,index),cursor];
  }catch(e){if(!photoCurrent(t)||s!==itemSeq)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}clearItems();itemsError.value='照片条目读取失败，请重试。';
  }finally{if(photoCurrent(t)&&s===itemSeq)itemsLoading.value=false;}
 }
 async function select(couple:string){if(!owner())return;if(couple===coupleId.value&&(space.value||listLoading.value))return;reset();coupleId.value=couple;await loadList();}
 async function openAlbum(album:string|null){if(!owner()||!coupleId.value)return;if(album===albumId.value&&(detail.value||detailLoading.value))return;target++;itemGeneration++;detailSeq++;itemSeq++;albumId.value=album;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value=itemsError.value='';view.value='current';if(album)await Promise.all([loadDetail(),loadItems()]);}
 async function setView(v:AlbumView){if(v===view.value)return;itemGeneration++;itemSeq++;view.value=v;clearItems();itemsLoading.value=false;await loadItems();}
 async function refreshList(){listSeq++;await loadList();}
 async function refreshDetail(){if(albumId.value)await loadDetail();}
 async function refreshItems(){itemGeneration++;itemSeq++;clearItems();await loadItems();}
 async function next(){if(nextCursor.value&&!listLoading.value)await loadList(pageIndex.value+1,nextCursor.value);}
 async function prev(){if(pageIndex.value&&!listLoading.value)await loadList(pageIndex.value-1,cursors[pageIndex.value-1]);}
 async function nextItems(){if(itemNextCursor.value&&!itemsLoading.value)await loadItems(itemPageIndex.value+1,itemNextCursor.value);}
 async function prevItems(){if(itemPageIndex.value&&!itemsLoading.value)await loadItems(itemPageIndex.value-1,itemCursors[itemPageIndex.value-1]);}
 const hasPrev=computed(()=>pageIndex.value>0),hasNext=computed(()=>nextCursor.value!==null),hasItemPrev=computed(()=>itemPageIndex.value>0),hasItemNext=computed(()=>itemNextCursor.value!==null);
 return {coupleId,albumId,space,availability,albums,detail,photos,view,listLoading,detailLoading,itemsLoading,listError,detailError,itemsError,pageIndex,itemPageIndex,hasPrev,hasNext,hasItemPrev,hasItemNext,select,openAlbum,setView,refreshList,refreshDetail,refreshItems,next,prev,nextItems,prevItems,reset};
});
