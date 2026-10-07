// =====================================================================
// services/api/src/controllers/SessionController.cc
//
// GET /api/v1/session —— 会话引导，同时是 CSRF 令牌的唯一获取入口。
// GET /api/v1/me      —— 当前用户与所属空间。
// PATCH /api/v1/me    —— 修改调用者自己的昵称（阶段 1 M1），成功返回与 GET 相同的 MeResponse。
//
// PATCH 的身份只来自会话（AuthFilter 写入的 user_id）。请求体只允许一个
// 字符串字段 display_name；任何其他字段（包括试图指定目标用户的字段）都按
// 400 拒绝，而不是被静默忽略。Origin 与 CSRF 由 AuthFilter 按状态修改请求校验。
//
// 为什么引导接口这么重要：
//   浏览器内存里的 CSRF 令牌在页面刷新后就没了，而数据库只存令牌的 SHA-256
//   （不可反推）。所以引导接口是这个小程序里唯一能把明文令牌交给同一个
//   浏览器的地方；没有它，任何写请求都无法通过 CSRF 校验。
//
// 预认证会话（登录前）也是在这里创建的：
//   发码（POST /auth/email-codes）与登录（POST /auth/sessions）都是状态修改
//   请求，都必须带 CSRF 令牌，而令牌必须依附一个会话。因此"未登录"不等于
//   "无会话"：未登录时先来一次 GET /api/v1/session 拿到预认证会话与令牌，
//   再用它去发码登录。
//
// 安全：引导接口本身是安全方法（GET），不需要 CSRF；它不会透露任何
//   他人信息，未登录时只回自己的（空）状态与一枚新令牌。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>
#include <string>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/ProfileService.h"
#include "services/SessionService.h"
#include "services/SpaceService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

// 创建一个全新的预认证会话，并把 Cookie 与明文 CSRF 令牌一并交给浏览器。
// 这是"登录前也受保护"的起点。
void respondWithNewPreauthSession(const drogon::HttpRequestPtr &req, const Callback &callback) {
    const AppConfig &cfg = AppConfig::instance();
    SessionService::instance().createPreauthSession(
        SessionService::userAgentFamily(req->getHeader("user-agent")),
        [callback, &cfg](const std::string & /*sessionId*/,
                         const std::string &rawToken,
                         const std::string &rawCsrfToken) {
            Json::Value body(Json::objectValue);
            body["authenticated"] = false;
            body["csrf_token"] = rawCsrfToken;
            body["pre_auth_expires_at"] = ctrl::nowPlusSecondsIso(cfg.preauthSessionTtlSeconds);
            auto resp = jsonResponse(body, 200);
            http::setSessionCookie(resp, cfg.sessionCookieName, rawToken,
                                   cfg.preauthSessionTtlSeconds);
            callback(resp);
        },
        [req, callback](const std::string &error) {
            LOG_ERROR << "创建预认证会话失败：" << error
                      << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
        });
}

// MeResponse：{ user, space | null }。GET /me 与 PATCH /me 成功后共用。
void respondWithMe(const drogon::HttpRequestPtr &req, const Callback &callback, const std::string &userId) {
    ctrl::loadUserJson(
        userId,
        [callback, userId](Json::Value user) {
            SpaceService::instance().getCurrentSpace(
                userId,
                [callback, user](SpaceService::Status status, SpaceInfo space) {
                    Json::Value body(Json::objectValue);
                    body["user"] = user;
                    body["space"] = status.ok() ? ctrl::spaceJson(space)
                                                : Json::Value(Json::nullValue);
                    callback(jsonResponse(body, 200));
                });
        },
        [req, callback](const std::string &error) {
            LOG_ERROR << "读取当前用户失败：" << error
                      << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
        });
}

}  // namespace

