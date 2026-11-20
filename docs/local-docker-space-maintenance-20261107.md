# Docker 磁盘维护记录（2026-11-07）

用户先授权清理构建缓存，后单独授权暂时停止 Docker 并离线压缩，要求保留项目可继续使用。未获删除历史容器、镜像或卷的授权，均保留。

构建缓存清理成功，Docker 报告移除 41.55 GB 缓存记录，最终缓存 0。该数字不是 Windows 实际释放空间的保证，不能把共享层大小相加估算回收量。

对 Docker 自己的 `/mnt/docker-desktop-disk` 使用内置 BusyBox fstrim，确认回收 40,988,069,888 字节空闲块。没有安装软件或创建辅助容器。Docker Desktop 随后正常停止，仅终止 docker-desktop 发行版；原 kali-linux 未启动或终止。

目标固定 `C:\Users\王继毅\AppData\Local\Docker\wsl\disk\docker_data.vhdx`，压缩前先验证管理员权限、确切路径和独占打开。第一轮 UTF-16 指令文件只有 DiskPart 启动输出，不能算成功；第二轮使用系统代码页 936 和无空格指令路径，DiskPart 明确记录“已成功压缩虚拟磁盘文件”。只有 select vdisk、compact vdisk、exit，没有删除、格式化或重建数据。

压缩实际文件从 96,235,159,552 字节变为 61,362,667,520 字节。Docker 恢复后文件约 57.18 GiB；维护开始前约 89.66 GiB。C 盘空闲观测从缓存清理前 20.19 GiB 到恢复后 60.51 GiB；这些是时点观测，期间其它系统写入未归因。

恢复后比对：701 个容器标识、51 个唯一镜像标识、3 个卷名称与快照全部一致，差异 0。运行的仍为 usapp-api-1、usapp-caddy-1、usapp-postgres-1；API 和 PostgreSQL 健康，Caddy 运行。没有重建、重新部署、迁移或业务数据查询；未进行浏览器/完整业务验收，健康不等于全部业务路径验证。

审计文件保留在 `Claude outputs/docker-compact-794b634bdf/`：before.json、verification.json、两轮助手与结果/输出日志。第二轮辅助指令文件位于结果记录的 `C:\ProgramData\usapp-compact-2bf9befe0bc042b4b08c252cc658bd0c`。没有清除这些审计文件。

随后开发验收仅使用 Python、自建回环服务和临时 SQLite，没有触发 Docker 构建。全部历史测试容器和镜像仍在；未来构建会重新生成缓存，后续清理或迁移须另确认范围。
