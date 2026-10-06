# PWA 缓存策略（Service Worker）

> 本文档描述 `apps/web` 中 Service Worker 的缓存边界。**本文档中的所有验证步骤都是给项目负责人手动执行的**，
> 本次写作**没有运行任何构建、没有启动任何服务、没有打开过浏览器 DevTools**，因此没有一条策略被实际观测过。
> 策略本身来自源码静态阅读，不是运行结果。

---

## 1. 权威来源

| 文件 | 角色 |
| --- | --- |
| `apps/web/src/pwa/cache-policy.ts` | **唯一**事实来源：黑名单前缀、外壳 `destination` 白名单、扩展名兜底、`isCacheableRequest()` |
| `apps/web/src/pwa/sw.ts` | `injectManifest` 入口：注册路由、预缓存、更新语义 |
| `apps/web/src/pwa/register-sw.ts` | 页面侧注册与"新版本可用"提示 |
| `apps/web/vite.config.ts` | `strategies: 'injectManifest'`、`filename: 'sw.ts'`、`registerType: 'prompt'`、`globPatterns` |
| `apps/web/src/api/client.ts` | 离线守卫、`X-CSRF-Token`、`credentials: 'same-origin'` |
| `apps/web/src/stores/session.ts` | 注销时的内存态清理 |
| `contracts/openapi.yaml`（`components/headers/NoStore`、`headers.SetSessionCookie`） | `no-store` 契约与 Cookie 属性 |

`cache-policy.ts` 的注释明确写了这样做的动机：规则只写一份，
`sw.ts` 与 `apps/web/tests/unit/sw-cache-policy.spec.ts` **复用同一实现**，
避免"测试里一套、运行时另一套"的假安全感。

---

## 2. 规则表

常量取值（`cache-policy.ts`）：

- 外壳缓存名：`SHELL_CACHE_NAME = 'usapp-shell-v1'`
- 永不缓存前缀：`SENSITIVE_PREFIXES = ['/api/', '/media/', '/private/', '/uploads/']`
- 允许进缓存的 `request.destination`：`['document', 'script', 'style', 'font', 'image']`
- `destination` 缺失时的扩展名兜底：`.js .mjs .css .html .svg .png .jpg .jpeg .webp .ico .woff .woff2 .webmanifest`

| # | URL / 请求形态 | 策略 | 依据 |
| --- | --- | --- | --- |
| 1 | `/api`、`/api/**`（任意方法） | **NetworkOnly，永不缓存** | `sw.ts` 第 53 行先注册黑名单路由 |
| 2 | `/media/**`、`/private/**`、`/uploads/**` | **NetworkOnly，永不缓存** | 同上（阶段 3 的私有媒体前缀，边界提前写死） |
| 3 | 任何非 `GET` 请求（POST/PUT/PATCH/DELETE） | **永不经过缓存层**，直接走网络 | `isCacheableRequest()` 首个判断：`method !== 'GET'` 直接 `false`（`cache-policy.ts` 第 100 行） |
| 4 | 同源 GET，且 `request.destination ∈ {document, script, style, font, image}`，且不在第 1–2 行前缀内 | `StaleWhileRevalidate`，缓存名 `usapp-shell-v1` | `sw.ts` 第 56–63 行 → `cache-policy.ts` 第 95–111 行 |
| 5 | 同源 GET，`destination` 为空，但路径以第 2 节扩展名结尾 | 同上（走扩展名兜底） | `cache-policy.ts` 第 109–110 行 |
| 6 | 跨源请求（`url.origin !== self.location.origin`） | 不缓存 | `sw.ts` 第 59 行传入 `sameOrigin: url.origin === self.location.origin`，`cache-policy.ts` 第 101 行 |
| 7 | 构建期预缓存清单 `self.__WB_MANIFEST`（由 `vite-plugin-pwa` 注入，`globPatterns: ['**/*.{js,css,html,svg,png,woff2}']`） | `precacheAndRoute()` 预缓存，`cleanupOutdatedCaches()` 清理旧版本条目 | `sw.ts` 第 46、66–67 行；`vite.config.ts` 第 63 行 |
| 8 | 导航请求且网络失败 | 兜底返回预缓存的 `/index.html`（或 `index.html`）；确实找不到则返回 `Response.error()` | `sw.ts` 第 72–78 行的 `setCatchHandler` |
| 9 | 其它同源 GET（既不命中黑名单，也不满足白名单，例如对非 `/api` 路径的 XHR/fetch 且 `destination` 为空、扩展名不在名单） | 不匹配任何路由 → 浏览器直连网络，**不进缓存** | 路由只注册了两条（`sw.ts` 第 53、56 行） |

