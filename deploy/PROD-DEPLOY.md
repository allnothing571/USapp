# USapp 线上生产部署手册（GitHub Actions GHCR + 海外 Linux VPS + Caddy 自动 HTTPS）

本手册对应已落地的生产套件：
- 镜像构建流水线：`.github/workflows/release-images.yml`（推送 `main` 或 `v*` 标签自动构建 `api`、`push-worker`、`edge` 到 GHCR）
- 生产编排：`deploy/docker-compose.prod.yml` + `deploy/Caddyfile.prod` + `deploy/Dockerfile.edge`
- 初始化与运维：`database/apply.sh`、`database/bootstrap-online.sql`、`deploy/scripts/gen-secrets.mjs`、`deploy/scripts/deploy-vps.sh`、`deploy/scripts/backup-db.sh`

> 注意：当前 C++ 主程序在未接入持久化删除日志适配器前，启用 `MEDIA_ENABLED=true` 或 `MEMORY_PURGE_ENABLED=true` 会主动拒绝启动，因此 `.env` 模板已默认锁定为 `false`（OSS 原图直传除外的所有核心功能均可正常使用）。

---

## 第一步：本地提交并推送至 `main` 触发镜像构建

1. 检查并整理 `codex/v2-shared-room` 工作树提交后，合并到 `main` 并推送到 `origin`（`github.com/allnothing571/USapp`）：
   ```powershell
   git push origin main
   ```
2. 在 GitHub 仓库的 **Actions** 页面等待 `Release Container Images to GHCR` 运行成功（3 个镜像：`ghcr.io/allnothing571/usapp-api:latest`、`usapp-push-worker:latest`、`usapp-edge:latest`）。
3. 如果你的 GitHub 仓库为 **Private**，请到 GitHub → Settings → Developer settings → Personal access tokens (classic) 生成一个勾选 `read:packages` 的只读 Token，供 VPS 登录拉取镜像。

---

## 第二步：准备 VPS 与域名解析

1. 购买 1 台海外/港澳台 Linux VPS（建议 Debian 12 或 Ubuntu 24.04，**1核2G** 起步即可）。
2. 将域名（如 `us.example.com`）添加 `A` 记录解析到 VPS 公网 IP。
3. 云主机安全组 / 防火墙放行 TCP **`22`**、**`80`**、**`443`**（不要对外开放 `5432`、`8080`、`8081`、`8444`）。
4. SSH 登录 VPS 后安装 Docker Engine 与 Compose 插件（以及 `python3`、`openssl`，一般系统自带）：
   ```bash
   curl -fsSL https://get.docker.com | sh
   sudo systemctl enable --now docker
   ```

---

## 第三步：在 VPS 拉取配置并生成密钥

```bash
git clone https://github.com/allnothing571/USapp.git /opt/usapp
cd /opt/usapp

# 若仓库为私有，登录 GHCR（输入 GitHub 用户名与 read:packages PAT）
docker login ghcr.io -u allnothing571

# 生成模板 .env
cp deploy/env.prod.example .env
chmod 600 .env
```

在本地电脑（已安装 Node.js）运行一次密钥生成器，把输出结果粘进 VPS 的 `.env` 中替换占位符：
```powershell
node deploy\scripts\gen-secrets.mjs "你自己的管理员强密码(至少12位)"
```

并同步补齐 `.env` 中的业务信息：
- `PUBLIC_DOMAIN=你的域名`（如 `us.example.com`）
- `ACME_EMAIL=你的邮箱`
- `ALLOWED_EMAILS=你的邮箱,伴侣的邮箱`
- `SMTP_HOST` / `SMTP_PORT` / `SMTP_USERNAME` / `SMTP_PASSWORD` / `SMTP_FROM`
- `VAPID_SUBJECT=mailto:你的邮箱`

---

## 第四步：一键部署上线

```bash
cd /opt/usapp
bash deploy/scripts/deploy-vps.sh
```

脚本会自动依次完成：
1. `docker compose pull` 拉取最新版 GHCR 镜像；
2. 启动 `postgres` 并等待健康就绪；
3. 运行 `database/apply.sh` 执行 `0000`..`0065` 全部迁移与最小权限角色赋权；
4. 启动 `push-worker`、`api`、`caddy`（自动签发 Let's Encrypt 证书）；
5. 运行 `database/bootstrap-online.sql` 自动上架 20 件装扮商店商品、设置初始穿搭、激活成就、开启推送总闸。

---

## 第五步：管理后台访问（SSH 回环隧道）

管理端绝不暴露在公网 `:443` 上。需要登录 `/admin/` 时，在自己电脑终端保持一条 SSH 端口转发：
```powershell
ssh -N -L 8444:127.0.0.1:8444 root@你的VPS_IP
```
然后在本地浏览器访问：
- `https://localhost:8444/admin/`（首次打开自签内网证书点继续即可，账号为 `.env` 中的 `ADMIN_BOOTSTRAP_USERNAME` 与生成 Hash 时的密码）。

---

## 第六步：配置每日自动加密备份与本地同步

1. 在 VPS 上添加每天凌晨 3 点定时任务（`crontab -e`）：
   ```cron
   0 3 * * * cd /opt/usapp && bash deploy/scripts/backup-db.sh >> /var/log/usapp-backup.log 2>&1
   ```
2. 随时手动执行一次备份并验证文件生成：
   ```bash
   cd /opt/usapp && bash deploy/scripts/backup-db.sh
   ```
3. 从个人 Windows 电脑定期拉取最新加密备份防单盘风险：
   ```powershell
   scp -r root@你的VPS_IP:/opt/usapp/backups/*.enc* D:\usapp-backups\
   ```
4. 灾备解密与恢复命令参考：
   ```bash
   openssl enc -d -aes-256-cbc -pbkdf2 -iter 600000 -in backups/usapp_xxx.dump.enc -out usapp.dump
   ```

---

## 日常版本升级命令

本地代码合并推送 `main` 且 GitHub Actions 构建完成后，在 VPS 上只需执行：
```bash
cd /opt/usapp && git pull origin main && bash deploy/scripts/deploy-vps.sh
```
