# 阶段 0 HTTP 契约手工检查清单

> **全部 22 条用例尚未执行。** 本文件是**给项目负责人手动执行**的清单，
> 不是自动化测试。以下所有"期望"都来自 `contracts/openapi.yaml` 的静态阅读，
> 未经任何实测。执行时请在文末的记录表里填写实际结果。

---

## 0. 契约缺口说明（执行前必读）

1. **契约没有定义任何 `requestBody`。** 对 `contracts/openapi.yaml` 全文检索 `requestBody`
   **零命中**：所有 POST 端点只定义了响应。因此下面用例里的请求体字段名
   （`email`、`code`、`name`）是**实现约定**，不是契约冻结项，必须以服务端实现为准。
   若实现使用了别的字段名，请把该用例改成实现的名字，并把它作为**契约缺口**上报。
2. 同样地，`POST /api/v1/spaces/current/invites` 的请求体在契约中不存在；清单用 `{}`。
3. 契约中的 `EMAIL_CODE_*` 没有定义；时限来自 `.env.example`：
   `EMAIL_CODE_TTL_SECONDS=600`、`EMAIL_CODE_MAX_ATTEMPTS=5`、
   `EMAIL_CODE_RESEND_COOLDOWN_SECONDS=60`、`EMAIL_CODE_HOURLY_LIMIT=8`。
4. `ADMIN_ORIGIN` 在 `.env.example` 中是 `https://localhost:8444`，但 `deploy/Caddyfile` 里
   **只有 `https://localhost:8443` 一个站点**，管理 API 挂在 `/api/admin/*`（转发到容器内 8081）、
   管理界面挂在 `/admin/`。8444 上没有任何东西。这条不一致被 `deploy/README.md` 自身列为待决项，
   **尚未决定以哪边为准**；本清单以 `deploy/Caddyfile` 的实际路由为准，并把它记进结果表。
5. 本地 CA 根证书的落点是 `deploy\caddy\certs\root.crt`（由 `deploy/LOCAL-HTTPS.md` 第 3 节
   用 `docker compose cp` 导出，该目录在 `.gitignore` 中）。**该文件当前尚未生成**，
   因为 Caddy 一次都没启动过。

---

## 1. 约定与变量准备（由负责人手动执行）

在 PowerShell 里设置变量。**必须使用 `curl.exe`**：在 Windows 上 `curl` 是
`Invoke-WebRequest` 的别名，参数语义完全不同。

```powershell
# 由负责人手动执行
$env:USAPP_CA_CERT = 'deploy\caddy\certs\root.crt'  # 由 deploy/LOCAL-HTTPS.md 第 3 节导出；当前尚未生成
$BASE     = 'https://localhost:8443'
$ORIGIN   = 'https://localhost:8443'
$JAR_PRE  = "$env:TEMP\usapp-pre.cookies"
$JAR_A    = "$env:TEMP\usapp-a.cookies"
$JAR_B    = "$env:TEMP\usapp-b.cookies"
$JAR_C    = "$env:TEMP\usapp-c.cookies"
$TMP      = $env:TEMP
```

从响应里取 `csrf_token` 时，用 PowerShell 直接解析（不依赖 `jq`）：

```powershell
# 由负责人手动执行
$boot = Get-Content "$TMP\c02.json" -Raw | ConvertFrom-Json
$CSRF_PRE = $boot.csrf_token
```

通用要求：

- **所有用例都必须带 `--cacert "$env:USAPP_CA_CERT"`，禁止 `-k`。**
- Cookie 一律通过 jar 文件（`-c` 写、`-b` 读），不要手抄。
- 除 `GET` 与用例 18（故意错 Origin）外，一律带 `-H "Origin: $ORIGIN"`。
- 每个响应都要顺带核对 `Cache-Control: no-store, no-cache, must-revalidate`
  （契约 `components/headers/NoStore`；`204` 除外，其定义中仍带该头）。

---

## 2. 用例

### 用例 1 —— 健康探针

前置：无（不需要会话）。契约：`GET /api/v1/health` → `200`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c01.h" -o "$TMP\c01.json" `
  "$BASE/api/v1/health"