补充要点（来自 `sw.ts` 的注释与实现）：

- 黑名单路由**先于**白名单注册，目的是"即使某个匹配条件写错也不会漏到缓存层"。
- 导航兜底**刻意不用** `NavigationRoute`：那会为所有导航（含 `/api/**`）抢占匹配，
  而 `/api/**` 必须留在 NetworkOnly 上。
- `sw.ts` **不调用** `clientsClaim()`，因此新 SW 不会自动接管已有页面。

---

## 3. 为什么 `/api/**` 与私有媒体绝不能缓存

1. **共享设备上的数据泄漏。** Cache Storage 是落在磁盘上的持久副本。
   登出只清理内存态与 Cookie，**不会**删除 `Cache Storage` 里已缓存的响应体；
   同一台设备上的另一个浏览器 profile / 另一个账号在离线或竞态下可能读回上一位用户的情侣业务数据。
2. **陈旧的鉴权状态。** 会话会被撤销、CSRF 令牌会在登录成功时轮换（见
   `contracts/openapi.yaml` 中 `createSession` 的说明与 `SessionBootstrap.csrf_token`）。
   若 API 响应被缓存，前端可能拿着"已经不该存在"的响应继续渲染已登录界面。
3. **违反 `no-store` 契约。** 契约把 `Cache-Control: no-store, no-cache, must-revalidate`
   定义为**所有** API 响应的固定头（`contracts/openapi.yaml` 的 `components/headers/NoStore`）。
   Service Worker 缓存层位于 HTTP 缓存语义**之上**，一旦缓存就等于单方面废除该契约。
4. **与"业务数据不落盘"的整体决策一致。** 黑名单与前端不使用任何持久化存储是同一个决策的两面。

---

## 4. 注销时清除了什么、刻意不持久化什么

### 清除（内存态）

`apps/web/src/stores/session.ts` 的 `logout()`（第 165–182 行）：

1. 调用 `authApi.deleteSession()`（即 `DELETE /api/v1/auth/sessions`）。
2. **无论服务端是否成功**，`finally` 中都执行 `resetBusinessState()`（第 70–79 行）：
   清空 `user`、`space`、`csrfToken`、`preAuthExpiresAt`，调用 `setCsrfToken(null)`
   清掉 HTTP 层持有的令牌副本，`status` 回到 `'anonymous'`，并调用 `useSpaceStore().reset()`。
3. 若服务端撤销失败，只打印警告，并说明"服务端会话可能仍然有效"——**不谎报成功**。

注释还要求：新增业务 store 时必须把它的 reset 一并登记进 `resetBusinessState()`。

### 刻意不持久化

| 数据 | 位置 | 原因 |
| --- | --- | --- |
| 会话/用户/空间等业务数据 | 仅 Vue 内存态（Pinia store） | `session.ts` 第 5–6 行明示：**不使用** pinia 持久化插件，不写 `localStorage` / `sessionStorage` / cookie |
| CSRF 令牌 | `apps/web/src/api/csrf.ts` 的内存持有者 | 令牌必须能在注销/切换账号时被立即丢弃；持久化会让它在会话轮换后仍然残留 |
| 业务数据到 IndexedDB | 不存在 | 与 Service Worker 黑名单同一决策：情侣业务数据不落盘 |

### 离线行为

- 提示：`apps/web/src/components/OfflineBanner.vue` 基于 `navigator.onLine` 与 `online`/`offline`
  事件显示"当前离线，只读浏览；业务操作不会提交"（`role="status"`）。
