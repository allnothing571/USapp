# J4c7：凭据保护与只读身份预检入口

状态（2026-11-07）：完成默认 dry-run 的只读身份预检入口、Windows 凭据文件读取器和 STS（Security Token Service，安全令牌服务）当前身份核对组件。本轮只执行代码、合成文件、内存响应及自建加密回环测试，**没有读取真实密钥、查询真实云账号、创建或绑定身份、创建桶/表、实际初始化/发布日志或部署**。J3 后续仍暂缓。

这只是实时试验入口的身份阶段，不是完整云端验收。三名调用者身份匹配不代表资源所有权、最小权限、桶设置、表事务或存储保证已验证。

## 1. 默认不读取凭据、不联网

新增 `tools/j4c_identity_preflight.py`。默认只验证资源审阅包和外部指定的配置摘要；即便传了 `--credentials` 或 `--sdk-site`，也不检查这些路径是否存在，更不读取文件或导入该处依赖。

已执行以下 dry-run，退出 0：

```powershell
python -B -m tools.j4c_identity_preflight --package 'docs/reviews/j4c-e3784866' --manifest-sha e18500bfe6efe8064febc018d495cdd7de14ca3d001ff6bcf4958dc8d6c9cedc --credentials 'C:\j4c-dry-run-must-not-open.json'
```

输出 mode=dry_run，identity_verified、resource_ownership_verified、creation_authorized、cloud_calls_allowed、serving_allowed 全为 false。审阅包仍是离线资源提案，不是任何操作的授权凭据；二次读取 manifest 后再比对摘要，改变配置则拒绝。

真正执行只读查询同时要求显式 `--run`、`--ack-read-only-cloud-access`、凭据文件、已选依赖目录及正确摘要。标志只是操作者确认意图，不是密码学授权或用户批准证明；自动化代理仍须先获得用户对真实凭据读取和云调用的单独授权，不能从文档/配置推断。

本轮没有执行这个真实 Run 路径。它只支持最多三次 GetCallerIdentity 查询，不包含资源创建/设置、日志初始化、发布、删除或清理。成功也固定报告 resource_ownership_verified=false、creation_authorized=false、serving_allowed=false。

## 2. 凭据文件边界

新增 `tools/j4c_credentials.py`。只接受操作者显式指定的普通本地固定盘文件，拒绝仓库内文件、网络路径、路径中的重解析点、硬链接及大于 16 KiB 的文件；不搜索环境、用户配置或其它路径。

Windows 通过同一个文件句柄读取最终路径、大小/链接数、所有者和 DACL（Discretionary Access Control List，自主访问控制列表）后再读取内容。检查期间不共享写入或删除访问。要求当前用户所有、DACL 受保护、无继承规则、只有当前用户及可选 SYSTEM/Administrators 的普通 Allow 规则，且当前用户拥有读取数据权限；未知/拒绝型规则、其它主体或不满足条件均停止。读取器本身不修复权限、不接管文件或修改用户文件。

文件 schema 为 version、manifest_sha256、expires_at、principals。principals 只能是 object-publisher、checkpoint-writer、recovery-reader，各含 access_key_id/access_key_secret；三组 ID 必须不同，配置摘要必须匹配。只支持这些独立 RAM（Resource Access Management，资源访问管理）用户的访问密钥，暂不支持临时会话令牌或角色身份，额外字段不能悄悄忽略。

expires_at 是 UTC（Coordinated Universal Time，协调世界时）秒级 Z 格式，读取时必须尚未过期且不超过未来两小时。**这是本地拒绝规则，不会让云端永久密钥自动失效**；禁用/撤销云密钥仍需所有者操作。

密钥字段不进入对象 repr、异常正文、命令参数、环境变量或输出日志。已批准执行时，父进程读取后只通过自有工作进程的内存管道传递；没有额外落盘密钥副本。不承诺 Python 字符串或系统内存的物理清零，也不抵御同用户/管理员控制整台机器的攻击。

## 3. 身份查询与签名

新增 `tools/j4c_identity.py`。请求固定为上海 STS 端点、POST /、GetCallerIdentity、版本 2015-04-01、空正文；每次独立 nonce，无密钥或签名查询字符串。默认传输仍阻止网络；只有明确 Run 的工作进程选择实际连接池。连接/读取时限 2/3 秒，不重试、不跟随重定向、校验证书、拒绝压缩及大于 16 KiB 的响应。

