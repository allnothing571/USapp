// =====================================================================
// services/api/src/services/PushService.cc
// =====================================================================
#include "services/PushService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define USAPP_NOT_REVOKED(g) \
    "NOT EXISTS (SELECT 1 FROM push_subscription_revocations rr WHERE rr.subscription_id = " g ".id)"
#define USAPP_SESSION_VALID(s) \
    "(" s ".is_authenticated AND " s ".revoked_at IS NULL AND " s ".expires_at > clock_timestamp())"

// Lock order position 1: the caller's session row FOR SHARE (it must still be the caller's
// valid formal session), then the caller's member row FOR SHARE.
const char *kSessionSql =
    "SELECT s.id::text AS id FROM sessions s WHERE s.id = $1::uuid AND s.user_id = $2::uuid AND "
    USAPP_SESSION_VALID("s") " FOR SHARE OF s";
const char *kMembershipSql =
    "SELECT m.couple_id::text AS couple_id FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";

// Lock order positions 2 and 3; the same keys as the 0022 guards (re-entrant there).
const char *kUserLockSql = "SELECT pg_advisory_xact_lock(33013::integer, hashtext($1::uuid::text))";
const char *kEndpointLockSql = "SELECT pg_advisory_xact_lock(33014::integer, hashtext($1::text))";

const char *kCommandByKeySql =
    "SELECT request_fingerprint, generation_id::text AS generation_id FROM push_registration_commands "
    "WHERE user_id = $1::uuid AND idempotency_key = $2";

// The unrevoked generation on an endpoint (at most one, by the 0022 guard), any user.
const char *kOccupantSql =
    "SELECT g.id::text AS id, g.user_id::text AS user_id, g.session_id::text AS session_id, g.p256dh, g.auth, "
    "g.vapid_key_id FROM push_subscriptions g WHERE g.endpoint_hash = $1 AND " USAPP_NOT_REVOKED("g");

// The cap count: the same conservative rule as the 0022 guard (unrevoked generations of the
// user's valid sessions, any key id), excluding the generation this command replaces.
const char *kCapCountSql =
    "SELECT count(*)::integer AS n FROM push_subscriptions g JOIN sessions s ON s.id = g.session_id "
    "WHERE g.user_id = $1::uuid AND " USAPP_NOT_REVOKED("g") " AND " USAPP_SESSION_VALID("s")
    " AND g.id IS DISTINCT FROM NULLIF($2, '')::uuid";

// A successor revocation (superseded / replaced / reowned) or a user revocation. A concurrent
// revocation of the same generation (the owner's explicit revoke does not take the endpoint
// lock) makes this a no-op; the target is revoked either way.
const char *kRevocationInsertSql =
    "INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason, caused_by) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, NULLIF($6, '')::uuid) "
    "ON CONFLICT DO NOTHING RETURNING subscription_id::text AS id";

const char *kGenerationInsertSql =
    "INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, "
    "vapid_key_id) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text, $7::text, $8::text, $9::text)";

const char *kCommandInsertSql =
    "INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, "
    "result, generation_id) VALUES ($1::uuid, $2::text, $3::uuid, $4::uuid, $5::text, $6::text, $7::uuid) "
    "ON CONFLICT DO NOTHING RETURNING user_id::text AS id";

// Identifiers and fixed labels only: never the endpoint or keys.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'push_subscription', $4::text, $5::jsonb)";

const char *kCommandFactsSql =
    "SELECT json_build_object('result', c.result, 'generation_id', c.generation_id, "
    "'recorded_at', " USAPP_TS("c.recorded_at") ")::text AS facts_json "
    "FROM push_registration_commands c WHERE c.user_id = $1::uuid AND c.idempotency_key = $2";

const char *kRevocationFactsSql =
    "SELECT json_build_object('generation_id', r.subscription_id, 'reason', r.reason, 'revoked_by', r.revoked_by, "
    "'revoked_at', " USAPP_TS("r.revoked_at") ")::text AS facts_json "
    "FROM push_subscription_revocations r WHERE r.subscription_id = $1::uuid";

