# 本地开发环境（阶段 0）

> 本文档里的每一条命令都是**给项目负责人手动执行**的说明，不是本次执行过的记录。
> 本次未执行任何命令（没有构建、没有启动服务、没有连接数据库、没有安装依赖、没有下载镜像）。
> 标注「尚未执行」的步骤表示它们在本次写作中完全没有被验证过。

---

## 1. 前置条件

| 组件 | 要求 | 用途 |
| --- | --- | --- |
| Node.js | **24**（`apps/web`、`apps/admin`、`services/push-worker` 的 `package.json` 均声明 `"node": ">=24"`） | 前端 dev server / 构建 / 单测；推送进程 |
| CMake | ≥ 3.20（`services/api/CMakeLists.txt` 声明） | 构建 C++ 后端 |
| C++ 编译器 | 支持 **C++17**（`CMAKE_CXX_STANDARD 17`，且开启了 `-Wall -Wextra -Wpedantic`） | 构建 C++ 后端 |
| Drogon / OpenSSL / libcurl | 由 `find_package(Drogon|OpenSSL|CURL REQUIRED)` 查找，必须已安装 | 后端依赖；CMake 注释声明"依赖全部为只读引用，不做任何联网下载" |
| PostgreSQL | **17**（原生安装或 Docker） | 数据存储；迁移与测试 |
| Caddy | 任意近期版本 | 同源 HTTPS 入口（`deploy/` 归属目录，见第 2 节） |
| psql | 随 PostgreSQL 17 提供，需在 `PATH` 中（`database/apply.ps1` 会显式检查） | 迁移、备份、恢复、锁实验 |

> `database/apply.ps1` 在找不到 `psql` 时会直接失败并提示
> "请把 PostgreSQL 17 的 bin 目录加入 PATH 后重试"。

---

## 2. 为什么本地也必须走同源 HTTPS

阶段 0 不允许用 `http://localhost` 做本地开发，原因有两层：

1. **Service Worker 需要安全上下文。** 浏览器只在 secure context 下注册 Service Worker。
   浏览器虽然把 `localhost` 特殊对待、视为安全上下文，但那只解决"SW 能不能注册"这一件事。
2. **`localhost` 特例会让本地与生产的安全语义发生偏离。**
   - 会话 Cookie 名带 `__Host-` 前缀。该前缀**强制要求** `Secure` + `Path=/` + 不设 `Domain`。
     在真实 `http://` 下浏览器不会接受这种带 `Secure` 的 `__Host-` Cookie，
     于是本地要么放弃 `__Host-`，要么放弃 `Secure`——两者都等于把生产的安全属性在本地降级。
   - 状态修改请求要求 `Origin` **精确等于**预期的 origin（scheme + host + port 字符串完全相等）。
     本地若用 `http://localhost:5173`（Vite 直接serve），这个 Origin 与生产的
     `https://<域名>` 形态完全不同；同源检查在本地就永远测不到真实路径。
   - Caddy 提供的是同源入口：网页与 API 在同一个 `https://localhost:8443` 下。
     这样 Cookie 的作用域、Origin 的形态、SW 的生效范围都与生产一致。

**结论：本地也跑 Caddy + 本地 CA 的 HTTPS，绝不通过关闭安全属性来"让本地跑起来"（见第 6 节）。**

---

## 3. 创建 `.env`

`.env.example` 是环境变量的权威清单（99 行）。负责人手动复制：

```powershell
# 由负责人手动执行
Copy-Item .env.example .env
```

然后逐项填写。`.gitignore` 已忽略 `.env`、`.env.*`（`!.env.example`）以及 `deploy/env/.env`、
`deploy/caddy/certs/`、`secrets/`、`*.pem`、`*.key`、`*.crt`，因此真实密钥不会进版本库。

必须注意的两个字段：

### `SESSION_SECRET` —— 至少 32 字符

`.env.example` 中的占位值是 `change-me-locally-32-bytes-minimum`（33 字符，仅用于说明长度要求）。
该变量用于会话令牌 / CSRF / 邀请码的密钥派生，**必须替换为真实的随机值**：

```powershell
# 由负责人手动执行（生成候选值，自行决定是否使用）
[Convert]::ToBase64String((1..48 | ForEach-Object { Get-Random -Maximum 256 }))
```

