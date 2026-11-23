// =====================================================================
// services/api/src/services/SpaceService.cc
// =====================================================================
#include "services/SpaceService.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AuditLog.h"

namespace usapp {

namespace {

constexpr const char *kIsoFormat = "YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"";

// 一次查询取回"空间 + 全部成员"。用 LEFT JOIN 让"只有创建者、还没有第二个成员"
// 的空间也能返回一行（否则空空间会被误判为不存在）。
const char *kSpaceQuery =
    "SELECT c.id::text AS space_id, c.name, c.timezone, c.status, "
    "       to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at, "
    "       m.user_id::text AS member_user_id, "
    "       COALESCE(u.display_name, '')  AS display_name, "
    "       COALESCE(u.email_display, '') AS email_display, "
    "       m.slot AS slot, "
    "       to_char(m.joined_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS joined_at "
    "FROM couple_members AS self_m "
    "JOIN couples AS c ON c.id = self_m.couple_id "
    "LEFT JOIN couple_members AS m ON m.couple_id = c.id "
    "LEFT JOIN users AS u ON u.id = m.user_id "
    "WHERE self_m.user_id = $1::uuid "
    "ORDER BY m.slot";

const char *kSpaceQueryById =
    "SELECT c.id::text AS space_id, c.name, c.timezone, c.status, "
    "       to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at, "
    "       m.user_id::text AS member_user_id, "
    "       COALESCE(u.display_name, '')  AS display_name, "
    "       COALESCE(u.email_display, '') AS email_display, "
    "       m.slot AS slot, "
    "       to_char(m.joined_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS joined_at "
    "FROM couple_members AS self_m "
    "JOIN couples AS c ON c.id = self_m.couple_id "
    "LEFT JOIN couple_members AS m ON m.couple_id = c.id "
    "LEFT JOIN users AS u ON u.id = m.user_id "
    "WHERE self_m.user_id = $1::uuid AND c.id = $2::uuid "
    "ORDER BY m.slot";

const char *kAuditInsert =
    "INSERT INTO audit_logs "
    "(actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3, $4, $5, $6::jsonb)";

// 当前空间的邀请列表（只含元数据，永不回明文码）。
const char *kInviteListQuery =
    "SELECT i.id::text AS id, i.max_uses, i.used_count, "
    "       (i.revoked_at IS NOT NULL) AS revoked, "
    "       to_char(i.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires_at, "
    "       to_char(i.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
    "FROM space_invites AS i "
    "WHERE i.couple_id = $1::uuid "
    "ORDER BY i.created_at DESC, i.id "
    "LIMIT 50";

// 邀请码字母表：去掉 0/O、1/I/L，避免用户抄错。
constexpr const char *kInviteAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kInviteCodeLength = 12;
constexpr size_t kInviteCodeGroup = 4;
constexpr int kInviteMaxInsertAttempts = 3;

std::string toJsonText(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

void assembleSpace(const drogon::orm::Result &rows, SpaceInfo &info) {
    info = SpaceInfo{};
    if (rows.empty()) {
        return;
    }
    const auto &first = rows[0];
    info.spaceId = first["space_id"].as<std::string>();
    info.name = first["name"].as<std::string>();
    info.timezone = first["timezone"].as<std::string>();
    info.status = first["status"].as<std::string>();
    info.createdAt = first["created_at"].as<std::string>();

    for (const auto &row : rows) {
        if (row["member_user_id"].isNull()) {
            continue;  // 空间还没有第二个成员
        }
        SpaceMemberInfo member;
        member.userId = row["member_user_id"].as<std::string>();
        member.displayName = row["display_name"].as<std::string>();
        member.emailDisplay = row["email_display"].as<std::string>();
        member.slot = row["slot"].as<int>();
        member.joinedAt = row["joined_at"].as<std::string>();
        info.members.push_back(std::move(member));
    }
}

SpaceService::Status makeStatus(SpaceService::Outcome outcome, std::string detail = "") {
    SpaceService::Status status;
    status.outcome = outcome;
    status.detail = std::move(detail);
    return status;
}

void failWithDb(const std::shared_ptr<db::TxScope> &scope,
                const char *context,
                const drogon::orm::DrogonDbException &e) {
    scope->fail(db::describePgError(context, e.base()));
}

// =====================================================================
// 创建空间
// =====================================================================
struct CreateSpaceCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    std::string spaceId;
    SpaceService::SpaceCallback done;
};

using CreateSpaceCtxPtr = std::shared_ptr<CreateSpaceCtx>;

void createRollback(const CreateSpaceCtxPtr &ctx, const SpaceService::Status &status) {
    ctx->scope->rollback([ctx, status]() { ctx->done(status, SpaceInfo{}); });
}

// ---- 步骤 6：读回刚创建的空间，提交后回调 ----
void createStepLoadSpace(const CreateSpaceCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kSpaceQuery,
        [ctx](const drogon::orm::Result &rows) {
            SpaceInfo info;
            assembleSpace(rows, info);
            if (info.spaceId.empty()) {
                createRollback(ctx, makeStatus(SpaceService::Outcome::Error, "空间写入后读回为空"));
                return;
            }
            ctx->scope->commit([ctx, info]() { ctx->done(makeStatus(SpaceService::Outcome::Ok), info); });
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "读回空间失败", e); },
        ctx->userId);
}

