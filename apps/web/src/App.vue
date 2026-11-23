<script setup lang="ts">
/**
 * 应用外壳。
 * - 顶部离线提示条（OfflineBanner）
 * - 新版本可用提示（prompt 模式下由用户确认刷新）
 * - RouterView 承载各页面
 * - 底部安装引导（InstallPrompt）
 * - 底部导航（BottomNav，M10；只在首页 / 任务 / 奖励 / 我的显示）
 * - 启动时调用 session store 的 bootstrap（与路由守卫共用同一个在途请求，不会重复打接口）
 */
import { onMounted } from 'vue'

import NotificationEntry from '@/components/NotificationEntry.vue'
import BottomNav from '@/components/BottomNav.vue'
import InstallPrompt from '@/components/InstallPrompt.vue'
import OfflineBanner from '@/components/OfflineBanner.vue'
import { setupServiceWorker } from '@/pwa/register-sw'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const { needRefresh, offlineReady, update: applyUpdate, dismiss: dismissUpdate } = setupServiceWorker()

onMounted(() => {
  void session.bootstrap()
})
</script>

<template>
  <div class="app-shell">
    <a href="#main-content" class="skip-link">跳到主要内容</a>
    <OfflineBanner />

    <div v-if="needRefresh" class="app-notice app-notice--accent" role="status">
      <span class="app-notice__text">有新版本可用</span>
      <button type="button" class="app-notice__action" @click="applyUpdate()">立即刷新</button>
      <button type="button" class="app-notice__ghost" @click="dismissUpdate()">稍后</button>
    </div>

    <div v-else-if="offlineReady" class="app-notice" role="status">
      <span class="app-notice__text">已可离线打开</span>
      <button type="button" class="app-notice__ghost" @click="dismissUpdate()">知道了</button>
    </div>

    <NotificationEntry />

    <main class="app-main" id="main-content" tabindex="-1">
      <RouterView />
    </main>

    <InstallPrompt />

    <BottomNav />
  </div>
</template>
