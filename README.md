# USapp

一个供情侣双方共同使用的私人 PWA（任务激励 / 小红花现实奖励 / 阳光值轻游戏化 / 回忆与共同计划）。

当前仓库在已验收的 **阶段 0：工程基础** 上推进阶段 1，已包含登录与情侣绑定、会话与 CSRF 防护、
安装与缓存、调度与管理基础，以及修改昵称、钱包流水基础和自己的资产余额只读展示。

> 阶段 0 **不实现**阶段 1～5 的任何业务功能（任务、奖励、资产结算、留言、照片、角色、成就等）。

> **当前状态**：阶段 0 已于 2026-10-06 由项目负责人验收通过，验收定位为**本地开发基线**，
> 范围与局限以 [docs/phase0-acceptance.md](docs/phase0-acceptance.md) 和
> [docs/claude-code-handoff.md](docs/claude-code-handoff.md) 为准。这不是生产就绪认可；
> 已执行的检查是有针对性的，不代表已完成穷尽的安全验证，也不代表所有契约检查与端到端检查都已执行。
> 阶段 1 按里程碑推进（[docs/phase1-plan.md](docs/phase1-plan.md)）：M1“修改昵称”已实现，
> 并于 2026-10-06 通过项目负责人执行的验收检查；M2-A“钱包流水基础”和 M2-B“余额读取、启动权限检查与首页展示”
> 已实现，并通过负责人执行的针对性验证（结果与局限见计划第 9.19 节，更新于 2026-10-09）。
> 主库已应用迁移 0006，目前余额为零；尚无资产入账或结算路径。M2 的单独正式验收决定尚未记录，
> M3～M10 尚未获批，任务、奖励兑换等功能尚未实现。以上结果不代表穷尽的安全验证。

## 目录职责

| 路径 | 职责 |
| --- | --- |
| `apps/web` | 手机客户端（Vue 3 + TS + Vite + vite-plugin-pwa） |
| `apps/admin` | 管理界面骨架（仅本机回环访问，只读运维视图） |
| `services/api` | C++17 / Drogon 模块化单体后端：认证、空间、调度基础、管理接口 |
| `services/push-worker` | Node.js 24 / TS 推送辅助进程（阶段 0 仅骨架，默认关闭） |
| `contracts` | OpenAPI 契约（阶段 0 操作保留，阶段 1 增量版本为 0.3.0） |
| `database/migrations` | PostgreSQL 17 迁移 |
| `deploy` | Docker Compose、Caddy 同源反向代理、本地 HTTPS 证书配置 |
| `tests` | 跨层验收清单与测试文档（契约、安全矩阵、集成与端到端）；由项目负责人按需选择性执行，并非全部检查都已执行，执行情况与结果以 [docs/claude-code-handoff.md](docs/claude-code-handoff.md) 为准 |
| `docs` | 手动操作说明与阶段交付文档 |

## 架构要点

- **同源**：Caddy 在用户入口与管理入口分别同源提供网页与 API，前端不跨域。Origin 精确校验只针对状态修改请求：用户端状态修改请求按 `PUBLIC_ORIGIN` 校验；管理端状态修改请求按 `ADMIN_ORIGIN` 校验（管理入口仅本机回环访问）。
- **认证**：邮箱一次性验证码，无密码体系。登录前使用短期预认证会话，发送验证码与登录同样受 CSRF + Origin 保护。
- **Cookie**：`__Host-usapp-session`，`Secure; HttpOnly; SameSite=Lax; Path=/`，不设置 `Domain`。
- **情侣模型**：`users` / `couples` / `couple_members`，`slot ∈ {1,2}`，不写死两名用户、不写死性别或权限。
- **调度**：C++ 主进程内调度，`pg_try_advisory_xact_lock` 保证跨实例唯一；无 Redis、无额外调度服务。
- **管理**：独立管理员认证，入口绑定回环地址，与情侣角色完全隔离。
- **媒体**：阶段 0 不接收原图；后续私有 OSS 直传，应用服务器不中转原图。

## 快速开始

阶段 0 的所有命令都必须由项目负责人手动执行。当前交接与验收状态记录见
[docs/claude-code-handoff.md](docs/claude-code-handoff.md)；较早的按序执行清单见
[docs/phase0-handoff.md](docs/phase0-handoff.md)（其中部分“未执行”说明与测试数量已过时）。

- 架构说明（历史快照）：[docs/architecture.md](docs/architecture.md)——其中的部署描述已过时
  （例如“控制器尚不存在”“管理入口只用 8443”），以当前代码、配置与交接文档为准
- 本地开发：[docs/local-development.md](docs/local-development.md)
- 数据库备份与空环境恢复：[docs/backup-restore.md](docs/backup-restore.md)
- 缓存与离线边界：[docs/pwa-cache-policy.md](docs/pwa-cache-policy.md)
- 调度基础：[docs/scheduler.md](docs/scheduler.md)
- 当前交接与验收状态记录：[docs/claude-code-handoff.md](docs/claude-code-handoff.md)

## 安全底线（阶段 0 已落地，后续不得放宽）

1. 不用关闭安全属性（`Secure` / `HttpOnly` / `SameSite`）来适配本地开发；本地走同源 HTTPS。
2. 状态修改请求必须同时通过精确匹配的 Origin 校验与会话绑定的 CSRF 令牌校验。普通用户登录前使用短期预认证会话，发送验证码与登录同样受 CSRF + Origin 保护。
   唯一例外是管理员登录（精确匹配 `POST /api/admin/v1/sessions`）：登录前管理面没有会话，因此不校验会话绑定的 CSRF 令牌，
   但必须通过精确的 `ADMIN_ORIGIN` 与 `X-Requested-By` 请求头校验；包括注销在内的其他受保护管理写请求仍必须携带会话绑定的 CSRF 令牌。
3. 登录成功后必须轮换会话标识与 CSRF 令牌。
4. 所有 `/api/**` 响应 `Cache-Control: no-store`，Service Worker 永不缓存 `/api` 与私有媒体。
5. 真实密钥只存在于部署环境的 `.env` / 密钥文件，仓库内只保留 `.env.example`。
