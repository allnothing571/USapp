// =====================================================================
// services/api/tests/AdminFilterProbe.h
//
// AdminFilter 行为测试支撑（UnitTest.cc 与 DbGatedTest.cc 共用）。
//
// 直接调用真实的 usapp::AdminFilter::doFilter()，记录它调用了哪一个回调：
//   * FilterCallback      —— 过滤器拒绝，返回错误响应（不进入控制器）
//   * FilterChainCallback —— 过滤器放行（在真实服务中会进入控制器）
// 这是行为断言，不是源码文本断言。
//
// 并发与生命周期约定：
//   * 回调只按值捕获 shared_ptr<ProbeState>，不捕获任何栈对象的引用；
//     即使等待超时、本函数已返回，迟到的回调也只会写入仍然存活的共享状态。
//   * 等待有上限；超时直接判定未完成，绝不随后阻塞在 future.get() 上。
//
// 配置约定：
//   * ScopedAdminConfig 只改写 AppConfig 单例中与管理面相关的三个字段，
//     析构时原样恢复。测试从不调用 AppConfig::load()，因此不读取任何 .env。
//   * 测试构造的请求没有真实 TCP 对端；对端地址取 Drogon 为该请求给出的
//     默认值，并只在测试期间把这个精确地址（/32 或 /128）加入白名单。
//     生产代码中的对端校验逻辑不做任何修改。
// =====================================================================
#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "config/AppConfig.h"
#include "http/AdminFilter.h"
#include "http/HttpUtil.h"

namespace usapptest::adminfilter {

// 测试专用值：与任何真实部署值都不同，便于确认测试没有沿用真实配置。
inline constexpr const char *kTestAdminOrigin = "https://admin.usapp.test";
inline constexpr const char *kTestCookieName = "__Host-usapp-admin-test";

inline constexpr const char *kHealthPath = "/api/admin/v1/health";
inline constexpr const char *kBootstrapPath = "/api/admin/v1/session";
inline constexpr const char *kSessionsPath = "/api/admin/v1/sessions";
inline constexpr const char *kUsersPath = "/api/admin/v1/users";
inline constexpr const char *kCouplesPath = "/api/admin/v1/couples";
inline constexpr const char *kAuditLogsPath = "/api/admin/v1/audit-logs";

inline const std::vector<std::string> &protectedListPaths() {
    static const std::vector<std::string> paths = {kUsersPath, kCouplesPath, kAuditLogsPath};
    return paths;
}

// ---------------------------------------------------------------------
// 配置：保存 -> 改写 -> 析构恢复
// ---------------------------------------------------------------------
class ScopedAdminConfig {
  public:
    explicit ScopedAdminConfig(std::vector<std::string> allowedCidrs)
        : cfg_(usapp::AppConfig::instance()),
          savedCidrs_(cfg_.adminAllowedCidrs),
          savedOrigin_(cfg_.adminOrigin),
          savedCookieName_(cfg_.adminSessionCookieName) {
        cfg_.adminAllowedCidrs = std::move(allowedCidrs);
        cfg_.adminOrigin = kTestAdminOrigin;
        cfg_.adminSessionCookieName = kTestCookieName;
    }

    ~ScopedAdminConfig() {
        cfg_.adminAllowedCidrs = savedCidrs_;
        cfg_.adminOrigin = savedOrigin_;
        cfg_.adminSessionCookieName = savedCookieName_;
    }

    ScopedAdminConfig(const ScopedAdminConfig &) = delete;
    ScopedAdminConfig &operator=(const ScopedAdminConfig &) = delete;

  private:
    usapp::AppConfig &cfg_;
    std::vector<std::string> savedCidrs_;
    std::string savedOrigin_;
    std::string savedCookieName_;
};

// 测试请求的对端地址（Drogon 对未经网络接收的请求给出的默认地址）对应的
// 单地址 CIDR。取不到时返回空串，由调用方判定失败。
inline std::string probePeerCidr() {
    const auto probe = drogon::HttpRequest::newHttpRequest();
    const std::string ip = usapp::http::peerIp(probe);
    if (ip.empty()) {
        return "";
    }
    return ip + (ip.find(':') != std::string::npos ? "/128" : "/32");
}

// ---------------------------------------------------------------------
// 请求构造
// ---------------------------------------------------------------------
inline std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// 带管理面入口标记的请求（模拟经由 8444 管理站点进入）。
inline drogon::HttpRequestPtr adminRequest(drogon::HttpMethod method,
                                           const std::string &path,
                                           bool withSurfaceHeader = true) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    if (withSurfaceHeader) {
        req->addHeader(lowerAscii(usapp::adminctx::kSurfaceHeader),
                       usapp::adminctx::kSurfaceValue);
    }
    return req;
}

inline void setOrigin(const drogon::HttpRequestPtr &req, const std::string &origin) {
    req->addHeader("origin", origin);
}

inline void setRequestedBy(const drogon::HttpRequestPtr &req, const std::string &value) {
    req->addHeader(lowerAscii(usapp::adminctx::kRequestedByHeader), value);
}

