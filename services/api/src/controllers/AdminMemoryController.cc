#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminMemoryService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using A = AdminMemoryService;

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
        case A::Outcome::NotFound: callback(ctrl::fail(ErrorCode::NotFound, "回忆不存在或当前不可用", req)); return;
        case A::Outcome::Error: break;
    }
    LOG_ERROR << "admin memory read failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AdminMemoryController : public drogon::HttpController<AdminMemoryController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminMemoryController::list, "/api/admin/v1/couples/{coupleId}/memories", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminMemoryController::detail, "/api/admin/v1/couples/{coupleId}/memories/{memoryId}", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminMemoryController::proposals, "/api/admin/v1/couples/{coupleId}/memories/{memoryId}/proposals", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminMemoryController::comparison, "/api/admin/v1/couples/{coupleId}/memories/{memoryId}/proposals/{proposalId}", drogon::Get, AdminFilter::classTypeName());
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1-100 的整数", req));
            return;
        }
        A::Cursor cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty() && !A::decodeCursor(raw, coupleId, cursor)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效，或不属于该空间", req));
            return;
        }
        A::list(coupleId, *limit, raw.empty() ? nullptr : &cursor,
                [req, callback](A::Result r) { respond(req, callback, r); });
    }

    void detail(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string memoryId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        if (!validId(req, callback, memoryId, "回忆 id 格式无效")) return;
        A::detail(coupleId, memoryId, [req, callback](A::Result r) { respond(req, callback, r); });
    }
    void proposals(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string memoryId) {
        if (!signedIn(req, callback) || !validId(req, callback, coupleId, "空间 id 格式无效") || !validId(req, callback, memoryId, "回忆 id 格式无效")) return;
        const auto limit=TaskService::parseLimit(req->getParameter("limit")); A::Cursor cursor; const auto raw=req->getParameter("cursor");
        if (!limit || (!raw.empty() && !A::decodeCursor(raw,coupleId,cursor,memoryId))) {callback(ctrl::fail(ErrorCode::ValidationError,"分页参数或游标无效",req));return;}
        A::proposals(coupleId,memoryId,*limit,raw.empty()?nullptr:&cursor,[req,callback](A::Result r){respond(req,callback,r);});
    }
    void comparison(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string memoryId, std::string proposalId) {
        if (!signedIn(req, callback) || !validId(req, callback, coupleId, "空间 id 格式无效") || !validId(req, callback, memoryId, "回忆 id 格式无效") || !validId(req,callback,proposalId,"请求 id 格式无效")) return;
        A::comparison(coupleId,memoryId,proposalId,[req,callback](A::Result r){respond(req,callback,r);});
    }
};
} // namespace usapp::controllers
