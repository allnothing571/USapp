# 数据库备份与恢复（阶段 0）

> 本文档中的所有命令都是**由项目负责人手动执行**的说明。本次写作**没有执行任何一条命令**：
> 没有运行 `pg_dump`、`pg_restore`、`psql`，没有连接任何数据库，没有创建任何目录或文件。
> 步骤标注意图，不标注"已完成"。

---

## 1. 范围

**本文档只覆盖 PostgreSQL 数据库的备份与恢复。**

- ✅ 数据库：`pg_dump` 自定义格式备份、恢复到**空环境**、恢复后的核对方法。
- ✅ 备份中的敏感信息盘点（依据迁移文件）。
- ✅ 自动化备份的基本要求（执行位置、口令来源、dumps 不入库、轮换）。
- ❌ **媒体备份不在本文档范围内，且被阶段 0 有意排除。** 阶段 0 不接收原图，
  READY 原图需要独立的媒体备份与一次恢复演练（见 `docs/architecture.md` 第 5 节第 7 条），
  两者都留待后续阶段。**不要**把数据库备份当成媒体备份。

---

## 2. 备份（自定义格式）

`pg_dump` 的自定义格式（`-Fc`）是可选择性恢复、可压缩、可校验的格式，阶段 0 采用它。

```powershell
# 由负责人手动执行
$env:PGPASSWORD = '<usapp_api 口令>'
New-Item -ItemType Directory -Force -Path .\backups | Out-Null   # .env 中的 BACKUP_DIR
pg_dump -h 127.0.0.1 -p 5432 -U usapp_api -d usapp -Fc `
        -f ".\backups\usapp-$(Get-Date -Format yyyyMMdd-HHmmss).dump"
$env:PGPASSWORD = $null
```

说明：

- 备份文件放在 `.env` 的 `BACKUP_DIR`（样例值 `./backups`）下。
  `.gitignore` 已忽略 `backups/`、`*.dump`、`*.sql.gz`，因此 dump **不会**被提交。
- 备份应由**目录级或宿主机级**定时任务调用，口令来自 `.env`，不要写进脚本或命令行历史。
- 备份完成后建议用 `pg_restore --list <dump>` 检查目录列表是否非空（本次未执行）。
- 轮换：保留策略由负责人按磁盘与合规要求定义（例如保留最近 N 份日备 + M 份周备），
  旧的 dump 必须真正删除，而不是改名留在同目录。

---

## 3. 恢复到**空**环境

前提：目标实例是**全新、空**的数据库，不存在 `usapp` 库或已存在的同名对象。

```powershell
# 由负责人手动执行（使用超级用户仅在"建库"这一步；业务库的读写用 usapp_api）
$env:PGPASSWORD = '<POSTGRES_SUPERUSER_PASSWORD>'
createdb -h 127.0.0.1 -p 5432 -U postgres usapp_restore_check
$env:PGPASSWORD = $null

# 由负责人手动执行：把 dump 恢复到新库
$env:PGPASSWORD = '<usapp_api 口令>'
pg_restore -h 127.0.0.1 -p 5432 -U usapp_api -d usapp_restore_check `
           --no-owner --no-privileges --exit-on-error `
           ".\backups\<备份文件>.dump"
