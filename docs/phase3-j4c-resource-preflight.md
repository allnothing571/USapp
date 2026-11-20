# J4c8：只读资源预检入口

签名修正（2026-11-07）：用户诊断输出表明身份已确认、首个桶信息查询被 SIGNATURE_REJECTED。对照[官方 OSS V4 规则](https://help.aliyun.com/zh/oss/developer-reference/recommend-to-use-signature-version-4/)及[官方 Python SDK V4 实现](https://github.com/aliyun/alibabacloud-oss-python-sdk-v2/blob/master/alibabacloud_oss_v2/signer/v4.py)，发现新增 BucketReader 把空值子资源规范化为 bucketInfo= / versioning=；正确规范化不含等号。已修正请求 URL 和签名查询行，不改动既有批次客户端。此前测试也错误地固定了带等号预期，现予纠正，另加完整规范化请求精确字节回归。最终 j4c-sdk-offline-7af7ac6283.log 共 199 项通过、无跳过；真实云端尚未重验。没有更改凭据、权限、实例或表，没有实际云调用。确认存在代码规范违背；是否完整解释实际拒绝，需修正后的真实查询结果确认。

后续诊断（2026-11-07）：用户报告实际桶阶段输出 J4C_RESOURCE_PREFLIGHT_UNKNOWN_OR_REFUSED，尚不知失败阶段或原因，不标记云端通过。新增显式 `--diagnostics`，只打印有限白名单阶段、字段名、HTTP 类别和已知错误码分类；不打印响应体、返回字段值、地址、密钥或异常内容。没有额外请求、重试或放宽校验；工作进程超时/未回收另有固定标签。新增 3 项脱敏/阶段/签名错误分类用例；最终离线回归 j4c-sdk-offline-12fb369896.log 共 198 项通过，无跳过。本批未读取真实凭据或重跑实际请求；下一步由用户运行带诊断的一次只读预检。实际失败尚未修复或确诊。

状态（2026-11-07）：新增默认 dry-run 的资源预检工具及 19 项回归。最终离线回归 195 项通过，无跳过。本批没有读取真实凭据、查询云端、写入对象或行、初始化日志、部署或执行 Docker 构建。J3 继续暂缓。

## 本轮已有资源证据

以下来自用户本轮终端输出、截图或明确确认，未由本批工具独立查询：

- 身份预检输出 `identity_verified=true`、`verified_principals=3`，摘要 e18500bfe6efe8064febc018d495cdd7de14ca3d001ff6bcf4958dc8d6c9cedc。
- 用户先批准三名无数据策略用户及只读身份查询，随后批准上海测试资源和 5 元人工预算；这不是自动扣费上限。
- 三名用户各绑定对应的审阅包策略。策略本身不证明不存在额外授权，最小权限还须正负测试。
- 桶 usapp-j4c-log-e3784866：截图显示上海、标准、本地冗余、私有、阻止公共访问、版本未开启、OSS 完全托管 AES256。
- 实例 uj4ce3784866：CU 按量、高性能、同城冗余；控制台该选项固定，不是日志桶的本地冗余设置。
- 表 journal：三字符串主键 journal_id（分区键）、row_type、row_id；版本 1、永久、允许更新、预留读写 0/0。用户确认建表时开启局部事务；详情页未显示此状态，仍待接口探测。
- 用户复制的公网地址 https://uj4ce3784866.cn-shanghai.ots.aliyuncs.com 与受限配置匹配。

原版本 1 审阅包保留历史提案状态，未改写其摘要或伪造成云端授权证明。

## 工具边界

`tools/j4c_resource_preflight.py`，分 `--phase bucket` 和 `--phase table`。默认只检查审阅包，即使提供凭据和依赖路径也不打开它们。实际运行要求 `--run --ack-read-only-cloud-access`、明确凭据文件及临时依赖位置，继续复用 Windows 受保护句柄读取、包摘要和两小时本地凭据窗口。不要将密钥输入命令行或聊天。

每阶段使用 recovery-reader 的密钥，先进行一次身份查询，拒绝错账号、错名称、非 RAMUser，之后才访问资源。独立工作进程预算 15 秒，超时由父进程回收；操作系统调度和进程创建不是硬实时保证。连接/读取/池等待维持 2/3/1 秒，不重试、不重定向、验证证书和主机。结果只输出固定字段/错误标签，不输出响应体或密钥。

桶阶段最多 3 次请求（身份 1 + 桶查询 2）：

- 固定目标桶的 GET `bucketInfo` 和 GET `versioning`，每个最多一次，响应上限各 64 KiB。
- 独立 OSS V4 查询签名，不改动现有批次对象客户端。
- 验证桶名、地域、公网服务地址、Owner/ID、标准、本地冗余、私有、阻止公共访问和 AES256。
- 版本接口必须返回无 Status；Enabled、Suspended、重复或空 Status 均拒绝。不能把暂停版本视为从未启用。
- XML 拒绝 DTD/实体声明、未知命名空间、所需字段缺失或重复。正常 OSS 默认命名空间支持。
- 不查询对象、不列举、不删除、不上传；没有验证生命周期、跨域、复制、保留锁、对象不可变性或有效策略的完整权限面。配置可能在查询后改变，不是原子快照或持续保证。

表阶段最多 4 次请求（身份 1 + 表操作 3）：

- 复用已固定版本的 TableStore 客户端，先收窄传输层为 DescribeTable、StartLocalTransaction、AbortTransaction，且各最多一次。
- 描述不符合主键、生命周期、版本、允许更新、0/0 预留或存在二级索引时，停止，不开启事务。
- 显式要求 `--ack-start-abort-probe`，开启本 journal_id 分区的一个局部事务并立即中止。没有数据读写、事务提交或故意冲突测试。
- 开启/中止丢失响应或工作进程超时只能报告未知。服务端可能残留事务租约，不能宣称无服务端状态变化，也不自动重试/另发清理。
- 成功仅证明当前调用者可访问并完成该配置及事务探测；不证明实例所有权、事务隔离/原子性、行级范围权限或检查点真实性。

每阶段只在完整验证且工作进程已回收时确认自己的结果。桶 Owner 匹配有单独字段；表所有权始终不作确认。初始化、发布、删除、资源创建及生产服务均不因该结果获准。

## 需另外绑定的最小桶查询权限

新文件 `docs/policies/j4c-e3784866-resource-inspector.policy.json`，不是原审阅包的一部分。仅允许本轮桶资源上的 oss:GetBucketInfo 与 oss:GetBucketVersioning；拟作为临时策略只绑定 recovery-reader，验证后移除时再按明确范围操作。本批未创建或绑定该策略，没有扩大原三份策略。

## 离线验证

- 新增 `tests/test_j4c_resource_preflight.py`：19 项，涵盖纯 XML 规则、CLI 门槛、包/凭据不读取、身份先决条件、查询签名包含子资源、错误脱敏、不重试/跳转、默认网络拒绝、真实固定 SDK 的合成 Describe/Start/Abort、错误配置提前停止、中止丢失响应、拒绝提交/写入/重复调用。
- 初次非加密回环回归：192 项，4 项回环用例跳过，日志 j4c-sdk-offline-8cb9f198af.log；之后增加 3 项回归。
- 受限沙箱运行 j4c-sdk-offline-0b40c2a168.log：195 项中原回环 3 项失败，服务端未收到请求；原因未独立确证，不能当作产品修复。
- 原断言不变，允许本机回环后 j4c-sdk-offline-903cba7e49.log：195 项通过，无跳过，12.329 秒。
- 输出范围字段修正后最终 j4c-sdk-offline-30ee7e1ab4.log：195 项通过，无跳过。没有增加测试进程 20 秒限制。
- 既有独立工作进程组未重跑；新增入口的整体超时复用已有工作进程工具，新入口本身没有新的真实网络/跨进程实测。Bucket 查询签名与实际响应形状未在真实 OSS 验证，不把桩测试当成云端通过。

## 官方依据与下一步

[GetBucketInfo](https://help.aliyun.com/zh/oss/developer-reference/getbucketinfo)定义查询权限和桶所有者、位置、加密等响应字段；[GetBucketVersioning](https://help.aliyun.com/zh/oss/developer-reference/getbucketversioning)明确无 Status 表示从未开启版本。查阅日期 2026-11-07。

下一步先由用户创建并仅给 recovery-reader 绑定上面的两项只读权限；随后逐阶段给出明确命令。若本地凭据窗口过期，仅续期本地文件并重新校验权限，不新建云密钥、不直接重跑未知结果。完整发布运行器、权限负例、初始化、限额、真实存储保证、检查点/恢复和 J3/J5 等仍未完成。
