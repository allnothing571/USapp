# USapp

> Startup safety (2026-11-15): a closed/unconfirmed deletion-recovery state now refuses the API before listeners/jobs. Real media or memory purge also refuses while the production journal provider is missing. This is a refusal layer, not restored-data release permission; see [scope and limits](docs/deletion-startup-refusal.md).

> P2-6 R1 提醒规则的存储与接口已实现，仅在一次性隔离环境中验证；0025 及其修复 0026 已冻结，尚未应用到共享测试库或主库；R2 应用内提醒生成已实现但默认关闭（`REMINDERS_ENABLED=false`），同样仅在隔离环境中验证，未部署，见[第 18 节](docs/phase2-reminders.md#18-r2-implementation-and-isolated-verification-claude-code-2026-11-02)。R3 网页端提醒设置与通知兼容已实现，仅通过自动化测试，尚未做浏览器或真机验收，见[第 19 节](docs/phase2-reminders.md#19-r3-implementation-and-automated-verification-claude-code-2026-11-02)。R4 任务修改获批时继承接收者的提醒规则已实现，0027 已冻结，仅在一次性隔离环境中验证，未部署，见[第 20 节](docs/phase2-reminders.md#20-r4-implementation-and-isolated-verification-claude-code-2026-11-03)。R5 过期提醒的推送抑制与有效期上限已实现，0028 已冻结，仅用假传输在一次性隔离环境中验证，未部署，见[第 21 节](docs/phase2-reminders.md#21-r5-implementation-and-isolated-verification-claude-code-2026-11-03)。提醒列表现在返回 `generation_enabled`（仅为配置状态），网页端据此显示提醒生成是否开启，未部署，见[第 22 节](docs/phase2-reminders.md#22-reminder-generation-status-claude-code-2026-11-03)。见 [实现与验证记录](docs/phase2-reminders.md#17-r1-implementation-and-isolated-verification-claude-code-2026-11-02)。

> P2-5c 推送传输、Service Worker 与订阅流程已实现，仅在一次性隔离环境中用桩验证；真实推送默认全部关闭，未连接真实推送服务商，未做设备验收，未部署。见 [实现与验证记录](docs/phase2-web-push.md#16-p2-5c-implementation-and-isolated-verification-claude-code-2026-11-02)。

> P2-5b 推送持久派发已实现，并仅用假传输在测试库完成自动化验证；0023 仅在 usapp_test 应用并冻结，未部署，没有发送任何真实推送。见 [实现与验证记录](docs/phase2-web-push.md#13-p2-5b-implementation-and-verification-claude-code-2026-11-02)。

> P2-5a 设备提醒订阅生命周期已实现并通过测试库自动化验证；0022 仅在 usapp_test 应用并冻结，未部署；订阅入口默认关闭（尚无推送传输）。见 [实现与验证记录](docs/phase2-web-push.md#12-p2-5a-implementation-and-verification-claude-code-2026-10-30)。

> P2-4 轻量回应已实现并通过测试库自动化验证；0021 仅在 usapp_test 应用并冻结，未部署。任务完成后发布者可回应一次；留言快捷表情复用单次回复。见 [实现与验证记录](docs/phase2-reactions.md#8-implementation-and-verification-codex-2026-10-30)。

> P2-3a 站内强制通知已实现并通过测试库验证；0019 仅应用测试库并冻结，未部署。P2-3b 可选通知与记忆选择已实现并通过测试库全部自动化验证；迁移 0020 仅应用测试库并冻结，主库未应用，未部署（见通知文档第 13 节）。详见 [实现与验证记录](docs/phase2-notifications.md#11-p2-3a-implementation-and-verification-codex-2026-10-29)。

一个供情侣双方共同使用的私人 PWA（任务激励 / 小红花现实奖励 / 阳光值轻游戏化 / 回忆与共同计划）。

当前仓库在已验收的 **阶段 0：工程基础** 上推进阶段 1，已包含登录与情侣绑定、会话与 CSRF 防护、
安装与缓存、调度与管理基础，以及修改昵称、钱包流水基础、自己的资产余额只读展示，
和一次性任务的发布、查看、完成与奖励结算（M3）。计数任务（M4）已部署并完成代表性现场检查；每天重复计数（M5-A）已通过负责人执行的后端／前端自动化检查，尚未部署。

> 阶段 0 **不实现**阶段 1～5 的任何业务功能（任务、奖励、资产结算、留言、照片、角色、成就等）。

> **当前状态**：阶段 0 已于 2026-10-06 由项目负责人验收通过，验收定位为**本地开发基线**，
> 范围与局限以 [docs/phase0-acceptance.md](docs/phase0-acceptance.md) 和
> [docs/claude-code-handoff.md](docs/claude-code-handoff.md) 为准。这不是生产就绪认可；
> 已执行的检查是有针对性的，不代表已完成穷尽的安全验证，也不代表所有契约检查与端到端检查都已执行。
> 阶段 1 按里程碑推进（[docs/phase1-plan.md](docs/phase1-plan.md)）：M1“修改昵称”已实现，
> 并于 2026-10-06 通过项目负责人执行的验收检查；M2-A“钱包流水基础”和 M2-B“余额读取、启动权限检查与首页展示”
> 已实现，并通过负责人执行的针对性验证（结果与局限见计划第 9.19 节，更新于 2026-10-09）。
> 主库已应用迁移 0006；当时余额为零的记录见计划第 9.19 节。M2 的单独正式验收决定尚未记录。
> M3-A“任务数据库基础”（迁移 0007 等）已获批、已实现，并于 2026-10-09 在测试库 `usapp_test`
> 上通过负责人执行的验证（结果与局限见 [docs/phase1-m3-design.md](docs/phase1-m3-design.md) 第 11.3 节）；
> 单独的正式验收决定尚未记录；负责人已于 2026-10-09 提交并推送 M3-A（`6a9da4b`）。
> M3-B“任务发布与读取接口、任务页面、启动时任务表权限检查”已于 2026-10-09 获批并实现，
> 并于 2026-10-10 通过负责人执行的针对性检查；负责人已在主库应用 0007 并部署 M3-B
> （结果与局限见 [docs/phase1-m3-design.md](docs/phase1-m3-design.md) 第 12.9 节）。负责人于 2026-10-10 在上述检查的范围与局限内
> 验收 M3-B（本地开发里程碑，不是生产就绪认可），并已提交推送（`380696b`，第 12.10 节）。
> M3-C（完成任务与奖励结算）已实现，通过已记录的负责人自动化检查并部署；针对性现场检查报告成功，
> 证据与局限见 [docs/phase1-m3c-design.md](docs/phase1-m3c-design.md) 第 18 节。
> M4“计数任务”已部署，自动化检查、数据库脚本与代表性 N=2 现场流程报告成功；负责人主动省略余下现场测试，
> 详见 [docs/phase1-plan.md](docs/phase1-plan.md) 第 10.8 节。M5-A“每天重复计数”已由 Codex 实现，并通过负责人执行的后端完整测试、前端单元测试及类型检查；0009 已应用测试库与主库，运行时与前端已部署，余下现场测试由负责人省略（未执行，不算通过），
> 详见 [docs/phase1-m5-daily.md](docs/phase1-m5-daily.md)。M5-B“每周重复计数”已实现，尚未构建、测试或应用 0010，
> 详见 [docs/phase1-m5-weekly.md](docs/phase1-m5-weekly.md)。自定义周期及 M6～M10 尚未获批。
> M5-C“指定日期范围计数”已实现并部署（迁移 0011 及修复 0012 已应用于测试库与主库并冻结，自动化测试通过；真实流程尚未验证），详见 [docs/phase1-m5-custom.md](docs/phase1-m5-custom.md)。
> M6“任务修改与取消（对方确认）”已实现并部署（迁移 0013 已应用于测试库与主库并冻结，自动化测试通过；真实流程尚未验证），详见 [docs/phase1-m6-design.md](docs/phase1-m6-design.md)。
> M7“奖励商店商品”和 M8“直接兑换”已实现、通过测试库自动化验证，并于 2026-10-26 由 Codex 按负责人授权联合部署至主库与网页（0014、0015 已应用并冻结）。真实上架／兑换／履约流程及浏览器更新尚未验收；发布证据见 [docs/phase1-m8-redemption.md](docs/phase1-m8-redemption.md) 第 7 节。
> M9“申请兑换与协商取消退款”由 Codex 直接实现并通过测试库自动化验证（0016 已应用测试库与主库并冻结），2026-10-26 已上线主库（接口与网页已部署；真实流程与浏览器验收尚未验证）。证据见 [docs/phase1-m9-redemption.md](docs/phase1-m9-redemption.md)。尚未提交或推送 Git。
> M10“首页四宫格与底部导航”（首页 / 任务 / 奖励 / 我的；今日任务与待处理提示）已在网页端实现并通过单元测试与类型检查，网页已于 2026-10-26 发布（在用网页哈希已核对；浏览器与设备验收尚未进行；阶段 1 尚未正式验收），详见 [docs/phase1-m10-home.md](docs/phase1-m10-home.md)。
> 阶段 1 已有限定结项记录（[docs/phase1-acceptance.md](docs/phase1-acceptance.md)，不代表可上线生产）。阶段 2 P2-1“双人留言板”已实现并通过自动化验证；迁移 0017 已应用测试库与主库，后端和网页已部署。手机与真实留言流程尚待验收，详见 [docs/phase2-messages.md](docs/phase2-messages.md) 第 13 节。
> 阶段 2 P2-2“纪念日与倒数日”设计已获负责人批准（2026-10-29），已实现，并通过测试库全部验证（回滚预验证、应用与赋权、权限偏差 0 行、后端完整测试、数据库脚本、前端单元测试与类型检查）；迁移 0018 仅应用于测试库并冻结，主库未应用，未部署。详见 [docs/phase2-anniversaries.md](docs/phase2-anniversaries.md) 第 14、15 节。
> 以上结果不代表穷尽的安全验证。

## 目录职责

| 路径 | 职责 |
| --- | --- |
| `apps/web` | 手机客户端（Vue 3 + TS + Vite + vite-plugin-pwa） |
| `apps/admin` | 管理界面骨架（仅本机回环访问，只读运维视图） |
| `services/api` | C++17 / Drogon 模块化单体后端：认证、空间、调度基础、管理接口 |
| `services/push-worker` | Node.js 24 / TS 无状态推送传输进程（默认关闭，无数据库访问） |
| `contracts` | OpenAPI 契约（阶段 0 操作保留，阶段 1 增量版本为 0.7.0） |
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
