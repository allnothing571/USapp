// =====================================================================
// services/api/tests/UnitTest.cc
//
// 目标：usapp_test_unit
// 纯单元测试：安全基线、注册白名单、Cookie 与 Origin 规则、游标分页、
// 错误码映射与锁键派生；AdminFilter 未认证访问行为回归（真实 doFilter）。
//
// 依赖：usapp_core（纯 C++ 逻辑与工具函数，不依赖数据库连接与网络）。
// =====================================================================

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AdminFilterProbe.h"
#include "AnniversaryUnitTests.h"
#include "MemoryUnitTests.h"
#include "DeletionJournalUnitTests.h"
#include "DeletionRecoveryUnitTests.h"
#include "DeletionStartupUnitTests.h"
#include "DeletionJournalBridgeUnitTests.h"
#include "RecoverySourceBridgeUnitTests.h"
#include "AlbumUnitTests.h"
#include "NotificationIntentUnitTests.h"
#include "ProductUnitTests.h"
#include "PushUnitTests.h"
#include "PushDispatchUnitTests.h"
#include "PushWorkerUnitTests.h"
#include "ReminderUnitTests.h"
#include "RedemptionUnitTests.h"
#include "MessageUnitTests.h"
#include "TaskReactionUnitTests.h"
#include "TaskChangeUnitTests.h"
#include "TaskCompletionUnitTests.h"
#include "TaskServiceUnitTests.h"
#include "TestDatabaseGuard.h"
#include "TestSupport.h"
#include "core/Crypto.h"
#include "core/Errors.h"
#include "db/Db.h"
#include "db/TaskPrivilegePolicy.h"
#include "db/WalletPrivilegePolicy.h"
#include "db/WalletStartupCheck.h"
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "scheduler/LockKey.h"
#include "services/ProfileService.h"
#include "services/RegistrationPolicy.h"
#include "services/WalletService.h"