// Current state columns; the provider is derived in SQL so the endpoint never leaves the
// database. $1 is the caller's session id.
#define USAPP_GENERATION_COLUMNS                                                                              \
    "g.id::text AS id, " USAPP_TS("g.activated_at") " AS activated_at, g.endpoint_hash, g.vapid_key_id, "     \
    "CASE WHEN g.endpoint LIKE 'https://fcm.googleapis.com/%' THEN 'fcm' "                                    \
    "WHEN g.endpoint LIKE 'https://updates.push.services.mozilla.com/%' THEN 'mozilla' "                      \
    "WHEN split_part(g.endpoint, '/', 3) LIKE '%.push.apple.com' THEN 'apple' ELSE 'windows' END AS provider, " \
    "r.reason AS revoked_reason, " USAPP_TS("r.revoked_at") " AS revoked_at, "                                \
    "(g.session_id = $1::uuid) AS current_session, s.user_agent_family, " USAPP_SESSION_VALID("s")             \
    " AS session_valid"
#define USAPP_GENERATION_FROM \
    "push_subscriptions g JOIN sessions s ON s.id = g.session_id " \
    "LEFT JOIN push_subscription_revocations r ON r.subscription_id = g.id"

const char *kGenerationByIdSql =
    "SELECT " USAPP_GENERATION_COLUMNS " FROM " USAPP_GENERATION_FROM " WHERE g.id = $2::uuid";

// Revoke target: only the caller's own generation (another user's or unknown -> not found).
const char *kRevokeTargetSql =
    "SELECT g.id::text AS id FROM push_subscriptions g WHERE g.id = $1::uuid AND g.user_id = $2::uuid";

// List contract: bucket 'device' is COMPLETE: every generation that occupies a cap slot
// (unrevoked, session valid; any key id), which the 0022 guard bounds at 10. It is fetched
// with LIMIT 11 only so that a broken invariant fails loudly instead of being truncated.
// Bucket 'history' is the newest $3 of the rest (revoked or session ended), bounded and
// never able to displace a device. $1 session, $2 user, $3 history limit, $4 device fetch.
#define USAPP_COUNTED "r.subscription_id IS NULL AND " USAPP_SESSION_VALID("s")
const char *kListSql =
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m LEFT JOIN LATERAL ("
    "(SELECT 'device'::text AS bucket, " USAPP_GENERATION_COLUMNS ", g.activated_at AS sort_at, g.id AS sort_id "
    "FROM " USAPP_GENERATION_FROM " WHERE g.user_id = m.user_id AND " USAPP_COUNTED
    " ORDER BY g.activated_at DESC, g.id DESC LIMIT $4::integer) UNION ALL "
    "(SELECT 'history'::text AS bucket, " USAPP_GENERATION_COLUMNS ", g.activated_at AS sort_at, g.id AS sort_id "
    "FROM " USAPP_GENERATION_FROM " WHERE g.user_id = m.user_id AND NOT (" USAPP_COUNTED ")"
    " ORDER BY g.activated_at DESC, g.id DESC LIMIT $3::integer)) x ON true "
    "WHERE m.user_id = $2::uuid ORDER BY x.bucket, x.sort_at DESC, x.sort_id DESC";
#undef USAPP_COUNTED

#undef USAPP_GENERATION_FROM
#undef USAPP_GENERATION_COLUMNS
#undef USAPP_SESSION_VALID
#undef USAPP_NOT_REVOKED
#undef USAPP_TS

std::string newUuid() {
    std::string hex = crypto::randomHex(16);
    hex[12] = '4';
    const char variant[] = {'8', '9', 'a', 'b'};
    hex[16] = variant[static_cast<unsigned char>(hex[16]) % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20, 12);
}

bool parseObject(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isObject();
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64UrlEncode(const unsigned char *data, size_t len) {
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kBase64Url[(buffer >> bits) & 0x3Fu]);
        }
    }
    if (bits > 0) out.push_back(kBase64Url[(buffer << (6 - bits)) & 0x3Fu]);
    return out;
}

