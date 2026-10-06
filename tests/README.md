# 测试策略与覆盖映射（阶段 0）

> **本仓库中的每一处测试都尚未执行。**
> 本次写作没有运行过任何测试、任何构建、任何迁移、任何 `psql`，也没有启动过任何服务。
> 下文出现的"覆盖""验证"是指**该文件在设计与断言层面覆盖了这个测试面**，
> **不是**"已经跑通"。所有测试的执行都**待负责人手动执行**。

---

## 1. 策略

阶段 0 的测试被刻意分成四层，各自只负责自己能证明的东西：

| 层 | 位置 | 能证明什么 | 不能证明什么 |
| --- | --- | --- | --- |
| **数据库约束层** | `database/tests/` | 表结构、唯一性、检查约束、事务级 advisory lock 语义 | HTTP 行为、鉴权顺序、前端行为 |
| **单元层** | `apps/web/tests/unit/`、`apps/admin/tests/unit/`、`services/push-worker/tests/` | 纯函数与 store 状态机、请求管线守卫、缓存判定规则 | 与服务端的真实交互 |
| **服务端层** | `services/api/tests/`（**当前不存在**，归组长所有） | 路由、鉴权管线、CSRF/Origin 校验、事务边界 | 浏览器行为 |
| **端到端 / 契约层** | `tests/contract/`、`tests/e2e/` | 真实部署下的 HTTP 契约与两用户流程 | 自动化回归（阶段 0 有意不引入浏览器自动化） |

**HTTP 契约的唯一事实来源是 `contracts/openapi.yaml`。**
任何端点的路径、状态码、错误码、Cookie 属性都以它为准；测试清单不得发明契约中没有的端点。

---

## 2. 阶段 0 必需测试面 → 覆盖文件映射

| # | 必需测试面 | 覆盖文件 | 现状 |
| --- | --- | --- | --- |
| 1 | 登录与会话 | `apps/web/tests/unit/session-store.spec.ts`（bootstrap / logout / fail-closed）、`database/tests/constraints_test.sql`（C 段 `sessions` 认证一致性）、`tests/contract/phase0-http-checks.md` 用例 2、5–8、20 | 前端与 DB 有文件；**服务端实现测试缺失** |
| 2 | 注册白名单 | `tests/contract/phase0-http-checks.md` 用例 3–4（白名单内/外**同为 202**，证明不枚举账号）、用例 4b（**从未注册过**的白名单外邮箱完成登录 → 403 `REGISTRATION_NOT_ALLOWED`） | 仅契约清单；**服务端实现测试缺失** |
| 3 | 邀请/加入与成员数约束 | `database/tests/constraints_test.sql`（B 段 `couple_members` 9 条、E 段 `space_invites` 2 条）、`tests/contract/phase0-http-checks.md` 用例 11–15 | DB 与契约有；**无自动化端到端** |
| 4 | 同空间授权 | `tests/contract/phase0-http-checks.md` 用例 16（非成员访问他人空间必须是 **404 `SPACE_NOT_FOUND`**，不是 403） | 仅契约清单；**服务端实现测试缺失** |
| 5 | CSRF 拒绝 | `apps/web/tests/unit/api-client.spec.ts`（CSRF/离线守卫）、`apps/admin/tests/unit/admin-client.spec.ts`（写请求必带 `X-CSRF-Token`）、`tests/contract/phase0-http-checks.md` 用例 17–19 | 前端与契约有；**服务端实现测试缺失** |
| 6 | 调度锁竞争与回滚释放 | `database/tests/advisory_lock_test.ps1`（5 个用例，含 `COMMIT` 释放与 `ROLLBACK` 释放）、`docs/scheduler.md` 第 7 节（手写两连接实验） | DB 脚本存在；`Scheduler.cc` 层**无测试文件** |
| 7 | 静态缓存边界 | `apps/web/tests/unit/sw-cache-policy.spec.ts`（`isSensitivePath` / `isCacheableRequest` 共 11 个用例）、`docs/pwa-cache-policy.md` 第 5 节、`tests/e2e/README.md` 第 7 步（注销后按后退键不得露出业务数据） | 单测存在；**未做浏览器实测** |
| 8 | 管理端认证与独立边界 | `apps/admin/tests/unit/admin-session-store.spec.ts`、`apps/admin/tests/unit/admin-client.spec.ts`、`database/tests/constraints_test.sql`（F 段：`admin_users` 与情侣模型零耦合、无外键）、`tests/contract/phase0-http-checks.md` 用例 21–22（回环 200 / 非允许来源必须被拒） | 存在；**服务端实现测试缺失** |
| 9 | 离线只读边界 | `apps/web/tests/unit/api-client.spec.ts`（离线时 POST/DELETE 被拦下且不发 `fetch`，GET 放行） | 单测存在 |

