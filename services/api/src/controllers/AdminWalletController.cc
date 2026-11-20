// =====================================================================
// services/api/src/controllers/AdminWalletController.cc
// Admin-only, read-only wallet inspection of one selected user (docs/admin-wallet-ledger.md). AdminFilter enforces the peer
// CIDR, the admin surface header and the admin session; the controller also requires the admin identity it attached.
//   GET /api/admin/v1/users/{userId}/wallet
//   GET /api/admin/v1/users/{userId}/wallet/ledger?asset=flower|sunshine&limit=1..100&cursor=
// The caller-only /api/v1/wallet is unchanged and has no user selector.
// =====================================================================

#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminWalletService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using W = AdminWalletService;

bool signedIn(const drogon::HttpRequestPtr &req, const Callback &callback) {
    if (!adminctx::userId(req).empty()) return true;
    callback(ctrl::fail(ErrorCode::Unauthenticated, "管理员未登录", req));
    return false;
}

// One canonical spelling of the id, for the response and for cursors (looksLikeUuid accepts either case).
void lower(std::string &id) {
    for (auto &ch : id) if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
}

void respond(const drogon::HttpRequestPtr &req, const Callback &callback, const W::Result &r) {
    switch (r.outcome) {
        case W::Outcome::Ok: callback(jsonResponse(r.body, 200)); return;
        case W::Outcome::NotFound: callback(ctrl::fail(ErrorCode::NotFound, "用户不存在", req)); return;
        case W::Outcome::Error: break;
    }
    LOG_ERROR << "admin wallet read failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AdminWalletController : public drogon::HttpController<AdminWalletController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminWalletController::summary, "/api/admin/v1/users/{userId}/wallet", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminWalletController::ledger, "/api/admin/v1/users/{userId}/wallet/ledger", drogon::Get, AdminFilter::classTypeName());
    METHOD_LIST_END

    void summary(const drogon::HttpRequestPtr &req, Callback &&callback, std::string userId) {
        if (!signedIn(req, callback)) return;
        lower(userId);
        if (!ctrl::looksLikeUuid(userId)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "用户 id 格式无效", req));
            return;
        }
        W::summary(userId, [req, callback](W::Result r) { respond(req, callback, r); });
    }

    void ledger(const drogon::HttpRequestPtr &req, Callback &&callback, std::string userId) {
        if (!signedIn(req, callback)) return;
        lower(userId);
        if (!ctrl::looksLikeUuid(userId)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "用户 id 格式无效", req));
            return;
        }
        const std::string asset = req->getParameter("asset");
        if (!W::validAsset(asset)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "asset 必须是 flower 或 sunshine", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1-100 的整数", req));
            return;
        }
        std::int64_t cursorSeq = 0;
        const std::string cursor = req->getParameter("cursor");
        if (!cursor.empty() && !W::decodeCursor(cursor, userId, asset, cursorSeq)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效，或不属于该用户与资产", req));
            return;
        }
        W::ledger(userId, asset, *limit, cursorSeq, [req, callback](W::Result r) { respond(req, callback, r); });
    }
};

}  // namespace usapp::controllers
