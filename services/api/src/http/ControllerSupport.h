// =====================================================================
// services/api/src/http/ControllerSupport.h
//
// 控制器共用的小工具（header-only，便于各控制器与测试共用同一份实现）。
//
// 存在的理由：错误码映射、JSON 形状、分页游标、用户信息读取这四件事
// 如果每个控制器各写一份，迟早会分叉。这里的实现必须是唯一的。
//
// 该文件不承载任何安全判定：认证、CSRF、Origin、管理面边界全部在
// AuthFilter / AdminFilter 中完成，控制器只消费过滤器写入的请求属性。
// =====================================================================
#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <ctime>
#include <functional>
#include <string>

#include "core/Errors.h"
#include "db/Db.h"
#include "http/AuthFilter.h"
#include "http/HttpUtil.h"
#include "services/SpaceService.h"

namespace usapp::ctrl {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

// ---- 统一出口 ----
// 所有错误都经这里：保证 (a) 形状统一，(b) 带 request id，(c) 带 no-store。
// 细节（数据库报错、内部状态）只写日志，绝不进响应体。
inline drogon::HttpResponsePtr fail(ErrorCode code,
                                    const std::string &message,
                                    const drogon::HttpRequestPtr &req) {
    return errorResponse(code, message, 0, http::requestId(req));
}

// 需要"已认证用户"。未认证直接回 401 并返回 false，调用方立即 return。
// 注意：AuthFilter 已经拦过一次；这里再判一次是因为控制器不能假设
// 路由一定挂了过滤器（误配路由时必须是 401 而不是继续执行）。
inline bool requireUser(const drogon::HttpRequestPtr &req,
                        const Callback &cb,
                        std::string &userId) {
    userId = authctx::userId(req);
    if (!authctx::isAuthenticated(req) || userId.empty()) {
        cb(fail(ErrorCode::Unauthenticated, "请先登录", req));
        return false;
    }
    return true;
}

// 请求体必须是 JSON 对象；不是就直接回 400 并返回 false。
inline bool parseBody(const drogon::HttpRequestPtr &req, const Callback &cb, Json::Value &out) {
    if (!http::isJsonObject(req, out)) {
        cb(fail(ErrorCode::ValidationError, "请求体必须是一个 JSON 对象", req));
        return false;
    }
    return true;
}

// ---- SpaceService::Outcome -> 错误码 ----
// 一一对应关系写死在这里，避免各控制器各映射一套。
inline ErrorCode errorCodeForSpaceOutcome(SpaceService::Outcome outcome) {
    switch (outcome) {
        case SpaceService::Outcome::AlreadyInSpace:
            return ErrorCode::AlreadyInSpace;
        case SpaceService::Outcome::SpaceNotFound:
            return ErrorCode::SpaceNotFound;
        case SpaceService::Outcome::SpaceFull:
            return ErrorCode::SpaceFull;
        case SpaceService::Outcome::InviteInvalidOrExpired:
            return ErrorCode::InviteInvalidOrExpired;
        case SpaceService::Outcome::InviteRevoked:
            return ErrorCode::InviteRevoked;
        case SpaceService::Outcome::Validation:
            return ErrorCode::ValidationError;
        case SpaceService::Outcome::Ok:
        case SpaceService::Outcome::Error:
        default:
            return ErrorCode::InternalError;
    }
}

// 仅供失败路径使用；成功路径不要调用它。
inline drogon::HttpResponsePtr spaceFailure(const SpaceService::Status &status,
                                            const drogon::HttpRequestPtr &req) {
    if (status.ok()) {
        // 不该发生：调用方把成功当失败处理了。宁可 500 也不要静默成功。
        LOG_ERROR << "spaceFailure() 被用于成功状态，request_id=" << http::requestId(req);
        return fail(ErrorCode::InternalError, "服务内部错误", req);
    }
    if (!status.detail.empty()) {
        LOG_WARN << "空间操作失败 outcome=" << static_cast<int>(status.outcome)
                 << " detail=" << status.detail << " request_id=" << http::requestId(req);
    }
    return fail(errorCodeForSpaceOutcome(status.outcome),
                SpaceService::outcomeMessage(status.outcome), req);
}

// ---- JSON 形状（唯一事实来源：contracts/openapi.yaml）----
// 注意 SpaceMember 里刻意没有 email：契约不含该字段，成员邮箱也不该由
// 同伴在空间接口里读到。
inline Json::Value memberJson(const SpaceMemberInfo &member) {
    Json::Value out(Json::objectValue);
    out["user_id"] = member.userId;
    out["slot"] = member.slot;
    out["display_name"] = member.displayName;
    out["joined_at"] = member.joinedAt;
    return out;
}

inline Json::Value spaceJson(const SpaceInfo &space, bool includeMembers = true) {
    Json::Value out(Json::objectValue);
    out["id"] = space.spaceId;
    out["name"] = space.name;
    // 阶段 0 固定 Asia/Shanghai，但字段保留（后续可能按空间配置）。
    out["timezone"] = space.timezone.empty() ? std::string("Asia/Shanghai") : space.timezone;
    out["member_count"] = static_cast<int>(space.members.size());
    out["created_at"] = space.createdAt;
    if (includeMembers) {
        Json::Value arr(Json::arrayValue);
        for (const auto &member : space.members) {
            arr.append(memberJson(member));
        }
        out["members"] = arr;
    }
    return out;
}

// Invite 形状（契约 Invite）。永不包含邀请码明文：明文只在创建响应里出现一次。
inline Json::Value inviteJson(const SpaceService::InviteInfo &invite) {
    Json::Value out(Json::objectValue);
    out["id"] = invite.id;
    out["expires_at"] = invite.expiresAt;
    out["created_at"] = invite.createdAt;
    out["max_uses"] = invite.maxUses;
    out["used_count"] = invite.usedCount;
    out["revoked"] = invite.revoked;
    return out;
}

// 创建邀请的响应（契约 CreatedInvite）= Invite + code + invite_url。
// invite_url 用 PUBLIC_ORIGIN 拼，而不是用请求的 Host 头：Host 头由客户端
// 控制，用它拼链接会把邀请码指到攻击者的域名上。
inline Json::Value createdInviteJson(const std::string &inviteId,
                                     const std::string &expiresAt,
                                     const std::string &rawCode,
                                     const std::string &publicOrigin) {
    Json::Value out(Json::objectValue);
    out["id"] = inviteId;
    out["expires_at"] = expiresAt;
    // 邀请刚刚插入，created_at 用当前时刻近似（差一个事务的毫秒级）。
    out["created_at"] = http::nowIso8601Utc();
    out["max_uses"] = SpaceService::kInviteMaxUses;
    out["used_count"] = 0;
    out["revoked"] = false;
    out["code"] = rawCode;
    out["invite_url"] = publicOrigin + "/invite/" + rawCode;
    return out;
}

// ---- 路径参数形状校验 ----
// 目的只是别把非法字符串塞进 `$1::uuid`：那会让 PostgreSQL 抛错，
// 最终变成一个假的 500，而正确结果是 404。它不是安全边界。
inline bool looksLikeUuid(const std::string &value) {
    if (value.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < 36; ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline Json::Value userJson(const std::string &id,
                            const std::string &email,
                            const std::string &displayName,
                            const std::string &createdAt) {
    Json::Value out(Json::objectValue);
    out["id"] = id;
    out["email"] = email;
    out["display_name"] = displayName;
    out["created_at"] = createdAt;
    return out;
}

// ---- 读用户信息 ----
// 契约的 User 形状需要 email 与 created_at，而过滤器只写入 user_id，
// 所以引导、登录与 /me 都要回表一次。
// 用 email_display（用户原始输入）而不是 email_canonical：前者才是"显示用"的值。
inline void loadUserJson(const std::string &userId,
                         const std::function<void(Json::Value)> &onFound,
                         const std::function<void(const std::string &)> &onError) {
    static const char *kSql =
        R"(SELECT id::text AS id,
                  email_display AS email,
                  display_name,
                  to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
             FROM users
            WHERE id = $1::uuid)";
    db::Pool::instance().client()->execSqlAsync(
        kSql,
        [onFound, onError](const drogon::orm::Result &rows) {
            if (rows.size() == 0) {
                onError("用户不存在");
                return;
            }
            const auto &row = rows[0];
            onFound(userJson(row["id"].as<std::string>(),
                             row["email"].as<std::string>(),
                             row["display_name"].as<std::string>(),
                             row["created_at"].as<std::string>()));
        },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(db::describePgError("读取用户信息失败", e.base()));
        },
        userId);
}

// ---- 时间 ----
inline std::string nowPlusSecondsIso(int seconds) {
    return http::toIso8601Utc(std::time(nullptr) + static_cast<std::time_t>(seconds));
}

// ---- 分页游标（阶段 0 骨架）----
// 契约只承诺"不透明字符串"，没有承诺格式。这里在内部用 "off-<十进制偏移>"。
// 刻意不做 base64：看不出来源的游标会被误当作稳定接口，而它并不稳定。
// 后续换 keyset 分页时格式可直接改变，客户端只需原样回传。
inline std::string encodeCursor(long long offset) {
    return "off-" + std::to_string(offset);
}

inline bool decodeCursor(const std::string &raw, long long &offset) {
    offset = 0;
    if (raw.empty()) {
        return true;  // 未提供游标 = 第一页
    }
    if (raw.rfind("off-", 0) != 0) {
        return false;
    }
    const std::string digits = raw.substr(4);
    if (digits.empty() || digits.size() > 9) {
        return false;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    offset = std::stoll(digits);
    return true;
}

// limit 非法时不报错而是收敛到合法范围（列表接口，宽容比 400 更有用）。
inline int clampLimit(const drogon::HttpRequestPtr &req) {
    const std::string raw = req->getParameter("limit");
    if (raw.empty()) {
        return 50;
    }
    try {
        const int value = std::stoi(raw);
        if (value < 1) {
            return 1;
        }
        if (value > 100) {
            return 100;
        }
        return value;
    } catch (const std::exception &) {
        return 50;
    }
}

// 下一页游标：取满 limit 行说明可能还有下一页。
inline Json::Value nextCursorOrNull(size_t returned, int limit, long long offset) {
    if (static_cast<int>(returned) < limit) {
        return Json::Value(Json::nullValue);
    }
    return Json::Value(encodeCursor(offset + limit));
}

}  // namespace usapp::ctrl
