// =====================================================================
// services/api/src/http/AuthFilter.cc
// =====================================================================
#include "http/AuthFilter.h"

#include <drogon/drogon.h>

#include <any>
#include <memory>
#include <string>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "http/HttpUtil.h"
#include "services/SessionService.h"

namespace usapp::authctx {

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

std::string sessionId(const drogon::HttpRequestPtr &req) {
    std::string value;
    readAttribute(req, kSessionId, value);
    return value;
}

bool isPreauth(const drogon::HttpRequestPtr &req) {
    bool value = false;
    return readAttribute(req, kPreauth, value) && value;
}

std::string expiresAt(const drogon::HttpRequestPtr &req) {
    std::string value;
    readAttribute(req, kExpiresAt, value);
    return value;
}

}  // namespace usapp::authctx

namespace usapp {

namespace {

void reject(const drogon::FilterCallback &fcb,
            const drogon::HttpRequestPtr &req,
            ErrorCode code,
            const std::string &message) {
    fcb(errorResponse(code, message, 0, http::requestId(req)));
}

// 把会话状态写进请求属性。属性值是 std::any，读取端只按同一组键取值。
void attachSession(const drogon::HttpRequestPtr &req, const SessionRecord &session) {
    // req->attributes() 返回 AttributesPtr 的引用，用auto & 接收。
    // 写入统一走 Attributes::insert(key, value) 这个两参数接口。
    auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return;
    }
    attributes->insert(authctx::kSessionId, session.id);
    attributes->insert(authctx::kAuthenticated, session.authenticated);
    attributes->insert(authctx::kPreauth, !session.authenticated);
    attributes->insert(authctx::kExpiresAt, session.expiresAt);
    if (session.authenticated && !session.userId.empty()) {
        attributes->insert(authctx::kUserId, session.userId);
    }
}

}  // namespace

void AuthFilter::doFilter(const drogon::HttpRequestPtr &req,
                          drogon::FilterCallback &&fcb,
                          drogon::FilterChainCallback &&ccb) {
    const auto &cfg = AppConfig::instance();
    const bool stateChanging = http::isStateChangingMethod(req->method());

    // ---- 1) Origin：状态修改请求必须精确同源 ----
    if (stateChanging) {
        const std::string origin = http::requestOrigin(req);
        if (!http::originMatches(origin, cfg.publicOrigin)) {
            LOG_WARN << "拒绝跨源状态修改请求 path=" << req->path() << " origin=" << origin
                     << " request_id=" << http::requestId(req);
            reject(fcb, req, ErrorCode::OriginNotAllowed, "请求来源不被允许");
            return;
        }
    }

    // ---- 2) 会话 ----
    const std::string rawToken = http::readCookie(req, cfg.sessionCookieName);
    if (rawToken.empty()) {
        if (stateChanging) {
            // 没有会话就没有可绑定的 CSRF 令牌，状态修改一律拒绝。
            // 前端必须先 GET /api/v1/session 拿到预认证会话与 CSRF 令牌。
            reject(fcb, req, ErrorCode::Unauthenticated, "缺少会话，请先获取会话与 CSRF 令牌");
            return;
        }
        // 安全方法（GET/HEAD/OPTIONS）放行：控制器自己决定要不要匿名。
        ccb();
        return;
    }

    SessionService::instance().loadByRawToken(
        rawToken,
        [req, stateChanging, fcb = std::move(fcb), ccb = std::move(ccb)](
            std::optional<SessionRecord> session) {
            if (!session.has_value()) {
                // 令牌无效/已撤销/已过期：按匿名处理，绝不延长或复活。
                if (stateChanging) {
                    reject(fcb, req, ErrorCode::Unauthenticated, "会话无效或已过期");
                    return;
                }
                ccb();
                return;
            }

            if (stateChanging) {
                const std::string provided = req->getHeader("x-csrf-token");
                if (!http::requiresCsrf(req->method()) ||
                    !SessionService::verifyCsrf(*session, provided)) {
                    LOG_WARN << "CSRF 校验失败 path=" << req->path() << " method=" << req->methodString()
                             << " request_id=" << http::requestId(req);
                    reject(fcb, req, ErrorCode::CsrfTokenInvalid, "CSRF 令牌缺失或不匹配");
                    return;
                }
            } else {
                // 安全方法顺手刷新 last_seen_at（尽力而为，失败不影响请求）。
                SessionService::instance().touchSession(session->id);
            }

            attachSession(req, *session);
            ccb();
        });
}

}  // namespace usapp
