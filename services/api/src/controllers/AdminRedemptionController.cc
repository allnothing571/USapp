#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminRedemptionService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using A = AdminRedemptionService;

bool signedIn(const drogon::HttpRequestPtr &req, const Callback &callback) {
    if (!adminctx::userId(req).empty()) return true;
    callback(ctrl::fail(ErrorCode::Unauthenticated, "管理员未登录", req));
    return false;
}

// One canonical spelling of the id, for the response and for cursors (looksLikeUuid accepts either case).
void lower(std::string &id) {
    for (auto &ch : id) if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
}

bool validId(const drogon::HttpRequestPtr &req, const Callback &callback, std::string &id, const char *message) {
    lower(id);
    if (ctrl::looksLikeUuid(id)) return true;
    callback(ctrl::fail(ErrorCode::ValidationError, message, req));
    return false;
}

void respond(const drogon::HttpRequestPtr &req, const Callback &callback, const A::Result &r) {
    switch (r.outcome) {
        case A::Outcome::Ok: callback(jsonResponse(r.body, 200)); return;
        case A::Outcome::SpaceNotFound: callback(ctrl::fail(ErrorCode::SpaceNotFound, "空间不存在", req)); return;
        case A::Outcome::RedemptionNotFound: callback(ctrl::fail(ErrorCode::RedemptionNotFound, "该空间中没有这个兑换", req)); return;
        case A::Outcome::Error: break;
    }
    LOG_ERROR << "admin redemption read failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AdminRedemptionController : public drogon::HttpController<AdminRedemptionController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminRedemptionController::list, "/api/admin/v1/couples/{coupleId}/redemptions", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminRedemptionController::detail, "/api/admin/v1/couples/{coupleId}/redemptions/{redemptionId}", drogon::Get,
                  AdminFilter::classTypeName());
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        const std::string status = req->getParameter("status");
        if (!A::validStatus(status)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "status 必须是 pending_confirmation、pending_fulfillment、fulfilled 或 cancelled", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1-100 的整数", req));
            return;
        }
        A::Cursor cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty() && !A::decodeCursor(raw, coupleId, status, cursor)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效，或不属于该空间与筛选条件", req));
            return;
        }
        A::list(coupleId, status, *limit, raw.empty() ? nullptr : &cursor,
                [req, callback](A::Result r) { respond(req, callback, r); });
    }

    void detail(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string redemptionId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        if (!validId(req, callback, redemptionId, "兑换 id 格式无效")) return;
        A::detail(coupleId, redemptionId, [req, callback](A::Result r) { respond(req, callback, r); });
    }
};

}  // namespace usapp::controllers
