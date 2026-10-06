// =====================================================================
// services/api/tests/csrf_tests.cc
//
// 要求 5「CSRF 拒绝场景」。
//
// 覆盖：
//   * http::originMatches —— 精确匹配 scheme+host+port；大小写敏感；
//     带尾斜杠不等；空串不等；8443 与 8444 不等；不接受子域名/前缀。
//   * http::requiresCsrf / http::isStateChangingMethod —— POST/PUT/PATCH/
//     DELETE 为真，GET/HEAD/OPTIONS 为假，未知方法按“状态修改”处理。
//   * AuthFilter.cc 的文本断言：CSRF 校验发生在 SessionService::verifyCsrf，
//     失败码是 ErrorCode::CsrfTokenInvalid。
//   * Errors.cc 的文本断言：applyNoStore 恒加 Cache-Control: no-store，
//     且被 errorResponse / jsonResponse / noContentResponse 三者共同调用。
//     （真正构造 HttpResponse 的运行时验证在 http_errors_tests.cc。）
//
// originMatches / requiresCsrf / isStateChangingMethod 是运行时验证，
// 不需要事件循环（纯函数）；AuthFilter.cc / Errors.cc 是静态文本断言。
// =====================================================================

#include <string>

#include "RepoText.h"
#include "TestSupport.h"
#include "http/HttpUtil.h"

namespace {

using usapp::http::isStateChangingMethod;
using usapp::http::originMatches;
using usapp::http::requiresCsrf;

const char *kPublicOrigin = "https://localhost:8443";

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

}  // namespace