// ---- 步骤 5：写审计（与业务同事务） ----
void createStepAudit(const CreateSpaceCtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["slot"] = 1;
    ctx->scope->tx().execSqlAsync(
        kAuditInsert,
        [ctx](const drogon::orm::Result &) { createStepLoadSpace(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "写空间审计失败", e); },
        ctx->userId,
        ctx->spaceId,
        std::string("space.created"),
        std::string("couple"),
        ctx->spaceId,
        toJsonText(detail));
}

// ---- 步骤 4：把自己放入 slot 1 ----
void createStepInsertMember(const CreateSpaceCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO couple_members (couple_id, user_id, slot) VALUES ($1::uuid, $2::uuid, 1)",
        [ctx](const drogon::orm::Result &) { createStepAudit(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "写入空间成员失败", e); },
        ctx->spaceId,
        ctx->userId);
}

// ---- 步骤 3：插入空间 ----
void createStepInsertSpace(const CreateSpaceCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO couples (created_by_user_id) VALUES ($1::uuid) RETURNING id::text AS id",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.empty()) {
                createRollback(ctx, makeStatus(SpaceService::Outcome::Error, "插入空间未返回 id"));
                return;
            }
            ctx->spaceId = rows[0]["id"].as<std::string>();
            createStepInsertMember(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "创建空间失败", e); },
        ctx->userId);
}

// ---- 步骤 2：确认该用户还没有任何空间 ----
void createStepCheckMembership(const CreateSpaceCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT couple_id::text AS couple_id FROM couple_members WHERE user_id = $1::uuid",
        [ctx](const drogon::orm::Result &rows) {
            if (!rows.empty()) {
                createRollback(ctx, makeStatus(SpaceService::Outcome::AlreadyInSpace,
                                               "该用户已在空间 " +
                                                   rows[0]["couple_id"].as<std::string>()));
                return;
            }
            createStepInsertSpace(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failWithDb(ctx->scope, "查询空间成员关系失败", e);
        },
        ctx->userId);
}

// ---- 步骤 1：锁住 users 行，让同一用户的并发创建串行化 ----
void createStepLockUser(const CreateSpaceCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id FROM users WHERE id = $1::uuid FOR UPDATE",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.empty()) {
                createRollback(ctx, makeStatus(SpaceService::Outcome::Validation, "用户不存在"));
                return;
            }
            createStepCheckMembership(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "锁定用户失败", e); },
        ctx->userId);
}

