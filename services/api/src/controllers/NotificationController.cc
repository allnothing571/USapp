#include <drogon/HttpController.h>
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/NotificationIntent.h"
#include "services/NotificationService.h"
#include "services/TaskService.h"

namespace usapp::controllers {
class NotificationController : public drogon::HttpController<NotificationController> {
 public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(NotificationController::list, "/api/v1/notifications", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(NotificationController::count, "/api/v1/notifications/unread-count", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(NotificationController::read, "/api/v1/notifications/{id}/read", drogon::Post, AuthFilter::classTypeName());
    // P2-3b: the caller's remembered ordinary-notification defaults (read only).
    ADD_METHOD_TO(NotificationController::choices, "/api/v1/notification-choices", drogon::Get, AuthFilter::classTypeName());
    METHOD_LIST_END
    void list(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string user;
        if (!ctrl::requireUser(req, cb, user)) return;
        auto limit = TaskService::parseLimit(req->getParameter("limit"));
        std::string raw = req->getParameter("cursor"), unread = req->getParameter("unread_only");
        std::optional<NotificationService::Cursor> cursor;
        if (!raw.empty()) cursor = MessageService::decodeCursor(raw);
        if (!limit || (!raw.empty() && !cursor) || (!unread.empty() && unread != "true" && unread != "false")) {
            cb(ctrl::fail(ErrorCode::ValidationError, "通知分页参数无效", req)); return;
        }
        NotificationService::list(user, *limit, cursor, unread == "true",
            [req, cb](NotificationService::Result r) { respond(req, cb, r); });
    }
    void count(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string user;
        if (!ctrl::requireUser(req, cb, user)) return;
        NotificationService::unreadCount(user, [req, cb](NotificationService::Result r) { respond(req, cb, r); });
    }
    void read(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb, std::string id) {
        std::string user;
        if (!ctrl::requireUser(req, cb, user)) return;
        if (!ctrl::looksLikeUuid(id)) { cb(ctrl::fail(ErrorCode::NotFound, "通知不存在", req)); return; }
        Json::Value body;
        if (!ctrl::parseBody(req, cb, body)) return;
        if (!body.isObject() || !body.empty()) { cb(ctrl::fail(ErrorCode::ValidationError, "请求正文必须为空对象", req)); return; }
        NotificationService::markRead(user, id, [req, cb](NotificationService::Result r) { respond(req, cb, r); });
    }
    void choices(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string user;
        if (!ctrl::requireUser(req, cb, user)) return;
        intent::readChoices(user, [req, cb](intent::ChoicesResult r) {
            if (r.status == 200) { cb(jsonResponse(r.body, 200)); return; }
            if (r.status == 404) { cb(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req)); return; }
            cb(ctrl::fail(ErrorCode::InternalError, "通知偏好读取失败", req));
        });
    }
 private:
    static void respond(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &cb,
                        const NotificationService::Result &r) {
        if (r.status == 200) { cb(jsonResponse(r.body, 200)); return; }
        if (r.status == 404) { cb(ctrl::fail(ErrorCode::NotFound, "通知不存在", req)); return; }
        if (r.status == 503) { cb(ctrl::fail(ErrorCode::ResultUnknown, "已读结果未确认，请重试同一条通知", req)); return; }
        cb(ctrl::fail(r.status == 400 ? ErrorCode::ValidationError : ErrorCode::InternalError, "通知请求无法完成", req));
    }
};
}
