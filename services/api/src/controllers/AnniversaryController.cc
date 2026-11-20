// =====================================================================
// services/api/src/controllers/AnniversaryController.cc
//
// Phase 2 P2-2: anniversaries and countdowns (contract 0.15.0;
// docs/phase2-anniversaries.md). Every route carries AuthFilter: writes are checked for
// exact Origin and the session-bound CSRF token by the filter; reads need a formal
// session. The user comes only from the session; space, creator, proposer and pin owner
// are derived server-side.
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AnniversaryService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class AnniversaryController : public drogon::HttpController<AnniversaryController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AnniversaryController::createAnniversary, "/api/v1/anniversaries", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::listAnniversaries, "/api/v1/anniversaries", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::getHome, "/api/v1/anniversaries/home", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::setHomePin, "/api/v1/anniversaries/home-pin", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::getAnniversary, "/api/v1/anniversaries/{anniversaryId}", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::editAnniversary, "/api/v1/anniversaries/{anniversaryId}/revisions",
                  drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::proposeEdit, "/api/v1/anniversaries/{anniversaryId}/proposals",
                  drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AnniversaryController::decideProposal,
                  "/api/v1/anniversaries/{anniversaryId}/proposals/{proposalId}/decision", drogon::Post,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    void createAnniversary(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        // P2-3b: notify_partner is taken out before the strict legacy parser.
        intent::Choice choice;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AnniversaryService::Terms terms;
        if (!AnniversaryService::parseCreateRequest(body, terms, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AnniversaryService::instance().create(userId, key, terms, [req, callback](AnniversaryService::Status status) {
            respond(req, callback, status, "创建纪念日");
        }, choice);
    }

    void listAnniversaries(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<AnniversaryService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            cursor = AnniversaryService::decodeCursor(rawCursor);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        AnniversaryService::instance().list(
            userId, *limit, cursor, [req, callback](AnniversaryService::ListResult result) {
                if (result.outcome != AnniversaryService::Outcome::Ok) {
                    respond(req, callback, statusOf(result.outcome, result.message, result.detail), "读取纪念日");
                    return;
                }
                Json::Value body(Json::objectValue);
                Json::Value items(Json::arrayValue);
                for (const auto &a : result.anniversaries) {
                    items.append(AnniversaryService::anniversaryJson(a, result.asOfDate));
                }
                body["as_of_date"] = result.asOfDate;
                body["anniversaries"] = items;
                body["next_cursor"] =
                    result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
                callback(jsonResponse(body, 200));
            });
    }

    void getHome(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        sendHome(req, callback, userId);
    }

    void setHomePin(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        std::optional<std::string> anniversaryId;
        std::string message;
        if (!AnniversaryService::parsePinRequest(body, anniversaryId, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AnniversaryService::instance().pin(userId, key, anniversaryId,
                                           [req, callback](AnniversaryService::Status status) {
                                               respond(req, callback, status, "设置首页纪念日");
                                           });
    }

    void getAnniversary(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string anniversaryId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        // Defensive: the literal /home route must win whichever order the router tries.
        if (anniversaryId == "home") {
            sendHome(req, callback, userId);
            return;
        }
        if (!ctrl::looksLikeUuid(anniversaryId)) {
            callback(ctrl::fail(ErrorCode::AnniversaryNotFound, "纪念日不存在", req));
            return;
        }
        AnniversaryService::instance().get(userId, anniversaryId, [req, callback](AnniversaryService::Status status) {
            if (!status.success()) {
                respond(req, callback, status, "读取纪念日详情");
                return;
            }
            Json::Value body(Json::objectValue);
            body["as_of_date"] = status.asOfDate;
            body["anniversary"] = AnniversaryService::anniversaryJson(status.data, status.asOfDate);
            callback(jsonResponse(body, 200));
        });
    }

    void editAnniversary(const drogon::HttpRequestPtr &req,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string anniversaryId) {
        revisionLike(req, callback, anniversaryId, false);
    }

    void proposeEdit(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string anniversaryId) {
        revisionLike(req, callback, anniversaryId, true);
    }

    void decideProposal(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string anniversaryId,
                        std::string proposalId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(anniversaryId)) {
            callback(ctrl::fail(ErrorCode::AnniversaryNotFound, "纪念日不存在", req));
            return;
        }
        if (!ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::AnniversaryProposalNotFound, "修改申请不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        AnniversaryService::Decision decision = AnniversaryService::Decision::Approve;
        std::string message;
        if (!AnniversaryService::parseDecisionRequest(body, decision, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AnniversaryService::instance().decide(userId, anniversaryId, proposalId, key, decision,
                                              [req, callback](AnniversaryService::Status status) {
                                                  respond(req, callback, status, "处理纪念日修改申请");
                                              });
    }

  private:
    static void revisionLike(const drogon::HttpRequestPtr &req,
                             const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                             const std::string &anniversaryId, bool proposal) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(anniversaryId)) {
            callback(ctrl::fail(ErrorCode::AnniversaryNotFound, "纪念日不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        // P2-3b: only the creator's direct edit accepts notify_partner; a proposal is a mandatory
        // notice, so the field stays in its body and the strict parser rejects it.
        intent::Choice choice;
        std::string message;
        if (!proposal && !intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        int expectedVersion = 0;
        AnniversaryService::Terms terms;
        if (!AnniversaryService::parseRevisionRequest(body, expectedVersion, terms, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        auto done = [req, callback, proposal](AnniversaryService::Status status) {
            respond(req, callback, status, proposal ? "提交纪念日修改申请" : "修改纪念日");
        };
        if (proposal) {
            AnniversaryService::instance().propose(userId, anniversaryId, key, expectedVersion, terms, done);
        } else {
            AnniversaryService::instance().edit(userId, anniversaryId, key, expectedVersion, terms, done, choice);
        }
    }

    static void sendHome(const drogon::HttpRequestPtr &req,
                         const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                         const std::string &userId) {
        AnniversaryService::instance().home(userId, [req, callback](AnniversaryService::HomeResult result) {
            if (result.outcome != AnniversaryService::Outcome::Ok) {
                respond(req, callback, statusOf(result.outcome, result.message, result.detail), "读取首页纪念日");
                return;
            }
            Json::Value body(Json::objectValue);
            body["as_of_date"] = result.asOfDate;
            body["source"] = result.source;
            body["anniversary"] = result.anniversary
                                      ? AnniversaryService::anniversaryJson(*result.anniversary, result.asOfDate)
                                      : Json::Value(Json::nullValue);
            callback(jsonResponse(body, 200));
        });
    }

    static AnniversaryService::Status statusOf(AnniversaryService::Outcome outcome, const std::string &message,
                                               const std::string &detail) {
        AnniversaryService::Status status;
        status.outcome = outcome;
        status.message = message;
        status.detail = detail;
        return status;
    }

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
                        const AnniversaryService::Status &status, const char *action) {
        if (status.success()) {
            callback(jsonResponse(AnniversaryService::responseBody(status),
                                  AnniversaryService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == AnniversaryService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复保存", req));
            return;
        }
        if (status.outcome == AnniversaryService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(AnniversaryService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
