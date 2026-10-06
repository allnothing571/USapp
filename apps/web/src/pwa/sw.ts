/**
 * USapp Service Worker（vite-plugin-pwa `injectManifest` 入口）。
 *
 * ══ 缓存白名单 / 黑名单（硬约束，来自 contracts/openapi.yaml）════════
 *
 * 黑名单（永不进缓存，一律 NetworkOnly）：
 *   - `/api/**`      所有 API 响应都是 `Cache-Control: no-store`；缓存会把情侣业务数据
 *                    留在设备上，且会破坏 CSRF 令牌的"每次登录轮换"语义。
 *   - `/media/**`    阶段 3 的私有 OSS 媒体前缀，提前把边界写死。
 *   - `/private/**`  私有资源前缀。
 *   - `/uploads/**`  上传资源前缀。
 *   - 任何非 GET 请求（POST/PUT/PATCH/DELETE 永不经过缓存层）。
 *
 * 白名单（静态外壳，StaleWhileRevalidate，缓存名 `usapp-shell-v1`）：
 *   - 同源 GET，且 `request.destination` ∈ {document, script, style, font, image}
 *   - 且 URL 不落在上面的黑名单前缀里
 *
 * 规则实现只有一份，在 ./cache-policy.ts；本文件与单测共用。
 *
 * ══ 更新语义 ══════════════════════════════════════════════════
 * 配合 `registerType: 'prompt'`：**不**调用 clientsClaim()，新 SW 保持 waiting 状态，
 * 只有页面显式 postMessage({type:'SKIP_WAITING'}) 才接管。
 * 这是本文件里唯一允许的自动更新入口（`self.skipWaiting()`）。
 * ══════════════════════════════════════════════════════════════
 */
import { cleanupOutdatedCaches, matchPrecache, precacheAndRoute } from 'workbox-precaching'
import { registerRoute, setCatchHandler } from 'workbox-routing'
import { NetworkOnly, StaleWhileRevalidate } from 'workbox-strategies'

import { SHELL_CACHE_NAME, isCacheableRequest, isSensitivePath } from './cache-policy'

/**
 * sw.ts 是 tsconfig 里唯一的 Service Worker 文件。这里**不**引入 `lib: ["WebWorker"]`
 * （会与 DOM lib 冲突），改为就地声明所需的最小 self 形状。
 */
interface ServiceWorkerScope {
  readonly location: Location
  /** 由 vite-plugin-pwa 在构建期注入的预缓存清单占位符。 */
  readonly __WB_MANIFEST: Array<{ url: string; revision: string | null }>
  addEventListener(type: 'message', listener: (event: MessageEvent) => void): void
  skipWaiting(): Promise<void>
}

declare const self: ServiceWorkerScope

const precacheManifest = self.__WB_MANIFEST

const networkOnly = new NetworkOnly()
const shellStrategy = new StaleWhileRevalidate({ cacheName: SHELL_CACHE_NAME })

// ── 1) 黑名单优先：/api、/media、/private、/uploads 一律直连网络 ──────────
// 放在静态外壳规则**之前**注册，保证即使某个匹配条件写错也不会漏到缓存层。
registerRoute(({ url }) => isSensitivePath(url.pathname), networkOnly)

// ── 2) 白名单：静态外壳走 StaleWhileRevalidate ───────────────────────────
registerRoute(
  ({ url, request }) =>
    isCacheableRequest(url.toString(), request.method, {
      sameOrigin: url.origin === self.location.origin,
      destination: request.destination,
    }),
  shellStrategy,
)

// ── 3) 预缓存静态外壳（清单由 vite-plugin-pwa 注入）──────────────────────
cleanupOutdatedCaches()
precacheAndRoute(precacheManifest)

// ── 4) 导航离线兜底：预缓存的 index.html；确实找不到就明确失败 ─────────────
// 刻意不用 NavigationRoute：那会为所有导航（含 /api/**）抢占匹配，
// 而 /api/** 必须牢牢留在 NetworkOnly 上。
setCatchHandler(async ({ request }) => {
  if (request.destination === 'document') {
    const cached = (await matchPrecache('/index.html')) ?? (await matchPrecache('index.html'))
    if (cached) return cached
  }
  return Response.error()
})

// ── 5) registerType: 'prompt' 语义：只有页面显式要求时才跳过等待 ──────────
self.addEventListener('message', (event) => {
  const data: unknown = event.data
  if (data && typeof data === 'object' && (data as { type?: unknown }).type === 'SKIP_WAITING') {
    void self.skipWaiting()
  }
})
