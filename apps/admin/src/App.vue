<script setup lang="ts">
import { computed } from 'vue';
import { RouterLink, RouterView, useRouter } from 'vue-router';

import { useAdminSessionStore } from '@/stores/admin-session';

const session = useAdminSessionStore();
const router = useRouter();

const buildLabel = import.meta.env.VITE_ADMIN_BUILD_LABEL ?? 'local';

const navItems = [
  { name: 'overview', label: '总览' },
  { name: 'users', label: '用户' },
  { name: 'couples', label: '情侣空间' },
  { name: 'audit', label: '审计日志' },
  { name: 'avatar-catalog', label: '阳光商店' },
] as const;

const showChrome = computed(() => session.authenticated);

async function onLogout(): Promise<void> {
  await session.logout();
  await router.push({ name: 'login' });
}
</script>

<template>
  <div class="app-shell">
    <header class="app-header">
      <div class="app-brand">
        USapp 管理后台
        <span class="tag placeholder">本地运维</span>
      </div>

      <nav v-if="showChrome" class="app-nav" aria-label="主导航">
        <RouterLink v-for="item in navItems" :key="item.name" :to="{ name: item.name }">
          {{ item.label }}
        </RouterLink>
      </nav>
      <div v-else class="app-nav" />

      <div class="app-header-meta">
        <template v-if="showChrome">
          <span class="mono">{{ session.adminLabel ?? '管理员' }}</span>
          <button class="btn" type="button" :disabled="session.submitting" @click="onLogout">
            注销
          </button>
        </template>
        <span v-else class="mono">未登录</span>
      </div>
    </header>

    <main class="app-main">
      <RouterView />
    </main>

    <footer class="app-footer">
      <span class="mono">build:{{ buildLabel }}</span>
      ·
      <span>本地运维界面；权限判定由服务端负责，商品修改记录审计。</span>
    </footer>
  </div>
</template>
