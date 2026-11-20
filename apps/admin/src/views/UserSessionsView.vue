<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useUserSessionsStore} from '@/stores/user-sessions';
import {formatDateTime} from '@/utils/format';
import type {UserSessionState} from '@/api/user-sessions';
const route=useRoute();const store=useUserSessionsStore();
watch(()=>route.params.userId,id=>{if(typeof id==='string')void store.select(id.toLowerCase());},{immediate:true});
onBeforeUnmount(()=>store.reset());
const labels:Record<UserSessionState,string>={valid:'未过期且未撤销',expired:'已到期',revoked:'已撤销'};
</script>
<template>
  <div><h1 class="page-title">用户会话</h1>
    <p class="page-subtitle">只读会话元数据，不提供强制退出、续期或撤销。列表不包含令牌、哈希、设备原始信息和预认证会话。</p>
    <div class="panel"><div class="panel-head"><h2 class="panel-title">会话列表</h2><span class="tag placeholder">只读</span><button class="btn" :disabled="store.loading" @click="store.refresh()">刷新</button></div>
      <div class="panel-body"><p class="mono">用户：{{store.userId}}</p><p v-if="store.user">账号状态：{{store.user.status==='active'?'正常':'已禁用'}}</p>
        <p class="notice">“未过期且未撤销”只描述会话时间和撤销状态，不保证当前可以登录；账号状态与其他服务端检查仍然生效。每页是独立快照，不会自动刷新。</p>
        <p v-if="store.readAt">读取时间：{{formatDateTime(store.readAt)}}</p></div>
      <p v-if="store.error" class="notice error" role="alert">{{store.error}}</p>
      <p v-if="store.loading">加载中…</p><p v-else-if="!store.error&&store.user&&store.items.length===0" class="notice">该用户没有会话记录。</p>
      <div class="table-wrap"><table class="data"><thead><tr><th>会话 ID</th><th>状态</th><th>创建时间</th><th>最后活动</th><th>到期时间</th><th>撤销时间</th></tr></thead>
      <tbody><tr v-for="s in store.items" :key="s.id"><td class="mono">{{s.id}}</td><td>{{labels[s.state]}}</td><td>{{formatDateTime(s.created_at)}}</td><td>{{formatDateTime(s.last_seen_at)}}</td><td>{{formatDateTime(s.expires_at)}}</td><td>{{s.revoked_at?formatDateTime(s.revoked_at):'—'}}</td></tr></tbody></table></div>
      <CursorPager :page-index="store.pageIndex" :item-count="store.items.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.loading" @prev="store.prev()" @next="store.next()" @reload="store.refresh()"/>
    </div>
  </div>
</template>
