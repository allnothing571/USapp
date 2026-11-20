<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useCoupleSharedPlansStore} from '@/stores/couple-shared-plans';
import {statusLabels,operationLabels,outcomeLabels} from '@/api/shared-plans';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(),store=useCoupleSharedPlansStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.planId)] as const,([couple,plan],old)=>{if(!couple)return;if(!old||old[0]!==couple)void store.select(couple);void store.openPlan(plan||null);},{immediate:true});
onBeforeUnmount(()=>store.reset());
const target=(id:string)=>({name:'couple-shared-plans',params:{coupleId:store.coupleId??'',planId:id}});
</script>
<template>
  <div><h1 class="page-title">空间共享计划</h1><p class="page-subtitle">只读查看计划状态、版本及历史，不提供编辑、审批或状态推进。仅展示计划本身，不展示关联回忆、照片或请求原文。</p>
    <div class="panel"><div class="panel-head"><h2 class="panel-title">计划列表</h2><span class="tag placeholder">只读</span><button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div>
      <p class="mono">空间：{{store.coupleId}}</p><p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p><p v-if="store.listLoading">加载中…</p><p v-else-if="store.isEmpty" class="notice">该空间没有计划记录。</p>
      <div class="table-wrap"><table class="data"><thead><tr><th>标题</th><th>状态</th><th>内容版本</th><th>状态版本</th><th>创建者</th><th>创建时间</th></tr></thead><tbody><tr v-for="p in store.items" :key="p.id"><td><RouterLink :to="target(p.id)">{{p.title}}</RouterLink></td><td>{{statusLabels[p.status]}}</td><td>{{p.body_version}}</td><td>{{p.status_version}}</td><td :title="p.creator_id">{{shortId(p.creator_id)}}</td><td>{{formatDateTime(p.created_at)}}</td></tr></tbody></table></div>
      <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/>
    </div>
    <div v-if="store.planId" class="panel"><div class="panel-head"><h2 class="panel-title">计划详情</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div>
      <p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
      <div v-else-if="store.detail" class="panel-body content"><h3>{{store.detail.plan.title}}</h3><p>{{statusLabels[store.detail.plan.status]}} · 内容版本 {{store.detail.plan.body_version}} · 状态版本 {{store.detail.plan.status_version}}</p>
        <p>日期：{{store.detail.plan.body.date??'未设置'}} · 时间：{{store.detail.plan.body.time??'未设置'}}</p><p>地点：{{store.detail.plan.body.place??'未设置'}}</p><p class="body-text">{{store.detail.plan.body.note??'无备注'}}</p>
        <p class="notice">计划文字独立保留，不会因关联回忆被删除而清除。本页不返回回忆关联信息。</p>
        <h3>事件历史</h3><p>共 {{store.detail.events.total}} 条，最多显示最新 50 条。历史请求不代表当前待办；每页和详情是独立快照。</p>
        <ol><li v-for="e in store.detail.events.items" :key="e.id"><strong>#{{e.seq}} {{operationLabels[e.operation]}}</strong> · {{e.actor_id}} · {{formatDateTime(e.created_at)}}<span v-if="e.outcome"> · {{outcomeLabels[e.outcome]}}</span><span v-if="e.proposal_id"> · 对应请求 {{e.proposal_id}}</span><p v-if="e.status">状态：{{statusLabels[e.status]}}（版本 {{e.status_version}}）</p>
          <details v-if="e.body"><summary>查看本次内容</summary><p>{{e.body.title}}</p><p>{{e.body.date??'未设置日期'}} · {{e.body.time??'未设置时间'}} · {{e.body.place??'未设置地点'}}</p><p class="body-text">{{e.body.note??'无备注'}}</p></details></li></ol>
        <p>详情读取时间：{{formatDateTime(store.detail.read_at)}}</p>
      </div>
    </div>
  </div>
</template>
<style scoped>.content{overflow-wrap:anywhere}.body-text{white-space:pre-wrap}li{margin-bottom:1rem}</style>
