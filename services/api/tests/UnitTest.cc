// =====================================================================
// services/api/tests/UnitTest.cc
//
// 目标：usapp_test_unit
// 纯单元测试：安全基线、注册白名单、Cookie 与 Origin 规则、游标分页、
// 错误码映射与锁键派生；AdminFilter 未认证访问行为回归（真实 doFilter）。
//
// 依赖：usapp_core（纯 C++ 逻辑与工具函数，不依赖数据库连接与网络）。
// =====================================================================

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AdminFilterProbe.h"
#include "TestSupport.h"
#include "core/Crypto.h"
#include "core/Errors.h"
#include "db/Db.h"
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "scheduler/LockKey.h"
#include "services/ProfileService.h"
#include "services/RegistrationPolicy.h"

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

    return usapptest::finish();
}
