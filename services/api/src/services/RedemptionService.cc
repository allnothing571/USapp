// =====================================================================
// services/api/src/services/RedemptionService.cc
// =====================================================================
#include "services/RedemptionService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/NotificationWriter.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// Current redemption: immutable snapshot plus the derived fulfillment state.
#define USAPP_REDEMPTION_COLUMNS                                                                        \
    "d.id::text AS id, d.product_id::text AS product_id, d.product_version, d.creator_id::text AS creator_id, " \
    "d.redeemer_id::text AS redeemer_id, d.title, d.description, d.price_flowers, d.redemption_mode, "    \
    "d.operation_id::text AS operation_id, " USAPP_TS("d.created_at") " AS created_at, "                  \
    "d.status, d.cancel_reason, " USAPP_TS("d.cancelled_at") " AS cancelled_at, " \
    "d.cancellation_id::text AS cancellation_id, d.cancellation_by::text AS cancellation_by, " \
    USAPP_TS("d.fulfilled_at") " AS fulfilled_at, d.fulfilled_by::text AS fulfilled_by"
#define USAPP_REDEMPTION_FROM \
    "reward_redemption_state d"
// Both parties (redeemer and creator) read their redemptions; nobody else does.
#define USAPP_PARTY "(d.creator_id = $1::uuid OR d.redeemer_id = $1::uuid)"

#define USAPP_LIST_SQL(predicate, limit)                                                               \
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "                               \
    "LEFT JOIN LATERAL (SELECT " USAPP_REDEMPTION_COLUMNS ", d.created_at AS sort_ca, d.id AS sort_id " \
    "FROM " USAPP_REDEMPTION_FROM " WHERE d.couple_id = m.couple_id AND " USAPP_PARTY predicate         \
    " ORDER BY d.created_at DESC, d.id DESC LIMIT " limit "::integer) x ON true "                      \
    "WHERE m.user_id = $1::uuid ORDER BY x.sort_ca DESC, x.sort_id DESC"

const char *kListFirstSql = USAPP_LIST_SQL("", "$2");
const char *kListAfterSql = USAPP_LIST_SQL(" AND (d.created_at, d.id) < ($2::timestamptz, $3::uuid)", "$4");

const char *kGetSql =
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "
    "LEFT JOIN LATERAL (SELECT " USAPP_REDEMPTION_COLUMNS " FROM " USAPP_REDEMPTION_FROM
    " WHERE d.couple_id = m.couple_id AND d.id = $2::uuid AND " USAPP_PARTY ") x ON true "
    "WHERE m.user_id = $1::uuid";

const char *kRedemptionByIdSql =
    "SELECT " USAPP_REDEMPTION_COLUMNS " FROM " USAPP_REDEMPTION_FROM " WHERE d.id = $1::uuid";

// Immutable facts of a redeem (the snapshot and its debit) and of a fulfil.
const char *kRedeemFactsSql =
    "SELECT json_build_object('action', 'redeem', 'redemption_id', d.id, 'product_id', d.product_id, "
    "'product_version', d.product_version, 'title', d.title, 'description', d.description, "
    "'price_flowers', d.price_flowers, 'redemption_mode', d.redemption_mode, 'creator_id', d.creator_id, "
    "'redeemer_id', d.redeemer_id, 'operation_id', d.operation_id, 'debited_flowers', CASE WHEN d.redemption_mode='direct' THEN d.price_flowers ELSE 0 END, "
    "'created_at', " USAPP_TS("d.created_at") ")::text AS mutation_json "
    "FROM reward_redemptions d WHERE d.id = $1::uuid";

const char *kEventFactsSql =
    "SELECT json_build_object('action',e.action,'event_id',e.id,'redemption_id',e.redemption_id,"
    "'actor_id',e.actor_id,'proposal_id',e.proposal_id,'operation_id',e.operation_id,"
    "'debited_flowers',CASE WHEN e.action='confirm' THEN d.price_flowers ELSE 0 END,"
    "'refunded_flowers',CASE WHEN e.action='cancel_approve' THEN d.price_flowers ELSE 0 END,"
    "'created_at'," USAPP_TS("e.created_at") ")::text AS mutation_json "
    "FROM reward_redemption_events e JOIN reward_redemptions d ON d.id=e.redemption_id WHERE e.id=$1::uuid";

const char *kFulfillFactsSql =
    "SELECT json_build_object('action', 'fulfill', 'redemption_id', f.redemption_id, "
    "'fulfilled_by', f.fulfilled_by, 'fulfilled_at', " USAPP_TS("f.fulfilled_at") ")::text AS mutation_json "
    "FROM reward_redemption_fulfillments f WHERE f.redemption_id = $1::uuid";

