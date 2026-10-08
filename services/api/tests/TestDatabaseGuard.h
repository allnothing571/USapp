// =====================================================================
// services/api/tests/TestDatabaseGuard.h
//
// 数据库测试的目标库守卫（纯函数，无数据库依赖，便于单元测试）。
//
// 规则：db_gated 只允许在名为 usapp_test 的数据库上运行。判定依据是连接后
// 服务器返回的 current_database()，必须与 kExpectedTestDatabase 完全相等；
// 不做子串、前后缀或大小写宽松匹配，也不依赖连接串内容。
// 不符或无法查询时，调用方必须以失败（非 Skipped）退出，且在任何夹具写入
// 与测试用例之前完成判定。
//
// 输出：只打印经 safeDatabaseName() 处理的库名与原因，从不打印连接串。
// =====================================================================
#pragma once

#include <string>

namespace usapptest {

inline constexpr const char *kExpectedTestDatabase = "usapp_test";

// 精确相等：usapp、usapp_test2、USAPP_TEST、"usapp_test " 等一律不通过。
inline bool isExpectedTestDatabase(const std::string &currentDatabase) {
    return currentDatabase == kExpectedTestDatabase;
}

// 只保留 [A-Za-z0-9_-]，其余替换为 '?'，并截断过长的名称，保证输出安全。
inline std::string safeDatabaseName(const std::string &name) {
    constexpr size_t kMaxLength = 63;  // PostgreSQL 标识符最大长度
    std::string out;
    for (const char c : name) {
        if (out.size() >= kMaxLength) {
            out += "...";
            break;
        }
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '-';
        out.push_back(safe ? c : '?');
    }
    return out.empty() ? std::string("<empty>") : out;
}

}  // namespace usapptest