> 上面这条命令只是"生成随机串"的示意；本次未执行。生产环境请使用系统级 CSPRNG
> （例如 `openssl rand -base64 48`），不要把随机串贴进聊天记录或提交到仓库。

### `ADMIN_BOOTSTRAP_PASSWORD_HASH` —— 固定格式

管理员口令**不以明文存储**。`admin_users.password_hash` 的格式由迁移
`database/migrations/0005_admin_audit.sql` 固化，`services/api/src/core/Crypto.h` 与
`0005_admin_audit.sql` 的列注释都写明了同一格式：

```
pbkdf2-sha256$<iterations>$<salt-base64>$<hash-base64>
```

即：算法标识 `pbkdf2-sha256`、迭代次数、base64 编码的盐、base64 编码的 32 字节派生密钥，
四段用 `$` 分隔。管理员用户名还需满足 `admin_users_username_shape` 约束：
`^[A-Za-z0-9._-]{3,64}$`。

### 计算 PBKDF2 散列（由负责人执行）

**方式 A：小脚本（推荐，避免 shell 引号问题）**

把下面内容保存为一个临时脚本（例如 `%TEMP%\usapp-pbkdf2.mjs`），然后运行。
`salt` 与派生结果都用 base64，输出即 `.env` 里可以直接粘贴的整串：

```js
// 由负责人手动创建并执行；本仓库不包含此脚本
import { pbkdf2Sync, randomBytes } from 'node:crypto'

const password = process.argv[2]
if (!password) throw new Error('用法: node usapp-pbkdf2.mjs <口令>')

const iterations = 210000
const salt = randomBytes(16)
const derived = pbkdf2Sync(password, salt, iterations, 32, 'sha256')

console.log(
  `pbkdf2-sha256$${iterations}$${salt.toString('base64')}$${derived.toString('base64')}`,
)
```

```powershell
# 由负责人手动执行
node "$env:TEMP\usapp-pbkdf2.mjs" '<管理员口令>'
```

**方式 B：`openssl kdf`**

```powershell
# 由负责人手动执行；<hex-salt> 与 <口令> 自行替换（2 行命令分开跑）
openssl rand -hex 16
openssl kdf -keylen 32 -kdfopt digest:SHA256 -kdfopt pass:'<口令>' -kdfopt hexsalt:<hex-salt> -kdfopt iter:210000 PBKDF2
```

方式 B 的输出是 hex，需要**再转成 base64**（以及把 hex 盐转成 base64）才能拼成
`pbkdf2-sha256$<iterations>$<salt-b64>$<hash-b64>`。两种方式的输出格式与迭代次数
**尚未在本次写作中被验证**——请在写入 `.env` 之前自行核对一次：
用同一个口令与同一个盐重算，结果必须逐字节相同。

---

## 4. 启动顺序（由负责人手动执行，编号即顺序）

> 下列每一步都标注「由负责人手动执行」。本次**一步都没有执行**。

1. **由负责人手动执行 —— 启动 PostgreSQL 17**（原生实例或容器）。
   确保 `.env` 中的 `POSTGRES_HOST/PORT/DB/USER/PASSWORD` 与之匹配。
2. **由负责人手动执行 —— 应用数据库迁移**：

   ```powershell
   $env:PGPASSWORD = '<usapp_api 口令>'
   pwsh -NoProfile -File database/apply.ps1 -Host 127.0.0.1 -Port 5432 -Database usapp -User usapp_api
   ```

   `database/apply.ps1` 的行为（来自脚本自身说明）：按文件名升序读取 `database/migrations/*.sql`；
   跳过已记录且 checksum 一致的版本；checksum 不一致直接失败（禁止事后修改已应用迁移）；
   每个文件用 `psql -v ON_ERROR_STOP=1 -1` 执行；成功后写入 `schema_migrations`。
   先跑 `-DryRun` 可以只看待应用清单。
3. **由负责人手动执行 —— 构建 C++ 后端**：

   ```powershell
   cmake -S services/api -B services/api/build -DCMAKE_BUILD_TYPE=Debug
   cmake --build services/api/build --target usapp_api
   ```

   ⚠️ 已知缺口（见 `docs/architecture.md` 第 6 节）：`services/api/CMakeLists.txt` 第 54～60 行
   逐个引用了 `src/main.cc` 与 `src/controllers/*.cc`（6 个：Health/Session/Auth/Space/Invite/Admin），
   但这些文件在本次阅读的仓库快照中**不存在**；`add_subdirectory(tests)`（第 71 行）所指向的
   `services/api/tests/` 目录同样不存在。其余模块（`src/core`、`config`、`db`、`http`、
   `services`、`mailer`、`scheduler`）的源文件均已存在。
   因此在补齐之前，这一步预期会失败。本次未执行，不声称结果。
