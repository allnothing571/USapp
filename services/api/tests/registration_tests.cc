// =====================================================================
// services/api/tests/registration_tests.cc
//
// 要求 2「注册白名单」。
//
// 覆盖：
//   * RegistrationPolicy 的真实接口（读 services/api/src/services/
//     RegistrationPolicy.h 得到：configure / isWhitelistMode /
//     isAllowedEmail / mayCreateAccount）。
//   * 白名单只影响「新账号注册」：已存在账号永不受影响。
//   * 邮箱经 crypto::canonicalizeEmail 归一化（大小写 + 首尾空白）后判定；
//     RegistrationPolicy 自身不做归一化，传入未归一化的原串不会命中。
//   * REGISTRATION_MODE 非 whitelist（= open）时的行为。
//   * ALLOWED_EMAILS 为空时的行为（逐请求 fail closed）。
//   * 它不是权限判定：本类没有任何权限接口，且 AuthService 只有一个调用点。
//
// 单元部分为运行时验证；「只影响新账号注册」的调用点证据是
// **静态文本断言**（读 AuthService.cc 文本），测试内已显式标注。
// =====================================================================

#include <string>
#include <unordered_set>
#include <vector>

#include "RepoText.h"
#include "TestSupport.h"
#include "core/Crypto.h"
#include "services/RegistrationPolicy.h"

using usapp::RegistrationPolicy;

