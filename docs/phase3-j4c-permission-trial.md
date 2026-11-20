# J4c10：三项条件写入权限负例

## 用户实际 v3 结果（2026-11-07）

用户提供终端 J4C_PERMISSION_PROBES_PASSED 和 v3 trial-evidence.json：completed=true；15 阶段均 ready、timed_out=false、worker_reaped=true。身份、资源、完整链、初始化回执及保护行前后核对通过。保护行阶段终端明确 GUARD_ABSENT_SINGLE_CONDITIONAL_WRITE，随后 guard-read 通过；两次表负例返回明确 TABLE_PERMISSION_DENIED，对象负例及最终回读通过。服务未启用、未清理。本节为用户运行提供的证据，本批没有由代理重跑真实请求或读取凭据。

累计负例配额为 requests=91、wire=15066、receive=39775、puts=1、upload=1147、operations=0，其中已包含 v2 的 51/7554/18708（v2 又包含最初只读失败的 30/4671/10775），不能重复相加。v3 新增请求为 40。puts=1 是一次被拒绝的 PutObject 请求及其上传体，不表示新增第三个对象。正常新增的远端数据只有纯合成保护行；原两批次、链/索引、bootstrap 回执及保护行的最终验证通过。

guard_resolution_mode=true 和 guard_creation_outcome_not_proven_by_matching_row=true 为处理模式的保守标志，不将“已有匹配行”当成 v2 创建成功。本次终端走了“缺失后单次条件写入”分支；v2 原始错误类别仍无法还原，不能因 v3 通过就宣称根因已修复。

本次仅验证当时 object-publisher 的 PutRow、recovery-reader 的 PutRow 和 PutObject 三个请求明确被拒绝；不关闭其他动作/资源范围、并发、未知提交、真实不可变性、防回退和恢复门槛。保留最初失败目录、v2、v3、正向试验目录、两对象、保护行、表/实例/桶和身份；未授权在本步骤删除。后续优先准备并发发布及提交答复丢失的独立受控方案，不重复初始化或扩大真实业务范围。J3 按用户决定仍暂缓。

## 显式先读保护行处理（2026-11-07）

用户在控制台按保护行单行查询，截图显示未返回数据；这只是该次查询观察，不用截图替代后续 API 验证，也不能还原原始 PutRow 的错误码。

增加 --previous-guard-failure 与 --ack-read-first-guard-resolution 配对选项，不能与 --previous-readonly-failure 混用。只接收同日志/摘要、前七阶段均 ready、guard-create conflict/unknown 且无超时并已回收、没有后续负例或候选对象写入的失败证据。对象 PUT=0 并不证明表未写入，该路径明确按“可能已应用”处理。累计 requests/wire/receive 结转，本例为 51/7554/18708，不重复加 30。

仍由用户显式指定另一个全新目录；先完整重做身份、资源、链、初始化回执门槛。guard-create 阶段先以 recovery-reader 精确读取保护行：若匹配则固定标签说明 NO_REWRITE / original_creation_outcome_unproven，并继续，不再写入；若不存在仅允许 writer 一次 EXPECT_NOT_EXIST 插入；若不匹配、读取未知或部分结果则停止，不写。读取后可能发生并发变化，条件插入仍防止覆盖，任何条件冲突/未知不会自动重试。沿用各阶段 5 秒，不增加权限、不变更现有头或日志批次、不清理任何目录。

证据增加 guard_resolution_mode 与 guard_creation_outcome_not_proven_by_matching_row，避免把复用事实当成原写入成功。此路径是明确授权的人工处理，不是通用业务写入恢复/自动续跑，也不证明防回退或原请求因果关系。新目录建议 usapp-j4c-permissions-e3784866-v3，原目录和 v2 均保留。

