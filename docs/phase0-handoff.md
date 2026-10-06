# USapp 阶段 0 收尾交接文档 (Phase 0 Handover)

> **核心声明**：
> 1. 本次交接文档及代码变更均**严格遵循阶段 0 边界**：仅读取和修改配置文件、代码与文档，**未执行任何终端命令**（未执行安装、构建、测试、迁移、服务启动或 Git 提交）。
> 2. 文档中的所有功能与状态结论均来自**静态审查与文件比对**；所有测试、构建与部署步骤必须由项目负责人按照手册**手动执行验证**。
> 3. 不包含任何真实密码、会话密钥或凭据；不读取或覆盖现有真实 `.env` 文件。

---

## 1. 修改摘要

针对审查中发现的配置偏差、权限隔离隐患与操作流程缺陷，本次进行了系统性的标准化修正：

1. **统一环境变量命名与管理员引导契约**：
   - 纠正了手册中对同源变量的误称（将 `API_PUBLIC_ORIGIN` 统一为代码实际加载的 `PUBLIC_ORIGIN`）。
   - 纠正了管理员引导参数（废除臆造的 `ADMIN_BOOTSTRAP_ENABLED` / `PASSWORD`，统一为代码严格校验的 `ADMIN_BOOTSTRAP_USERNAME` 与 `ADMIN_BOOTSTRAP_PASSWORD_HASH`），并给出标准的 PBKDF2-SHA256 哈希生成命令行。