```

- 期望状态码：**200**
- 期望响应体：`{"status":"ok","version":"...","time":"..."}`（`status` 固定为 `ok`，契约 `Health`）
- 期望头：`Cache-Control: no-store, no-cache, must-revalidate`
- Set-Cookie 断言：**不应出现** `Set-Cookie`

### 用例 2 —— 会话引导（同时是 CSRF 入口）

前置：无。契约：`GET /api/v1/session` → `200`，声明 `Set-Cookie: SetSessionCookie`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c02.h" -o "$TMP\c02.json" `
  -c $JAR_PRE "$BASE/api/v1/session"
```

- 期望状态码：**200**
- 期望响应体：`authenticated = false`、`csrf_token` 为非空字符串、
  `pre_auth_expires_at` 约为当前时间 + 30 分钟（契约说明：预认证会话 TTL 30 分钟）
- Set-Cookie 断言：存在 `__Host-usapp-session=<不透明值>`，且该行**同时**包含
  `Path=/`、`Secure`、`HttpOnly`、`SameSite=Lax`，且**不包含** `Domain=`
  （契约 `headers.SetSessionCookie` 的示例即为此形态）
- 记下 `$CSRF_PRE`（见第 1 节）。

### 用例 3 —— 发送验证码（白名单内邮箱）

前置：用例 2（预认证会话 + CSRF）。契约：`POST /api/v1/auth/email-codes` → `202`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c03.h" -o "$TMP\c03.json" `
  -b $JAR_PRE -c $JAR_PRE -X POST "$BASE/api/v1/auth/email-codes" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<白名单内的测试邮箱>"}'
```

- 期望状态码：**202**
- 期望响应体：`resend_after_seconds` 与 `expires_in_seconds` 均为整数（契约 `EmailCodeAccepted`）
- 不要在这里断言错误码——白名单**不**在这一步生效（契约说明：白名单在验证码校验阶段才产生 403）

### 用例 4 —— 发送验证码（白名单外邮箱，证明不枚举账号）

前置：同用例 3，但使用一个**不在** `ALLOWED_EMAILS` 中的邮箱，并注意
`EMAIL_CODE_RESEND_COOLDOWN_SECONDS=60`（同一会话/邮箱有冷却，换邮箱可避开）。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c04.h" -o "$TMP\c04.json" `
  -b $JAR_PRE -c $JAR_PRE -X POST "$BASE/api/v1/auth/email-codes" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<不在白名单的测试邮箱>"}'
```

- 期望状态码：**202**（与用例 3 **完全相同**）
- 期望响应体：字段集合与用例 3 一致。不要断言两者字节相等——
  `resend_after_seconds` 可能因冷却计时不同而不同；**关键是没有任何可见差异能区分"白名单内/外"**。
- 这一条就是对"不枚举账号"的验证。

**用例 4b —— 白名单外的新邮箱完成登录必须被拒**（白名单真正的生效点是这里，不是发码）

前置：用例 4（已向一个**从未在本库注册过**的白名单外邮箱发码），并从 API 日志读出该码。
契约：`POST /api/v1/auth/sessions` → `403` + `REGISTRATION_NOT_ALLOWED`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c04b.h" -o "$TMP\c04b.json" `
  -b $JAR_PRE -c $JAR_PRE -X POST "$BASE/api/v1/auth/sessions" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<用例 4 的白名单外邮箱>","code":"<从日志读到的验证码>"}'
```

- 期望状态码：**403**
- 期望错误码：**`REGISTRATION_NOT_ALLOWED`**（契约：未注册且不在白名单 → 403）
- ⚠️ 必须使用**从未注册过**的地址。若该地址已经是既有用户，白名单不生效，
  此时应得到 **200**——"已有用户始终可登录"本身就是另一条断言，请分别记录。
- Set-Cookie 断言：**不应**出现成功的会话 Cookie

### 用例 5 —— 用错误的验证码登录

前置：用例 3（已发送过一次码）。契约：`POST /api/v1/auth/sessions` → `401`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c05.h" -o "$TMP\c05.json" `
  -b $JAR_PRE -c $JAR_PRE -X POST "$BASE/api/v1/auth/sessions" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<用例 3 的邮箱>","code":"000000"}'
