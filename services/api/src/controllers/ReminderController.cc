// =====================================================================
// services/api/src/controllers/ReminderController.cc
//
// Phase 2 P2-6 R1: the caller's own reminder rules (contract 0.21.0; docs/phase2-reminders.md). Every
// route carries AuthFilter: writes are checked for exact Origin and the session-bound CSRF token by the
// filter; reads need a formal session. The owner comes only from the session. No route creates a reminder.
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "config/AppConfig.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AnniversaryService.h"
#include "services/ReminderService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class ReminderController : public drogon::HttpController<ReminderController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ReminderController::listRules, "/api/v1/reminders", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(ReminderController::setTaskRule, "/api/v1/tasks/{taskId}/reminder", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(ReminderController::setAnniversaryRule, "/api/v1/anniversaries/{anniversaryId}/reminder",
                  drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void listRules(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<ReminderService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            // The same strict timestamp_uuid form as the anniversary list.
            const auto decoded = AnniversaryService::decodeCursor(rawCursor);
            if (!decoded) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
            cursor = ReminderService::Cursor{decoded->createdAt, decoded->id};
        }
        ReminderService::instance().list(userId, *limit, cursor, [req, callback](ReminderService::ListResult result) {
            if (result.outcome != ReminderService::Outcome::Ok) {
                ReminderService::Status status;
                status.outcome = result.outcome;
                status.message = result.message;
                status.detail = result.detail;
                respond(req, callback, status);
                return;
            }
            const AppConfig &cfg = AppConfig::instance();
            const bool generation = ReminderService::generationEnabled(cfg.remindersEnabled, cfg.schedulerEnabled);
            callback(jsonResponse(ReminderService::listBody(result, generation), 200));
        });
    }

    void setTaskRule(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                     std::string taskId) {
        setRule(req, callback, ReminderService::TargetKind::Task, taskId);
    }

    void setAnniversaryRule(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string anniversaryId) {
        setRule(req, callback, ReminderService::TargetKind::Anniversary, anniversaryId);
    }

  private:
    static void setRule(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        ReminderService::TargetKind kind, const std::string &targetId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        const bool isTask = kind == ReminderService::TargetKind::Task;
        if (!ctrl::looksLikeUuid(targetId)) {
            callback(isTask ? ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req)
                            : ctrl::fail(ErrorCode::AnniversaryNotFound, "纪念日不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        ReminderService::Terms terms;
        std::string message;
        const bool parsed = isTask ? ReminderService::parseTaskRequest(body, terms, message)
                                   : ReminderService::parseAnniversaryRequest(body, terms, message);
        if (!parsed) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        ReminderService::instance().set(userId, kind, targetId, key, terms, [req, callback](ReminderService::Status status) {
            respond(req, callback, status);
        });
    }

    static void respond(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const ReminderService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(ReminderService::responseBody(status), ReminderService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == ReminderService::Outcome::ResultUnknown) {
            LOG_ERROR << "保存提醒：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复保存", req));
            return;
        }
        if (status.outcome == ReminderService::Outcome::Error) {
            LOG_ERROR << "提醒请求失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(ReminderService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