// =====================================================================
// 接受邀请
// =====================================================================
struct AcceptInviteCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    std::string codeHash;
    std::string inviteId;
    std::string coupleId;
    int maxUses = 1;
    int usedCount = 0;
    SpaceService::SpaceCallback done;
};

using AcceptInviteCtxPtr = std::shared_ptr<AcceptInviteCtx>;

void acceptRollback(const AcceptInviteCtxPtr &ctx, const SpaceService::Status &status) {
    ctx->scope->rollback([ctx, status]() { ctx->done(status, SpaceInfo{}); });
}

// ---- 步骤 8：读回空间并提交 ----
void acceptStepLoadSpace(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kSpaceQuery,
        [ctx](const drogon::orm::Result &rows) {
            SpaceInfo info;
            assembleSpace(rows, info);
            if (info.spaceId.empty()) {
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::Error, "加入后读回空间为空"));
                return;
            }
            ctx->scope->commit([ctx, info]() { ctx->done(makeStatus(SpaceService::Outcome::Ok), info); });
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "读回空间失败", e); },
        ctx->userId);
}

// ---- 步骤 7：写审计 ----
void acceptStepAudit(const AcceptInviteCtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["invite_id"] = ctx->inviteId;
    detail["used_count"] = ctx->usedCount;
    ctx->scope->tx().execSqlAsync(
        kAuditInsert,
        [ctx](const drogon::orm::Result &) { acceptStepLoadSpace(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failWithDb(ctx->scope, "写接受邀请审计失败", e);
        },
        ctx->userId,
        ctx->coupleId,
        std::string("invite.accepted"),
        std::string("space_invite"),
        ctx->inviteId,
        toJsonText(detail));
}

// ---- 步骤 6：消费邀请（递增 used_count + 写 consumed_*） ----
void acceptStepConsumeInvite(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "UPDATE space_invites "
        "SET used_count = used_count + 1, "
        "    consumed_by_user_id = $1::uuid, "
        "    consumed_at = now() "
        "WHERE id = $2::uuid AND used_count < max_uses AND revoked_at IS NULL "
        "  AND consumed_at IS NULL",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.affectedRows() == 0) {
                // 与步骤 4 的校验结论不一致 => 说明并发改动，整体回滚。
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteInvalidOrExpired,
                                               "消费邀请时发现邀请已不可用"));
                return;
            }
            ctx->usedCount += 1;
            ctx->scope->tx().execSqlAsync(
                "SELECT public.usapp_room_initialize_positions($1::uuid, $2::uuid)",
                [ctx](const drogon::orm::Result &) { acceptStepAudit(ctx); },
                [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "初始化站位失败", e); },
                ctx->userId, ctx->inviteId);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failWithDb(ctx->scope, "消费邀请失败", e);
        },
        ctx->userId,
        ctx->inviteId);
}

// ---- 步骤 5：选中空 slot 并插入成员 ----
void acceptStepInsertMember(const AcceptInviteCtxPtr &ctx, int freeSlot) {
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO couple_members (couple_id, user_id, slot) VALUES ($1::uuid, $2::uuid, $3::integer)",
        [ctx](const drogon::orm::Result &) { acceptStepConsumeInvite(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "加入空间成员失败", e); },
        ctx->coupleId,
        ctx->userId,
        freeSlot);
}

// ---- 步骤 4b：读取已占用的 slot，选出第一个空位 ----
void acceptStepReadSlots(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT slot FROM couple_members WHERE couple_id = $1::uuid ORDER BY slot",
        [ctx](const drogon::orm::Result &rows) {
            // 容量由 slot 的取值域决定，不写死"两名用户"：找 1..2 里第一个没被占用的。
            bool occupied[3] = {false, false, false};
            for (const auto &row : rows) {
                const int slot = row["slot"].as<int>();
                if (slot >= 1 && slot <= 2) {
                    occupied[slot] = true;
                }
            }
            int freeSlot = 0;
            for (int slot = 1; slot <= 2; ++slot) {
                if (!occupied[slot]) {
                    freeSlot = slot;
                    break;
                }
            }
            if (freeSlot == 0) {
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::SpaceFull, "两个槽位都已占用"));
                return;
            }
            acceptStepInsertMember(ctx, freeSlot);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "读取槽位失败", e); },
        ctx->coupleId);
}