- 真正的拦截在 `apps/web/src/api/client.ts`：离线时任何**非 GET** 请求直接抛
  本地离线错误（`client.ts` 第 82–104 行附近），并在请求管线上按
  "离线守卫 → CSRF 守卫 → 真正 fetch"的顺序执行（`client.ts` 第 162 行附近）。
  因此离线不会把业务写请求发出去，也不会静默排队。

---

## 5. 浏览器 DevTools 手动验证程序（由负责人手动执行）

前提：已在 `https://localhost:8443` 打开应用，并且**至少做过一次生产构建**
（开发模式下 `register-sw.ts` 第 32 行会因 `import.meta.env.DEV` 而**不注册** Service Worker，
`vite.config.ts` 也未开启 `devOptions`，此时 `/sw.js` 并不存在）。

1. **Service Workers 面板**：DevTools → `Application` → `Service Workers`。
   预期：存在一个来自 `/sw.js` 的注册，状态为 `activated and is running`；
   `Update on reload` 保持**关闭**（否则会掩盖 prompt 语义）。
2. **Cache Storage 面板**：`Application` → `Cache Storage` → `usapp-shell-v1`。
   预期：条目只包含静态外壳资源（`index.html`、`assets/*.js`、`assets/*.css`、图标、字体等）。
   在过滤框里输入 `api`、`media`、`private`、`uploads`：预期 **0 条命中**。
3. **API 不缓存的运行时验证**：`Network` 面板刷新页面后点击任意会请求
   `/api/v1/session` 的操作。预期该请求：
   - `Size` 列**不显示** `(ServiceWorker)` 或 `(disk cache)`；
   - 响应头包含 `cache-control: no-store, no-cache, must-revalidate`；
   - 在 `Application` → `Cache Storage` 中搜不到它的响应体。
4. **写请求不经过 SW**：执行一次登出或创建空间。预期对应 `POST`/`DELETE` 请求
   在 `Network` 面板的 `Size` 列没有任何缓存来源标记。
5. **离线外壳**：`Network` 面板切换到 `Offline`，刷新页面。
   预期：页面仍能加载（来自预缓存的外壳），顶部出现离线提示条；
   任何业务操作不提交（应在 Network 面板看到请求根本没发出，或被本地守卫拦下）。
6. **确认没有业务数据落盘**：`Application` → `IndexedDB` 与 `Local Storage` / `Session Storage`。
   预期：不存在存放情侣业务数据的数据库或键值。
7. **注销后不可回退读取**：登录后登出，然后按浏览器 `Back` 按钮回到上一页。
   预期：不会渲染出业务数据（内存态已清空）。若仍看到数据，说明有绕过内存态的持久化或
   缓存，需要作为缺陷记录（详见 `tests/e2e/README.md` 的第 7 步）。

---

## 6. 升级与提示行为

- `vite.config.ts` 设置 `registerType: 'prompt'`（第 26 行），策略为 `injectManifest`
  （第 22 行）、`filename: 'sw.ts'`（第 24 行）。
- `sw.ts` **不**调用 `clientsClaim()`：新 Service Worker 安装后停在 `waiting`，
  只有页面显式发送 `{ type: 'SKIP_WAITING' }` 才调用 `self.skipWaiting()` 接管（`sw.ts` 第 80–86 行）。
- `register-sw.ts` 暴露 `needRefresh` / `offlineReady` 两个只读状态与 `update()` / `dismiss()`：
  - `needRefresh` 为真时由 **UI 询问用户**，不自动刷新；
  - `update()` 调用 `updateSW(true)`（内部会 `postMessage` 再 reload）；
  - `dismiss()` 只是把提示置为 false，不改变 SW 状态；
  - 开发模式或浏览器不支持时只打印一条 info 日志，不注册。
- `cleanupOutdatedCaches()` 会清理旧版本插件产生的预缓存条目。
- 改名 `SHELL_CACHE_NAME`（例如 `usapp-shell-v1` → `usapp-shell-v2`）
  **即等于让旧缓存整体失效**——这是 `cache-policy.ts` 第 20–21 行写明的意图。

## 7. 手动强制清除缓存