4. **由负责人手动执行 —— 运行后端**（加载 `.env` 中的变量后启动 `usapp_api`）。
   public 面默认 `API_PUBLIC_BIND=0.0.0.0`、`API_PUBLIC_PORT=8080`；
   管理面 `API_ADMIN_BIND=127.0.0.1`、`API_ADMIN_PORT=8081`。
5. **由负责人手动执行 —— 启动 Web 客户端 dev server**：

   ```powershell
   cd apps/web
   pnpm install   # 依赖安装由负责人执行
   pnpm dev       # 监听 127.0.0.1:5173（vite.config.ts 中 strictPort: true）
   ```

6. **由负责人手动执行 —— 启动管理界面 dev server**：

   ```powershell
   cd apps/admin
   pnpm install
   pnpm dev       # 监听 127.0.0.1:5174，base 固定 /admin/
   ```

7. **由负责人手动执行 —— 启动 Caddy（同源 HTTPS 入口）**，使
   `https://localhost:8443` 同时提供网页与 API。`deploy/` 目录与 `deploy/Caddyfile` 已存在，
   但该配置**从未被加载验证过**；本地 CA 需要先导出并导入信任库，
   完整步骤与命令见 `deploy/LOCAL-HTTPS.md`（第 3 节导出、第 4 节导入、第 5 节验证）。
   关键路径：导出的根证书落在 `deploy\caddy\certs\root.crt`，该目录已被 `.gitignore` 忽略。
   ⚠️ `ADMIN_ORIGIN=https://localhost:8444` 与 `deploy/Caddyfile` 的单站点拓扑不一致
   （站点只有 8443，管理界面在 `/admin/`）；遇到 origin 相关失败时先确认这一点。本次未执行。
8. **由负责人手动执行（可选）—— 启动推送辅助进程**：

   ```powershell
   cd services/push-worker
   pnpm install
   pnpm run build
   node dist/src/index.js
   ```

   默认 `PUSH_WORKER_ENABLED=false`，打印说明后以 0 退出。非回环 `PUSH_WORKER_HOST` 必须同时设置
   `PUSH_WORKER_PRIVATE_NETWORK=true`；启用时 `PUSH_WORKER_TOKEN` 与三个 VAPID 变量必须齐全，否则以 78 退出。
   说明见 `services/push-worker/README.md`（P2-5c）。

---

## 5. 本地开发常见配置速查

| 变量 | 值 | 备注 |
| --- | --- | --- |
| `APP_ENV` | `development` | `.env.example` 默认值 |
| `PUBLIC_ORIGIN` | `https://localhost:8443` | 状态修改请求的 `Origin` 必须与此**完全相等** |
| `ADMIN_ORIGIN` | `https://localhost:8444` | `.env.example` 中的管理入口 origin |
| `REGISTRATION_MODE` | `WHITELIST` | 阶段 0 只允许这个值 |
| `ALLOWED_EMAILS` | 逗号分隔 | 只管"能否创建新账号" |
| `MAIL_MODE` | `log` | 本地不真实投递邮件，验证码写日志 |
| `SCHEDULER_ENABLED` | `true` | 进程内调度（阶段 0 只有锁基础） |
| `SCHEDULER_KEY_NAMESPACE` | `usapp.scheduler` | 跨实例必须完全一致，改动等同于换锁 |
| `REMINDERS_ENABLED` | `false` | P2-6 R2 应用内提醒生成；开启前数据库必须已应用 0026 |
| `MEMORY_PURGE_ENABLED` | `false` | D6 M1d 到期后不可逆地擦除已删除回忆的文字；没有外部删除日志和恢复演练不得在生产开启，开启前数据库必须已应用 0039 |

---

## 6. 安全属性绝不允许为了"让本地跑起来"而关闭

以下操作在本项目中都是**禁止**的，任何一条都会让本地环境失去与生产一致的安全语义：