// ---- 步骤 4a：锁住 couples 行，让同一空间的并发加入串行化 ----
void acceptStepLockCouple(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id FROM couples WHERE id = $1::uuid FOR UPDATE",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.empty()) {
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteInvalidOrExpired,
                                               "邀请指向的空间已不存在"));
                return;
            }
            acceptStepReadSlots(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "锁定空间失败", e); },
        ctx->coupleId);
}

// ---- 步骤 3：确认接受者自己还没有空间 ----
void acceptStepCheckMembership(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT couple_id::text AS couple_id FROM couple_members WHERE user_id = $1::uuid",
        [ctx](const drogon::orm::Result &rows) {
            if (!rows.empty()) {
                const std::string existing = rows[0]["couple_id"].as<std::string>();
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::AlreadyInSpace,
                                               "该用户已在空间 " + existing));
                return;
            }
            acceptStepLockCouple(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failWithDb(ctx->scope, "查询成员关系失败", e);
        },
        ctx->userId);
}

// ---- 步骤 2：校验邀请行（已 FOR UPDATE 锁定） ----
void acceptStepValidateInvite(const AcceptInviteCtxPtr &ctx, const drogon::orm::Result &rows) {
    if (rows.empty()) {
        acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteInvalidOrExpired,
                                       "邀请码不存在"));
        return;
    }
    const auto &row = rows[0];
    if (row["revoked"].as<bool>()) {
        acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteRevoked, "邀请已被撤销"));
        return;
    }
    if (row["expired"].as<bool>()) {
        acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteInvalidOrExpired,
                                       "邀请已过期"));
        return;
    }
    ctx->inviteId = row["id"].as<std::string>();
    ctx->coupleId = row["couple_id"].as<std::string>();
    ctx->maxUses = row["max_uses"].as<int>();
    ctx->usedCount = row["used_count"].as<int>();
    if (ctx->usedCount >= ctx->maxUses) {
        acceptRollback(ctx, makeStatus(SpaceService::Outcome::InviteInvalidOrExpired,
                                       "邀请已用尽"));
        return;
    }
    acceptStepCheckMembership(ctx);
}

// ---- 步骤 1b：按散列锁住邀请行 ----
void acceptStepLockInvite(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id, couple_id::text AS couple_id, max_uses, used_count, "
        "       (revoked_at IS NOT NULL) AS revoked, "
        "       (expires_at <= now())    AS expired "
        "FROM space_invites WHERE code_hash = $1 FOR UPDATE",
        [ctx](const drogon::orm::Result &rows) { acceptStepValidateInvite(ctx, rows); },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "锁定邀请失败", e); },
        ctx->codeHash);
}

// ---- 步骤 1a：锁住接受者的 users 行 ----
void acceptStepLockUser(const AcceptInviteCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id FROM users WHERE id = $1::uuid FOR UPDATE",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.empty()) {
                acceptRollback(ctx, makeStatus(SpaceService::Outcome::Validation, "用户不存在"));
                return;
            }
            acceptStepLockInvite(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx->scope, "锁定用户失败", e); },
        ctx->userId);
}

}  // namespace

SpaceService &SpaceService::instance() {
    static SpaceService service;
    return service;
}

// =====================================================================
// 工具
// =====================================================================
std::string SpaceService::normalizeInviteCode(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '-' || c == ' ' || c == '\t') {
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    if (out.size() != kInviteCodeLength) {
        return "";
    }
    const std::string alphabet(kInviteAlphabet);
    for (const char ch : out) {
        if (alphabet.find(ch) == std::string::npos) {
            return "";
        }
    }
    return out;
}

