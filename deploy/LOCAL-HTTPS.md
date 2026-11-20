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
  2026-10-26 起改用局域网证书方案（不用隧道、不放宽任何安全属性），见第 8 节。
- Caddy 的证书会自动续期，**不需要**人工干预；只有 CA 本身（`caddy_data` 卷）需要保留。

## 8. 局域网 HTTPS（Android 同一 Wi-Fi 访问，2026-10-26 已应用）

> 本节记录 2026-10-26 的历史配置。2026-10-27 已切换到当前 WLAN 地址，现行状态见第 9 节。

> 由 Claude Code 在负责人授权下配置并在本机验证。Android 端的证书信任与浏览器访问尚未执行，
> 由负责人在手机上完成。

### 8.1 规范地址与本机访问方式

- **规范公开地址（唯一）**：`https://192.168.1.55:8443`。API 的 `PUBLIC_ORIGIN` 等于它；
  应用只接受一个精确相等的 Origin（`AppConfig::isAllowedPublicOrigin`），不支持多个值，
  因此没有“localhost 与局域网地址同时可用”的配置。
- **本机也要改用** `https://192.168.1.55:8443`。`https://localhost:8443` 现在只返回
  308 跳转到规范地址（保留路径与查询），不再提供网页，避免在写请求必然被 Origin 校验拒绝的地址上使用应用。
  已在本机浏览器信任本地 CA 的情况下，新地址无需再次导入证书（同一个 CA）。
  本机已安装的 PWA 是按旧地址安装的，需要按新地址重新打开或安装。
- **管理面不变**：`https://localhost:8444/admin/` 只在本机可用（8444 与 API 管理端口 8081
  仍只发布在 `127.0.0.1`；从局域网地址连接这两个端口会被拒绝）。

### 8.2 改动内容（可逆，基础文件未改）

| 文件 | 作用 |
| --- | --- |
| `deploy/Caddyfile.lan`（新增） | 由 `deploy/Caddyfile` 派生，只有三处不同：全局 `default_sni 192.168.1.55`（按 IP 访问时客户端不发送 SNI，原先 Caddy 无证书可选，这就是 Android 的 `ERR_SSL_PROTOCOL_ERROR`）；公开站点地址改为 `https://192.168.1.55:8443`（`tls internal` 为该 IP 签发证书）；`https://localhost:8443` 只做 308 跳转。安全头、缓存规则、`/api/*` 反代、公开站点上 `/api/admin/*` 与 `/admin/*` 的 404、管理站点全部逐字保留。 |
| `deploy/docker-compose.lan.yml`（新增） | 覆盖文件：`api.environment.PUBLIC_ORIGIN = https://192.168.1.55:8443`；`caddy` 改挂 `./Caddyfile.lan` 到 `/etc/caddy/Caddyfile`。镜像、卷、端口发布与其它变量沿用基础文件。 |
| `deploy/caddy/certs/usapp-local-root-ca.crt`（新增，已被 .gitignore 忽略） | 供 Android 导入的本地 CA **根证书（仅公钥证书）**。 |

**此后所有 compose 命令都必须叠加覆盖文件**，否则会把 api/caddy 重建回 localhost 配置：

```powershell
docker compose --env-file .\.env -f .\deploy\docker-compose.yml -f .\deploy\docker-compose.lan.yml ps
```

### 8.3 证书

- 继续使用 `usapp_caddy_data` 卷里原有的本地 CA，未轮换：
  `CN=Caddy Local Authority - 2026 ECC Root`，有效期至 2036-08-13，
  **SHA-256 指纹 `F5:D6:FC:89:C1:C7:4F:5E:74:45:28:84:1C:EB:AF:43:5F:8F:BB:27:B5:13:03:D0:6E:19:D9:08:7E:1E:13:EC`**
  （与原有 `deploy/caddy/certs/root.crt` 相同；重建 caddy 前后一致）。
- Android 用的文件：`D:\JYJ\USapp\deploy\caddy\certs\usapp-local-root-ca.crt`（PEM，627 字节，
  只含证书，不含私钥）。在手机上导入时请核对上面的指纹。
- 站点证书为 `IP Address=192.168.1.55`，由 Caddy 自动签发与续期（短有效期）。
- 从未读取、导出或打印 CA 私钥，也没有复制整个 caddy 数据卷。

### 8.4 应用与验证记录（2026-10-26）

- 备份：`backups/before-lan-https-20261026_225135/`，包含原 `Caddyfile` 与
  `docker-compose.yml` 副本，以及 `rollback-info.json`（原 `PUBLIC_ORIGIN=https://localhost:8443`、
  `ADMIN_ORIGIN=https://localhost:8444`、容器与镜像标识、CA 指纹）。未复制任何秘密或数据卷。