$env:PGPASSWORD = $null
```

要点：

- 恢复目标必须是**空库**。`--exit-on-error` 让任何一条失败立刻中止，避免"看起来恢复了、其实少了一半"。
- `--no-owner --no-privileges` 适用于目标实例上角色名与源不同（例如演练库）的情况。
- 迁移本身不创建任何扩展（本次检查 `database/migrations/*.sql` 中没有 `CREATE EXTENSION`），
  也没有创建角色或 `GRANT` 语句，因此恢复不需要额外的扩展/角色预置步骤。
- 需要重建**业务库**（而不是演练库）时，先确认源库已经离线或不再接受写入，再执行替换；
  替换不是本文档的推荐路径，演练库核对通过后再人工决定。

---

## 4. 恢复后的核对

### 4.1 关键表行数

在演练库上查询以下五张表的行数，并与其来源库（或备份前的已知值）逐一比对：

```sql
-- 由负责人手动执行
SELECT 'users'          AS table_name, count(*) FROM users
UNION ALL SELECT 'couples',        count(*) FROM couples
UNION ALL SELECT 'couple_members', count(*) FROM couple_members
UNION ALL SELECT 'sessions',       count(*) FROM sessions
UNION ALL SELECT 'audit_logs',     count(*) FROM audit_logs;
```

判定：

- 五张表的行数与来源库**完全一致**（`sessions` 因为存在过期清理与登录产生的写入，可能因时间差而不同，
  此时必须能解释差异来源）。
- `couple_members` 的行数不得超过 `couples` 行数的 **2 倍**：`couple_members` 上有
  `couple_members_couple_slot_unique UNIQUE (couple_id, slot)` 与 `couple_members_slot_valid CHECK (slot IN (1,2))`
  （见 `database/migrations/0003_couple_binding.sql`），空间容量由 slot 域定义。

### 4.2 迁移记录数必须与迁移文件数一致

```sql
-- 由负责人手动执行
SELECT count(*) AS applied FROM schema_migrations;
SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version;
```

核对方法：`database/migrations/` 目录下的 `.sql` 文件数为 **6**
（`0000_schema_migrations`、`0001_common`、`0002_identity`、`0003_couple_binding`、
`0004_scheduler`、`0005_admin_audit`），因此完全应用后的 `schema_migrations` 行数也必须是 **6**。

依据：`database/apply.ps1` 对每个 `*.sql` 文件都以**文件基名**作为 `version` 写入一行
（包括 `0000_schema_migrations.sql` 本身），并在已应用且 checksum 一致时跳过。所以：

- 行数 **小于** 6 → 有迁移没应用；
- 行数 **大于** 6 → 出现了没有对应文件的版本记录（例如手工插入，或删除了迁移文件）；
- 任何一行的 `checksum` 与该文件的 SHA-256 不一致 → `apply.ps1` 会直接失败并提示
  "禁止修改已应用的迁移，请新增一个迁移文件"。

### 4.3 备份里有没有明文密钥

结论：**备份中不存在应用层面的明文口令、明文会话令牌或明文验证码**，
但备份中**确实**包含应用运行所必需的、哈希后的凭据，以及业务数据本身。逐项依据：

| 数据 | 迁移中的列 | 存储形式 | 依据文件 |
| --- | --- | --- | --- |
| 情侣端会话令牌 | `sessions.token_hash`（`UNIQUE`） | SHA-256 散列，Cookie 中才是不透明随机串 | `database/migrations/0002_identity.sql` |
| 会话绑定的 CSRF 令牌 | `sessions.csrf_token_hash` | SHA-256 散列 | `database/migrations/0002_identity.sql` |
| 邮箱验证码 | `email_verification_codes.code_hash` | SHA-256 散列（明文只交给 Mailer，绝不入库） | `database/migrations/0002_identity.sql` |
| 邀请码 | `space_invites.code_hash`（`UNIQUE`）、`space_invites.code_prefix` | SHA-256 散列 + 仅用于展示的前缀 | `database/migrations/0003_couple_binding.sql` |
| 管理员口令 | `admin_users.password_hash` | **PBKDF2**，格式 `pbkdf2-sha256$<iterations>$<salt-b64>$<hash-b64>` | `database/migrations/0005_admin_audit.sql`、`services/api/src/core/Crypto.h` |
| 管理会话 | `admin_sessions.token_hash`、`admin_sessions.csrf_token_hash` | SHA-256 散列 | `database/migrations/0005_admin_audit.sql` |

其它必须知道的点：

- **业务数据本身当然在备份里**（`users.email_canonical`、`couples`、`couple_members`、`audit_logs`
  与后续阶段的业务表）。备份文件的访问控制等级必须与数据库同级。
- 管理员**用户名**（`admin_users.username`）是明文列，这不是密钥泄漏，但仍是身份信息。
- 数据库口令、`SESSION_SECRET`、SMTP 口令、VAPID 私钥**不在备份里**：它们只存在于 `.env` / 密钥文件。
  因此"恢复完成后应用能起来"还依赖这些密钥同样被安全地保存与轮换——这一点不在本文档的核对范围内。
- 审计日志 `audit_logs` 的列（见 `0005_admin_audit.sql`）包含 `actor_type ∈ {user, admin, system}`、
  `actor_id`、`couple_id`、`action` 等，恢复后应与来源库一致。

---

## 5. 自动化备份的基本要求

1. **从宿主机执行，而不是从应用服务器执行。** 备份进程需要读取数据库口令并写出 dump 文件，
   把它放在对外提供服务的应用主机上会扩大凭据与文件的暴露面。
2. **口令来自 `.env`。** 不要在计划任务/CI 的明文命令行里内联口令，
   不要为备份单独建一个可达全库的超级用户（业务库读写用 `usapp_api` 即可）。
3. **绝不提交 dump。** `.gitignore` 已经忽略 `backups/`、`*.dump`、`*.sql.gz`，
   但忽略规则不是权限控制：备份目录仍需限定本机访问。
4. **必须轮换。** 定义保留窗口并让脚本真正删除过期文件；同时定期（至少一次）执行第 3～4 节的
   **恢复演练**，否则"备份存在"并不等于"能恢复"。
5. **不要修改已应用的迁移来修数据。** 备份恢复流程本身不授权改动 `database/migrations/*.sql`；
   修正必须通过新迁移文件（`apply.ps1` 的 checksum 校验会直接拒绝被修改的历史迁移）。

---

## 6. 已实现 / 仅骨架 / 尚未实现

### 已实现（文件存在，仍未执行验证）

- `database/apply.ps1`：按序应用迁移、checksum 校验、跳过已应用版本。
- `database/migrations/0000_schema_migrations.sql` … `0005_admin_audit.sql`：全部表结构、
  唯一约束与检查约束（备份/恢复的对象就是它们）。
- `.env.example` 中的 `POSTGRES_*`、`POSTGRES_SUPERUSER*`、`BACKUP_DIR`。
- `.gitignore` 中对 `backups/`、`*.dump`、`*.sql.gz`、`.env`、`*.key`、`*.pem` 的忽略规则。

### 仅骨架

- **备份脚本本身不存在**：仓库里没有 `database/backup*.ps1` 之类的脚本，
  第 2 节的命令需要负责人自行封装成脚本或计划任务。
- 恢复演练**没有任何脚本或清单文件**，第 3、4 节是纯手工步骤。

### 尚未实现

- 媒体（原图）的独立备份与恢复演练——**阶段 0 有意排除**。
- 备份加密、异地副本、恢复时间点目标（RPO/RTO）的定义与自动化校验。
- 备份完成后的自动校验（例如自动 `pg_restore --list` + 行数比对）。

---

## 7. 尚未执行的检查

1. 未执行 `pg_dump`，没有生成任何备份文件。
2. 未执行 `createdb` / `pg_restore`，没有恢复过任何库。
3. 未执行第 4.1 节的行数查询，五张表的行数一致性**未经验证**。
4. 未执行第 4.2 节的迁移记录数核对（文件数 = 6 这一事实来自目录列表，行数 = 6 是**推断**，未查询数据库）。
5. 未验证"备份中不存在明文密钥"这一结论——它来自对迁移文件列的**静态阅读**，
   没有对任何真实 dump 做过内容检查（例如 `pg_restore --list` 或字符串搜索）。
6. 未配置任何计划任务、保留策略或轮换脚本。
7. 未执行任何恢复演练，因此 RPO/RTO 未知。

---

## 附：本文档依据的文件

- `database/migrations/0000_schema_migrations.sql`、`0002_identity.sql`、`0003_couple_binding.sql`、`0005_admin_audit.sql`
- `database/apply.ps1`
- `.env.example`、`.gitignore`
- `README.md`（目录职责表）