std::string SpaceService::generateInviteCode() {
    const std::string body = crypto::randomFromAlphabet(kInviteAlphabet, kInviteCodeLength);
    std::string formatted;
    formatted.reserve(body.size() + body.size() / kInviteCodeGroup);
    for (size_t i = 0; i < body.size(); ++i) {
        if (i > 0 && i % kInviteCodeGroup == 0) {
            formatted.push_back('-');
        }
        formatted.push_back(body[i]);
    }
    return formatted;
}

const char *SpaceService::outcomeMessage(Outcome outcome) {
    switch (outcome) {
        case Outcome::Ok:
            return "ok";
        case Outcome::AlreadyInSpace:
            return "该账号已经属于某个空间";
        case Outcome::SpaceNotFound:
            return "尚未加入任何空间";
        case Outcome::SpaceFull:
            return "该空间成员已满";
        case Outcome::InviteInvalidOrExpired:
            return "邀请码无效、已过期或已使用";
        case Outcome::InviteRevoked:
            return "邀请已被撤销";
        case Outcome::Validation:
            return "请求参数不合法";
        case Outcome::Error:
            return "服务内部错误";
    }
    return "服务内部错误";
}

// =====================================================================
// 创建空间
// =====================================================================
void SpaceService::createSpaceForUser(const std::string &userId, SpaceCallback cb) {
    auto ctx = std::make_shared<CreateSpaceCtx>();
    ctx->userId = userId;
    ctx->done = std::move(cb);

    // 先做一次廉价的入参检查；真正的并发安全由事务内的行锁保证。
    if (userId.empty()) {
        Status status = makeStatus(Outcome::Validation, "缺少用户标识");
        ctx->done(status, SpaceInfo{});
        return;
    }

    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            createStepLockUser(ctx);
        },
        [ctx](const std::string &message) {
            ctx->done(makeStatus(Outcome::Error, message), SpaceInfo{});
        });
}

// =====================================================================
// 查询当前空间
// =====================================================================
void SpaceService::getCurrentSpace(const std::string &userId, SpaceCallback cb) {
    if (userId.empty()) {
        cb(makeStatus(Outcome::Validation, "缺少用户标识"), SpaceInfo{});
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            kSpaceQuery,
            [cb](const drogon::orm::Result &rows) {
                if (rows.empty()) {
                    cb(makeStatus(Outcome::SpaceNotFound, "该用户不在任何空间"), SpaceInfo{});
                    return;
                }
                SpaceInfo info;
                assembleSpace(rows, info);
                cb(makeStatus(Outcome::Ok), info);
            },
            [cb](const drogon::orm::DrogonDbException &e) {
                cb(makeStatus(Outcome::Error, db::describePgError("查询当前空间失败", e.base())),
                   SpaceInfo{});
            },
            userId);
    } catch (const std::exception &e) {
        cb(makeStatus(Outcome::Error, std::string("查询当前空间异常: ") + e.what()), SpaceInfo{});
    }
}

void SpaceService::getSpaceForMember(const std::string &userId,
                                     const std::string &spaceId,
                                     SpaceCallback cb) {
    if (userId.empty() || spaceId.empty()) {
        cb(makeStatus(Outcome::Validation, "缺少用户或空间标识"), SpaceInfo{});
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            kSpaceQueryById,
            [cb](const drogon::orm::Result &rows) {
                if (rows.empty()) {
                    // 不存在与无权限合并为同一结果，避免探测他人空间。
                    cb(makeStatus(Outcome::SpaceNotFound, "空间不存在或无权查看"), SpaceInfo{});
                    return;
                }
                SpaceInfo info;
                assembleSpace(rows, info);
                cb(makeStatus(Outcome::Ok), info);
            },
            [cb](const drogon::orm::DrogonDbException &e) {
                cb(makeStatus(Outcome::Error, db::describePgError("查询空间失败", e.base())),
                   SpaceInfo{});
            },
            userId,
            spaceId);
    } catch (const std::exception &e) {
        cb(makeStatus(Outcome::Error, std::string("查询空间异常: ") + e.what()), SpaceInfo{});
    }
}

