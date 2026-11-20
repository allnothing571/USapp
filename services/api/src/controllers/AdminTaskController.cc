// =====================================================================
// services/api/src/controllers/AdminTaskController.cc
// Admin-only, read-only inspection of the stored tasks of one selected couple/space (docs/admin-task-inspection.md).
// AdminFilter enforces the peer CIDR, the admin surface header and the admin session; the controller also requires the
// admin identity it attached.
//   GET /api/admin/v1/couples/{coupleId}/tasks?kind=task|template|period&limit=1..100&cursor=
//   GET /api/admin/v1/couples/{coupleId}/tasks/{taskId}
// The public /api/v1/tasks reads are unchanged and keep their caller scope and 30-day history window.
// =====================================================================

#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminTaskService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using A = AdminTaskService;

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
        case A::Outcome::TaskNotFound: callback(ctrl::fail(ErrorCode::TaskNotFound, "该空间中没有这个任务", req)); return;
        case A::Outcome::Error: break;
    }
    LOG_ERROR << "admin task read failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AdminTaskController : public drogon::HttpController<AdminTaskController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminTaskController::list, "/api/admin/v1/couples/{coupleId}/tasks", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminTaskController::detail, "/api/admin/v1/couples/{coupleId}/tasks/{taskId}", drogon::Get,
                  AdminFilter::classTypeName());
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        const std::string kind = req->getParameter("kind");
        if (!A::validKind(kind)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "kind 必须是 task、template 或 period", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1-100 的整数", req));
            return;
        }
        A::Cursor cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty() && !A::decodeCursor(raw, coupleId, kind, cursor)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效，或不属于该空间与筛选条件", req));
            return;
        }
        A::list(coupleId, kind, *limit, raw.empty() ? nullptr : &cursor,
                [req, callback](A::Result r) { respond(req, callback, r); });
    }

    void detail(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string taskId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        if (!validId(req, callback, taskId, "任务 id 格式无效")) return;
        A::detail(coupleId, taskId, [req, callback](A::Result r) { respond(req, callback, r); });
    }
};

}  // namespace usapp::controllers
