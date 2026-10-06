# 阶段 0 端到端手动走查脚本（两用户流程）

> **这是一份人工浏览器走查脚本，不是自动化运行器。**
> 阶段 0 **不添加任何浏览器自动化依赖**（不引入 Playwright / Cypress / Puppeteer / WebdriverIO，
> 也不添加对应的 npm 脚本）。下面每一步都由项目负责人用真实浏览器手动执行并观察。
>
> **本文件中的全部步骤尚未执行。** 所有"预期"都来自对 `contracts/openapi.yaml`、
> `database/migrations/*.sql`、`apps/web/src/**`、`apps/admin/src/**` 的**静态阅读**，
> 不是实测结果。

---

## 0. 走查目标

这一条流程覆盖了阶段 0 唯一一条真正跨两个用户、跨两个设备的主线，
并且刻意把三个最容易"看起来能跑但实际上是错的"的地方放在路径上：

1. **两用户看到同一个空间**——不是各建了一个空间。
2. **第三人被拒**——容量由 `couple_members` 的 slot 值域决定，不是靠应用层数人数。
3. **注销后按浏览器后退键看不到业务数据**——证明业务数据只存在内存、不进 Cache Storage，
   Service Worker 也没有缓存 `/api/**`。

## 1. 前置条件（全部由负责人手动准备）

- 后端已构建并启动，`GET https://localhost:8443/api/v1/health` 返回 `200`（对应
  `tests/contract/phase0-http-checks.md` 用例 1）。
- Caddy 同源 HTTPS 入口已就绪，且**本地 CA 已被浏览器信任**（否则 Service Worker 无法注册、
  PWA 无法安装；见 `docs/local-development.md` 第 2 节）。
  注意：`deploy/` 已存在（`deploy/Caddyfile` 只定义 `https://localhost:8443` 一个站点，
   管理界面在 `/admin/`），但**从未启动验证过**；本地 CA 的导出与导入步骤见
   `deploy/LOCAL-HTTPS.md`（第 3 节导出到 `deploy\caddy\certs\root.crt`，第 4 节手动导入信任库）。
   另外 `.env.example` 的 `ADMIN_ORIGIN=https://localhost:8444` 与这套单站点拓扑不一致
   （见 `docs/architecture.md` 第 1 节），本走查不涉及管理端，但遇到 origin 报错时先想到它。
- `.env` 已由 `.env.example` 生成：`REGISTRATION_MODE=WHITELIST`，
  `ALLOWED_EMAILS` 至少包含两个测试邮箱；`MAIL_MODE=log` 时验证码只写进 API 进程日志
  （`.env.example` 里 `MAIL_MODE=log` 是默认值），**负责人需要从日志里读取验证码**。
- 三个可用的测试邮箱：A、B 在 `ALLOWED_EMAILS` 内，C 用于第 6 步（也应在白名单内，
  否则拒绝原因会是 `REGISTRATION_NOT_ALLOWED` 而不是我们要验证的 `SPACE_FULL`）。
- 准备两个互相隔离的浏览器环境（**不要用同一个普通窗口的两个标签页**——
  普通标签页共享 Cookie 与 Service Worker）：
  - **设备 A**：主浏览器（或它的一个独立用户配置）。
  - **设备 B**：另一个浏览器，或同一浏览器的一个**新的用户配置 / 隐身窗口**。
    若用隐身窗口，注意隐身会话关闭即清空，适合走查但**不支持安装 PWA**；
    若要验证设备 B 的安装行为，请使用第二个持久化用户配置。

> 走查过程中会留下真实数据（用户、空间、邀请）。这是本地库，不是生产库；
> 但请在开始前确认自己连的是**本地** PostgreSQL（`.env` 的 `POSTGRES_HOST=127.0.0.1`）。

---

## 2. 主流程

### 第 1 步 —— 设备 A：打开入口并安装 PWA

1. 在设备 A 打开 `https://localhost:8443`。
2. 打开 DevTools → **Application** 标签：
   - **Service Workers**：预期能看到一个已激活（activated）的 Service Worker。
     若为 "waiting"，刷新一次页面。
   - **Manifest**：预期能看到应用名与图标，浏览器地址栏出现"安装"入口。