// Redeem step 1: membership FOR SHARE joined to a product of the same space.
const char *kRedeemTargetSql =
    "SELECT m.couple_id::text AS couple_id, p.id::text AS product_id, p.creator_id::text AS creator_id, "
    "p.owner_id::text AS owner_id FROM couple_members m "
    "LEFT JOIN reward_products p ON p.couple_id = m.couple_id AND p.id = $2::uuid "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kProductLockSql = "SELECT pg_advisory_xact_lock(33007::integer, hashtext($1::text))";
const char *kRedemptionLockSql = "SELECT pg_advisory_xact_lock(33008::integer, hashtext($1::text))";

const char *kRedeemByKeySql =
    "SELECT id::text AS id, request_fingerprint FROM reward_redemptions "
    "WHERE redeemer_id = $1::uuid AND idempotency_key = $2";

const char *kLatestRevisionSql =
    "SELECT version, status, redemption_mode, price_flowers FROM reward_product_revisions "
    "WHERE product_id = $1::uuid ORDER BY version DESC LIMIT 1";

// Operation claim: the source key is the new redemption id (never conflicts);
// the request key lives on the redemption row.
const char *kClaimSql =
    "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'REWARD_REDEMPTION', 'reward_redemption', $4::text) "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

const char *kAccountInsertSql =
    "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES ($1::uuid, 'flower', $2::uuid) "
    "ON CONFLICT (user_id, asset_type) DO NOTHING";

const char *kAccountLockSql =
    "SELECT 1 AS locked FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = 'flower' FOR UPDATE";

// Read after the lock statement: a new READ COMMITTED snapshot sees the previous holder's commit.
const char *kLatestEntrySql =
    "SELECT COALESCE((SELECT e.seq FROM wallet_ledger_entries e WHERE e.user_id = $1::uuid "
    "AND e.asset_type = 'flower' ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS seq, "
    "COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e WHERE e.user_id = $1::uuid "
    "AND e.asset_type = 'flower' ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS balance";

// Snapshot of exactly the version the caller saw (the guard re-checks it is still latest).
const char *kRedemptionInsertSql =
    "INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title, "
    "description, price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint) "
    "SELECT $1::uuid, $2::uuid, r.product_id, r.version, $3::uuid, $4::uuid, r.title, r.description, "
    "r.price_flowers, r.redemption_mode, NULLIF($5,'')::uuid, $6::text, $7::text "
    "FROM reward_product_revisions r WHERE r.product_id = $8::uuid AND r.version = $9::integer "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

const char *kDebitSql =
    "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'flower', $4::bigint, $5::bigint, $6::bigint)";

// Fulfill step 1: membership FOR SHARE joined to a redemption the caller is a party to.
const char *kFulfillTargetSql =
    "SELECT m.couple_id::text AS couple_id, d.id::text AS redemption_id, d.creator_id::text AS creator_id "
    "FROM couple_members m LEFT JOIN reward_redemptions d ON d.couple_id = m.couple_id AND d.id = $2::uuid "
    "AND (d.creator_id = $1::uuid OR d.redeemer_id = $1::uuid) "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kFulfillByKeySql =
    "SELECT redemption_id::text AS redemption_id, request_fingerprint FROM reward_redemption_fulfillments "
    "WHERE fulfilled_by = $1::uuid AND idempotency_key = $2";



const char *kFulfillInsertSql =
    "INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key, "
    "request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::text) "
    "ON CONFLICT DO NOTHING RETURNING redemption_id::text AS id";

const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'reward_redemption', $4::text, $5::jsonb)";

#undef USAPP_LIST_SQL
#undef USAPP_PARTY
#undef USAPP_REDEMPTION_FROM
#undef USAPP_REDEMPTION_COLUMNS
#undef USAPP_TS

bool isStrictInteger(const Json::Value &v) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt();
}

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

std::string textOrEmpty(const drogon::orm::Row &row, const char *name) {
    return row[name].isNull() ? std::string() : row[name].as<std::string>();
}

RedemptionService::RedemptionData redemptionFromRow(const drogon::orm::Row &row) {
    RedemptionService::RedemptionData d;
    d.id = row["id"].as<std::string>();
    d.productId = row["product_id"].as<std::string>();
    d.productVersion = row["product_version"].as<int>();
    d.creatorId = row["creator_id"].as<std::string>();
    d.redeemerId = row["redeemer_id"].as<std::string>();
    d.title = row["title"].as<std::string>();
    d.description = row["description"].as<std::string>();
    d.priceFlowers = row["price_flowers"].as<int>();
    d.redemptionMode = row["redemption_mode"].as<std::string>();
    d.operationId = textOrEmpty(row, "operation_id");
    d.status = row["status"].as<std::string>();
    d.createdAt = row["created_at"].as<std::string>();
    d.fulfilledAt = textOrEmpty(row, "fulfilled_at");
    d.fulfilledBy = textOrEmpty(row, "fulfilled_by");
    d.cancelReason = textOrEmpty(row, "cancel_reason");
    d.cancelledAt = textOrEmpty(row, "cancelled_at");
    d.cancellationId = textOrEmpty(row, "cancellation_id");
    d.cancellationBy = textOrEmpty(row, "cancellation_by");
    return d;
}

