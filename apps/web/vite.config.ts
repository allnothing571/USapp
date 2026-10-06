import { fileURLToPath, URL } from 'node:url'

import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vite'
import { VitePWA } from 'vite-plugin-pwa'

/**
 * USapp Web 客户端（PWA）构建配置。
 *
 * 关键约束（与 contracts/openapi.yaml 对齐）：
 * - Web 与 API 同源部署，因此不做任何跨域代理；`VITE_API_BASE` 默认留空表示同源。
 * - Service Worker 用 injectManifest 手写（src/pwa/sw.ts），因为缓存白名单/黑名单
 *   必须严格区分 `/api/**`（no-store，永不可缓存）与静态外壳。
 * - `registerType: 'prompt'`：新版本就绪时不自动接管，必须由用户确认刷新。
 * - `injectRegister: 'auto'`：src/pwa/register-sw.ts 已手动 import 'virtual:pwa-register'，
 *   插件据此判定"已手动注册"，不会再往 index.html 注入自动注册脚本。
 */
export default defineConfig({
  plugins: [
    vue(),
    VitePWA({
      strategies: 'injectManifest',
      srcDir: 'src/pwa',
      filename: 'sw.ts',
      injectRegister: 'auto',
      registerType: 'prompt',
      includeAssets: ['favicon.svg', 'icons/*.png', 'apple-touch-icon.png'],
      manifest: {
        name: '我们的小空间',
        short_name: '小空间',
        description: '只属于两个人的小空间：共享日程、奖励与留言。阶段 0 只交付登录、会话与情侣空间。',
        start_url: '/',
        scope: '/',
        display: 'standalone',
        orientation: 'portrait',
        theme_color: '#FBF7F0',
        background_color: '#FBF7F0',
        lang: 'zh-CN',
        icons: [
          {
            src: '/icons/icon-192.png',
            sizes: '192x192',
            type: 'image/png',
            purpose: 'any',
          },
          {
            src: '/icons/icon-512.png',
            sizes: '512x512',
            type: 'image/png',
            purpose: 'any',
          },
          {
            src: '/icons/icon-maskable-512.png',
            sizes: '512x512',
            type: 'image/png',
            purpose: 'maskable',
          },
        ],
      },
      injectManifest: {
        // 只预缓存构建产物里的静态外壳。API / 私有媒体永远不在 glob 覆盖范围内，
        // 即便被匹配到，sw.ts 的 NetworkOnly 规则也会先命中。
        globPatterns: ['**/*.{js,css,html,svg,png,woff2}'],
        // 排除项说明：
        // - sw.js 自身不能进预缓存清单，否则每次更新都会自引用。
        // - icons/、apple-touch-icon.png、favicon.svg 由 includeAssets 单独登记，
        //   这里排除是为了避免同一 URL 在预缓存清单里出现两次。
        globIgnores: [
          '**/sw.js',
          '**/sw.js.map',
          '**/icons/**',
          '**/apple-touch-icon.png',
          '**/favicon.svg',
        ],
      },
    }),
  ],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
  build: {
    target: 'es2022',
    sourcemap: false,
  },
})