3. 触发安装（地址栏安装图标，或浏览器菜单 → 安装应用）。

**预期观察**

- 安装成功后以独立窗口启动，地址栏形态与普通标签页不同。
- **Application → Cache Storage** 中存在 **静态外壳** 的缓存
  （具体缓存名与 URL 列表见 `docs/pwa-cache-policy.md` 的规则表），
  但**不应**存在任何 `/api/...` 条目。
- **Application → IndexedDB / Local Storage / Session Storage**：
  预期看不到存储情侣业务数据的库或键（见 `docs/pwa-cache-policy.md`）。

### 第 2 步 —— 设备 A：用邮箱验证码登录

1. 在应用内输入邮箱 A，点击发送验证码。
2. **从 API 进程日志里读取验证码**（`MAIL_MODE=log`）。
3. 输入验证码并登录。

**预期观察**

- 登录成功后进入应用主界面（此时还没有空间）。
- DevTools → **Application → Cookies**：存在 `__Host-usapp-session`，
  且属性为 `Secure`、`HttpOnly`、`SameSite=Lax`、`Path=/`、**无 `Domain`**
  （`HttpOnly` 在 DevTools 里会显示为不可读的值，这是正常的）。
  预认证阶段的 Cookie 值应与登录后的**不同**（会话轮换）。
- **CSRF 令牌不应出现在 Cookies / Local Storage / Session Storage 里**——
  它按设计只存在于内存（见 `docs/architecture.md` 第 1 节）。

### 第 3 步 —— 设备 A：创建空间

1. 在界面上创建一个空间，填一个名字。

**预期观察**

- 创建成功，界面显示该空间与 1 名成员。
- 空间时区显示为 `Asia/Shanghai`（契约里 `Space.timezone` 是 `const Asia/Shanghai`）。
- 再次点"创建空间"应被拒绝（契约 `409 SPACE_ALREADY_EXISTS`）；
  这是 `apps/admin` 之外的业务规则，界面上预期表现为一条错误提示而不是崩溃。

### 第 4 步 —— 设备 A：生成邀请链接

1. 在空间内创建一条邀请。界面应给出**一次性的明文邀请码 / 邀请链接**。
2. 记下该链接（形态为 `{PUBLIC_ORIGIN}/invite/{code}`，见契约 `CreatedInvite.invite_url`）。

**预期观察**

- 刷新或重新打开邀请列表后，**明文邀请码不再出现**
  （契约中 `Invite` 没有 `code` 字段，明文只在创建的 `201` 响应里出现一次；
  库内只存 `code_hash`，见 `database/migrations/0003_couple_binding.sql` 的 `space_invites`）。

### 第 5 步 —— 设备 B：打开邀请链接、登录、接受

1. 在设备 B 打开第 4 步的邀请链接（此时设备 B 尚未登录）。
2. 用邮箱 B 走与第 2 步相同的登录流程（发码 → 从日志读码 → 登录）。
3. 接受邀请。

**预期观察**

- 接受成功后，设备 B 界面显示的空间 `id` 与设备 A 的**同一个空间**，
  且成员数为 **2**，两个成员的 `slot` 分别是 **1 和 2**
  （契约 `SpaceMember.slot` 的 `enum: [1, 2]`）。
- **两个设备刷新后看到的仍然同一个空间**——这是本步的真正断言：
  若设备 B 其实是新建了一个空间，两边会各显示 1 名成员。
- 设备 A 刷新后应看到成员数变为 2。

### 第 6 步 —— 第三个账号接受邀请：必须被拒

前置细节很重要：**邀请要在空间未满时创建**。空间满 2 人后再创建邀请本身就会返回
`409 SPACE_FULL`（契约 `POST /api/v1/spaces/current/invites`），
那样就测不到"第三人接受被拒"这条路径。因此：

1. 回到设备 A（`slot` 为 1 的那个账号），**再创建一条邀请**，记下链接。
2. 用第三个浏览器环境（或设备 B 的另一个用户配置）打开该链接，用邮箱 C 登录，尝试接受。

**预期观察**