RedemptionService::Status makeStatus(RedemptionService::Outcome outcome, std::string message = "",
                                     std::string detail = "") {
    RedemptionService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

std::string label(bool fulfill, const char *stage, const char *reason) {
    return TaskService::failureDetail(fulfill ? "redemption_fulfill" : "redemption_redeem", stage, reason);
}

// ---------------------------------------------------------------------
// Lifetime: Ctx --(shared)--> TxScope --(onError_, weak)--> Ctx, as in the task and
// product services. Every end goes through complete() exactly once; commitRequested
// is set before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    bool fulfill = false;
    bool requestMode = false;
    std::string action, proposalId, eventId, redeemerId;
    std::string userId;
    std::string coupleId;
    std::string productId;
    std::string creatorId;  // the product creator (snapshot)
    std::string redemptionId;
    std::string operationId;
    std::string key;
    std::string fingerprint;
    int expectedVersion = 0;
    std::int64_t price = 0;
    std::int64_t prevSeq = 0;
    std::int64_t prevBalance = 0;
    RedemptionService::Status result;
    RedemptionService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<Ctx>;

void complete(const CtxPtr &ctx, RedemptionService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "RedemptionService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->fulfill, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, RedemptionService::Status status) {
    auto shared = std::make_shared<RedemptionService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

// Immutable facts (redeem or fulfil) plus the current redemption, inside the transaction.
void readFacts(const CtxPtr &ctx, const std::string &redemptionId, bool fulfillFacts,
               RedemptionService::Outcome outcome, std::function<void(RedemptionService::Status)> next) {
    std::string id = redemptionId;
    ctx->scope->tx().execSqlAsync(
        !ctx->action.empty() ? kEventFactsSql : (fulfillFacts ? kFulfillFactsSql : kRedeemFactsSql),
        [ctx, id, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_facts", "row_count"); return; }
            auto status = std::make_shared<RedemptionService::Status>(makeStatus(outcome));
            try {
                if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) {
                    failAt(ctx, "read_facts", "json_format");
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "read_facts", "column_format"); return; }
            std::string current = id;
            ctx->scope->tx().execSqlAsync(
                kRedemptionByIdSql,
                [ctx, status, next](const drogon::orm::Result &found) {
                    if (found.size() != 1u) { failAt(ctx, "read_redemption", "row_count"); return; }
                    try { status->redemption = redemptionFromRow(found[0]); }
                    catch (const std::exception &) { failAt(ctx, "read_redemption", "column_format"); return; }
                    next(std::move(*status));
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_redemption", "db_error"); },
                current);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_facts", "db_error"); },
        ctx->action.empty() ? id : ctx->eventId);
}

void replay(const CtxPtr &ctx, const std::string &redemptionId) {
    readFacts(ctx, redemptionId, ctx->fulfill, RedemptionService::Outcome::Replayed,
              [ctx](RedemptionService::Status status) { rejectWith(ctx, std::move(status)); });
}

void commitWithFacts(const CtxPtr &ctx, RedemptionService::Outcome outcome) {
    readFacts(ctx, ctx->redemptionId, ctx->fulfill, outcome, [ctx](RedemptionService::Status status) {
        ctx->result = std::move(status);
        ctx->commitRequested = true;
        ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
    });
}

void audit(const CtxPtr &ctx, const char *action, const Json::Value &detail, std::function<void()> next) {
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx, next = std::move(next)](const drogon::orm::Result &) {
            const std::string family = !ctx->action.empty() ? "redemption_event" :
                                       (ctx->fulfill ? "fulfillment" : "redemption");
            writeNotification(ctx->scope, family, ctx->action.empty() ? ctx->redemptionId : ctx->eventId,
                              next, [ctx]() { failAt(ctx, "notification", "write_failed"); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId, ctx->coupleId, std::string(action), ctx->redemptionId, compactJson(detail));
}

// ================================================================ redeem
void redeemClassify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kRedeemByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string id;
            std::string stored;
            try {
                id = rows[0]["id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            replay(ctx, id);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void redeemDebit(const CtxPtr &ctx) {
    std::int64_t seq = ctx->prevSeq + 1;
    std::int64_t amount = -ctx->price;
    std::int64_t balanceAfter = ctx->prevBalance - ctx->price;
    ctx->scope->tx().execSqlAsync(
        kDebitSql,
        [ctx](const drogon::orm::Result &) {
            Json::Value detail(Json::objectValue);
            detail["product_id"] = ctx->productId;
            detail["product_version"] = ctx->expectedVersion;
            detail["price_flowers"] = Json::Int64(ctx->price);
            detail["operation_id"] = ctx->operationId;
            audit(ctx, "reward_redemption.redeemed", detail,
                  [ctx]() { commitWithFacts(ctx, RedemptionService::Outcome::Redeemed); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "debit", "db_error"); },
        ctx->operationId, ctx->coupleId, ctx->userId, seq, amount, balanceAfter);
}

void redeemSnapshot(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kRedemptionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // A concurrent same-key redemption of another product committed first.
                redeemClassify(ctx, [ctx]() { failAt(ctx, "snapshot", "conflict_unexplained"); });
                return;
            }
            if (ctx->requestMode) {
                Json::Value detail(Json::objectValue);
                detail["product_id"] = ctx->productId;
                audit(ctx, "reward_redemption.requested", detail, [ctx]() { commitWithFacts(ctx, RedemptionService::Outcome::Requested); });
            } else redeemDebit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "snapshot", "db_error"); },
        ctx->redemptionId, ctx->coupleId, ctx->creatorId, ctx->userId, ctx->operationId, ctx->key,
        ctx->fingerprint, ctx->productId, std::to_string(ctx->expectedVersion));
}

void redeemBalance(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kLatestEntrySql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "balance", "row_shape"); return; }
            try {
                ctx->prevSeq = static_cast<std::int64_t>(rows[0]["seq"].as<long long>());
                ctx->prevBalance = static_cast<std::int64_t>(rows[0]["balance"].as<long long>());
            } catch (const std::exception &) { failAt(ctx, "balance", "column_format"); return; }
            if (ctx->prevBalance < ctx->price) {
                // Rolled back: the claimed operation and any new account disappear; nothing is reserved.
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::InsufficientBalance, "小红花不足，无法兑换"));
                return;
            }
            redeemSnapshot(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "balance", "db_error"); },
        ctx->userId);
}

