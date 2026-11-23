// =====================================================================
// services/api/src/controllers/AvatarLoadoutController.cc
// Phase 4 B3 (docs/phase4-avatar-shop-design.md Section 7). One user route, AuthFilter, the caller only:
//   POST /api/v1/avatar/loadout   save the caller's whole six-slot loadout (Idempotency-Key; Origin and CSRF as every user write)
// The caller comes only from the session. Reading stays GET /api/v1/avatar (B1, extended in B3).
// =====================================================================

#include <drogon/HttpController.h>
#include "SharedRoomLive.h"

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AvatarLoadoutService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using L = AvatarLoadoutService;

void respond(const drogon::HttpRequestPtr &req, const Callback &callback, const L::Result &r) {
    switch (r.outcome) {
        case L::Outcome::Replayed: callback(jsonResponse(r.body, 200)); return;
        case L::Outcome::Created: callback(jsonResponse(r.body, 201)); return;
        case L::Outcome::Validation: callback(ctrl::fail(ErrorCode::ValidationError, r.message, req)); return;
        case L::Outcome::SpaceNotFound: callback(ctrl::fail(ErrorCode::SpaceNotFound, r.message, req)); return;
        case L::Outcome::NotFound: callback(ctrl::fail(ErrorCode::AvatarItemNotFound, r.message, req)); return;
        case L::Outcome::SlotInvalid: callback(ctrl::fail(ErrorCode::AvatarSlotInvalid, r.message, req)); return;
        case L::Outcome::NotOwned: callback(ctrl::fail(ErrorCode::AvatarNotOwned, r.message, req)); return;
        case L::Outcome::VersionConflict: callback(ctrl::fail(ErrorCode::AvatarVersionConflict, r.message, req)); return;
        case L::Outcome::DefaultsUnavailable: callback(ctrl::fail(ErrorCode::AvatarDefaultsUnavailable, r.message, req)); return;
        case L::Outcome::KeyReused: callback(ctrl::fail(ErrorCode::IdempotencyKeyReused, r.message, req)); return;
        case L::Outcome::ResultUnknown: callback(ctrl::fail(ErrorCode::ResultUnknown, r.message, req)); return;
        case L::Outcome::Error: break;
    }
    LOG_ERROR << "avatar loadout request failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AvatarLoadoutController : public drogon::HttpController<AvatarLoadoutController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AvatarLoadoutController::save, "/api/v1/avatar/loadout", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void save(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        L::Request request;
        std::string message;
        if (!L::parse(body, request, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        L::instance().save(userId, key, request, [req, callback, userId](L::Result r) {
            if (r.outcome == L::Outcome::Created) notifySharedRoomChange(userId);
            respond(req, callback, r);
        });
    }
};

}  // namespace usapp::controllers
