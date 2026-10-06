<script setup lang="ts">
import { ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';

import { useAdminSessionStore } from '@/stores/admin-session';

const session = useAdminSessionStore();
const route = useRoute();
const router = useRouter();

const username = ref('');
const password = ref('');

async function onSubmit(): Promise<void> {
  const ok = await session.login(username.value.trim(), password.value);
  if (!ok) return;

  // 密码用完即弃：不留在内存里等一次 XSS 来读。
  password.value = '';

  const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : null;
  await router.push(redirect ?? { name: 'overview' });
}
</script>

<template>
  <div class="login-wrap">
    <div class="login-card">
      <h1>USapp 管理后台</h1>

      <div class="notice info">
        仅本地可用：管理入口只绑定回环地址（127.0.0.1），不接受来自公网的访问。
        管理员账号独立于情侣账号体系，与本机开发环境之外的用户无关。
      </div>

      <div v-if="session.errorMessage" class="notice error" role="alert">
        {{ session.errorMessage }}
        <span v-if="session.errorCode" class="mono">[{{ session.errorCode }}]</span>
      </div>

      <form @submit.prevent="onSubmit">
        <div class="field">
          <label for="admin-username">用户名</label>
          <input
            id="admin-username"
            v-model="username"
            type="text"
            name="username"
            autocomplete="username"
            autocapitalize="none"
            spellcheck="false"
            required
          />
        </div>

        <div class="field">
          <label for="admin-password">密码</label>
          <input
            id="admin-password"
            v-model="password"
            type="password"
            name="password"
            autocomplete="current-password"
            required
          />
        </div>

        <button
          class="btn primary"
          type="submit"
          :disabled="session.submitting || username.length === 0 || password.length === 0"
        >
          {{ session.submitting ? '登录中…' : '登录' }}
        </button>
      </form>

      <p class="faint" style="font-size: 11px; margin-bottom: 0">
        会话 Cookie 为 <span class="mono">__Host-usapp-admin</span>（HttpOnly，前端不可读）。
        页面不做任何权限判断；服务端对每个请求独立校验来源、会话与 CSRF 令牌。
      </p>
    </div>
  </div>
</template>