```

- 期望状态码：**401**
- 期望错误码：**`CODE_INVALID_OR_EXPIRED`**（契约 `Unauthenticated` 语义为"未认证或会话已失效 / 验证码错误"）
- Set-Cookie 断言：**不应**出现成功的会话 Cookie
- 注意：`EMAIL_CODE_MAX_ATTEMPTS=5`，多次失败会先触发 `429 RATE_LIMITED`

### 用例 6 —— 用过期的验证码登录

前置：取到一个真实验证码后**等它过期**。`EMAIL_CODE_TTL_SECONDS=600`，
即需要等待约 10 分钟；若嫌久，可由负责人临时把该值改小并重启服务（属配置变更，请自行恢复）。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c06.h" -o "$TMP\c06.json" `
  -b $JAR_PRE -c $JAR_PRE -X POST "$BASE/api/v1/auth/sessions" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<用例 3 的邮箱>","code":"<已过期的验证码>"}'
```

- 期望状态码：**401**
- 期望错误码：**`CODE_INVALID_OR_EXPIRED`**（与用例 5 同码：过期与错误故意不可区分）
- Set-Cookie 断言：**不应**出现成功的会话 Cookie

### 用例 7 —— 登录成功（会话与 CSRF 旋转、旧 Cookie 失效）

前置：用例 3（新码）。契约：`POST /api/v1/auth/sessions` → `200` + `Set-Cookie: SetSessionCookie`。

```powershell
# 7a：先记下"旧"的预认证 Cookie 值
Copy-Item $JAR_PRE "$TMP\usapp-pre.old.cookies" -Force

# 7b：登录
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c07.h" -o "$TMP\c07.json" `
  -b $JAR_PRE -c $JAR_A -X POST "$BASE/api/v1/auth/sessions" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_PRE" `
  -d '{"email":"<白名单内的测试邮箱>","code":"<用例 3 收到的验证码>"}'
```

断言：

- 期望状态码：**200**
- 期望响应体：`authenticated = true`、`user.email` 与登录邮箱一致；
  **`csrf_token` 与 `$CSRF_PRE` 不同**（契约：成功时更换会话标识与 CSRF 令牌）
- **Set-Cookie 断言（新 Cookie）**：`__Host-usapp-session=<新值>`，
  `Path=/`、`Secure`、`HttpOnly`、`SameSite=Lax`，无 `Domain=`；
  并且 `<新值>` **不等于** 登录前 jar 中的旧值（用文件对比：
  `$TMP\usapp-pre.old.cookies` 与 `$JAR_A` 中该 Cookie 的值必须不同）
- **旧 Cookie 已被撤销**：

  ```powershell
  curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c07-old.h" -o "$TMP\c07-old.json" `
    -b "$TMP\usapp-pre.old.cookies" "$BASE/api/v1/me"
  ```

  期望：**401**，错误码 `UNAUTHENTICATED`（旧预认证会话已被撤销，不能继续用）

### 用例 8 —— `GET /api/v1/me`

前置：用例 7（正式会话）。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c08.h" -o "$TMP\c08.json" `
  -b $JAR_A "$BASE/api/v1/me"
```

- 期望状态码：**200**
- 期望响应体：`user`（含 `id`/`email`/`display_name`/`created_at`）；
  此刻 `space` 应为 `null` 或缺省（尚未创建空间）
- 期望头：`Cache-Control: no-store, no-cache, must-revalidate`

### 用例 9 —— 创建空间

前置：用例 7。契约：`POST /api/v1/spaces` → `201`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c09.h" -o "$TMP\c09.json" `
  -b $JAR_A -c $JAR_A -X POST "$BASE/api/v1/spaces" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_A" `
  -d '{"name":"<空间名称>"}'
```

- 期望状态码：**201**
- 期望响应体：`Space`，其中 `timezone` **恒为** `Asia/Shanghai`（契约 `const`）、
  `member_count = 1`、`members` 含 1 个 `slot = 1` 的成员
- 记下 `id` 作为 `<SPACE_A_ID>`，并把 `$CSRF_A` 设为用例 7 返回的 `csrf_token`
- 若此时返回 `403 CSRF_TOKEN_INVALID`，先回用例 2 重新引导并更新 `$CSRF_A`

### 用例 10 —— 重复创建空间

前置：用例 9。契约：`POST /api/v1/spaces` → `409`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c10.h" -o "$TMP\c10.json" `
  -b $JAR_A -X POST "$BASE/api/v1/spaces" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_A" `
  -d '{"name":"<另一个名字>"}'
