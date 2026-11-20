// =====================================================================
// services/api/src/controllers/MessageController.cc
//
// Phase 2 P2-1: partner message board (contract 0.14.0; docs/phase2-messages.md).
// Every route carries AuthFilter: writes are checked for exact Origin and the
// session-bound CSRF token by the filter; reads need a formal session. The user comes
// only from the session; space, partner and visibility are derived server-side.
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/MessageService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class MessageController : public drogon::HttpController<MessageController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MessageController::createMessage, "/api/v1/messages", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MessageController::listMessages, "/api/v1/messages", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MessageController::getMessage, "/api/v1/messages/{messageId}", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(MessageController::replyMessage, "/api/v1/messages/{messageId}/reply", drogon::Post,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    void createMessage(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        // P2-3b: notify_partner is taken out before the strict legacy parser.
        intent::Choice choice;
        std::string text;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        if (!MessageService::parseMessageRequest(body, text, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MessageService::instance().create(userId, key, text, [req, callback](MessageService::Status status) {
            respond(req, callback, status, "发布留言");
        }, choice);
    }

    void listMessages(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        const std::string from = req->getParameter("from");
        if (!from.empty() && from != "all" && from != "partner") {
            callback(ctrl::fail(ErrorCode::ValidationError, "from 只能是 all 或 partner", req));
            return;
        }
        std::optional<MessageService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            cursor = MessageService::decodeCursor(rawCursor);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        MessageService::instance().list(
            userId, from == "partner", *limit, cursor, [req, callback](MessageService::ListResult result) {
                if (result.outcome != MessageService::Outcome::Ok) {
                    MessageService::Status status;
                    status.outcome = result.outcome;
                    status.message = result.message;
                    status.detail = result.detail;
                    respond(req, callback, status, "读取留言");
                    return;
                }
                Json::Value body(Json::objectValue);
                Json::Value items(Json::arrayValue);
                for (const auto &m : result.messages) items.append(MessageService::messageJson(m));
                body["messages"] = items;
                body["next_cursor"] =
                    result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
                callback(jsonResponse(body, 200));
            });
    }

    void getMessage(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string messageId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(messageId)) {
            callback(ctrl::fail(ErrorCode::MessageNotFound, "留言不存在", req));
            return;
        }
        MessageService::instance().get(userId, messageId, [req, callback](MessageService::Status status) {
            if (!status.success()) {
                respond(req, callback, status, "读取留言详情");
                return;
            }
            Json::Value body(Json::objectValue);
            body["message"] = MessageService::messageJson(status.data);
            callback(jsonResponse(body, 200));
        });
    }

    void replyMessage(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string messageId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(messageId)) {
            callback(ctrl::fail(ErrorCode::MessageNotFound, "留言不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        intent::Choice choice;
        std::string text;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        if (!MessageService::parseReplyRequest(body, text, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MessageService::instance().reply(userId, messageId, key, text,
                                         [req, callback](MessageService::Status status) {
                                             respond(req, callback, status, "回复留言");
                                         }, choice);
    }

  private:
    static bool prepareMutation(const drogon::HttpRequestPtr &req,
                                const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                std::string &userId, std::string &key) {
        if (!ctrl::requireUser(req, callback, userId)) return false;
        key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return false;
        }
        return true;
    }

    static void respond(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const MessageService::Status &status, const char *action) {
        if (status.success()) {
            callback(jsonResponse(MessageService::responseBody(status), MessageService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == MessageService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复发布", req));
            return;
        }
        if (status.outcome == MessageService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(MessageService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
