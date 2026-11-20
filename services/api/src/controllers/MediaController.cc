// =====================================================================
// services/api/src/controllers/MediaController.cc
//
// Phase 3 P3-1a: private media (docs/phase3-plan.md Section 4). Every route carries AuthFilter: writes are checked for
// exact Origin and the session-bound CSRF token by the filter; reads need a formal session. The user comes only from the
// session, and no client value reaches an object key.
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
#include "services/TaskService.h"

namespace usapp::controllers {

class MediaController : public drogon::HttpController<MediaController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MediaController::createUpload, "/api/v1/media/uploads", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::complete, "/api/v1/media/uploads/{mediaId}/complete", drogon::Post, AuthFilter::classTypeName());
    // Before the {mediaId} route, which the same path would otherwise match.
    ADD_METHOD_TO(MediaController::listUndoable, "/api/v1/media/deletions", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::listRetained, "/api/v1/media/retained", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::listUnattached, "/api/v1/media/unattached", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::capabilities, "/api/v1/media/capabilities", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::facts, "/api/v1/media/{mediaId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::content, "/api/v1/media/{mediaId}/content", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::deletePhoto, "/api/v1/media/{mediaId}/deletions", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(MediaController::undoDeletion, "/api/v1/media/{mediaId}/deletions/{deletionId}/undo", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void createUpload(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        std::string contentType, message;
        int64_t size = 0;
        if (!MediaService::parseUploadRequest(body, contentType, size, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MediaService::instance().createUpload(userId, key, contentType, size, [req, callback](MediaService::Status status) {
            respond(req, callback, status);
        });
    }

    void complete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string mediaId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(mediaId)) {
            callback(ctrl::fail(ErrorCode::MediaNotFound, "图片不存在", req));
            return;
        }
        MediaService::instance().complete(userId, mediaId, [req, callback](MediaService::Status status) {
            respond(req, callback, status);
        });
    }

    void facts(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string mediaId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(mediaId)) {
            callback(ctrl::fail(ErrorCode::MediaNotFound, "图片不存在", req));
            return;
        }
        MediaService::instance().facts(userId, mediaId, [req, callback](MediaService::Status status) {
            respond(req, callback, status);
        });
    }

    void content(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string mediaId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(mediaId)) {
            callback(ctrl::fail(ErrorCode::MediaNotFound, "图片不存在", req));
            return;
        }
        std::string variant = req->getParameter("variant");
        if (variant.empty()) variant = "view";
        if (variant != "thumb" && variant != "view" && variant != "original") {
            callback(ctrl::fail(ErrorCode::ValidationError, "variant 只能是 thumb、view 或 original", req));
            return;
        }
        MediaService::instance().content(userId, mediaId, variant, [req, callback](MediaService::Status status) {
            if (!status.success()) {
                respond(req, callback, status);
                return;
            }
            auto response = drogon::HttpResponse::newRedirectionResponse(status.redirectUrl);
            response->addHeader("Cache-Control", "no-store");
            callback(response);
        });
    }

    void listUndoable(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        listPage(req, std::move(callback), "deletions", &MediaService::listUndoable);
    }

    void listRetained(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        listPage(req, std::move(callback), "photos", &MediaService::listRetained);
    }

    // Whether new uploads are on now, for a member of a space. A snapshot: the upload routes still answer MEDIA_DISABLED by themselves.
    void capabilities(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        MediaService::instance().capabilities(userId, [req, callback](MediaService::Capabilities result) {
            if (result.outcome == MediaService::Outcome::Ok) {
                Json::Value body(Json::objectValue);
                body["uploads_enabled"] = result.uploadsEnabled;
                callback(jsonResponse(body, 200));
                return;
            }
            if (result.outcome == MediaService::Outcome::SpaceNotFound) {
                callback(ctrl::fail(ErrorCode::SpaceNotFound, result.message, req));
                return;
            }
            LOG_ERROR << "媒体请求失败 detail=media stage=capabilities request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
        });
    }

    // The caller's own READY photos that may still be attached: a read that reserves and renews nothing.
    void listUnattached(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        listPage(req, std::move(callback), "photos", &MediaService::listUnattached);
    }

    using ListMethod = void (MediaService::*)(const std::string &, int, const std::optional<std::pair<std::string, std::string>> &,
                                              std::function<void(MediaService::DeletionPage)>);
    static void listPage(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> callback, const char *field, ListMethod method) {
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
        (MediaService::instance().*method)(userId, *limit, after, [req, callback, field](MediaService::DeletionPage page) {
            if (page.outcome == MediaService::Outcome::Ok) {
                Json::Value body(Json::objectValue);
                body[field] = Json::Value(Json::arrayValue);
                for (auto &item : page.items) body[field].append(std::move(item));
                body["next_cursor"] = page.nextCursor ? Json::Value(*page.nextCursor) : Json::Value(Json::nullValue);
                callback(jsonResponse(body, 200));
                return;
            }
            if (page.outcome == MediaService::Outcome::SpaceNotFound) {
                callback(ctrl::fail(ErrorCode::SpaceNotFound, page.message, req));
                return;
            }
            LOG_ERROR << "媒体请求失败 detail=" << page.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
        });
    }

    void deletePhoto(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string mediaId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(mediaId)) {
            callback(ctrl::fail(ErrorCode::MediaNotFound, "图片不存在", req));
            return;
        }
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        MediaService::instance().deletePhoto(userId, mediaId, key, [req, callback](MediaService::Status status) {
            respond(req, callback, status);
        });
    }

    void undoDeletion(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string mediaId,
                      std::string deletionId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(mediaId) || !ctrl::looksLikeUuid(deletionId)) {
            callback(ctrl::fail(ErrorCode::MediaNotFound, "图片不存在", req));
            return;
        }
        const std::string key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return;
        }
        MediaService::instance().undoDeletion(userId, mediaId, deletionId, key, [req, callback](MediaService::Status status) {
            respond(req, callback, status);
        });
    }

  private:
    static void respond(const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const MediaService::Status &status) {
        if (status.success()) {
            callback(jsonResponse(MediaService::responseBody(status), MediaService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == MediaService::Outcome::ResultUnknown) {
            LOG_ERROR << "媒体请求：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试", req));
            return;
        }
        if (status.outcome == MediaService::Outcome::Error) {
            LOG_ERROR << "媒体请求失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(MediaService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
