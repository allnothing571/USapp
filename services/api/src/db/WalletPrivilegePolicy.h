// =====================================================================
// services/api/src/db/WalletPrivilegePolicy.h
//
// 钱包表的“有效权限矩阵”检查（阶段 1 M2-A；docs/phase1-plan.md 9.5 / 9.6）。
//
// 用途：db_gated 测试现在使用；M2-B 起 API 启动时复用同一实现（本阶段不接入启动流程）。
//
// 检查内容（以 current_user 的身份执行一条只读查询）：
//   * 身份：current_user 必须是 usapp_api；不是超级用户；不属于任何其他角色
//     （pg_auth_members 为空，堵住 SET ROLE 获得非继承角色权限的路径）；
//     usapp_push 必须存在。
//   * 三张钱包表必须存在，且 owner 不是 current_user（owner 隐含拥有全部权限）。
//   * 表级权限（has_table_privilege，计入直接授权、PUBLIC 授权与继承授权）：
//     current_user 必须有 SELECT、INSERT；不得有 UPDATE、DELETE、TRUNCATE、
//     REFERENCES、TRIGGER、MAINTAIN。usapp_push 与 PUBLIC 全部为否。
//   * 任意列权限（has_any_column_privilege）：current_user 的 SELECT / INSERT 为是，
//     UPDATE 只在 wallet_accounts 上为是，REFERENCES 为否；usapp_push 与 PUBLIC 全部为否。
//   * 列级 UPDATE（has_column_privilege，列名在检查时从系统目录枚举，未来新增列自动覆盖）：
//     只有 current_user 在 wallet_accounts.lock_token 上为是，其余一律为否。
//
// 期望值只在本文件的 C++ 代码中定义（expectedFor），SQL 只返回实际值，避免两处各写一份策略。
// 输出只含角色名、表名、列名、权限名与布尔值：从不包含连接串、口令或任何密钥。
// =====================================================================
#pragma once

#include <drogon/orm/Result.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/Db.h"

namespace usapp::db {

// 查询返回的一行实际值。subject 取值：current / usapp_push / public。
struct WalletPrivilegeRow {
    std::string kind;       // identity | table | table_privilege | any_column_privilege | column_privilege
    std::string subject;
    std::string object;     // 表名；identity/is_app_role 行为 current_user 的角色名
    std::string column;     // 仅 column_privilege 行
    std::string privilege;
    std::optional<bool> actual;  // NULL（无法判定）一律视为不符合
};

struct WalletPrivilegeCheck {
    WalletPrivilegeRow row;
    bool expected = false;
    bool ok() const { return row.actual.has_value() && *row.actual == expected; }
};

struct WalletPrivilegeReport {
    std::string currentRole;                  // 来自 identity/is_app_role 行；只是角色名
    std::vector<WalletPrivilegeCheck> checks;
    std::vector<std::string> missing;         // 必须出现却没有返回的检查项（安全名称）
    std::vector<std::string> unexpected;      // 无法识别的返回行（安全名称）

    std::vector<WalletPrivilegeCheck> mismatches() const;
    bool ok() const;
    // 多行摘要：只含安全名称与布尔值。
    std::string summary() const;
};

class WalletPrivilegePolicy {
  public:
    static constexpr const char *kAppRole = "usapp_api";
    static constexpr const char *kPushRole = "usapp_push";
    static constexpr const char *kLockColumnTable = "wallet_accounts";
    static constexpr const char *kLockColumn = "lock_token";

    static const std::vector<std::string> &walletTables();
    static const std::vector<std::string> &subjects();             // current, usapp_push, public
    static const std::vector<std::string> &tablePrivileges();      // 8 种表级权限
    static const std::vector<std::string> &anyColumnPrivileges();  // 4 种列级权限

    // 只读查询；无参数。
    static const char *matrixSql();

    // 某一行的期望值；无法识别的行返回 std::nullopt。
    static std::optional<bool> expectedFor(const WalletPrivilegeRow &row);

    // 纯函数：根据实际值行生成报告（含完整性检查）。
    static WalletPrivilegeReport evaluate(const std::vector<WalletPrivilegeRow> &rows);

    static std::vector<WalletPrivilegeRow> rowsFromResult(const drogon::orm::Result &result);

    // 把名称限制为 [A-Za-z0-9_.-]，其余字符替换为 '?'，保证输出只含安全名称。
    static std::string safeName(const std::string &name);

    using ReportCallback = std::function<void(WalletPrivilegeReport)>;
    using ErrorCallback = std::function<void(const std::string &)>;

    // 在给定客户端上执行矩阵查询。错误描述不含 SQL 参数或连接信息。
    static void check(const ClientPtr &client, ReportCallback onReport, ErrorCallback onError);

  private:
    WalletPrivilegePolicy() = delete;
};

}  // namespace usapp::db
