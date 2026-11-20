import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {useAdminSessionStore} from './admin-session';
import {AdminApiError} from '@/api/client';
import {memoryAdminApi as api,isMemoryPage,isMemoryDetail,isProposalPage,isComparison,type Memory,type MemoryDetail,type ProposalPage,type Comparison,type Space} from '@/api/memories';
export const useCoupleMemoriesStore=defineStore('couple-memories',()=>{
 const session=useAdminSessionStore();
 const coupleId=ref<string|null>(null),memoryId=ref<string|null>(null),space=ref<Space|null>(null);
 const availability=ref<'available'|'unavailable'>('available'),memories=ref<Memory[]>([]),detail=ref<MemoryDetail|null>(null),history=ref<ProposalPage|null>(null);
 const listLoading=ref(false),detailLoading=ref(false),itemsLoading=ref(false),listError=ref(''),detailError=ref(''),itemsError=ref('');
 const pageIndex=ref(0),itemPageIndex=ref(0),nextCursor=ref<string|null>(null),itemNextCursor=ref<string|null>(null);
 let cursors:(string|null)[]=[null],itemCursors:(string|null)[]=[null];
 const comparison=ref<Comparison|null>(null),proposalId=ref<string|null>(null),comparisonLoading=ref(false),comparisonError=ref('');
 let comparisonSeq=0;
 function clearComparison(){comparisonSeq++;comparison.value=null;proposalId.value=null;comparisonLoading.value=false;comparisonError.value='';}
 let root=0,target=0,itemGeneration=0,listSeq=0,detailSeq=0,itemSeq=0;
 const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
 const token=()=>({owner:owner(),root,couple:coupleId.value,memory:memoryId.value,target,itemGeneration});
 type Token=ReturnType<typeof token>;
 const base=(t:Token)=>!!t.owner&&t.owner===owner()&&t.root===root&&t.couple===coupleId.value;
 const selected=(t:Token)=>base(t)&&t.memory===memoryId.value&&t.target===target;
 const photoCurrent=(t:Token)=>selected(t)&&t.itemGeneration===itemGeneration;
 function clearList(){space.value=null;memories.value=[];nextCursor.value=null;pageIndex.value=0;cursors=[null];}
 function clearItems(){history.value=null;itemNextCursor.value=null;itemPageIndex.value=0;itemCursors=[null];}
 function invalidate(){clearComparison();root++;target++;itemGeneration++;listSeq++;detailSeq++;itemSeq++;listLoading.value=detailLoading.value=itemsLoading.value=false;clearList();detail.value=null;clearItems();}
 function reset(){invalidate();coupleId.value=memoryId.value=null;space.value=null;availability.value='available';listError.value=detailError.value=itemsError.value='';}
 watch(owner,reset,{flush:'sync'});
 const auth=(e:unknown)=>e instanceof AdminApiError&&(e.status===401||e.status===403);
 function authLost(){invalidate();listError.value=detailError.value=itemsError.value='管理员会话已失效或无权访问，已清除显示的数据。请重新登录。';}
 function gone(){invalidate();listError.value='回忆信息已变化，请刷新列表。';detailError.value='回忆不存在或当前不可用。';itemsError.value='';}
 async function loadList(index=0,cursor:string|null=null){
  if(!owner()||!coupleId.value)return;const t=token(),s=++listSeq;listLoading.value=true;listError.value='';
  try{const r=await api.list(t.couple!,cursor);if(!base(t)||s!==listSeq)return;if(!isMemoryPage(r,t.couple!))throw new Error('shape');
   space.value=r.space;availability.value=r.availability;memories.value=r.items;nextCursor.value=r.next_cursor;pageIndex.value=index;cursors=[...cursors.slice(0,index),cursor];
   if(r.availability==='unavailable'){clearComparison();target++;itemGeneration++;detailSeq++;itemSeq++;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value='回忆内容当前不可用。';}
  }catch(e){if(!base(t)||s!==listSeq)return;if(auth(e)){authLost();return;}clearList();listError.value=e instanceof AdminApiError&&e.status===404?'该空间不存在。':'回忆列表读取失败，请重试。';
   if(e instanceof AdminApiError&&e.status===404){clearComparison();target++;itemGeneration++;detailSeq++;itemSeq++;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value=listError.value;}
  }finally{if(base(t)&&s===listSeq)listLoading.value=false;}
 }
 async function loadDetail(){
  if(!owner()||!coupleId.value||!memoryId.value)return;const t=token(),s=++detailSeq;detailLoading.value=true;detailError.value='';
  try{const r=await api.detail(t.couple!,t.memory!);if(!selected(t)||s!==detailSeq)return;if(!isMemoryDetail(r,t.couple!,t.memory!))throw new Error('shape');detail.value=r;
  }catch(e){if(!selected(t)||s!==detailSeq)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}detail.value=null;detailError.value='回忆详情读取失败，请重试。';
  }finally{if(selected(t)&&s===detailSeq)detailLoading.value=false;}
 }
 async function loadItems(index=0,cursor:string|null=null){
  if(!owner()||!coupleId.value||!memoryId.value)return;const t=token(),s=++itemSeq;itemsLoading.value=true;itemsError.value='';
  try{const r=await api.proposals(t.couple!,t.memory!,cursor);if(!photoCurrent(t)||s!==itemSeq)return;if(!isProposalPage(r,t.couple!,t.memory!))throw new Error('shape');history.value=r;itemNextCursor.value=r.next_cursor;itemPageIndex.value=index;itemCursors=[...itemCursors.slice(0,index),cursor];
  }catch(e){if(!photoCurrent(t)||s!==itemSeq)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}clearItems();itemsError.value='修改历史读取失败，请重试。';
  }finally{if(photoCurrent(t)&&s===itemSeq)itemsLoading.value=false;}
 }
 async function select(couple:string){if(!owner())return;if(couple===coupleId.value&&(space.value||listLoading.value))return;reset();coupleId.value=couple;await loadList();}
 async function openMemory(memory:string|null){if(!owner()||!coupleId.value)return;if(memory===memoryId.value&&(detail.value||detailLoading.value))return;clearComparison();target++;itemGeneration++;detailSeq++;itemSeq++;memoryId.value=memory;detail.value=null;clearItems();detailLoading.value=itemsLoading.value=false;detailError.value=itemsError.value='';if(memory)await Promise.all([loadDetail(),loadItems()]);}
 async function openProposal(id:string){
  if(!owner()||!coupleId.value||!memoryId.value)return;clearComparison();proposalId.value=id;const t=token(),s=++comparisonSeq;comparisonLoading.value=true;
  try{const r=await api.comparison(t.couple!,t.memory!,id);if(!selected(t)||s!==comparisonSeq||proposalId.value!==id)return;if(!isComparison(r,t.couple!,t.memory!,id))throw new Error('shape');comparison.value=r;}
  catch(e){if(!selected(t)||s!==comparisonSeq||proposalId.value!==id)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}comparisonError.value='修改对比读取失败，请重试。';}
  finally{if(selected(t)&&s===comparisonSeq)comparisonLoading.value=false;}
 }
 async function refreshList(){listSeq++;await loadList();}
 async function refreshDetail(){if(memoryId.value)await loadDetail();}
 async function refreshItems(){itemGeneration++;itemSeq++;clearItems();await loadItems();}
 async function next(){if(nextCursor.value&&!listLoading.value)await loadList(pageIndex.value+1,nextCursor.value);}
 async function prev(){if(pageIndex.value&&!listLoading.value)await loadList(pageIndex.value-1,cursors[pageIndex.value-1]);}
 async function nextItems(){if(itemNextCursor.value&&!itemsLoading.value)await loadItems(itemPageIndex.value+1,itemNextCursor.value);}
 async function prevItems(){if(itemPageIndex.value&&!itemsLoading.value)await loadItems(itemPageIndex.value-1,itemCursors[itemPageIndex.value-1]);}
 const hasPrev=computed(()=>pageIndex.value>0),hasNext=computed(()=>nextCursor.value!==null),hasItemPrev=computed(()=>itemPageIndex.value>0),hasItemNext=computed(()=>itemNextCursor.value!==null);
 return {comparison,proposalId,comparisonLoading,comparisonError,openProposal,coupleId,memoryId,space,availability,memories,detail,history,listLoading,detailLoading,itemsLoading,listError,detailError,itemsError,pageIndex,itemPageIndex,hasPrev,hasNext,hasItemPrev,hasItemNext,select,openMemory,refreshList,refreshDetail,refreshItems,next,prev,nextItems,prevItems,reset};
});
