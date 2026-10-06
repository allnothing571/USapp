# USapp

一个供情侣双方共同使用的私人 PWA（任务激励 / 小红花现实奖励 / 阳光值轻游戏化 / 回忆与共同计划）。

当前仓库处于 **阶段 0：工程基础**，只包含骨架、登录与情侣绑定、会话与 CSRF 防护、安装与缓存、
调度基础、管理与备份基础、契约与测试文件。

> 阶段 0 **不实现**阶段 1～5 的任何业务功能（任务、奖励、资产结算、留言、照片、角色、成就等）。

## 目录职责

| 路径 | 职责 |
| --- | --- |
| `apps/web` | 手机客户端（Vue 3 + TS + Vite + vite-plugin-pwa） |
| `apps/admin` | 管理界面骨架（仅本机回环访问，只读运维视图） |
| `services/api` | C++17 / Drogon 模块化单体后端：认证、空间、调度基础、管理接口 |
| `services/push-worker` | Node.js 24 / TS 推送辅助进程（阶段 0 仅骨架，默认关闭） |
| `contracts` | OpenAPI 契约（阶段 0 已冻结） |
| `database/migrations` | PostgreSQL 17 迁移 |
| `deploy` | Docker Compose、Caddy 同源反向代理、本地 HTTPS 证书配置 |
| `tests` | 跨层验收测试（契约、安全矩阵、集成与端到端；只编写不执行） |
| `docs` | 手动操作说明与阶段交付文档 |

## 架构要点

- **同源**：Caddy 同时提供网页与 API，前端不跨域。所有接口只接受 `PUBLIC_ORIGIN`。
- **认证**：邮箱一次性验证码，无密码体系。登录前使用短期预认证会话，发送验证码与登录同样受 CSRF + Origin 保护。
- **Cookie**：`__Host-usapp-session`，`Secure; HttpOnly; SameSite=Lax; Path=/`，不设置 `Domain`。
- **情侣模型**：`users` / `couples` / `couple_members`，`slot ∈ {1,2}`，不写死两名用户、不写死性别或权限。
- **调度**：C++ 主进程内调度，`pg_try_advisory_xact_lock` 保证跨实例唯一；无 Redis、无额外调度服务。
- **管理**：独立管理员认证，入口绑定回环地址，与情侣角色完全隔离。
- **媒体**：阶段 0 不接收原图；后续私有 OSS 直传，应用服务器不中转原图。

## 快速开始

阶段 0 的所有命令都必须由项目负责人手动执行。完整按序清单见
[docs/phase0-handoff.md](docs/phase0-handoff.md)。

- 架构说明：[docs/architecture.md](docs/architecture.md)
- 本地开发：[docs/local-development.md](docs/local-development.md)
- 数据库备份与空环境恢复：[docs/backup-restore.md](docs/backup-restore.md)
- 缓存与离线边界：[docs/pwa-cache-policy.md](docs/pwa-cache-policy.md)
- 调度基础：[docs/scheduler.md](docs/scheduler.md)
- 阶段 0 交付与验收：[docs/phase0-delivery.md](docs/phase0-delivery.md)

## 安全底线（阶段 0 已落地，后续不得放宽）

1. 不用关闭安全属性（`Secure` / `HttpOnly` / `SameSite`）来适配本地开发；本地走同源 HTTPS。
2. 状态修改请求必须同时通过 CSRF 令牌与会话绑定校验、以及精确匹配的 Origin 校验。
3. 登录成功后必须轮换会话标识与 CSRF 令牌。
4. 所有 `/api/**` 响应 `Cache-Control: no-store`，Service Worker 永不缓存 `/api` 与私有媒体。
5. 真实密钥只存在于部署环境的 `.env` / 密钥文件，仓库内只保留 `.env.example`。
