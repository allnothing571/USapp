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
        case ErrorCode::TaskNotFound:
            return "TASK_NOT_FOUND";
        case ErrorCode::PartnerRequired:
            return "PARTNER_REQUIRED";
        case ErrorCode::IdempotencyKeyReused:
            return "IDEMPOTENCY_KEY_REUSED";
        case ErrorCode::ResultUnknown:
            return "RESULT_UNKNOWN";
        case ErrorCode::TaskNotAssignee:
            return "TASK_NOT_ASSIGNEE";
        case ErrorCode::TaskAlreadyCompleted:
            return "TASK_ALREADY_COMPLETED";
        case ErrorCode::TaskExpired:
            return "TASK_EXPIRED";
        case ErrorCode::WalletBalanceLimit:
            return "WALLET_BALANCE_LIMIT";
        case ErrorCode::AdminAdjustmentConflict:
            return "ADMIN_ADJUSTMENT_CONFLICT";
        case ErrorCode::AdminAdjustmentForbidden:
            return "ADMIN_ADJUSTMENT_FORBIDDEN";
        case ErrorCode::TaskCompletionModeInvalid:
            return "TASK_COMPLETION_MODE_INVALID";
        case ErrorCode::TaskNotStarted:
            return "TASK_NOT_STARTED";
        case ErrorCode::TaskChangePending:
            return "TASK_CHANGE_PENDING";
        case ErrorCode::TaskHasProgress:
            return "TASK_HAS_PROGRESS";
        case ErrorCode::TaskChangeDecided:
            return "TASK_CHANGE_DECIDED";
        case ErrorCode::TaskChangeForbidden:
            return "TASK_CHANGE_FORBIDDEN";
        case ErrorCode::TaskChangeNotFound:
            return "TASK_CHANGE_NOT_FOUND";
        case ErrorCode::TaskCancelled:
            return "TASK_CANCELLED";
        case ErrorCode::TaskSuperseded:
            return "TASK_SUPERSEDED";
        case ErrorCode::ProductNotFound:
            return "PRODUCT_NOT_FOUND";
        case ErrorCode::ProductForbidden:
            return "PRODUCT_FORBIDDEN";
        case ErrorCode::ProductArchived:
            return "PRODUCT_ARCHIVED";
        case ErrorCode::ProductVersionConflict:
            return "PRODUCT_VERSION_CONFLICT";
        case ErrorCode::InsufficientBalance:
            return "INSUFFICIENT_BALANCE";
        case ErrorCode::RedemptionNotFound:
            return "REDEMPTION_NOT_FOUND";
        case ErrorCode::RedemptionForbidden:
            return "REDEMPTION_FORBIDDEN";
        case ErrorCode::RedemptionModeUnsupported:
            return "REDEMPTION_MODE_UNSUPPORTED";
        case ErrorCode::RedemptionStateConflict:
            return "REDEMPTION_STATE_CONFLICT";
        case ErrorCode::RedemptionAlreadyFulfilled:
            return "REDEMPTION_ALREADY_FULFILLED";
        case ErrorCode::MessageNotFound:
            return "MESSAGE_NOT_FOUND";
        case ErrorCode::MessageReplyForbidden:
            return "MESSAGE_REPLY_FORBIDDEN";
        case ErrorCode::MessageAlreadyReplied:
            return "MESSAGE_ALREADY_REPLIED";
        case ErrorCode::AnniversaryNotFound:
            return "ANNIVERSARY_NOT_FOUND";
        case ErrorCode::AnniversaryEditForbidden:
            return "ANNIVERSARY_EDIT_FORBIDDEN";
        case ErrorCode::AnniversaryVersionConflict:
            return "ANNIVERSARY_VERSION_CONFLICT";
        case ErrorCode::AnniversaryProposalPending:
            return "ANNIVERSARY_PROPOSAL_PENDING";
        case ErrorCode::AnniversaryProposalNotFound:
            return "ANNIVERSARY_PROPOSAL_NOT_FOUND";
        case ErrorCode::AnniversaryProposalDecided:
            return "ANNIVERSARY_PROPOSAL_DECIDED";
        case ErrorCode::AnniversaryProposalStale:
            return "ANNIVERSARY_PROPOSAL_STALE";
        case ErrorCode::TaskNotCompleted: return "TASK_NOT_COMPLETED";
        case ErrorCode::TaskReactionForbidden: return "TASK_REACTION_FORBIDDEN";
        case ErrorCode::TaskAlreadyReacted: return "TASK_ALREADY_REACTED";
        case ErrorCode::PushUnavailable: return "PUSH_UNAVAILABLE";
        case ErrorCode::PushDeviceLimit: return "PUSH_DEVICE_LIMIT";
        case ErrorCode::PushSubscriptionNotFound: return "PUSH_SUBSCRIPTION_NOT_FOUND";
        case ErrorCode::ReminderNotAssignee: return "REMINDER_NOT_ASSIGNEE";
        case ErrorCode::ReminderTargetClosed: return "REMINDER_TARGET_CLOSED";
        case ErrorCode::ReminderTimeInvalid: return "REMINDER_TIME_INVALID";
        case ErrorCode::ReminderTimePassed: return "REMINDER_TIME_PASSED";
        case ErrorCode::MediaDisabled: return "MEDIA_DISABLED";
        case ErrorCode::MediaNotFound: return "MEDIA_NOT_FOUND";
        case ErrorCode::MediaLimitExceeded: return "MEDIA_LIMIT_EXCEEDED";
        case ErrorCode::MediaNotUploaded: return "MEDIA_NOT_UPLOADED";
        case ErrorCode::MediaGrantExpired: return "MEDIA_GRANT_EXPIRED";
        case ErrorCode::MediaStoreUnavailable: return "MEDIA_STORE_UNAVAILABLE";
        case ErrorCode::MediaAlreadyDeleted: return "MEDIA_ALREADY_DELETED";
        case ErrorCode::MediaUndoClosed: return "MEDIA_UNDO_CLOSED";
        case ErrorCode::MediaDeletionNotActive: return "MEDIA_DELETION_NOT_ACTIVE";
        case ErrorCode::MediaDeletionLinked: return "MEDIA_DELETION_LINKED";
        case ErrorCode::MemoryNotFound: return "MEMORY_NOT_FOUND";
        case ErrorCode::MemoryEditForbidden: return "MEMORY_EDIT_FORBIDDEN";
        case ErrorCode::MemoryVersionConflict: return "MEMORY_VERSION_CONFLICT";
        case ErrorCode::MemoryProposalPending: return "MEMORY_PROPOSAL_PENDING";
        case ErrorCode::MemoryProposalNotFound: return "MEMORY_PROPOSAL_NOT_FOUND";
        case ErrorCode::MemoryProposalDecided: return "MEMORY_PROPOSAL_DECIDED";
        case ErrorCode::MemoryProposalStale: return "MEMORY_PROPOSAL_STALE";
        case ErrorCode::MemoryPhotoUnavailable: return "MEMORY_PHOTO_UNAVAILABLE";
        case ErrorCode::MemoryAlreadyDeleted: return "MEMORY_ALREADY_DELETED";
        case ErrorCode::MemoryUndoClosed: return "MEMORY_UNDO_CLOSED";
        case ErrorCode::MemoryDeletionNotActive: return "MEMORY_DELETION_NOT_ACTIVE";
        case ErrorCode::AlbumNotFound: return "ALBUM_NOT_FOUND";
        case ErrorCode::AlbumPhotoUnavailable: return "ALBUM_PHOTO_UNAVAILABLE";
        case ErrorCode::AlbumPhotoNotFound: return "ALBUM_PHOTO_NOT_FOUND";
        case ErrorCode::AlbumPhotoForbidden: return "ALBUM_PHOTO_FORBIDDEN";
        case ErrorCode::AlbumPhotoRemoved: return "ALBUM_PHOTO_REMOVED";
        case ErrorCode::PlanEditForbidden: return "PLAN_EDIT_FORBIDDEN";
        case ErrorCode::PlanConflict: return "PLAN_CONFLICT";
        case ErrorCode::AlbumEditForbidden: return "ALBUM_EDIT_FORBIDDEN";
        case ErrorCode::AlbumVersionConflict: return "ALBUM_VERSION_CONFLICT";
        case ErrorCode::AlbumProposalPending: return "ALBUM_PROPOSAL_PENDING";
        case ErrorCode::AlbumProposalNotFound: return "ALBUM_PROPOSAL_NOT_FOUND";
        case ErrorCode::AlbumProposalDecided: return "ALBUM_PROPOSAL_DECIDED";
        case ErrorCode::AvatarItemNotFound: return "AVATAR_ITEM_NOT_FOUND";
        case ErrorCode::AvatarVersionConflict: return "AVATAR_VERSION_CONFLICT";
        case ErrorCode::SharedRoomVersionConflict: return "SHARED_ROOM_VERSION_CONFLICT";
        case ErrorCode::RoomFurnitureOwned: return "ROOM_FURNITURE_OWNED";
        case ErrorCode::SharedGardenDateChanged: return "SHARED_GARDEN_DATE_CHANGED";
        case ErrorCode::AvatarAssetInvalid: return "AVATAR_ASSET_INVALID";
        case ErrorCode::AvatarStarterInvalid: return "AVATAR_STARTER_INVALID";
        case ErrorCode::AvatarAlreadyOwned: return "AVATAR_ALREADY_OWNED";
        case ErrorCode::AvatarItemUnavailable: return "AVATAR_ITEM_UNAVAILABLE";
        case ErrorCode::AvatarSlotInvalid: return "AVATAR_SLOT_INVALID";
        case ErrorCode::AvatarNotOwned: return "AVATAR_NOT_OWNED";
        case ErrorCode::AvatarDefaultsUnavailable: return "AVATAR_DEFAULTS_UNAVAILABLE";
        case ErrorCode::ExportTooLarge: return "EXPORT_TOO_LARGE";
        case ErrorCode::ExportUnavailable: return "EXPORT_UNAVAILABLE";
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
        case ErrorCode::TaskNotFound:
            return 404;
        case ErrorCode::PartnerRequired:
            return 409;
        case ErrorCode::IdempotencyKeyReused:
            return 409;
        case ErrorCode::ResultUnknown:
            return 503;
        case ErrorCode::TaskNotAssignee:
            return 403;
        case ErrorCode::TaskAlreadyCompleted:
            return 409;
        case ErrorCode::TaskExpired:
            return 409;
        case ErrorCode::WalletBalanceLimit:
            return 409;
        case ErrorCode::AdminAdjustmentConflict:
            return 409;
        case ErrorCode::AdminAdjustmentForbidden:
            return 403;
        case ErrorCode::TaskCompletionModeInvalid:
            return 409;
        case ErrorCode::TaskNotStarted:
            return 409;
        case ErrorCode::TaskChangePending:
        case ErrorCode::TaskHasProgress:
        case ErrorCode::TaskChangeDecided:
        case ErrorCode::TaskCancelled:
        case ErrorCode::TaskSuperseded:
            return 409;
        case ErrorCode::TaskChangeForbidden:
            return 403;
        case ErrorCode::TaskChangeNotFound:
            return 404;
        case ErrorCode::ProductNotFound:
            return 404;
        case ErrorCode::ProductForbidden:
            return 403;
        case ErrorCode::ProductArchived:
        case ErrorCode::ProductVersionConflict:
            return 409;
        case ErrorCode::InsufficientBalance:
        case ErrorCode::RedemptionStateConflict:
        case ErrorCode::RedemptionModeUnsupported:
        case ErrorCode::RedemptionAlreadyFulfilled:
            return 409;
        case ErrorCode::RedemptionNotFound:
            return 404;
        case ErrorCode::RedemptionForbidden:
            return 403;
        case ErrorCode::MessageNotFound:
            return 404;
        case ErrorCode::MessageReplyForbidden:
            return 403;
        case ErrorCode::MessageAlreadyReplied:
            return 409;
        case ErrorCode::AnniversaryNotFound:
        case ErrorCode::AnniversaryProposalNotFound:
            return 404;
        case ErrorCode::AnniversaryEditForbidden:
            return 403;
        case ErrorCode::AnniversaryVersionConflict:
        case ErrorCode::AnniversaryProposalPending:
        case ErrorCode::AnniversaryProposalDecided:
        case ErrorCode::AnniversaryProposalStale:
            return 409;
        case ErrorCode::TaskNotCompleted:
        case ErrorCode::TaskAlreadyReacted: return 409;
        case ErrorCode::TaskReactionForbidden: return 403;
        case ErrorCode::PushUnavailable:
        case ErrorCode::PushDeviceLimit: return 409;
        case ErrorCode::PushSubscriptionNotFound: return 404;
        case ErrorCode::MediaDisabled:
        case ErrorCode::MediaStoreUnavailable: return 503;
        case ErrorCode::MediaAlreadyDeleted: return 409;
        case ErrorCode::MediaUndoClosed: return 409;
        case ErrorCode::MediaDeletionNotActive: return 409;
        case ErrorCode::MediaDeletionLinked: return 409;
        case ErrorCode::MediaNotFound: return 404;
        case ErrorCode::MediaLimitExceeded: return 429;
        case ErrorCode::MediaNotUploaded:
        case ErrorCode::MediaGrantExpired: return 409;
        case ErrorCode::MemoryNotFound:
        case ErrorCode::MemoryProposalNotFound: return 404;
        case ErrorCode::MemoryEditForbidden: return 403;
        case ErrorCode::MemoryVersionConflict:
        case ErrorCode::MemoryProposalPending:
        case ErrorCode::MemoryProposalDecided:
        case ErrorCode::MemoryProposalStale:
        case ErrorCode::MemoryPhotoUnavailable: return 409;
        case ErrorCode::MemoryAlreadyDeleted: return 409;
        case ErrorCode::MemoryUndoClosed: return 409;
        case ErrorCode::MemoryDeletionNotActive: return 409;
        case ErrorCode::AlbumNotFound:
        case ErrorCode::AlbumProposalNotFound:
        case ErrorCode::AlbumPhotoNotFound: return 404;
        case ErrorCode::AlbumEditForbidden:
        case ErrorCode::PlanEditForbidden:
        case ErrorCode::AlbumPhotoForbidden: return 403;
        case ErrorCode::PlanConflict: return 409;
        case ErrorCode::AlbumVersionConflict:
        case ErrorCode::AlbumProposalPending:
        case ErrorCode::AlbumProposalDecided:
        case ErrorCode::AlbumPhotoUnavailable:
        case ErrorCode::AlbumPhotoRemoved: return 409;
        case ErrorCode::ReminderNotAssignee: return 403;
        case ErrorCode::ReminderTimeInvalid: return 400;
        case ErrorCode::ReminderTargetClosed:
        case ErrorCode::ReminderTimePassed: return 409;
        case ErrorCode::AvatarItemNotFound: return 404;
        case ErrorCode::AvatarVersionConflict: return 409;
        case ErrorCode::RoomFurnitureOwned:
        case ErrorCode::SharedRoomVersionConflict: return 409;
        case ErrorCode::SharedGardenDateChanged: return 409;
        case ErrorCode::AvatarAssetInvalid:
        case ErrorCode::AvatarStarterInvalid: return 400;
        case ErrorCode::AvatarAlreadyOwned:
        case ErrorCode::AvatarItemUnavailable: return 409;
        case ErrorCode::AvatarSlotInvalid: return 400;
        case ErrorCode::AvatarNotOwned:
        case ErrorCode::AvatarDefaultsUnavailable: return 409;
        case ErrorCode::ExportTooLarge: return 413;
        case ErrorCode::ExportUnavailable: return 503;
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

drogon::HttpResponsePtr attachmentJsonResponse(std::string body, const std::string &filename) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCodeAndCustomString(drogon::CT_APPLICATION_JSON, "application/json; charset=utf-8");
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    resp->setBody(std::move(body));
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