官方已将[旧 V2 签名机制](https://help.aliyun.com/en/sdk/product-overview/rpc-mechanism)标为停用，本批采用[当前 V3 规则](https://help.aliyun.com/en/sdk/product-overview/v3-request-structure-and-signature)。公开完整示例的规范请求摘要 `7ea06492...45b1e259` 和最终签名 `06563a9e...3283c0` 均匹配；不是只用同一实现自算预期值。[上海端点](https://www.alibabacloud.com/help/zh/ram/developer-reference/api-sts-2015-04-01-endpoint)及官方[POST GetCallerIdentity 示例](https://help.aliyun.com/en/landing-zone/solution-242)另作核对。元数据 JSON 的公开抓取失败，没有把它算作成功；没有因此尝试真实账号。

根据[官方当前身份接口](https://www.alibabacloud.com/help/en/ram/developer-reference/api-sts-2015-04-01-getcalleridentity)，查询结果含身份类型、账号及调用者 ARN（Alibaba Cloud Resource Name，阿里云资源名称）。客户端要求 RAMUser、AccountId 与声明账号完全相同、Arn 与指定新用户完全相同；拒绝主账号、角色、错误账号或其它用户。固定顺序验证三人，任一失败即停止，结果未知或进程未回收不能报已验证。

整个身份工作进程最多 10 秒，另用既有进程回收预算；最多三次查询，没有自动重试。该上限不包括文件读取或操作系统进程创建的硬实时保证，也不等于费用止付。返回只含核对成功计数和摘要，不返回原始身份/错误响应。

## 4. 本轮验证

新增 24 个不同用例：schema/权限模型 6、V3 向量 1、身份响应桩 4、入口门槛 5、工作进程逻辑 2、实际 Windows 文件权限 3、身份加密工作进程 3。

本轮执行 **183 个不同用例，分四个有独立截止的进程通过**，包含原离线回归；原有另外 19 个工作进程用例没有重复运行。不能写成本轮一次跑过全部累计用例。

| 组 | 数量 | 用时 | Claude outputs 日志 |
|---|---:|---:|---|
| 离线/原本地连接/新签名与门槛回归 | 176 | 11.947 秒 | j4c-sdk-offline-c587fbc30a.log |
| 原对象整体截止（独立重验） | 1 | 5.533 秒 | j4c-sdk-offline-e3bce9cffb.log |
| 实际 Windows 合成凭据文件权限/句柄 | 3 | 3.905 秒 | j4c-sdk-offline-86568b0b02.log |
| 合成凭据跨进程及自建 STS 形状服务 | 3 | 5.344 秒 | j4c-sdk-offline-c2e22f0740.log |

四个进程退出均为 0，原 20 秒测试进程截止未提高。所有文件读取用例都是本轮新建的合成文件；权限设置仅作用于这些文件，未变更真实凭据或目录权限。身份工作进程只连接本次自建回环端口，验证三名指定用户、不同 nonce、错误身份提前停止和缺少确认不连接；自建服务不等于云认证证据。

中间 `472ca47618` 跑 177 项有一项旧对象截止用例失败：未观察到服务端已接受 PUT，尚不能验证写入后超时。此前用例给 2 秒，却依赖工作进程在该时间内完成启动和连接；日志没有记录失败时 timed_out/value，具体原因未确证。将该用例独立分组，改用已发布默认 5 秒，合成慢响应延长至超过预算，保留“已接收写入、超时、回收、只发送一次、结果未知”全部断言，并增加失败状态信息；总测试进程截止不变。实际 3 秒停滞读取用例也保留。不能声称这修复了任何生产故障。

没有本批变异测试、实际供应商请求、真实凭据、用户身份创建、权限绑定、桶/表检查、数据发布/删除、C++、数据库、Web、浏览器或设备验收。没有新增依赖、Docker 构建、部署、迁移、契约或清除开关变化。SDK（Software Development Kit，软件开发工具包）仍只来自既有临时隔离目录；版本目录存在不证明供应链完整，真实执行前仍应按锁定包重建/核对可信依赖。

## 5. 下一步需单独批准的最小实际范围

建议先准备三名**没有数据访问策略**、无控制台登录和继承授权的新 RAM 测试用户：

- usapp-j4c-e3784866-object-publisher
- usapp-j4c-e3784866-checkpoint-writer
- usapp-j4c-e3784866-recovery-reader

此时不绑定审阅包的数据权限。官方 GetCallerIdentity 不要求额外动作授权，但仍需要有效签名凭据；先核对三名用户的账号/名称，再申请后续付费资源和数据权限，避免提前开表或把旧账号的密钥混入。

这会修改真实云账号并生成真实密钥，**尚未授权/执行**。需用户另行同意身份准备、仓库外受保护文件以及最多三次只读身份查询；密钥不发送到聊天。可按用户习惯逐步手动操作，每步完成再继续。本文不批准自动化访问控制台或读取任何现有密钥。

桶/实例/表创建、初始化身份、最小权限正负测试、实际存储保证、可信当前检查点、防回退、恢复演练、J3/J5 和上线门槛仍未关闭。只读身份核验入口不是完整的实时发布/初始化运行器，后者需在资源证据及相应权限获批后接入。
