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
    // 阶段 1 M3-C1（契约 0.5.0）：任务完成与奖励结算。
    TaskNotAssignee,
    TaskAlreadyCompleted,
    TaskExpired,
    WalletBalanceLimit,
    AdminAdjustmentConflict,
    AdminAdjustmentForbidden,
    TaskCompletionModeInvalid,
    // M5-C (contract 0.9.0): a custom date range has not started yet.
    TaskNotStarted,
    TaskNotCompleted,
    TaskReactionForbidden,
    TaskAlreadyReacted,
    // M6 (contract 0.10.0): confirmed task edits and cancellations.
    TaskChangePending,
    TaskHasProgress,
    TaskChangeDecided,
    TaskChangeForbidden,
    TaskChangeNotFound,
    TaskCancelled,
    TaskSuperseded,
    // M7 (contract 0.11.0): reward-shop products.
    ProductNotFound,
    ProductForbidden,
    ProductArchived,
    ProductVersionConflict,
    // M8 (contract 0.12.0): direct redemption.
    InsufficientBalance,
    RedemptionNotFound,
    RedemptionForbidden,
    RedemptionModeUnsupported,
    RedemptionAlreadyFulfilled,
    RedemptionStateConflict,
    // P2-1 messages.
    MessageNotFound,
    MessageReplyForbidden,
    MessageAlreadyReplied,
    // P2-2 anniversaries.
    AnniversaryNotFound,
    AnniversaryEditForbidden,
    AnniversaryVersionConflict,
    AnniversaryProposalPending,
    AnniversaryProposalNotFound,
    AnniversaryProposalDecided,
    AnniversaryProposalStale,
    // P2-5a Web Push device subscriptions.
    PushUnavailable,
    PushDeviceLimit,
    PushSubscriptionNotFound,
    // P2-6 reminders.
    ReminderNotAssignee,
    ReminderTargetClosed,
    ReminderTimeInvalid,
    ReminderTimePassed,
    MediaDisabled,
    MediaNotFound,
    MediaLimitExceeded,
    MediaNotUploaded,
    MediaGrantExpired,
    MediaStoreUnavailable,
    MediaAlreadyDeleted,
    MediaUndoClosed,
    MediaDeletionNotActive,
    MediaDeletionLinked,
    MemoryNotFound,
    MemoryEditForbidden,
    MemoryVersionConflict,
    MemoryProposalPending,
    MemoryProposalNotFound,
    MemoryProposalDecided,
    MemoryProposalStale,
    MemoryPhotoUnavailable,
    MemoryAlreadyDeleted,
    MemoryUndoClosed,
    MemoryDeletionNotActive,
    AlbumNotFound,
    AlbumPhotoUnavailable,
    AlbumPhotoNotFound,
    AlbumPhotoForbidden,
    AlbumPhotoRemoved,
    PlanEditForbidden,
    PlanConflict,
    AlbumEditForbidden,
    AlbumVersionConflict,
    AlbumProposalPending,
    AlbumProposalNotFound,
    AlbumProposalDecided,
    AvatarItemNotFound,
    AvatarVersionConflict,
    AvatarAssetInvalid,
    AvatarStarterInvalid,
    AvatarAlreadyOwned,
    AvatarItemUnavailable,
    AvatarSlotInvalid,
    AvatarNotOwned,
    AvatarDefaultsUnavailable,
    // E1 user-data export (docs/user-data-export-design.md Section 8.1).
    ExportTooLarge,
    ExportUnavailable,
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

// E1 export: an already-serialized JSON document sent byte for byte as an attachment, with the same no-store headers.
drogon::HttpResponsePtr attachmentJsonResponse(std::string body, const std::string &filename);

}  // namespace usapp