bool validLabel(const std::string &label) {
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
    for (char c : label) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

// Every label of a lowercase DNS name; no empty label (so no trailing dot), no IP literal
// (an all-numeric name cannot end in an allowlisted suffix anyway).
bool validHostName(const std::string &host) {
    size_t start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        if (!validLabel(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start))) return false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

bool endsWith(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool allowedHost(const std::string &host) {
    if (!validHostName(host)) return false;
    if (host == "fcm.googleapis.com" || host == "updates.push.services.mozilla.com") return true;
    // Label boundary: at least one label before the suffix, so "xpush.apple.com" fails.
    for (const char *suffix : {".push.apple.com", ".notify.windows.com"}) {
        const std::string s(suffix);
        if (host.size() > s.size() && endsWith(host, s)) return true;
    }
    return false;
}

std::string lengthPrefixed(const char *name, const std::string &value) {
    return std::string(name) + "=" + std::to_string(value.size()) + ":" + value + ";";
}

PushService::Status makeStatus(PushService::Outcome outcome, std::string message = "", std::string detail = "") {
    PushService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

std::string optText(const drogon::orm::Row &row, const char *column) {
    return row[column].isNull() ? std::string() : row[column].as<std::string>();
}

// Current state of one generation. Revoked wins; otherwise active needs a valid session and
// the configured key id (a different or absent key id is inactive).
PushService::Generation generationFromRow(const drogon::orm::Row &row, const std::string &vapidKeyId) {
    PushService::Generation g;
    g.id = row["id"].as<std::string>();
    g.activatedAt = row["activated_at"].as<std::string>();
    g.endpointHash = row["endpoint_hash"].as<std::string>();
    g.vapidKeyId = row["vapid_key_id"].as<std::string>();
    g.provider = row["provider"].as<std::string>();
    g.revokedReason = optText(row, "revoked_reason");
    g.revokedAt = optText(row, "revoked_at");
    g.currentSession = row["current_session"].as<bool>();
    g.userAgentFamily = optText(row, "user_agent_family");
    const bool sessionValid = row["session_valid"].as<bool>();
    if (!g.revokedReason.empty()) g.state = "revoked";
    else if (sessionValid && !vapidKeyId.empty() && g.vapidKeyId == vapidKeyId) g.state = "active";
    else g.state = "inactive";
    return g;
}

// ---------------------------------------------------------------------
// Lifetime: Ctx --(shared)--> TxScope --(onError_, weak)--> Ctx, as in the other
// services. Every end goes through complete() exactly once; commitRequested is set
// before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    bool revoking = false;
    std::string userId;
    std::string sessionId;
    std::string coupleId;
    std::string key;
    PushService::Registration registration;
    std::string endpointHash;
    std::string vapidKeyId;
    std::string markerSecret;
    std::string fingerprint;
    std::string generationId;  // new generation (register) or target (revoke)
    std::string replacedId;    // register: the revoked predecessor, if any
    std::string replacedOwner;
    std::string replacedReason;
    std::string result;        // register: created | already_active
    PushService::Status status;
    PushService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<Ctx>;

std::string label(bool revoking, const char *stage, const char *reason) {
    return TaskService::failureDetail(revoking ? "push_revoke" : "push_register", stage, reason);
}

void complete(const CtxPtr &ctx, PushService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "PushService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->revoking, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, PushService::Status status) {
    auto shared = std::make_shared<PushService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

// One statement in the transaction: a database error or a column-format exception fails the
// transaction with a fixed stage label. Parameters are text (Drogon binds strings).
template <typename... Args>
void exec(const CtxPtr &ctx, const char *sql, const char *stage,
          std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    ctx->scope->tx().execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                failAt(ctx, stage, "column_format");
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) { failAt(ctx, stage, "db_error"); },
        std::forward<Args>(args)...);
}

// Immutable facts (command or revocation) plus the generation's current state, inside the
// transaction; the marker only accompanies an active generation of this session's registration.
void readGeneration(const CtxPtr &ctx, std::shared_ptr<PushService::Status> status, const std::string &generationId,
                    std::function<void(PushService::Status)> next) {
    exec(ctx, kGenerationByIdSql, "read_generation",
         [ctx, status, next = std::move(next)](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "read_generation", "row_count"); return; }
             status->generation = generationFromRow(rows[0], ctx->vapidKeyId);
             if (!ctx->revoking && status->generation.state == "active" && status->generation.currentSession) {
                 status->marker = PushService::markerFor(ctx->markerSecret, status->generation.id);
             }
             next(std::move(*status));
         },
         ctx->sessionId, generationId);
}

