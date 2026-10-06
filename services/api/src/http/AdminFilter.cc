// =====================================================================
// services/api/src/http/AdminFilter.cc
// =====================================================================
#include "http/AdminFilter.h"

#include <drogon/drogon.h>

#include <any>
#include <memory>
#include <string>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "http/HttpUtil.h"
#include "services/AdminAuthService.h"

namespace usapp::adminctx {

namespace {

// Drogon v1.9.11 的 Attributes 是一个封装类（drogon/Attribute.h），不是
// std::unordered_map，因此只能使用它公开的接口：
//   * bool find(key)            —— 只回答"键是否存在"，不返回迭代器
//   * std::any &operator[](key) —— 返回 std::any 的引用，键缺失时会【插入】默认值
//   * void insert(key, value)   —— 两参数写入
// 该类没有公开 end()，所以不能写 attributes->end()。
//
// req->attributes() 返回的是 AttributesPtr（= std::shared_ptr<Attributes>）的引用，
// 因此这里用 auto & 接收；写成 auto * 无法从引用绑定出指针。
const std::any *findAttribute(const drogon::HttpRequestPtr &req, const char *key) {
    const auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return nullptr;
    }
    // 先查存在性：缺失时直接返回，绝不走到 operator[]，
    // 否则读取一个不存在的键会向请求里插入一个空的 std::any，
    // 让"属性缺失"与"属性存在但为空"变得无法区分。
    if (!attributes->find(key)) {
        return nullptr;
    }
    // 键已存在，operator[] 不会插入任何东西。这里返回的是请求自己持有的
    // 那个 std::any 的地址，生命周期与 req 一致，不是局部副本的地址。
    //
    // 关于 shared_ptr 按值拷贝：拷贝与原对象共享同一个 Attributes，销毁局部
    // 副本本身不会让对象失效（请求仍持有一份），所以按值拷贝也不会必然悬空；
    // 真正决定有效性的是请求的生命周期。用引用只是避免多一次引用计数增减。
    return &(*attributes)[key];
}

template <typename T>
bool readAttribute(const drogon::HttpRequestPtr &req, const char *key, T &out) {
    const auto *value = findAttribute(req, key);
    if (value == nullptr) {
        return false;
    }
    try {
        out = std::any_cast<T>(*value);
        return true;
    } catch (const std::bad_any_cast &) {
        return false;
    }
}

}  // namespace

bool isAuthenticated(const drogon::HttpRequestPtr &req) {
    bool value = false;
    return readAttribute(req, kAuthenticated, value) && value;
}

std::string userId(const drogon::HttpRequestPtr &req) {
    std::string value;
    readAttribute(req, kUserId, value);
    return value;
}

std::string username(const drogon::HttpRequestPtr &req) {
    std::string value;
    readAttribute(req, kUsername, value);
    return value;
}

std::string sessionId(const drogon::HttpRequestPtr &req) {
    std::string value;
    readAttribute(req, kSessionId, value);
    return value;
}

}  // namespace usapp::adminctx

namespace usapp {

namespace {

// 管理面的登录路由。它是唯一"允许没有会话"的状态修改路由。
constexpr const char *kLoginPath = "/api/admin/v1/sessions";

// 未认证即可通过本过滤器的管理路由：精确的"方法 + 路径"白名单，默认拒绝。
//
// 只有这三项。其余所有管理请求——包括 GET 列表这类只读请求——都必须持有
// 有效的管理员会话，否则 401 UNAUTHENTICATED，且绝不调用 FilterChainCallback
// （即不会进入控制器）。
//
// 刻意不做前缀匹配、不做大小写归一、不容忍尾斜杠：任何与下表不逐字相等的
// 方法或路径（如 /api/admin/v1/health/、/api/admin/v1/sessionx、HEAD /health、
// GET /sessions）一律按受保护路由处理。白名单只决定"缺少会话时是否放行"，
// 不跳过前面的对端地址、入口标记以及登录的 Origin + 自定义头校验。
struct AnonymousAdminRoute {
    drogon::HttpMethod method;
    const char *path;
};

constexpr AnonymousAdminRoute kAnonymousAdminRoutes[] = {
    {drogon::Get, "/api/admin/v1/health"},   // 存活探针
    {drogon::Get, "/api/admin/v1/session"},  // 未登录引导：authenticated=false、csrf_token=""
    {drogon::Post, kLoginPath},              // 登录
};

bool allowsAnonymous(const drogon::HttpRequestPtr &req) {
    const drogon::HttpMethod method = req->method();
    const std::string &path = req->path();
    for (const auto &route : kAnonymousAdminRoutes) {
        if (method == route.method && path == route.path) {
            return true;
        }
    }
    return false;
}

void reject(const drogon::FilterCallback &fcb,
            const drogon::HttpRequestPtr &req,
            ErrorCode code,
            const std::string &message) {
    fcb(errorResponse(code, message, 0, http::requestId(req)));
}

// 对端不在白名单时按 404 处理：不告诉任何人"这里存在一个管理入口"。
void rejectAsNotFound(const drogon::FilterCallback &fcb, const drogon::HttpRequestPtr &req) {
    fcb(errorResponse(ErrorCode::NotFound, "资源不存在", 0, http::requestId(req)));
}

void attachSession(const drogon::HttpRequestPtr &req, const AdminSessionRecord &session) {
    // req->attributes() 返回 AttributesPtr 的引用，用 auto & 接收。
    // 写入统一走 Attributes::insert(key, value) 这个两参数接口。
    auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return;
    }
    attributes->insert(adminctx::kSessionId, session.id);
    attributes->insert(adminctx::kUserId, session.adminUserId);
    attributes->insert(adminctx::kUsername, session.username);
    attributes->insert(adminctx::kAuthenticated, true);
}

bool isLoginRequest(const drogon::HttpRequestPtr &req) {
    return req->method() == drogon::Post && req->path() == kLoginPath;
}

// "没有可用的管理员会话"时的唯一出口：缺 Cookie，或 Cookie 对应的会话
// 不存在 / 已过期 / 已撤销 / 加载失败（fail closed）都走这里。
//
// 受保护的管理读请求（用户 / 空间 / 审计日志列表等）同样需要认证：只读不等于
// 公开。只有 kAnonymousAdminRoutes 中逐字匹配的方法 + 路径才会继续，且不附加
// 任何会话属性——引导接口据此回答 authenticated=false 与空 csrf_token，
// 不会为管理面创建任何"预认证会话"。注销（DELETE /sessions）不在白名单内，
// 仍是受保护的写请求。
void continueWithoutSession(const drogon::HttpRequestPtr &req,
                            const drogon::FilterCallback &fcb,
                            const drogon::FilterChainCallback &ccb) {
    if (!allowsAnonymous(req)) {
        LOG_INFO << "管理面拒绝未认证请求 path=" << req->path()
                 << " request_id=" << http::requestId(req);
        reject(fcb, req, ErrorCode::Unauthenticated, "管理员未登录");
        return;
    }
    ccb();
}

}  // namespace

