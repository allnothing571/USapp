<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { useSessionStore } from '@/stores/session'
import { useNotificationsStore } from '@/stores/notifications'
const route = useRoute(), session = useSessionStore(), notifications = useNotificationsStore()
const allowed = new Set(['home','tasks','rewards','us','message-detail','anniversary-detail','me','notifications','space-create','space-join','invite'])
const visible = computed(() => session.isAuthenticated && allowed.has(String(route.name)))
let pending: Promise<void> | null = null
let stopped = false
function refresh(): void {
  if (stopped || !visible.value || document.visibilityState === 'hidden' || pending) return
  const request = notifications.loadCount()
  pending = request
  void request.finally(() => { if (pending === request) pending = null })
}
watch(() => session.user?.id, () => { pending = null; refresh() })
watch(() => route.name, refresh)
onMounted(() => {
  refresh(); window.addEventListener('focus', refresh); window.addEventListener('online', refresh)
  document.addEventListener('visibilitychange', refresh)
})
onUnmounted(() => {
  stopped = true; window.removeEventListener('focus', refresh); window.removeEventListener('online', refresh)
  document.removeEventListener('visibilitychange', refresh)
})
</script>
<template>
  <nav v-if="visible" class="notification-entry" aria-label="站内通知">
    <RouterLink to="/notifications">通知<span v-if="notifications.unreadCount !== null">（未读 {{ notifications.unreadCount }}）</span></RouterLink>
    <button v-if="notifications.countError" type="button" class="btn btn--small" @click="refresh">未读数读取失败，重试</button>
  </nav>
</template>