void readFacts(const CtxPtr &ctx, PushService::Outcome outcome, std::function<void(PushService::Status)> next) {
    auto onFacts = [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
        if (rows.size() != 1u) { failAt(ctx, "read_facts", "row_count"); return; }
        auto status = std::make_shared<PushService::Status>(makeStatus(outcome));
        status->isRevoke = ctx->revoking;
        Json::Value facts;
        if (!parseObject(rows[0]["facts_json"].as<std::string>(), facts) || !facts["generation_id"].isString()) {
            failAt(ctx, "read_facts", "json_format");
            return;
        }
        if (ctx->revoking) status->revocation = facts;
        else status->command = facts;
        readGeneration(ctx, status, facts["generation_id"].asString(), next);
    };
    if (ctx->revoking) exec(ctx, kRevocationFactsSql, "read_facts", std::move(onFacts), ctx->generationId);
    else exec(ctx, kCommandFactsSql, "read_facts", std::move(onFacts), ctx->userId, ctx->key);
}

void auditAndCommit(const CtxPtr &ctx, PushService::Outcome outcome) {
    Json::Value detail(Json::objectValue);
    detail["generation_id"] = ctx->generationId;
    if (!ctx->revoking) {
        detail["result"] = ctx->result;
        if (!ctx->replacedId.empty()) {
            detail["revoked_generation_id"] = ctx->replacedId;
            detail["revoked_reason"] = ctx->replacedReason;
        }
    }
    exec(ctx, kAuditSql, "audit",
         [ctx, outcome](const drogon::orm::Result &) {
             readFacts(ctx, outcome, [ctx](PushService::Status status) {
                 ctx->status = std::move(status);
                 ctx->commitRequested = true;
                 ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->status)); });
             });
         },
         ctx->userId, ctx->coupleId,
         std::string(ctx->revoking ? "push.subscription_revoked" : "push.subscription_registered"), ctx->generationId,
         compactJson(detail));
}

// Lock order position 1 (session row, then member row FOR SHARE), then 2 (user lock).
void membership(const CtxPtr &ctx, std::function<void()> next) {
    exec(ctx, kSessionSql, "session",
         [ctx, next = std::move(next)](const drogon::orm::Result &sessions) {
             if (sessions.size() != 1u) {
                 rejectWith(ctx, makeStatus(PushService::Outcome::SessionInvalid, "请先登录"));
                 return;
             }
             exec(ctx, kMembershipSql, "membership",
                  [ctx, next](const drogon::orm::Result &rows) {
                      if (rows.size() == 0u) {
                          rejectWith(ctx, makeStatus(PushService::Outcome::SpaceNotFound, "尚未加入空间"));
                          return;
                      }
                      if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
                      ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                      exec(ctx, kUserLockSql, "user_lock", [next](const drogon::orm::Result &) { next(); },
                           ctx->userId);
                  },
                  ctx->userId);
         },
         ctx->sessionId, ctx->userId);
}

