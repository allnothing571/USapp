<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useCoupleProductsStore} from '@/stores/couple-products';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(),store=useCoupleProductsStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.productId)] as const,([couple,id],old)=>{if(!couple)return;if(!old||old[0]!==couple)void store.select(couple);void store.openProduct(id||null);},{immediate:true});
onBeforeUnmount(()=>store.reset());
const target=(productId:string)=>({name:'couple-products',params:{coupleId:store.coupleId??'',productId}});
const mode=(v:string)=>v==='direct'?'直接兑换':'申请兑换';
const status=(v:string)=>v==='active'?'有效':'已归档下架';
</script>
<template>
 <div><h1 class="page-title">空间奖励商品</h1><p class="page-subtitle">只读查看当前奖励商品，包含已归档下架的商品。</p>
 <div class="panel"><div class="panel-head"><h2 class="panel-title">商品列表</h2><button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div><p class="mono">空间：{{store.coupleId}}</p><p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p><p v-if="store.listLoading">加载中…</p><p v-else-if="!store.listError&&store.space&&!store.items.length">该空间没有奖励商品。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>商品</th><th>当前价格（小红花）</th><th>兑换方式</th><th>状态</th><th>版本</th><th>创建者</th><th>领取者</th></tr></thead><tbody><tr v-for="p in store.items" :key="p.id"><td><RouterLink :to="target(p.id)">{{p.title}}</RouterLink></td><td>{{p.price_flowers}}</td><td>{{mode(p.redemption_mode)}}</td><td>{{status(p.status)}}</td><td>{{p.version}}</td><td :title="p.creator_id">{{shortId(p.creator_id)}}</td><td :title="p.owner_id">{{shortId(p.owner_id)}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/></div>
 <div v-if="store.productId" class="panel"><div class="panel-head"><h2 class="panel-title">商品详情</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div><p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
 <template v-if="store.detail"><h3>{{store.detail.product.title}}</h3><p>{{store.detail.product.price_flowers}} 朵小红花 · {{mode(store.detail.product.redemption_mode)}} · {{status(store.detail.product.status)}} · 版本 {{store.detail.product.version}}</p><p class="body-text">{{store.detail.product.description}}</p><p>创建时间：{{formatDateTime(store.detail.product.created_at)}} · 最近修订：{{formatDateTime(store.detail.product.updated_at)}}</p><p>这里显示当前商品条款，不是历史兑换成交快照。已归档商品仅供管理员巡检，不恢复普通领取者的访问或兑换资格。</p><p>列表与详情独立读取，可能处于不同快照。详情读取时间：{{formatDateTime(store.detail.read_at)}}</p></template>
 </div></div>
</template>
<style scoped>.panel{overflow-wrap:anywhere}.body-text{white-space:pre-wrap}</style>
