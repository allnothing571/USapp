# C11a：超时后的单独收尾入口

状态（2026-11-08）：入口和离线验证完成。没有读取真实凭据、查询/写入云端、重新注入故障、执行 C11b、初始化或清理。本入口仅处理已授权 C11a 的同一条纯合成 aborted 收尾，不能重跑原整批。

## 实际证据与未确认边界

用户提供原 C11a 失败文件：受控 loss-intent 满足 SDK 成功标记、预期 UNKNOWN、无超时且子进程已回收；recover-intent 与 replay-readonly 均通过且无写 API。abort-intent 超时、子进程已回收，整个原运行 completed=false。累计已知 278 请求、5 对象 PUT 尝试、upload 5731、wire 57944、receive 配额 1169315；已经包含前批 179，不再重复相加。receive 包含未结算保留额度，不能直接当作实际下行字节。

abort-intent 已声明候选对象 9660afa17516d35765d91d7f3d4ec145520ec8bc85744ab8cde3e5f9e103c165.json，不能凭上传判定元数据提交；其 API 记录没有 CommitTransaction，但不能仅以日志缺项替代独立云端读取。

用户后来在控制台按精确主键读取：head/current 的 batch_seq=3、batch_head=aac12c0e767225240ed5e7ec331695f82315212f534bedf9b61041727223e4fc，operation/632d633b-aae9-51b0-8fe2-ac9093b2a858 的 phase=intent，操作、指纹及 subject 与场景一致。之前误发的 permission_probe payload 仅是保护行，不作检查点证据。以上均为用户提供的分时观察，不是原子快照或持续状态保证。

核心答复丢弃与只读重放已有本轮真实证据；原批次的中止/最终验证尚未完成，不能报告 C11a 整体通过。

## 入口与固定范围

新 tools/j4c_terminal_finalize.py，默认 dry-run、不读任何凭据或失败证据路径、不创建账本。固定原 manifest 和原 C11a 操作，独立收尾场景摘要：e4d793887c8706d9d3f209c96b50ae141cf84d280420d310fa415fbe5ed05902。

真实运行要求 --run、--ack-read-or-single-terminal-commit、上述精确 --scenario-sha、受保护凭据文件、临时 SDK 目录、原 C11a 失败证据及另一个全新状态目录。失败证据必须精确对应原阶段顺序、SDK 成功标记、只读恢复无写、最后 aborted 超时并已回收，且候选键已声明、操作数 2。不能用其他写入失败、未回收或已经 completed 的文件启用本入口。文件本身不是授权或可信检查点。

每次先重新核对三身份、桶、表，再读取当前 head，其他历史不接受：

- head 为精确的第三批：独立验证三批次全链、bootstrap、保护行，以及已上传候选的完整字节。之后最多一次调用原 Metadata.commit，期望前驱为固定第三批、目标为固定第四批。
- head 已为精确的第四批：仅验证完整四批次链、bootstrap、保护行，不执行任何写入或提交。
- 其他 head、缺失/不匹配候选、权限异常、配置变化、未知或超时：停止，不上传、不补数据、不恢复旧初始化。

中止路径复用已上传候选；所有对象客户端拒绝 PUT。移除的仅是原 Publisher 外层重复完整读取、对象 PUT/回读等步骤；**原 Metadata.commit 自己的独立完整链、索引、候选字节验证、事务中 head 复核、条件更新、原子提交及未知提交处理保持不变**。不是裸 UpdateRow，也不以之前的缓存代替独立验证。

每个工作进程仍 5 秒、每组离线测试仍 20 秒，未知时停止，不自动重试。若在独立检查和实际提交之间状态已变化，仍由原验证/条件更新拒绝，不能盲目覆盖。只读检查只是当时的事实；原调用结果和远端租约消失不因它们被重构或保证。

## 账本、证据与结果解释

结转原失败文件的全部已知消耗（包括可能未结算接收预留），不重新加入 179、不退款、不重置旧目录；本地两小时窗口不是云密钥过期或账号总计费上限。原两个操作登记保留，不创造第三条合成操作或新批次内容。原 C11a 目录、保护行和对象均保留。

新目录使用原父进程账本、API 审计/发送前预扣及回收后本地 hot journal 恢复，审计明确禁止任何对象 PUT；非提交阶段禁止所有写 API。新证据 test_kind=c11a_terminal_finalize，记录分支、prior_sdk_success_observed、new_fault_injection=false、object_upload_allowed=false、terminal_fact_confirmed。没有新的故障成功标记，不能伪称第二次注入。

完成后输出 J4C_TERMINAL_FACT_CONFIRMED，只表示本次核对了完整终态；不会改写原 failed run 的 completed=false 或抹去其超时。若本次提交答复也未知，即使实际可能已达到第四批，仍停止，不能自动进入最终验证/重试。

## 离线验证

| 组 | 数量 | 日志 |
|---|---:|---|
| 原基础回归 | 244 | j4c-sdk-offline-6bd6359a00.log |
| terminal-unit | 9 | j4c-sdk-offline-69f3c31ac1.log |
| terminal-commit | 1 | j4c-sdk-offline-29bf0c761d.log |
| terminal-read | 1 | j4c-sdk-offline-7219d01140.log |
| terminal-stop | 1 | j4c-sdk-offline-290cb9846b.log |
| 原 reply-loss 路径复验 | 1 | j4c-sdk-offline-5d3f653407.log |

合计 257 个不同用例分组通过，无跳过；其他旧组未重跑。新跨进程测试明确预置第三/第四批内存服务，验证独立 Metadata.commit 与最终回读无上传、第四批无提交、提交后断连未知即停。它们不是一次完整真实 CLI 云端演练；无真实故障、并发、恢复演练或部署验收。

共享 r.execute 仅添加默认关闭的 terminal_finalize 路由，由新入口使用；原入口默认行为不变。原 Metadata、Publisher、数据库、契约、服务端、依赖、部署均未变更。原有 J3、C11b、真实不可变性、防回退、独立删除日志、完整权限/故障/上线门槛，以及 v2 原始 guard-create 错误原因仍开放。

下一步由用户在原已授权的 C11a 收尾范围内执行一次此入口，仅读/最多一次同一元数据提交。不要重跑旧整批或两批次脚本，不删除/改名状态目录；如本地凭据窗口已过期，仅更新本地有效期，不另建云密钥。
