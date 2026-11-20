// =====================================================================
// services/api/src/controllers/MemoryController.cc
//
// Phase 3 P3-1b: photo memories (docs/phase3-plan.md). Every route carries AuthFilter: writes are checked for exact Origin and
// the session-bound CSRF token by the filter; reads need a formal session. The user comes only from the session; the couple,
// creator and proposer are derived server-side.
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AlbumService.h"
#include "services/MediaService.h"
#include "services/MemoryService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class MemoryController : public drogon::HttpController<MemoryController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MemoryController::createMemory, "/api/v1/memories", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::listMemories, "/api/v1/memories", drogon::Get, AuthFilter::classTypeName());
    // Before the {memoryId} route, which the same path would otherwise match.
    ADD_METHOD_TO(MemoryController::listDeletions, "/api/v1/memories/deletions", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::getMemory, "/api/v1/memories/{memoryId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::deleteMemory, "/api/v1/memories/{memoryId}/deletions", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::undoDeletion, "/api/v1/memories/{memoryId}/deletions/{deletionId}/undo", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::requestDeletion, "/api/v1/memories/{memoryId}/delete-requests", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::getDeleteRequest, "/api/v1/memories/{memoryId}/delete-requests/{requestId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::decideDeleteRequest, "/api/v1/memories/{memoryId}/delete-requests/{requestId}/decision", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::editMemory, "/api/v1/memories/{memoryId}/revisions", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::proposeEdit, "/api/v1/memories/{memoryId}/proposals", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::listProposals, "/api/v1/memories/{memoryId}/proposals", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::getProposal, "/api/v1/memories/{memoryId}/proposals/{proposalId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MemoryController::decideProposal, "/api/v1/memories/{memoryId}/proposals/{proposalId}/decision", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void createMemory(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        // D20: the optional notify_partner is taken out first, so the strict parser sees the legacy body.
        intent::Choice choice;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MemoryService::Terms terms;
        if (!MemoryService::parseCreateRequest(body, terms, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MemoryService::instance().create(userId, key, terms, [req, callback](MemoryService::Status status) { respond(req, callback, status); }, choice);
    }

    void listMemories(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<MemoryService::Cursor> cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            cursor = MemoryService::decodeCursor(raw);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        MemoryService::instance().list(userId, *limit, cursor, [req, callback](MemoryService::ListResult result) {
            if (result.outcome != MemoryService::Outcome::Ok) {
                MemoryService::Status status;
                status.outcome = result.outcome;
                status.message = result.message;
                status.detail = result.detail;
                respond(req, callback, status);
                return;
            }
            Json::Value body(Json::objectValue);
            Json::Value items(Json::arrayValue);
            for (const auto &item : result.memories) items.append(item);
            body["memories"] = items;
            body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
            body["uploads_enabled"] = MediaService::instance().uploadsEnabled();
            callback(jsonResponse(body, 200));
        });
    }

    void getMemory(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        MemoryService::instance().get(userId, memoryId, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void editMemory(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        revision(req, callback, memoryId, true);
    }

    void proposeEdit(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        revision(req, callback, memoryId, false);
    }

    void getProposal(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId,
                     std::string proposalId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(memoryId) || !ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::MemoryProposalNotFound, "修改申请不存在", req));
            return;
        }
        MemoryService::instance().getProposal(userId, memoryId, proposalId, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    // Summaries of one memory's edit proposals, newest first. A read: no Origin or CSRF, nothing written.
    void listProposals(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<std::pair<std::string, std::string>> after;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            const auto cursor = AlbumService::decodeCursor(raw);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
            after = std::make_pair(cursor->createdAt, cursor->id);
        }
        MemoryService::instance().listProposals(userId, memoryId, *limit, after, [req, callback](MemoryService::ListResult result) {
            if (result.outcome != MemoryService::Outcome::Ok) {
                MemoryService::Status status;
                status.outcome = result.outcome;
                status.message = result.message;
                status.detail = result.detail;
                respond(req, callback, status);
                return;
            }
            Json::Value body(Json::objectValue);
            Json::Value items(Json::arrayValue);
            for (const auto &item : result.memories) items.append(item);
            body["proposals"] = items;
            body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
            body["current_version"] = Json::Value(static_cast<Json::Int64>(result.currentVersion));
            callback(jsonResponse(body, 200));
        });
    }

    void decideProposal(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId,
                        std::string proposalId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        if (!ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::MemoryProposalNotFound, "修改申请不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        MemoryService::Decision decision = MemoryService::Decision::Reject;
        std::string message;
        if (!MemoryService::parseDecisionRequest(body, decision, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MemoryService::instance().decide(userId, memoryId, proposalId, key, decision,
                                         [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void listDeletions(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<std::pair<std::string, std::string>> after;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            const auto cursor = AlbumService::decodeCursor(raw);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
            after = std::make_pair(cursor->createdAt, cursor->id);
        }
        MemoryService::instance().listDeletions(userId, *limit, after, [req, callback](MemoryService::ListResult result) {
            if (result.outcome != MemoryService::Outcome::Ok) {
                MemoryService::Status status;
                status.outcome = result.outcome;
                status.message = result.message;
                status.detail = result.detail;
                respond(req, callback, status);
                return;
            }
            Json::Value body(Json::objectValue);
            Json::Value items(Json::arrayValue);
            for (const auto &item : result.memories) items.append(item);
            body["deletions"] = items;
            body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
            callback(jsonResponse(body, 200));
        });
    }

    void deleteMemory(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        MemoryService::instance().deleteMemory(userId, memoryId, key, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void undoDeletion(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId,
                      std::string deletionId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId) || !ctrl::looksLikeUuid(deletionId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        MemoryService::instance().undoDeletion(userId, memoryId, deletionId, key, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void requestDeletion(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        MemoryService::instance().requestDeletion(userId, memoryId, key, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void getDeleteRequest(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId,
                          std::string requestId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(memoryId) || !ctrl::looksLikeUuid(requestId)) {
            callback(ctrl::fail(ErrorCode::MemoryProposalNotFound, "删除申请不存在", req));
            return;
        }
        MemoryService::instance().getDeleteRequest(userId, memoryId, requestId, [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

    void decideDeleteRequest(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string memoryId,
                             std::string requestId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        if (!ctrl::looksLikeUuid(requestId)) {
            callback(ctrl::fail(ErrorCode::MemoryProposalNotFound, "删除申请不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        MemoryService::Decision decision = MemoryService::Decision::Reject;
        std::string message;
        if (!MemoryService::parseDecisionRequest(body, decision, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MemoryService::instance().decideDeleteRequest(userId, memoryId, requestId, key, decision,
                                                      [req, callback](MemoryService::Status status) { respond(req, callback, status); });
    }

  private:
    static bool prepareMutation(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                std::string &userId, std::string &key) {
        if (!ctrl::requireUser(req, callback, userId)) return false;
        key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return false;
        }
        return true;
    }

    static void revision(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                         const std::string &memoryId, bool direct) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(memoryId)) {
            callback(ctrl::fail(ErrorCode::MemoryNotFound, "回忆不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        MemoryService::Terms terms;
        int expected = 0;
        std::string message;
        if (!MemoryService::parseRevisionRequest(body, expected, terms, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        auto done = [req, callback](MemoryService::Status status) { respond(req, callback, status); };
        if (direct) MemoryService::instance().edit(userId, memoryId, key, expected, terms, done);
        else MemoryService::instance().propose(userId, memoryId, key, expected, terms, done);
    }

    static void respond(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const MemoryService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(MemoryService::responseBody(status), MemoryService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == MemoryService::Outcome::ResultUnknown) {
            LOG_ERROR << "回忆请求：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复保存", req));
            return;
        }
        if (status.outcome == MemoryService::Outcome::Error) {
            LOG_ERROR << "回忆请求失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(MemoryService::errorCodeFor(status.outcome), status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
