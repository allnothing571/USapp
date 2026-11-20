// =====================================================================
// services/api/src/db/TaskPrivilegePolicy.h
//
// 任务表（tasks、task_completions、task_progress_events）及只读 task_read_model 的有效权限检查（M5）；
// docs/phase1-m3-design.md 第 3.1 节，D26）。
//
// 与钱包检查（WalletPrivilegePolicy）相同的身份要求：current_user 必须是 usapp_api、
// 不是超级用户、不属于任何其他角色、usapp_push 存在；任务表存在且 owner 不是 current_user。
// 任务表的期望矩阵：
//   * 表级：current_user 只有 SELECT、INSERT；UPDATE、DELETE、TRUNCATE、REFERENCES、
//     TRIGGER、MAINTAIN 一律为否；usapp_push 与 PUBLIC 全部为否。
//   * 任意列：current_user 的 SELECT / INSERT 为是，UPDATE / REFERENCES 为否；其他主体全部为否。
//   * task_read_model 仅允许 current_user SELECT；表级及任意列 INSERT 也必须为否。
//   * 列级 UPDATE：所有主体、所有列一律为否（任务表从不加行锁，因此没有锁列）。
//
// 复用 WalletPrivilegeRow / WalletPrivilegeCheck / WalletPrivilegeReport 的数据形状与
// WalletPrivilegePolicy 的 rowsFromResult / safeName；钱包检查本身不做任何改动。
// M3-A 起供测试使用；M3-B 起 API 启动时经 TaskStartupCheck 复用同一实现（钱包检查之后）。
// 输出只含角色名、表名、列名、权限名与布尔值：从不包含连接串、口令或任何密钥。
// =====================================================================
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/Db.h"
#include "db/WalletPrivilegePolicy.h"

namespace usapp::db {

class TaskPrivilegePolicy {
  public:
    static const std::vector<std::string> &taskTables();

    // 只读查询；无参数。返回列与 WalletPrivilegePolicy::matrixSql() 相同。
    static const char *matrixSql();

    // 某一行的期望值；无法识别的行返回 std::nullopt。
    static std::optional<bool> expectedFor(const WalletPrivilegeRow &row);

    // 纯函数：根据实际值行生成报告（含完整性检查）。
    static WalletPrivilegeReport evaluate(const std::vector<WalletPrivilegeRow> &rows);

    // 任务矩阵的摘要（标注为 task privilege policy；只含安全名称与布尔值）。
    static std::string summary(const WalletPrivilegeReport &report);

    // 在给定客户端上执行矩阵查询。错误描述不含 SQL 参数或连接信息。
    static void check(const ClientPtr &client,
                      WalletPrivilegePolicy::ReportCallback onReport,
                      WalletPrivilegePolicy::ErrorCallback onError);

  private:
    TaskPrivilegePolicy() = delete;
};

}  // namespace usapp::db
