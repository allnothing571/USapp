import { fileURLToPath, URL } from 'node:url';

import vue from '@vitejs/plugin-vue';
import { defineConfig } from 'vitest/config';

/**
 * 管理界面构建配置。
 *
 * - base 固定 `/admin/`：管理界面与 API 同源部署（PUBLIC_ORIGIN），
 *   但挂在 `/admin/` 子路径下，绝不与情侣端 apps/web 抢根路径。
 * - 开发服务器强绑定回环地址 + strictPort，避免"起了第二个端口还以为在跑"。
 * - 不使用 PWA 插件：管理界面是运维工具，禁止被 Service Worker 缓存。
 */
export default defineConfig({
  plugins: [vue()],

  base: '/admin/',

  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },

  server: {
    host: '127.0.0.1',
    port: 5174,
    strictPort: true,
  },

  preview: {
    host: '127.0.0.1',
    port: 5174,
    strictPort: true,
  },

  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: true,
    target: 'es2022',
  },

  test: {
    environment: 'jsdom',
    include: ['tests/unit/**/*.spec.ts'],
    restoreMocks: true,
  },
});
