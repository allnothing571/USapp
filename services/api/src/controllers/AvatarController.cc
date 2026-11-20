// =====================================================================
// services/api/src/controllers/AvatarController.cc
// Phase 4 B1 avatar catalog (docs/phase4-avatar-shop-design.md).
//
// User routes (AuthFilter, a user session):
//   GET  /api/v1/avatar/catalog                          listed items, current revision
//   GET  /api/v1/avatar                                  the caller's appearance (B1: starter defaults)
// Admin routes (AdminFilter: peer CIDR, admin surface header, exact Origin + X-Requested-By and the admin session's CSRF
// token for writes, an admin session for everything):
//   GET  /api/admin/v1/avatar/assets                     the approved asset registry
//   GET  /api/admin/v1/avatar/items                      every item, listed or not
//   GET  /api/admin/v1/avatar/items/{itemId}             one item with its revisions
//   GET  /api/admin/v1/avatar/starter                    the current starter configuration, or null
//   POST /api/admin/v1/avatar/items                      create (Idempotency-Key)
//   POST /api/admin/v1/avatar/items/{itemId}/revisions   rename, correct asset, reprice, list or unlist (Idempotency-Key)
//   POST /api/admin/v1/avatar/starter                    a new starter configuration (Idempotency-Key)
// No user route writes the catalog, and no admin route reads a user's appearance.
// =====================================================================

#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AvatarService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using S = AvatarService;

void respond(const drogon::HttpRequestPtr &req, const Callback &callback, const S::Result &r) {
    switch (r.outcome) {
        case S::Outcome::Ok:
        case S::Outcome::Replayed: callback(jsonResponse(r.body, 200)); return;
        case S::Outcome::Created: callback(jsonResponse(r.body, 201)); return;
        case S::Outcome::Validation: callback(ctrl::fail(ErrorCode::ValidationError, r.message, req)); return;
        case S::Outcome::NotFound: callback(ctrl::fail(ErrorCode::AvatarItemNotFound, r.message, req)); return;
        case S::Outcome::VersionConflict: callback(ctrl::fail(ErrorCode::AvatarVersionConflict, r.message, req)); return;
        case S::Outcome::AssetInvalid: callback(ctrl::fail(ErrorCode::AvatarAssetInvalid, r.message, req)); return;
        case S::Outcome::StarterInvalid: callback(ctrl::fail(ErrorCode::AvatarStarterInvalid, r.message, req)); return;
        case S::Outcome::KeyReused: callback(ctrl::fail(ErrorCode::IdempotencyKeyReused, r.message, req)); return;
        case S::Outcome::ResultUnknown: callback(ctrl::fail(ErrorCode::ResultUnknown, r.message, req)); return;
        case S::Outcome::Error: break;
    }
    LOG_ERROR << "avatar request failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

// The admin's identity comes only from the session AdminFilter attached; writes also need a valid Idempotency-Key.
bool adminWrite(const drogon::HttpRequestPtr &req, const Callback &callback, std::string &adminId, std::string &key, Json::Value &body) {
    adminId = adminctx::userId(req);
    if (adminId.empty()) {
        callback(ctrl::fail(ErrorCode::Unauthenticated, "管理员未登录", req));
        return false;
    }
    key = req->getHeader("idempotency-key");
    if (!TaskService::validIdempotencyKey(key)) {
        callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
        return false;
    }
    return ctrl::parseBody(req, callback, body);
}

}  // namespace

class AvatarController : public drogon::HttpController<AvatarController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AvatarController::catalog, "/api/v1/avatar/catalog", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AvatarController::appearance, "/api/v1/avatar", drogon::Get, AuthFilter::classTypeName());
    METHOD_LIST_END

    void catalog(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        S::instance().catalog([req, callback](S::Result r) { respond(req, callback, r); });
    }

    void appearance(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        S::instance().appearance(userId, [req, callback](S::Result r) { respond(req, callback, r); });
    }
};

class AdminAvatarController : public drogon::HttpController<AdminAvatarController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminAvatarController::assets, "/api/admin/v1/avatar/assets", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::items, "/api/admin/v1/avatar/items", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::item, "/api/admin/v1/avatar/items/{itemId}", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::starter, "/api/admin/v1/avatar/starter", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::create, "/api/admin/v1/avatar/items", drogon::Post, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::revise, "/api/admin/v1/avatar/items/{itemId}/revisions", drogon::Post, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAvatarController::configureStarter, "/api/admin/v1/avatar/starter", drogon::Post, AdminFilter::classTypeName());
    METHOD_LIST_END

    void assets(const drogon::HttpRequestPtr &req, Callback &&callback) {
        if (!signedIn(req, callback)) return;
        S::instance().adminAssets([req, callback](S::Result r) { respond(req, callback, r); });
    }

    void items(const drogon::HttpRequestPtr &req, Callback &&callback) {
        if (!signedIn(req, callback)) return;
        S::instance().adminItems([req, callback](S::Result r) { respond(req, callback, r); });
    }

    void item(const drogon::HttpRequestPtr &req, Callback &&callback, std::string itemId) {
        if (!signedIn(req, callback)) return;
        S::instance().adminItem(itemId, [req, callback](S::Result r) { respond(req, callback, r); });
    }

    void starter(const drogon::HttpRequestPtr &req, Callback &&callback) {
        if (!signedIn(req, callback)) return;
        S::instance().adminStarter([req, callback](S::Result r) { respond(req, callback, r); });
    }

    void create(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string adminId, key, message;
        Json::Value body;
        if (!adminWrite(req, callback, adminId, key, body)) return;
        S::ItemInput in;
        if (!S::parseCreate(body, in, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        S::instance().createItem(adminId, key, in, [req, callback](S::Result r) { respond(req, callback, r); });
    }

    void revise(const drogon::HttpRequestPtr &req, Callback &&callback, std::string itemId) {
        std::string adminId, key, message;
        Json::Value body;
        if (!adminWrite(req, callback, adminId, key, body)) return;
        S::RevisionInput in;
        if (!S::parseRevision(body, in, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        S::instance().reviseItem(adminId, key, itemId, in, [req, callback](S::Result r) { respond(req, callback, r); });
    }

    void configureStarter(const drogon::HttpRequestPtr &req, Callback &&callback) {
        std::string adminId, key, message;
        Json::Value body;
        if (!adminWrite(req, callback, adminId, key, body)) return;
        S::StarterInput in;
        if (!S::parseStarter(body, in, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        S::instance().setStarter(adminId, key, in, [req, callback](S::Result r) { respond(req, callback, r); });
    }

  private:
    static bool signedIn(const drogon::HttpRequestPtr &req, const Callback &callback) {
        if (!adminctx::userId(req).empty()) return true;
        callback(ctrl::fail(ErrorCode::Unauthenticated, "管理员未登录", req));
        return false;
    }
};

}  // namespace usapp::controllers