// =====================================================================
// 创建邀请
//
// 用一条 INSERT ... SELECT 同时完成"确认是成员"与"插入邀请"，不给自己留下
// "先查后写"的竞态。返回 0 行有两种可能：调用者不是任何空间的成员，或者
// 邀请码散列撞车（概率约 2^-59）。两者的后续处理完全不同，必须用一次廉价
// 查询区分，否则会把"重试一次就能成功"的撞车误报成"你没有空间"。
//
// 用共享上下文 + 自由函数做重试，是为了避免把 std::function 递归地捕获进
// 自己（那会形成 shared_ptr 自引用环，回调对象永不释放）。
// =====================================================================
namespace {

struct InviteCreateCtx {
    std::string issuerSide;
    std::string userId;
    SpaceService::InviteCreatedCallback cb;
};

using InviteCreateCtxPtr = std::shared_ptr<InviteCreateCtx>;

void inviteCreateAttempt(const InviteCreateCtxPtr &ctx, int attemptsLeft);

void inviteCreateAfterEmpty(const InviteCreateCtxPtr &ctx, int attemptsLeft) {
    try {
        db::Pool::instance().client()->execSqlAsync(
            "SELECT couple_id::text AS couple_id FROM couple_members WHERE user_id = $1::uuid",
            [ctx, attemptsLeft](const drogon::orm::Result &memberRows) {
                if (memberRows.empty()) {
                    ctx->cb(makeStatus(SpaceService::Outcome::SpaceNotFound, "该用户不在任何空间"),
                            "", "", "");
                    return;
                }
                if (attemptsLeft <= 1) {
                    ctx->cb(makeStatus(SpaceService::Outcome::Error, "邀请码生成连续冲突"), "", "", "");
                    return;
                }
                inviteCreateAttempt(ctx, attemptsLeft - 1);
            },
            [ctx](const drogon::orm::DrogonDbException &e) {
                ctx->cb(makeStatus(SpaceService::Outcome::Error,
                                   db::describePgError("查询成员关系失败", e.base())),
                        "", "", "");
            },
            ctx->userId);
    } catch (const std::exception &e) {
        ctx->cb(makeStatus(SpaceService::Outcome::Error, std::string("查询成员异常: ") + e.what()),
                "", "", "");
    }
}

void inviteCreateAttempt(const InviteCreateCtxPtr &ctx, int attemptsLeft) {
    const std::string rawCode = SpaceService::generateInviteCode();
    const std::string normalized = SpaceService::normalizeInviteCode(rawCode);
    const std::string codeHash = crypto::sha256Hex(normalized);
    const std::string prefix = normalized.substr(0, 4);

    try {
        db::Pool::instance().client()->execSqlAsync(
            "INSERT INTO space_invites "
            "(couple_id, code_hash, code_prefix, created_by_user_id, max_uses, expires_at, room_issuer_side) "
            "SELECT m.couple_id, $2, $3, $1::uuid, $4::integer, now() + make_interval(secs => $5::integer), NULLIF($6, '') "
            "FROM couple_members AS m WHERE m.user_id = $1::uuid "
            "ON CONFLICT (code_hash) DO NOTHING "
            "RETURNING id::text AS id, couple_id::text AS couple_id, "
            "          to_char(expires_at AT TIME ZONE 'UTC', "
            "                  'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires_at",
            [ctx, rawCode, attemptsLeft](const drogon::orm::Result &rows) {
                if (rows.empty()) {
                    inviteCreateAfterEmpty(ctx, attemptsLeft);
                    return;
                }
                const std::string inviteId = rows[0]["id"].as<std::string>();
                const std::string coupleId = rows[0]["couple_id"].as<std::string>();
                const std::string expiresAt = rows[0]["expires_at"].as<std::string>();

                // 审计是尽力而为的：邀请已经落库，不能因为审计写失败而回滚。
                AuditLog::record("user",
                                 ctx->userId,
                                 coupleId,
                                 "invite.created",
                                 "space_invite",
                                 inviteId,
                                 Json::Value(Json::objectValue));
                ctx->cb(makeStatus(SpaceService::Outcome::Ok), rawCode, inviteId, expiresAt);
            },
            [ctx](const drogon::orm::DrogonDbException &e) {
                ctx->cb(makeStatus(SpaceService::Outcome::Error,
                                   db::describePgError("创建邀请失败", e.base())),
                        "", "", "");
            },
            ctx->userId,
            codeHash,
            prefix,
            SpaceService::kInviteMaxUses,
            SpaceService::kInviteTtlSeconds, ctx->issuerSide);
    } catch (const std::exception &e) {
        ctx->cb(makeStatus(SpaceService::Outcome::Error, std::string("创建邀请异常: ") + e.what()),
                "", "", "");
    }
}

}  // namespace