```

- 期望状态码：**409**
- 期望错误码：**`SPACE_ALREADY_EXISTS`**（契约 `Conflict` 的语义之一）
- `details` 若存在，不应泄漏其他用户的信息

### 用例 11 —— 创建邀请

前置：用例 9。契约：`POST /api/v1/spaces/current/invites` → `201`；空间已满时为 `409 SPACE_FULL`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c11.h" -o "$TMP\c11.json" `
  -b $JAR_A -X POST "$BASE/api/v1/spaces/current/invites" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_A" `
  -d '{}'
```

- 期望状态码：**201**
- 期望响应体：契约中的 `CreatedInvite`（含 `code` 与 `invite_url`；请求体字段名见第 0 节说明）
- **为用例 15 做准备：这里连续创建两个邀请，把两个 `code` 分别记为 `<CODE_1>`、`<CODE_2>`**

### 用例 12 —— 列邀请

前置：用例 11。契约：`GET /api/v1/spaces/current/invites` → `200`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c12.h" -o "$TMP\c12.json" `
  -b $JAR_A "$BASE/api/v1/spaces/current/invites"
```

- 期望状态码：**200**
- 期望响应体：`InviteListResponse`；每个 `Invite` 含 `id`、`expires_at`、`created_at`、
  `max_uses`、`used_count`（契约 `Invite` 的 `required` 字段）
- **明文邀请码不应出现在列表里**——契约中 `Invite` 没有 `code` 字段
  （明文码只在创建的 201 响应里出现一次）。请把这一条当作断言核对。

### 用例 13 —— 撤销邀请

前置：用例 12（取一个 `inviteId`）。契约：`DELETE /api/v1/spaces/current/invites/{inviteId}` → `204`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c13.h" -o "$TMP\c13.json" `
  -b $JAR_A -X DELETE "$BASE/api/v1/spaces/current/invites/<inviteId>" `
  -H "Origin: $ORIGIN" -H "X-CSRF-Token: $CSRF_A"
```

- 期望状态码：**204**，响应体为空
- 之后用被撤销的邀请码接受邀请，期望 **409 `INVITE_REVOKED`**
  （契约 `Conflict` 的语义之一；`acceptInvite` 声明 `409`）

### 用例 14 —— 接受邀请（第二个成员加入）

前置：用例 11（`<CODE_1>` 未撤销）。需要用户 B 自己的会话，流程与用例 2、3、7 相同，
但使用 `$JAR_B`。

```powershell
# 14a：B 引导
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -o "$TMP\c14-boot.json" `
  -c $JAR_B "$BASE/api/v1/session"
# 14b：（用 B 的 csrf_token）发送验证码 → 登录，写 $-b $JAR_B -c $JAR_B，得到 $CSRF_B
# 14c：接受邀请
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c14.h" -o "$TMP\c14.json" `
  -b $JAR_B -X POST "$BASE/api/v1/invites/<CODE_1>/accept" `
  -H "Origin: $ORIGIN" -H "X-CSRF-Token: $CSRF_B"
```

- 期望状态码：**200**
- 期望响应体：`SpaceResponse`，`member_count = 2`，
  `members` 的 `slot` 恰好是 **1 和 2**（契约 `SpaceMember.slot` 的 `enum: [1, 2]`）
- 期望：两边的 `space.id` 与用例 9 的 `<SPACE_A_ID>` 相同（B 加入的是同一个空间，不是新建）
- **`acceptInvite` 必须在同一事务内完成校验 + 消费 + 加入**（契约 `description`）；
  这条断言只能通过"失败时不留半成品"来间接核对——建议在用例 15 之后再回查一次邀请列表的 `used_count`

### 用例 15 —— 第三个成员接受邀请（必须 `SPACE_FULL`）

前置：用例 14（空间已有 2 人），并且**邀请是在空间未满时创建的**
（因此要用用例 11 里预先创建的 `<CODE_2>`；空间满后再创建邀请本身就会返回 `409 SPACE_FULL`）。

```powershell
# 15a：用户 C 引导 + 发码 + 登录（同用例 14 的流程，使用 $JAR_C）
# 15b：C 接受第二个邀请
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c15.h" -o "$TMP\c15.json" `
  -b $JAR_C -X POST "$BASE/api/v1/invites/<CODE_2>/accept" `
  -H "Origin: $ORIGIN" -H "X-CSRF-Token: $CSRF_C"
```

