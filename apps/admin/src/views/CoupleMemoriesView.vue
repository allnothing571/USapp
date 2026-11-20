<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import MemoryPhotoReferences from '@/components/MemoryPhotoReferences.vue';
import {useCoupleMemoriesStore} from '@/stores/couple-memories';
import {decisionText} from '@/api/memories';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(),store=useCoupleMemoriesStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.memoryId)] as const,([couple,memory],old)=>{if(!couple)return;if(!old||old[0]!==couple)void store.select(couple);void store.openMemory(memory||null);},{immediate:true});
onBeforeUnmount(()=>store.reset());
const target=(memoryId:string)=>({name:'couple-memories',params:{coupleId:store.coupleId??'',memoryId}});
</script>
<template>
 <div><h1 class="page-title">空间回忆</h1><p class="page-subtitle">只读查看当前内容和修改请求历史。照片仅显示引用信息。</p>
 <div class="panel"><div class="panel-head"><h2 class="panel-title">回忆列表</h2><button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div>
 <p class="mono">空间：{{store.coupleId}}</p><p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p>
 <p v-if="store.availability==='unavailable'" class="notice">回忆内容当前不可用。</p><p v-else-if="store.listLoading">加载中…</p><p v-else-if="!store.listError&&!store.memories.length&&store.space">该空间没有可见回忆。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>标题</th><th>回忆日期</th><th>版本</th><th>可见照片</th><th>创建者</th><th>创建时间</th></tr></thead><tbody><tr v-for="m in store.memories" :key="m.id"><td><RouterLink :to="target(m.id)">{{m.title}}</RouterLink></td><td>{{m.date}}</td><td>{{m.version}}</td><td>{{m.photo_count}}</td><td :title="m.creator_id">{{shortId(m.creator_id)}}</td><td>{{formatDateTime(m.created_at)}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.pageIndex" :item-count="store.memories.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/>
 </div>
 <div v-if="store.memoryId" class="panel"><div class="panel-head"><h2 class="panel-title">当前回忆</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div>
 <p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
 <div v-if="store.detail" class="panel-body"><h3>{{store.detail.memory.title}}</h3><p>{{store.detail.memory.date}} · 版本 {{store.detail.memory.version}}</p><p class="body-text">{{store.detail.memory.description}}</p><MemoryPhotoReferences :photos="store.detail.memory.photos"/><p>详情读取时间：{{formatDateTime(store.detail.read_at)}}</p></div>
 </div>
 <div v-if="store.memoryId" class="panel"><div class="panel-head"><h2 class="panel-title">修改请求历史</h2><button class="btn" :disabled="store.itemsLoading" @click="store.refreshItems()">刷新历史</button></div>
 <p>历史决定与当前内容分开显示。列表、详情、历史及对比各自读取快照；删除或清除后的内容不会通过新读取显示。</p>
 <p v-if="store.itemsError" role="alert" class="notice error">{{store.itemsError}}</p><p v-if="store.itemsLoading">加载中…</p><p v-else-if="store.history&&!store.history.items.length">没有修改请求。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>请求</th><th>提出者</th><th>创建时间</th><th>基础版本</th><th>决定</th><th>决定者／时间</th></tr></thead><tbody><tr v-for="p in store.history?.items??[]" :key="p.id"><td><button class="btn" @click="store.openProposal(p.id)">查看对比</button></td><td :title="p.proposer_id">{{shortId(p.proposer_id)}}</td><td>{{formatDateTime(p.created_at)}}</td><td>{{p.base_version}}</td><td>{{decisionText(p,store.history!.current_version)}}</td><td>{{p.decided_by?shortId(p.decided_by)+' · '+formatDateTime(p.decided_at!):'—'}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.itemPageIndex" :item-count="store.history?.items.length??0" :has-prev="store.hasItemPrev" :has-next="store.hasItemNext" :loading="store.itemsLoading" @prev="store.prevItems()" @next="store.nextItems()" @reload="store.refreshItems()"/>
 </div>
 <div v-if="store.proposalId" class="panel"><div class="panel-head"><h2 class="panel-title">修改对比</h2><button class="btn" :disabled="store.comparisonLoading" @click="store.openProposal(store.proposalId!)">重新读取对比</button></div><p v-if="store.comparisonError" role="alert">{{store.comparisonError}}</p><p v-if="store.comparisonLoading">加载中…</p>
 <template v-if="store.comparison"><p>{{decisionText(store.comparison.comparison.proposal,store.comparison.current_version)}}</p><div class="comparison"><section><h3>原内容（版本 {{store.comparison.comparison.original.version}}）</h3><h4>{{store.comparison.comparison.original.title}}</h4><p>{{store.comparison.comparison.original.date}}</p><p class="body-text">{{store.comparison.comparison.original.description}}</p><MemoryPhotoReferences :photos="store.comparison.comparison.original.photos"/></section><section><h3>请求内容</h3><h4>{{store.comparison.comparison.proposed.title}}</h4><p>{{store.comparison.comparison.proposed.date}}</p><p class="body-text">{{store.comparison.comparison.proposed.description}}</p><MemoryPhotoReferences :photos="store.comparison.comparison.proposed.photos"/></section></div><p>对比读取时间：{{formatDateTime(store.comparison.read_at)}}</p></template>
 </div></div>
</template>
<style scoped>.panel{overflow-wrap:anywhere}.body-text{white-space:pre-wrap}.comparison{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,18rem),1fr));gap:1rem}</style>
