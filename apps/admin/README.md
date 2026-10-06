# @usapp/admin —— 管理界面骨架（阶段 0）

USapp 的**管理界面**，与情侣端 `apps/web` 完全独立的一个前端应用。

当前是**阶段 0 的只读运维骨架**：能看，不能改。这不是"还没写完"的托词，
而是刻意的范围约束 —— 阶段 0 的冻结契约里就没有任何管理写入端点。

---

## 1. 关键事实（先读这一段）

| 事实 | 值 |
| --- | --- |
| 访问路径 | `/admin/`（**不是**根路径，与 `apps/web` 不抢路由空间） |
| 开发服务器 | `vite`，绑定 `127.0.0.1:5174`，`strictPort: true` |
| 路由模式 | hash（`/admin/index.html#/users`），**不需要**服务端重写规则 |
| 构建产物 | `apps/admin/dist/` |
| 会话 Cookie | `__Host-usapp-admin`（HttpOnly，前端**不读取**） |
| CSRF | `X-CSRF-Token`，来自 `GET /api/admin/v1/session` |
| 请求凭据 | `credentials: 'same-origin'`（绝不用 `include`） |
| 持久化 | **无**。刷新页面 = 重新引导会话 |
| PWA / Service Worker | **不启用**。管理界面禁止被缓存 |

### 管理入口只绑定回环地址

契约（`contracts/openapi.yaml`）对管理端点的要求是"仅回环 / 受信代理可达"。
前端假定：**能打开这个页面，说明网络位置已经被服务端校验过了**。
这只是前提说明，不是授权判断。

### 独立的管理员认证

管理账号与情侣账号体系**没有关系**。管理员不是"某个用户"：

- 情侣端用 `__Host-usapp-session`；管理端用 `__Host-usapp-admin`。
- 两张会话表、两套认证主体、两条生命周期。
- 不要为了"少写一个登录页"把它们合并 —— 合并意味着任何一个普通用户
  的会话都可能被当成管理会话的前置条件。

### 前端不做任何权限判断

本目录里**没有** `isAdmin`、`hasPermission`、`canEdit` 之类的逻辑。
按钮 disabled 只是 UI 取舍，不是安全边界。

> 服务端才是唯一权威：`/api/admin/v1/**` 的每一个请求都由服务端独立校验
> 来源地址、`__Host-usapp-admin` 会话与 `X-CSRF-Token`。
> "前端判断过了所以可以放心"是错的。

---

## 2. 目录结构

```
apps/admin/
├── index.html                  lang=zh-CN，declares noindex
├── package.json                @usapp/admin，engines.node >= 24
├── tsconfig.json               应用与测试（strict）
├── tsconfig.node.json          vite.config.ts（strict）
├── vite.config.ts              base /admin/、127.0.0.1:5174、strictPort、alias @、vitest 配置
├── README.md
├── src/
│   ├── main.ts                 createApp → pinia → router（顺序有意义）
│   ├── App.vue                 顶栏导航 + RouterView + 页脚
│   ├── env.d.ts                ImportMetaEnv（仅非机密构建期变量）
│   ├── api/
│   │   ├── client.ts           AdminHttpClient：CSRF、错误信封、same-origin
│   │   └── admin.ts            按契约逐条封装管理端点 + 单例 adminApi
│   ├── components/
│   │   ├── CursorPager.vue     游标分页条（只表达契约给得出的东西）
│   │   └── StageFiveAction.vue disabled 的阶段占位按钮 + title 说明
│   ├── composables/
│   │   └── useCursorList.ts    只读游标分页（游标栈实现 prev）
│   ├── router/index.ts         6 条路由 + 会话引导守卫
│   ├── stores/
│   │   └── admin-session.ts    pinia，纯内存，无持久化
│   ├── styles/main.css         暖色调变量（与 apps/web 同一套语义，更"运维工具"）
│   ├── utils/format.ts         时间 / id / 计数 / actor_type 展示格式
│   └── views/
│       ├── LoginView.vue       用户名 + 密码，提示"仅本地可用"
│       ├── OverviewView.vue    健康、版本、数据库/迁移占位
│       ├── UsersView.vue       只读表格 + 关键字筛选
│       ├── CouplesView.vue     只读表格（契约无筛选参数，故无搜索框）
│       ├── AuditLogsView.vue   只读表格
│       └── NotFoundView.vue    404
└── tests/unit/
    ├── admin-client.spec.ts            CSRF 头 / 缺令牌 / 错误信封
    └── admin-session-store.spec.ts     注销清空内存态 / 无持久化
```

---

## 3. 路由与会话引导

| 路径 | 名称 | 是否需要认证 |
| --- | --- | --- |
| `#/login` | `login` | 否（已登录会被送回总览） |
| `#/` | `overview` | 是 |
| `#/users` | `users` | 是 |
| `#/couples` | `couples` | 是 |
| `#/audit` | `audit` | 是 |
| `#/*` | `not-found` | 是 |

守卫（`src/router/index.ts`）在**首次导航**时（含 `#/login`）等待
`session.bootstrap()`，也就是 `GET /api/admin/v1/session`：

1. 取回并保存 CSRF 令牌 —— 没有它，登录 POST 会被客户端在本地直接拒绝；
2. 读取 `authenticated` 决定跳转目标。