void redeemAccount(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kAccountInsertSql,
        [ctx](const drogon::orm::Result &) {
            ctx->scope->tx().execSqlAsync(
                kAccountLockSql,
                [ctx](const drogon::orm::Result &rows) {
                    if (rows.size() != 1u) { failAt(ctx, "account", "lock_missing"); return; }
                    redeemBalance(ctx);
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "account", "db_error"); },
                ctx->userId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "account", "db_error"); },
        ctx->userId, ctx->coupleId);
}

void redeemClaim(const CtxPtr &ctx) {
    ctx->redemptionId = newUuid();
    ctx->operationId = newUuid();
    ctx->scope->tx().execSqlAsync(
        kClaimSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "claim", "conflict_unexplained"); return; }
            redeemAccount(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "claim", "db_error"); },
        ctx->operationId, ctx->coupleId, ctx->userId, ctx->redemptionId);
}

void redeemState(const CtxPtr &ctx, const std::string &creatorId, const std::string &ownerId) {
    ctx->scope->tx().execSqlAsync(
        kLatestRevisionSql,
        [ctx, creatorId, ownerId](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            int version = 0;
            std::string status;
            std::string mode;
            try {
                version = rows[0]["version"].as<int>();
                status = rows[0]["status"].as<std::string>();
                mode = rows[0]["redemption_mode"].as<std::string>();
                ctx->price = rows[0]["price_flowers"].as<int>();
            } catch (const std::exception &) { failAt(ctx, "state", "column_format"); return; }
            if (ctx->userId == creatorId) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::Forbidden, "不能兑换自己上架的奖励"));
                return;
            }
            if (ctx->userId != ownerId) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::ProductNotFound, "奖励不存在"));
                return;
            }
            if (status == "archived") {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::ProductArchived, "奖励已下架，不能兑换"));
                return;
            }
            if (version != ctx->expectedVersion) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::VersionConflict,
                                           "奖励内容或价格已更新，请刷新后确认新的价格"));
                return;
            }
            if (mode != "direct" && mode != "request") {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::ModeUnsupported, "申请兑换将在之后的版本提供"));
                return;
            }
            ctx->creatorId = creatorId;
            ctx->requestMode = mode == "request";
            if (ctx->requestMode) { ctx->redemptionId = newUuid(); redeemSnapshot(ctx); }
            else redeemClaim(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->productId);
}

void redeemTarget(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kRedeemTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["product_id"].isNull()) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::ProductNotFound, "奖励不存在"));
                return;
            }
            std::string creatorId;
            std::string ownerId;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->productId = rows[0]["product_id"].as<std::string>();  // canonical lock key
                creatorId = rows[0]["creator_id"].as<std::string>();
                ownerId = rows[0]["owner_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = RedemptionService::redeemFingerprint(ctx->productId, ctx->expectedVersion);
            ctx->scope->tx().execSqlAsync(
                kProductLockSql,
                [ctx, creatorId, ownerId](const drogon::orm::Result &) {
                    // Replay first: a committed redemption replays after later edits/archive/fulfillment.
                    redeemClassify(ctx, [ctx, creatorId, ownerId]() { redeemState(ctx, creatorId, ownerId); });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "lock", "db_error"); },
                ctx->productId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->productId);
}

