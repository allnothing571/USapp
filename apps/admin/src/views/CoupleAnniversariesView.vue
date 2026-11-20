<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useCoupleAnniversariesStore} from '@/stores/couple-anniversaries';
import {decisionText} from '@/api/anniversaries';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(),store=useCoupleAnniversariesStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.anniversaryId)] as const,([couple,id],old)=>{if(!couple)return;if(!old||old[0]!==couple)void store.select(couple);void store.openAnniversary(id||null);},{immediate:true});
onBeforeUnmount(()=>store.reset());
const target=(anniversaryId:string)=>({name:'couple-anniversaries',params:{coupleId:store.coupleId??'',anniversaryId}});
const kind=(v:string)=>v==='anniversary'?'纪念日':'倒计时';
</script>
<template>
 <div><h1 class="page-title">空间纪念日</h1><p class="page-subtitle">只读查看纪念日、倒计时和当前修改状态。</p>
 <div class="panel"><div class="panel-head"><h2 class="panel-title">纪念日列表</h2><button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div><p class="mono">空间：{{store.coupleId}}</p><p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p><p v-if="store.listLoading">加载中…</p><p v-else-if="!store.listError&&store.space&&!store.items.length">该空间没有纪念日。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>标题</th><th>类型</th><th>日期</th><th>版本</th><th>创建者</th><th>创建时间</th></tr></thead><tbody><tr v-for="a in store.items" :key="a.id"><td><RouterLink :to="target(a.id)">{{a.title}}</RouterLink></td><td>{{kind(a.kind)}}</td><td>{{a.date}}</td><td>{{a.version}}</td><td :title="a.creator_id">{{shortId(a.creator_id)}}</td><td>{{formatDateTime(a.created_at)}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/></div>
 <div v-if="store.anniversaryId" class="panel"><div class="panel-head"><h2 class="panel-title">纪念日详情</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div><p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
 <template v-if="store.detail"><section><h3>{{store.detail.anniversary.title}}</h3><p>{{kind(store.detail.anniversary.kind)}} · {{store.detail.anniversary.date}} · 当前版本 {{store.detail.anniversary.version}}</p><p class="body-text">{{store.detail.anniversary.note}}</p></section>
 <section><h3>待处理修改请求</h3><template v-if="store.detail.pending_proposal"><p>基础版本 {{store.detail.pending_proposal.base_version}} · 提出者 {{shortId(store.detail.pending_proposal.proposer_id)}} · {{formatDateTime(store.detail.pending_proposal.created_at)}}</p><h4>{{store.detail.pending_proposal.title}}</h4><p>{{kind(store.detail.pending_proposal.kind)}} · {{store.detail.pending_proposal.date}}</p><p class="body-text">{{store.detail.pending_proposal.note}}</p></template><p v-else>没有待处理请求。</p></section>
 <section><h3>最近一次决定</h3><template v-if="store.detail.last_decision"><p>{{decisionText(store.detail.last_decision,store.detail.anniversary.version)}}</p><p>基础版本 {{store.detail.last_decision.base_version}} · 决定者 {{shortId(store.detail.last_decision.decided_by)}} · {{formatDateTime(store.detail.last_decision.decided_at)}}</p></template><p v-else>尚无决定。</p><p>这里只显示最近一次决定，不代表完整修改历史。</p></section><p>详情读取时间：{{formatDateTime(store.detail.read_at)}}。列表与详情分别读取，可能处于不同快照；日期为记录的日历日期，不计算剩余或累计天数。</p></template>
 </div></div>
</template>
<style scoped>.panel{overflow-wrap:anywhere}.body-text{white-space:pre-wrap}</style>