引导失败（服务端不可达）不阻塞渲染：停在登录页并显示错误，而不是白屏。
失败也会置 `bootstrapped = true`，避免每次导航都重复重试。

**为什么用 hash 路由**：用 history 模式就必须在 Caddy 上补一条
`/admin/*` 回退到 `/admin/index.html` 的重写规则；漏配的表现是刷新
`/admin/users` 得到 404。管理界面是回环运维工具，少一个部署前提比 URL 好看更重要。

---

## 4. API 层

`src/api/admin.ts` 只封装契约中真实存在的管理端点：

| 方法 | 端点 | 说明 |
| --- | --- | --- |
| `health()` | `GET /api/admin/v1/health` | 存活（`security: []`） |
| `bootstrapSession()` | `GET /api/admin/v1/session` | 引导 + 取 CSRF 令牌 |
| `login(u, p)` | `POST /api/admin/v1/sessions` | 管理员登录，**成功后轮换令牌** |
| `logout()` | `DELETE /api/admin/v1/sessions` | 注销，成功后本地令牌作废 |
| `listUsers({limit,cursor,query})` | `GET /api/admin/v1/users` | 只读分页 |
| `listCouples({limit,cursor})` | `GET /api/admin/v1/couples` | 只读分页 |
| `listAuditLogs({limit,cursor})` | `GET /api/admin/v1/audit-logs` | 只读分页 |

行为约定（`src/api/client.ts`）：

- 写方法（POST/PUT/PATCH/DELETE）**必须**带 `X-CSRF-Token`；
  令牌缺失时抛 `AdminApiError(status=0, code='CSRF_TOKEN_MISSING')`，
  **请求不会发出**（`status=0` 用来与真实 HTTP 失败区分）。
- 失败响应解析 `{error:{code,message,details}}` 到 `AdminApiError`；
  响应体不是契约信封（网关 HTML、空体）时退回 `INTERNAL_ERROR` 并保留 HTTP 状态。
- `204` 返回 `undefined`，不尝试解析 JSON。
- 基址默认 `/api/admin/v1`，相对路径 —— 不接受绝对地址，避免把管理会话送去第三方域名。

### 分页为什么只有"上一页/下一页"

契约的列表端点只给 `next_cursor`：没有总数、没有反向游标。
`prev` 由 `useCursorList.ts` 记住走过的游标实现（回到上一页 = 用那一页曾用过的游标重新请求）。
前端不假装有 offset 分页，也不显示编造的总条数。

---

## 5. 阶段 0 的占位与未实现项

以下内容**明确未实现**，界面上以 disabled 按钮（`StageFiveAction.vue`）或
"契约未定义"标签呈现。它们的存在是为了让"本该有什么"可见，不构成任何安全边界。

| 未实现项 | 位置 | 原因 |
| --- | --- | --- |
| 数据库连通性 / 迁移版本 / 连接池 | 总览 | 契约中**没有**对应端点，不编造数字 |
| 触发数据库备份 | 总览 | 阶段 5；备份编排与保留策略未设计 |
| 修正用户资产 | 总览、用户 | 阶段 5；资产体系尚未进入契约 |
| 维护阳光商店 | 总览、情侣空间 | 阶段 5 业务域 |
| 禁用账号 / 强制注销会话 | 用户 | 阶段 5；状态机与审计要求未落地 |
| 解散空间 | 情侣空间 | 阶段 5；解散语义未定义 |
| 导出审计日志 | 审计日志 | 阶段 5；导出格式与脱敏规则未定义 |

阶段 0 的管理界面**没有任何**修改用户 / 资产 / 空间的请求代码。

---

## 6. 命令

```bash
# 在 apps/admin 下执行；依赖安装由项目负责人统一手动完成
pnpm dev          # vite dev server → http://127.0.0.1:5174/admin/
pnpm build        # vue-tsc 类型检查 + vite build → dist/
pnpm preview      # 预览构建产物（同样绑定 127.0.0.1:5174）
pnpm typecheck    # vue-tsc 检查应用与 vite.config.ts
pnpm test:unit    # vitest run（jsdom）
```

> 本目录**不包含** lock 文件：`pnpm-lock.yaml` 由项目负责人在仓库根统一生成。

管理界面的 API 调用依赖同源部署：开发时需要通过 Caddy（或等价反向代理）
把 `/api/admin/v1/**` 代理到后端，并保证浏览器地址是 `https://localhost:8443`
之类的同源入口。直接用 `127.0.0.1:5174` 打开时不会有 `__Host-usapp-admin` Cookie
——`__Host-` 前缀要求 `Secure`，而 `http://127.0.0.1` 不满足（即便浏览器把
localhost 当作 secure context）。

---

## 7. 已有测试（本阶段仅编写，未执行）

| 文件 | 覆盖 |
| --- | --- |
| `tests/unit/admin-client.spec.ts` | 写请求带 `X-CSRF-Token`；缺令牌抛错且不发请求；错误信封解析；非契约响应兜底；令牌轮换；查询参数名与契约一致 |
| `tests/unit/admin-session-store.spec.ts` | 注销清空内存态（含服务端调用失败的情况）；引导失败按未认证处理；**不写入任何 Web Storage** |

> 说明：这些测试是**写好的**，不是**跑过的**。阶段 0 的实现者被明确禁止执行
> 任何构建、测试或安装命令（依赖尚未安装，无法执行）。类型检查与单测通过与否
> 由项目负责人在安装依赖后确认。
