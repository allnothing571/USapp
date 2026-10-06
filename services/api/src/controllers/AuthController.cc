// =====================================================================
// services/api/src/controllers/AuthController.cc
//
// POST   /api/v1/auth/email-codes  发送一次性邮箱验证码      -> 202
// POST   /api/v1/auth/sessions     校验验证码并建立正式会话  -> 200
// DELETE /api/v1/auth/sessions     注销当前会话              -> 204
//
// 三个都是状态修改请求，因此都已经被 AuthFilter 强制要求：
//   精确 Origin + 有效会话 + 会话绑定的 CSRF 令牌（X-CSRF-Token）。
// 登录前用的是预认证会话，这正是"发码与登录也必须受保护"的落点。
//
// 防枚举（阶段 0 的硬约束，来自迁移注释与契约）：
//   * 发码阶段【不】区分邮箱是否在白名单，也【不】区分是否已注册，一律 202。
//     白名单只决定"该邮箱能否创建新账号"，在验证码校验阶段才产生 403。
//   * 登录失败的对外文案不区分"验证码错"与"验证码过期"。
//   * 控制器从不回显验证码明文，也不把它写进日志（MAIL_MODE=log 时由
//     Mailer 按既定格式打印，那是负责人有意的本地开发开关）。
//
// 契约缺口（已知，见 docs/phase0-handoff.md）：openapi.yaml 的这四个写端点
// 没有声明 requestBody schema，因此下面的请求体字段名是"实现与前端约定"，
// 而不是契约强制的。前端发的是 {"email": ...} 与 {"email": ..., "code": ...}。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>
#include <string>

#include "config/AppConfig.h"
#include "core/Crypto.h"
#include "core/Errors.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "mailer/Mailer.h"
#include "services/AuthService.h"
#include "services/SessionService.h"
#include "services/SpaceService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

// 从请求体里取邮箱并规范化。失败时已经回了 400，返回 false。
bool readCanonicalEmail(const drogon::HttpRequestPtr &req,
                        const Callback &callback,
                        std::string &out) {
    Json::Value body;
    if (!ctrl::parseBody(req, callback, body)) {
        return false;
    }
    const std::string rawEmail =
        body.isMember("email") && body["email"].isString() ? body["email"].asString() : "";
    crypto::CanonicalEmail canonical;
    if (!crypto::canonicalizeEmail(rawEmail, canonical)) {
        // 只回形状错误，不回"这个邮箱不存在/不在白名单"，避免枚举。
        callback(ctrl::fail(ErrorCode::ValidationError, "邮箱格式不正确", req));
        return false;
    }
    out = canonical.canonical;
    return true;
}

std::string readString(const Json::Value &body, const char *key) {
    return body.isMember(key) && body[key].isString() ? body[key].asString() : "";
}

}  // namespace