// ---- register ----
void insertCommand(const CtxPtr &ctx) {
    exec(ctx, kCommandInsertSql, "insert_command",
         [ctx](const drogon::orm::Result &rows) {
             // The user lock serializes same-key requests before classification, so a skipped
             // insert is unexplained.
             if (rows.size() != 1u) { failAt(ctx, "insert_command", "conflict_unexplained"); return; }
             auditAndCommit(ctx, ctx->result == "created" ? PushService::Outcome::Created : PushService::Outcome::Ok);
         },
         ctx->userId, ctx->key, ctx->coupleId, ctx->sessionId, ctx->fingerprint, ctx->result, ctx->generationId);
}

void insertGeneration(const CtxPtr &ctx) {
    ctx->generationId = newUuid();
    ctx->result = "created";
    auto insert = [ctx]() {
        exec(ctx, kGenerationInsertSql, "insert_generation",
             [ctx](const drogon::orm::Result &) { insertCommand(ctx); }, ctx->generationId, ctx->userId,
             ctx->coupleId, ctx->sessionId, ctx->registration.endpoint, ctx->endpointHash, ctx->registration.p256dh,
             ctx->registration.auth, ctx->vapidKeyId);
    };
    if (ctx->replacedId.empty()) {
        insert();
        return;
    }
    // The predecessor's revocation first; its successor link is checked at commit.
    exec(ctx, kRevocationInsertSql, "revoke_previous", [insert](const drogon::orm::Result &) { insert(); },
         ctx->replacedId, ctx->replacedOwner, ctx->coupleId, ctx->userId, ctx->replacedReason, ctx->generationId);
}

// Cap: a new device for this user (including a re-owned endpoint) needs a free slot; a
// replacement of the user's own generation on this endpoint does not grow the count.
void checkCap(const CtxPtr &ctx) {
    const std::string excluded = ctx->replacedOwner == ctx->userId ? ctx->replacedId : std::string();
    exec(ctx, kCapCountSql, "cap",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "cap", "row_count"); return; }
             if (rows[0]["n"].as<int>() >= PushService::kMaxActiveDevices) {
                 rejectWith(ctx, makeStatus(PushService::Outcome::DeviceLimit,
                                            "最多可在 10 台设备上开启提醒，请先关闭一台设备"));
                 return;
             }
             insertGeneration(ctx);
         },
         ctx->userId, excluded);
}

void occupant(const CtxPtr &ctx) {
    exec(ctx, kOccupantSql, "occupant",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() > 1u) { failAt(ctx, "occupant", "endpoint_invariant"); return; }
             if (rows.size() == 0u) { checkCap(ctx); return; }
             const auto &row = rows[0];
             const std::string id = row["id"].as<std::string>();
             const std::string owner = row["user_id"].as<std::string>();
             const bool sameKeys = row["p256dh"].as<std::string>() == ctx->registration.p256dh &&
                                   row["auth"].as<std::string>() == ctx->registration.auth &&
                                   row["vapid_key_id"].as<std::string>() == ctx->vapidKeyId;
             const bool sameSession = row["session_id"].as<std::string>() == ctx->sessionId;
             if (owner == ctx->userId && sameSession && sameKeys) {
                 // The caller's session is valid and the key id is current: already active.
                 ctx->result = "already_active";
                 ctx->generationId = id;
                 insertCommand(ctx);
                 return;
             }
             ctx->replacedId = id;
             ctx->replacedOwner = owner;
             ctx->replacedReason = owner != ctx->userId ? "reowned" : (sameKeys ? "superseded" : "replaced");
             checkCap(ctx);
         },
         ctx->endpointHash);
}

// Key classification before every state check (under the user lock).
void classify(const CtxPtr &ctx) {
    exec(ctx, kCommandByKeySql, "classify",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) {
                 // Lock order position 3: the endpoint lock.
                 exec(ctx, kEndpointLockSql, "endpoint_lock", [ctx](const drogon::orm::Result &) { occupant(ctx); },
                      ctx->endpointHash);
                 return;
             }
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 rejectWith(ctx, makeStatus(PushService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                 return;
             }
             // Replay: the original command plus the generation's current state; read-only.
             readFacts(ctx, PushService::Outcome::Replayed,
                       [ctx](PushService::Status status) { rejectWith(ctx, std::move(status)); });
         },
         ctx->userId, ctx->key);
}