// ================================================================ fulfill
void fulfillClassify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kFulfillByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string id;
            std::string stored;
            try {
                id = rows[0]["redemption_id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            replay(ctx, id);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void fulfillInsert(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kFulfillInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                fulfillClassify(ctx, [ctx]() { failAt(ctx, "insert", "conflict_unexplained"); });
                return;
            }
            Json::Value detail(Json::objectValue);
            detail["redemption_id"] = ctx->redemptionId;
            audit(ctx, "reward_redemption.fulfilled", detail,
                  [ctx]() { commitWithFacts(ctx, RedemptionService::Outcome::Fulfilled); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
        ctx->redemptionId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
}

void fulfillState(const CtxPtr &ctx, const std::string &creatorId) {
    if (ctx->userId != creatorId) {
        rejectWith(ctx, makeStatus(RedemptionService::Outcome::Forbidden, "只有上架这件奖励的一方可以确认履约"));
        return;
    }
    ctx->scope->tx().execSqlAsync(
        "SELECT status FROM reward_redemption_state WHERE id=$1::uuid",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "missing"); return; }
            const auto state = rows[0]["status"].as<std::string>();
            if (state != "pending_fulfillment") {
                rejectWith(ctx, makeStatus(state == "fulfilled" ? RedemptionService::Outcome::AlreadyFulfilled : RedemptionService::Outcome::InvalidState, "当前状态不能确认履约"));
                return;
            }
            fulfillInsert(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->redemptionId);
}

void fulfillTarget(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kFulfillTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["redemption_id"].isNull()) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::NotFound, "兑换记录不存在"));
                return;
            }
            std::string creatorId;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->redemptionId = rows[0]["redemption_id"].as<std::string>();
                creatorId = rows[0]["creator_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = RedemptionService::fulfillFingerprint(ctx->redemptionId);
            ctx->scope->tx().execSqlAsync(
                kRedemptionLockSql,
                [ctx, creatorId](const drogon::orm::Result &) {
                    fulfillClassify(ctx, [ctx, creatorId]() { fulfillState(ctx, creatorId); });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "lock", "db_error"); },
                ctx->redemptionId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->redemptionId);
}

// M9 transitions share the fulfillment advisory lock. Actor/key classification
// precedes state checks, so old confirmed facts remain replayable after any state.
void eventClassify(const CtxPtr &ctx, std::function<void()> missing) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text,redemption_id::text,request_fingerprint FROM reward_redemption_events "
        "WHERE actor_id=$1::uuid AND idempotency_key=$2",
        [ctx, missing=std::move(missing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { missing(); return; }
            if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(RedemptionService::Outcome::KeyReused,"该请求标识已用于另一项操作")); return;
            }
            ctx->eventId=rows[0]["id"].as<std::string>();
            replay(ctx, rows[0]["redemption_id"].as<std::string>());
        }, [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"classify","db_error"); },
        ctx->userId,ctx->key);
}

void eventInsert(const CtxPtr &ctx) {
    ctx->eventId=newUuid();
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO reward_redemption_events(id,redemption_id,couple_id,actor_id,action,proposal_id,"
        "operation_id,idempotency_key,request_fingerprint) VALUES($1::uuid,$2::uuid,$3::uuid,$4::uuid,"
        "$5,NULLIF($6,'')::uuid,NULLIF($7,'')::uuid,$8,$9) ON CONFLICT DO NOTHING RETURNING id",
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                eventClassify(ctx,[ctx]() { failAt(ctx,"event","conflict_unexplained"); }); return;
            }
            auto finish=[ctx]() {
                Json::Value detail(Json::objectValue);
                detail["event_id"]=ctx->eventId; detail["action"]=ctx->action;
                audit(ctx,"reward_redemption.changed",detail,[ctx]() {
                    commitWithFacts(ctx,RedemptionService::Outcome::Changed);
                });
            };
            if (ctx->operationId.empty()) { finish(); return; }
            std::int64_t amount=ctx->action=="confirm" ? -ctx->price : ctx->price;
            std::int64_t seq=ctx->prevSeq+1, balance=ctx->prevBalance+amount;
            ctx->scope->tx().execSqlAsync(kDebitSql,
                [finish](const drogon::orm::Result &) { finish(); },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"posting","db_error"); },
                ctx->operationId,ctx->coupleId,ctx->redeemerId,seq,amount,balance);
        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"event","db_error"); },
        ctx->eventId,ctx->redemptionId,ctx->coupleId,ctx->userId,ctx->action,ctx->proposalId,
        ctx->operationId,ctx->key,ctx->fingerprint);
}