int main() {
    using namespace usapp;

    // -----------------------------------------------------------------
    // 1. 注册白名单策略
    // -----------------------------------------------------------------
    TEST_CASE("RegistrationPolicy: 白名单模式匹配与非白名单拒绝") {
        auto &policy = RegistrationPolicy::instance();
        std::unordered_set<std::string> allowed = {"alice@example.com", "bob@example.com"};
        policy.configure("whitelist", allowed);

        CHECK(policy.isAllowedEmail("alice@example.com"));
        CHECK(policy.isAllowedEmail("bob@example.com"));
        // 邮箱先规范化，再传给只接受 canonicalEmail 的策略接口。
        crypto::CanonicalEmail normalized;
        CHECK(crypto::canonicalizeEmail("ALICE@example.com", normalized));
        CHECK(policy.isAllowedEmail(normalized.canonical));
        CHECK(crypto::canonicalizeEmail("Alice@Example.Com", normalized));
        CHECK(policy.isAllowedEmail(normalized.canonical));

        // 未列入白名单的邮箱必须被拒绝
        CHECK_FALSE(policy.isAllowedEmail("eve@example.com"));
        CHECK_FALSE(policy.isAllowedEmail(""));
        CHECK_FALSE(policy.isAllowedEmail("alice@example.org"));
        CHECK_FALSE(policy.isAllowedEmail("malice@example.com"));
        // 白名单限制新注册，不撤销已有账号的登录资格。
        CHECK_FALSE(policy.mayCreateAccount("eve@example.com", false));
        CHECK(policy.mayCreateAccount("eve@example.com", true));
    }

    // -----------------------------------------------------------------
    // 2. HTTP Origin 精确匹配与 CSRF 方法判定
    // -----------------------------------------------------------------
    TEST_CASE("HttpUtil: Origin 精确比对与 CSRF 方法约束") {
        const std::string expected = "https://localhost:8443";

        // 完全相等才匹配（字符串精确比对）
        CHECK(http::originMatches("https://localhost:8443", expected));
        CHECK_FALSE(http::originMatches("http://localhost:8443", expected));    // 协议不同
        CHECK_FALSE(http::originMatches("https://localhost:8444", expected));   // 端口不同
        CHECK_FALSE(http::originMatches("https://localhost:8443/", expected));  // 带斜杠不同
        CHECK_FALSE(http::originMatches("https://evil.com", expected));         // 域名不同
        CHECK_FALSE(http::originMatches("https://localhost:8443.evil.com", expected)); // 前缀欺骗
        CHECK_FALSE(http::originMatches("", expected));                         // 空 Origin

        // CSRF: 安全方法跳过，状态修改方法必须校验
        CHECK_FALSE(http::requiresCsrf(drogon::Get));
        CHECK_FALSE(http::requiresCsrf(drogon::Head));
        CHECK_FALSE(http::requiresCsrf(drogon::Options));
        CHECK(http::requiresCsrf(drogon::Post));
        CHECK(http::requiresCsrf(drogon::Put));
        CHECK(http::requiresCsrf(drogon::Delete));
        CHECK(http::requiresCsrf(drogon::Patch));

        // 恒定时间字符串比较
        CHECK(crypto::constantTimeEquals("token-abc-123", "token-abc-123"));
        CHECK_FALSE(crypto::constantTimeEquals("token-abc-123", "token-abc-124"));
        CHECK_FALSE(crypto::constantTimeEquals("token-abc-123", "token-abc-12"));
    }

    // -----------------------------------------------------------------
    // 3. CIDR 回环与子网判定
    // -----------------------------------------------------------------
    TEST_CASE("HttpUtil: CIDR 匹配（管理面网络边界）") {
        std::vector<std::string> allowedCidrs = {
            "127.0.0.1/32",
            "::1/128",
            "172.28.0.0/16"
        };

        CHECK(http::ipInAnyCidr("127.0.0.1", allowedCidrs));
        CHECK(http::ipInAnyCidr("::1", allowedCidrs));
        CHECK(http::ipInAnyCidr("172.28.0.1", allowedCidrs));
        CHECK(http::ipInAnyCidr("172.28.255.254", allowedCidrs));

        // 容器子网外或外网 IP 必须拒绝
        CHECK_FALSE(http::ipInAnyCidr("192.168.1.1", allowedCidrs));
        CHECK_FALSE(http::ipInAnyCidr("10.0.0.1", allowedCidrs));
        CHECK_FALSE(http::ipInAnyCidr("172.29.0.1", allowedCidrs));
        CHECK_FALSE(http::ipInAnyCidr("8.8.8.8", allowedCidrs));
    }

    // -----------------------------------------------------------------
    // 4. ControllerSupport 工具与分页游标
    // -----------------------------------------------------------------
    TEST_CASE("ControllerSupport: UUID 校验、游标编解码与分页约束") {
        // UUID 格式检查
        CHECK(ctrl::looksLikeUuid("123e4567-e89b-12d3-a456-426614174000"));
        CHECK(ctrl::looksLikeUuid("E9067B8A-B42F-4952-B108-9DFCB67CBBE8"));
        CHECK_FALSE(ctrl::looksLikeUuid("not-a-uuid"));
        CHECK_FALSE(ctrl::looksLikeUuid("123e4567-e89b-12d3-a456-42661417400"));   // 长度不足
        CHECK_FALSE(ctrl::looksLikeUuid("123e4567-e89b-12d3-a456-4266141740000"));  // 长度超长
        CHECK_FALSE(ctrl::looksLikeUuid("123e4567_e89b_12d3_a456_426614174000"));  // 分隔符错误

        // 游标编解码
        CHECK_EQ(ctrl::encodeCursor(0), "off-0");
        CHECK_EQ(ctrl::encodeCursor(50), "off-50");

        long long offset = -1;
        CHECK(ctrl::decodeCursor("", offset));
        CHECK_EQ(offset, 0);

        CHECK(ctrl::decodeCursor("off-50", offset));
        CHECK_EQ(offset, 50);

        CHECK_FALSE(ctrl::decodeCursor("invalid", offset));
        CHECK_FALSE(ctrl::decodeCursor("off-abc", offset));
        CHECK_FALSE(ctrl::decodeCursor("cursor-50", offset));

        // 下一页游标
        CHECK(ctrl::nextCursorOrNull(50, 50, 0).isString());
        CHECK_EQ(ctrl::nextCursorOrNull(50, 50, 0).asString(), "off-50");
        CHECK(ctrl::nextCursorOrNull(30, 50, 0).isNull()); // 少于 limit 表明已是末页
    }

    // -----------------------------------------------------------------
    // 5. 锁键确定性派生与密码学
    // -----------------------------------------------------------------
    TEST_CASE("LockKey & Crypto: 跨实例确定性派生与邮箱规范化") {
        // 同一 namespace + jobKey 派生结果必须恒定
        const int64_t key1 = scheduler::LockKey::make("usapp.scheduler", "heartbeat").value();
        const int64_t key2 = scheduler::LockKey::make("usapp.scheduler", "heartbeat").value();
        CHECK_EQ(key1, key2);
        CHECK(key1 > 0); // 必须是正 64 位整数

        // 命名空间隔离：不同命名空间下的同名 job 必须互不干扰
        const int64_t keyOtherNs = scheduler::LockKey::make("other.ns", "heartbeat").value();
        CHECK_NE(key1, keyOtherNs);

        // 非法 jobKey 抛异常
        CHECK_THROWS_AS(scheduler::LockKey::make("usapp.scheduler", ""), std::invalid_argument);
        CHECK_THROWS_AS(scheduler::LockKey::make("usapp.scheduler", "Heartbeat"), std::invalid_argument); // 必须小写
        CHECK_THROWS_AS(scheduler::LockKey::make("usapp:colon", "heartbeat"), std::invalid_argument);     // namespace 不得含冒号

        // 邮箱规范化
        crypto::CanonicalEmail canon;
        CHECK(crypto::canonicalizeEmail("  Test.User+Tag@Example.COM  ", canon));
        CHECK_EQ(canon.canonical, "test.user+tag@example.com");
        CHECK_FALSE(crypto::canonicalizeEmail("invalid-email-no-at", canon));
    }

    // -----------------------------------------------------------------
    // 5b. 昵称校验（阶段 1 M1）：按 Unicode 码点计数，首尾空白裁剪，
    //     控制字符 / 换行 / 非法 UTF-8 一律拒绝。纯函数，无数据库。
    // -----------------------------------------------------------------
    TEST_CASE("ProfileService: 昵称校验与规范化（码点计数、裁剪、拒绝控制字符与非法 UTF-8）") {
        using E = ProfileService::DisplayNameError;
        auto repeat = [](const std::string &unit, int n) {
            std::string out;
            for (int i = 0; i < n; ++i) {
                out += unit;
            }
            return out;
        };
        auto check = [](const std::string &raw, E expected, const std::string &expectedOut) {
            std::string out = "sentinel";
            const E actual = ProfileService::normalizeDisplayName(raw, out);
            CHECK(actual == expected);
            CHECK_EQ(out, expectedOut);  // 失败时 out 必须被清空
        };

        const std::string han = "\xE5\x90\x8D";            // U+540D，3 字节，1 个码点
        const std::string emoji = "\xF0\x9F\x98\x80";      // U+1F600，4 字节，UTF-16 下 2 个码元
        const std::string ideoSpace = "\xE3\x80\x80";      // U+3000 表意空格
        const std::string nbsp = "\xC2\xA0";               // U+00A0

        // ASCII 与边界：1 与 20 个码点通过，21 个拒绝。
        check("A", E::None, "A");
        check("Alice", E::None, "Alice");
        check(std::string(20, 'a'), E::None, std::string(20, 'a'));
        check(std::string(21, 'a'), E::TooLong, "");

        // 中文：20 个汉字是 60 字节，仍按 20 个码点计。
        check(repeat(han, 20), E::None, repeat(han, 20));
        check(repeat(han, 21), E::TooLong, "");

        // Emoji：20 个是 80 字节 / 40 个 UTF-16 码元，仍按 20 个码点计。
        check(repeat(emoji, 20), E::None, repeat(emoji, 20));
        check(repeat(emoji, 21), E::TooLong, "");
        check(repeat(emoji, 10) + repeat(han, 10), E::None, repeat(emoji, 10) + repeat(han, 10));

        // 首尾空白裁剪；内部空白保留；裁剪后再计数。
        check("  Alice  ", E::None, "Alice");
        check(ideoSpace + han + han + ideoSpace, E::None, han + han);
        check(nbsp + "Bob" + nbsp, E::None, "Bob");
        check("Al ice", E::None, "Al ice");
        check("  " + std::string(20, 'a') + "  ", E::None, std::string(20, 'a'));
        check("  " + std::string(21, 'a') + "  ", E::TooLong, "");

        // 空输入与纯空白。
        check("", E::Empty, "");
        check("   ", E::Empty, "");
        check(ideoSpace + nbsp + " ", E::Empty, "");

        // 控制字符（任何位置）：C0、NUL、DEL、C1，以及制表符。
        check("A\x01" "B", E::ControlCharacter, "");
        check(std::string("A\0B", 3), E::ControlCharacter, "");
        check("A\x7F", E::ControlCharacter, "");
        check("A\xC2\x9F" "B", E::ControlCharacter, "");  // U+009F（C1）
        check("A\tB", E::ControlCharacter, "");
        check("\tAlice", E::ControlCharacter, "");

        // 换行（任何位置，含首尾——不会被当作空白裁掉）。
        check("Alice\n", E::ControlCharacter, "");
        check("\nAlice", E::ControlCharacter, "");
        check("A\r\nB", E::ControlCharacter, "");
        check("A\rB", E::ControlCharacter, "");
        check("A\xC2\x85" "B", E::ControlCharacter, "");      // U+0085 NEL
        check("A\xE2\x80\xA8" "B", E::ControlCharacter, "");  // U+2028 行分隔符
        check("A\xE2\x80\xA9" "B", E::ControlCharacter, "");  // U+2029 段分隔符

        // 非法 UTF-8。
        check("\xFF", E::InvalidUtf8, "");
        check("\x80", E::InvalidUtf8, "");                 // 孤立续字节
        check("A\xC3", E::InvalidUtf8, "");                // 截断的 2 字节序列
        check("A\xE5\x90", E::InvalidUtf8, "");            // 截断的 3 字节序列
        check("\xC0\xAF", E::InvalidUtf8, "");             // 过长编码（2 字节）
        check("\xE0\x80\xAF", E::InvalidUtf8, "");         // 过长编码（3 字节）
        check("\xF0\x80\x80\xAF", E::InvalidUtf8, "");     // 过长编码（4 字节）
        check("\xED\xA0\x80", E::InvalidUtf8, "");         // UTF-16 代理项 U+D800
        check("\xF4\x90\x80\x80", E::InvalidUtf8, "");     // 超出 U+10FFFF
        check("A\xE5\x41\x8D", E::InvalidUtf8, "");        // 续字节位置不是续字节

        // 超大原始输入不做逐字符解码，直接判为过长。
        check(std::string(ProfileService::kDisplayNameMaxRawBytes + 1, ' '), E::TooLong, "");

        // 提示文案存在且不回显输入。
        CHECK(std::string(ProfileService::displayNameErrorMessage(E::TooLong)).find("20") !=
              std::string::npos);
        CHECK_FALSE(std::string(ProfileService::displayNameErrorMessage(E::Empty)).empty());
        CHECK_FALSE(std::string(ProfileService::displayNameErrorMessage(E::ControlCharacter)).empty());
        CHECK_FALSE(std::string(ProfileService::displayNameErrorMessage(E::InvalidUtf8)).empty());
    }

    // -----------------------------------------------------------------
    // 5b2. db_gated 目标库守卫（纯函数）：只有精确等于 usapp_test 才放行。
    //      不对主库运行任何测试来验证拒绝路径；拒绝路径由这里的纯函数断言覆盖。
    // -----------------------------------------------------------------
    TEST_CASE("TestDatabaseGuard: current_database() 必须精确等于 usapp_test") {
        CHECK(usapptest::isExpectedTestDatabase("usapp_test"));
        for (const std::string name : {"usapp", "usapp_test2", "usapp_testx", "USAPP_TEST", "Usapp_test",
                                       "usapp_test ", " usapp_test", "prod_usapp_test", "usapp_tes", "",
                                       "postgres", "usapp_recovery"}) {
            CHECK_FALSE(usapptest::isExpectedTestDatabase(name));
        }
        CHECK_FALSE(usapptest::isExpectedTestDatabase(std::string("usapp_test\0x", 12)));
        // 输出只含安全字符，空名有占位，过长名被截断。
        CHECK_EQ(usapptest::safeDatabaseName("usapp"), std::string("usapp"));
        CHECK_EQ(usapptest::safeDatabaseName("a b'c;d"), std::string("a?b?c?d"));
        CHECK_EQ(usapptest::safeDatabaseName(""), std::string("<empty>"));
        CHECK_EQ(usapptest::safeDatabaseName(std::string(100, 'x')).size(), 66u);
    }

    // -----------------------------------------------------------------
    // 5c. 钱包有效权限矩阵的期望值与报告（阶段 1 M2-A）：纯函数，无数据库。
    //     真实数据库上的检查见 DbGatedTest.cc / WalletLedgerDbTests.h。
    // -----------------------------------------------------------------
    TEST_CASE("WalletPrivilegePolicy: 期望值、完整性、NULL 与安全输出") {
        using usapp::db::WalletPrivilegePolicy;
        using usapp::db::WalletPrivilegeRow;
        using P = WalletPrivilegePolicy;

        auto row = [](const std::string &kind, const std::string &subject, const std::string &object,
                      const std::string &column, const std::string &privilege, std::optional<bool> actual) {
            WalletPrivilegeRow r;
            r.kind = kind;
            r.subject = subject;
            r.object = object;
            r.column = column;
            r.privilege = privilege;
            r.actual = actual;
            return r;
        };

        // 期望值：只有 current 在三张表上有 SELECT / INSERT；UPDATE 只在 wallet_accounts.lock_token。
        CHECK(P::expectedFor(row("table_privilege", "current", "wallet_ledger_entries", "", "SELECT", {})) == true);
        CHECK(P::expectedFor(row("table_privilege", "current", "wallet_ledger_entries", "", "INSERT", {})) == true);
        for (const std::string priv : {"UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER", "MAINTAIN"}) {
            CHECK(P::expectedFor(row("table_privilege", "current", "wallet_accounts", "", priv, {})) == false);
        }
        CHECK(P::expectedFor(row("table_privilege", "public", "wallet_accounts", "", "SELECT", {})) == false);
        CHECK(P::expectedFor(row("table_privilege", "usapp_push", "wallet_operations", "", "INSERT", {})) == false);
        CHECK(P::expectedFor(row("any_column_privilege", "current", "wallet_accounts", "", "UPDATE", {})) == true);
        CHECK(P::expectedFor(row("any_column_privilege", "current", "wallet_ledger_entries", "", "UPDATE", {})) == false);
        CHECK(P::expectedFor(row("any_column_privilege", "current", "wallet_accounts", "", "REFERENCES", {})) == false);
        CHECK(P::expectedFor(row("column_privilege", "current", "wallet_accounts", "lock_token", "UPDATE", {})) == true);
        CHECK(P::expectedFor(row("column_privilege", "current", "wallet_accounts", "couple_id", "UPDATE", {})) == false);
        CHECK(P::expectedFor(row("column_privilege", "public", "wallet_accounts", "lock_token", "UPDATE", {})) == false);
        CHECK(P::expectedFor(row("identity", "current", "usapp_api", "", "is_app_role", {})) == true);
        CHECK(P::expectedFor(row("identity", "current", "", "", "rolsuper", {})) == false);
        CHECK(P::expectedFor(row("identity", "current", "", "", "has_role_memberships", {})) == false);
        CHECK(P::expectedFor(row("table", "current", "wallet_operations", "", "owned_by_current_user", {})) == false);
        // 无法识别的行没有期望值。
        CHECK_FALSE(P::expectedFor(row("table_privilege", "current", "users", "", "SELECT", {})).has_value());
        CHECK_FALSE(P::expectedFor(row("table_privilege", "someone", "wallet_accounts", "", "SELECT", {})).has_value());

        // 完全符合的结果集：用期望值作为实际值构造。
        std::vector<WalletPrivilegeRow> rows;
        auto add = [&](WalletPrivilegeRow r) {
            r.actual = P::expectedFor(r);
            rows.push_back(r);
        };
        add(row("identity", "current", "usapp_api", "", "is_app_role", {}));
        add(row("identity", "current", "", "", "rolsuper", {}));
        add(row("identity", "current", "", "", "has_role_memberships", {}));
        add(row("identity", "usapp_push", "", "", "role_exists", {}));
        const std::vector<std::pair<std::string, std::vector<std::string>>> columns = {
            {"wallet_accounts", {"user_id", "asset_type", "couple_id", "lock_token", "created_at"}},
            {"wallet_operations", {"id", "couple_id", "actor_user_id", "source_id"}},
            {"wallet_ledger_entries", {"id", "operation_id", "amount", "balance_after"}},
        };
        for (const auto &table : P::walletTables()) {
            add(row("table", "current", table, "", "exists", {}));
            add(row("table", "current", table, "", "owned_by_current_user", {}));
            for (const auto &subject : P::subjects()) {
                for (const auto &priv : P::tablePrivileges()) add(row("table_privilege", subject, table, "", priv, {}));
                for (const auto &priv : P::anyColumnPrivileges()) {
                    add(row("any_column_privilege", subject, table, "", priv, {}));
                }
            }
        }
        for (const auto &entry : columns) {
            for (const auto &column : entry.second) {
                for (const auto &subject : P::subjects()) {
                    add(row("column_privilege", subject, entry.first, column, "UPDATE", {}));
                }
            }
        }
        const auto good = P::evaluate(rows);
        CHECK(good.ok());
        CHECK(db::walletStartupAllowed(good));
        CHECK((db::WalletStartupResult{db::WalletStartupResult::State::Report, good}).allowed());
        CHECK_FALSE((db::WalletStartupResult{db::WalletStartupResult::State::QueryError, good}).allowed());
        CHECK_FALSE((db::WalletStartupResult{db::WalletStartupResult::State::Timeout, good}).allowed());
        CHECK_FALSE(db::walletStartupAllowed(std::nullopt));
        CHECK_FALSE(db::walletStartupAllowed(db::WalletPrivilegeReport{}));
        CHECK_EQ(good.currentRole, std::string("usapp_api"));
        CHECK(good.mismatches().empty());
        CHECK(good.missing.empty());

        // 多出的 UPDATE 权限（例如 init-roles 重新放宽）必须被发现。
        auto widened = rows;
        for (auto &r : widened) {
            if (r.kind == "table_privilege" && r.subject == "current" && r.object == "wallet_ledger_entries" &&
                r.privilege == "UPDATE") {
                r.actual = true;
            }
        }
        const auto widenedReport = P::evaluate(widened);
        CHECK_FALSE(widenedReport.ok());
        CHECK_FALSE(db::walletStartupAllowed(widenedReport));
        CHECK_EQ(widenedReport.mismatches().size(), 1u);

        // 其他列被授予 UPDATE 也必须被发现。
        auto columnWidened = rows;
        for (auto &r : columnWidened) {
            if (r.kind == "column_privilege" && r.subject == "current" && r.column == "couple_id" &&
                r.object == "wallet_accounts") {
                r.actual = true;
            }
        }
        CHECK_FALSE(P::evaluate(columnWidened).ok());
        CHECK_FALSE(db::walletStartupAllowed(P::evaluate(columnWidened)));

        // NULL（无法判定，例如缺表）视为不符合。
        auto withNull = rows;
        withNull[0].actual.reset();
        CHECK_FALSE(P::evaluate(withNull).ok());
        CHECK_FALSE(db::walletStartupAllowed(P::evaluate(withNull)));

        // 缺少必需的检查项。
        auto missingLock = rows;
        missingLock.erase(std::remove_if(missingLock.begin(), missingLock.end(),
                                         [](const WalletPrivilegeRow &r) {
                                             return r.kind == "column_privilege" && r.column == "lock_token" &&
                                                    r.subject == "current";
                                         }),
                          missingLock.end());
        const auto missingReport = P::evaluate(missingLock);
        CHECK_FALSE(db::walletStartupAllowed(missingReport));
        CHECK_FALSE(missingReport.ok());
        CHECK_EQ(missingReport.missing.size(), 1u);

        // 无法识别的行。
        auto extra = rows;
        extra.push_back(row("table_privilege", "current", "users", "", "SELECT", true));
        CHECK_FALSE(P::evaluate(extra).ok());
        CHECK_FALSE(db::walletStartupAllowed(P::evaluate(extra)));

        // 身份错误：不是 usapp_api。
        auto wrongRole = rows;
        wrongRole[0].object = "postgres";
        wrongRole[0].actual = false;
        const auto wrongReport = P::evaluate(wrongRole);
        CHECK_FALSE(db::walletStartupAllowed(wrongReport));
        CHECK_FALSE(wrongReport.ok());
        CHECK_EQ(wrongReport.currentRole, std::string("postgres"));

        // 输出只含安全字符。
        CHECK_EQ(P::safeName("a b;c'd"), std::string("a?b?c?d"));
        const std::string summary = widenedReport.summary();
        CHECK_CONTAINS(summary, "mismatches=1");
        CHECK_CONTAINS(summary, "wallet_ledger_entries");
        CHECK_NOT_CONTAINS(summary, "postgresql://");
        CHECK_NOT_CONTAINS(summary, "password");
    }

    // -----------------------------------------------------------------
    // 5d. 任务表有效权限矩阵（阶段 1 M3-A）：纯函数，无数据库。
    //     真实数据库上的检查见 DbGatedTest.cc / TaskLedgerDbTests.h。
    // -----------------------------------------------------------------
    TEST_CASE("TaskPrivilegePolicy: 只允许 SELECT/INSERT，任何列都不得 UPDATE，缺项与 NULL 不通过") {
        using usapp::db::TaskPrivilegePolicy;
        using usapp::db::WalletPrivilegePolicy;
        using usapp::db::WalletPrivilegeRow;
        using T = TaskPrivilegePolicy;

        auto row = [](const std::string &kind, const std::string &subject, const std::string &object,
                      const std::string &column, const std::string &privilege, std::optional<bool> actual) {
            WalletPrivilegeRow r;
            r.kind = kind;
            r.subject = subject;
            r.object = object;
            r.column = column;
            r.privilege = privilege;
            r.actual = actual;
            return r;
        };

        CHECK(T::expectedFor(row("table_privilege", "current", "tasks", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("table_privilege", "current", "task_completions", "", "INSERT", {})) == true);
        for (const std::string priv : {"UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER", "MAINTAIN"}) {
            CHECK(T::expectedFor(row("table_privilege", "current", "tasks", "", priv, {})) == false);
        }
        CHECK(T::expectedFor(row("table_privilege", "public", "tasks", "", "SELECT", {})) == false);
        CHECK(T::expectedFor(row("table_privilege", "usapp_push", "task_completions", "", "SELECT", {})) == false);
        // 与钱包不同：任务表上任何列都没有 UPDATE（包括任意列汇总）。
        CHECK(T::expectedFor(row("any_column_privilege", "current", "tasks", "", "UPDATE", {})) == false);
        CHECK(T::expectedFor(row("any_column_privilege", "current", "tasks", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("table_privilege", "current", "task_read_model", "", "INSERT", {})) == false);
        CHECK(T::expectedFor(row("table_privilege", "current", "task_read_model", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("identity", "usapp_memory_eraser", "", "", "no_violations", {})) == true);
        CHECK(T::expectedFor(row("identity", "timestamp_guards", "", "", "no_column_defaults", {})) == true);
        CHECK(T::expectedFor(row("identity", "phase3_notifications", "", "", "sources_present", {})) == true);
        CHECK_FALSE(T::expectedFor(row("identity", "usapp_memory_eraser", "", "", "violation_superuser", {})).has_value());
        CHECK(T::expectedFor(row("table_privilege", "current", "memory_purges", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("table_privilege", "current", "memory_purges", "", "INSERT", {})) == false);
        // Phase 4 B1: the avatar asset registry is read-only to the application; the catalog tables take INSERT.
        CHECK(T::expectedFor(row("table_privilege", "current", "avatar_assets", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("table_privilege", "current", "avatar_assets", "", "INSERT", {})) == false);
        CHECK(T::expectedFor(row("any_column_privilege", "current", "avatar_assets", "", "INSERT", {})) == false);
        CHECK(T::expectedFor(row("table_privilege", "current", "achievement_activation", "", "SELECT", {})) == true);
        CHECK(T::expectedFor(row("table_privilege", "current", "achievement_activation", "", "INSERT", {})) == false);
        CHECK(T::expectedFor(row("any_column_privilege", "current", "achievement_activation", "", "INSERT", {})) == false);
        for (const std::string table : {"avatar_items", "avatar_item_revisions", "avatar_starter_configs"}) {
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "INSERT", {})) == true);
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "UPDATE", {})) == false);
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "DELETE", {})) == false);
            CHECK(T::expectedFor(row("table_privilege", "usapp_push", table, "", "SELECT", {})) == false);
        }
        CHECK(T::expectedFor(row("table_privilege", "current", "memory_purges", "", "UPDATE", {})) == false);
        CHECK(T::expectedFor(row("any_column_privilege", "current", "memory_purges", "", "INSERT", {})) == false);
        CHECK(T::expectedFor(row("column_privilege", "current", "tasks", "id", "UPDATE", {})) == false);
        CHECK(T::expectedFor(row("column_privilege", "current", "task_completions", "completed_at", "UPDATE", {})) ==
              false);
        CHECK(T::expectedFor(row("identity", "current", "usapp_api", "", "is_app_role", {})) == true);
        // M6: change proposals, decisions and closures are append-only (SELECT/INSERT only).
        for (const std::string table : {"task_change_proposals", "task_change_decisions", "task_closures",
                                        "reward_products", "reward_product_revisions",
                                        "reward_redemptions", "reward_redemption_fulfillments", "reward_redemption_events",
                                        "messages", "message_replies", "anniversaries", "anniversary_revisions",
                                        "anniversary_edit_proposals", "anniversary_edit_decisions",
                                        "anniversary_home_pins", "notifications", "notification_reads",
                                        "notification_intents", "task_reactions", "push_subscriptions",
                                        "push_subscription_revocations", "push_registration_commands",
                                        "push_dispatch_control", "push_deliveries", "push_attempts",
                                        "push_attempt_observations", "push_attempt_expiries",
                                        "push_delivery_outcomes", "reminder_rules", "reminder_occurrences",
                                        "media_objects", "media_promotion_attempts", "media_upload_outcomes",
                                        "media_cleanup_plans", "media_cleanup_passes", "media_bindings",
                                        "media_late_objects", "memories", "memory_texts", "memory_revisions",
                                        "memory_revision_photos", "memory_edit_proposals", "memory_proposal_photos",
                                        "memory_edit_decisions", "albums", "album_texts", "album_revisions",
                                        "album_photo_batches", "album_items", "album_item_removals", "album_cover_events",
                                        "album_edit_proposals", "album_edit_decisions", "shared_plans", "shared_plan_events", "shared_plan_memory_links",
                                        "media_deletions", "media_deletion_reversals", "memory_deletions", "memory_deletion_reversals",
                                        "memory_delete_requests", "memory_delete_decisions", "memory_deletion_photo_cycles"}) {
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "INSERT", {})) == true);
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "UPDATE", {})) == false);
            CHECK(T::expectedFor(row("table_privilege", "current", table, "", "DELETE", {})) == false);
        }
        // 钱包表的行不属于任务矩阵。
        CHECK_FALSE(T::expectedFor(row("table_privilege", "current", "wallet_accounts", "", "SELECT", {})).has_value());

        std::vector<WalletPrivilegeRow> rows;
        auto add = [&](WalletPrivilegeRow r) {
            r.actual = T::expectedFor(r);
            rows.push_back(r);
        };
        add(row("identity", "current", "usapp_api", "", "is_app_role", {}));
        add(row("identity", "current", "", "", "rolsuper", {}));
        add(row("identity", "current", "", "", "has_role_memberships", {}));
        add(row("identity", "usapp_push", "", "", "role_exists", {}));
        add(row("identity", "usapp_memory_eraser", "", "", "no_violations", {}));
        add(row("identity", "timestamp_guards", "", "", "no_column_defaults", {}));
        add(row("identity", "phase3_notifications", "", "", "sources_present", {}));
        const std::vector<std::pair<std::string, std::vector<std::string>>> columns = {
            {"tasks", {"id", "couple_id", "title", "deadline_at"}},
            {"task_completions", {"task_id", "operation_id", "completed_at"}},
            {"task_progress_events", {"task_id", "id", "progress_count"}},
            {"task_read_model", {"id", "period_date", "current_period"}},
            {"task_change_proposals", {"id", "task_id", "kind"}},
            {"task_change_decisions", {"proposal_id", "outcome"}},
            {"task_closures", {"task_id", "closed_at"}},
            {"reward_products", {"id", "owner_id"}},
            {"reward_product_revisions", {"product_id", "price_flowers"}},
            {"reward_redemptions", {"product_id", "operation_id"}},
            {"reward_redemption_fulfillments", {"redemption_id", "fulfilled_by"}},
            {"reward_redemption_events", {"redemption_id", "actor_id"}},
            {"reward_redemption_state", {"id", "status"}},
            {"messages", {"id", "body"}},
            {"message_replies", {"message_id", "body"}},
            {"anniversaries", {"id", "creator_id"}},
            {"anniversary_revisions", {"anniversary_id", "event_date"}},
            {"anniversary_edit_proposals", {"id", "base_version"}},
            {"anniversary_edit_decisions", {"proposal_id", "outcome"}},
            {"anniversary_home_pins", {"user_id", "seq"}},
            {"notifications", {"id", "recipient_id"}},
            {"notification_reads", {"notification_id", "recipient_id"}},
            {"task_reactions", {"id", "completion_task_id"}},
            {"notification_intents", {"id", "preference_seq"}},
            {"push_subscriptions", {"id", "endpoint_hash"}},
            {"push_subscription_revocations", {"subscription_id", "reason"}},
            {"push_registration_commands", {"user_id", "idempotency_key"}},
            {"push_dispatch_control", {"revision", "kind"}},
            {"push_deliveries", {"notification_id", "generation_id"}},
            {"push_attempts", {"notification_id", "attempt_no"}},
            {"push_attempt_observations", {"notification_id", "outcome"}},
            {"push_attempt_expiries", {"notification_id", "expired_at"}},
            {"push_delivery_outcomes", {"notification_id", "final_state"}},
            {"reminder_rules", {"id", "owner_id"}},
            {"reminder_occurrences", {"id", "owner_id"}},
            {"media_objects", {"id", "staging_key"}},
            {"media_promotion_attempts", {"media_id", "final_key"}},
            {"media_upload_outcomes", {"media_id", "outcome"}},
            {"media_cleanup_plans", {"media_id", "scope"}},
            {"media_cleanup_passes", {"media_id", "kind"}},
            {"media_bindings", {"media_id", "target_id"}},
            {"media_late_objects", {"object_key", "disposition"}},
            {"memories", {"id", "creator_id"}},
            {"memory_texts", {"id", "title"}},
            {"memory_revisions", {"memory_id", "version"}},
            {"memory_revision_photos", {"memory_id", "media_id"}},
            {"memory_edit_proposals", {"id", "base_version"}},
            {"memory_proposal_photos", {"proposal_id", "media_id"}},
            {"memory_edit_decisions", {"proposal_id", "outcome"}},
            {"albums", {"id", "creator_id"}},
            {"album_texts", {"id", "name"}},
            {"album_revisions", {"album_id", "version"}},
            {"album_photo_batches", {"id", "album_id"}},
            {"album_items", {"album_id", "media_id"}},
            {"album_item_removals", {"album_id", "media_id"}},
            {"album_cover_events", {"album_id", "media_id"}},
            {"album_edit_proposals", {"id", "base_version"}},
            {"album_edit_decisions", {"proposal_id", "outcome"}},
            {"shared_plans", {"id"}}, {"shared_plan_events", {"id"}}, {"shared_plan_memory_links", {"plan_id"}},
            {"media_deletions", {"media_id", "purge_after"}}, {"media_deletion_reversals", {"media_id", "deletion_id"}},
            {"memory_deletions", {"memory_id", "purge_after"}}, {"memory_deletion_reversals", {"memory_id", "deletion_id"}},
            {"memory_delete_requests", {"memory_id", "requester_id"}}, {"memory_delete_decisions", {"memory_id", "deletion_id"}},
            {"memory_deletion_photo_cycles", {"memory_id", "media_deletion_id"}},
            {"memory_purges", {"memory_id", "deletion_id"}},
            {"deletion_recovery_attempts", {"generation", "database_id"}},
            {"deletion_recovery_manifests", {"generation", "payload"}},
            {"deletion_recovery_quarantine", {"generation", "target_id"}},
            {"deletion_recovery_verifications", {"generation", "head_hash"}},
            {"avatar_assets", {"asset_key", "layers"}}, {"avatar_items", {"id", "family"}},
            {"avatar_item_revisions", {"item_id", "price_sunshine"}}, {"avatar_starter_configs", {"version", "hair_item_id"}},
            {"avatar_purchases", {"id", "price_sunshine"}},
            {"avatar_loadout_events", {"id", "hair_item_id"}},
            {"achievement_activation", {"singleton", "enabled_at"}},
            {"admin_wallet_adjustments", {"id", "admin_id"}},
            {"admin_redemption_cancellations", {"id", "admin_id"}},
            {"account_login_events", {"id", "user_id"}},
        };
        for (const auto &table : T::taskTables()) {
            add(row("table", "current", table, "", "exists", {}));
            add(row("table", "current", table, "", "owned_by_current_user", {}));
            for (const auto &subject : WalletPrivilegePolicy::subjects()) {
                for (const auto &priv : WalletPrivilegePolicy::tablePrivileges()) {
                    add(row("table_privilege", subject, table, "", priv, {}));
                }
                for (const auto &priv : WalletPrivilegePolicy::anyColumnPrivileges()) {
                    add(row("any_column_privilege", subject, table, "", priv, {}));
                }
            }
        }
        for (const auto &entry : columns) {
            for (const auto &column : entry.second) {
                for (const auto &subject : WalletPrivilegePolicy::subjects()) {
                    add(row("column_privilege", subject, entry.first, column, "UPDATE", {}));
                }
            }
        }
        const auto good = T::evaluate(rows);
        CHECK(good.ok());
        CHECK(good.missing.empty());
        CHECK_EQ(good.currentRole, std::string("usapp_api"));

        // 重新执行 init-roles 若放宽为 UPDATE，必须被发现。
        auto widened = rows;
        for (auto &r : widened) {
            if (r.kind == "table_privilege" && r.subject == "current" && r.object == "tasks" &&
                r.privilege == "UPDATE") {
                r.actual = true;
            }
        }
        const auto widenedReport = T::evaluate(widened);
        CHECK_FALSE(widenedReport.ok());
        CHECK_EQ(widenedReport.mismatches().size(), 1u);

        // 任一列被授予 UPDATE 也必须被发现。
        auto columnWidened = rows;
        for (auto &r : columnWidened) {
            if (r.kind == "column_privilege" && r.subject == "current" && r.column == "completed_at") {
                r.actual = true;
            }
        }
        CHECK_FALSE(T::evaluate(columnWidened).ok());

        // NULL 与缺项都不通过。
        auto withNull = rows;
        withNull[1].actual.reset();
        CHECK_FALSE(T::evaluate(withNull).ok());
        // 两张表都有 task_id；逐表删除，分别证明缺少必需列检查时会拒绝。
        for (const std::string table : {"task_completions", "task_progress_events"}) {
            auto missingColumn = rows;
            missingColumn.erase(std::remove_if(missingColumn.begin(), missingColumn.end(),
                                               [&table](const WalletPrivilegeRow &r) {
                                                   return r.kind == "column_privilege" && r.object == table &&
                                                          r.column == "task_id";
                                               }),
                                missingColumn.end());
            const auto missingReport = T::evaluate(missingColumn);
            CHECK_FALSE(missingReport.ok());
            CHECK_EQ(missingReport.missing.size(), 3u);  // 本表三个主体各缺一项
            for (const std::string subject : {"current", "usapp_push", "public"}) {
                const auto expected = "column_privilege/" + subject + "/" + table + ".task_id/UPDATE";
                CHECK(std::find(missingReport.missing.begin(), missingReport.missing.end(), expected) !=
                      missingReport.missing.end());
            }
        }

        const std::string summary = T::summary(widenedReport);
        CHECK_CONTAINS(summary, "task privilege policy:");
        CHECK_CONTAINS(summary, "mismatches=1");
        CHECK_NOT_CONTAINS(summary, "postgresql://");
        CHECK_NOT_CONTAINS(summary, "password");
    }

    // -----------------------------------------------------------------
    // 6-9. AdminFilter 行为回归（真实 doFilter，无数据库）
    //
    // 回归对象：未登录即可 GET /api/admin/v1/users 等列表并拿到 200 + 数据。
    // 本进程从不初始化数据库连接池；带 Cookie 的用例因此覆盖"会话无法加载
    // 时 fail closed"。真实会话（有效 / 过期 / 撤销）见 DbGatedTest.cc。
    // -----------------------------------------------------------------
    {
        namespace af = usapptest::adminfilter;
        const std::string peerCidr = af::probePeerCidr();
        // 只把测试请求的精确默认对端地址加入白名单，作用域结束即恢复原配置。
        af::ScopedAdminConfig adminConfig(std::vector<std::string>{peerCidr});

        TEST_CASE("AdminFilter: 无 Cookie 访问三个管理列表返回 401 且不进入控制器") {
            CHECK_FALSE(peerCidr.empty());
            for (const auto &path : af::protectedListPaths()) {
                auto req = af::adminRequest(drogon::Get, path);
                const auto result = af::runAdminFilter(req);
                CHECK_ADMIN_REJECTED(result, 401, "UNAUTHENTICATED", af::describe(drogon::Get, path));
                CHECK_FALSE(adminctx::isAuthenticated(req));
            }
        }

        TEST_CASE("AdminFilter: 未认证白名单仅放行 health / bootstrap / login，且不绕过来源校验") {
            // health 与 bootstrap：无 Cookie 也放行，且不附加任何会话属性。
            {
                auto req = af::adminRequest(drogon::Get, af::kHealthPath);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "GET health");
                CHECK_FALSE(adminctx::isAuthenticated(req));
            }
            {
                auto req = af::adminRequest(drogon::Get, af::kBootstrapPath);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "GET session (bootstrap)");
                CHECK_FALSE(adminctx::isAuthenticated(req));
                CHECK(adminctx::sessionId(req).empty());
            }

            // 登录：精确 Origin + X-Requested-By 时放行。
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath);
                af::addWriteHeaders(req);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "POST sessions (login, 完整来源头)");
                CHECK_FALSE(adminctx::isAuthenticated(req));
            }
            // 登录缺 Origin -> 403 ORIGIN_NOT_ALLOWED
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath);
                af::setRequestedBy(req, adminctx::kRequestedByValue);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED", "login 缺 Origin");
            }
            // 登录 Origin 前缀欺骗 -> 403 ORIGIN_NOT_ALLOWED
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath);
                af::setOrigin(req, std::string(af::kTestAdminOrigin) + ".evil.example");
                af::setRequestedBy(req, adminctx::kRequestedByValue);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED", "login Origin 前缀欺骗");
            }
            // 登录缺 X-Requested-By -> 403 CSRF_TOKEN_INVALID
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath);
                af::setOrigin(req, af::kTestAdminOrigin);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID", "login 缺 X-Requested-By");
            }
            // 登录 X-Requested-By 值错误 -> 403 CSRF_TOKEN_INVALID
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath);
                af::setOrigin(req, af::kTestAdminOrigin);
                af::setRequestedBy(req, "usapp-admin-x");
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID", "login X-Requested-By 错误");
            }

            // 缺管理面入口标记：白名单路由同样 403。
            {
                auto req = af::adminRequest(drogon::Get, af::kHealthPath, false);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED", "health 缺入口标记");
            }
            {
                auto req = af::adminRequest(drogon::Get, af::kBootstrapPath, false);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED", "bootstrap 缺入口标记");
            }
            {
                auto req = af::adminRequest(drogon::Post, af::kSessionsPath, false);
                af::addWriteHeaders(req);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED", "login 缺入口标记");
            }

            // 对端不在 ADMIN_ALLOWED_CIDRS：白名单路由同样 404（嵌套作用域，结束后恢复外层测试配置）。
            {
                // 192.0.2.0/24 为 RFC 5737 文档保留地址段，不会与测试请求的默认对端重合。
                af::ScopedAdminConfig foreignPeerOnly(std::vector<std::string>{"192.0.2.1/32"});
                const std::vector<std::pair<drogon::HttpMethod, std::string>> probes = {
                    {drogon::Get, af::kHealthPath},
                    {drogon::Get, af::kBootstrapPath},
                    {drogon::Post, af::kSessionsPath},
                    {drogon::Get, af::kUsersPath},
                };
                for (const auto &probe : probes) {
                    auto req = af::adminRequest(probe.first, probe.second);
                    if (probe.first == drogon::Post) {
                        af::addWriteHeaders(req);
                    }
                    CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 404, "NOT_FOUND",
                                         "非白名单对端 " + af::describe(probe.first, probe.second));
                }
            }
        }

        TEST_CASE("AdminFilter: 相似路径与其他方法不能绕过精确白名单") {
            // 读方法（无 Cookie）：任何与白名单不逐字相等的路径都必须 401。
            const std::vector<std::pair<drogon::HttpMethod, std::string>> readProbes = {
                {drogon::Get, "/api/admin/v1/health/"},
                {drogon::Get, "/api/admin/v1/healthz"},
                {drogon::Get, "/api/admin/v1/health/users"},
                {drogon::Get, "/api/admin/v1/health/../users"},
                {drogon::Get, "/api/admin/v1/Health"},
                {drogon::Get, "/API/ADMIN/V1/HEALTH"},
                {drogon::Get, "/api/admin/v1/session/"},
                {drogon::Get, "/api/admin/v1/sessionx"},
                {drogon::Get, "/api/admin/v1/session/users"},
                {drogon::Get, af::kSessionsPath},  // 登录路径换成 GET
                {drogon::Get, "/api/admin/v1/users/"},
                {drogon::Get, "/api/admin/v1/users/x"},
                {drogon::Get, "/api/admin/v1/"},
                {drogon::Get, "/api/admin/v1"},
                {drogon::Get, "/api/admin/v1/unknown"},
                {drogon::Head, af::kHealthPath},
                {drogon::Head, af::kBootstrapPath},
                {drogon::Head, af::kUsersPath},
                {drogon::Options, af::kHealthPath},
                {drogon::Options, af::kBootstrapPath},
                {drogon::Options, af::kUsersPath},
            };
            for (const auto &probe : readProbes) {
                auto req = af::adminRequest(probe.first, probe.second);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     af::describe(probe.first, probe.second));
            }

            // 写方法（无 Cookie，但带完整 Origin + X-Requested-By）：只有 POST /sessions 是登录。
            const std::vector<std::pair<drogon::HttpMethod, std::string>> writeProbes = {
                {drogon::Delete, af::kSessionsPath},  // 注销仍是受保护写请求
                {drogon::Put, af::kSessionsPath},
                {drogon::Patch, af::kSessionsPath},
                {drogon::Post, "/api/admin/v1/sessions/"},
                {drogon::Post, "/api/admin/v1/sessions/x"},
                {drogon::Post, "/api/admin/v1/sessionsx"},
                {drogon::Post, "/API/ADMIN/V1/SESSIONS"},
                {drogon::Post, af::kBootstrapPath},  // 引导路径换成 POST
                {drogon::Post, af::kHealthPath},
                {drogon::Post, af::kUsersPath},
                {drogon::Delete, af::kUsersPath},
            };
            for (const auto &probe : writeProbes) {
                auto req = af::adminRequest(probe.first, probe.second);
                af::addWriteHeaders(req);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     af::describe(probe.first, probe.second));
            }
        }

        TEST_CASE("AdminFilter: 会话无法加载时 fail closed（带 Cookie，无数据库）") {
            // 前提：本测试进程从不初始化连接池，loadByRawToken 必然回调 nullopt。
            CHECK_FALSE(db::Pool::instance().ready());
            const std::string bogusToken = crypto::randomHex(32);

            for (const auto &path : af::protectedListPaths()) {
                auto req = af::adminRequest(drogon::Get, path);
                af::setSessionCookie(req, bogusToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     "带不可加载 Cookie " + af::describe(drogon::Get, path));
                CHECK_FALSE(adminctx::isAuthenticated(req));
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setCsrfToken(req, crypto::randomHex(32));
                af::setSessionCookie(req, bogusToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     "带不可加载 Cookie 的注销");
            }
            // 引导接口：带陈旧 Cookie 仍放行，且按未登录处理（不附加会话属性）。
            {
                auto req = af::adminRequest(drogon::Get, af::kBootstrapPath);
                af::setSessionCookie(req, bogusToken);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "带不可加载 Cookie 的 bootstrap");
                CHECK_FALSE(adminctx::isAuthenticated(req));
                CHECK(adminctx::sessionId(req).empty());
            }
        }
    }

    TEST_CASE("Wallet: response envelope uses bounded integer balances, not strings") {
        const auto zero = WalletService::responseBody({});
        CHECK_EQ(zero.size(), 1u);
        CHECK_EQ(zero["wallet"].size(), 2u);
        CHECK(zero["wallet"]["flower"].isInt64());
        CHECK_EQ(zero["wallet"]["flower"].asInt64(), Json::Int64(0));
        CHECK_EQ(zero["wallet"]["sunshine"].asInt64(), Json::Int64(0));
        const auto caps = WalletService::responseBody({1000000, 1000000000});
        CHECK_EQ(caps["wallet"]["flower"].asInt64(), Json::Int64(1000000));
        CHECK_EQ(caps["wallet"]["sunshine"].asInt64(), Json::Int64(1000000000));
        CHECK_FALSE(caps["wallet"]["flower"].isString());
    }

    TEST_CASE("Wallet startup: absent client refuses with no connection details") {
        const auto result = db::checkWalletPrivilegesForStartup(nullptr);
        CHECK(result.state == db::WalletStartupResult::State::QueryError);
        CHECK_FALSE(result.allowed());
        CHECK_FALSE(result.report.has_value());
    }

    // 阶段 1 M3-B：任务发布与读取的纯函数、游标、映射与启动判定（见 TaskServiceUnitTests.h）。
    usapptest::taskunit::runTaskServiceUnitTests();

    // 阶段 1 M3-C1：完成结算的纯函数（见 TaskCompletionUnitTests.h）。
    usapptest::taskcompletionunit::runTaskCompletionUnitTests();

    // M6: change request parsing, fingerprints, error mapping and additive Task fields.
    usapptest::taskchangeunit::runTaskChangeUnitTests();

    // M7: reward-product request parsing, fingerprints and error mapping.
    usapptest::productunit::runProductUnitTests();
    usapptest::redemptionunit::runRedemptionUnitTests();
    usapptest::messageunit::runMessageUnitTests();
    usapptest::reactionunit::run();
    usapptest::anniversaryunit::runAnniversaryUnitTests();
    usapptest::memoryunit::runMemoryUnitTests();
    usapptest::albumunit::runAlbumUnitTests();
    usapptest::notificationintentunit::runNotificationIntentUnitTests();
    // P2-5a: push endpoint/key validation, fingerprint, marker and closed gate.
    usapptest::pushunit::runPushUnitTests();
    // P2-5b: dispatch outcome classes, tick plan and pre-send decisions.
    usapptest::pushdispatchunit::runPushDispatchUnitTests();
    usapptest::pushworkerunit::runPushWorkerUnitTests();
    usapptest::reminderunit::runReminderUnitTests();

    usapptest::deletionjournalunit::runDeletionJournalUnitTests();
    usapptest::deletionrecoveryunit::runDeletionRecoveryUnitTests();
    usapptest::deletionstartupunit::run();
    usapptest::journalbridgeunit::run();
    usapptest::recoverysourceunit::run();
    return usapptest::finish();
}