// ---- revoke ----
void revokeTarget(const CtxPtr &ctx) {
    exec(ctx, kRevokeTargetSql, "target",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) {
                 rejectWith(ctx, makeStatus(PushService::Outcome::NotFound, "设备不存在"));
                 return;
             }
             ctx->generationId = rows[0]["id"].as<std::string>();  // canonical form
             exec(ctx, kRevocationInsertSql, "insert_revocation",
                  [ctx](const drogon::orm::Result &inserted) {
                      if (inserted.size() == 1u) {
                          auditAndCommit(ctx, PushService::Outcome::Ok);
                          return;
                      }
                      // Already revoked (an earlier revoke, or a successor): the original revocation.
                      readFacts(ctx, PushService::Outcome::Replayed,
                                [ctx](PushService::Status status) { rejectWith(ctx, std::move(status)); });
                  },
                  ctx->generationId, ctx->userId, ctx->coupleId, ctx->userId, std::string("user"), std::string());
         },
         ctx->generationId, ctx->userId);
}

void run(const CtxPtr &ctx) {
    std::weak_ptr<Ctx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            if (ctx->revoking) membership(ctx, [ctx]() { revokeTarget(ctx); });
            else membership(ctx, [ctx]() { classify(ctx); });
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(PushService::Outcome::ResultUnknown, "",
                                                label(strong->revoking, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(PushService::Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(PushService::Outcome::Error, "",
                                                label(strong->revoking, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "PushService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

bool isLowerHex64(const std::string &value) {
    if (value.size() != 64u) return false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

PushService &PushService::instance() {
    static PushService service;
    return service;
}

PushService::Config PushService::configFrom(bool subscribeEnabled, const std::string &vapidPublicKey,
                                            bool transportReady) {
    Config config;
    if (validPublicKey(vapidPublicKey)) {
        config.vapidPublicKey = vapidPublicKey;
        config.vapidKeyId = vapidKeyId(vapidPublicKey);
    }
    config.subscribeAvailable = subscribeEnabled && transportReady && !config.vapidKeyId.empty();
    return config;
}

std::optional<std::string> PushService::base64UrlDecode(const std::string &text) {
    if (text.size() % 4u == 1u) return std::nullopt;
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        const char *pos = c == '\0' ? nullptr : std::strchr(kBase64Url, c);
        if (pos == nullptr) return std::nullopt;
        buffer = (buffer << 6) | static_cast<uint32_t>(pos - kBase64Url);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFFu));
        }
    }
    // Canonical form only: the unused trailing bits must be zero.
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0u) return std::nullopt;
    return out;
}

bool PushService::validPublicKey(const std::string &key) {
    if (key.size() != 87u) return false;
    const auto raw = base64UrlDecode(key);
    if (!raw || raw->size() != 65u || static_cast<unsigned char>((*raw)[0]) != 0x04u) return false;
    // A point of the right shape can still be off the curve: OpenSSL decides membership.
    const std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1),
                                                                    EC_GROUP_free);
    if (!group) return false;
    const std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> point(EC_POINT_new(group.get()), EC_POINT_free);
    return point &&
           EC_POINT_oct2point(group.get(), point.get(), reinterpret_cast<const unsigned char *>(raw->data()), raw->size(),
                              nullptr) == 1 &&
           EC_POINT_is_on_curve(group.get(), point.get(), nullptr) == 1 &&
           EC_POINT_is_at_infinity(group.get(), point.get()) == 0;
}