2. **数据库账户最小权限分离与角色初始化落地**：
   - 彻底解决了官方 `postgres:17` 镜像默认将 `POSTGRES_USER` 设为超级用户的越权隐患：明确将集群超级用户绑定为 `postgres`（密码由 `POSTGRES_SUPERUSER_PASSWORD` 控制），专门用于 DDL 迁移、角色管理与数据备份恢复。
   - 创建了 [deploy/docker-entrypoint-initdb.d/01-init-roles.sh](file:///d:/JYJ/USapp/deploy/docker-entrypoint-initdb.d/01-init-roles.sh) 与 [database/init-roles.sql](file:///d:/JYJ/USapp/database/init-roles.sql)，创建受限的 `usapp_api` 业务运行账户（`NOSUPERUSER`，仅授予业务表 DML 读写与序列权限）以及受限的 `usapp_push` 推送账户（`NOSUPERUSER`，阶段 0 严格隔离，禁止访问用户、空间、管理员及审计表）。
3. **消除 Compose 服务间的凭据泄露风险**：
   - 彻底移除了 `postgres`、`api` 与 `push-worker` 服务对全局 `env_file: - ../.env` 的直接挂载，改为在 Compose 中按服务显式注入变量。
   - API 容器不再获得超级用户密码与推送密钥；推送容器不再获得 `SESSION_SECRET`、管理员引导凭据或数据库超级用户密码。
4. **测试运行路径标准化（容器内编译运行，零 Windows 宿主工具链依赖）**：
   - 在 [deploy/Dockerfile.api](file:///d:/JYJ/USapp/deploy/Dockerfile.api) 中补充了多阶段构建目标 `tester`，复用编译好的 Drogon、OpenSSL 与 libpq 开发库。
   - 在 [deploy/docker-compose.yml](file:///d:/JYJ/USapp/deploy/docker-compose.yml) 中新增 `test-runner` 服务（受 `profiles: ["test"]` 保护），测试库连接通过 Docker 内网直接连通 `postgres:5432/usapp_test`，无需对外发布 5432 端口即可运行全量 12 个测试用例。
5. **迁移执行入口、版本格式与原子事务强化**：
   - 重构了 [database/apply.ps1](file:///d:/JYJ/USapp/database/apply.ps1)：提供默认 Docker 容器模式（免宿主 psql 与 5432 暴露）与 Host 模式；统一迁移版本为完整文件名 BaseName；计算原始 SQL 的 SHA-256 校验和；将迁移 DDL 与 `schema_migrations` 记录包装在单事务中原子提交；加入 `pg_advisory_xact_lock(7492000)` 并发锁；DryRun 保持纯只读，不提前建表。
   - 修正了 [services/api/tests/DbGatedTest.cc](file:///d:/JYJ/USapp/services/api/tests/DbGatedTest.cc)：精准校验阶段 0 的全部 6 个完整 BaseName 迁移版本。
   - 强化了 [database/init-roles.sql](file:///d:/JYJ/USapp/database/init-roles.sql)：动态对当前数据库授权 `CONNECT`，角色不存在时明确中断，严禁弱密码兜底。
   - 修复了备份恢复逻辑：杜绝 PowerShell 二进制文本重定向造成的 dump 损坏；备份在容器内生成后经 `docker compose cp` 导出；恢复先复制进容器再执行 `pg_restore`；验证完成前保留备份文件与恢复库供人工复核。
   - 修复了配置模板复制指令：增加存在性检查，绝不意外覆盖现有 `.env`；构建与安装步骤修正为明确标注写入磁盘。

---

## 2. 新增及修改文件清单

| 相对路径 | 变更类型 | 变更内容说明 |
| --- | --- | --- |
| `database/apply.ps1` | **修改** | 迁移应用主入口：支持 Docker/Host 模式、完整 BaseName、单事务原子提交、并发锁与纯只读 DryRun |
| `database/init-roles.sql` | **修改** | 动态适配当前数据库、角色缺失报错、超级用户与应用/推送角色最小权限分离 |
| `services/api/tests/DbGatedTest.cc` | **修改** | 数据库门控测试：精准比对 6 个 BaseName 迁移版本存在性 |
| `deploy/docker-entrypoint-initdb.d/01-init-roles.sh` | **新增** | 容器初始化角色脚本：创建 `usapp_api` 与 `usapp_push` 受限角色并设置默认权限 |
| `deploy/Dockerfile.api` | **修改** | 补充 `tester` 多阶段构建目标，配置 Drogon 子模块递归克隆与共享库选项 |
| `deploy/docker-compose.yml` | **修改** | 消除全局 `env_file`，显式分发服务环境变量；绑定超级用户与初始化脚本；新增 `test-runner` 容器服务 |
| `.env.example` | **修改** | 补充 `POSTGRES_PUSH_*` 受限配置；完善 `ADMIN_BOOTSTRAP_*` 格式与 PBKDF2 生成说明 |
| `deploy/README.md` | **修改** | 同步多账户隔离架构、`apply.ps1` 容器迁移命令、容器测试命令及安全的备份恢复流程 |
| `services/api/tests/README.md` | **修改** | 补充容器内执行测试的命令，标明宿主机直连时的端口发布前置条件与 BaseName 迁移依赖 |
| `docs/phase0-handoff.md` | **修改** | 重构交接手册，修复全部已知问题，提供完整跨平台操作指南 |

*(注：此前已落盘的测试文件 `UnitTest.cc`、`DbGatedTest.cc`、`StaticCheckTest.cc`、`CMakeLists.txt`、根目录 `/.dockerignore` 以及控制器路由参数形参修复均保持完备。)*

---

## 3. 功能状态划分

### A. 已实现（代码与静态配置就绪）
- **身份认证与会话管理**：
  - 邮箱注册白名单校验（`RegistrationPolicy`）与大小写规范化。
  - 验证码哈希存储、TTL、冷却时间与频控限流。
  - Cookie 安全加固：`__Host-` 前缀、`SameSite=Strict`、`Secure`、`Path=/`；Origin 精确比对与 CSRF 双重提交。
  - 登录成功原子撤销预认证会话并换发新会话（Session Fixation 防护）。
- **双人空间与邀请模型**：
  - 双人空间模型（严格 1 对 1）：`couple_members.user_id UNIQUE`，`CHECK (slot IN (1, 2))`，无特权与性别角色。
  - 邀请码生命周期：生成 12 字符高熵邀请码，库内存 SHA-256，行级锁原子单次消费。
- **进程内调度基础**：
  - `Scheduler` 基于 `pg_try_advisory_xact_lock` 的分布式互斥心跳任务；事务回滚自动释放锁。
- **安全边界与反向代理**：
  - Caddy 双站点架构：公开业务面 `8443` 与宿主回环管理面 `8444`。
  - 管理面多重防护：回环发布、Caddy 注入 `X-Usapp-Admin-Surface`、`AdminFilter` 校验 TCP 对端 CIDR、独立 `admin_users` 体系。
- **权限与部署隔离**：
  - 数据库角色三权分立（超级用户 `postgres`、业务运行 `usapp_api`、受限推送 `usapp_push`）。
  - Compose 变量精细分发，无秘密跨服务扩散。

### B. 仅有骨架（待外部工具链产出）
- `services/push-worker`：目录已具备 `package.json`（Node 24 / TS），依赖锁文件 `pnpm-lock.yaml` 需负责人在本地执行 `pnpm install` 生成。阶段 0 通过 Compose profile 默认保持关闭。

### C. 尚未实现（严格界定为阶段 1 及以后）
- 待办任务清单、任务状态流转与双人协作机制。
- 积分钱包、代币流转、兑换规则与结算流水。
- 媒体/原图私有云直传与 OSS 授权预签名。
- 真实邮件 SMTP 发送器（阶段 0 仅提供 `log` 模拟输出）。

---

## 4. 负责人配置清单（绝不含真实凭据）

请确保根目录 `.env` 包含以下配置项，并替换占位符：

```bash
# =====================================================================
# USapp 阶段 0 本地环境配置清单
# =====================================================================

# ---------- 基础环境与同源 ----------
APP_ENV=development
APP_VERSION=0.1.0
PUBLIC_ORIGIN=https://localhost:8443
ADMIN_ORIGIN=https://localhost:8444
API_PUBLIC_PORT=8080
API_ADMIN_PORT=8081
ADMIN_ALLOWED_CIDRS=127.0.0.1/32,::1/128,172.28.0.0/16

# ---------- 数据库集群超级用户（仅容器初始化、DDL 迁移与备份恢复使用）----------
POSTGRES_DB=usapp
POSTGRES_SUPERUSER=postgres
POSTGRES_SUPERUSER_PASSWORD=<FILL_SECURE_SUPERUSER_PASSWORD>

# ---------- API 运行账户（受限账户，仅有 DML 权限）----------
POSTGRES_USER=usapp_api
POSTGRES_PASSWORD=<FILL_SECURE_API_PASSWORD>
POSTGRES_POOL_SIZE=8

# ---------- 受限推送账户（阶段 0 严格隔离，禁止访问业务表）----------
POSTGRES_PUSH_USER=usapp_push
POSTGRES_PUSH_PASSWORD=<FILL_SECURE_PUSH_PASSWORD>

# ---------- 注册策略与白名单 ----------
REGISTRATION_MODE=WHITELIST
ALLOWED_EMAILS=user1@example.com,user2@example.com

# ---------- 安全密钥（必填：32 字节高熵随机串）----------
# 生成方式：openssl rand -hex 32
SESSION_SECRET=<FILL_32_BYTES_HEX_SECRET>

# ---------- 邮件与调度 ----------
MAIL_MODE=log
SCHEDULER_ENABLED=true
SCHEDULER_TICK_INTERVAL_MS=15000
SCHEDULER_INSTANCE_ID=local
SCHEDULER_KEY_NAMESPACE=usapp.scheduler

# ---------- 管理后台初始化账号 ----------
ADMIN_BOOTSTRAP_USERNAME=admin
# 格式：pbkdf2-sha256$<iterations>$<salt-base64>$<hash-base64>
# 生成命令参见本手册第 6 节
ADMIN_BOOTSTRAP_PASSWORD_HASH=<FILL_PBKDF2_PASSWORD_HASH>

# ---------- 推送辅助进程（阶段 0 保持关闭）----------
PUSH_WORKER_ENABLED=false
```

---

## 5. 未执行的检查、已知风险与阻塞

1. **未执行代码编译与链接**：
   - C++ 业务代码与测试套件尚未在目标编译器上实际编译过。
2. **测试用例未运行**：
   - 12 个自动化测试用例（单元测试、静态检查、DB 门控测试）尚未在真实环境中执行。
3. **数据库迁移未实际应用**：
   - `0000..0005` 迁移尚未在真实的 PostgreSQL 17 实例中加载并验证校验和。
4. **前端未构建**：
   - `apps/web/dist` 与 `apps/admin/dist` 尚未生成，若直接启动 Caddy 会导致前端路由 404。
5. **本地 CA 未导入信任**：
   - Caddy 生成的自签根证书 `root.crt` 需负责人手动导入操作系统受信任根证书列表，否则浏览器会显示证书风险提示。
6. **Docker 网段冲突**：
   - Compose 固定子网 `172.28.0.0/16` 若与宿主机现有网络重叠，Docker 会拒绝创建网络，需在 Compose 与 `.env` 中同步更换网段。

---

## 6. 按顺序排列的手动执行手册

### 阶段 A：环境配置与凭据准备

#### Windows (PowerShell)
```powershell
# 工作目录: 仓库根目录 D:\JYJ\USapp
# 执行环境: 宿主机 PowerShell 7+
# 是否写入数据: 是（生成 .env 文件，若已存在则跳过覆盖）

# 1. 安全生成 .env（绝不覆盖已有文件）
if (-not (Test-Path .env)) {
    Copy-Item .env.example .env
    Write-Host "已创建 .env，请使用文本编辑器填写真实配置。"
} else {
    Write-Host ".env 文件已存在，保留现有配置，跳过覆盖。"
}

# 2. 生成管理员密码 PBKDF2 哈希（使用 Node.js，亦可用 Python）
# 请将 YOUR_ADMIN_PASSWORD 替换为您设定的管理员初始密码：
node -e 'const c=require("crypto"),s=c.randomBytes(16),h=c.pbkdf2Sync("YOUR_ADMIN_PASSWORD",s,600000,32,"sha256");console.log(`ADMIN_BOOTSTRAP_PASSWORD_HASH=pbkdf2-sha256$600000$${s.toString("base64")}$${h.toString("base64")}`)'

# 将输出的哈希值填入 .env 中的 ADMIN_BOOTSTRAP_PASSWORD_HASH
```

#### Linux / WSL
```bash
# 工作目录: 仓库根目录
# 执行环境: 宿主机 Bash
# 是否写入数据: 是

[ ! -f .env ] && cp .env.example .env && echo "已创建 .env" || echo ".env 已存在，保留现有配置"

node -e 'const c=require("crypto"),s=c.randomBytes(16),h=c.pbkdf2Sync("YOUR_ADMIN_PASSWORD",s,600000,32,"sha256");console.log(`ADMIN_BOOTSTRAP_PASSWORD_HASH=pbkdf2-sha256$600000$${s.toString("base64")}$${h.toString("base64")}`)'
```

---

### 阶段 B：前端依赖安装与静态产物构建

#### Windows (PowerShell)
```powershell
# 工作目录: 仓库根目录
# 执行环境: 宿主机 Node.js 24 + pnpm
# 是否写入数据: 是（向磁盘写入 node_modules 依赖库与 dist 前端构建产物）

# 1. 构建 Web 前端
Set-Location apps\web
pnpm install
pnpm run build
if ($LASTEXITCODE -ne 0) { throw "Web 端构建失败" }

# 2. 构建管理前端
Set-Location ..\admin
pnpm install
pnpm run build
if ($LASTEXITCODE -ne 0) { throw "管理端构建失败" }

Set-Location ..\..
```

#### Linux / WSL
```bash
# 工作目录: 仓库根目录
# 执行环境: 宿主机 Bash + Node.js 24 + pnpm
# 是否写入数据: 是

cd apps/web && pnpm install && pnpm run build
cd ../admin && pnpm install && pnpm run build
cd ../..
```

**预期结果**：`apps/web/dist/index.html` 与 `apps/admin/dist/index.html` 生成成功。

---

### 阶段 C：数据库启动、角色初始化与 DDL 迁移

#### Windows (PowerShell 5.1 / 7+)
```powershell
# 工作目录: 仓库根目录
# 执行环境: Docker Compose + Windows PowerShell 5.1 (系统自带) 或 PowerShell 7+ (pwsh)
# 是否写入数据: 是（初始化数据库集群、角色并写入表结构及迁移登记）

# 1. 启动 PostgreSQL 容器并等待健康检查就绪
docker compose -f deploy/docker-compose.yml up -d postgres
docker compose -f deploy/docker-compose.yml ps

# 2. 调用正式迁移入口 apply.ps1（单事务原子提交 DDL 与迁移登记，并执行角色赋权）
# Windows PowerShell 5.1（系统内置 powershell）：
powershell -NoProfile -ExecutionPolicy Bypass -File database\apply.ps1 -Database usapp -InitRoles

# 或若已安装 PowerShell 7+（pwsh）：
# pwsh -NoProfile -File database\apply.ps1 -Database usapp -InitRoles

# 3. 校验迁移记录表 schema_migrations 完整性（必须准确存在 6 项完整 BaseName 记录）
docker compose -f deploy/docker-compose.yml exec -T postgres psql -U postgres -d usapp -v ON_ERROR_STOP=1 -c "SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version;"
```

#### Linux / WSL
```bash
# 工作目录: 仓库根目录
# 执行环境: Bash + Docker Compose + PowerShell (pwsh)
# 是否写入数据: 是

docker compose -f deploy/docker-compose.yml up -d postgres

# 调用正式迁移脚本（Docker 模式，单事务原子提交并赋权）
pwsh -NoProfile -File database/apply.ps1 -Database usapp -InitRoles

# 校验迁移记录
docker compose -f deploy/docker-compose.yml exec -T postgres psql -U postgres -d usapp -v ON_ERROR_STOP=1 -c "SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version;"
```

**预期结果**：`schema_migrations` 表精确包含 6 条迁移记录（`0000_schema_migrations`、`0001_common`、`0002_identity`、`0003_couple_binding`、`0004_scheduler`、`0005_admin_audit`），且 `checksum` 与原始 SQL 文件的 SHA-256 完全一致。

---

### 阶段 D：容器化测试运行（推荐主路径，零 Windows 宿主工具链依赖）

#### Windows (PowerShell 5.1 / 7+) / Linux / WSL 通用
```powershell
# 工作目录: 仓库根目录
# 执行环境: Docker Compose + Windows PowerShell 5.1 / 7+
# 是否写入数据: 是（创建测试库 usapp_test 并运行测试）

# 1. 创建专用测试数据库并应用完整迁移与角色赋权（使用统一入口 apply.ps1）
# Windows PowerShell 5.1（系统内置 powershell）：
powershell -NoProfile -ExecutionPolicy Bypass -File database\apply.ps1 -Database usapp_test -CreateDatabase -InitRoles

# 或若已安装 PowerShell 7+（pwsh）：
# pwsh -NoProfile -File database\apply.ps1 -Database usapp_test -CreateDatabase -InitRoles

# 2. 校验测试库迁移记录完整性
docker compose -f deploy/docker-compose.yml exec -T postgres psql -U postgres -d usapp_test -v ON_ERROR_STOP=1 -c "SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version;"

# 3. 启动容器内测试套件（直接连通容器内部 postgres:5432，宿主无需发布 5432 端口）
docker compose -f deploy/docker-compose.yml --profile test run --rm test-runner
```

**预期结果**：
- `unit` 纯单元测试：`PASS` (5/5 通过)。
- `static_check` 静态合规测试：`PASS` (3/3 通过)。
- `db_gated` 数据库门控测试：`PASS` (4/4 通过，确认 6 个完整 BaseName 迁移均已就绪)。
- 总计 12 个测试用例全部通过，退出码为 0。

*(备选路径：若负责人在宿主机具备完整 C++17 + CMake + Drogon 环境并希望在宿主机本地运行测试，需先在 Compose 的 `postgres` 下临时取消发布 `127.0.0.1:5432:5432`，设置 `$env:USAPP_TEST_DSN="postgresql://usapp_api:<密码>@127.0.0.1:5432/usapp_test"` 后在 `services/api` 下执行 `ctest`。)*

---

### 阶段 E：全量服务启动、CA 证书信任与接口点验

#### 1. 服务启动与根证书导出
```powershell
# 工作目录: 仓库根目录
# 执行环境: Docker Compose
# 是否写入数据: 是（记录服务启动日志与心跳）

# 构建后端镜像并启动全部服务
docker compose -f deploy/docker-compose.yml build api
docker compose -f deploy/docker-compose.yml up -d
docker compose -f deploy/docker-compose.yml ps

# 导出 Caddy 本地 CA 根证书
New-Item -ItemType Directory -Force -Path deploy\caddy\certs
docker compose -f deploy/docker-compose.yml cp caddy:/data/caddy/pki/authorities/local/root.crt deploy/caddy/certs/root.crt
```

> **证书信任步骤**：请按照 [deploy/LOCAL-HTTPS.md](file:///d:/JYJ/USapp/deploy/LOCAL-HTTPS.md) 的说明，将导出的 `root.crt` 手动导入当前操作系统的“受信任的根证书颁发机构”。

#### 2. 服务健康检查与安全边界验证
```powershell
# A. 业务公开端点存活检查
curl.exe --cacert deploy/caddy/certs/root.crt https://localhost:8443/api/v1/health
# 预期结果: HTTP 200 {"status":"pass",...}

# B. 管理端点存活检查（通过 8444 回环访问）
curl.exe --cacert deploy/caddy/certs/root.crt https://localhost:8444/api/admin/v1/health
# 预期结果: HTTP 200 {"status":"ok","version":"0.1.0","time":"..."}

# C. 安全边界拦截验证：公网 8443 端口禁止访问管理接口
curl.exe -i --cacert deploy/caddy/certs/root.crt https://localhost:8443/api/admin/v1/health
# 预期结果: HTTP 404 Not Found

# D. 安全边界拦截验证：直接访问宿主 8081 端口缺少入口标记头
curl.exe -i http://127.0.0.1:8081/api/admin/v1/health
# 预期结果: HTTP 403 Forbidden {"error":{"code":"ORIGIN_NOT_ALLOWED","message":"该入口不提供管理接口"}}

# 附带标记头时通过宿主回环排查
curl.exe -H "X-Usapp-Admin-Surface: usapp-admin" http://127.0.0.1:8081/api/admin/v1/health
# 预期结果: HTTP 200 {"status":"ok",...}
```

#### 3. 管理员引导登录与权限验证
```powershell
# A. 未登录状态查询管理会话
curl.exe --cacert deploy/caddy/certs/root.crt https://localhost:8444/api/admin/v1/session
# 预期结果: HTTP 200 {"authenticated":false,"csrf_token":""}

# B. 执行管理员登录（使用阶段 A 设定初始密码）
curl.exe -i -X POST --cacert deploy/caddy/certs/root.crt `
  -H "Content-Type: application/json" `
  -H "Origin: https://localhost:8444" `
  -H "X-Usapp-Admin-Request: 1" `
  -d '{\"username\":\"admin\",\"password\":\"YOUR_ADMIN_PASSWORD\"}' `
  https://localhost:8444/api/admin/v1/sessions
# 预期结果:
# 1. 响应头包含 Set-Cookie: __Host-usapp-admin=...; Path=/; Secure; HttpOnly; SameSite=Strict
# 2. 响应体包含 HTTP 200 {"authenticated":true,"csrf_token":"...","admin":{"id":"...","username":"admin"}}

# C. 携带 Cookie 访问受保护管理接口
curl.exe --cacert deploy/caddy/certs/root.crt `
  --cookie "__Host-usapp-admin=<获取到的会话Token>" `
  https://localhost:8444/api/admin/v1/users
# 预期结果: HTTP 200 {"users":[],"next_cursor":""}
```

---

### 阶段 F：非破坏性备份与恢复验证

```powershell
# 工作目录: 仓库根目录
# 执行环境: Docker Compose
# 是否写入数据: 是（生成备份文件与恢复测试库）

# 1. 在容器内部生成二进制压缩备份（杜绝宿主机终端管道转码破坏二进制内容）
docker compose -f deploy/docker-compose.yml exec -T postgres pg_dump -U postgres -d usapp -F c -f /tmp/usapp_stage0.dump
if ($LASTEXITCODE -ne 0) { throw "备份导出失败" }

# 2. 将备份文件拉取至宿主机 backups 目录持久化
New-Item -ItemType Directory -Force -Path backups
docker compose -f deploy/docker-compose.yml cp postgres:/tmp/usapp_stage0.dump backups/usapp_stage0.dump
if ($LASTEXITCODE -ne 0) { throw "备份拉取失败" }

# 3. 将备份文件复制进容器准备恢复
docker compose -f deploy/docker-compose.yml cp backups/usapp_stage0.dump postgres:/tmp/usapp_restore.dump
if ($LASTEXITCODE -ne 0) { throw "备份推入失败" }

# 4. 创建空测试恢复库并恢复
docker compose -f deploy/docker-compose.yml exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE usapp_recovery OWNER postgres;"
docker compose -f deploy/docker-compose.yml exec -T postgres pg_restore -U postgres -d usapp_recovery -v /tmp/usapp_restore.dump
if ($LASTEXITCODE -ne 0) { throw "数据恢复失败" }

# 5. 校验恢复库中迁移版本完整性（包含全部 6 项 BaseName 记录）
docker compose -f deploy/docker-compose.yml exec -T postgres psql -U postgres -d usapp_recovery -v ON_ERROR_STOP=1 -c "SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version;"
if ($LASTEXITCODE -ne 0) { throw "恢复记录校验失败" }

# 6. 清理容器内临时转储（注意：严格保留宿主机 backups/usapp_stage0.dump 与容器库 usapp_recovery 供人工复核）
docker compose -f deploy/docker-compose.yml exec -T postgres rm -f /tmp/usapp_stage0.dump /tmp/usapp_restore.dump
Write-Host "备份与恢复验证完成。恢复库 usapp_recovery 与备份文件 backups/usapp_stage0.dump 已保留。"
```

**预期结果**：恢复库中包含完整的 `0000_schema_migrations` 至 `0005_admin_audit` 共 6 条迁移记录。
