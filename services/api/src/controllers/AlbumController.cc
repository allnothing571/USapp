// =====================================================================
// services/api/src/controllers/AlbumController.cc
//
// Phase 3 P3-2a: shared albums (docs/phase3-p32-albums-design.md). Every route carries AuthFilter: writes are checked for exact
// Origin and the session-bound CSRF token by the filter; reads need a formal session. The user comes only from the session; the
// couple is derived server-side. P3-2b adds rename and rename requests (docs/phase3-p32b-album-rename-design.md).
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AlbumService.h"
#include "services/MediaService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class AlbumController : public drogon::HttpController<AlbumController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AlbumController::createAlbum, "/api/v1/albums", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::listAlbums, "/api/v1/albums", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::getAlbum, "/api/v1/albums/{albumId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::listPhotos, "/api/v1/albums/{albumId}/photos", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::addPhotos, "/api/v1/albums/{albumId}/photos", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::removePhoto, "/api/v1/albums/{albumId}/photos/{mediaId}/removal", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::setCover, "/api/v1/albums/{albumId}/cover", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::renameAlbum, "/api/v1/albums/{albumId}/revisions", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::proposeRename, "/api/v1/albums/{albumId}/proposals", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::listProposals, "/api/v1/albums/{albumId}/proposals", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::getProposal, "/api/v1/albums/{albumId}/proposals/{proposalId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(AlbumController::decideProposal, "/api/v1/albums/{albumId}/proposals/{proposalId}/decision", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void renameAlbum(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        nameWrite(req, callback, albumId, true);
    }

    void proposeRename(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        nameWrite(req, callback, albumId, false);
    }

    // Summaries of one album's rename requests, newest first. A read: no Origin or CSRF, nothing written.
    void listProposals(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<AlbumService::Cursor> after;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            after = AlbumService::decodeCursor(raw);
            if (!after) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        AlbumService::instance().listProposals(userId, albumId, *limit, after, [req, callback](AlbumService::ListResult result) {
            if (result.outcome != AlbumService::Outcome::Ok) {
                respondList(req, callback, result, "proposals", false);
                return;
            }
            Json::Value body(Json::objectValue);
            Json::Value items(Json::arrayValue);
            for (const auto &item : result.items) items.append(item);
            body["proposals"] = items;
            body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
            body["current_version"] = Json::Value(static_cast<Json::Int64>(result.currentVersion));
            callback(jsonResponse(body, 200));
        });
    }

    void getProposal(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId, std::string proposalId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(albumId) || !ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::AlbumProposalNotFound, "改名申请不存在", req));
            return;
        }
        AlbumService::instance().getProposal(userId, albumId, proposalId, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
    }

    void decideProposal(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId, std::string proposalId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        if (!ctrl::looksLikeUuid(proposalId)) {
            callback(ctrl::fail(ErrorCode::AlbumProposalNotFound, "改名申请不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        AlbumService::Decision decision = AlbumService::Decision::Reject;
        std::string message;
        if (!AlbumService::parseDecisionRequest(body, decision, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AlbumService::instance().decide(userId, albumId, proposalId, key, decision, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
    }

    void createAlbum(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        std::string name, message;
        if (!AlbumService::parseCreateRequest(body, name, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AlbumService::instance().create(userId, key, name, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
    }

    void listAlbums(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<AlbumService::Cursor> cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            cursor = AlbumService::decodeCursor(raw);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        AlbumService::instance().list(userId, *limit, cursor, [req, callback](AlbumService::ListResult result) {
            respondList(req, callback, result, "albums", true);
        });
    }

    void getAlbum(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        AlbumService::instance().get(userId, albumId, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
    }

    void listPhotos(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<int> cursor;
        const std::string raw = req->getParameter("cursor");
        if (!raw.empty()) {
            cursor = AlbumService::decodePhotoCursor(raw);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        AlbumService::instance().listPhotos(userId, albumId, *limit, cursor, [req, callback](AlbumService::ListResult result) {
            respondList(req, callback, result, "photos", false);
        });
    }

    void addPhotos(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        // D20: the optional notify_partner is taken out first, so the strict parser sees the legacy body.
        intent::Choice choice;
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        std::vector<std::string> mediaIds;
        if (!AlbumService::parseAddRequest(body, mediaIds, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AlbumService::instance().addPhotos(userId, albumId, key, mediaIds, [req, callback](AlbumService::Status status) { respond(req, callback, status); },
                                           AlbumService::Fault::None, choice);
    }

    void removePhoto(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId, std::string mediaId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        if (!ctrl::looksLikeUuid(mediaId)) {
            callback(ctrl::fail(ErrorCode::AlbumPhotoNotFound, "相册里没有这张照片", req));
            return;
        }
        AlbumService::instance().removePhoto(userId, albumId, mediaId, key, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
    }

    void setCover(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string albumId) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        std::string mediaId, message;
        if (!AlbumService::parseCoverRequest(body, mediaId, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        AlbumService::instance().setCover(userId, albumId, mediaId, key, [req, callback](AlbumService::Status status) { respond(req, callback, status); });
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

    static void nameWrite(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback, const std::string &albumId, bool direct) {
        std::string userId, key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(albumId)) {
            callback(ctrl::fail(ErrorCode::AlbumNotFound, "相册不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        int expected = 0;
        std::string name, message;
        if (!AlbumService::parseRevisionRequest(body, expected, name, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        auto done = [req, callback](AlbumService::Status status) { respond(req, callback, status); };
        if (direct) AlbumService::instance().rename(userId, albumId, key, expected, name, done);
        else AlbumService::instance().propose(userId, albumId, key, expected, name, done);
    }

    static void respondList(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                            const AlbumService::ListResult &result, const char *field, bool withUploads) {
        if (result.outcome != AlbumService::Outcome::Ok) {
            AlbumService::Status status;
            status.outcome = result.outcome;
            status.message = result.message;
            status.detail = result.detail;
            respond(req, callback, status);
            return;
        }
        Json::Value body(Json::objectValue);
        Json::Value items(Json::arrayValue);
        for (const auto &item : result.items) items.append(item);
        body[field] = items;
        body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
        if (withUploads) body["uploads_enabled"] = MediaService::instance().uploadsEnabled();
        callback(jsonResponse(body, 200));
    }

    static void respond(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const AlbumService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(AlbumService::responseBody(status), AlbumService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == AlbumService::Outcome::ResultUnknown) {
            LOG_ERROR << "相册请求：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复保存", req));
            return;
        }
        if (status.outcome == AlbumService::Outcome::Error) {
            LOG_ERROR << "相册请求失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(AlbumService::errorCodeFor(status.outcome), status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
