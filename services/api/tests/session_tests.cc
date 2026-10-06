// =====================================================================
// services/api/tests/session_tests.cc
//
// 要求 1「登录与会话」的纯单元测试。
//
// 覆盖：
//   * SessionService::tokenHash —— 不可逆（散列 != 明文）、同输入稳定、
//     定长 64 位小写 hex、与 crypto::sha256Hex 一致。
//   * SessionService::verifyCsrf —— 正确令牌 true；错误令牌 / 空令牌 /
//     会话未绑定 CSRF 散列 一律 false。
//   * SessionService::userAgentFamily —— 归类正确（含边界与优先级）。
//
// 全部为运行时验证，不需要数据库 / 网络 / 事件循环：
// 这三个函数是 static 纯函数（SessionService.cc:47/237/245）。
// =====================================================================

#include <set>
#include <string>

#include "TestSupport.h"
#include "core/Crypto.h"
#include "services/SessionService.h"

using usapp::SessionRecord;
using usapp::SessionService;

namespace {

bool isLowerHex64(const std::string &s) {
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // =================================================================
    // tokenHash
    // =================================================================
    TEST_CASE("要求1 tokenHash：不可逆、同输入稳定、定长小写 hex") {
        const std::string raw =
            "3f9a1c0b7e2d4f6a8b0c1d2e3f4a5b6c7d8e9f00112233445566778899aabbcc";

        const std::string h1 = SessionService::tokenHash(raw);
        const std::string h2 = SessionService::tokenHash(raw);

        CHECK_EQ(static_cast<int>(h1.size()), 64);
        CHECK(isLowerHex64(h1));

        // 不可逆的直接可观测形态：散列不等于明文，也不包含明文。
        CHECK_NE(h1, raw);
        CHECK(h1.find(raw) == std::string::npos);
        // 空串散列也不等于空串（否则“缺失令牌”会与“空令牌”混淆）。
        CHECK_NE(SessionService::tokenHash(""), std::string(""));

        // 同输入稳定（同一进程内多次调用）。
        CHECK_EQ(h1, h2);
        // 与 crypto 层一致：tokenHash 只是 sha256Hex 的别名（SessionService.cc:47）。
        CHECK_EQ(h1, usapp::crypto::sha256Hex(raw));

        // 不同输入 -> 不同散列（含单字符差异与空串）。
        CHECK_NE(SessionService::tokenHash("token-1"), SessionService::tokenHash("token-2"));
        CHECK_NE(SessionService::tokenHash(""), SessionService::tokenHash("a"));
        CHECK_NE(SessionService::tokenHash("A"), SessionService::tokenHash("a"));

        // 空串也有确定的 64 字符散列。
        CHECK_EQ(static_cast<int>(SessionService::tokenHash("").size()), 64);
        CHECK(isLowerHex64(SessionService::tokenHash("")));

        // 长度不一的输入不产生前缀关系（避免“散列前缀可比较”的误解）。
        const std::string shortHash = SessionService::tokenHash("x");
        CHECK(shortHash.substr(0, 16) != h1.substr(0, 16));
    }

    // =================================================================
    // verifyCsrf
    // =================================================================
    TEST_CASE("要求1 verifyCsrf：错误令牌与空令牌返回 false") {
        SessionRecord session;
        session.id = "session-1";
        session.csrfTokenHash = usapp::crypto::sha256Hex("csrf-plain-token");

        // 正确令牌（明文 -> sha256 与会话里保存的散列一致）。
        CHECK(SessionService::verifyCsrf(session, "csrf-plain-token"));

        // 错误令牌。
        CHECK_FALSE(SessionService::verifyCsrf(session, "csrf-plain-toke"));
        CHECK_FALSE(SessionService::verifyCsrf(session, "csrf-plain-token "));
        CHECK_FALSE(SessionService::verifyCsrf(session, "csrF-plain-token"));
        CHECK_FALSE(SessionService::verifyCsrf(session, "CSRF-PLAIN-TOKEN"));
        CHECK_FALSE(SessionService::verifyCsrf(session, "csrf-plain-tokenX"));

        // 空令牌（缺失头 -> http::requestOrigin 之外的另一处 fail closed）。
        CHECK_FALSE(SessionService::verifyCsrf(session, ""));

        // 会话未绑定 CSRF 散列：即使传入非空令牌也必须拒绝。
        SessionRecord unbound;
        unbound.id = "session-2";
        CHECK_FALSE(SessionService::verifyCsrf(unbound, "csrf-plain-token"));
        CHECK_FALSE(SessionService::verifyCsrf(unbound, ""));

        // 会话散列是任意非法串时不得误通过（比较前会重新散列 provided）。
        SessionRecord bogus;
        bogus.csrfTokenHash = "not-a-hash";
        CHECK_FALSE(SessionService::verifyCsrf(bogus, "not-a-hash"));

        // 把会话里保存的散列当作“令牌”传回也不能通过（证明不是明文比较）。
        CHECK_FALSE(SessionService::verifyCsrf(session, session.csrfTokenHash));
    }

    // =================================================================
    // userAgentFamily
    // =================================================================
    TEST_CASE("要求1 userAgentFamily：归类正确（含优先级与边界）") {
        const std::string kChrome =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/120.0.0.0 Safari/537.36";
        const std::string kEdge =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/120.0.0.0 Safari/537.36 Edg/120.0.2210.91";
        const std::string kFirefox =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:121.0) Gecko/20100101 Firefox/121.0";
        const std::string kSafari =
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like "
            "Gecko) Version/17.1 Safari/605.1.15";
        const std::string kChromium =
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chromium/120.0.6099.109 Chrome/120.0.6099.109 Safari/537.36";

        // 真正的空串 -> unknown（SessionService.cc:251）。
        CHECK_EQ(SessionService::userAgentFamily(""), std::string("unknown"));

        CHECK_EQ(SessionService::userAgentFamily(kChrome), std::string("chrome"));
        // Edge 的 UA 也含 "Chrome/"，但 "edg/" 判定在前 -> edge。
        CHECK_EQ(SessionService::userAgentFamily(kEdge), std::string("edge"));
        CHECK_EQ(SessionService::userAgentFamily(kFirefox), std::string("firefox"));
        CHECK_EQ(SessionService::userAgentFamily(kSafari), std::string("safari"));
        CHECK_EQ(SessionService::userAgentFamily("curl/8.4.0"), std::string("curl"));
        CHECK_EQ(SessionService::userAgentFamily("Wget/1.21.3"), std::string("other"));
        CHECK_EQ(SessionService::userAgentFamily("SomeCustomAgent/1.0"), std::string("other"));

        // 大小写不敏感：实现先整体 tolower 再匹配。
        CHECK_EQ(SessionService::userAgentFamily("CURL/8.4.0"), std::string("curl"));
        CHECK_EQ(SessionService::userAgentFamily("FIREFOX/121.0"), std::string("firefox"));
        CHECK_EQ(SessionService::userAgentFamily("XXX CHROME/120.0 YYY"), std::string("chrome"));
        CHECK_EQ(SessionService::userAgentFamily("XXX EDG/120.0 YYY"), std::string("edge"));

        // 边界：只有"真正的空串"才是 unknown；全空白 UA 落到 other。
        CHECK_EQ(SessionService::userAgentFamily("   "), std::string("other"));

        // 边界：含 "chromium" 时显式跳过 chrome 分支；该 UA 同时含 "safari/"，
        // 因此当前实现把它归为 safari。这是实现的确定行为（不是"正确性"声明），
        // 与 SessionService.cc:256 的 `!= "chromium"` 条件对应。
        CHECK_EQ(SessionService::userAgentFamily(kChromium), std::string("safari"));

        // 归类结果只落在有限集合内（不返回原始 UA，减少隐私面）。
        const std::string family = SessionService::userAgentFamily(kEdge);
        CHECK(family.find('/') == std::string::npos);
        CHECK(family.find("Mozilla") == std::string::npos);
        std::set<std::string> allowed{"unknown", "edge", "chrome", "firefox", "safari", "curl",
                                      "other"};
        CHECK(allowed.count(family) == 1);
    }

    return usapptest::finish();
}
