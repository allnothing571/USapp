# 本地 HTTPS 证书与手动信任（阶段 0）

> **状态：本文所有命令都尚未执行。** 全部步骤由项目负责人手动、逐条执行并自行确认结果。
> 本文档不包含任何脚本化的信任库改动，也不包含任何证书内容。

## 1. 为什么要做这一步

阶段 0 有一条不能放宽的约束：**不使用关闭安全属性的方式适配本地开发**。
也就是说本地也必须走真实 HTTPS，而不是退化成 http 或让 Cookie 去掉 `Secure`。

因此：

- 会话 Cookie 名是 `__Host-usapp-session`，带 `Secure` 前缀要求，**只有在 HTTPS 下才能生效**；
- 管理界面 Cookie 是 `__Host-usapp-admin`，同理；
- Service Worker（PWA 的核心）在不受信任的证书下**拒绝注册**，
  没有它就没有离线与安装能力。

Caddy 使用自签名性质的本地 CA（配置见 [Caddyfile](Caddyfile) 的 `tls internal`）。
这个 CA **不会被任何操作系统自动信任**，必须由负责人手动、明确地导入一次。

> ⚠️ **警告**：导入根 CA 是**机器级/用户级的信任变更**，影响这台电脑上所有程序的证书校验结果。
> 请只在你自己的开发机上做，并且**只导入你自己这份部署生成的 CA 证书**（不要从别处复制 `root.crt`）。
> 不需要它时，请按第 6 节把它删干净。

## 2. CA 在哪里生成的

- Caddy 第一次启动时生成两样东西：本地 CA（根证书 + 私钥）与
  `localhost` 的站点证书（由该 CA 签发，短有效期、Caddy 会自动续期）。
- 容器内的位置：

  | 内容 | 容器内路径 |
  | --- | --- |
  | 本地 CA 根证书 | `/data/caddy/pki/authorities/local/root.crt` |
  | 本地 CA 私钥（**绝不要复制到仓库**） | `/data/caddy/pki/authorities/local/root.key` |
  | 签发记录与站点证书 | `/data/caddy/pki/authorities/local/`、`/data/caddy/certificates/local/localhost/` |

- `/data` 在 [docker-compose.yml](docker-compose.yml) 中挂在命名卷 `usapp_caddy_data` 上，
  **这个卷必须保留**：删掉它等于换掉 CA，之前导入系统信任库的证书立刻失效。
- 在宿主机上，该卷的物理位置由 Docker 决定（Windows + Docker Desktop 位于 WSL2 内的
  `docker-desktop-data` 发行版里，**不能**当成普通 Windows 路径直接访问）。
  正确做法是用 `docker cp` / `docker compose cp` 把根证书复制出来。

## 3. 把根证书导出到宿主机

以下命令在仓库根目录 `D:\JYJ\USapp` 执行，**待负责人手动执行**：

```powershell
# 1) 启动过一次 caddy 之后，CA 才存在
docker compose -f deploy/docker-compose.yml up -d caddy

# 2) 建一个本地目录放证书（该目录已在 .gitignore 中忽略，证书永远不会进仓库）
New-Item -ItemType Directory -Force deploy\caddy\certs

# 3) 从容器里把根证书复制出来
docker compose -f deploy/docker-compose.yml cp `
  caddy:/data/caddy/pki/authorities/local/root.crt deploy\caddy\certs\root.crt

