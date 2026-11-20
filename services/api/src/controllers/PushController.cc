// =====================================================================
// services/api/src/controllers/PushController.cc
//
// Phase 2 P2-5a: Web Push device subscriptions (contract 0.19.0; docs/phase2-web-push.md
// Section 9). Every route carries AuthFilter: writes are checked for exact Origin and the
// session-bound CSRF token by the filter; reads need a formal session. The user and the
// session come only from the filter; space membership is checked server-side.
//
// Gate: registration is available only when PUSH_SUBSCRIBE_ENABLED=true, a valid
// PUSH_VAPID_PUBLIC_KEY is configured AND the worker transport is configured (P2-5c). All are off
// by default, so registration answers 409 PUSH_UNAVAILABLE; list and revoke stay available. The
// browser separately needs an ACTIVE service worker with push:v1 before it offers anything.
// No endpoint or key is ever logged or returned in a list.
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <string>

#include "config/AppConfig.h"
#include "core/Env.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/PushService.h"
#include "services/PushWorkerTransport.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class PushController : public drogon::HttpController<PushController> {
  public:
    static constexpr size_t kMaxBodyBytes = 4096;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PushController::getConfig, "/api/v1/push/config", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(PushController::listSubscriptions, "/api/v1/push/subscriptions", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(PushController::registerSubscription, "/api/v1/push/subscriptions", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(PushController::revokeSubscription, "/api/v1/push/subscriptions/{generationId}/revoke",
                  drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void getConfig(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto &cfg = config();
        Json::Value body(Json::objectValue);
        body["subscribe_available"] = cfg.subscribeAvailable;
        body["vapid_public_key"] =
            cfg.subscribeAvailable ? Json::Value(cfg.vapidPublicKey) : Json::Value(Json::nullValue);
        body["device_limit"] = PushService::kMaxActiveDevices;
        callback(jsonResponse(body, 200));
    }

    void listSubscriptions(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string sessionId;
        if (!requireSession(req, callback, userId, sessionId)) return;
        PushService::instance().list(userId, sessionId, config().vapidKeyId,
                                     [req, callback](PushService::ListResult result) {
                                         if (result.outcome != PushService::Outcome::Ok) {
                                             PushService::Status status;
                                             status.outcome = result.outcome;
                                             status.message = result.message;
                                             status.detail = result.detail;
                                             respond(req, callback, status, "读取推送设备");
                                             return;
                                         }
                                         // devices is complete (every occupied slot); history is bounded.
                                         Json::Value body(Json::objectValue);
                                         Json::Value devices(Json::arrayValue);
                                         Json::Value history(Json::arrayValue);
                                         for (const auto &g : result.devices) {
                                             devices.append(PushService::generationJson(g, false));
                                         }
                                         for (const auto &g : result.history) {
                                             history.append(PushService::generationJson(g, false));
                                         }
                                         body["devices"] = devices;
                                         body["history"] = history;
                                         body["occupied_slots"] = static_cast<int>(result.devices.size());
                                         body["device_limit"] = PushService::kMaxActiveDevices;
                                         callback(jsonResponse(body, 200));
                                     });
    }

    void registerSubscription(const drogon::HttpRequestPtr &req,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string sessionId;
        if (!requireSession(req, callback, userId, sessionId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        const auto &cfg = config();
        if (!cfg.subscribeAvailable) {
            callback(ctrl::fail(ErrorCode::PushUnavailable, "当前无法开启设备提醒", req));
            return;
        }
        if (req->body().size() > kMaxBodyBytes) {
            callback(ctrl::fail(ErrorCode::ValidationError, "请求体过大", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        PushService::Registration registration;
        std::string message;
        if (!PushService::parseRegistration(body, registration, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        PushService::instance().registerSubscription(
            userId, sessionId, key, registration, cfg.vapidKeyId, AppConfig::instance().sessionSecret,
            [req, callback](PushService::Status status) { respond(req, callback, status, "开启设备提醒"); });
    }

    void revokeSubscription(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                            std::string generationId) {
        std::string userId;
        std::string sessionId;
        if (!requireSession(req, callback, userId, sessionId)) return;
        if (!ctrl::looksLikeUuid(generationId)) {
            callback(ctrl::fail(ErrorCode::PushSubscriptionNotFound, "设备不存在", req));
            return;
        }
        PushService::instance().revoke(
            userId, sessionId, generationId, config().vapidKeyId,
            [req, callback](PushService::Status status) { respond(req, callback, status, "关闭设备提醒"); });
    }

  private:
    // Read once; invalid key material is treated as not configured (never logged).
    static const PushService::Config &config() {
        static const PushService::Config cfg = PushService::configFrom(
            env::getBool("PUSH_SUBSCRIBE_ENABLED", false), env::get("PUSH_VAPID_PUBLIC_KEY"),
            push::workerConfigFromEnv().has_value());
        return cfg;
    }

    static bool requireSession(const drogon::HttpRequestPtr &req,
                               const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                               std::string &userId, std::string &sessionId) {
        if (!ctrl::requireUser(req, callback, userId)) return false;
        sessionId = authctx::sessionId(req);
        if (sessionId.empty()) {
            callback(ctrl::fail(ErrorCode::Unauthenticated, "请先登录", req));
            return false;
        }
        return true;
    }

    static void respond(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const PushService::Status &status, const char *action) {
        if (status.success()) {
            callback(jsonResponse(PushService::responseBody(status), PushService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == PushService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试", req));
            return;
        }
        if (status.outcome == PushService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(PushService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