- 校验：`caddy validate`（同一 caddy 镜像，无网络的临时容器）结果 Valid configuration，exit 0；
  `docker compose config` 解析出的 `PUBLIC_ORIGIN` 与端口发布符合预期。
- 应用：api 用同一镜像 `sha256:a5e17286…b480b9` 以 `up -d --no-deps --no-build --wait` 重建，exit 0，
  healthy；启动检查 wallet `checks=187`、task `checks=972`，均为 `ok=true mismatches=0 missing=0 unexpected=0`；
  运行时 `PUBLIC_ORIGIN=https://192.168.1.55:8443`。caddy 用同一镜像 `sha256:d44355d3…5c10d9` 重建，exit 0，
  运行中的 Caddyfile 与 `Caddyfile.lan` 哈希一致，并为 `192.168.1.55` 签发了证书。PostgreSQL、网页 dist 未动。
- 本机验证（不经代理；以导出的根证书为唯一信任锚，校验证书链并要求地址身份匹配；
  仅因该私有 CA 没有吊销列表而不做吊销查询；未使用任何跳过校验的参数）：
  - `https://192.168.1.55:8443/api/v1/health` 200（`Cache-Control: no-store…`）；`/` 与 `/home` 200，
    `index.html` 哈希等于已发布的 M10 版本 `515cf844…`；`/sw.js` 200，哈希 `453bbbcd…`；
    静态响应带 CSP 与 `X-Frame-Options: DENY`；
  - 公开站点上 `/api/admin/session` 与 `/admin/` 均为 404；
  - `https://localhost:8443/…` 308 跳转到规范地址；`https://localhost:8444/admin/` 200；
  - `192.168.1.55:8444` 与 `192.168.1.55:8081` 连接被拒绝；`127.0.0.1` 上二者可达。
- 未做：登录、发码、任何任务完成或兑换写操作；防火墙、VPN、系统代理、路由器设置均未改动。

### 8.5 回滚（未执行）

```powershell
docker compose --env-file .\.env -f .\deploy\docker-compose.yml up -d --no-deps --no-build --wait --wait-timeout 120 api
docker compose --env-file .\.env -f .\deploy\docker-compose.yml up -d --no-deps --no-build caddy
```

即不带覆盖文件重建 api 与 caddy，恢复 `PUBLIC_ORIGIN=https://localhost:8443` 与原 Caddyfile；
新增的两个文件可以保留不用。CA 不变，已导入的信任无需改动。

### 8.6 注意事项

- **地址会变**：`192.168.1.55` 来自 DHCP。若地址变化，需同时修改 `Caddyfile.lan`（三处）与
  `docker-compose.lan.yml`，然后按 8.4 的方式重建 api 与 caddy。最好在路由器上为本机做 DHCP 地址保留（负责人自行决定）。
- **VPN（FlClash）**：本机另有虚拟地址 `28.0.0.1`，那**不是**局域网地址，手机不能用它访问。
  本机诊断时用了 `--noproxy`/不走代理的方式；本次没有修改任何 VPN 或系统代理设置。
  若手机同时开着 VPN，访问 `192.168.1.55` 可能被代理拦截，需要在手机 VPN 中让局域网地址直连（负责人自行处理）。
- 仍然禁止：跳过证书警告、`-k/--insecure`、HTTP 降级、放宽 Origin/CSRF、对公网暴露或端口转发。

## 9. 当前 Wi-Fi 地址（2026-10-27）

- 负责人选择切换后，当前规范地址为 `https://10.6.245.114:8443`，它等于 API 的
  `PUBLIC_ORIGIN`。`Caddyfile.lan` 与 `docker-compose.lan.yml` 已同步更新；每次 Compose
  命令仍须带两个配置文件。管理员入口仍在本机回环地址。
- 使用相同的 Caddy 本地根 CA。Caddy 内公开根证书文件与已导出给 Android 的证书文件哈希一致；
  未读取或导出私钥。手机已信任该 CA 的事实不等于新地址已在手机上验证。
- 本机直接访问新地址时，证书链与 IP 匹配检查通过；健康接口 200/no-store，未登录留言接口
  401，公开站点管理路径 404，网页与服务工作线程哈希与本次构建一致。手机是否能连接当前
  Wi-Fi、登录和安装仍待负责人确认。
- 切换前的两个 LAN 配置及候选文件保存在
  `backups/before-p21-20261027_203943/lan-before-switch/`。地址由当前 WLAN 提供，换到另一
  Wi-Fi 或 DHCP 重新分配后可能失效；应先重新核对电脑 WLAN 地址，再决定是否切换配置。
  不同 IP 是不同来源，手机可能需要重新登录及重新添加桌面入口。
- P2-1 发布的数据库、网页和镜像证据及回退限制见 `docs/phase2-messages.md` 第 13 节。