namespace {

size_t countOccurrences(const std::string &hay, const std::string &needle) {
    if (needle.empty()) {
        return 0;
    }
    size_t n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

void configurePolicy(const std::string &mode, std::initializer_list<const char *> emails) {
    std::unordered_set<std::string> set;
    for (const char *e : emails) {
        set.insert(std::string(e));
    }
    RegistrationPolicy::instance().configure(mode, set);
}

}  // namespace

int main() {
    // =================================================================
    // 白名单模式：新账号
    // =================================================================
    TEST_CASE("要求2 白名单模式：只有白名单内的新邮箱可创建账号") {
        configurePolicy("whitelist", {"alice@example.com", "bob@example.com"});

        CHECK(RegistrationPolicy::instance().isWhitelistMode());

        // 新账号（userExists == false）+ 白名单内 -> 允许。
        CHECK(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", false));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("bob@example.com", false));

        // 新账号 + 不在白名单 -> 拒绝。
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("carol@example.com", false));
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("", false));

        // isAllowedEmail 只做精确集合成员判定。
        CHECK(RegistrationPolicy::instance().isAllowedEmail("alice@example.com"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("alice@example.COM"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail(" alice@example.com"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("alice@example.com "));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("alice@example.com\n"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("alice+tag@example.com"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("sub.alice@example.com"));
    }

    // =================================================================
    // 白名单只影响新账号
    // =================================================================
    TEST_CASE("要求2 白名单只影响新账号注册：已存在账号永远可继续登录") {
        // 关闭白名单与开启白名单下，都测得同一结论。
        configurePolicy("whitelist", {"alice@example.com"});

        // userExists == true 时，无论邮箱是否在白名单内，都放行（实现第一条分支）。
        CHECK(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", true));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("carol@example.com", true));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("", true));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("totally@elsewhere.test", true));

        // 对照：同一个不在白名单的邮箱，userExists == false 时被拒。
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("carol@example.com", false));
    }

    // =================================================================
    // 归一化发生在调用方
    // =================================================================
    TEST_CASE("要求2 邮箱归一化后判定：大小写与首尾空白在进入本类之前已被消除") {
        // canonicalizeEmail 是 AuthService 归一化邮箱的入口（crypto 层）。
        usapp::crypto::CanonicalEmail parsed;

        CHECK(usapp::crypto::canonicalizeEmail("  ALICE@Example.COM  ", parsed));
        CHECK_EQ(parsed.canonical, std::string("alice@example.com"));

        // 白名单集合里存的是「已归一化」的邮箱（AppConfig.cc 用 parsed.canonical 插入）。
        configurePolicy("whitelist", {"alice@example.com"});

        // 归一化后的规范串命中。
        CHECK(RegistrationPolicy::instance().mayCreateAccount(parsed.canonical, false));

        // 大小写归一化：另一种写法的原串先归一化，再判定，同样命中。
        usapp::crypto::CanonicalEmail again;
        CHECK(usapp::crypto::canonicalizeEmail("Alice@EXAMPLE.com", again));
        CHECK_EQ(again.canonical, parsed.canonical);
        CHECK(RegistrationPolicy::instance().mayCreateAccount(again.canonical, false));

        // 但直接把未归一化的原串交给本类，是**不命中**的——本类不做归一化。
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("  ALICE@Example.COM  ", false));
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("ALICE@EXAMPLE.COM", false));

        // 归一只处理大小写与首尾空白；内部空白属于非法邮箱。
        usapp::crypto::CanonicalEmail bad;
        CHECK_FALSE(usapp::crypto::canonicalizeEmail("alice @example.com", bad));
        CHECK_FALSE(usapp::crypto::canonicalizeEmail("alice@exa mple.com", bad));
        CHECK_FALSE(usapp::crypto::canonicalizeEmail("", bad));
        CHECK_FALSE(usapp::crypto::canonicalizeEmail("   ", bad));
        CHECK_FALSE(usapp::crypto::canonicalizeEmail("alice@example.com extra", bad));
    }

    // =================================================================
    // 非白名单模式
    // =================================================================
    TEST_CASE("要求2 REGISTRATION_MODE 非 whitelist（open）时的行为") {
        configurePolicy("open", {"alice@example.com"});

        CHECK_FALSE(RegistrationPolicy::instance().isWhitelistMode());

        // open 模式下任何人都能创建新账号（白名单集合仍在内存里但没有作用）。
        CHECK(RegistrationPolicy::instance().mayCreateAccount("carol@example.com", false));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", false));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("anyone@anywhere.test", false));
        CHECK(RegistrationPolicy::instance().mayCreateAccount("", false));

        // isAllowedEmail 仍然反映集合内容——它只是查询，不是模式判定。
        CHECK(RegistrationPolicy::instance().isAllowedEmail("alice@example.com"));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("carol@example.com"));

        // 模式字符串比较是**区分大小写的字面量比较**（RegistrationPolicy.cc:13
        // 为 mode == "whitelist"）。AppConfig.cc 会在 load() 时先 tolower，
        // 所以真实链路上永远是小写；这里把这个前提钉住。
        configurePolicy("WHITELIST", {"alice@example.com"});
        CHECK_FALSE(RegistrationPolicy::instance().isWhitelistMode());
        CHECK(RegistrationPolicy::instance().mayCreateAccount("carol@example.com", false));

        configurePolicy("Whitelist", {"alice@example.com"});
        CHECK_FALSE(RegistrationPolicy::instance().isWhitelistMode());
    }

    // =================================================================
    // 空白名单
    // =================================================================
    TEST_CASE("要求2 ALLOWED_EMAILS 为空时的行为") {
        configurePolicy("whitelist", {});

        CHECK(RegistrationPolicy::instance().isWhitelistMode());

        // 空集合 + 白名单模式 => 任何新账号都被拒（逐请求 fail closed）。
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", false));
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("", false));
        CHECK_FALSE(RegistrationPolicy::instance().mayCreateAccount("root@localhost", false));
        CHECK_FALSE(RegistrationPolicy::instance().isAllowedEmail("alice@example.com"));

        // 但已存在账号仍然可登录（白名单不锁门）。
        CHECK(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", true));

        // 空集合 + open 模式 => 不构成任何限制。
        configurePolicy("open", {});
        CHECK(RegistrationPolicy::instance().mayCreateAccount("alice@example.com", false));
    }

    // =================================================================
    // 不是权限判定
    // =================================================================
    TEST_CASE("要求2 白名单不是权限判定（接口面 + 唯一调用点 + 文档注释）") {
        usapptest::noteStaticAssertion(
            "（读 services/api/src/services/AuthService.cc 文本；无数据库环境下这是"
            "“白名单只在注册判定处生效”的可执行代理证据，不是运行时验证）");

        const std::string auth = usapptest::readRepoFile("services/api/src/services/AuthService.cc");
        CHECK(!auth.empty());
        if (auth.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            // 唯一调用点：全文件只出现一次 mayCreateAccount。
            CHECK_EQ(static_cast<int>(countOccurrences(auth, "mayCreateAccount")), 1);
            // 调用点把「用户是否已存在」作为第二个实参传入。
            CHECK_CONTAINS(auth, "mayCreateAccount(ctx->emailCanonical, exists)");
            // 该调用点位于「先查用户是否存在」的步骤之后（stepLookupUser）。
            CHECK_CONTAINS(auth, "FROM users WHERE email_canonical = $1");
            CHECK_ORDER(auth, "FROM users WHERE email_canonical = $1",
                        "mayCreateAccount(ctx->emailCanonical, exists)");
            // 拒绝路径产出 RegistrationNotAllowed，而不是任何权限类错误。
            CHECK_CONTAINS(auth, "LoginOutcome::RegistrationNotAllowed");
            CHECK_CONTAINS(auth, "consumed_reason = 'rejected'");
            // 实现注释明确「白名单只在这里生效」。
            CHECK_CONTAINS(auth, "白名单只在这里生效");
        }

        // RegistrationPolicy 的公开接口里没有 authorize/permission/role 之类的成员。
        const std::string header =
            usapptest::readRepoFile("services/api/src/services/RegistrationPolicy.h");
        CHECK(!header.empty());
        if (!header.empty()) {
            CHECK_NOT_CONTAINS(header, "authorize");
            CHECK_NOT_CONTAINS(header, "isPermitted");
            CHECK_NOT_CONTAINS(header, "hasRole");
            CHECK_NOT_CONTAINS(header, "canAccess");
            // 注释明说「不代表任何权限」。
            CHECK_CONTAINS(header, "权限");
        }
    }

    return usapptest::finish();
}