- 接受失败，错误码为 **`SPACE_FULL`**（HTTP `409`，契约 `acceptInvite` 的 `409`）。
- C **没有**加入任何空间：C 自己的"我的空间"仍为空。
- 该邀请**没有被消费**（邀请列表里 `used_count` 不增加）——
  契约要求校验、消费、加入在同一事务内完成，任一步失败整体回滚。
- C 尝试直接访问空间详情 URL（`/api/v1/spaces/<A 的空间 id>`）时，应得到
  **`404 SPACE_NOT_FOUND`，而不是 `403`**（契约明确"非成员一律 404，故意不区分"）。

### 第 7 步 —— 设备 A：注销，然后按浏览器后退键

1. 在设备 A 注销（或切换账号）。
2. 注销完成后，**反复按浏览器的后退键**，并刷新页面。

**预期观察（本步是整条流程里最关键的负向断言）**

- 后退**不应**显示注销前的空间内容：既不应显示缓存的页面数据，
  也不应因为 Service Worker 的缓存而把 `/api/**` 的旧响应拿出来。
  预期结果是要么回到未登录界面，要么显示离线/未认证提示。
- DevTools → **Application → Cache Storage**：仍然只有静态外壳的缓存，
  **没有** `/api/**` 条目，也没有任何业务 JSON。
- 若后退键**确实**显示了业务数据，这是一个**必须上报的缺陷**：
  它意味着 `/api/**` 或私有资源被缓存了，或业务数据被写进了持久化存储。

---

## 3. 附带的边界观察（建议顺带做，不作为通过标准）

- **离线**：DevTools → Network 切到 Offline，在应用内尝试提交一个业务操作。
  预期：界面提示离线并**拒绝**提交，而不是排队后补发（见 `docs/pwa-cache-policy.md`）。
- **多标签页**：在同一浏览器里开第二个标签页访问入口。预期：不产生第二份业务数据副本，
  两个标签页的会话是同一份。
  （若观察到不一致，属于**尚未验证的设计空白**，见第 5 节。）

---

## 4. 已实现 / 仅骨架 / 尚未实现

### 已实现

- 本走查脚本本身（步骤、预期观察、负向断言）。

### 仅骨架

- 前端界面：`apps/web` 的 store 与视图是骨架实现，
  具体按钮文案与页面路径以实际代码为准；本脚本描述的是**业务行为**而不是像素。
- 管理端（`apps/admin`）不参与这条流程，本脚本不覆盖它。

### 尚未实现

- **任何自动化端到端测试**：没有 Playwright / Cypress / Puppeteer，也没有 npm 脚本，
  阶段 0 明确不添加浏览器自动化依赖。
- **推送通知**：`PUSH_WORKER_ENABLED=false`（`.env.example` 默认值），
  本流程不涉及推送，`services/push-worker` 也不参与。
- **媒体上传 / 原图**：阶段 0 没有任何媒体端点，因此本流程不包含上传与备份恢复演练。
- **任务、奖励、小红花**：阶段 0 没有任务实体，因此本流程不包含任务周期与结算。

## 5. 尚未执行的检查

1. 本文件全部步骤**一步都没有执行**：没有安装过 PWA，没有登录过，没有创建过空间或邀请。
2. 未验证 `apps/web` 是否真的注册了 Service Worker（`vite.config.ts` 未开启 `devOptions`，
   dev server 下 `/sw.js` 是否可访问本次未确认）。
3. 未验证任何缓存名、缓存条目或 Cache Storage 的实际内容。
4. 未验证 `MAIL_MODE=log` 时验证码写入的日志格式与位置。
5. 未验证邀请链接在设备 B 上被打开时，前端是否会把未登录用户引导到登录流程后再回到邀请页
   （**该行为是设计意图，但本次读到的前端骨架中未经确认**）。
6. 未验证"同一浏览器两个标签页"的会话一致性。
7. 未验证离线状态下的提交拒绝提示的确切文案与形态。
8. 未验证注销后浏览器后退键的具体浏览器行为（不同浏览器对 history 与 BFCache 的处理不同，
   本脚本只给出"不得显示业务数据"这一条断言，不给具体表现）。
