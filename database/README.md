# database

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
