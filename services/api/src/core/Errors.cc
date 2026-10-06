// =====================================================================
// services/api/src/core/Errors.cc
// =====================================================================
#include "core/Errors.h"

namespace usapp {

const char *errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::ValidationError:
            return "VALIDATION_ERROR";
        case ErrorCode::Unauthenticated:
            return "UNAUTHENTICATED";
        case ErrorCode::CsrfTokenInvalid:
            return "CSRF_TOKEN_INVALID";
        case ErrorCode::OriginNotAllowed:
            return "ORIGIN_NOT_ALLOWED";
        case ErrorCode::RateLimited:
            return "RATE_LIMITED";
        case ErrorCode::CodeInvalidOrExpired:
            return "CODE_INVALID_OR_EXPIRED";
        case ErrorCode::RegistrationNotAllowed:
            return "REGISTRATION_NOT_ALLOWED";
        case ErrorCode::SpaceNotFound:
            return "SPACE_NOT_FOUND";
        case ErrorCode::SpaceAlreadyExists:
            return "SPACE_ALREADY_EXISTS";
        case ErrorCode::SpaceFull:
            return "SPACE_FULL";
        case ErrorCode::InviteInvalidOrExpired:
            return "INVITE_INVALID_OR_EXPIRED";
        case ErrorCode::InviteRevoked:
            return "INVITE_REVOKED";
        case ErrorCode::AlreadyInSpace:
            return "ALREADY_IN_SPACE";
        case ErrorCode::NotFound:
            return "NOT_FOUND";
        case ErrorCode::InternalError:
            return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

int defaultStatusFor(ErrorCode code) {
    switch (code) {
        case ErrorCode::ValidationError:
            return 400;
        case ErrorCode::Unauthenticated:
            return 401;
        case ErrorCode::CsrfTokenInvalid:
            return 403;
        case ErrorCode::OriginNotAllowed:
            return 403;
        case ErrorCode::RateLimited:
            return 429;
        case ErrorCode::CodeInvalidOrExpired:
            return 401;
        case ErrorCode::RegistrationNotAllowed:
            return 403;
        case ErrorCode::SpaceNotFound:
            return 404;
        case ErrorCode::SpaceAlreadyExists:
            return 409;
        case ErrorCode::SpaceFull:
            return 409;
        case ErrorCode::InviteInvalidOrExpired:
            return 404;
        case ErrorCode::InviteRevoked:
            return 410;
        case ErrorCode::AlreadyInSpace:
            return 409;
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::InternalError:
            return 500;
    }
    return 500;
}

namespace {

// 业务响应永不进入浏览器 HTTP 缓存，也不进入 Service Worker 缓存。
void applyNoStore(const drogon::HttpResponsePtr &resp) {
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, private");
    resp->addHeader("Pragma", "no-cache");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("Referrer-Policy", "same-origin");
}

}  // namespace

drogon::HttpResponsePtr errorResponse(ErrorCode code,
                                      const std::string &message,
                                      int status,
                                      const std::string &requestId) {
    Json::Value error;
    error["code"] = errorCodeToString(code);
    error["message"] = message;

    Json::Value body;
    body["error"] = error;
    if (!requestId.empty()) {
        body["request_id"] = requestId;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status > 0 ? status
                                                                      : defaultStatusFor(code)));
    applyNoStore(resp);
    return resp;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value &body, int status) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    applyNoStore(resp);
    return resp;
}

drogon::HttpResponsePtr noContentResponse() {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    applyNoStore(resp);
    return resp;
}

}  // namespace usapp
