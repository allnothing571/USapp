# @usapp/web

USapp 手机客户端（PWA）。阶段 0 交付范围：**账号登录 + 空间创建/加入 + 安装引导 + 离线边界**。

## 环境要求

- Node.js `>= 24`
- pnpm（`pnpm-lock.yaml` 由负责人手动生成，本目录不提交任何 lockfile）

## 命令

```bash
pnpm install          # 由负责人在仓库根目录执行
pnpm dev              # 开发服务器，固定 127.0.0.1:5173（strictPort）
pnpm build            # 类型检查 + 生产构建
pnpm preview          # 预览构建产物
pnpm typecheck        # vue-tsc --noEmit -p tsconfig.json && tsc --noEmit -p tsconfig.node.json
pnpm test             # 同 test:unit
pnpm test:unit        # vitest run
pnpm test:watch       # vitest
```

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `VITE_API_BASE` | API 基地址前缀。同源部署时留空（默认），所有请求发往当前 origin。 |
| `VITE_PUBLIC_ORIGIN` | 期望的对外 origin，例如 `https://app.example.com`。用于**同源自检**：与 `window.location.origin` 不一致时只在控制台告警。 |

`VITE_PUBLIC_ORIGIN` 不是"把请求改到别的域"的开关。会话 Cookie（`__Host-usapp-session`）与 CSRF/Origin 校验都建立在同源部署之上，不同源时正确做法是修部署入口，前端不做静默降级。

## 安全与数据边界

- 会话 Cookie 名固定 `__Host-usapp-session`，`HttpOnly`；前端**不读不写**，只依赖浏览器在 `credentials: 'same-origin'` 下自动携带。
- CSRF 令牌只存在于**内存**（`src/api/csrf.ts`），来源是 `GET /api/v1/session`。不写 localStorage / sessionStorage / cookie。
- 所有写请求（POST/PUT/PATCH/DELETE）自动带 `X-CSRF-Token`；取不到令牌时**先抛错、不发请求**。
- 离线（`navigator.onLine === false`）时任何非 GET 请求直接抛 `OfflineMutationBlockedError`，绝不发出业务写请求。
- API 响应一律 `Cache-Control: no-store`；前端不持久化任何情侣业务数据（无 persist 插件、无 IndexedDB 业务缓存）。
- 任务页（阶段 1 M3-B，`/tasks`，已实现、尚未验证）：发布请求带 `Idempotency-Key`，键只在内存中；
  结果未确认（网络错误、5xx、503 `RESULT_UNKNOWN`）时由用户点击重试，复用同一个键，不会重复发布；
  从不自动重放写请求。注销或切换账号时任务 store 与未完成的键一并清空，晚到的响应被丢弃。
  完成任务与奖励结算属于 M3-C，页面上没有完成按钮。
- `dist/` 以绑定挂载方式由 Caddy 直接提供：`pnpm build` 会立即替换线上文件，只应在后端已就绪后执行。

## PWA 缓存策略

规则的**唯一事实来源**是 `src/pwa/cache-policy.ts`，`src/pwa/sw.ts` 与单测复用同一份实现。

黑名单（永不缓存）：

- `/api/**` —— 契约要求所有 API 响应 `no-store`
- `/media/**`、`/private/**`、`/uploads/**` —— 阶段 3 私有 OSS 媒体前缀，边界先写死
- 任何非 GET 请求、任何跨源请求

白名单（`StaleWhileRevalidate`，缓存名 `usapp-shell-v1`）：

- 同源 GET，`request.destination ∈ {document, script, style, font, image}`，且不落在黑名单前缀内

其它：

- 导航请求失败时回退到预缓存的 `index.html`（`setCatchHandler` + `matchPrecache`），再找不到才返回 `Response.error()`。刻意不使用 `NavigationRoute`，否则会抢占 `/api/` 的 `NetworkOnly` 匹配。
- 更新策略为 `registerType: 'prompt'`：新版本就绪后提示用户，由用户点击"立即刷新"再发 `SKIP_WAITING`。只有 `self.skipWaiting()`，没有自动接管页面的 `clientsClaim()`。

## 目录

```
src/
  api/        HTTP 客户端、契约类型、错误文案
  components/ OfflineBanner、InstallPrompt
  pwa/        cache-policy、sw.ts（injectManifest 入口）、register-sw
  router/     路由与全局守卫
  stores/     session、space（均为内存态）
  styles/     main.css（暖白色板）
  views/      Login / CreateSpace / JoinSpace / Invite / Home / NotFound
tests/unit/   api-client、session-store、sw-cache-policy
public/       favicon.svg、apple-touch-icon.png、icons/*（由负责人提供）
```

## 阶段 0 未实现的功能

首页只是占位面板（当前用户、空间时区、成员槽位、可用邀请、注销）。
**任务、奖励、留言、日历、纪念日等阶段 1+ 功能均未实现**，需要先有各自评审过的接口契约再开工。

## 本文件未做的事

本目录的代码由实现方编写，**没有执行过** `pnpm install` / `typecheck` / `test` / `build`；这些验证由负责人在生成 lockfile 后统一执行。
