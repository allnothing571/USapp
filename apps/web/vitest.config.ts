import { fileURLToPath, URL } from 'node:url'

import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vitest/config'

/**
 * 单测独立配置：刻意**不**加载 vite-plugin-pwa。
 * 这样 tests/ 下不会触碰 `virtual:pwa-register` 这类只有 PWA 插件在构建期
 * 才会生成的虚拟模块，也不会有 Service Worker 相关的副作用。
 * 与 vite.config.ts 共用同一个 `@` → ./src 别名。
 * Vue 组件测试需要现有的 Vue 插件来编译 .vue 文件。
 */
export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['tests/**/*.spec.ts'],
    restoreMocks: true,
    unstubGlobals: true,
  },
})