// 合法管理面写请求所需的两层来源标记：精确 Origin + X-Requested-By。
inline void addWriteHeaders(const drogon::HttpRequestPtr &req) {
    setOrigin(req, kTestAdminOrigin);
    setRequestedBy(req, usapp::adminctx::kRequestedByValue);
}

inline void setSessionCookie(const drogon::HttpRequestPtr &req, const std::string &rawToken) {
    req->addCookie(kTestCookieName, rawToken);
}

inline void setCsrfToken(const drogon::HttpRequestPtr &req, const std::string &token) {
    req->addHeader("x-csrf-token", token);
}

// ---------------------------------------------------------------------
// 执行过滤器
// ---------------------------------------------------------------------
struct FilterResult {
    bool completed = false;  // 时限内恰好收到一次回调
    bool continued = false;  // 只调用了 FilterChainCallback
    bool rejected = false;   // 只调用了 FilterCallback
    int rejectCalls = 0;
    int continueCalls = 0;
    int status = 0;          // 拒绝时的 HTTP 状态码
    std::string errorCode;   // 拒绝时 error.code
};

namespace detail {

struct ProbeState {
    std::promise<void> done;
    std::atomic<bool> settled{false};
    std::atomic<int> rejectCalls{0};
    std::atomic<int> continueCalls{0};
    std::mutex mutex;
    drogon::HttpResponsePtr response;

    void settle() {
        if (!settled.exchange(true)) {
            try {
                done.set_value();
            } catch (...) {
                // promise 已被满足：忽略（只可能在重复回调时发生，计数会暴露它）。
            }
        }
    }
};

}  // namespace detail

inline FilterResult runAdminFilter(const drogon::HttpRequestPtr &req,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto state = std::make_shared<detail::ProbeState>();
    auto future = state->done.get_future();

    // 过滤器对象由 shared_ptr 持有；AdminFilter 的异步回调本身不捕获 this。
    auto filter = std::make_shared<usapp::AdminFilter>();
    filter->doFilter(
        req,
        [state](const drogon::HttpResponsePtr &resp) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->response) {
                    state->response = resp;
                }
            }
            state->rejectCalls.fetch_add(1);
            state->settle();
        },
        [state]() {
            state->continueCalls.fetch_add(1);
            state->settle();
        });

    FilterResult result;
    if (future.wait_for(timeout) != std::future_status::ready) {
        // 超时：不调用 future.get()，直接返回未完成。
        result.rejectCalls = state->rejectCalls.load();
        result.continueCalls = state->continueCalls.load();
        return result;
    }

    result.rejectCalls = state->rejectCalls.load();
    result.continueCalls = state->continueCalls.load();
    result.completed = (result.rejectCalls + result.continueCalls) == 1;
    result.continued = result.continueCalls == 1 && result.rejectCalls == 0;
    result.rejected = result.rejectCalls == 1 && result.continueCalls == 0;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->response) {
        result.status = static_cast<int>(state->response->getStatusCode());
        const auto &json = state->response->getJsonObject();
        if (json && json->isObject() && json->isMember("error")) {
            const Json::Value &error = (*json)["error"];
            if (error.isObject() && error.isMember("code") && error["code"].isString()) {
                result.errorCode = error["code"].asString();
            }
        }
    }
    return result;
}

inline std::string describe(drogon::HttpMethod method, const std::string &path) {
    const char *name = "?";
    switch (method) {
        case drogon::Get: name = "GET"; break;
        case drogon::Post: name = "POST"; break;
        case drogon::Put: name = "PUT"; break;
        case drogon::Delete: name = "DELETE"; break;
        case drogon::Patch: name = "PATCH"; break;
        case drogon::Head: name = "HEAD"; break;
        case drogon::Options: name = "OPTIONS"; break;
        default: break;
    }
    return std::string(name) + " " + path;
}

}  // namespace usapptest::adminfilter

// ---- 断言宏：失败时额外打印请求描述，便于定位是哪一个探针 --------------
#define CHECK_ADMIN_REJECTED(result, expectedStatus, expectedCode, label)          \
    do {                                                                       \
        const int _usapp_before = usapptest::failedChecks();                   \
        const auto &_usapp_r = (result);                                       \
        CHECK(_usapp_r.completed);                                             \
        CHECK(_usapp_r.rejected);                                              \
        CHECK_EQ(_usapp_r.continueCalls, 0);                                   \
        CHECK_EQ(_usapp_r.status, (expectedStatus));                           \
        CHECK_EQ(_usapp_r.errorCode, std::string(expectedCode));               \
        if (usapptest::failedChecks() != _usapp_before) {                      \
            usapptest::note(std::string("  -> 失败探针: ") + (label));          \
        }                                                                      \
    } while (0)

#define CHECK_ADMIN_CONTINUED(result, label)                                    \
    do {                                                                       \
        const int _usapp_before = usapptest::failedChecks();                   \
        const auto &_usapp_r = (result);                                       \
        CHECK(_usapp_r.completed);                                             \
        CHECK(_usapp_r.continued);                                             \
        CHECK_EQ(_usapp_r.rejectCalls, 0);                                     \
        if (usapptest::failedChecks() != _usapp_before) {                      \
            usapptest::note(std::string("  -> 失败探针: ") + (label));          \
        }                                                                      \
    } while (0)
