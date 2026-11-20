// =====================================================================
// services/api/src/controllers/ExportController.cc
// E1 structured user-data export (docs/user-data-export-design.md Section 8.1). One user route, AuthFilter:
//   POST /api/v1/me/export   exact Origin and CSRF as every user write; the body is absent or exactly {}; no Idempotency-Key.
// The subject is always the session user. Read only.
// =====================================================================

#include <drogon/HttpController.h>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/ExportService.h"

namespace usapp::controllers {

class ExportController : public drogon::HttpController<ExportController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ExportController::create, "/api/v1/me/export", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void create(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string user;
        if (!ctrl::requireUser(req, callback, user)) return;
        // No identity, scope or any other selector: an absent body or exactly {}.
        if (!req->body().empty()) {
            Json::Value body;
            if (!http::isJsonObject(req, body) || !body.empty()) {
                callback(ctrl::fail(ErrorCode::ValidationError, "导出请求不接受任何参数", req));
                return;
            }
        }
        ExportService::exportFor(user, [req, callback](ExportService::Result r) {
            using O = ExportService::Outcome;
            switch (r.outcome) {
                case O::Ok: callback(attachmentJsonResponse(std::move(r.body), r.filename)); return;
                case O::SpaceNotFound: callback(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req)); return;
                case O::TooLarge: callback(ctrl::fail(ErrorCode::ExportTooLarge, "数据量超过单次导出上限，未生成文件", req)); return;
                case O::Busy: {
                    auto resp = ctrl::fail(ErrorCode::RateLimited, "导出过于频繁，请稍后再试", req);
                    resp->addHeader("Retry-After", std::to_string(r.retryAfterSeconds > 0 ? r.retryAfterSeconds : 1));
                    callback(resp);
                    return;
                }
                case O::Unavailable: break;
            }
            callback(ctrl::fail(ErrorCode::ExportUnavailable, "暂时无法生成导出文件，未生成任何文件", req));
        });
    }
};

}  // namespace usapp::controllers