- 期望状态码：**409**
- 期望错误码：**`SPACE_FULL`**
- 期望副作用：C **没有**加入任何空间（C 的 `GET /api/v1/me` 里 `space` 仍为空），
  且该邀请**没有被消费**（`used_count` 不增加）——即整体回滚

### 用例 16 —— 非成员访问他人空间（必须是 404，不是 403）

前置：用例 15（C 未加入任何空间）。契约：`GET /api/v1/spaces/{spaceId}` 明确写着
"非本空间成员一律得到 404 `SPACE_NOT_FOUND`（不区分'不存在'与'无权限'）"。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c16.h" -o "$TMP\c16.json" `
  -b $JAR_C "$BASE/api/v1/spaces/<SPACE_A_ID>"
```

- 期望状态码：**404**（**不是 403**）
- 期望错误码：**`SPACE_NOT_FOUND`**
- 附加断言：再用一个**不存在的随机 UUID** 请求同一路径，响应必须与上面**无法区分**
  （同状态码、同错误码）；这正是"故意不区分"的含义

### 用例 17 —— 状态修改请求缺少 `X-CSRF-Token`

前置：用户 A 的正式会话。契约：`403` + `CSRF_TOKEN_INVALID`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c17.h" -o "$TMP\c17.json" `
  -b $JAR_A -X POST "$BASE/api/v1/spaces" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -d '{"name":"<名字>"}'
```

- 期望状态码：**403**
- 期望错误码：**`CSRF_TOKEN_INVALID`**
- 期望：请求被**在到达业务逻辑之前**拒绝（不应产生副作用：空间名不变）
- 对照：`GET /api/v1/me` 不带令牌应仍然成功（安全方法不要求 CSRF），
  对应 `services/api/src/http/HttpUtil.h` 的 `requiresCsrf()` 语义

### 用例 18 —— 状态修改请求带错误的 `Origin`

前置：用户 A 的正式会话 + 正确令牌。契约：`403` + `ORIGIN_NOT_ALLOWED`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c18.h" -o "$TMP\c18.json" `
  -b $JAR_A -X POST "$BASE/api/v1/spaces" `
  -H "Origin: https://attacker.example.invalid" `
  -H "Content-Type: application/json" -H "X-CSRF-Token: $CSRF_A" `
  -d '{"name":"<名字>"}'
```

- 期望状态码：**403**
- 期望错误码：**`ORIGIN_NOT_ALLOWED`**
- 附加：再测一次**缺失** Origin 的请求，同样必须被拒（`originMatches("", expected)` 为 false，
  见 `services/api/src/http/HttpUtil.h` 的安全不变量 2 与 3"fail closed"）

### 用例 19 —— 正确的 `Origin` + 正确的令牌（成功路径）

前置：用户 A 的正式会话。契约：`POST /api/v1/auth/email-codes` → `202`（状态修改，需 CSRF + Origin）。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c19.h" -o "$TMP\c19.json" `
  -b $JAR_A -X POST "$BASE/api/v1/auth/email-codes" `
  -H "Origin: $ORIGIN" -H "Content-Type: application/json" `
  -H "X-CSRF-Token: $CSRF_A" `
  -d '{"email":"<白名单内的测试邮箱>"}'
```

- 期望状态码：**202**
- 附加断言（令牌与会话绑定）：用**另一个用户 B 的令牌**配 A 的 Cookie 发同一请求，
  期望 **403 `CSRF_TOKEN_INVALID`**（契约说明令牌是"与会话绑定"的）：
  `-b $JAR_A -H "X-CSRF-Token: $CSRF_B"`
- 注意：如果刚在用例 3/4 发过码，可能先触发 `429 RATE_LIMITED`
  （`EMAIL_CODE_RESEND_COOLDOWN_SECONDS=60`）；请间隔或换邮箱

### 用例 20 —— 注销

前置：用户 A 的正式会话。契约：`DELETE /api/v1/auth/sessions` → `204` + `Set-Cookie: ClearSessionCookie`。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c20.h" -o "$TMP\c20.json" `
  -b $JAR_A -c $JAR_A -X DELETE "$BASE/api/v1/auth/sessions" `
  -H "Origin: $ORIGIN" -H "X-CSRF-Token: $CSRF_A"
```

