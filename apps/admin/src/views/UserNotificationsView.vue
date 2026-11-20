<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useUserNotificationsStore} from '@/stores/user-notifications';
import {outcomeLabels} from '@/api/notifications';
import {formatDateTime} from '@/utils/format';
const route=useRoute(),store=useUserNotificationsStore();
watch(()=>route.params.userId,id=>{if(typeof id==='string')void store.select(id.toLowerCase());},{immediate:true});
onBeforeUnmount(()=>store.reset());
</script>
<template>
  <div><h1 class="page-title">用户通知与推送记录</h1><p class="page-subtitle">只读元数据，不提供发送、重试、标为已读或修改订阅。不会显示通知正文、内容目标、订阅地址或密钥。</p>
    <div class="panel"><div class="panel-head"><h2 class="panel-title">通知列表</h2><span class="tag placeholder">只读</span><button class="btn" :disabled="store.loading" @click="store.refresh()">刷新</button></div>
      <div class="panel-body"><p class="mono">用户：{{store.userId}}</p><p v-if="store.user">账号状态：{{store.user.status==='active'?'正常':'已禁用'}}</p>
        <p class="notice">推送服务接收不代表设备已展示；没有投递或结果记录，不等于未发送或失败。每条通知只展示最新 5 个订阅代次及各自最近一次尝试，不能据此推断当前会否重试。</p><p v-if="store.readAt">本页快照：{{formatDateTime(store.readAt)}}，不会自动刷新。</p></div>
      <p v-if="store.error" role="alert" class="notice error">{{store.error}}</p><p v-if="store.loading">加载中…</p><p v-else-if="!store.error&&store.user&&store.items.length===0" class="notice">该用户没有通知记录。</p>
      <div v-for="n in store.items" :key="n.id" class="panel-body record"><h3>{{n.event_type}}</h3><p class="mono">通知 {{n.id}} · {{formatDateTime(n.created_at)}}</p><p>{{n.read_at?'已读：'+formatDateTime(n.read_at):'无已读回执'}}</p>
        <details><summary>投递记录：显示 {{n.push.items.length}} / {{n.push.total}} 个订阅代次</summary>
          <p v-if="n.push.items.length===0">无投递记录，不据此推断原因。</p>
          <div v-for="d in n.push.items" :key="d.generation_id" class="delivery"><p class="mono">订阅代次 {{d.generation_id}} · 建立投递 {{formatDateTime(d.created_at)}}</p>
            <p>最终记录：{{d.final_state?outcomeLabels[d.final_state]:'尚无最终记录'}}<span v-if="d.decided_at"> · {{formatDateTime(d.decided_at)}}</span></p>
            <template v-if="d.latest_attempt"><p>最近尝试 #{{d.latest_attempt.attempt_no}} · {{formatDateTime(d.latest_attempt.claimed_at)}} · 租约截止 {{formatDateTime(d.latest_attempt.lease_until)}}</p>
              <p v-if="d.latest_attempt.expired_at">尝试过期记录：{{formatDateTime(d.latest_attempt.expired_at)}}（不代表从未发出请求）</p>
              <p v-if="d.latest_attempt.observation">最近结果：{{outcomeLabels[d.latest_attempt.observation.outcome]}} · {{d.latest_attempt.observation.class==='provider'?'推送服务结果':'内部结果'}}<span v-if="d.latest_attempt.observation.http_status!==null"> · 状态码 {{d.latest_attempt.observation.http_status}}</span><span v-if="d.latest_attempt.observation.retry_after_s!==null"> · 记录的重试建议 {{d.latest_attempt.observation.retry_after_s}} 秒</span> · {{formatDateTime(d.latest_attempt.observation.observed_at)}}</p>
              <p v-else>尚无尝试结果记录，不推断成败。</p>
            </template><p v-else>尚无尝试记录。</p>
          </div>
        </details>
      </div>
      <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.loading" @prev="store.prev()" @next="store.next()" @reload="store.refresh()"/>
    </div>
  </div>
</template>
<style scoped>.record{border-top:1px solid var(--border,#ddd);overflow-wrap:anywhere}.delivery{padding:0.5rem 0;border-top:1px solid var(--border,#ddd)}summary{cursor:pointer}</style>
