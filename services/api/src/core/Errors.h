// =====================================================================
// services/api/src/core/Errors.h
//
// 统一错误信封，与 contracts/openapi.yaml 的 ErrorEnvelope 一一对应。
// 不向客户端泄露内部异常文本；内部细节只写日志。
// =====================================================================
#pragma once

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <string>

namespace usapp {

enum class ErrorCode {
    ValidationError,
    Unauthenticated,
    CsrfTokenInvalid,
    OriginNotAllowed,
    RateLimited,
    CodeInvalidOrExpired,
    RegistrationNotAllowed,
    SpaceNotFound,
    SpaceAlreadyExists,
    SpaceFull,
    InviteInvalidOrExpired,
    InviteRevoked,
    AlreadyInSpace,
    NotFound,
    InternalError,
    // 阶段 1 M3-B（契约 0.4.0）：任务发布与读取。
    TaskNotFound,
    PartnerRequired,
    IdempotencyKeyReused,
    ResultUnknown,
};

// 与 OpenAPI 中 error.code 枚举完全一致的稳定字符串。
const char *errorCodeToString(ErrorCode code);

// 默认 HTTP 状态码。
int defaultStatusFor(ErrorCode code);

// 构造错误响应。所有 /api 响应都强制 Cache-Control: no-store。
drogon::HttpResponsePtr errorResponse(ErrorCode code,
                                      const std::string &message,
                                      int status = 0,
                                      const std::string &requestId = "");

// 成功响应（同样带 no-store）。
drogon::HttpResponsePtr jsonResponse(const Json::Value &body, int status = 200);

// 为空响应体（204）构造响应，仍带 no-store 与 cookie 处理发生在调用方。
drogon::HttpResponsePtr noContentResponse();

}  // namespace usapp
