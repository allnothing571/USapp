// =====================================================================
// services/api/src/controllers/AvatarPurchaseController.cc
// Phase 4 B2 (docs/phase4-avatar-shop-design.md Section 6). User routes, AuthFilter, the caller only:
//   POST /api/v1/avatar/purchases   buy one paid item with sunshine (Idempotency-Key; Origin and CSRF as every user write)
//   GET  /api/v1/avatar/owned       the caller's own paid items, paginated
// No route names another user, equips, refunds or transfers.
// =====================================================================

#include <drogon/HttpController.h>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AvatarPurchaseService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using P = AvatarPurchaseService;

void respond(const drogon::HttpRequestPtr &req, const Callback &callback, const P::Result &r) {
    switch (r.outcome) {
        case P::Outcome::Ok:
        case P::Outcome::Replayed: callback(jsonResponse(r.body, 200)); return;
        case P::Outcome::Created: callback(jsonResponse(r.body, 201)); return;
        case P::Outcome::Validation: callback(ctrl::fail(ErrorCode::ValidationError, r.message, req)); return;
        case P::Outcome::SpaceNotFound: callback(ctrl::fail(ErrorCode::SpaceNotFound, r.message, req)); return;
        case P::Outcome::NotFound: callback(ctrl::fail(ErrorCode::AvatarItemNotFound, r.message, req)); return;
        case P::Outcome::Unavailable: callback(ctrl::fail(ErrorCode::AvatarItemUnavailable, r.message, req)); return;
        case P::Outcome::VersionConflict: callback(ctrl::fail(ErrorCode::AvatarVersionConflict, r.message, req)); return;
        case P::Outcome::AlreadyOwned: callback(ctrl::fail(ErrorCode::AvatarAlreadyOwned, r.message, req)); return;
        case P::Outcome::Insufficient: callback(ctrl::fail(ErrorCode::InsufficientBalance, r.message, req)); return;
        case P::Outcome::KeyReused: callback(ctrl::fail(ErrorCode::IdempotencyKeyReused, r.message, req)); return;
        case P::Outcome::ResultUnknown: callback(ctrl::fail(ErrorCode::ResultUnknown, r.message, req)); return;
        case P::Outcome::Error: break;
    }
    LOG_ERROR << "avatar purchase request failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AvatarPurchaseController : public drogon::HttpController<AvatarPurchaseController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AvatarPurchaseController::purchase, "/api/v1/avatar/purchases", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AvatarPurchaseController::owned, "/api/v1/avatar/owned", drogon::Get, AuthFilter::classTypeName());
    METHOD_LIST_END

    void purchase(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        P::Request request;
        std::string message;
        if (!P::parse(body, request, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        P::instance().purchase(userId, key, request, [req, callback](P::Result r) { respond(req, callback, r); });
    }

    void owned(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        int limit = 50;
        const auto rawLimit = req->getParameter("limit");
        if (!rawLimit.empty()) {
            if (rawLimit.size() > 3 || rawLimit.find_first_not_of("0123456789") != std::string::npos) {
                callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100", req));
                return;
            }
            limit = std::stoi(rawLimit);
        }
        P::instance().owned(userId, limit, req->getParameter("cursor"), [req, callback](P::Result r) { respond(req, callback, r); });
    }
};

}  // namespace usapp::controllers