void eventBalance(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(kLatestEntrySql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size()!=1u) { failAt(ctx,"balance","row_count"); return; }
            ctx->prevSeq=rows[0]["seq"].as<long long>();
            ctx->prevBalance=rows[0]["balance"].as<long long>();
            if (ctx->action=="confirm" && ctx->prevBalance<ctx->price) {
                rejectWith(ctx,makeStatus(RedemptionService::Outcome::InsufficientBalance,"小红花不足，申请仍待确认")); return;
            }
            if (ctx->action=="cancel_approve" && ctx->prevBalance>RedemptionService::kMaxBalanceFlowers-ctx->price) {
                rejectWith(ctx,makeStatus(RedemptionService::Outcome::BalanceLimit,"退款将超过余额上限，取消提议仍待确认")); return;
            }
            eventInsert(ctx);
        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"balance","db_error"); },ctx->redeemerId);
}

void eventMoney(const CtxPtr &ctx) {
    ctx->operationId=newUuid();
    std::string type=ctx->action=="confirm" ? "REWARD_REDEMPTION" : "REDEMPTION_REFUND";
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) "
        "VALUES($1::uuid,$2::uuid,$3::uuid,$4,'reward_redemption',$5)",
        [ctx](const drogon::orm::Result &) {
            ctx->scope->tx().execSqlAsync(kAccountInsertSql,
                [ctx](const drogon::orm::Result &) {
                    ctx->scope->tx().execSqlAsync(kAccountLockSql,
                        [ctx](const drogon::orm::Result &rows) {
                            if(rows.size()!=1u) { failAt(ctx,"account","missing"); return; }
                            eventBalance(ctx);
                        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"account","db_error"); },ctx->redeemerId);
                },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"account","db_error"); },ctx->redeemerId,ctx->coupleId);
        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"claim","db_error"); },
        ctx->operationId,ctx->coupleId,ctx->userId,type,ctx->redemptionId);
}

void eventState(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT status,creator_id::text,redeemer_id::text,price_flowers,cancellation_id::text,"
        "cancellation_by::text FROM reward_redemption_state WHERE id=$1::uuid",
        [ctx](const drogon::orm::Result &rows) {
            if(rows.size()!=1u) { failAt(ctx,"state","row_count"); return; }
            const auto &r=rows[0];
            const auto state=r["status"].as<std::string>();
            ctx->creatorId=r["creator_id"].as<std::string>();
            ctx->redeemerId=r["redeemer_id"].as<std::string>();
            ctx->price=r["price_flowers"].as<int>();
            bool forbidden=false, invalid=false;
            if(ctx->action=="confirm" || ctx->action=="reject" || ctx->action=="withdraw") {
                forbidden=ctx->userId!=(ctx->action=="withdraw" ? ctx->redeemerId : ctx->creatorId);
                invalid=state!="pending_confirmation";
            } else if(ctx->action=="cancel_propose") {
                invalid=state!="pending_fulfillment" || !r["cancellation_id"].isNull();
            } else {
                const auto proposer=textOrEmpty(r,"cancellation_by");
                forbidden=!proposer.empty() && (ctx->action=="cancel_withdraw" ? ctx->userId!=proposer : ctx->userId==proposer);
                invalid=state!="pending_fulfillment" || textOrEmpty(r,"cancellation_id")!=ctx->proposalId;
            }
            if(forbidden) { rejectWith(ctx,makeStatus(RedemptionService::Outcome::Forbidden,"你不能执行这项操作")); return; }
            if(invalid) { rejectWith(ctx,makeStatus(RedemptionService::Outcome::InvalidState,"兑换状态已改变，请刷新后重试")); return; }
            if(ctx->action=="confirm" || ctx->action=="cancel_approve") eventMoney(ctx);
            else eventInsert(ctx);
        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"state","db_error"); },ctx->redemptionId);
}

void eventTarget(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(kFulfillTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if(rows.size() == 0u) { rejectWith(ctx,makeStatus(RedemptionService::Outcome::SpaceNotFound,"尚未加入空间")); return; }
            if(rows.size()!=1u) { failAt(ctx,"membership","row_count"); return; }
            if(rows[0]["redemption_id"].isNull()) { rejectWith(ctx,makeStatus(RedemptionService::Outcome::NotFound,"兑换记录不存在")); return; }
            ctx->coupleId=rows[0]["couple_id"].as<std::string>();
            ctx->redemptionId=rows[0]["redemption_id"].as<std::string>();
            ctx->fingerprint=RedemptionService::transitionFingerprint(ctx->redemptionId,ctx->action,ctx->proposalId);
            ctx->scope->tx().execSqlAsync(kRedemptionLockSql,
                [ctx](const drogon::orm::Result &) { eventClassify(ctx,[ctx]() { eventState(ctx); }); },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"lock","db_error"); },ctx->redemptionId);
        },[ctx](const drogon::orm::DrogonDbException &) { failAt(ctx,"membership","db_error"); },ctx->userId,ctx->redemptionId);
}

