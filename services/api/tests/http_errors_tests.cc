// =====================================================================
// services/api/tests/http_errors_tests.cc
//
// 要求 5 的**运行时**部分：真的调用 errorResponse / jsonResponse /
// noContentResponse，检查返回的 HttpResponse 上恒有
// `Cache-Control: no-store`（以及 Pragma / nosniff / Referrer-Policy）。
//
// 为什么可以在这里构造 HttpResponse：这三个函数用的是
//   drogon::HttpResponse::newHttpJsonResponse(body)
//   drogon::HttpResponse::newHttpResponse()
// 不依赖事件循环（它们在 services/api/src/core/Errors.cc:106/114/121）。
//
// 关于头读取 API 的稳健性：drogon 不同版本对 HttpResponse 的读头接口
// 可能是 `std::string_view getHeader(...)` 或 `const std::string &getHeader(...)`
// （甚至历史上只有 headers() 映射）。本文件用 SFINAE 探测 getHeader 是否存在，
// 因此**在任何版本上都能编译**：
//   * 存在 -> 读到真实头并断言；
//   * 不存在 -> 记为一条失败断言并打印提示，而不是编译错误。
// 这样做的代价是：如果探测不到，本文件无法给出运行时证据，必须靠
// csrf_tests.cc 的静态断言（Errors.cc 文本）兜底。
// =====================================================================

#include <string>
#include <type_traits>
#include <utility>

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include "TestSupport.h"
#include "core/Errors.h"

namespace {

// 探测 drogon::HttpResponse 是否有可读的 getHeader。
template <typename T, typename = void>
struct HasGetHeader : std::false_type {};

template <typename T>
struct HasGetHeader<T, std::void_t<decltype(std::declval<const T &>().getHeader("X"))>>
    : std::true_type {};

template <typename RespPtr>
std::string readHeaderImpl(const RespPtr &resp, const std::string &name, std::true_type) {
    return std::string(resp->getHeader(name));
}

template <typename RespPtr>
std::string readHeaderImpl(const RespPtr &, const std::string &, std::false_type) {
    return std::string();
}

template <typename RespPtr>
std::string readHeader(const RespPtr &resp, const std::string &name) {
    return readHeaderImpl(resp, name, HasGetHeader<drogon::HttpResponse>{});
}

void checkNoStore(const drogon::HttpResponsePtr &resp, const char *what) {
    if (!HasGetHeader<drogon::HttpResponse>::value) {
        usapptest::note(std::string("drogon::HttpResponse::getHeader 不可用（版本差异），") +
                        "跳过 " + what + " 的运行时头断言；由 csrf_tests.cc 的静态断言兜底");
        CHECK(false);
        return;
    }
    if (!resp) {
        CHECK(false);
        return;
    }

    const std::string cacheControl = readHeader(resp, "Cache-Control");
    // 核心不变量：任何 JSON / 错误响应都不可被浏览器或 SW 缓存。
    CHECK_CONTAINS(cacheControl, "no-store");
    CHECK_CONTAINS(cacheControl, "no-cache");
    CHECK_CONTAINS(cacheControl, "must-revalidate");
    CHECK_CONTAINS(cacheControl, "private");

    CHECK_CONTAINS(readHeader(resp, "Pragma"), "no-cache");
    CHECK_CONTAINS(readHeader(resp, "X-Content-Type-Options"), "nosniff");
    CHECK_CONTAINS(readHeader(resp, "Referrer-Policy"), "same-origin");
}

}  // namespace

int main() {
    TEST_CASE("要求5 运行时：errorResponse 恒带 Cache-Control: no-store") {
        const auto notFound = usapp::errorResponse(usapp::ErrorCode::NotFound, "找不到");
        checkNoStore(notFound, "errorResponse(NotFound)");

        const auto csrf = usapp::errorResponse(usapp::ErrorCode::CsrfTokenInvalid, "CSRF 不匹配");
        checkNoStore(csrf, "errorResponse(CsrfTokenInvalid)");

        const auto origin = usapp::errorResponse(usapp::ErrorCode::OriginNotAllowed, "来源不允许");
        checkNoStore(origin, "errorResponse(OriginNotAllowed)");

        const auto internal = usapp::errorResponse(usapp::ErrorCode::InternalError, "内部错误");
        checkNoStore(internal, "errorResponse(InternalError)");

        // 显式状态码覆盖路径同样带 no-store。
        const auto explicitStatus =
            usapp::errorResponse(usapp::ErrorCode::SpaceNotFound, "空间不存在", 404);
        checkNoStore(explicitStatus, "errorResponse(...,404)");

        // 带 request_id 的分支（body 结构不同）也必须带 no-store。
        const auto withRequestId =
            usapp::errorResponse(usapp::ErrorCode::ValidationError, "参数不合法", 400, "req-123");
        checkNoStore(withRequestId, "errorResponse(...,request_id)");
    }

    TEST_CASE("要求5 运行时：jsonResponse 与 noContentResponse 恒带 Cache-Control: no-store") {
        Json::Value body;
        body["ok"] = true;
        checkNoStore(usapp::jsonResponse(body, 200), "jsonResponse(200)");
        checkNoStore(usapp::jsonResponse(body, 201), "jsonResponse(201)");

        Json::Value empty;  // 即使是空 JSON 对象也不得被缓存
        checkNoStore(usapp::jsonResponse(empty, 200), "jsonResponse(empty)");

        checkNoStore(usapp::noContentResponse(), "noContentResponse(204)");

        // 没有调用方的情况下也直接验证一次 newHttpResponse 是可控的：
        // 证明“可以在没有事件循环时构造 HttpResponse”这一前提成立。
        const auto bare = drogon::HttpResponse::newHttpResponse();
        CHECK(bare != nullptr);
    }

    return usapptest::finish();
}