- 期望状态码：**204**
- **Set-Cookie 断言**：`__Host-usapp-session=;` 且带 `Max-Age=0`，
  并且属性与设置时**完全一致**（`Path=/`、`Secure`、`HttpOnly`、`SameSite=Lax`，无 `Domain=`）——
  `services/api/src/http/HttpUtil.h` 明确写着"属性与设置时完全一致，否则浏览器不会覆盖旧值"
- 注销后立即复用旧 Cookie：

  ```powershell
  curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c20-after.h" -o "$TMP\c20-after.json" `
    -b $JAR_A "$BASE/api/v1/me"
  ```

  期望：**401**，错误码 `UNAUTHENTICATED`（服务端立即撤销，不依赖 Cookie 过期）

### 用例 21 —— 管理端健康检查（来自回环）

前置：后端与 Caddy 均已启动。契约：`GET /api/admin/v1/health` → `200`；`403` 为来源不被允许。

`deploy/Caddyfile` 第 101～103 行把 `/api/admin/*` 转发到 `api:8081`（管理监听器），
因此经同源入口访问是**配置已确认**的路径；直连监听则取决于后端实际监听方式（本次未启动）。

```powershell
# 21a：经同源入口
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c21.h" -o "$TMP\c21.json" `
  "$BASE/api/admin/v1/health"

# 21b（备选）：直连管理监听；scheme（http/https）与是否需要 --cacert 请以实现为准，本次未确认
# curl.exe -sS -D "$TMP\c21.h" -o "$TMP\c21.json" "http://127.0.0.1:8081/api/admin/v1/health"
```

- 期望状态码：**200**
- 期望响应体：`Health`（`status = ok`）
- 期望头：契约只对管理端点声明 `200` / `403`（无 `404`）；若观察到 `404`，属于**契约偏离**，应记录
- 说明（拓扑不一致，必须记录）：`.env.example` 的 `ADMIN_ORIGIN=https://localhost:8444`，
  而 `deploy/Caddyfile` 里**只有 `https://localhost:8443` 一个站点**，管理界面在 `/admin/`。
  也就是说 8444 上没有任何东西。`deploy/README.md` 自己把这条列为待决项；
  **以哪边为准尚未决定**，本用例只要求记录实际观察到的结果

### 用例 22 —— 管理端健康检查（来自不在 `ADMIN_ALLOWED_CIDRS` 的来源）

前置：`.env` 中 `ADMIN_ALLOWED_CIDRS=127.0.0.1/32,::1/128,172.28.0.0/16`（来自 `.env.example`）。
判定由 `services/api/src/http/HttpUtil.h` 的 `peerIp()` + `ipInAnyCidr()` 实现
（**`peerIp()` 取的是直连地址还是受信代理头，本次未逐行确认**）。

`deploy/Caddyfile` 第 92～99 行的注释把这条边界的机制说得很明确，本用例必须按它来设计：

> Caddy 层**不做**来源 IP 限制，因为对 Caddy 而言所有请求的 TCP 对端都是 caddy 容器本身
> （即落在 `172.28.0.0/16` 内，属于允许集合），`remote_ip` 规则只是摆设；
> 真正的边界在应用层，由 api 以 **TCP 对端地址**再次校验 `ADMIN_ALLOWED_CIDRS`；
> 并且 **api 绝不能把 `X-Forwarded-For` 当作来源地址**，否则该边界可被伪造绕过。

因此两种可行做法：

- **方法 A（不改配置）**：绕开 Caddy，直接向 api 的管理监听发起请求，
  但来源必须是**不属于** `127.0.0.1/32`、`::1/128`、`172.28.0.0/16` 的地址
  （例如从另一台机器、或另一个网段的容器）。走同源入口是**测不出** 403 的。
- **方法 B（改配置，需自行恢复）**：把 `ADMIN_ALLOWED_CIDRS` 临时改为不含请求来源的值，
  重启后端后重发用例 21 的请求。
- **附加断言（防伪造）**：在方法 A/B 的基础上，额外发送一次带
  `-H "X-Forwarded-For: 127.0.0.1"` 的请求，仍然必须被拒——
  若加了该头就变 200，说明来源判定被 `X-Forwarded-For` 污染，这是**安全缺陷**。

```powershell
curl.exe -sS --cacert "$env:USAPP_CA_CERT" -D "$TMP\c22.h" -o "$TMP\c22.json" `
  "$BASE/api/admin/v1/health"
```

- 期望状态码：**403**（契约对管理端点只声明 `200` / `403`）
- 期望错误码：契约 `Forbidden` 的语义为"Origin / CSRF / 白名单 / 管理访问来源校验失败"；
  管理来源失败**没有**单独的 code，请记录实际返回的 `code` 值，
  若为 `404` 或其它值，作为**契约缺口**上报
- 附加断言：该请求**不应**返回任何管理数据；也**不应**泄漏"哪些 CIDR 被允许"

---

## 3. 结果记录表（执行时填写）

| # | 用例 | 期望状态码 | 期望错误码 | 实际状态码 | 实际错误码 | 结论（待填） |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 健康探针 | 200 | — | | | |
| 2 | 会话引导 | 200 | — | | | |
| 3 | 发码（白名单内） | 202 | — | | | |
| 4 | 发码（白名单外） | 202 | — | | | |
| 4b | 白名单外新邮箱登录 | 403 | `REGISTRATION_NOT_ALLOWED` | | | |
| 5 | 错误验证码登录 | 401 | `CODE_INVALID_OR_EXPIRED` | | | |
| 6 | 过期验证码登录 | 401 | `CODE_INVALID_OR_EXPIRED` | | | |
| 7 | 登录成功（旋转 + 旧 Cookie 失效） | 200 | — | | | |
| 8 | `GET /api/v1/me` | 200 | — | | | |
| 9 | 创建空间 | 201 | — | | | |
| 10 | 重复创建空间 | 409 | `SPACE_ALREADY_EXISTS` | | | |
| 11 | 创建邀请 | 201 | — | | | |
| 12 | 列邀请 | 200 | — | | | |
| 13 | 撤销邀请 | 204 | — | | | |
| 14 | 接受邀请（第 2 人） | 200 | — | | | |
| 15 | 第 3 人接受邀请 | 409 | `SPACE_FULL` | | | |
| 16 | 非成员访问他人空间 | 404 | `SPACE_NOT_FOUND` | | | |
| 17 | 缺 `X-CSRF-Token` | 403 | `CSRF_TOKEN_INVALID` | | | |
| 18 | 错误 `Origin` | 403 | `ORIGIN_NOT_ALLOWED` | | | |
| 19 | 正确 Origin + 令牌 | 202 | — | | | |
| 20 | 注销 | 204 | — | | | |
| 21 | 管理端健康（回环） | 200 | — | | | |
| 22 | 管理端健康（非允许 CIDR） | 403 | （契约未单列 code） | | | |

---

## 4. 已实现 / 仅骨架 / 尚未实现

### 已实现

- 本清单本身（22 条用例、每条含可直接粘贴的 `curl.exe` 命令）。

### 仅骨架

- 「结果记录表」为空白模板，**没有**任何已填结果。
- 没有任何脚本把本清单自动化；它是纯人工清单。

### 尚未实现

- 自动化 HTTP 契约测试（无 `newman`、无 `schemathesis`、无 `pytest+requests`，
  也没有任何依赖被加入）。
- 契约中 `requestBody` 的补全（见第 0 节第 1 条）——这是**契约缺口**，不是本清单能修的。
- 管理端来源校验的专用错误码。

## 5. 尚未执行的检查

1. 本文件全部 22 条 `curl` 命令**一条都没有执行**。
2. 未验证 Caddy 是否已启动、`https://localhost:8443` 是否可用。
3. 未确认 `deploy\caddy\certs\root.crt` 是否真的被导出（Caddy 从未启动，该文件当前不存在）。
4. 未确认同源入口能否连通，也未确认直连 8081 的 scheme 与可用性。
5. 未确认任何请求体的字段名是否与实现一致（契约未定义 `requestBody`）。
6. 未确认 `pre_auth_expires_at` 是否恰好为 30 分钟。
7. 未确认用例 12 中邀请列表确实不含明文 `code`。
8. 未确认用例 13 撤销后复用邀请码返回的确实是 `INVITE_REVOKED`。
9. 未确认用例 15 中"邀请未被消费"（`used_count` 不增加）是否真的成立。
10. 未确认用例 19 中"用 B 的令牌 + A 的 Cookie"返回的是 `CSRF_TOKEN_INVALID`。
11. 未确认用例 22 中来源拒绝返回的具体错误码。