- 为了在 `http://` 下拿到 Cookie 而删掉 `Secure`。
- 为了在 HTTP 下让浏览器接受会话 Cookie 而把 `__Host-usapp-session` 改名去掉 `__Host-` 前缀。
- 把 `SameSite=Lax` 放宽为 `None`，或为此设置 `Domain`。
- 放宽 `Origin` 校验（改成前缀匹配、允许 `null`、干脆跳过）。
- 关闭 CSRF 校验，或把 CSRF 令牌写进 `localStorage` / `sessionStorage` / `document.cookie`。
- 把 `ADMIN_ALLOWED_CIDRS` 设成 `0.0.0.0/0`，或把管理监听改成 `0.0.0.0`。

**如果本地因为证书、origin mismatch 或 Cookie 被拒而跑不起来，正确做法是修证书/入口配置，
而不是放宽上面的任何一条。** 典型修法：把本地 Caddy 的根证书导入系统/浏览器信任库，
或使用 Caddy 的 `tls internal` 并在客户端显式指定 `--cacert`（见
`tests/contract/phase0-http-checks.md`）。

---

## 7. 已实现 / 仅骨架 / 尚未实现

### 已实现

- 本文件本身：前置条件清单、`.env` 生成步骤、PBKDF2 散列的计算方式、编号启动顺序、
  配置速查表，以及"禁止为了跑起来而降低安全属性"的负面清单。
- `services/api` 的 `src/core`、`src/config`、`src/db`、`src/http`、`src/services`、
  `src/mailer`、`src/scheduler` 模块的源文件（已存在，本次未构建）。
- `database/apply.ps1` 与 6 个迁移文件；`apps/web`、`apps/admin`、`services/push-worker` 的配置与源文件。

### 仅骨架

- `services/api/src/controllers/**`（6 个控制器）与 `src/main.cc`：**不存在**，
  但 `CMakeLists.txt` 已逐个引用它们，因此按本文件第 4 节第 3 条构建 `usapp_api` 预期会失败。
- `services/api/tests/`：目录不存在，而 `CMakeLists.txt` 有 `add_subdirectory(tests)`，
  同样的原因会让 CMake 配置阶段失败。
- `deploy/`：编排文件（`docker-compose.yml`、`Caddyfile`、两个 Dockerfile、`LOCAL-HTTPS.md`）
  已存在，但**从未运行过**——没有任何一次 `docker compose up`，Caddy 配置未加载，
  本地 CA 未导出/导入。因此第 4 节第 7 条仍是"待负责人执行"。

### 尚未实现

- 本地开发环境的一键脚本（没有 Makefile / Taskfile；`deploy/docker-compose.yml` 是最接近的东西，
  但它把 API/数据库/Caddy 都放进容器，与本节"宿主机跑 dev server"的路径是两条并行的路，
  两条路都**未执行验证**）。
- 自动导入本地 CA 到系统信任库的步骤（依赖操作系统；`deploy/LOCAL-HTTPS.md` 提供手动命令，
  并明确拒绝脚本化该信任变更）。
- 任何自动化健康检查（无脚本探活 `usapp_api`）。

## 8. 尚未执行的检查

1. 未执行 `Copy-Item .env.example .env`，未填写任何真实变量。
2. 未生成任何 `SESSION_SECRET`，未生成任何 PBKDF2 散列，未验证第 3 节的两种计算方式。
3. 未执行 `database/apply.ps1`（含 `-DryRun`）。
4. 未执行任何 `cmake` / 编译命令。
5. 未执行 `pnpm install`、`pnpm dev`、`pnpm build`、`pnpm test`、`pnpm typecheck`（`apps/web`、`apps/admin`、`services/push-worker`）。
6. 未启动 PostgreSQL、Caddy、`usapp_api`、`usapp_push_worker`。
7. 未验证阶段 0 的任何监听端口、证书或 Cookie 行为。
8. 未验证 `SMTP_*` 相关变量与 `services/api/src/config/AppConfig.h` 中的字段名是否一一对应
   （`.env.example` 使用 `SMTP_HOST` / `SMTP_PORT` / `SMTP_USERNAME` / `SMTP_FROM`，
   而 `AppConfig.h` 声明的字段是 `smtpUrl` / `smtpUser` / `mailFrom`，两者看起来不一致；
   该不一致本身也**未经确认**，只是本次静态阅读留下的疑点）。
