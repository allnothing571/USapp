# services/api/tests —— 后端测试套件

> **状态说明**：本目录下测试文件均已静态落盘，**尚未执行编译与测试运行**。
> 测试必须由负责人在具有 C++17 编译器与 Drogon 依赖的环境下手动执行。

---

## 1. 测试套件构成

测试套件由 3 个独立可执行目标组成，共计 12 个测试用例：

| 目标名称 | 源码文件 | 包含用例数 | 外部依赖 | 说明 |
| --- | --- | --- | --- | --- |
| `usapp_test_unit` | `UnitTest.cc` | 5 | 无（仅 `usapp_core`） | 纯单元测试：安全基线、注册白名单、Origin/CSRF 规则、CIDR 范围、游标编码、锁键确定性 Hash |
| `usapp_test_db_gated` | `DbGatedTest.cc` | 4 | PostgreSQL 17 测试库 | 数据库门控测试：会话生命周期与 Fixation 防护、空间成员槽位约束、邀请单次原子消费、Advisory Lock 回滚释放 |
| `usapp_test_static` | `StaticCheckTest.cc` | 3 | 无（纯文件检查） | 静态合规断言：OpenAPI 契约一致性、数据库迁移 DDL 不变量、Drogon 控制器路径参数类型安全绑定与 Admin 守卫 |

框架支撑：`TestSupport.h`（轻量级零外部依赖断言库，支持 `CHECK`、`CHECK_EQ`、`CHECK_CONTAINS`、`finish()` 及门控退出码 `skip()`）。

---

## 2. 数据库门控（USAPP_TEST_DSN）机制

为了保障持续集成与无数据库开发环境下的基本验证，`usapp_test_db_gated` 实现了严格的门控保护：

1. **未配置门控**：
   - 当环境变量 `USAPP_TEST_DSN` 为空或未设置时，程序在控制台打印跳过说明，并以**独立退出码 77** 退出。
   - `CMakeLists.txt` 中已配置 `set_tests_properties(db_gated PROPERTIES SKIP_RETURN_CODE 77)`。CTest 将其报告为 `***Skipped`，**绝不返回 0 误报为测试通过**。
2. **防误触生产库**：
   - 若 `USAPP_TEST_DSN` 包含 `prod` 关键字且不含 `test`，程序判定为疑似生产环境，直接以状态码 1 失败退出。
3. **已配置门控但不可用**：
   - 若提供了 DSN，但无法建立连接、`SELECT 1` 失败、或 `schema_migrations` 中缺失阶段 0 迁移版本（精确按完整 BaseName 校验：`0000_schema_migrations`、`0001_common`、`0002_identity`、`0003_couple_binding`、`0004_scheduler`、`0005_admin_audit`），程序明确打印缺失项并以**退出码 1 失败退出**。**绝不伪装为跳过**。
4. **测试数据隔离与清理**：
   - 测试用例使用 `dbtest_` 前缀的测试账号与隔离 UUID，在用例完成后立即执行 `DELETE` 或利用数据库事务回滚清理。
   - 测试只允许连接专用的本地测试库（如 `usapp_test`），**严禁直接对生产数据库或主开发数据库执行测试**。

---

## 3. 手动构建与运行命令（由负责人执行）

### A. 容器内运行（推荐方式，免除 Windows 宿主 C++ / Drogon 工具链依赖）

```powershell
# 1. 确保 PostgreSQL 容器已启动，并通过 apply.ps1 准备好 usapp_test 测试库与角色权限：
# Windows PowerShell 5.1:
powershell -NoProfile -ExecutionPolicy Bypass -File database\apply.ps1 -Database usapp_test -CreateDatabase -InitRoles
# 或 PowerShell 7+:
# pwsh -NoProfile -File database\apply.ps1 -Database usapp_test -CreateDatabase -InitRoles

# 2. 直接启动测试运行器容器（自动在容器内编译并运行全部 12 个测试）
docker compose -f deploy/docker-compose.yml --profile test run --rm test-runner
```

### B. 宿主机 Windows (PowerShell)

> **注意**：宿主机直连数据库需要临时在 `deploy/docker-compose.yml` 的 `postgres` 服务中取消发布 `127.0.0.1:5432:5432` 的注释。

```powershell
# 1. 切换到 services/api 目录
Set-Location services\api

# 2. 生成构建工程（启用测试构建）
cmake -B build -S . -DUSAPP_BUILD_TESTS=ON

# 3. 编译全部测试目标
cmake --build build --config RelWithDebInfo

# 4. 运行基础测试（无数据库环境，db_gated 将被 Skipped）
ctest --test-dir build --output-on-failure

# 5. 运行完整测试（提供测试库 DSN）
$env:USAPP_TEST_DSN = "postgresql://usapp_api:change-me-locally@127.0.0.1:5432/usapp_test"
ctest --test-dir build --output-on-failure

# 6. 单独执行各测试可执行程序
.\build\tests\RelWithDebInfo\usapp_test_unit.exe
.\build\tests\RelWithDebInfo\usapp_test_static.exe
.\build\tests\RelWithDebInfo\usapp_test_db_gated.exe
```

### C. 宿主机 Linux / WSL

```bash
# 1. 切换目录
cd services/api

# 2. 生成构建工程
cmake -B build -S . -DUSAPP_BUILD_TESTS=ON

# 3. 编译
cmake --build build -j$(nproc)

# 4. 运行基础测试
ctest --test-dir build --output-on-failure

# 5. 运行完整数据库测试
USAPP_TEST_DSN="postgresql://usapp_api:change-me-locally@127.0.0.1:5432/usapp_test" ctest --test-dir build --output-on-failure

# 6. 单独执行各测试程序
./build/tests/usapp_test_unit
./build/tests/usapp_test_static
./build/tests/usapp_test_db_gated
```

---

## 4. 自动化测试 vs 手动验证边界

以下场景已由自动化测试（单元、静态、DB）覆盖：
- 注册白名单模式与邮箱大小写规范化校验
- Origin 协议/端口精确比对、CSRF 方法判断（GET/HEAD 免检，POST/PUT/DELETE 强制）
- CIDR 网段与 IPv4/IPv6 回环匹配
- OpenAPI 路由路径、安全响应头与参数名
- 数据库 0000..0005 迁移关键约束
- 控制器参数从 Drogon 路由参数正确绑定

**仍需负责人进行人工验证的场景**：
1. **浏览器 Sandbox Cookie 行为**：`__Host-` 前缀 Cookie 要求 HTTPS 且不可设置 Domain，需真实浏览器（Chrome / Firefox / Safari）验证。
2. **本地证书信任与 TLS 握手**：Caddy 自签本地 CA 导入操作系统根证书库后的信任状态。
3. **PWA 功能**：Service Worker 离线缓存、Manifest 安装弹窗。
4. **反向代理端口隔离**：公网访问 8443 时，确认无法访问 `/admin/` 与 `/api/admin/*`；宿主通过 127.0.0.1:8444 访问时确认能正常加载管理面。