void SpaceService::createInvite(const std::string &userId, InviteCreatedCallback cb, const std::string &issuerSide) {
    if (userId.empty() || (!issuerSide.empty() && issuerSide != "left" && issuerSide != "right")) {
        cb(makeStatus(Outcome::Validation, "用户或站位无效"), "", "", "");
        return;
    }
    auto ctx = std::make_shared<InviteCreateCtx>();
    ctx->userId = userId;
    ctx->issuerSide = issuerSide;
    ctx->cb = std::move(cb);
    inviteCreateAttempt(ctx, kInviteMaxInsertAttempts);
}

void SpaceService::previewInvite(const std::string &userId, const std::string &normalizedCode,
                                 std::function<void(Status, std::string)> cb) {
    db::Pool::instance().client()->execSqlAsync(
        "SELECT CASE WHEN COALESCE(i.room_issuer_side, 'left') = 'right' THEN 'left' ELSE 'right' END AS side "
        "FROM space_invites i JOIN couple_members issuer ON (issuer.couple_id, issuer.user_id) = (i.couple_id, i.created_by_user_id) "
        "WHERE i.code_hash = $2 AND i.revoked_at IS NULL AND i.expires_at > now() AND i.used_count < i.max_uses "
        "AND NOT EXISTS (SELECT FROM couple_members WHERE user_id = $1::uuid OR (couple_id = i.couple_id AND user_id <> issuer.user_id))",
        [cb](const drogon::orm::Result &rows) {
            if (rows.empty()) { cb(makeStatus(Outcome::InviteInvalidOrExpired, "邀请不可用"), ""); return; }
            cb(makeStatus(Outcome::Ok), rows[0]["side"].as<std::string>());
        }, [cb](const drogon::orm::DrogonDbException &) { cb(makeStatus(Outcome::Error, "暂时无法读取邀请"), ""); },
        userId, crypto::sha256Hex(normalizedCode));
}

// =====================================================================
// 撤销邀请
// =====================================================================
void SpaceService::revokeInvite(const std::string &userId,
                                const std::string &inviteId,
                                StatusCallback cb) {
    if (userId.empty() || inviteId.empty()) {
        cb(makeStatus(Outcome::Validation, "缺少用户或邀请标识"));
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            "UPDATE space_invites AS i SET revoked_at = now() "
            "FROM couple_members AS m "
            "WHERE i.id = $2::uuid AND m.user_id = $1::uuid AND m.couple_id = i.couple_id "
            "  AND i.revoked_at IS NULL AND i.consumed_at IS NULL "
            "RETURNING i.id::text AS id, i.couple_id::text AS couple_id",
            [userId, cb](const drogon::orm::Result &rows) {
                if (rows.empty()) {
                    // 不是本空间成员 / 邀请不存在 / 已撤销 / 已使用，对外一律 NotFound。
                    cb(makeStatus(Outcome::SpaceNotFound, "邀请不存在或不可撤销"));
                    return;
                }
                AuditLog::record("user",
                                 userId,
                                 rows[0]["couple_id"].as<std::string>(),
                                 "invite.revoked",
                                 "space_invite",
                                 rows[0]["id"].as<std::string>(),
                                 Json::Value(Json::objectValue));
                cb(makeStatus(Outcome::Ok));
            },
            [cb](const drogon::orm::DrogonDbException &e) {
                cb(makeStatus(Outcome::Error, db::describePgError("撤销邀请失败", e.base())));
            },
            userId,
            inviteId);
    } catch (const std::exception &e) {
        cb(makeStatus(Outcome::Error, std::string("撤销邀请异常: ") + e.what()));
    }
}