int main() {
    // =================================================================
    // originMatches
    // =================================================================
    TEST_CASE("要求5 originMatches：精确匹配 scheme+host+port") {
        // 正常相等。
        CHECK(originMatches(kPublicOrigin, kPublicOrigin));
        CHECK(originMatches("https://usapp.example.invalid", "https://usapp.example.invalid"));
        CHECK(originMatches("https://localhost:8444", "https://localhost:8444"));

        // 端口不同 -> 不等（8443 vs 8444）。
        CHECK_FALSE(originMatches("https://localhost:8443", "https://localhost:8444"));
        CHECK_FALSE(originMatches("https://localhost:8444", "https://localhost:8443"));

        // 空串（请求头缺失 或 期望值为空）-> 不等，绝不「宽松放行」。
        CHECK_FALSE(originMatches("", kPublicOrigin));
        CHECK_FALSE(originMatches(kPublicOrigin, ""));
        CHECK_FALSE(originMatches("", ""));

        // 尾斜杠参与比较 -> 不等（实现是纯字符串相等，不做 URL 归一化）。
        CHECK_FALSE(originMatches("https://localhost:8443/", "https://localhost:8443"));
        CHECK_FALSE(originMatches("https://localhost:8443", "https://localhost:8443/"));
        CHECK_FALSE(originMatches("https://localhost:8443/", "https://localhost:8443/"));

        // 大小写敏感（scheme 与 host 都不做大小写折叠）。
        CHECK_FALSE(originMatches("HTTPS://localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://LOCALHOST:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://LocalHost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("Https://localhost:8443", kPublicOrigin));

        // 不接受子域名（前缀匹配会放行 https://evil.localhost:8443）。
        CHECK_FALSE(originMatches("https://evil.localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://a.b.localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost.evil.test:8443", kPublicOrigin));

        // 不接受前缀/后缀拼接。
        CHECK_FALSE(originMatches("https://localhost:8443x", kPublicOrigin));
        CHECK_FALSE(originMatches("xhttps://localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost:84430", kPublicOrigin));

        // scheme 不同（http vs https；'null' 也是常见伪造值）。
        CHECK_FALSE(originMatches("http://localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("null", kPublicOrigin));

        // 常见绕过形态不得放行：userinfo / 路径 / 查询串 / 前后空白 / 端口省略。
        CHECK_FALSE(originMatches("https://localhost:8443@evil.test", kPublicOrigin));
        CHECK_FALSE(originMatches("https://evil.test#https://localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost:8443/path", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost:8443?x=1", kPublicOrigin));
        CHECK_FALSE(originMatches(" https://localhost:8443", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost:8443 ", kPublicOrigin));
        CHECK_FALSE(originMatches("https://localhost", kPublicOrigin));

        // 反向：把「期望值」设成上面那些异常串时，也只有逐字相等才通过。
        CHECK(originMatches("https://localhost:8443@evil.test",
                            "https://localhost:8443@evil.test"));
    }

    // =================================================================
    // requiresCsrf / isStateChangingMethod
    // =================================================================
    TEST_CASE("要求5 requiresCsrf / isStateChangingMethod：方法分类") {
        // 状态修改方法 -> 需要 CSRF。
        CHECK(isStateChangingMethod(drogon::Post));
        CHECK(isStateChangingMethod(drogon::Put));
        CHECK(isStateChangingMethod(drogon::Patch));
        CHECK(isStateChangingMethod(drogon::Delete));

        // 安全方法 -> 不需要 CSRF。
        CHECK_FALSE(isStateChangingMethod(drogon::Get));
        CHECK_FALSE(isStateChangingMethod(drogon::Head));
        CHECK_FALSE(isStateChangingMethod(drogon::Options));

        // 未知方法走 switch 的 default 分支 -> 视为状态修改（fail closed）。
        CHECK(isStateChangingMethod(static_cast<drogon::HttpMethod>(255)));
        CHECK(isStateChangingMethod(static_cast<drogon::HttpMethod>(0)));

        // requiresCsrf 与 isStateChangingMethod 完全同义（实现是直接转发）。
        CHECK(requiresCsrf(drogon::Post));
        CHECK(requiresCsrf(drogon::Put));
        CHECK(requiresCsrf(drogon::Patch));
        CHECK(requiresCsrf(drogon::Delete));
        CHECK_FALSE(requiresCsrf(drogon::Get));
        CHECK_FALSE(requiresCsrf(drogon::Head));
        CHECK_FALSE(requiresCsrf(drogon::Options));
        CHECK(requiresCsrf(static_cast<drogon::HttpMethod>(255)));

        // 逐方法交叉一致（同一输入两个函数必须同时为真或同时为假）。
        const drogon::HttpMethod methods[] = {
            drogon::Get,   drogon::Post,   drogon::Put,    drogon::Patch,
            drogon::Delete, drogon::Head,  drogon::Options,
            static_cast<drogon::HttpMethod>(255)};
        for (const drogon::HttpMethod m : methods) {
            CHECK_EQ(isStateChangingMethod(m), requiresCsrf(m));
        }
    }

    // =================================================================
    // AuthFilter 的 CSRF 校验点（静态）
    // =================================================================
    TEST_CASE("要求5 静态断言：CSRF 校验发生在 verifyCsrf，失败码 CsrfTokenInvalid") {
        usapptest::noteStaticAssertion(
            "（读 services/api/src/http/AuthFilter.cc 文本；证明 CSRF 拒绝路径的"
            "实现位置与错误码，不是运行时验证）");

        const std::string filter = usapptest::readRepoFile("services/api/src/http/AuthFilter.cc");
        CHECK(!filter.empty());
        if (filter.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            const std::string flat = usapptest::collapseWs(filter);

            // 令牌取自 x-csrf-token 请求头。
            CHECK_CONTAINS(flat, "req->getHeader(\"x-csrf-token\")");
            // 校验调用。
            CHECK_CONTAINS(flat, "!SessionService::verifyCsrf(*session, provided)");
            // 失败码恰为 CsrfTokenInvalid。
            CHECK_CONTAINS(flat, "ErrorCode::CsrfTokenInvalid");
            CHECK_CONTAINS(flat, "CSRF 令牌缺失或不匹配");
            // 只在状态修改方法上做校验（GET/HEAD/OPTIONS 不带 CSRF）。
            CHECK_CONTAINS(flat, "http::requiresCsrf(req->method())");
            CHECK_CONTAINS(flat, "req->method()");
            // Origin 检查先于 CSRF 检查（同一个 doFilter 里）。
            CHECK_ORDER(filter, "ErrorCode::OriginNotAllowed", "ErrorCode::CsrfTokenInvalid");
            // 未认证时不进入 CSRF 分支（CSRF 失败是 403，不是 401）。
            CHECK_ORDER(filter, "ErrorCode::Unauthenticated", "ErrorCode::CsrfTokenInvalid");
            // CSRF 失败只出现一次 reject（没有别的兜底分支会放行）。
            CHECK_EQ(static_cast<int>(countOccurrences(flat, "ErrorCode::CsrfTokenInvalid")), 1);
        }
    }

    // =================================================================
    // no-store 恒定性（静态）
    // =================================================================
    TEST_CASE("要求5 静态断言：所有 JSON/错误响应恒带 Cache-Control: no-store") {
        usapptest::noteStaticAssertion(
            "（读 services/api/src/core/Errors.cc 文本；运行时构造 HttpResponse 的"
            "等价验证在 http_errors_tests.cc，两者应同时通过）");

        const std::string errors = usapptest::readRepoFile("services/api/src/core/Errors.cc");
        CHECK(!errors.empty());
        if (errors.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            const std::string flat = usapptest::collapseWs(errors);

            CHECK_CONTAINS(flat, "void applyNoStore(const drogon::HttpResponsePtr &resp)");
            CHECK_CONTAINS(flat, "resp->addHeader(\"Cache-Control\",");
            CHECK_CONTAINS(flat, "no-store, no-cache, must-revalidate, private");
            CHECK_CONTAINS(flat, "Pragma");
            CHECK_CONTAINS(flat, "no-cache");
            CHECK_CONTAINS(flat, "X-Content-Type-Options");
            CHECK_CONTAINS(flat, "nosniff");
            CHECK_CONTAINS(flat, "Referrer-Policy");
            CHECK_CONTAINS(flat, "same-origin");

            // 三个构造函数都必须调用 applyNoStore。
            CHECK_CONTAINS(flat, "drogon::HttpResponsePtr errorResponse");
            CHECK_CONTAINS(flat, "drogon::HttpResponsePtr jsonResponse");
            CHECK_CONTAINS(flat, "drogon::HttpResponsePtr noContentResponse");
            CHECK_EQ(static_cast<int>(countOccurrences(flat, "applyNoStore(")), 4);

            // CSRF 状态码是 403（契约与实现一致）。
            CHECK_CONTAINS(flat, "case ErrorCode::CsrfTokenInvalid:");
            CHECK_CONTAINS(flat, "CSRF_TOKEN_INVALID");
            CHECK_CONTAINS(flat, "case ErrorCode::OriginNotAllowed:");
            CHECK_CONTAINS(flat, "ORIGIN_NOT_ALLOWED");
        }
    }

    return usapptest::finish();
}
