<script setup lang="ts">
/**
 * 离线提示条。
 *
 * 只用 `navigator.onLine` + online/offline 事件，不猜"服务器是否可达"：
 * 浏览器报告的在线状态就是唯一依据。离线时提示条固定在顶部，明确告知用户
 * 当前是只读浏览，任何业务写操作都不会提交（真正的拦截在 api/client.ts 里）。
 */
import { onBeforeUnmount, onMounted, ref } from 'vue'

const isOffline = ref(typeof navigator !== 'undefined' && navigator.onLine === false)

function goOnline(): void {
  isOffline.value = false
}

function goOffline(): void {
  isOffline.value = true
}

onMounted(() => {
  // 事件可能在挂载前就触发过，这里以当前值再校准一次。
  isOffline.value = navigator.onLine === false
  window.addEventListener('online', goOnline)
  window.addEventListener('offline', goOffline)
})

onBeforeUnmount(() => {
  window.removeEventListener('online', goOnline)
  window.removeEventListener('offline', goOffline)
})
</script>

<template>
  <div v-if="isOffline" class="offline-banner" role="status" aria-live="polite">
    当前离线，只读浏览；业务操作不会提交
  </div>
</template>