新增 4 项离线回归：写入未确认不能作为只读失败、严格旧失败条件；匹配行不写、缺失行先读再单次条件写、异常内容/未知不写。最终日志 j4c-sdk-offline-14ac01c1fc.log：239 项通过，无跳过。其他独立工作进程组未重跑。本批未读取真实凭据或执行真实云端请求；原错误原因仍未证实，下一次实际请求会输出固定错误类别。

## v2 保护行创建未确认（2026-11-07）

用户本地凭据窗口过期导致一次准备拒绝，未创建 v2 目录；用户仅更新本地 expires_at 后执行 v2。v2 通过身份、桶、表、单次链和独立 bootstrap-before，在 guard-create 停止：旧实现记录 state=conflict、timed_out=false、worker_reaped=true。累计 51 请求、wire 7554、receive 18708，其中结转前一轮 30/4671/10775；operations/puts/upload 均 0、对象候选空。这里 puts/upload 只统计 HTTP PUT 对象上传，不能据此声称 POST PutRow 没有发送或生效。尚未执行后三项负例。

代码缺陷：guard-create 把 write_guard 的 UNKNOWN 也映射为 CONFLICT，旧输出不能判断条件、授权、参数、锁或传输错误。已保留 UNKNOWN，并新增固定白名单诊断（权限拒绝、条件、参数、认证、行/事务冲突、配额、超时或未确认）；不输出错误正文或 request ID。没有改写条件、重试或权限。新增 2 项分类/脱敏回归；离线日志 j4c-sdk-offline-82652c48e6.log 共 235 项通过，无跳过；其他独立工作进程组未重跑。本批未读取真实凭据或执行真实查询/写入。

下一步仅由用户在本轮表的数据管理中读取精确保护行主键及合成 payload，确定是否存在/匹配，再设计一次有证据的后续操作。不要重跑初始化/负例、创建 v3、覆盖或删除 v2。现有 readonly_failure 门槛不接受 guard-create 失败，不能拿前一轮只读超时文件绕过本次写入结果未确认。局部事务文档允许事务外 PutRow，不能仅凭表启用事务就断言本次失败原因；实际错误类别仍未知。

## 实际只读超时后的修正（2026-11-07）

用户实际运行停在 verify-before：timed_out=true、worker_reaped=true，前五阶段均通过。证据为 30 请求、wire 4671、receive 10775、operations/puts/upload 均 0、object_keys 空。结合该阶段代码只进行读取及 Start/Abort，尚未进入保护行创建或三项负向写入。可能残留读取事务租约，其服务端到期未测；不能由子进程已回收推断远端租约已消失。

发现旧 verify-before/after 先调用 live.verify 再完整 _load 一次，重复读取两遍链。去掉重复读取，单次快照同时核对完整两批次、相位、合成操作和精确对象键，SmokeMetadata 继续核对当前检查点/索引。bootstrap-before / bootstrap-after 独立校验初始化回执，guard-read-after 独立核对保护行。每阶段仍 5 秒，不放宽超时、不删完整性校验。已证实实际超时及代码重复；是否完整解释现场时延，需要新实际运行验证。

新增可选 --previous-readonly-failure：仅接受同摘要、同日志、前五阶段 ready，且 verify-before 只读超时并已回收、没有后续阶段、候选对象为空且 operations/puts/upload 为 0 的旧证据。不会续跑、覆盖或清理旧目录；用户手动指定另一个全新目录，旧 requests/wire/receive 先计入新账本，再发任何请求，不重置已知消耗。它是有限的人工新试验，不是已写入后自动恢复协议；本地证据不是防伪/防回退的可信权威。

新增 5 项基础回归覆盖单次链读取、独立初始化回执、不降低键/相位验证、旧失败拒绝条件及消耗结转。增加 permission-verify 独立工作进程用例，用真实固定 SDK 和自建加密回环验证单次完整链加独立回执；对象恰读两次、两个 Start/Abort、没有 PutRow/Commit，行内容未变。日志 j4c-sdk-offline-fa1f9fcc53.log：1 项通过，7.653 秒。最后基础日志 j4c-sdk-offline-fd3f2356d4.log：233 项通过，无跳过，15.053 秒。另有中间 231 项通过记录 2e01f747fc / 55295158b2。一次最终验证因自动审批服务额度不足未执行；用户继续后获准补跑，通过结果如上。原 negative 请求桩和其他旧工作进程组没有本轮重跑。