---

## 3. 具体产物清单

### 3.1 数据库层

- **`database/tests/constraints_test.sql`** —— 整个文件包在 `BEGIN ... ROLLBACK` 中，
  测试数据全部丢弃；每个"应当失败"的插入都包在 `BEGIN...EXCEPTION` 里，
  若未抛异常就主动 `RAISE EXCEPTION` 让 `psql` 以非零码退出；
  全部通过时打印 `ALL CONSTRAINT ASSERTIONS PASSED`。覆盖段落：
  - **A. 邮箱规范化**：A1 `email_canonical` 未小写被拒；A2 重复 `email_canonical` 被 `UNIQUE` 拒。
  - **B. `couple_members`**：B1/B2 `slot=3` / `slot=0` 被 `CHECK` 拒；B3 `slot IS NULL`、
    B4 `user_id IS NULL`、B5 `couple_id IS NULL` 被 `NOT NULL` 拒；
    B6 同一 `user_id` 加入第二个 `couple` 被 `UNIQUE(user_id)` 拒；
    B7 同一 `(couple_id, slot)` 被两人占用被 `UNIQUE` 拒；B8/B9 slot 1 / slot 2 已占用仍可插入。
  - **C. `sessions` 认证一致性**：C1 正式会话缺 `user_id` 被 `CHECK` 拒；
    C2 预认证会话带 `user_id` 被 `CHECK` 拒；C3 重复 `token_hash` 被 `UNIQUE` 拒；
    C4 `expires_at` 早于 `created_at` 被 `CHECK` 拒。
  - **D. `email_verification_codes`**：D1 同一预认证会话存在两张未消费验证码；
    D2 `attempts > max_attempts` 被 `CHECK` 拒。
  - **E. `space_invites`**：E1 `used_count > max_uses` 被 `CHECK` 拒；
    E2 `consumed_at` 有值但 `consumed_by_user_id` 为空被 `CHECK` 拒。
  - **F. 边界**：F1 `admin_users` 出现与情侣模型耦合的列；F2 `admin_users` 存在外键；
    F3 `couples.timezone` 列缺失；F4 `couples.max_members` 不应存在（容量由 slot 取值域定义）。
  - 运行方式（**由负责人手动执行**）：`psql -h 127.0.0.1 -U usapp_api -d usapp -v ON_ERROR_STOP=1 -f database/tests/constraints_test.sql`

- **`database/tests/advisory_lock_test.ps1`** —— 用文件重定向运行两个 `psql` 进程（不依赖管道）。
  用例 1 需要真正的并发：持锁期间并发获取必须立即返回 `false`（不排队）；
  用例 2 `COMMIT` 后锁自动释放；用例 3 `ROLLBACK` 后锁自动释放；
  用例 4 不同 key 互不阻塞；用例 5 同一事务内重复获取同 key 幂等。
  运行方式（**由负责人手动执行**）：
  `pwsh -NoProfile -File database/tests/advisory_lock_test.ps1 -Host 127.0.0.1 -Port 5432 -Database usapp -User usapp_api`

### 3.2 前端层

- `apps/web/tests/unit/sw-cache-policy.spec.ts` —— 直接复用 `src/pwa/cache-policy.ts`，
  覆盖 `/api` 及子路径、私有媒体前缀、非 GET、跨源、静态外壳、
  `destination` 缺失时的扩展名兜底，以及"外壳缓存名固定为 `usapp-shell-v1`"。
- `apps/web/tests/unit/api-client.spec.ts` —— 离线守卫（POST/DELETE 拦截且不发请求、GET 放行）、
  CSRF 守卫（缺令牌抛 `CSRF_TOKEN_INVALID`、有令牌时设置请求头与 same-origin 凭据）、
  错误信封解析。
- `apps/web/tests/unit/session-store.spec.ts` —— bootstrap 三态、注销清空业务态、
  服务端注销失败时本地照样清空但不谎报成功。
- `apps/admin/tests/unit/admin-session-store.spec.ts` —— 管理员身份引导、登录/注销、
  "不写入任何 Web Storage"。
- `apps/admin/tests/unit/admin-client.spec.ts` —— URL 拼接、错误信封、写请求必带 `X-CSRF-Token`、
  `DELETE` 同样需要令牌、`204` 返回 `undefined`、列表端点参数名与契约一致。

> 前端测试的命令（**由负责人手动执行**）：在 `apps/web` 或 `apps/admin` 下运行其 `package.json`
> 中定义的脚本。**本次未执行、未核对实际脚本名**，请以各自 `package.json` 为准。

### 3.3 推送辅助进程

- `services/push-worker/tests/policy.test.ts`、`config.test.ts`、`queue.test.ts`
  （文件存在；**未执行**）。

