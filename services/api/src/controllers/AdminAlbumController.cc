#include <drogon/HttpController.h>

#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminAlbumService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using A = AdminAlbumService;

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
        case A::Outcome::AlbumNotFound: callback(ctrl::fail(ErrorCode::NotFound, "相册不存在或当前不可用", req)); return;
        case A::Outcome::Error: break;
    }
    LOG_ERROR << "admin album read failed request_id=" << http::requestId(req);
    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
}

}  // namespace

class AdminAlbumController : public drogon::HttpController<AdminAlbumController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminAlbumController::list, "/api/admin/v1/couples/{coupleId}/albums", drogon::Get, AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAlbumController::detail, "/api/admin/v1/couples/{coupleId}/albums/{albumId}", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminAlbumController::items, "/api/admin/v1/couples/{coupleId}/albums/{albumId}/items", drogon::Get, AdminFilter::classTypeName());
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

    void detail(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string albumId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效")) return;
        if (!validId(req, callback, albumId, "相册 id 格式无效")) return;
        A::detail(coupleId, albumId, [req, callback](A::Result r) { respond(req, callback, r); });
    }
    void items(const drogon::HttpRequestPtr &req, Callback &&callback, std::string coupleId, std::string albumId) {
        if (!signedIn(req, callback)) return;
        if (!validId(req, callback, coupleId, "空间 id 格式无效") || !validId(req, callback, albumId, "相册 id 格式无效")) return;
        const auto limit=TaskService::parseLimit(req->getParameter("limit"));
        const auto view=req->getParameter("view");
        if (!limit || (!view.empty() && view!="current" && view!="history")) { callback(ctrl::fail(ErrorCode::ValidationError,"分页参数无效",req)); return; }
        const bool history=view=="history"; int before=0; const auto raw=req->getParameter("cursor");
        if (!raw.empty() && !A::decodeItemCursor(raw,coupleId,albumId,history,before)) { callback(ctrl::fail(ErrorCode::ValidationError,"游标不属于当前空间、相册或视图",req));return; }
        A::items(coupleId,albumId,history,*limit,before,[req,callback](A::Result r){respond(req,callback,r);});
    }

};

}  // namespace usapp::controllers