class AuthController : public drogon::HttpController<AuthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::sendEmailCode, "/api/v1/auth/email-codes", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AuthController::login, "/api/v1/auth/sessions", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AuthController::logout, "/api/v1/auth/sessions", drogon::Delete,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    // ------------------------------------------------------------------
    // 发送验证码。请求体 {"email": "..."}。
    // ------------------------------------------------------------------
    void sendEmailCode(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string sessionId = authctx::sessionId(req);
        if (sessionId.empty()) {
            // AuthFilter 已经拦过，这里是防御性的第二道。
            callback(ctrl::fail(ErrorCode::Unauthenticated, "请先建立会话", req));
            return;
        }
        std::string emailCanonical;
        if (!readCanonicalEmail(req, callback, emailCanonical)) {
            return;
        }

        const AppConfig &cfg = AppConfig::instance();
        const int resendCooldown = cfg.emailCodeResendCooldownSeconds;
        const int emailCodeTtl = cfg.emailCodeTtlSeconds;

        AuthService::instance().sendCode(
            emailCanonical, sessionId,
            [req, callback, emailCanonical, resendCooldown, emailCodeTtl](AuthService::SendResult result) {
                switch (result.outcome) {
                    case AuthService::SendOutcome::Accepted: {
                        usapp::mailer::Mailer::instance().sendVerificationCode(
                            emailCanonical, result.code, emailCodeTtl,
                            [req, callback, resendCooldown, emailCodeTtl](usapp::mailer::SendResult mailResult) {
                                if (!mailResult.ok) {
                                    LOG_WARN << "验证码邮件发送失败：" << mailResult.error
                                             << " request_id=" << http::requestId(req);
                                }
                                // 202：已受理，不保证邮件已投递，也不透露邮箱是否在白名单。
                                Json::Value body(Json::objectValue);
                                body["resend_after_seconds"] = resendCooldown;
                                body["expires_in_seconds"] = emailCodeTtl;
                                callback(jsonResponse(body, 202));
                            });
                        return;
                    }
                    case AuthService::SendOutcome::Throttled:
                        callback(errorResponse(ErrorCode::RateLimited,
                                               "请求过于频繁，请稍后再试", 0,
                                               http::requestId(req)));
                        return;
                    case AuthService::SendOutcome::Error:
                    default:
                        LOG_ERROR << "发送验证码失败：" << result.error
                                  << " request_id=" << http::requestId(req);
                        callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
                        return;
                }
            });
    }

    // ------------------------------------------------------------------
    // 登录 / 首次注册。请求体 {"email": "...", "code": "..."}。
    // 成功后 AuthService 已在同一事务内消费验证码、建/复用用户、
    // 新建正式会话并撤销旧预认证会话（会话标识与 CSRF 令牌都更换）。
    // ------------------------------------------------------------------
    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string preauthSessionId = authctx::sessionId(req);
        if (preauthSessionId.empty()) {
            callback(ctrl::fail(ErrorCode::Unauthenticated, "请先建立预认证会话", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) {
            return;
        }
        const std::string rawEmail = readString(body, "email");
        const std::string code = readString(body, "code");
        crypto::CanonicalEmail canonical;
        if (!crypto::canonicalizeEmail(rawEmail, canonical)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "邮箱格式不正确", req));
            return;
        }
        if (code.empty() || code.size() > 16) {
            callback(ctrl::fail(ErrorCode::ValidationError, "验证码格式不正确", req));
            return;
        }

        const AppConfig &cfg = AppConfig::instance();
        const std::string ua = SessionService::userAgentFamily(req->getHeader("user-agent"));
        AuthService::instance().loginWithCode(
            canonical.canonical, code, preauthSessionId, ua,
            [req, callback, &cfg](AuthService::LoginResult result) {
                switch (result.outcome) {
                    case AuthService::LoginOutcome::Success: {
                        ctrl::loadUserJson(
                            result.userId,
                            [req, callback, &cfg, result](Json::Value user) {
                                SpaceService::instance().getCurrentSpace(
                                    result.userId,
                                    [callback, &cfg, result, user](SpaceService::Status status,
                                                                   SpaceInfo space) {
                                        Json::Value body(Json::objectValue);
                                        body["authenticated"] = true;
                                        body["csrf_token"] = result.rawCsrfToken;
                                        body["user"] = user;
                                        // 新注册用户没有空间，这是正常状态而不是错误。
                                        body["space"] = status.ok()
                                                            ? ctrl::spaceJson(space)
                                                            : Json::Value(Json::nullValue);
                                        auto resp = jsonResponse(body, 200);
                                        // Cookie 里放的是新正式会话的明文令牌；
                                        // 旧预认证会话的 Cookie 值已随会话撤销失效。
                                        http::setSessionCookie(resp, cfg.sessionCookieName,
                                                               result.rawSessionToken,
                                                               cfg.authSessionTtlSeconds);
                                        callback(resp);
                                    });
                            },
                            [req, callback](const std::string &error) {
                                LOG_ERROR << "登录后读取用户失败：" << error
                                          << " request_id=" << http::requestId(req);
                                callback(ctrl::fail(ErrorCode::InternalError,
                                                    "服务内部错误", req));
                            });
                        return;
                    }
                    case AuthService::LoginOutcome::CodeInvalidOrExpired:
                        // 刻意不区分"错误"与"过期"，也不回显尝试次数上限之外的细节。
                        callback(errorResponse(ErrorCode::CodeInvalidOrExpired,
                                               "验证码无效或已过期", 0, http::requestId(req)));
                        return;
                    case AuthService::LoginOutcome::TooManyAttempts:
                        callback(errorResponse(ErrorCode::RateLimited,
                                               "尝试次数过多，请重新获取验证码", 0,
                                               http::requestId(req)));
                        return;
                    case AuthService::LoginOutcome::RegistrationNotAllowed:
                        // 只有"未注册且不在白名单"才会走到这里：此时验证码已经校验通过，
                        // 因此这不算泄露邮箱是否已注册之外的信息。
                        callback(errorResponse(ErrorCode::RegistrationNotAllowed,
                                               "该邮箱不在注册白名单内", 0,
                                               http::requestId(req)));
                        return;
                    case AuthService::LoginOutcome::NoSession:
                        callback(ctrl::fail(ErrorCode::Unauthenticated,
                                            "预认证会话已失效，请重新载入页面", req));
                        return;
                    case AuthService::LoginOutcome::Error:
                    default:
                        LOG_ERROR << "登录失败：" << result.error
                                  << " request_id=" << http::requestId(req);
                        callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
                        return;
                }
            });
    }

    // ------------------------------------------------------------------
    // 注销。无论服务端撤销成功与否，都必须清掉浏览器 Cookie：
    // 否则用户会卡在"看起来还登录着"的状态里。撤销本身是幂等的。
    // ------------------------------------------------------------------
    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const AppConfig &cfg = AppConfig::instance();
        const std::string sessionId = authctx::sessionId(req);

        auto respondCleared = [callback, &cfg]() {
            auto resp = noContentResponse();
            http::clearSessionCookie(resp, cfg.sessionCookieName);
            callback(resp);
        };

        if (sessionId.empty()) {
            respondCleared();
            return;
        }
        SessionService::instance().revokeSession(
            sessionId, "user_logout", respondCleared,
            [req, respondCleared](const std::string &error) {
                LOG_ERROR << "撤销会话失败（仍清空客户端 Cookie）：" << error
                          << " request_id=" << http::requestId(req);
                respondCleared();
            });
    }
};

}  // namespace usapp::controllers
