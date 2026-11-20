# database

2026-11-10 成就首批：`0047_achievements.sql` 在一次性实例冻结，sha256 `75d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132`。仅增加只读统计内核与维护专用的一次性启用记录，不发资产、不修改任务完成路径。应用迁移不会启用统计；`achievement_enable.sql` 是另行确认目标后由维护身份操作的入口，本轮未在共享或生产库执行。48 个迁移校验和匹配；下一编号 0048。实现、规则、失败与最终隔离回归见 [成就文档](../docs/phase4-achievements.md)。

P2-6 R1: `0025_reminders.sql` 新增只追加的提醒规则与提醒发生记录（仅 SELECT/INSERT），并为通知增加封闭的提醒事件族（只有该事件族的 `actor_id` 可为 NULL）。已冻结，仅在一次性隔离实例中应用；共享 usapp_test 与主库均未应用。证据见 [提醒文档第 17 节](../docs/phase2-reminders.md#17-r1-implementation-and-isolated-verification-claude-code-2026-11-02)。

P2-6 R4: `0027_reminder_successor_rules.sql` 为提醒规则增加来源字段与守卫，任务修改获批时复制接收者最新规则，并提供 `needs_reset` 的统一定义。已冻结，仅在一次性隔离实例中应用。证据见提醒文档第 20 节。

P3-1a: `0029_media.sql` 增加私有媒体的元数据、不可变最终对象、清理围栏与迟到对象记录，图片字节不进数据库。已冻结，仅在一次性隔离实例中应用。证据见第 3 阶段计划第 21 节。

P3-1b: `0030_memories.sql` 增加只追加的照片回忆、修改申请与决定，并替换 0029 的清理计划与绑定守卫：待处理申请只预留照片，批准时才绑定，拒绝或撤回即释放。已冻结，仅在一次性隔离实例中应用。证据见第 3 阶段计划第 22 节。

P3-2a: `0031_albums.sql` 增加只追加的共享相册，绑定类型新增 `album`，并替换 0030 的绑定守卫：照片只有一个归属，移除只隐藏，绑定与字节保留到 D6。已冻结，仅在一次性隔离实例中应用。证据见相册设计第 12 节。

P3-2b: `0032_album_rename.sql` 增加只追加的相册改名申请与决定，扩展 `album_revisions` 并替换其守卫：有待处理申请时不能直接改名，批准时应用申请的名称。已冻结，仅在一次性隔离实例中应用。证据见改名设计第 7 节。

P2-6 R5: `0028_reminder_push_stale.sql` 增加提醒来源有效性函数、内部结果 `source_stale_skipped` 与终态 `source_stale`，不改动任何已有行。已冻结，仅在一次性隔离实例中应用。证据见提醒文档第 21 节。

P2-6 R1 评审修复: `0026_reminder_next_fire.sql` 仅替换两个只读函数，使 `next_fire_at` 排除已触发的提醒身份。已冻结，同样仅在一次性隔离实例中应用。证据见提醒文档第 17.8 节。

P2-5b 修复：`0024_push_dispatch_repairs.sql` 已冻结，仅在一次性隔离测试实例中应用并验证；共享 usapp_test 仍为 0000-0023，主库仍为 0000-0017。证据见 [Web Push 文档第 15 节](../docs/phase2-web-push.md#15-p2-5b-isolated-repair-verification-claude-code-2026-11-02)。

P2-5b: `0023_push_dispatch.sql` 新增只追加的派发控制、投递、尝试、观测、租约过期与终态表（仅 SELECT/INSERT；启用、停用、恢复只能由维护角色执行，应用角色只能自动暂停），并新增不属于用户操作的 `gone` 撤销原因。0023 仅应用并冻结于 usapp_test，主库未应用。证据见 [Web Push 文档第 13 节](../docs/phase2-web-push.md#13-p2-5b-implementation-and-verification-claude-code-2026-11-02)。

P2-5a: `0022_push_subscriptions.sql` 新增不可变订阅代、撤销与注册命令三张只追加表（仅 SELECT/INSERT；每用户最多 10 台活跃设备、每个端点最多一个未撤销代、会话/空间/时间与延迟链接守卫）。0022 仅应用并冻结于 usapp_test，主库未应用。证据见 [Web Push 文档第 12 节](../docs/phase2-web-push.md#12-p2-5a-implementation-and-verification-claude-code-2026-10-30)。

P2-4: `0021_task_reactions.sql` 新增绑定实际完成记录的只追加回应表；仅 SELECT/INSERT，来源/角色/时间守卫。0021 仅应用并冻结于 usapp_test，主库未应用。双角色 SQL、权限重复检查和完整回归证据见 [回应文档第 8 节](../docs/phase2-reactions.md#8-implementation-and-verification-codex-2026-10-30)。

P2-3a 新增 `0019_notifications.sql`，通知与已读回执仅 SELECT/INSERT，来源/角色/时间守卫已验证。0019 仅在 usapp_test 应用并冻结，主库未应用。P2-3b 新增 `0020_notification_choices.sql`（通知意图表与可选通知来源；2026-10-30 仅应用 usapp_test 并冻结，主库未应用）及 `tests/notification_choice_test.sql`；详见通知文档第 13 节。详见 [通知记录](../docs/phase2-notifications.md#11-p2-3a-implementation-and-verification-codex-2026-10-29)。

PostgreSQL 17 迁移与数据库侧测试。

## 迁移文件

按文件名升序执行，每个文件自带显式事务。**已应用的迁移文件禁止修改内容**，
迁移记录表会校验 `checksum`，改动会被检测出来。

| 文件 | 内容 |
| --- | --- |
| `0000_schema_migrations.sql` | `schema_migrations` 迁移记录表 |
| `0001_common.sql` | `usapp_set_updated_at()` 触发器函数 |
| `0002_identity.sql` | `users` / `sessions` / `email_verification_codes` |
| `0003_couple_binding.sql` | `couples` / `couple_members` / `space_invites` |
| `0004_scheduler.sql` | `scheduler_ticks`（调度基础） |
| `0005_admin_audit.sql` | `admin_users` / `admin_sessions` / `audit_logs` + 只读视图 |
| `0006_wallet_ledger.sql` | 阶段 1 M2-A：`wallet_accounts` / `wallet_operations` / 只追加的 `wallet_ledger_entries`；链式校验、上限、只追加触发器、钱包表权限收窄；不写入任何钱包数据 |
| `0007_tasks_one_off.sql` | 阶段 1 M3-A：`tasks` / 只追加的 `task_completions`；奖励与文本约束、完成记录校验、任务结算一致性（含钱包表上仅对任务操作生效的延迟校验）、任务表权限收窄；不写入任何数据 |
| `0008_task_counting.sql` | M4：任务完成模式与目标次数、只追加的 `task_progress_events`、连续进度与最终结算关联守卫、进度表权限收窄；测试库与主库均在使用，禁止修改 |
| `0009_daily_tasks.sql` | M5-A：每日模板与按日实例、周期快照／日期守卫、仅 SELECT 的 `task_read_model`；已应用测试库与主库并冻结 |
| `0010_weekly_tasks.sql` | M5-B：每周模板（北京时间周一 0 点至下周一 0 点）；原地替换 0009 的两个守卫函数与 `task_read_model`（列与权限不变），每日规则不变；尚未应用任何数据库 |
| `0011_custom_range_tasks.sql` | M5-C：指定日期范围（北京时间开始／结束日期均含当天）的计数任务；新增 `tasks.start_local_date`、开始前拒绝进度的守卫、视图末尾追加该列（权限不变）；已应用测试库与主库并冻结 |
| `0012_custom_action_guard.sql` | M5-C 修复：原地替换 0010 的 `usapp_daily_action_guard`，使指定日期范围任务不再被当作模板拒绝（仅一行变化，每日／每周规则不变）；已应用测试库与主库并冻结 |
| `0013_task_changes.sql` | M6：任务修改／取消的提议、决定与关闭记录（只追加），经确认的修改以新任务替换原任务；进度、完成与新周期在关闭后由数据库拒绝；视图列不变；三张新表只授予 SELECT, INSERT；已应用测试库与主库并冻结 |
| `0014_reward_products.sql` | M7：奖励商店商品（不可变身份 + 每次上架／修改／下架一条只追加的版本记录；价格 1–999 小红花；下架单向；不写钱包）；两张新表只授予 SELECT, INSERT；已应用测试库与主库并冻结（2026-10-26 联合发布，证据见 ../docs/phase1-m8-redemption.md 第 7 节） |
| `0015_reward_redemptions.sql` | M8：直接兑换（快照只追加；同一事务扣除小红花并写审计；每个 REWARD_REDEMPTION 操作必须恰好关联一条兑换与一条等额扣减，延迟检查；上架一方确认履约，只追加且仅一次；仅直接兑换方式）；两张新表只授予 SELECT, INSERT；已应用测试库与主库并冻结（2026-10-26 联合发布，证据见 ../docs/phase1-m8-redemption.md 第 7 节） |

M9 新增 `0016_redemption_workflow.sql`：申请快照允许空扣款操作；确认、拒绝、
撤回及协商取消退款使用只追加事件，状态由仅 SELECT 的视图推导；数据库验证精确
扣款／退款关联，履约与取消共用兑换锁。已应用测试库与主库并冻结（2026-10-26 上线，证据见 M9 文档第 5 节）。
证据与校验值见 [M9 文档](../docs/phase1-m9-redemption.md)。

阶段 2 P2-1 新增 `0017_messages.sql`：双人留言板（`messages` 与 `message_replies` 两张只追加的表；
回复以留言 id 为主键，每条留言至多一条回复，且只能由收到留言的一方回复；成员关系由外键约束；
只授予 SELECT, INSERT）。已应用测试库与主库并冻结。证据与校验值见
[P2-1 文档](../docs/phase2-messages.md) 第 10、13 节。

阶段 2 P2-2 新增 `0018_anniversaries.sql`：纪念日与倒数日（身份、版本、修改申请、处理结果与个人首页置顶
五张只追加的表；创建者直接修改，对方提交修改申请，同一纪念日至多一条待处理申请；守卫 UN001～UN006，
锁 33010／33011；只授予 SELECT, INSERT，`init-roles.sql` 第 16 节）。测试脚本
`tests/anniversary_ledger_test.sql`（两种身份运行，以 ROLLBACK 结束）。已于 2026-10-29 应用测试库并冻结，
**主库尚未应用**。详见 [P2-2 文档](../docs/phase2-anniversaries.md) 第 14、15 节。

### 钱包表（0006）要点

- 账户不保存余额或序号；余额 = 该账户 `seq` 最大的一条流水的 `balance_after`，没有流水即为 0。
- 每条流水在账户行锁下对照上一条不可变流水校验：`seq` 连续、`balance_after = 上一条 + amount`（起始状态为 0）。
- 每条 INSERT 只能写一行流水；三张表的 UPDATE / DELETE / TRUNCATE 由触发器拒绝（对维护角色同样生效）。
- `usapp_api` 只有三张表的 SELECT、INSERT，以及 `wallet_accounts.lock_token` 的列级 UPDATE（仅用于行锁，真正的 UPDATE 仍被触发器拒绝）；`usapp_push` 与 PUBLIC 没有任何权限。
- `init-roles.sql` 第 7 节在 ALL TABLES 授权之后重新收窄钱包表权限，因此重新执行 `apply.ps1 -InitRoles` 不会放宽。
- `0006_wallet_ledger.sql` 已在测试库与主库应用，**禁止修改**（SHA-256 `2633e17ea6ff6e664832df84023d1fab8aa9513b4cfe48786c4ca093bc7d543d`）。

### 任务表（0007）要点（阶段 1 M3-A；2026-10-09 在 `usapp_test` 通过负责人执行的验证；2026-10-10 已应用到主库）

- 验证结果与局限见 [docs/phase1-m3-design.md](../docs/phase1-m3-design.md) 第 11.3 节。`0007_tasks_one_off.sql` 已在测试库 `usapp_test` 应用，**禁止修改**（SHA-256 `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`）。2026-10-10 负责人在新鲜备份与（按基本计数核对的）恢复检查之后，以 `-InitRoles` 将 0007 应用到主库，校验和一致，重复 `-InitRoles` 后权限检查不变（见 `docs/phase1-m3-design.md` 第 12.9 节）。0007 现已在测试库与主库应用，**禁止修改**。
- 该验证覆盖迁移、权限、CTest 与两份 psql 脚本；不包含任务服务或接口、真实结算服务，也不包含针对已提交结算的跨事务攻击测试（属于 M3-B/M3-C）。
- M3-B（任务发布与读取）不新增迁移，但 API 运行时启动时会检查任务表权限矩阵，缺少 0007 或权限不符即拒绝启动。
  2026-10-10 主库已按 `docs/phase1-m3-design.md` 第 12.8 节的顺序应用 0007 并部署 M3-B 运行时（启动时钱包与任务权限检查均为 ok=true）；
  负责人同日在已记录检查的范围内验收 M3-B（第 12.10 节）。
- M3-C（完成任务与结算）已实现并通过负责人记录的检查与部署（`docs/phase1-m3c-design.md` 第 18 节）；不新增迁移。
- M3-B 的 db_gated 测试会在 `usapp_test` 中**永久提交**按次隔离的任务夹具（`@m3-fixture.test` 域名的用户、
  空间、任务与 `task.created` 审计；不含钱包行）。此后“`usapp_test` 中没有 `tasks` 行”不再成立，
  检查应按夹具标识过滤。

- `0007_tasks_one_off.sql`：一次性任务 `tasks`（发布后不可修改）与完成记录 `task_completions`（只追加，每个任务至多一条）。
- 奖励为整数：小红花 0-999、阳光值 0-100000，且至少一项为正数；标题去首尾空白后 1-40 个码点、无控制字符与换行；描述最多 500 个码点，允许 LF，不允许 CR 与其他控制字符；时区仅支持 `Asia/Shanghai`，`deadline_at` 必须与本地日期/时间一致（只有日期时为次日 0 点）。
- 发布时**不**在数据库中要求截止时间在未来（服务端在幂等分类之后校验，避免误拒重放）。
- 完成记录插入时立即校验：任务与空间、完成者是接收者、奖励快照、钱包操作的类型/来源/发起者/空间、完成时间（不晚于当前、不早于任务创建、严格早于截止时间）。
- 共享结算校验器 `usapp_task_settlement_validate` 由三个延迟触发器调用：完成记录插入、`source_type = 'task'` 的钱包操作插入、以及任何流水插入（仅对任务来源的操作生效）。之后任何事务向任务操作追加流水都会被重新校验；非任务操作不受影响。
- `usapp_api` 对两张任务表只有 SELECT、INSERT，没有任何 UPDATE（包括列级）；`usapp_push` 与 PUBLIC 无权限；`init-roles.sql` 第 8 节在 ALL TABLES 授权之后重新收窄。
- 自定义 SQLSTATE：`UT001` 只追加；`UT002`-`UT006` 完成记录立即校验；`UT007` 结算整体不一致。

### 计数任务（0008，M4；已部署，负责人验证范围见计划第 10.8 节）

- 旧任务默认 `single` / 目标 1；新计数任务目标为正整数（存储范围 1～2147483647），发布后不可更改。
- 进度事件只追加，接收者可记录；同用户同进度键至多一次，任务次数从 1 连续增长到目标。
- 未到目标不生成钱包操作。最终事件必须关联同事务的完成与结算，不能通过旧完成记录绕过目标。
- 使用保留的双整数事务咨询锁 `(33004, hashtext(规范任务 UUID))` 串行处理；哈希碰撞只增加等待。
- `usapp_api` 对进度表只有 SELECT、INSERT；`init-roles.sql` 第 9 节重复收窄。新运行时启动权限检查包含进度表，必须先应用 0008。
- 详细实现、验证顺序与回退局限见 `docs/phase1-plan.md` 第 10 节；0000～0007 不变。
- 0008 在测试库与主库使用，禁止修改；测试库应用及主库预览的 SHA-256 为 `36fe053917c86bcd7ed1931b085f59effd778205d93a1ac871e53b3336413065`。主库实际迁移命令输出未提供，后续权限与运行结果已记录；勿补造退出码。

### 每日计数（0009，M5-A；已应用测试库与主库）

- 每日模板不可修改，每个日期至多一个实例；首次 +1 才创建，GET 不写数据库。按北京时间午夜重新从零计数，无补发、扣分或自动奖励。
- 实例有同空间的模板外键、内容快照与日期守卫，模板不能直接写进度或完成。实例复用原有计数及钱包结算约束。
- `task_read_model` 使用 `security_invoker`，只投影当前周期和有存储记录的历史周期；对 API 仅 SELECT，PUBLIC／推送角色无权访问。`init-roles.sql` 第 10 节重复收窄，启动权限检查也包含该视图。
- 0009 已应用 `usapp_test` 并冻结，SHA-256 为 `9f739a6b2675e3ecf6624400433f8790c31f2d97a0b91c07f9fac5aed3c76c86`；重复赋权与数据库脚本兼容性已通过，主库已应用。详细证据、未验证风险与后续顺序见 [phase1-m5-daily.md](../docs/phase1-m5-daily.md) 的 Verification；0000～0009 禁止修改。新运行时要求先应用 0009；存在每日任务后，旧 M4 镜像不具备透明回退兼容性。M5-B 运行时要求先应用 0010，见 [phase1-m5-weekly.md](../docs/phase1-m5-weekly.md)。

## 关键约束（阶段 0 决策，后续阶段不得放宽）

- `couple_members.user_id` **NOT NULL + UNIQUE**：阶段 0 每个用户最多属于一个 couple。
- `couple_members(couple_id, slot)` **UNIQUE** 且 `slot IN (1,2)`：每个空间每个槽位最多一人。
- `slot` 不代表性别、角色或权限；双方权限完全对称。
- `couples.timezone` 阶段 0 固定写入 `Asia/Shanghai`，但列保留。
- 库内只保存验证码 / 会话令牌 / CSRF 令牌 / 邀请码的 **SHA-256**，不保存明文。
- 管理员在 `admin_users`，与 `users` 无任何外键关系，不通过情侣角色授予管理权限。
- 刻意不引入 `max_members` 列：容量由 `slot` 取值域 `(1,2)` 定义，避免约束自相矛盾。

## 手动执行（由项目负责人执行，Agent 不执行）

本地 PostgreSQL 17 已启动、已创建数据库 `usapp` 后：

```powershell
$env:PGPASSWORD = "<POSTGRES_PASSWORD>"
psql -h 127.0.0.1 -p 5432 -U usapp_api -d usapp -v ON_ERROR_STOP=1 -f database/apply.ps1  # 见下方说明
```

更推荐使用随仓库提供的应用脚本（它负责顺序、事务与 checksum 校验）：

```powershell
pwsh -NoProfile -File database/apply.ps1 `
  -Host 127.0.0.1 -Port 5432 -Database usapp -User usapp_api
```

`apply.ps1` 需要环境变量 `PGPASSWORD`，或通过 `-Password` 传入（不推荐，会进入命令历史）。

## 数据库侧测试（只编写，不执行）

`database/tests/` 下的 `.sql` 文件使用 `psql` 直接执行，脚本内通过
`DO $$ ... RAISE EXCEPTION ... $$` 做断言，失败即非零退出。

```powershell
psql -h 127.0.0.1 -U usapp_api -d usapp -v ON_ERROR_STOP=1 -f database/tests/constraints_test.sql
psql -h 127.0.0.1 -U usapp_api -d usapp -v ON_ERROR_STOP=1 -f database/tests/advisory_lock_test.sql
```

> 这些测试需要在**已应用全部迁移**的数据库上运行；`advisory_lock_test.sql`
> 需要两个并发连接，因此必须用 psql 的 `\connect`/后台会话方式运行（文件内已写明步骤）。

`wallet_ledger_test.sql`（阶段 1 M2-A）只在测试库 `usapp_test` 上运行，并且要运行两次：
一次以 `usapp_api`（验证权限屏障，期望 42501），一次以维护超级用户（验证触发器，期望 `UW001`）。
脚本自行识别运行身份，其他身份直接报错；全程 `BEGIN ... ROLLBACK`，不提交任何数据；
任何意外成功都会 `RAISE EXCEPTION` 并使 psql 以非零码退出。

`task_ledger_test.sql`（阶段 1 M3-A）同样只在 `usapp_test` 上、以两种身份各运行一次：
`usapp_api` 下任务表的 UPDATE / DELETE / TRUNCATE 期望 42501，维护超级用户下期望 `UT001`。
它在任何夹具写入之前检查 `current_database() = 'usapp_test'`，全程 `BEGIN ... ROLLBACK`。
跨事务攻击“已真正提交的结算”需要已提交的夹具，属于 M3-C，本脚本不覆盖。
