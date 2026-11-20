<script setup lang="ts">
/**
 * 底部导航（阶段 1 M10，D16；阶段 2 P2-1 加入“我们”，V1 §47）：首页 / 任务 / 奖励 / 我们 / 我的。
 *
 * 只在已认证、已加入空间、且当前页属于这五个入口时显示（留言详情与纪念日详情属于“我们”）；登录、创建/加入空间、
 * 邀请与 404 页面不显示。当前入口标记 aria-current="page"（只有一个）。
 * 导航在文档流的最后并贴住视口底部（sticky），不会遮住页面末尾的内容；底部留出安全区。
 */
import { computed } from 'vue'
import { useRoute } from 'vue-router'

import { useSessionStore } from '@/stores/session'

const NAV_ITEMS = [
  { name: 'home', label: '首页' },
  { name: 'tasks', label: '任务' },
  { name: 'rewards', label: '奖励' },
  { name: 'us', label: '我们' },
  { name: 'me', label: '我的' },
] as const

/** Sub-pages that belong to a navigation destination. */
const SECTION_OF: Record<string, string> = {
  'message-detail': 'us', 'anniversary-detail': 'us', memories: 'us', 'memory-new': 'us', 'memory-detail': 'us', albums: 'us', 'album-detail': 'us',
}

const session = useSessionStore()
const route = useRoute()

const current = computed(() => {
  const name = typeof route.name === 'string' ? route.name : ''
  return SECTION_OF[name] ?? name
})

const visible = computed(() =>
  session.isAuthenticated && session.hasSpace && NAV_ITEMS.some((item) => item.name === current.value),
)
</script>

<template>
  <nav v-if="visible" class="bottom-nav" aria-label="主导航" data-test="bottom-nav">
    <RouterLink
      v-for="item in NAV_ITEMS"
      :key="item.name"
      class="bottom-nav__item"
      :class="{ 'bottom-nav__item--active': item.name === current }"
      active-class=""
      exact-active-class=""
      :aria-current="item.name === current ? 'page' : undefined"
      :to="{ name: item.name }"
      :data-test="`nav-${item.name}`"
    >
      {{ item.label }}
    </RouterLink>
  </nav>
</template>