### 3.4 服务端层

- **`services/api/tests/` 目录当前不存在。** 阶段 0 该目录由**组长**所有，
  并且在 `services/api/CMakeLists.txt` 中被 `add_subdirectory(tests)` 引用。
  因此下面的测试面目前**没有任何服务端测试文件**覆盖，只能靠 `tests/contract/` 的手动清单：
  路由与状态码、Origin/CIDR 校验顺序、会话装载、CSRF 校验、邀请接受事务、
  调度 tick 的事务边界。
- 已知相关的源码文件（用于将来补测试时定位）：
  `services/api/src/http/HttpUtil.h`、`services/api/src/core/Errors.h`、
  `services/api/src/services/SessionService.h`、`services/api/src/services/AuthService.h`、
  `services/api/src/services/RegistrationPolicy.h`、`services/api/src/scheduler/Scheduler.h`。

### 3.5 契约与端到端

- `contracts/openapi.yaml` —— 所有 HTTP 断言的事实来源。
- `tests/contract/README.md` + `tests/contract/phase0-http-checks.md` —— 手动 `curl` 契约清单。
- `tests/e2e/README.md` —— 手动浏览器两用户流程。

---

## 4. 运行方式（全部**待负责人手动执行**）

| 对象 | 命令（示意，未执行） |
| --- | --- |
| DB 约束 | `psql ... -v ON_ERROR_STOP=1 -f database/tests/constraints_test.sql` |
| DB 锁语义 | `pwsh -NoProfile -File database/tests/advisory_lock_test.ps1 ...` |
| 前端单测 | 在 `apps/web` / `apps/admin` 下执行其 `package.json` 中的测试脚本 |
| 推送单测 | 在 `services/push-worker` 下执行其 `package.json` 中的测试脚本 |
| HTTP 契约 | 见 `tests/contract/phase0-http-checks.md`（逐条 `curl`） |
| 端到端 | 见 `tests/e2e/README.md`（纯手工） |

阶段 0 **不引入**任何浏览器自动化依赖（Playwright / Cypress / Puppeteer 均不在依赖中），
因此 `tests/e2e/` 是**脚本文档**，不是可执行的测试运行器。

---

## 5. 已实现 / 仅骨架 / 尚未实现

### 已实现

- `database/tests/constraints_test.sql`：6 个段落、24 条断言（A1–A2、B1–B9、C1–C4、D1–D2、E1–E2、F1–F4）。
- `database/tests/advisory_lock_test.ps1`：5 个锁语义用例。
- `apps/web/tests/unit/` 下 3 个 spec、`apps/admin/tests/unit/` 下 2 个 spec、
  `services/push-worker/tests/` 下 3 个 spec。
- `tests/README.md`、`tests/contract/README.md`、`tests/contract/phase0-http-checks.md`、`tests/e2e/README.md`（本次创建）。

### 仅骨架

- `services/api/tests/` **不存在**，但 `CMakeLists.txt` 已经引用它 → 一旦配置构建就会因缺少子目录而失败。
- `tests/contract/` 与 `tests/e2e/` 没有任何自动化运行器，只有人工步骤。
- 没有 CI 配置：没有任何流水线会执行上述任何测试。

### 尚未实现

- 服务端单元/集成测试（鉴权管线、事务边界、错误码映射）。
- 前端组件测试（`views/*`、`InstallPrompt.vue`、`OfflineBanner.vue` 均无测试）。
- 浏览器自动化端到端测试（阶段 0 有意不引入）。
- 覆盖率统计与阈值。
- 任何形式的负载/并发压测。

---

## 6. 尚未执行的检查（清单）

1. 未执行 `database/tests/constraints_test.sql`（24 条断言全部未跑）。
2. 未执行 `database/tests/advisory_lock_test.ps1`（5 个用例全部未跑，包括 `COMMIT`/`ROLLBACK` 释放）。
3. 未执行 `apps/web` 的任何测试（3 个 spec 未跑）。
4. 未执行 `apps/admin` 的任何测试（2 个 spec 未跑）。
5. 未执行 `services/push-worker` 的任何测试（3 个 spec 未跑）。
6. 未执行 `tests/contract/phase0-http-checks.md` 中的任何一条 `curl`。
7. 未执行 `tests/e2e/README.md` 中的任何一步浏览器操作。
8. 未执行 `database/apply.ps1`，因此上述所有数据库测试**依赖的表结构在本地可能根本还不存在**。
9. 未验证各 `package.json` 中测试脚本的名称与参数（本文档未假设具体脚本名）。
10. 未验证 `services/api` 能否成功配置构建（`add_subdirectory(tests)` 引用了不存在的目录）。
11. 未验证上述 spec 文件在语法上能否被各自的测试运行器加载。