class SessionController : public drogon::HttpController<SessionController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SessionController::bootstrap, "/api/v1/session", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(SessionController::me, "/api/v1/me", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(SessionController::updateMe, "/api/v1/me", drogon::Patch,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    void bootstrap(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string sessionId = authctx::sessionId(req);
        if (sessionId.empty()) {
            // 没有任何会话（首次访问，或 Cookie 已被清掉）。
            respondWithNewPreauthSession(req, callback);
            return;
        }

        // 已有会话：库中只有 CSRF 的散列，明文不可反推，所以必须轮换一枚新令牌。
        // 代价（同一会话多标签页互相顶掉）见 SessionService::rotateCsrfToken 注释。
        SessionService::instance().rotateCsrfToken(
            sessionId,
            [req, callback](const std::string &rawCsrfToken) {
                if (rawCsrfToken.empty()) {
                    // 会话在过滤器放行之后失效（竞态）。按"没有会话"处理：
                    // 给一个新的预认证会话，而不是报错。
                    respondWithNewPreauthSession(req, callback);
                    return;
                }
                if (!authctx::isAuthenticated(req)) {
                    // 预认证会话：没有用户与空间可回。
                    Json::Value body(Json::objectValue);
                    body["authenticated"] = false;
                    body["csrf_token"] = rawCsrfToken;
                    const std::string expiresAt = authctx::expiresAt(req);
                    if (!expiresAt.empty()) {
                        body["pre_auth_expires_at"] = expiresAt;
                    }
                    callback(jsonResponse(body, 200));
                    return;
                }

                const std::string userId = authctx::userId(req);
                ctrl::loadUserJson(
                    userId,
                    [callback, userId, rawCsrfToken](Json::Value user) {
                        SpaceService::instance().getCurrentSpace(
                            userId,
                            [callback, user, rawCsrfToken](SpaceService::Status status,
                                                           SpaceInfo space) {
                                Json::Value body(Json::objectValue);
                                body["authenticated"] = true;
                                body["csrf_token"] = rawCsrfToken;
                                body["user"] = user;
                                // 没有空间不是错误：刚注册的用户就是这种状态。
                                body["space"] = status.ok()
                                                    ? ctrl::spaceJson(space)
                                                    : Json::Value(Json::nullValue);
                                callback(jsonResponse(body, 200));
                            });
                    },
                    [req, callback](const std::string &error) {
                        LOG_ERROR << "会话引导读取用户失败：" << error
                                  << " request_id=" << http::requestId(req);
                        callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
                    });
            },
            [req, callback](const std::string &error) {
                LOG_ERROR << "会话引导轮换 CSRF 令牌失败：" << error
                          << " request_id=" << http::requestId(req);
                callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            });
    }

    void me(const drogon::HttpRequestPtr &req,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        respondWithMe(req, callback, userId);
    }

    // 只改会话用户自己的昵称。昵称值不进日志、不进审计。
    void updateMe(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) {
            return;
        }
        if (body.size() != 1u || !body.isMember("display_name") || !body["display_name"].isString()) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "请求体只能包含一个字符串字段 display_name", req));
            return;
        }
        const std::string rawName = body["display_name"].asString();

        ProfileService::instance().updateDisplayName(
            userId, rawName, [req, callback, userId](ProfileService::Status status) {
                switch (status.outcome) {
                    case ProfileService::Outcome::Ok:
                        // 已确认提交，再读回最新的用户与空间。
                        respondWithMe(req, callback, userId);
                        return;
                    case ProfileService::Outcome::Validation:
                        callback(ctrl::fail(ErrorCode::ValidationError,
                                            ProfileService::displayNameErrorMessage(status.validation),
                                            req));
                        return;
                    case ProfileService::Outcome::NotFound:
                        LOG_WARN << "修改昵称时会话用户不存在 request_id=" << http::requestId(req);
                        callback(ctrl::fail(ErrorCode::Unauthenticated, "请先登录", req));
                        return;
                    case ProfileService::Outcome::Error:
                    default:
                        LOG_ERROR << "修改昵称失败：" << status.detail
                                  << " request_id=" << http::requestId(req);
                        callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
                        return;
                }
            });
    }
};

}  // namespace usapp::controllers
