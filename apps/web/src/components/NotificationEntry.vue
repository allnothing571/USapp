<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { useSessionStore } from '@/stores/session'
import { useNotificationsStore } from '@/stores/notifications'
const route = useRoute(), session = useSessionStore(), notifications = useNotificationsStore()
const allowed = new Set(['home', 'tasks', 'rewards', 'us', 'me', 'notifications', 'space-create', 'space-join', 'invite'])
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
  <Transition name="top-bar">
    <nav v-if="visible" class="notification-entry" aria-label="站内通知">
    <RouterLink to="/notifications" class="notification-pill">
      <svg class="notification-pill__icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
        <path d="M6 9a6 6 0 1 1 12 0c0 5 2 6.5 2 6.5H4s2-1.5 2-6.5" />
        <path d="M10 19a2 2 0 0 0 4 0" />
      </svg>
      <span>通知</span>
      <span
        v-if="notifications.unreadCount !== null && notifications.unreadCount > 0"
        class="notification-pill__badge"
      >未读 {{ notifications.unreadCount }}</span>
      <span
        v-else-if="notifications.unreadCount === 0"
        class="notification-pill__zero"
      >（未读 0）</span>
    </RouterLink>
    <button v-if="notifications.countError" type="button" class="btn btn--small" @click="refresh">未读数读取失败，重试</button>
  </nav>
  </Transition>
</template>
