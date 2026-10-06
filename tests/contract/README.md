# tests/contract —— HTTP 契约手工检查

> **本目录中的任何检查都尚未执行。** 这里只有给项目负责人手动执行的清单，
> 不是自动化测试，也不存在任何运行器。

## 内容

| 文件 | 作用 |
| --- | --- |
| `phase0-http-checks.md` | 阶段 0 的 HTTP 契约手工清单：22 条编号用例（含 4b，共 23 次执行），每条给出可直接粘贴的 `curl`、期望状态码、期望错误码与 `Set-Cookie` 断言 |

## 事实来源

- **`contracts/openapi.yaml`** 是唯一权威：路径、状态码、错误码枚举、Cookie 属性、
  `Cache-Control` 头都从它读出来。清单中**没有**任何契约之外的端点。
- 错误码取值来自契约 `components/schemas/ErrorEnvelope` 的 `code` 枚举：
  `VALIDATION_ERROR`、`UNAUTHENTICATED`、`CSRF_TOKEN_INVALID`、`ORIGIN_NOT_ALLOWED`、
  `RATE_LIMITED`、`CODE_INVALID_OR_EXPIRED`、`REGISTRATION_NOT_ALLOWED`、`SPACE_NOT_FOUND`、
  `SPACE_ALREADY_EXISTS`、`SPACE_FULL`、`INVITE_INVALID_OR_EXPIRED`、`INVITE_REVOKED`、
  `ALREADY_IN_SPACE`、`NOT_FOUND`、`INTERNAL_ERROR`。

## 前提（全部由负责人自行准备）

1. 后端已构建并启动（`usapp_api`），数据库迁移已应用。
2. Caddy 已启动，`https://localhost:8443` 同时提供网页与 `/api/**`。
3. Caddy 已按 `deploy/docker-compose.yml` 启动，`https://localhost:8443` 同时提供网页与 `/api/**`
   和管理路径 `/api/admin/**`、`/admin/`（路由见 `deploy/Caddyfile`）。
   本地 CA 根证书按 `deploy/LOCAL-HTTPS.md` 第 3 节导出到 `deploy\caddy\certs\root.crt`
   （该目录在 `.gitignore` 中）；**当前该文件尚未生成**，因为 Caddy 一次都没启动过。
4. 有一对可用的测试邮箱，其中一个在 `ALLOWED_EMAILS` 里、另一个不在。
5. 能读取 `MAIL_MODE=log` 输出的验证码（本清单假设 `MAIL_MODE=log`；
   若改成真实投递，请从收件箱取码）。

## 硬性约定

- **一律使用 `--cacert "$env:USAPP_CA_CERT"`，禁止使用 `-k` / `--insecure`。**
  用 `-k` 等于跳过证书校验，会同时掩盖"证书配置错误"这一最常见的本地故障，
  与本项目"绝不为了跑起来而降低安全属性"的要求冲突。
- Cookie 一律用 **cookie jar 文件**管理（`-c` / `-b`），不要手抄 Cookie。
  jar 文件放在临时目录，不要提交进仓库。
- 每条用例都必须显式带上 `-H "Origin: https://localhost:8443"`
  （除只读的 `GET` 用例以及**故意**测错 Origin 的用例 18 之外）。
- 请求体字段名见 `phase0-http-checks.md` 顶部的**契约缺口说明**。

## 已实现 / 仅骨架 / 尚未实现

### 已实现

- 本文件与 `phase0-http-checks.md`：全部用例（22 条编号 + 4b）的正文、命令与预期断言均已写完。

### 仅骨架

- `phase0-http-checks.md` 第 3 节的「结果记录表」是空表，没有任何已填结果。

### 尚未实现

- 任何自动化的契约校验（没有 `newman`、`schemathesis`、`curl` 包装脚本或 npm 脚本）。
- `contracts/openapi.yaml` 中 `requestBody` 的补全：契约全文没有定义任何请求体，
  这是**契约缺口**，不是本目录能修的；清单中的请求体字段名因此只是实现约定。

## 尚未执行的检查

1. 本目录下的**所有**检查（22 条用例）尚未执行。
2. 未确认 `deploy\caddy\certs\root.crt` 是否真的被导出（Caddy 从未启动）。
3. 未确认后端口与管理端监听的启动方式、scheme 与路径前缀。
4. 未确认任何请求体字段名与实现一致。
5. 未确认请求头 `Cache-Control: no-store, no-cache, must-revalidate` 在实际响应中确实存在。