void run(const CtxPtr &ctx, void (*first)(const CtxPtr &)) {
    std::weak_ptr<Ctx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx, first](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            first(ctx);
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(RedemptionService::Outcome::ResultUnknown, "",
                                                label(strong->fulfill, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(RedemptionService::Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(RedemptionService::Outcome::Error, "",
                                                label(strong->fulfill, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "RedemptionService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

RedemptionService &RedemptionService::instance() {
    static RedemptionService service;
    return service;
}

bool RedemptionService::parseRedeemRequest(const Json::Value &body, int &expectedVersion, std::string &message) {
    if (!body.isObject() || body.size() != 1u || !isStrictInteger(body["expected_version"]) ||
        body["expected_version"].asInt() < 1) {
        message = "请求体只能包含正整数 expected_version（页面显示的奖励版本）";
        return false;
    }
    expectedVersion = body["expected_version"].asInt();
    return true;
}

std::string RedemptionService::redeemFingerprint(const std::string &productId, int expectedVersion) {
    const std::string version = std::to_string(expectedVersion);
    return crypto::sha256Hex("reward-redeem:v1:product_id=" + std::to_string(productId.size()) + ":" + productId +
                             ";expected_version=" + std::to_string(version.size()) + ":" + version + ";");
}

std::string RedemptionService::fulfillFingerprint(const std::string &redemptionId) {
    return crypto::sha256Hex("reward-fulfill:v1:redemption_id=" + std::to_string(redemptionId.size()) + ":" +
                             redemptionId + ";");
}

bool RedemptionService::validTransition(const std::string &action) {
    return action=="confirm" || action=="reject" || action=="withdraw" || action=="cancel_propose" ||
           action=="cancel_approve" || action=="cancel_reject" || action=="cancel_withdraw";
}

std::string RedemptionService::transitionFingerprint(const std::string &id,const std::string &action,const std::string &proposalId) {
    return crypto::sha256Hex("redemption-transition:v1:id="+std::to_string(id.size())+":"+id+
        ";action="+std::to_string(action.size())+":"+action+";proposal="+std::to_string(proposalId.size())+":"+proposalId+";");
}

std::string RedemptionService::encodeCursor(const Cursor &cursor) {
    return cursor.createdAt + "_" + cursor.id;
}

std::optional<RedemptionService::Cursor> RedemptionService::decodeCursor(const std::string &raw) {
    if (raw.size() != 64u || raw[27] != '_') return std::nullopt;
    Cursor cursor{raw.substr(0, 27), raw.substr(28)};
    if (!TaskService::isCursorTimestamp(cursor.createdAt) || !TaskService::isCanonicalUuid(cursor.id)) {
        return std::nullopt;
    }
    return cursor;
}

Json::Value RedemptionService::redemptionJson(const RedemptionData &d) {
    Json::Value out(Json::objectValue);
    out["id"] = d.id;
    out["product_id"] = d.productId;
    out["product_version"] = d.productVersion;
    out["creator_id"] = d.creatorId;
    out["redeemer_id"] = d.redeemerId;
    out["title"] = d.title;
    out["description"] = d.description;
    out["price_flowers"] = d.priceFlowers;
    out["redemption_mode"] = d.redemptionMode;
    out["operation_id"] = d.operationId.empty() ? Json::Value(Json::nullValue) : Json::Value(d.operationId);
    out["cancel_reason"] = d.cancelReason.empty() ? Json::Value(Json::nullValue) : Json::Value(d.cancelReason);
    out["cancelled_at"] = d.cancelledAt.empty() ? Json::Value(Json::nullValue) : Json::Value(d.cancelledAt);
    out["pending_cancellation"] = Json::Value(Json::nullValue);
    if (!d.cancellationId.empty()) {
        out["pending_cancellation"]["id"] = d.cancellationId;
        out["pending_cancellation"]["proposed_by"] = d.cancellationBy;
    }
    out["status"] = d.status;
    out["created_at"] = d.createdAt;
    out["fulfilled_at"] = d.fulfilledAt.empty() ? Json::Value(Json::nullValue) : Json::Value(d.fulfilledAt);
    out["fulfilled_by"] = d.fulfilledBy.empty() ? Json::Value(Json::nullValue) : Json::Value(d.fulfilledBy);
    return out;
}

Json::Value RedemptionService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["mutation"] = status.mutation;
    body["redemption"] = redemptionJson(status.redemption);
    return body;
}

ErrorCode RedemptionService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::InvalidState: return ErrorCode::RedemptionStateConflict;
        case Outcome::BalanceLimit: return ErrorCode::WalletBalanceLimit;
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::ProductNotFound: return ErrorCode::ProductNotFound;
        case Outcome::ProductArchived: return ErrorCode::ProductArchived;
        case Outcome::VersionConflict: return ErrorCode::ProductVersionConflict;
        case Outcome::ModeUnsupported: return ErrorCode::RedemptionModeUnsupported;
        case Outcome::Forbidden: return ErrorCode::RedemptionForbidden;
        case Outcome::InsufficientBalance: return ErrorCode::InsufficientBalance;
        case Outcome::NotFound: return ErrorCode::RedemptionNotFound;
        case Outcome::AlreadyFulfilled: return ErrorCode::RedemptionAlreadyFulfilled;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::Requested:
        case Outcome::Changed:
        case Outcome::Redeemed:
        case Outcome::Fulfilled:
        case Outcome::Replayed:
        case Outcome::Ok:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int RedemptionService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Redeemed || outcome == Outcome::Requested) return 201;
    if (outcome == Outcome::Changed) return 200;
    if (outcome == Outcome::Fulfilled || outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void RedemptionService::redeem(const std::string &userId, const std::string &productId, const std::string &key,
                               int expectedVersion, Callback cb) {
    if (userId.empty() || productId.empty() || expectedVersion < 1 || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(false, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(false, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->userId = userId;
    ctx->productId = productId;
    ctx->key = key;
    ctx->expectedVersion = expectedVersion;
    ctx->done = std::move(cb);
    run(ctx, &redeemTarget);
}

void RedemptionService::fulfill(const std::string &userId, const std::string &redemptionId, const std::string &key,
                                Callback cb) {
    if (userId.empty() || redemptionId.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(true, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(true, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->fulfill = true;
    ctx->userId = userId;
    ctx->redemptionId = redemptionId;
    ctx->key = key;
    ctx->done = std::move(cb);
    run(ctx, &fulfillTarget);
}

void RedemptionService::transition(const std::string &userId,const std::string &redemptionId,const std::string &key,
                                  const std::string &action,const std::string &proposalId,Callback cb) {
    const bool needsProposal=action=="cancel_approve" || action=="cancel_reject" || action=="cancel_withdraw";
    if(userId.empty() || redemptionId.empty() || !validTransition(action) || !TaskService::validIdempotencyKey(key) ||
       (needsProposal ? !TaskService::isCanonicalUuid(proposalId) : !proposalId.empty())) {
        cb(makeStatus(Outcome::Validation,"请求不合法")); return;
    }
    if(!db::Pool::instance().ready()) { cb(makeStatus(Outcome::Error,"",label(false,"pool","not_ready"))); return; }
    auto ctx=std::make_shared<Ctx>();
    ctx->userId=userId;ctx->redemptionId=redemptionId;ctx->key=key;ctx->action=action;
    ctx->proposalId=proposalId;ctx->done=std::move(cb);
    run(ctx,&eventTarget);
}

void RedemptionService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor,
                             ListCallback cb) {
    ListResult early;
    if (userId.empty() || limit < 1 || limit > TaskService::kMaxPageSize) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = TaskService::failureDetail("redemption_list", "pool", "not_ready");
        cb(std::move(early));
        return;
    }
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::SpaceNotFound;
            result.message = "尚未加入空间";
            (*done)(std::move(result));
            return;
        }
        try {
            for (const auto &row : rows) {
                if (row["id"].isNull()) continue;
                result.redemptions.push_back(redemptionFromRow(row));
            }
        } catch (const std::exception &) {
            result = ListResult{};
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("redemption_list", "query", "column_format");
            (*done)(std::move(result));
            return;
        }
        if (result.redemptions.size() > static_cast<size_t>(limit)) {
            result.redemptions.resize(static_cast<size_t>(limit));
            const auto &last = result.redemptions.back();
            result.nextCursor = encodeCursor(Cursor{last.createdAt, last.id});
        }
        result.outcome = Outcome::Ok;
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult result;
        result.outcome = Outcome::Error;
        result.detail = TaskService::failureDetail("redemption_list", "query", "db_error");
        (*done)(std::move(result));
    };
    std::string fetch = std::to_string(limit + 1);
    if (cursor) {
        std::string createdAt = cursor->createdAt;
        std::string id = cursor->id;
        db::Pool::instance().client()->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId,
                                                  createdAt, id, fetch);
    } else {
        db::Pool::instance().client()->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId,
                                                  fetch);
    }
}

void RedemptionService::get(const std::string &userId, const std::string &redemptionId, Callback cb) {
    if (userId.empty() || redemptionId.empty()) {
        cb(makeStatus(Outcome::Validation, "请求参数不合法"));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", TaskService::failureDetail("redemption_get", "pool", "not_ready")));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { (*done)(makeStatus(Outcome::SpaceNotFound, "尚未加入空间")); return; }
            if (rows.size() != 1u) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("redemption_get", "query", "row_count")));
                return;
            }
            if (rows[0]["id"].isNull()) { (*done)(makeStatus(Outcome::NotFound, "兑换记录不存在")); return; }
            Status status = makeStatus(Outcome::Ok);
            try { status.redemption = redemptionFromRow(rows[0]); }
            catch (const std::exception &) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("redemption_get", "query", "column_format")));
                return;
            }
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("redemption_get", "query", "db_error")));
        },
        userId, redemptionId);
}

}  // namespace usapp