# 4) 确认文件存在且非空（文件是 PEM 文本）
Get-ChildItem deploy\caddy\certs\root.crt
```

> 不要把 `root.key` 复制到仓库；`.gitignore` 已忽略 `*.key` 与 `deploy/caddy/certs/`，
> 但真正的防线是"不要去复制它"。

## 4. 手动导入系统信任库（⚠️ 请确认后再执行）

### Windows（Windows 10/11）

**作用范围是本用户的根证书库（`-user Root`），不是整机。**
以**普通用户**权限打开 PowerShell 即可，**不需要**管理员：

```powershell
certutil -addstore -user Root deploy\caddy\certs\root.crt
```

导入后浏览器需**完全重启**（Chrome/Edge 会缓存信任状态）。

> ⚠️ **警告**：这是手动、持久的信任变更。执行前请确认：
> 1. 这份 `root.crt` 确实来自**你自己**的 `usapp_caddy_data` 卷；
> 2. 你接受本机所有程序从此信任该 CA 签发的证书。

### macOS

```bash
# 只对当前用户生效（推荐）；要全机生效需加 -k /Library/Keychains/System.keychain 并 sudo
security add-trusted-cert -d -r trustRoot -k ~/Library/Keychains/login.keychain-db deploy/caddy/certs/root.crt
```

### Linux（Debian / Ubuntu 系）

```bash
sudo cp deploy/caddy/certs/root.crt /usr/local/share/ca-certificates/usapp-caddy-local-ca.crt
sudo update-ca-certificates
```

### Linux（RHEL / Fedora 系）

```bash
sudo cp deploy/caddy/certs/root.crt /etc/pki/ca-trust/source/anchors/usapp-caddy-local-ca.crt
sudo update-ca-trust
```

> Firefox 使用自己的证书库（NSS），**不读取系统库**。若要用 Firefox 验证，
> 需要在「设置 → 隐私与安全 → 证书 → 查看证书 → 证书颁发机构 → 导入」中单独导入该根证书。

## 5. 验证

### 5.1 用 curl 验证（不依赖系统信任库）

```powershell
curl.exe --cacert deploy\caddy\certs\root.crt https://localhost:8443/api/v1/health
```

- 返回 200 与健康检查内容 = 证书链、域名与端口都对；
- 报 `unable to get local issuer certificate` / `certificate verify failed` =
  导出的 `root.crt` 与 Caddy 当前使用的 CA 不是同一个（多半是 `caddy_data` 卷被删过）。

也可以显式指定访问的地址，避免代理干扰：

```powershell
curl.exe --noproxy "*" --cacert deploy\caddy\certs\root.crt https://localhost:8443/api/v1/health
```

### 5.2 用浏览器验证

打开 `https://localhost:8443`，应满足：

1. 地址栏是锁形图标，**没有**证书警告；
2. 点开证书详情，"颁发者"是本机 Caddy 的本地 CA；
3. 管理界面：`https://localhost:8443/admin/` 能打开并停在登录页。

### 5.3 关于 `curl -k` / `--insecure`

`curl.exe -k https://localhost:8443/api/v1/health` **只允许用于一次性的本机冒烟排查**
（例如确认进程起来了、路由通不通）。

**绝不允许**把这种"跳过校验"的方式用在手机或任何真实使用场景：

- Service Worker 在不受信任的证书下**拒绝注册**，PWA 装不上；
- Cookie 带 `Secure`，HTTP 或证书不受信的场景下会话行为会与真实环境不一致；
- 跳过校验会让人误以为"证书已经配好"，把问题拖到后面。

## 6. 事后移除这个 CA

不需要再信任它时（例如换了机器、删过 `caddy_data` 卷），请按平台移除。

### Windows

```powershell
# 先按证书名称或指纹定位
certutil -store -user Root
# 移除
certutil -delstore -user Root "Caddy Local Authority"
```

> 证书的显示名随 Caddy 版本变化（形如 `Caddy Local Authority - 20xx ECC Root`）。
> 请用 `certutil -store -user Root` 的输出确认要删的正是**你自己导入的那一个**，
> 不要凭名字批量删除。

### macOS

```bash
security delete-certificate -c "Caddy Local Authority" ~/Library/Keychains/login.keychain-db
```

### Linux（Debian/Ubuntu 系）

```bash
sudo rm /usr/local/share/ca-certificates/usapp-caddy-local-ca.crt
sudo update-ca-certificates --fresh
```

也可以直接在系统证书界面里手动删除，效果等价。

## 7. 与证书相关的其它注意事项

- **不要**把证书或私钥提交进仓库：`.gitignore` 已忽略 `*.pem` `*.key` `*.crt` `*.pfx`
  以及 `deploy/caddy/certs/`。
- **不要**在 `apps/*` 里写死 `https://localhost:8443` 以外的 origin。
  接口的 Origin 校验要求字符串完全相等，`http://localhost:8443` 一样会被拒绝。
- 手机访问时**不能**用 `localhost`（那是手机自己）。阶段 0 不解决手机真机 HTTPS；
  真机联调需要另行决定隧道或局域网证书方案，**本阶段不做**，也不要为此放宽任何安全属性。
- Caddy 的证书会自动续期，**不需要**人工干预；只有 CA 本身（`caddy_data` 卷）需要保留。
