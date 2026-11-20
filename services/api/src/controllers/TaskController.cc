// =====================================================================
// services/api/src/controllers/TaskController.cc
//
// 阶段 1 M3-B：一次性任务的发布、列表与详情（契约 0.4.0）；
// 阶段 1 M3-C1：任务完成与奖励结算（契约 0.5.0）。
// 阶段 1 M4：累计计数 +1 与最终结算（契约 0.6.0）。
// 阶段 1 M6：任务修改/取消的提议与确认（契约 0.10.0）。
// 所有路由都挂 AuthFilter：写请求由过滤器校验 Origin + 会话绑定的 CSRF，
// 读请求只要求正式会话。用户只取自会话；空间与接收者由服务端推导。
// 完成接口只把会话用户、路径中的任务 id 与幂等键交给服务，不传测试用的故障注入参数。
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/TaskChangeService.h"
#include "services/TaskCompletionService.h"
#include "services/TaskService.h"
#include "services/TaskReactionService.h"

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
    ADD_METHOD_TO(TaskController::completeTask, "/api/v1/tasks/{taskId}/completion", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::progressTask, "/api/v1/tasks/{taskId}/progress", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::proposeChange, "/api/v1/tasks/{taskId}/changes", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::decideChange, "/api/v1/tasks/{taskId}/changes/{proposalId}/decision",
                  drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(TaskController::reactTask, "/api/v1/tasks/{taskId}/reaction", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void reactTask(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb, std::string taskId) {
        std::string user;
        if (!ctrl::requireUser(req, cb, user)) return;
        if (!TaskService::isCanonicalUuid(taskId)) { cb(ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req)); return; }
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) { cb(ctrl::fail(ErrorCode::ValidationError, "请求标识无效", req)); return; }
        Json::Value body;
        if (!ctrl::parseBody(req, cb, body)) return;
        TaskReactionService::Input input; std::string message;
        if (!TaskReactionService::parse(body, input, message)) { cb(ctrl::fail(ErrorCode::ValidationError,message,req)); return; }
        TaskReactionService::react(user,taskId,key,input,[req,cb](TaskReactionService::Status s){
            using O=TaskReactionService::Outcome;
            if (s.outcome==O::Created || s.outcome==O::Replayed) {
                Json::Value response;response["mutation"]=s.mutation;response["task"]=TaskService::taskJson(s.task);
                cb(jsonResponse(response,s.outcome==O::Created?201:200));return;
            }
            cb(ctrl::fail(TaskReactionService::errorCode(s.outcome),
                          s.outcome==O::ResultUnknown?"结果未确认，请使用原请求重试":"回应未能保存，请检查任务状态或稍后重试",req));
        });
    }

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
        // P2-3b: the optional notify_partner is taken out first, so the strict legacy parser
        // and fingerprint see exactly the legacy body.
        intent::Choice choice;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        TaskService::CreateInput input;
        if (!TaskService::parseCreateRequest(body, input, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        TaskService::instance().createTask(
            userId, input, key, [req, callback](TaskService::Status status) {
                respondWithTask(req, callback, status, "发布任务", true);
            }, choice);
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
        }, req->getParameter("period_date"));
    }

    void completeTask(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::string taskId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        if (!ctrl::looksLikeUuid(taskId)) {
            callback(ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req));
            return;
        }
        const auto body = req->body();
        if (!TaskCompletionService::isEmptyCompletionBody(std::string(body.data(), body.size()))) {
            callback(ctrl::fail(ErrorCode::ValidationError, "完成任务的请求体必须为空", req));
            return;
        }
        TaskCompletionService::instance().completeTask(
            userId, taskId, key, [req, callback](TaskCompletionService::Status status) {
                if (status.success()) {
                    callback(jsonResponse(TaskCompletionService::responseBody(status),
                                          TaskCompletionService::httpStatusFor(status.outcome)));
                    return;
                }
                if (status.outcome == TaskCompletionService::Outcome::ResultUnknown) {
                    LOG_ERROR << "完成任务：提交结果未确认 detail=" << status.detail
                              << " request_id=" << http::requestId(req);
                    callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复入账", req));
                    return;
                }
                if (status.outcome == TaskCompletionService::Outcome::Error) {
                    LOG_ERROR << "完成任务失败 detail=" << status.detail << " request_id=" << http::requestId(req);
                    callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
                    return;
                }
                callback(ctrl::fail(TaskCompletionService::errorCodeFor(status.outcome),
                                    status.message.empty() ? std::string("请求无法完成") : status.message, req));
            });
    }

    void progressTask(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::string taskId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key", req));
            return;
        }
        if (!ctrl::looksLikeUuid(taskId)) {
            callback(ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req));
            return;
        }
        const auto body = req->body();
        if (!TaskCompletionService::isEmptyCompletionBody(std::string(body.data(), body.size()))) {
            const auto json = req->getJsonObject();
            if (!json || !json->isObject() || json->size() != 1 || !(*json)["period_date"].isString() ||
                !TaskService::validPeriodDate((*json)["period_date"].asString())) {
                callback(ctrl::fail(ErrorCode::ValidationError, "+1 请求体只能包含有效的周期日期", req));
                return;
            }
            TaskCompletionService::instance().progressDailyTask(
                userId, taskId, key, (*json)["period_date"].asString(), [req, callback](TaskCompletionService::Status status) {
                    respondWithProgress(req, callback, status);
                });
            return;
        }
        TaskCompletionService::instance().progressTask(
            userId, taskId, key, [req, callback](TaskCompletionService::Status status) {
                respondWithProgress(req, callback, status);
            });
    }

    // M6: propose an edit (creator) or a cancellation (either member).
    void proposeChange(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                       std::string taskId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key", req));
            return;
        }
        if (!ctrl::looksLikeUuid(taskId)) {
            callback(ctrl::fail(ErrorCode::TaskNotFound, "任务不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        TaskChangeService::ProposeInput input;
        std::string message;
        if (!TaskChangeService::parseProposeRequest(body, input, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        TaskChangeService::instance().propose(
            userId, taskId, key, input, [req, callback](TaskChangeService::Status status) {
                respondWithChange(req, callback, status);
            });
    }

    // M6: approve / reject (the other member) or withdraw (the proposer).
    void decideChange(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::string taskId, std::string proposalId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key", req));
            return;
        }
        if (!ctrl::looksLikeUuid(taskId) || !ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::TaskChangeNotFound, "变更不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        TaskChangeService::Action action = TaskChangeService::Action::Reject;
        std::string message;
        if (!TaskChangeService::parseDecisionRequest(body, action, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        TaskChangeService::instance().decide(
            userId, taskId, proposalId, key, action, [req, callback](TaskChangeService::Status status) {
                respondWithChange(req, callback, status);
            });
    }

  private:
    static void respondWithChange(const drogon::HttpRequestPtr &req,
                                  const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                  const TaskChangeService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(TaskChangeService::responseBody(status),
                                  TaskChangeService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == TaskChangeService::Outcome::ResultUnknown) {
            LOG_ERROR << "任务变更：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复处理", req));
            return;
        }
        if (status.outcome == TaskChangeService::Outcome::Error) {
            LOG_ERROR << "任务变更失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(TaskChangeService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }

    static void respondWithProgress(const drogon::HttpRequestPtr &req,
                                    const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                    const TaskCompletionService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(TaskCompletionService::progressResponseBody(status)));
            return;
        }
        const auto code = TaskCompletionService::errorCodeFor(status.outcome);
        if (status.outcome == TaskCompletionService::Outcome::Error ||
            status.outcome == TaskCompletionService::Outcome::ResultUnknown) {
            LOG_ERROR << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(code, status.outcome == TaskCompletionService::Outcome::ResultUnknown
                ? "记录结果未确认，请使用相同请求标识重试" : "记录进度失败，请稍后重试", req));
        } else callback(ctrl::fail(code, status.message, req));
    }

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
                                const char *action,
                                bool withNotification = false) {
        if (!status.success()) {
            respondWithFailure(req, callback, status.outcome, status.message, status.detail, action);
            return;
        }
        Json::Value body(Json::objectValue);
        body["task"] = TaskService::taskJson(status.task);
        // Publication only: original notification facts (null for a legacy publication).
        if (withNotification) body["notification"] = status.notification;
        callback(jsonResponse(body, TaskService::httpStatusFor(status.outcome)));
    }
};

}  // namespace usapp::controllers