任选其一（均由负责人手动执行）：

1. **DevTools 一站式清除**：`Application` → `Storage` → `Clear site data`
   （勾选 `Service workers`、`Cache storage`），然后刷新页面。
2. **分步清除**：`Application` → `Service Workers` → `Unregister`；
   `Application` → `Cache Storage` → 右键删除 `usapp-shell-v1`；再刷新页面重新注册。
3. **代码级失效**：提升 `SHELL_CACHE_NAME` 的版本号或 `vite.config.ts` 中的构建产物哈希，
   重新构建部署，让旧缓存整体被判定过期。
4. **硬刷新**：`Ctrl` + `Shift` + `R` 只绕过 HTTP 缓存，**不**卸载 Service Worker
   与 Cache Storage，因此它**不能**单独解决 SW 层的残留问题。

---

## 8. 已实现 / 仅骨架 / 尚未实现

### 已实现

- `apps/web/src/pwa/cache-policy.ts`：完整、可单测的判定函数（`isSensitivePath`、
  `isCacheableRequest`、`toPathname`）与常量。
- `apps/web/src/pwa/sw.ts`：黑名单路由、外壳路由、预缓存、导航离线兜底、`SKIP_WAITING` 处理。
- `apps/web/src/pwa/register-sw.ts`：注册与 `needRefresh` / `offlineReady` 控制接口。
- `apps/web/src/stores/session.ts`：`resetBusinessState()` 与 fail-closed 的 `logout()`。
- `apps/web/src/components/OfflineBanner.vue`：离线提示条。
- `apps/web/src/api/client.ts`：离线守卫、CSRF 头、`credentials: 'same-origin'`。
- 单测文件 `apps/web/tests/unit/sw-cache-policy.spec.ts`（**未执行**）。
- 服务端的另一半策略：`deploy/Caddyfile` 对 `/api/**` **不设置任何缓存相关响应头**
  （注释明确写明"由应用决定，业务响应必须是 `no-store`"，且不对 `/api/**` 启用 encode），
  并只对带内容哈希的静态资源（`/assets/*`、`/admin/assets/*`、`/icons/*`、`/favicon.svg`）
  下发 `public, max-age=31536000, immutable`，对入口 HTML、
  `/sw.js`、`/registerSW.js`、`/manifest.webmanifest` 下发 `no-cache`。
  这一段 Caddyfile **从未被加载验证过**（见 `docs/architecture.md` 第 1 节）。

### 仅骨架

- `register-sw.ts` 只提供状态与操作接口，**没有**任何实际 UI 呈现
  （谁渲染"新版本可用"的弹窗由页面决定；本次未逐一核对各 `views/*` 是否接入了它）。
- `OfflineBanner.vue` 只做提示，不做排队/重试——这是有意的，离线业务写请求应被拒绝而非缓存。

### 尚未实现

- 私有媒体（`/media/**`、`/uploads/**`）**在阶段 0 并不存在**：这些前缀只是提前写死的边界。
- 没有任何自动化的离线/缓存集成测试（没有浏览器自动化依赖，见 `tests/e2e/README.md`）。
- 没有推送（Push）与后台同步相关的 Service Worker 处理器。

---

## 9. 尚未执行的检查

1. 未执行 `pnpm --filter apps/web build` 或任何构建，因此 `sw.js` 从未被生成，
   `__WB_MANIFEST` 的实际内容**未知**。
2. 未执行 `pnpm --filter apps/web test`，`apps/web/tests/unit/sw-cache-policy.spec.ts` 未运行。
3. 未在浏览器中注册过 Service Worker，第 5 节全部 7 步验证均**未执行**。
4. 未观测过任何 `/api/**` 响应的实际 `Cache-Control` 头（该头来自契约定义，不是运行结果）。
5. 未验证 `registerType: 'prompt'` 的实际提示行为（未构造过一次真实的版本更新）。
6. 未验证离线拦截的实际表现（`navigator.onLine === false` 路径只在源码中被读到）。
7. 未检查过任何真实 Cache Storage 内容，因此"缓存中不含 API 响应"这一结论
   来自**静态规则阅读**，不是实测。
