import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {useAdminSessionStore} from './admin-session';
import {AdminApiError} from '@/api/client';
import {productAdminApi as api,isProductPage,isProductDetail,type Product,type ProductDetail,type Space} from '@/api/products';
export const useCoupleProductsStore=defineStore('couple-products',()=>{
 const session=useAdminSessionStore();
 const coupleId=ref<string|null>(null),productId=ref<string|null>(null),space=ref<Space|null>(null);
 const items=ref<Product[]>([]),detail=ref<ProductDetail|null>(null);
 const listLoading=ref(false),detailLoading=ref(false),listError=ref(''),detailError=ref('');
 const pageIndex=ref(0),nextCursor=ref<string|null>(null);let cursors:(string|null)[]=[null];
 let root=0,target=0,listSeq=0,detailSeq=0;
 const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
 const token=()=>({owner:owner(),root,couple:coupleId.value,id:productId.value,target});
 type Token=ReturnType<typeof token>;
 const base=(t:Token)=>!!t.owner&&t.owner===owner()&&t.root===root&&t.couple===coupleId.value;
 const selected=(t:Token)=>base(t)&&t.id===productId.value&&t.target===target;
 function clearList(){space.value=null;items.value=[];nextCursor.value=null;pageIndex.value=0;cursors=[null];}
 function invalidate(){root++;target++;listSeq++;detailSeq++;listLoading.value=detailLoading.value=false;clearList();detail.value=null;}
 function reset(){invalidate();coupleId.value=productId.value=null;listError.value=detailError.value='';}
 watch(owner,reset,{flush:'sync'});
 const auth=(e:unknown)=>e instanceof AdminApiError&&(e.status===401||e.status===403);
 function authLost(){invalidate();listError.value=detailError.value='管理员会话已失效或无权访问，已清除显示的数据。请重新登录。';}
 function gone(){invalidate();listError.value='奖励商品信息已变化，请刷新列表。';detailError.value='该空间中没有这个奖励商品。';}
 async function loadList(index=0,cursor:string|null=null){
  if(!owner()||!coupleId.value)return;const t=token(),s=++listSeq;listLoading.value=true;listError.value='';
  try{const r=await api.list(t.couple!,cursor);if(!base(t)||s!==listSeq)return;if(!isProductPage(r,t.couple!))throw new Error('shape');space.value=r.space;items.value=r.items;nextCursor.value=r.next_cursor;pageIndex.value=index;cursors=[...cursors.slice(0,index),cursor];}
  catch(e){if(!base(t)||s!==listSeq)return;if(auth(e)){authLost();return;}clearList();listError.value=e instanceof AdminApiError&&e.status===404?'该空间不存在。':'奖励商品列表读取失败，请重试。';if(e instanceof AdminApiError&&e.status===404){target++;detailSeq++;detail.value=null;detailLoading.value=false;detailError.value=listError.value;}}
  finally{if(base(t)&&s===listSeq)listLoading.value=false;}
 }
 async function loadDetail(){
  if(!owner()||!coupleId.value||!productId.value)return;const t=token(),s=++detailSeq;detailLoading.value=true;detailError.value='';
  try{const r=await api.detail(t.couple!,t.id!);if(!selected(t)||s!==detailSeq)return;if(!isProductDetail(r,t.couple!,t.id!))throw new Error('shape');detail.value=r;}
  catch(e){if(!selected(t)||s!==detailSeq)return;if(auth(e)){authLost();return;}if(e instanceof AdminApiError&&e.status===404){gone();return;}detail.value=null;detailError.value='奖励商品详情读取失败，请重试。';}
  finally{if(selected(t)&&s===detailSeq)detailLoading.value=false;}
 }
 async function select(couple:string){if(!owner())return;if(couple===coupleId.value&&(space.value||listLoading.value))return;reset();coupleId.value=couple;await loadList();}
 async function openProduct(id:string|null){if(!owner()||!coupleId.value)return;if(id===productId.value&&(detail.value||detailLoading.value))return;target++;detailSeq++;productId.value=id;detail.value=null;detailLoading.value=false;detailError.value='';if(id)await loadDetail();}
 async function refreshList(){await loadList();}
 async function refreshDetail(){await loadDetail();}
 async function next(){if(nextCursor.value&&!listLoading.value)await loadList(pageIndex.value+1,nextCursor.value);}
 async function prev(){if(pageIndex.value&&!listLoading.value)await loadList(pageIndex.value-1,cursors[pageIndex.value-1]);}
 const hasPrev=computed(()=>pageIndex.value>0),hasNext=computed(()=>nextCursor.value!==null);
 return {coupleId,productId,space,items,detail,listLoading,detailLoading,listError,detailError,pageIndex,hasPrev,hasNext,select,openProduct,refreshList,refreshDetail,next,prev,reset};
});