bool PushService::validEndpoint(const std::string &endpoint) {
    static const std::string kScheme = "https://";
    if (endpoint.size() > kMaxEndpointBytes || endpoint.compare(0, kScheme.size(), kScheme) != 0) return false;
    const size_t slash = endpoint.find('/', kScheme.size());
    if (slash == std::string::npos) return false;
    // A port, userinfo or uppercase letter makes the host fail (labels are [a-z0-9-] only).
    if (!allowedHost(endpoint.substr(kScheme.size(), slash - kScheme.size()))) return false;
    for (size_t i = slash; i < endpoint.size(); ++i) {
        const char c = endpoint[i];
        if (c < '!' || c > '~' || c == '#' || c == '\\') return false;
    }
    return true;
}

bool PushService::parseRegistration(const Json::Value &body, Registration &out, std::string &message) {
    if (!body.isObject() || body.size() != 2u || !body["endpoint"].isString() || !body["keys"].isObject()) {
        message = "请求体只能包含 endpoint 与 keys";
        return false;
    }
    const Json::Value &keys = body["keys"];
    if (keys.size() != 2u || !keys["p256dh"].isString() || !keys["auth"].isString()) {
        message = "keys 只能包含字符串字段 p256dh 与 auth";
        return false;
    }
    Registration parsed{body["endpoint"].asString(), keys["p256dh"].asString(), keys["auth"].asString()};
    if (!validEndpoint(parsed.endpoint)) {
        message = "不支持的推送服务地址";
        return false;
    }
    if (!validPublicKey(parsed.p256dh)) {
        message = "p256dh 不合法";
        return false;
    }
    const auto auth = parsed.auth.size() == 22u ? base64UrlDecode(parsed.auth) : std::nullopt;
    if (!auth || auth->size() != 16u) {
        message = "auth 不合法";
        return false;
    }
    out = std::move(parsed);
    message.clear();
    return true;
}

std::string PushService::endpointHash(const std::string &endpoint) {
    return crypto::sha256Hex(endpoint);
}

std::string PushService::vapidKeyId(const std::string &publicKey) {
    return crypto::sha256Hex(publicKey);
}

std::string PushService::fingerprint(const std::string &endpointHash, const Registration &registration,
                                     const std::string &vapidKeyId, const std::string &sessionId) {
    return crypto::sha256Hex("push-register:v1:" + lengthPrefixed("endpoint_hash", endpointHash) +
                             lengthPrefixed("p256dh", registration.p256dh) + lengthPrefixed("auth", registration.auth) +
                             lengthPrefixed("vapid_key_id", vapidKeyId) + lengthPrefixed("session_id", sessionId));
}

std::string PushService::markerFor(const std::string &secret, const std::string &generationId) {
    const std::string message = "push-marker:v1:" + generationId;
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char *>(message.data()), message.size(), mac, &macLen) == nullptr ||
        macLen < 16u) {
        throw std::runtime_error("marker derivation failed");
    }
    return base64UrlEncode(mac, 16u);
}

Json::Value PushService::generationJson(const Generation &g, bool includeHash) {
    Json::Value out(Json::objectValue);
    out["id"] = g.id;
    out["state"] = g.state;
    out["activated_at"] = g.activatedAt;
    out["revoked_reason"] = g.revokedReason.empty() ? Json::Value(Json::nullValue) : Json::Value(g.revokedReason);
    out["revoked_at"] = g.revokedAt.empty() ? Json::Value(Json::nullValue) : Json::Value(g.revokedAt);
    out["current_session"] = g.currentSession;
    out["provider"] = g.provider;
    out["user_agent_family"] =
        g.userAgentFamily.empty() ? Json::Value(Json::nullValue) : Json::Value(g.userAgentFamily);
    if (includeHash) {
        out["endpoint_hash"] = g.endpointHash;
        out["vapid_key_id"] = g.vapidKeyId;
    }
    return out;
}

Json::Value PushService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    if (status.isRevoke) {
        body["revocation"] = status.revocation;
    } else {
        body["command"] = status.command;
        body["marker"] = status.marker.empty() ? Json::Value(Json::nullValue) : Json::Value(status.marker);
    }
    body["generation"] = generationJson(status.generation, true);
    return body;
}