本批只开发/离线验证，未读取真实凭据或用户实际试验目录、未发真实云调用。保留原 usapp-j4c-permissions-e3784866；拟由用户明确执行一次新目录 usapp-j4c-permissions-e3784866-v2，并引用旧失败证据结转计数。新运行前若凭据窗口过期，停止处理本地有效期，不更换云密钥或重复发请求。

状态（2026-11-07）：入口和离线测试完成，本批未读取真实凭据、执行云端查询/写入、更改策略或清理资源。用户已同意继续准备最小权限负例；实际运行由用户执行下面范围的专用入口，不重跑初始化。

## 前置实际证据

用户提供 J4c9 成功终端和 trial-evidence.json：completed=true，9 阶段均无超时、worker_reaped=true；一条合成操作，2 PUT，88 请求，上传 2292 字节、请求体 20883 字节、接收 26181 字节。真实初始化、intent、aborted 和恢复回读通过；服务未启用、未清理。该证据由用户运行提供，本批没有独立云端重验。

日志 0afe377a-3e06-4134-997f-4f1a437b8492 的两个对象：

- journals/0afe377a-3e06-4134-997f-4f1a437b8492/batches/87ab1abff10482be97cb530b0d017437bb8690a0d2e440a790993af3d4a2d120.json
- journals/0afe377a-3e06-4134-997f-4f1a437b8492/batches/de9b1928cdc212238f831f3ff56d73cb36bc6e30f247316f1797c57461d3aa86.json

## 精确测试范围

新增 tools/j4c_permission_trial.py，默认 dry-run。实际运行要求 --run、--ack-conditional-permission-probes、同一审阅包和摘要、受保护凭据文件、旧正向试验的精确证据文件、固定临时依赖和另一个全新状态目录。默认不读取任何传入的凭据、旧证据或状态目录路径。证据校验不代替真实回读。

每个阶段 5 秒，使用原父进程配额、网络预扣和确切主机白名单，不增大 20 秒离线测试进程限制。不支持续跑、自动重试、初始化、清理、资源或权限修改。负例是独立试验账本；此前 88 请求不在新账本内，不能声称本地限额跨批次合计或自动限制 5 元账单。

执行顺序：

1. 三名用户各一次身份查询；检查桶配置/从未启用版本和表配置/开启立即中止事务。
2. recovery-reader 验证 J4c9 完整两批次链及合成操作，实际对象索引须与旧证据中的两键一致。
3. checkpoint-writer 创建一条额外保护行，主键精确为 (journal_id, permission_probe, write-guard-v1)，payload 只有固定版本、合成标识和摘要。独立 PutRow，EXPECT_NOT_EXIST；已有行即停止，不覆盖、不借此恢复。
4. recovery-reader 成功读取和验证该行，形成正向对照。
5. object-publisher 尝试 PutRow 同一保护行，EXPECT_NOT_EXIST，期望明确权限拒绝。
6. recovery-reader 尝试同一个有条件 PutRow，期望明确权限拒绝。
7. recovery-reader 读取一个已验证完整批次，并用原字节尝试 PUT 同键，x-oss-forbid-overwrite=true，期望 403 AccessDenied。
8. recovery-reader 再验证日志链、两对象及保护行未变化。

正常情况下只新增一条保护行，不产生新日志批次、不修改检查点。即使负例获得了错误授予的权限，表条件也不允许覆盖现有保护行；若有人并发移除了保护行，最多重新创建同一条纯合成行。对象发送的是该对象已验证原字节，附禁止覆盖头；不宣称可以阻止管理员并发改变版本控制或保障所有元数据无变化。非权限拒绝立即停止并保留证据，禁止自动修复。