void AdminFilter::doFilter(const drogon::HttpRequestPtr &req,
                           drogon::FilterCallback &&fcb,
                           drogon::FilterChainCallback &&ccb) {
    const auto &cfg = AppConfig::instance();
    const bool stateChanging = http::isStateChangingMethod(req->method());

    // ---- 1) 对端地址白名单 ----
    const std::string peer = http::peerIp(req);
    if (!http::ipInAnyCidr(peer, cfg.adminAllowedCidrs)) {
        LOG_WARN << "管理面拒绝非白名单对端 peer=" << peer << " path=" << req->path()
                 << " request_id=" << http::requestId(req);
        rejectAsNotFound(fcb, req);
        return;
    }

    // ---- 2) 管理面入口标记（不依赖反向代理配置正确）----
    // 公开监听器上同样注册着 /api/admin/** 路由，所以必须有一样东西能证明
    // "这个请求确实是从管理入口进来的"。管理站点注入该头，公网站点剥除
    // 客户端伪造的同名头（deploy/Caddyfile）。缺头一律 403（契约里管理面
    // 声明的正是 401/403）。
    if (req->getHeader(adminctx::kSurfaceHeader) != adminctx::kSurfaceValue) {
        LOG_WARN << "管理面拒绝无入口标记的请求 path=" << req->path()
                 << " request_id=" << http::requestId(req);
        reject(fcb, req, ErrorCode::OriginNotAllowed, "该入口不提供管理接口");
        return;
    }

    // ---- 3) 状态修改：精确 Origin + 自定义头 ----
    if (stateChanging) {
        const std::string origin = http::requestOrigin(req);
        if (!http::originMatches(origin, cfg.adminOrigin)) {
            LOG_WARN << "管理面拒绝跨源请求 path=" << req->path() << " origin=" << origin
                     << " request_id=" << http::requestId(req);
            reject(fcb, req, ErrorCode::OriginNotAllowed, "请求来源不被允许");
            return;
        }
        if (req->getHeader(adminctx::kRequestedByHeader) != adminctx::kRequestedByValue) {
            // 跨站表单没法设置自定义头（要设置就得先过 CORS 预检，而我们不放行任何跨源）。
            LOG_WARN << "管理面缺少 " << adminctx::kRequestedByHeader
                     << " 头 path=" << req->path() << " request_id=" << http::requestId(req);
            reject(fcb, req, ErrorCode::CsrfTokenInvalid, "缺少管理面请求标记");
            return;
        }
    }

    // ---- 4) 管理员会话（默认拒绝）----
    // 除 kAnonymousAdminRoutes 中的精确白名单外，每个管理请求（读写皆然）
    // 都必须持有有效的管理员会话；缺失 / 无效 / 过期 / 已撤销一律 401 并立即终止。
    const std::string rawToken = http::readCookie(req, cfg.adminSessionCookieName);
    if (rawToken.empty()) {
        continueWithoutSession(req, fcb, ccb);
        return;
    }

    AdminAuthService::instance().loadByRawToken(
        rawToken,
        [req, stateChanging, fcb = std::move(fcb), ccb = std::move(ccb)](
            std::optional<AdminSessionRecord> session) {
            if (!session.has_value()) {
                // 会话无效/过期/撤销/加载失败：Cookie 还在，但按未登录处理。
                continueWithoutSession(req, fcb, ccb);
                return;
            }

            // 登录路由即使带着旧会话也不再校验 CSRF（旧会话可能是过期的
            // 浏览器残留），它已有 Origin + 自定义头两层保护。
            if (stateChanging && !isLoginRequest(req)) {
                const std::string provided = req->getHeader("x-csrf-token");
                if (!AdminAuthService::verifyCsrf(*session, provided)) {
                    LOG_WARN << "管理面 CSRF 校验失败 path=" << req->path()
                             << " request_id=" << http::requestId(req);
                    reject(fcb, req, ErrorCode::CsrfTokenInvalid, "CSRF 令牌缺失或不匹配");
                    return;
                }
            } else if (!stateChanging) {
                AdminAuthService::instance().touchSession(session->id);
            }

            attachSession(req, *session);
            ccb();
        });
}

}  // namespace usapp
