// =====================================================================
// services/api/src/controllers/TaskController.cc
//
// 阶段 1 M3-B：一次性任务的发布、列表与详情（契约 0.4.0）。
// 三个路由都挂 AuthFilter：写请求由过滤器校验 Origin + 会话绑定的 CSRF，
// 读请求只要求正式会话。用户只取自会话；空间与接收者由服务端推导。
// 本控制器不提供完成接口（M3-C）。
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class TaskController : public drogon::HttpController<TaskController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TaskController::createTask, "/api/v1/tasks", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::listTasks, "/api/v1/tasks", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::getTask, "/api/v1/tasks/{taskId}", drogon::Get,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    void createTask(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        TaskService::CreateInput input;
        std::string message;
        if (!TaskService::parseCreateRequest(body, input, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        TaskService::instance().createTask(
            userId, input, key, [req, callback](TaskService::Status status) {
                respondWithTask(req, callback, status, "发布任务");
            });
    }

    void listTasks(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto view = TaskService::parseView(req->getParameter("view"));
        if (!view) {
            callback(ctrl::fail(ErrorCode::ValidationError, "view 必须是 open 或 history", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<TaskService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            // 非法游标直接 400，不执行任何查询。
            cursor = TaskService::decodeCursor(rawCursor, *view);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        TaskService::instance().listTasks(
            userId, *view, cursor, *limit, [req, callback](TaskService::ListResult result) {
                if (result.outcome != TaskService::Outcome::Ok) {
                    respondWithFailure(req, callback, result.outcome, result.message, result.detail, "读取任务列表");
                    return;
                }
                Json::Value body(Json::objectValue);
                Json::Value tasks(Json::arrayValue);
                for (const auto &task : result.tasks) {
                    tasks.append(TaskService::taskJson(task));
                }
                body["tasks"] = tasks;
                body["next_cursor"] =
                    result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
                callback(jsonResponse(body, 200));
            });
    }

    void getTask(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                 std::string taskId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(taskId)) {
            // 形状不对与“不存在 / 属于其他空间”一样回 404，避免把非法串交给 $2::uuid 变成 500。
            callback(ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req));
            return;
        }
        TaskService::instance().getTask(userId, taskId, [req, callback](TaskService::Status status) {
            respondWithTask(req, callback, status, "读取任务详情");
        });
    }

  private:
    static void respondWithFailure(const drogon::HttpRequestPtr &req,
                                   const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                   TaskService::Outcome outcome,
                                   const std::string &message,
                                   const std::string &detail,
                                   const char *action) {
        if (outcome == TaskService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复发布", req));
            return;
        }
        if (outcome == TaskService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        if (!detail.empty()) {
            LOG_WARN << action << "被拒绝 outcome=" << static_cast<int>(outcome) << " detail=" << detail
                     << " request_id=" << http::requestId(req);
        }
        callback(ctrl::fail(TaskService::errorCodeFor(outcome),
                            message.empty() ? std::string("请求无法完成") : message, req));
    }

    static void respondWithTask(const drogon::HttpRequestPtr &req,
                                const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                const TaskService::Status &status,
                                const char *action) {
        if (!status.success()) {
            respondWithFailure(req, callback, status.outcome, status.message, status.detail, action);
            return;
        }
        Json::Value body(Json::objectValue);
        body["task"] = TaskService::taskJson(status.task);
        callback(jsonResponse(body, TaskService::httpStatusFor(status.outcome)));
    }
};

}  // namespace usapp::controllers