## 严格结果分类

只接受以下明确拒绝：

- TableStore：HTTP 403 + OTSNoPermissionAccess；或者 HTTP 403 + OTSAuthFailed 且错误消息精确属于官方的两种“无实例访问权限”文本。仅在内存比较固定原文，不打印原始消息。
- OSS：HTTP 403 + Error/Code=AccessDenied。签名错误不能算通过。

OTSAuthFailed 同样可能表示坏密钥、时间、签名、实例不存在或网络限制，不能笼统接受。条件失败、409 已存在、2xx、其他 4xx/5xx、超时、未知或未回收子进程均不算权限负例通过。2xx 的意外表写入标为冲突；条件拒绝也不证明权限策略有效。

**范围限制：** 三项通过只证明当时这些目标和调用者的 PutRow / PutObject 请求被明确拒绝，不证明 UpdateRow、CommitTransaction、删除、其他桶/表/分区或其他继承授权均被拒绝。不是完整最小权限认证，也不是不可篡改/故障恢复验收。

## 证据和后续清理

新目录保留 trial-evidence.json 和独立 trial-<manifest>.sqlite3，记录 test_kind=negative_permissions、先前两对象键、额外保护行主键、各阶段枚举、配额和潜在 PUT 对象键。不保存密钥或服务端原始错误。失败时 JSON 可能落后于 SQLite，保留二者；不要删除目录、换目录重跑或动云端数据。

保护行是未知逻辑类型，既有读取器忽略它；初始化器会拒绝非空分区，本轮原本也禁止重新初始化。它必须加入后续确切清理清单，不应因业务索引读不到就遗漏。没有自动删除代码或清理授权扩展。

## 本批离线验证

- 新增 tests/test_j4c_permission_trial.py，12 项：严格错误分类、证据绑定、dry-run 不读路径、运行开关、新目录、失败即停、固定 5 秒、精确保护行/条件的真实 SDK 编码、错误签名/网络/条件不冒充拒绝、真实 SDK 正向保护行读取、正确调用者选择等。
- 新增 tests/j4c_worker/test_permission_worker.py，独立 permission-stages 组 1 项：三次新子进程通过自建加密回环发送两个条件 PutRow 和一次 GET+PUT；服务端检查保护行、条件、原字节、禁止覆盖头并显式拒绝。使用合成密钥；没有模拟完整 CLI、真实身份/桶门槛或真实云端授权。
- 初次 j4c-sdk-offline-431a13a157.log：227 项，1 失败、7 错误、4 跳过。包含沙箱下既有文件替换 WinError 5，新增测试使用 SDK 序列化器时缺少参数，以及测试误把签名拒绝预期为冲突（产品当时实际返回未知，正确不算通过）。后两项仅修正测试；不宣称修复云端产品缺陷。
- j4c-sdk-offline-39c53b228a.log：227 项通过，无跳过，15.047 秒。
- 最终基础 j4c-sdk-offline-1852878152.log：228 项通过，无跳过；独立新工作进程 j4c-sdk-offline-d88173a89a.log：1 项通过、7.246 秒。本轮 229 个不同用例分组通过；其他旧工作进程组未重跑。
- 带不存在凭据/旧证据/状态路径的 dry-run 已通过；未读取真实密钥、既有实际试验目录或查询云端。没有 C++、Web、契约、迁移、依赖、部署变更。

## 官方依据

[TableStore 错误码](https://help.aliyun.com/zh/tablestore/developer-reference/error-codes)说明 OTSAuthFailed 的多种原因及 OTSNoPermissionAccess；[PutObject](https://help.aliyun.com/en/oss/developer-reference/putobject)区分 AccessDenied 与禁止覆盖的 FileAlreadyExists。[版本控制说明](https://help.aliyun.com/en/oss/user-guide/overview-78/)指出版本开启时禁止覆盖头不生效，因此不能把开启或暂停版本的桶带入本测试。查阅日期 2026-11-07。
