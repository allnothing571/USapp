<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import RedemptionCancellationPanel from '@/components/RedemptionCancellationPanel.vue';
import CursorPager from '@/components/CursorPager.vue';
import {statuses,statusLabels,actionLabels,type StatusFilter} from '@/api/redemptions';
import {useCoupleRedemptionsStore} from '@/stores/couple-redemptions';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(); const store=useCoupleRedemptionsStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.redemptionId)] as const,([couple,id],old)=>{
  if(!couple)return;
  if(!old||old[0]!==couple)void store.select(couple);
  void store.openRedemption(id||null);
},{immediate:true});
onBeforeUnmount(()=>store.reset());
const filters:Array<{value:StatusFilter;label:string}>=[{value:'',label:'全部'},...statuses.map(s=>({value:s,label:statusLabels[s]}))];
const routeTo=(id:string)=>({name:'couple-redemptions',params:{coupleId:store.coupleId??'',redemptionId:id}});
</script>
<template>
  <div>
    <h1 class="page-title">空间兑换记录</h1>
    <p class="page-subtitle">只读查看兑换时保存的商品快照、状态与历史。每页和详情分别是读取时的快照；筛选结果可能随审批或履约变化。异常处理仅允许在独立确认区取消未履约兑换并按原扣款退款，不提供强制履约或历史修改。</p>
    <div class="panel">
      <div class="panel-head"><h2 class="panel-title">兑换列表</h2><span class="tag placeholder">只读</span><span class="mono">{{shortId(store.coupleId)}}</span>
        <div class="toolbar"><button v-for="f in filters" :key="f.value" class="btn" :class="{primary:store.status===f.value}" :disabled="store.listLoading" @click="store.selectStatus(f.value)">{{f.label}}</button>
          <button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div></div>
      <p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p>
      <p v-if="store.listLoading">加载中…</p><p v-else-if="store.isEmpty" class="notice">该空间在此筛选下没有兑换记录。</p>
      <div class="table-wrap"><table class="data"><thead><tr><th>商品快照</th><th>小红花价格</th><th>兑换者</th><th>创建者</th><th>状态</th><th>创建时间</th></tr></thead>
        <tbody><tr v-for="r in store.items" :key="r.id"><td><RouterLink :to="routeTo(r.id)">{{r.title}}</RouterLink></td><td>{{r.price_flowers}}</td><td :title="r.redeemer_id">{{shortId(r.redeemer_id)}}</td><td :title="r.creator_id">{{shortId(r.creator_id)}}</td><td>{{statusLabels[r.status]}}</td><td>{{formatDateTime(r.created_at)}}</td></tr></tbody></table></div>
      <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/>
    </div>
    <RedemptionCancellationPanel :detail="store.detail" :loading="store.detailLoading" @confirmed="store.refreshDetail();store.refreshList()"/>
    <div v-if="store.redemptionId" class="panel"><div class="panel-head"><h2 class="panel-title">兑换详情</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div>
      <p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
      <div v-else-if="store.detail" class="panel-body">
        <h3>{{store.detail.redemption.title}}</h3><p class="snapshot-text">{{store.detail.redemption.description}}</p>
        <p>商品 {{store.detail.redemption.product_id}} · 第 {{store.detail.redemption.product_version}} 版 · {{store.detail.redemption.redemption_mode==='direct'?'直接兑换':'申请兑换'}}</p>
        <p>价格：{{store.detail.redemption.price_flowers}} 小红花 · {{statusLabels[store.detail.redemption.status]}}</p>
        <p>创建者：{{store.detail.redemption.creator_id}} · 兑换者：{{store.detail.redemption.redeemer_id}}</p>
        <p v-if="store.detail.redemption.fulfilled_at">履约人：{{store.detail.redemption.fulfilled_by}} · {{formatDateTime(store.detail.redemption.fulfilled_at)}}</p>
        <p v-if="store.detail.redemption.cancelled_at">取消原因：{{store.detail.redemption.cancel_reason}} · {{formatDateTime(store.detail.redemption.cancelled_at)}}</p>
        <p v-if="store.detail.redemption.cancellation_id">待处理取消申请：{{store.detail.redemption.cancellation_id}} · 提出人：{{store.detail.redemption.cancellation_by}}</p>
        <div v-if="store.detail.admin_cancellation"><h3>管理员取消记录</h3>
          <p>管理员 {{store.detail.admin_cancellation.admin_id}} · 原退款 {{store.detail.admin_cancellation.refunded_flowers}} 小红花 · {{formatDateTime(store.detail.admin_cancellation.created_at)}}</p>
          <p>原因：{{store.detail.admin_cancellation.reason}}</p>
        </div>
        <h3>成员事件历史</h3><p>共 {{store.detail.events.total}} 条，最多显示最新 100 条；同一时间按事件 ID 排序。管理员取消记录单独展示。</p>
        <ol><li v-for="e in store.detail.events.items" :key="e.id">{{actionLabels[e.action]}} · {{e.actor_id}} · {{formatDateTime(e.created_at)}}<span v-if="e.proposal_id"> · 对应申请 {{e.proposal_id}}</span></li></ol>
        <p>读取时间：{{formatDateTime(store.detail.read_at)}}</p>
      </div>
    </div>
  </div>
</template>
<style scoped>.snapshot-text{white-space:pre-wrap;overflow-wrap:anywhere}.panel-body{overflow-wrap:anywhere}</style>
