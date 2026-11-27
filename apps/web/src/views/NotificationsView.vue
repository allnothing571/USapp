<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import { useRouter } from 'vue-router'
import { useNotificationsStore } from '@/stores/notifications'
import { useSessionStore } from '@/stores/session'
import { notificationLabel, notificationTarget, type Notification } from '@/api/notifications'
import { useSmartBack } from '@/router/back-navigation'
const back = useSmartBack('notifications', 'home')
const store = useNotificationsStore(), session = useSessionStore(), router = useRouter()
let epoch = 0
let refreshing: Promise<void> | null = null
let alive = true
function refresh(): void {
  if (!alive || !session.user || document.visibilityState === 'hidden' || refreshing) return
  const pending = Promise.all([store.load(), store.loadCount()]).then(() => {})
  refreshing = pending
  void pending.finally(() => { if (refreshing === pending) refreshing = null })
}
watch(() => session.user?.id, () => { ++epoch; refreshing = null; refresh() })
onMounted(() => {
  refresh(); window.addEventListener('focus', refresh); window.addEventListener('online', refresh)
  document.addEventListener('visibilitychange', refresh)
})
onUnmounted(() => {
  ++epoch; alive = false
  window.removeEventListener('focus', refresh); window.removeEventListener('online', refresh)
  document.removeEventListener('visibilitychange', refresh)
})
async function open(item: Notification): Promise<void> {
  const started = epoch, owner = session.user?.id
  if (await store.markRead(item.id) && epoch === started && owner === session.user?.id)
    await router.push(notificationTarget(item))
}
</script>
<template>
  <section class="page stack">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="notifications-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">通知</h1>
      <p class="subpage-header__subtitle">通知记录当时的事件，当前处理状态请在对应页面查看。</p>
    </header>
    <div class="card stack">
      <label><input type="checkbox" :checked="store.unreadOnly" @change="store.load(!store.unreadOnly)">只看未读</label>
      <button class="btn btn--small" type="button" @click="refresh">刷新</button>
      <p v-if="store.status === 'loading'" role="status">正在读取通知…</p>
      <p v-else-if="store.status === 'error'" role="alert">{{ store.errorText }}</p>
      <p v-else-if="store.status === 'ready' && !store.items.length">当前列表没有通知。</p>
      <ul class="list">
        <li v-for="item in store.items" :key="item.id" class="list__item">
          <div class="list__main">
            <p>{{ item.read_at ? '已读' : '未读' }} · {{ notificationLabel(item) }}</p>
            <time :datetime="item.created_at">{{ new Date(item.created_at).toLocaleString('zh-CN') }}</time>
            <p v-if="store.reading[item.id]?.error" class="hint hint--error" role="alert">已读结果未确认：{{ store.reading[item.id]?.error }}</p>
            <button class="btn btn--small" type="button" :disabled="store.reading[item.id]?.status === 'submitting'" @click="open(item)">
              {{ store.reading[item.id]?.status === 'unknown' ? '重试已读并查看' : item.target_kind === 'account' ? '确认已读' : item.target_kind === 'task' ? '前往任务页查看' : item.target_kind === 'reward' ? '前往奖励页查看' : item.target_kind === 'message' ? '查看留言' : item.target_kind === 'memory' ? '查看回忆' : item.target_kind === 'shared_plan' ? '查看计划' : item.target_kind === 'album' ? '查看相册' : '查看纪念日' }}
            </button>
          </div>
        </li>
      </ul>
      <p v-if="store.moreError" role="alert">{{ store.moreError }}</p>
      <button v-if="store.nextCursor" class="btn" type="button" :disabled="store.loadingMore" @click="store.loadMore()">{{ store.loadingMore ? '读取中…' : '加载更多' }}</button>
    </div>
  </section>
</template>