// =====================================================================
// 接受邀请
// =====================================================================
void SpaceService::acceptInvite(const std::string &userId,
                                const std::string &rawCode,
                                SpaceCallback cb) {
    const std::string normalized = normalizeInviteCode(rawCode);
    if (userId.empty()) {
        cb(makeStatus(Outcome::Validation, "缺少用户标识"), SpaceInfo{});
        return;
    }
    if (normalized.empty()) {
        // 形状就不对：不要拿它去查库，也给出与"不存在"完全一致的对外结果。
        cb(makeStatus(Outcome::InviteInvalidOrExpired, "邀请码形状不合法"), SpaceInfo{});
        return;
    }

    auto ctx = std::make_shared<AcceptInviteCtx>();
    ctx->userId = userId;
    ctx->codeHash = crypto::sha256Hex(normalized);
    ctx->done = std::move(cb);

    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            acceptStepLockUser(ctx);
        },
        [ctx](const std::string &message) {
            ctx->done(makeStatus(Outcome::Error, message), SpaceInfo{});
        });
}

// =====================================================================
// 邀请列表
// =====================================================================
void SpaceService::listInvites(const std::string &userId, InviteListCallback cb) {
    if (userId.empty()) {
        cb(makeStatus(Outcome::Validation, "缺少用户标识"), {});
        return;
    }
    // 两步而不是一条带子查询的 SQL：一条 SQL 里"用户没有空间"和"空间还没有
    // 任何邀请"都会返回 0 行，无法区分，会把后者误报成 404。
    db::Pool::instance().client()->execSqlAsync(
        "SELECT couple_id::text AS couple_id FROM couple_members WHERE user_id = $1::uuid",
        [cb](const drogon::orm::Result &r) {
            if (r.empty()) {
                cb(makeStatus(Outcome::SpaceNotFound, "用户不属于任何空间"), {});
                return;
            }
            const std::string coupleId = r[0]["couple_id"].as<std::string>();
            db::Pool::instance().client()->execSqlAsync(
                kInviteListQuery,
                [cb](const drogon::orm::Result &rows) {
                    std::vector<InviteInfo> invites;
                    invites.reserve(static_cast<size_t>(rows.size()));
                    for (const auto &row : rows) {
                        InviteInfo info;
                        info.id = row["id"].as<std::string>();
                        info.maxUses = row["max_uses"].as<int>();
                        info.usedCount = row["used_count"].as<int>();
                        info.revoked = row["revoked"].as<bool>();
                        info.expiresAt = row["expires_at"].as<std::string>();
                        info.createdAt = row["created_at"].as<std::string>();
                        invites.push_back(std::move(info));
                    }
                    cb(makeStatus(Outcome::Ok, ""), std::move(invites));
                },
                [cb](const drogon::orm::DrogonDbException &e) {
                    cb(makeStatus(Outcome::Error, db::describePgError("查询邀请列表失败", e.base())), {});
                },
                coupleId);
        },
        [cb](const drogon::orm::DrogonDbException &e) {
            cb(makeStatus(Outcome::Error, db::describePgError("查询成员关系失败", e.base())), {});
        },
        userId);
}

}  // namespace usapp