ErrorCode PushService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::SessionInvalid: return ErrorCode::Unauthenticated;
        case Outcome::Unavailable: return ErrorCode::PushUnavailable;
        case Outcome::DeviceLimit: return ErrorCode::PushDeviceLimit;
        case Outcome::NotFound: return ErrorCode::PushSubscriptionNotFound;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::Created:
        case Outcome::Replayed:
        case Outcome::Ok:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int PushService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Created) return 201;
    if (outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void PushService::registerSubscription(const std::string &userId, const std::string &sessionId,
                                       const std::string &key, const Registration &registration,
                                       const std::string &vapidKeyId, const std::string &markerSecret, Callback cb) {
    if (!TaskService::isCanonicalUuid(userId) || !TaskService::isCanonicalUuid(sessionId) ||
        !TaskService::validIdempotencyKey(key) || !validEndpoint(registration.endpoint) ||
        !validPublicKey(registration.p256dh) || registration.auth.size() != 22u ||
        !base64UrlDecode(registration.auth) || !isLowerHex64(vapidKeyId) || markerSecret.empty()) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(false, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(false, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->userId = userId;
    ctx->sessionId = sessionId;
    ctx->key = key;
    ctx->registration = registration;
    ctx->endpointHash = endpointHash(registration.endpoint);
    ctx->vapidKeyId = vapidKeyId;
    ctx->markerSecret = markerSecret;
    ctx->fingerprint = fingerprint(ctx->endpointHash, registration, vapidKeyId, sessionId);
    ctx->done = std::move(cb);
    run(ctx);
}

void PushService::revoke(const std::string &userId, const std::string &sessionId, const std::string &generationId,
                         const std::string &vapidKeyId, Callback cb) {
    if (!TaskService::isCanonicalUuid(userId) || !TaskService::isCanonicalUuid(sessionId) || generationId.empty()) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(true, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(true, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->revoking = true;
    ctx->userId = userId;
    ctx->sessionId = sessionId;
    ctx->generationId = generationId;
    ctx->vapidKeyId = vapidKeyId;
    ctx->done = std::move(cb);
    run(ctx);
}

void PushService::list(const std::string &userId, const std::string &sessionId, const std::string &vapidKeyId,
                       ListCallback cb) {
    ListResult early;
    if (!TaskService::isCanonicalUuid(userId) || !TaskService::isCanonicalUuid(sessionId)) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = TaskService::failureDetail("push_list", "pool", "not_ready");
        cb(std::move(early));
        return;
    }
    auto done = std::make_shared<ListCallback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kListSql,
        [done, vapidKeyId](const drogon::orm::Result &rows) {
            ListResult result;
            if (rows.size() == 0u) {
                result.outcome = Outcome::SpaceNotFound;
                result.message = "尚未加入空间";
                (*done)(std::move(result));
                return;
            }
            try {
                for (const auto &row : rows) {
                    if (row["id"].isNull()) continue;  // member without generations
                    auto &bucket = row["bucket"].as<std::string>() == "device" ? result.devices : result.history;
                    bucket.push_back(generationFromRow(row, vapidKeyId));
                }
            } catch (const std::exception &) {
                result = ListResult{};
                result.outcome = Outcome::Error;
                result.detail = TaskService::failureDetail("push_list", "query", "column_format");
                (*done)(std::move(result));
                return;
            }
            if (result.devices.size() > static_cast<size_t>(kMaxActiveDevices)) {
                // The 0022 guard makes this impossible; never return a silently truncated list.
                result = ListResult{};
                result.outcome = Outcome::Error;
                result.detail = TaskService::failureDetail("push_list", "query", "cap_invariant");
                (*done)(std::move(result));
                return;
            }
            result.outcome = Outcome::Ok;
            (*done)(std::move(result));
        },
        [done](const drogon::orm::DrogonDbException &) {
            ListResult result;
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("push_list", "query", "db_error");
            (*done)(std::move(result));
        },
        sessionId, userId, std::to_string(kHistoryLimit), std::to_string(kMaxActiveDevices + 1));
}

}  // namespace usapp
