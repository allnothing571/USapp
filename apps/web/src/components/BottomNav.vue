<script setup lang="ts">
/**
 * 底部导航（一级门户层专用）：首页 / 任务 / 奖励 / 我们 / 我的。
 *
 * 严格只在已认证、已加入空间且处于五大一级门户页时显示；进入任意二级功能专页、小房间、花园、
 * 装扮或引导/404 页面时自动隐藏，由各二级页顶部的返回栏负责导航。
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

const session = useSessionStore()
const route = useRoute()

const current = computed(() => (typeof route.name === 'string' ? route.name : ''))

const visible = computed(() =>
  session.isAuthenticated && session.hasSpace && NAV_ITEMS.some((item) => item.name === current.value),
)
</script>

<template>
  <Transition name="bottom-dock">
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
      <svg
        v-if="item.name === 'home'"
        class="bottom-nav__icon"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.9"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
      >
        <path d="M3 10.5 12 3l9 7.5" />
        <path d="M5.5 9.5V20h13V9.5" />
        <path d="M9.5 20v-5.5h5V20" />
      </svg>
      <svg
        v-else-if="item.name === 'tasks'"
        class="bottom-nav__icon"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.9"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
      >
        <rect x="4" y="4" width="16" height="16" rx="3" />
        <path d="m8.5 12 2.5 2.5L15.5 9.5" />
      </svg>
      <svg
        v-else-if="item.name === 'rewards'"
        class="bottom-nav__icon"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.9"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
      >
        <rect x="3.5" y="8" width="17" height="4" rx="1" />
        <path d="M5.5 12v8h13v-8" />
        <path d="M12 8v12" />
        <path d="M12 8c-1.5-2.8-4.8-2.6-4.8-.6 0 1.4 2.4 1.6 4.8.6Z" />
        <path d="M12 8c1.5-2.8 4.8-2.6 4.8-.6 0 1.4-2.4 1.6-4.8.6Z" />
      </svg>
      <svg
        v-else-if="item.name === 'us'"
        class="bottom-nav__icon"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.9"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
      >
        <path d="M12 20s-7-4.6-8.5-9.2C2.3 7 4.7 4 7.9 4c1.9 0 3.3 1 4.1 2.4C12.8 5 14.2 4 16.1 4c3.2 0 5.6 3 4.4 6.8C19 15.4 12 20 12 20Z" />
      </svg>
      <svg
        v-else
        class="bottom-nav__icon"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.9"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
      >
        <circle cx="12" cy="8.5" r="3.5" />
        <path d="M5 19.5c1.5-3.3 4.1-4.8 7-4.8s5.5 1.5 7 4.8" />
      </svg>
      <span>{{ item.label }}</span>
    </RouterLink>
  </nav>
  </Transition>
</template>
