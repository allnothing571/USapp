// =====================================================================
// services/api/src/services/AnniversaryService.cc
// =====================================================================
#include "services/AnniversaryService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <cctype>
#include <exception>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/NotificationWriter.h"
#include "services/AnniversaryCalendar.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define USAPP_DATE(expr) "to_char(" expr ", 'YYYY-MM-DD')"

// Beijing "today", evaluated once per statement (OFFSET 0 keeps it a single row).
#define USAPP_TODAY \
    "(SELECT " USAPP_DATE("(clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date") " AS as_of_date OFFSET 0) t"

// The current revision, the pending proposal and the newest decision of each item; the
// caller's current pin comes from the outer "pin" lateral.
#define USAPP_ANNIVERSARY_COLUMNS                                                                         \
    "a.id::text AS id, a.creator_id::text AS creator_id, r.version, r.title, "                           \
    USAPP_DATE("r.event_date") " AS event_date, r.kind, r.note, " USAPP_TS("a.created_at") " AS created_at, " \
    USAPP_TS("r.created_at") " AS updated_at, (a.id = pin.anniversary_id) IS TRUE AS pinned_by_me, "      \
    "p.id::text AS p_id, p.proposer_id::text AS p_proposer_id, p.base_version AS p_base_version, "        \
    "p.title AS p_title, " USAPP_DATE("p.event_date") " AS p_date, p.kind AS p_kind, p.note AS p_note, "  \
    USAPP_TS("p.created_at") " AS p_created_at, ld.proposal_id::text AS d_proposal_id, "                  \
    "ld.proposer_id::text AS d_proposer_id, ld.outcome AS d_outcome, ld.decided_by::text AS d_decided_by, " \
    USAPP_TS("ld.decided_at") " AS d_decided_at"
#define USAPP_ANNIVERSARY_FROM                                                                             \
    "anniversaries a "                                                                                     \
    "JOIN LATERAL (SELECT x.* FROM anniversary_revisions x WHERE x.anniversary_id = a.id "                 \
    "ORDER BY x.version DESC LIMIT 1) r ON true "                                                          \
    "LEFT JOIN LATERAL (SELECT x.* FROM anniversary_edit_proposals x WHERE x.anniversary_id = a.id "       \
    "AND NOT EXISTS (SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id = x.id) "              \
    "ORDER BY x.created_at DESC, x.id DESC LIMIT 1) p ON true "                                            \
    "LEFT JOIN LATERAL (SELECT d.proposal_id, x.proposer_id, d.outcome, d.decided_by, d.decided_at "       \
    "FROM anniversary_edit_decisions d JOIN anniversary_edit_proposals x ON x.id = d.proposal_id "         \
    "WHERE x.anniversary_id = a.id ORDER BY d.decided_at DESC, d.proposal_id DESC LIMIT 1) ld ON true"
#define USAPP_MEMBER_PIN                                                                                   \
    "LEFT JOIN LATERAL (SELECT hp.anniversary_id FROM anniversary_home_pins hp WHERE hp.user_id = m.user_id " \
    "ORDER BY hp.seq DESC LIMIT 1) pin ON true "

// Both members see every item of their space. One row per item, or one row with NULL
// item columns when the space has none (zero rows: no membership). tail = item filter
// plus ORDER/LIMIT inside the lateral.
#define USAPP_SPACE_SQL(tail)                                                                      \
    "SELECT m.couple_id::text AS couple_id, t.as_of_date, x.* FROM couple_members m CROSS JOIN "  \
    USAPP_TODAY " " USAPP_MEMBER_PIN                                                               \
    "LEFT JOIN LATERAL (SELECT " USAPP_ANNIVERSARY_COLUMNS ", a.created_at AS sort_ca, a.id AS sort_id " \
    "FROM " USAPP_ANNIVERSARY_FROM " WHERE a.couple_id = m.couple_id" tail ") x ON true "         \
    "WHERE m.user_id = $1::uuid ORDER BY x.sort_ca DESC, x.sort_id DESC"

// Pages in the immutable identity order (created_at DESC, id DESC). First: $1 user,
// $2 limit+1. Next: $2 created_at, $3 id, $4 limit+1.
const char *kListFirstSql = USAPP_SPACE_SQL(" ORDER BY a.created_at DESC, a.id DESC LIMIT $2::integer");
const char *kListAfterSql = USAPP_SPACE_SQL(
    " AND (a.created_at, a.id) < ($2::timestamptz, $3::uuid) ORDER BY a.created_at DESC, a.id DESC LIMIT $4::integer");
// Home: the complete current dataset, no limit ($1 user).
const char *kHomeSql = USAPP_SPACE_SQL("");
// One item of the caller's space ($1 user, $2 id).
const char *kGetSql = USAPP_SPACE_SQL(" AND a.id = $2::uuid");

#undef USAPP_SPACE_SQL

// Write step 1 for create: the caller's member row FOR SHARE.
const char *kMembershipSql =
    "SELECT m.couple_id::text AS couple_id FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";
// Edit/propose step 1: membership FOR SHARE joined to an item of the same space.
const char *kTargetSql =
    "SELECT m.couple_id::text AS couple_id, a.id::text AS anniversary_id, a.creator_id::text AS creator_id "
    "FROM couple_members m LEFT JOIN anniversaries a ON a.couple_id = m.couple_id AND a.id = $2::uuid "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";
// Decide step 1: membership, the item of the same space and a proposal of that item.
const char *kDecisionTargetSql =
    "SELECT m.couple_id::text AS couple_id, a.id::text AS anniversary_id, a.creator_id::text AS creator_id, "
    "p.id::text AS proposal_id, p.proposer_id::text AS proposer_id FROM couple_members m "
    "LEFT JOIN anniversaries a ON a.couple_id = m.couple_id AND a.id = $2::uuid "
    "LEFT JOIN anniversary_edit_proposals p ON p.anniversary_id = a.id AND p.id = $3::uuid "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";
// Pin step 1: membership and the optional target of the same space ($2 '' = clear).
const char *kPinTargetSql =
    "SELECT m.user_id::text AS user_id, m.couple_id::text AS couple_id, a.id::text AS anniversary_id "
    "FROM couple_members m LEFT JOIN anniversaries a ON a.couple_id = m.couple_id "
    "AND a.id = NULLIF($2::text, '')::uuid WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kAnniversaryLockSql = "SELECT pg_advisory_xact_lock(33010::integer, hashtext($1::text))";
const char *kPinLockSql = "SELECT pg_advisory_xact_lock(33011::integer, hashtext($1::text))";

// Mutable state, read after the lock.
const char *kStateSql =
    "SELECT r.version, r.title, " USAPP_DATE("r.event_date") " AS event_date, r.kind, r.note, "
    "EXISTS (SELECT 1 FROM anniversary_edit_proposals x WHERE x.anniversary_id = r.anniversary_id "
    "AND NOT EXISTS (SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id = x.id)) AS has_pending "
    "FROM anniversary_revisions r WHERE r.anniversary_id = $1::uuid ORDER BY r.version DESC LIMIT 1";
const char *kDecisionStateSql =
    "SELECT (SELECT max(r.version) FROM anniversary_revisions r WHERE r.anniversary_id = p.anniversary_id) "
    "AS current_version, p.base_version, "
    "EXISTS (SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id = p.id) AS decided "
    "FROM anniversary_edit_proposals p WHERE p.id = $1::uuid";

// Key classification per table.
const char *kRevisionByKeySql =
    "SELECT anniversary_id::text AS ref, version::text AS ref2, request_fingerprint FROM anniversary_revisions "
    "WHERE actor_id = $1::uuid AND idempotency_key = $2";
const char *kProposalByKeySql =
    "SELECT id::text AS ref, '' AS ref2, request_fingerprint FROM anniversary_edit_proposals "
    "WHERE proposer_id = $1::uuid AND idempotency_key = $2";
const char *kDecisionByKeySql =
    "SELECT proposal_id::text AS ref, '' AS ref2, request_fingerprint FROM anniversary_edit_decisions "
    "WHERE decided_by = $1::uuid AND idempotency_key = $2";
const char *kPinByKeySql =
    "SELECT user_id::text AS ref, seq::text AS ref2, request_fingerprint FROM anniversary_home_pins "
    "WHERE user_id = $1::uuid AND idempotency_key = $2";

// Immutable facts of a write; each also names the item to read back (NULL for a cleared pin).
const char *kRevisionFactsSql =
    "SELECT r.anniversary_id::text AS anniversary_id, json_build_object('action', r.action, "
    "'anniversary_id', r.anniversary_id, 'version', r.version, 'actor_id', r.actor_id, "
    "'proposal_id', r.proposal_id, 'title', r.title, 'date', " USAPP_DATE("r.event_date") ", 'kind', r.kind, "
    "'note', r.note, 'created_at', " USAPP_TS("r.created_at") ")::text AS mutation_json "
    "FROM anniversary_revisions r WHERE r.anniversary_id = $1::uuid AND r.version = $2::integer";
const char *kProposalFactsSql =
    "SELECT p.anniversary_id::text AS anniversary_id, json_build_object('action', 'propose', "
    "'anniversary_id', p.anniversary_id, 'proposal_id', p.id, 'proposer_id', p.proposer_id, "
    "'base_version', p.base_version, 'title', p.title, 'date', " USAPP_DATE("p.event_date") ", "
    "'kind', p.kind, 'note', p.note, 'created_at', " USAPP_TS("p.created_at") ")::text AS mutation_json "
    "FROM anniversary_edit_proposals p WHERE p.id = $1::uuid";
const char *kDecisionFactsSql =
    "SELECT p.anniversary_id::text AS anniversary_id, json_build_object('action', 'decide', "
    "'anniversary_id', p.anniversary_id, 'proposal_id', d.proposal_id, 'proposer_id', p.proposer_id, "
    "'outcome', d.outcome, 'decided_by', d.decided_by, 'base_version', p.base_version, "
    "'version', r.version, 'decided_at', " USAPP_TS("d.decided_at") ")::text AS mutation_json "
    "FROM anniversary_edit_decisions d JOIN anniversary_edit_proposals p ON p.id = d.proposal_id "
    "LEFT JOIN anniversary_revisions r ON r.proposal_id = d.proposal_id AND r.action = 'approve' "
    "WHERE d.proposal_id = $1::uuid";
const char *kPinFactsSql =
    "SELECT h.anniversary_id::text AS anniversary_id, json_build_object('action', 'pin', 'user_id', h.user_id, "
    "'seq', h.seq, 'anniversary_id', h.anniversary_id, 'created_at', " USAPP_TS("h.created_at") ")::text "
    "AS mutation_json FROM anniversary_home_pins h WHERE h.user_id = $1::uuid AND h.seq = $2::integer";
// The caller's current pin.
const char *kCurrentPinSql =
    "SELECT h.seq, h.anniversary_id::text AS anniversary_id, " USAPP_TS("h.created_at") " AS created_at "
    "FROM anniversary_home_pins h WHERE h.user_id = $1::uuid ORDER BY h.seq DESC LIMIT 1";

const char *kIdentityInsertSql =
    "INSERT INTO anniversaries (id, couple_id, creator_id) VALUES ($1::uuid, $2::uuid, $3::uuid)";
const char *kRevisionInsertSql =
    "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, "
    "kind, note, idempotency_key, request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::integer, $4::uuid, "
    "$5::text, $6::text, $7::date, $8::text, $9::text, $10::text, $11::text) "
    "ON CONFLICT DO NOTHING RETURNING version";
const char *kProposalInsertSql =
    "INSERT INTO anniversary_edit_proposals (id, couple_id, anniversary_id, proposer_id, base_version, title, "
    "event_date, kind, note, idempotency_key, request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, "
    "$4::uuid, $5::integer, $6::text, $7::date, $8::text, $9::text, $10::text, $11::text) "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";
const char *kDecisionInsertSql =
    "INSERT INTO anniversary_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, "
    "request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::text, $6::text) "
    "ON CONFLICT DO NOTHING RETURNING proposal_id::text AS id";
// Approval: exactly the proposal's terms as base_version + 1.
const char *kApproveRevisionSql =
    "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, proposal_id, title, "
    "event_date, kind, note) SELECT p.anniversary_id, p.couple_id, p.base_version + 1, $2::uuid, 'approve', p.id, "
    "p.title, p.event_date, p.kind, p.note FROM anniversary_edit_proposals p WHERE p.id = $1::uuid "
    "RETURNING version";
const char *kPinInsertSql =
    "INSERT INTO anniversary_home_pins (user_id, seq, couple_id, anniversary_id, idempotency_key, "
    "request_fingerprint) SELECT $1::uuid, COALESCE(max(h.seq), 0) + 1, $2::uuid, NULLIF($3::text, '')::uuid, "
    "$4::text, $5::text FROM anniversary_home_pins h WHERE h.user_id = $1::uuid "
    "ON CONFLICT DO NOTHING RETURNING seq::text AS seq";

// The audit detail holds ids, version, kind and outcome only, never text.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, $4::text, NULLIF($5::text, ''), $6::jsonb)";

#undef USAPP_MEMBER_PIN
#undef USAPP_ANNIVERSARY_FROM
#undef USAPP_ANNIVERSARY_COLUMNS
#undef USAPP_TODAY
#undef USAPP_DATE
#undef USAPP_TS

using Outcome = AnniversaryService::Outcome;
using Terms = AnniversaryService::Terms;
using Decision = AnniversaryService::Decision;

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

std::string field(const char *name, const std::string &value) {
    return std::string(name) + "=" + std::to_string(value.size()) + ":" + value + ";";
}

std::string termsCanonical(const Terms &t) {
    return field("title", t.title) + field("date", t.date) + field("kind", t.kind) + field("note", t.note);
}

// The body holds only allowed members and every required one.
bool onlyFields(const Json::Value &body, std::initializer_list<const char *> required,
                std::initializer_list<const char *> optional, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是 JSON 对象";
        return false;
    }
    std::set<std::string> allowed;
    for (const char *name : required) allowed.insert(name);
    for (const char *name : optional) allowed.insert(name);
    for (const auto &name : body.getMemberNames()) {
        if (!allowed.count(name)) {
            message = "请求体包含不允许的字段";
            return false;
        }
    }
    for (const char *name : required) {
        if (!body.isMember(name)) {
            message = std::string("缺少字段 ") + name;
            return false;
        }
    }
    return true;
}

bool parseTerms(const Json::Value &body, Terms &out, std::string &message) {
    if (!body["title"].isString()) {
        message = "标题必须是字符串";
        return false;
    }
    switch (TaskService::normalizeTitle(body["title"].asString(), out.title)) {
        case TaskService::TextError::None: break;
        case TaskService::TextError::InvalidUtf8: message = "标题包含无法识别的字符"; return false;
        case TaskService::TextError::ControlCharacter: message = "标题只能是一行，且不能包含控制字符"; return false;
        case TaskService::TextError::Empty: message = "标题不能为空"; return false;
        case TaskService::TextError::TooLong: message = "标题最多 40 个字符"; return false;
    }
    if (!body["date"].isString()) {
        message = "日期必须是 YYYY-MM-DD 字符串";
        return false;
    }
    const auto date = AnniversaryCalendar::parse(body["date"].asString());
    if (!date || !AnniversaryCalendar::inInputRange(*date)) {
        message = "日期必须是 1900-01-01 到 2199-12-31 之间的有效日期";
        return false;
    }
    out.date = AnniversaryCalendar::format(*date);
    if (!body["kind"].isString() || !AnniversaryCalendar::parseKind(body["kind"].asString())) {
        message = "类型只能是 anniversary 或 countdown";
        return false;
    }
    out.kind = body["kind"].asString();
    out.note.clear();
    if (body.isMember("note")) {
        if (!body["note"].isString()) {
            message = "备注必须是字符串";
            return false;
        }
        switch (TaskService::normalizeDescription(body["note"].asString(), out.note)) {
            case TaskService::TextError::None:
            case TaskService::TextError::Empty:
                break;
            case TaskService::TextError::InvalidUtf8: message = "备注包含无法识别的字符"; return false;
            case TaskService::TextError::ControlCharacter: message = "备注不能包含控制字符"; return false;
            case TaskService::TextError::TooLong: message = "备注最多 500 个字符"; return false;
        }
    }
    return true;
}

bool isStrictInteger(const Json::Value &v) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt();
}

std::string optText(const drogon::orm::Row &row, const char *name) {
    return row[name].isNull() ? std::string() : row[name].as<std::string>();
}

AnniversaryService::AnniversaryData anniversaryFromRow(const drogon::orm::Row &row) {
    AnniversaryService::AnniversaryData a;
    a.id = row["id"].as<std::string>();
    a.creatorId = row["creator_id"].as<std::string>();
    a.version = row["version"].as<int>();
    a.terms.title = row["title"].as<std::string>();
    a.terms.date = row["event_date"].as<std::string>();
    a.terms.kind = row["kind"].as<std::string>();
    a.terms.note = row["note"].as<std::string>();
    a.createdAt = row["created_at"].as<std::string>();
    a.updatedAt = row["updated_at"].as<std::string>();
    a.pinnedByMe = row["pinned_by_me"].as<bool>();
    if (!row["p_id"].isNull()) {
        AnniversaryService::ProposalData p;
        p.id = row["p_id"].as<std::string>();
        p.proposerId = row["p_proposer_id"].as<std::string>();
        p.baseVersion = row["p_base_version"].as<int>();
        p.terms.title = row["p_title"].as<std::string>();
        p.terms.date = row["p_date"].as<std::string>();
        p.terms.kind = row["p_kind"].as<std::string>();
        p.terms.note = row["p_note"].as<std::string>();
        p.createdAt = row["p_created_at"].as<std::string>();
        a.pending = p;
    }
    if (!row["d_proposal_id"].isNull()) {
        AnniversaryService::DecisionData d;
        d.proposalId = row["d_proposal_id"].as<std::string>();
        d.proposerId = row["d_proposer_id"].as<std::string>();
        d.outcome = row["d_outcome"].as<std::string>();
        d.decidedBy = row["d_decided_by"].as<std::string>();
        d.decidedAt = row["d_decided_at"].as<std::string>();
        a.lastDecision = d;
    }
    return a;
}

AnniversaryService::Status makeStatus(Outcome outcome, std::string message = "", std::string detail = "") {
    AnniversaryService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

// ---------------------------------------------------------------------
// Lifetime: Ctx --(shared)--> TxScope --(onError_, weak)--> Ctx, as in the other
// services. Every end goes through complete() exactly once; commitRequested is set
// before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
enum class Op { Create, Edit, Propose, Decide, Pin };

struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    Op op = Op::Create;
    std::string userId;
    std::string coupleId;
    std::string anniversaryId;  // canonical, from the database (pin: target or empty)
    std::string creatorId;
    std::string proposalId;
    std::string proposerId;
    std::string key;
    std::string fingerprint;
    Terms terms;
    int expectedVersion = 0;
    Decision decision = Decision::Approve;
    bool clearPin = false;
    intent::Choice choice;     // create / creator edit only (P2-3b)
    Json::Value notification;  // original notification facts of this revision
    // Reference of the committed write for read-back.
    std::string ref;
    std::string ref2;
    AnniversaryService::Status result;
    AnniversaryService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<Ctx>;

const char *opName(Op op) {
    switch (op) {
        case Op::Create: return "anniversary_create";
        case Op::Edit: return "anniversary_edit";
        case Op::Propose: return "anniversary_propose";
        case Op::Decide: return "anniversary_decide";
        case Op::Pin: break;
    }
    return "anniversary_pin";
}

std::string label(Op op, const char *stage, const char *reason) {
    return TaskService::failureDetail(opName(op), stage, reason);
}

void complete(const CtxPtr &ctx, AnniversaryService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "AnniversaryService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->op, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, AnniversaryService::Status status) {
    auto shared = std::make_shared<AnniversaryService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

void reject(const CtxPtr &ctx, Outcome outcome, const char *message) {
    rejectWith(ctx, makeStatus(outcome, message));
}

// Current state for the response: the caller's pin, or the item with as_of_date.
void readCurrent(const CtxPtr &ctx, const std::shared_ptr<AnniversaryService::Status> &status,
                 const std::string &anniversaryId, std::function<void(AnniversaryService::Status)> next) {
    if (ctx->op == Op::Pin) {
        status->isPin = true;
        ctx->scope->tx().execSqlAsync(
            kCurrentPinSql,
            [ctx, status, next](const drogon::orm::Result &rows) {
                if (rows.size() != 1u) { failAt(ctx, "read_pin", "row_count"); return; }
                try {
                    status->pin.seq = rows[0]["seq"].as<int>();
                    if (!rows[0]["anniversary_id"].isNull()) {
                        status->pin.anniversaryId = rows[0]["anniversary_id"].as<std::string>();
                    }
                    status->pin.updatedAt = rows[0]["created_at"].as<std::string>();
                } catch (const std::exception &) { failAt(ctx, "read_pin", "column_format"); return; }
                next(std::move(*status));
            },
            [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_pin", "db_error"); },
            ctx->userId);
        return;
    }
    std::string id = anniversaryId;
    ctx->scope->tx().execSqlAsync(
        kGetSql,
        [ctx, status, next](const drogon::orm::Result &rows) {
            if (rows.size() != 1u || rows[0]["id"].isNull()) { failAt(ctx, "read_current", "row_count"); return; }
            try {
                status->asOfDate = rows[0]["as_of_date"].as<std::string>();
                status->data = anniversaryFromRow(rows[0]);
            } catch (const std::exception &) { failAt(ctx, "read_current", "column_format"); return; }
            next(std::move(*status));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_current", "db_error"); },
        ctx->userId, id);
}

const char *factsSqlFor(Op op) {
    switch (op) {
        case Op::Create:
        case Op::Edit: return kRevisionFactsSql;
        case Op::Propose: return kProposalFactsSql;
        case Op::Decide: return kDecisionFactsSql;
        case Op::Pin: break;
    }
    return kPinFactsSql;
}

// Immutable facts of the write (ref, ref2) plus the current state, inside the transaction.
void readFacts(const CtxPtr &ctx, Outcome outcome, std::function<void(AnniversaryService::Status)> next) {
    auto onRows = [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
        if (rows.size() != 1u) { failAt(ctx, "read_mutation", "row_count"); return; }
        auto status = std::make_shared<AnniversaryService::Status>(makeStatus(outcome));
        std::string anniversaryId;
        try {
            if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) {
                failAt(ctx, "read_mutation", "json_format");
                return;
            }
            anniversaryId = optText(rows[0], "anniversary_id");
        } catch (const std::exception &) { failAt(ctx, "read_mutation", "column_format"); return; }
        readCurrent(ctx, status, anniversaryId, next);
    };
    auto onError = [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_mutation", "db_error"); };
    const char *sql = factsSqlFor(ctx->op);
    if (ctx->op == Op::Propose || ctx->op == Op::Decide) {
        ctx->scope->tx().execSqlAsync(sql, std::move(onRows), std::move(onError), ctx->ref);
    } else {
        ctx->scope->tx().execSqlAsync(sql, std::move(onRows), std::move(onError), ctx->ref, ctx->ref2);
    }
}

const char *classifySqlFor(Op op) {
    switch (op) {
        case Op::Create:
        case Op::Edit: return kRevisionByKeySql;
        case Op::Propose: return kProposalByKeySql;
        case Op::Decide: return kDecisionByKeySql;
        case Op::Pin: break;
    }
    return kPinByKeySql;
}

// Same actor and key: an identical request replays its committed write (read-only);
// a different fingerprint is IDEMPOTENCY_KEY_REUSED. Runs before any state check.
// P2-3b: creation and the creator's direct edit carry an optional notification intent.
bool hasIntent(Op op) {
    return op == Op::Create || op == Op::Edit;
}

intent::Operation intentOperation(Op op) {
    return op == Op::Create ? intent::Operation::AnniversaryCreate : intent::Operation::AnniversaryEdit;
}

// The revision of this write: (anniversary id, version) in ref / ref2.
intent::Source revisionSource(const CtxPtr &ctx) {
    intent::Source source;
    source.anniversaryId = ctx->ref;
    try { source.anniversaryVersion = std::stoi(ctx->ref2); } catch (const std::exception &) {}
    return source;
}

void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        classifySqlFor(ctx->op),
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string stored;
            try {
                ctx->ref = rows[0]["ref"].as<std::string>();
                ctx->ref2 = rows[0]["ref2"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                return;
            }
            readFacts(ctx, Outcome::Replayed, [ctx](AnniversaryService::Status status) {
                if (!hasIntent(ctx->op)) {
                    rejectWith(ctx, std::move(status));
                    return;
                }
                // A replay only reads the original decision; it never appends an intent or notice.
                auto shared = std::make_shared<AnniversaryService::Status>(std::move(status));
                intent::readOriginal(
                    ctx->scope, intentOperation(ctx->op), revisionSource(ctx),
                    [ctx, shared](Json::Value facts) {
                        shared->hasNotification = true;
                        shared->notification = std::move(facts);
                        rejectWith(ctx, std::move(*shared));
                    },
                    [ctx]() { failAt(ctx, "classify", "intent_read_failed"); });
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

const char *auditAction(const CtxPtr &ctx) {
    switch (ctx->op) {
        case Op::Create: return "anniversary.created";
        case Op::Edit: return "anniversary.edited";
        case Op::Propose: return "anniversary.edit_proposed";
        case Op::Decide:
            if (ctx->decision == Decision::Approve) return "anniversary.edit_approved";
            if (ctx->decision == Decision::Reject) return "anniversary.edit_rejected";
            return "anniversary.edit_withdrawn";
        case Op::Pin: break;
    }
    return "anniversary.home_pinned";
}

// Audit, read back, then commit; success only after a confirmed commit.
void auditAndCommit(const CtxPtr &ctx, Outcome outcome, Json::Value detail) {
    std::string target = ctx->op == Op::Pin ? "anniversary_home_pin" : "anniversary";
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx, outcome](const drogon::orm::Result &) {
            auto finish = [ctx, outcome]() {
                readFacts(ctx, outcome, [ctx](AnniversaryService::Status status) {
                    status.hasNotification = hasIntent(ctx->op);
                    status.notification = ctx->notification;
                    ctx->result = std::move(status);
                    ctx->commitRequested = true;
                    ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
                });
            };
            if (hasIntent(ctx->op)) {
                // Intent (and, when eligible, preference and notice) in this same transaction.
                intent::record(
                    ctx->scope, intentOperation(ctx->op), revisionSource(ctx), ctx->choice,
                    [ctx, finish](Json::Value facts) {
                        ctx->notification = std::move(facts);
                        finish();
                    },
                    [ctx]() { failAt(ctx, "notification_intent", "write_failed"); });
                return;
            }
            if (ctx->op != Op::Propose && ctx->op != Op::Decide) { finish(); return; }
            writeNotification(ctx->scope, ctx->op == Op::Propose ? "anniversary_proposal" : "anniversary_decision",
                              ctx->ref, finish, [ctx]() { failAt(ctx, "notification", "write_failed"); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), target, ctx->anniversaryId, compactJson(detail));
}

// A skipped ON CONFLICT insert: a concurrent same-key write committed first, or (for a
// decision) another decision won.
void reclassify(const CtxPtr &ctx, std::function<void()> otherwise) {
    classify(ctx, std::move(otherwise));
}

void lockThen(const CtxPtr &ctx, const char *sql, const std::string &id, std::function<void()> next) {
    ctx->scope->tx().execSqlAsync(
        sql, [next = std::move(next)](const drogon::orm::Result &) { next(); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "lock", "db_error"); }, id);
}

// ---- create ----
void createRevision(const CtxPtr &ctx) {
    std::string version = "1";
    std::string action = "create";
    ctx->scope->tx().execSqlAsync(
        kRevisionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // A concurrent same-key create committed first; this identity row rolls back.
                reclassify(ctx, [ctx]() { failAt(ctx, "insert_revision", "conflict_unexplained"); });
                return;
            }
            ctx->ref = ctx->anniversaryId;
            ctx->ref2 = "1";
            Json::Value detail(Json::objectValue);
            detail["anniversary_id"] = ctx->anniversaryId;
            detail["version"] = 1;
            detail["kind"] = ctx->terms.kind;
            auditAndCommit(ctx, Outcome::Created, detail);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_revision", "db_error"); },
        ctx->anniversaryId, ctx->coupleId, version, ctx->userId, action, ctx->terms.title, ctx->terms.date,
        ctx->terms.kind, ctx->terms.note, ctx->key, ctx->fingerprint);
}

void createInsert(const CtxPtr &ctx) {
    ctx->anniversaryId = newUuid();
    ctx->scope->tx().execSqlAsync(
        kIdentityInsertSql, [ctx](const drogon::orm::Result &) { createRevision(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_identity", "db_error"); },
        ctx->anniversaryId, ctx->coupleId, ctx->userId);
}

void createMembership(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kMembershipSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
            if (rows.size() != 1u || rows[0]["couple_id"].isNull()) { failAt(ctx, "membership", "row_shape"); return; }
            try { ctx->coupleId = rows[0]["couple_id"].as<std::string>(); }
            catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            classify(ctx, [ctx]() { createInsert(ctx); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId);
}

// ---- edit / propose ----
void editInsert(const CtxPtr &ctx) {
    std::string version = std::to_string(ctx->expectedVersion + 1);
    std::string action = "edit";
    ctx->scope->tx().execSqlAsync(
        kRevisionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                reclassify(ctx, [ctx]() { failAt(ctx, "insert_revision", "conflict_unexplained"); });
                return;
            }
            ctx->ref = ctx->anniversaryId;
            ctx->ref2 = std::to_string(ctx->expectedVersion + 1);
            Json::Value detail(Json::objectValue);
            detail["anniversary_id"] = ctx->anniversaryId;
            detail["version"] = ctx->expectedVersion + 1;
            detail["kind"] = ctx->terms.kind;
            auditAndCommit(ctx, Outcome::Created, detail);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_revision", "db_error"); },
        ctx->anniversaryId, ctx->coupleId, version, ctx->userId, action, ctx->terms.title, ctx->terms.date,
        ctx->terms.kind, ctx->terms.note, ctx->key, ctx->fingerprint);
}

void proposeInsert(const CtxPtr &ctx) {
    ctx->proposalId = newUuid();
    std::string base = std::to_string(ctx->expectedVersion);
    ctx->scope->tx().execSqlAsync(
        kProposalInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                reclassify(ctx, [ctx]() { failAt(ctx, "insert_proposal", "conflict_unexplained"); });
                return;
            }
            ctx->ref = ctx->proposalId;
            ctx->ref2.clear();
            Json::Value detail(Json::objectValue);
            detail["anniversary_id"] = ctx->anniversaryId;
            detail["proposal_id"] = ctx->proposalId;
            detail["base_version"] = ctx->expectedVersion;
            detail["kind"] = ctx->terms.kind;
            auditAndCommit(ctx, Outcome::Created, detail);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_proposal", "db_error"); },
        ctx->proposalId, ctx->coupleId, ctx->anniversaryId, ctx->userId, base, ctx->terms.title, ctx->terms.date,
        ctx->terms.kind, ctx->terms.note, ctx->key, ctx->fingerprint);
}

// After the lock and classification: role, version, pending and no-op checks.
void revisionChecks(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kStateSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            int current = 0;
            bool pending = false;
            Terms base;
            try {
                current = rows[0]["version"].as<int>();
                pending = rows[0]["has_pending"].as<bool>();
                base.title = rows[0]["title"].as<std::string>();
                base.date = rows[0]["event_date"].as<std::string>();
                base.kind = rows[0]["kind"].as<std::string>();
                base.note = rows[0]["note"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "state", "column_format"); return; }
            const bool isCreator = ctx->creatorId == ctx->userId;
            if (ctx->op == Op::Edit && !isCreator) {
                reject(ctx, Outcome::Forbidden, "只有创建者可以直接修改，你可以提交修改申请");
                return;
            }
            if (ctx->op == Op::Propose && isCreator) {
                reject(ctx, Outcome::Forbidden, "创建者请直接修改");
                return;
            }
            if (ctx->expectedVersion != current) {
                reject(ctx, Outcome::VersionConflict, "内容已经更新，请刷新后再修改");
                return;
            }
            if (pending) {
                reject(ctx, Outcome::ProposalPending,
                       ctx->op == Op::Edit ? "有待处理的修改申请，请先同意或拒绝" : "已有待处理的修改申请");
                return;
            }
            if (ctx->terms == base) {
                reject(ctx, Outcome::Validation, "内容没有变化");
                return;
            }
            if (ctx->op == Op::Edit) editInsert(ctx);
            else proposeInsert(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->anniversaryId);
}

void revisionTarget(const CtxPtr &ctx) {
    std::string requested = ctx->anniversaryId;
    ctx->scope->tx().execSqlAsync(
        kTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["anniversary_id"].isNull()) { reject(ctx, Outcome::NotFound, "纪念日不存在"); return; }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->anniversaryId = rows[0]["anniversary_id"].as<std::string>();  // canonical form
                ctx->creatorId = rows[0]["creator_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = ctx->op == Op::Edit
                                   ? intent::fingerprint(AnniversaryService::editFingerprint(
                                                             ctx->anniversaryId, ctx->expectedVersion, ctx->terms),
                                                         ctx->choice)
                                   : AnniversaryService::proposeFingerprint(ctx->anniversaryId,
                                                                            ctx->expectedVersion, ctx->terms);
            lockThen(ctx, kAnniversaryLockSql, ctx->anniversaryId,
                     [ctx]() { classify(ctx, [ctx]() { revisionChecks(ctx); }); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, requested);
}

// ---- decide ----
void decisionInsert(const CtxPtr &ctx) {
    std::string outcome = AnniversaryService::outcomeName(ctx->decision);
    ctx->scope->tx().execSqlAsync(
        kDecisionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                reclassify(ctx, [ctx]() { reject(ctx, Outcome::ProposalDecided, "这条修改申请已经处理过了"); });
                return;
            }
            ctx->ref = ctx->proposalId;
            ctx->ref2.clear();
            auto finish = [ctx](int version) {
                Json::Value detail(Json::objectValue);
                detail["anniversary_id"] = ctx->anniversaryId;
                detail["proposal_id"] = ctx->proposalId;
                detail["outcome"] = AnniversaryService::outcomeName(ctx->decision);
                detail["version"] = version > 0 ? Json::Value(version) : Json::Value(Json::nullValue);
                auditAndCommit(ctx, Outcome::Ok, detail);
            };
            if (ctx->decision != Decision::Approve) {
                finish(0);
                return;
            }
            ctx->scope->tx().execSqlAsync(
                kApproveRevisionSql,
                [ctx, finish](const drogon::orm::Result &revisions) {
                    if (revisions.size() != 1u) { failAt(ctx, "insert_revision", "row_count"); return; }
                    int version = 0;
                    try { version = revisions[0]["version"].as<int>(); }
                    catch (const std::exception &) { failAt(ctx, "insert_revision", "column_format"); return; }
                    finish(version);
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_revision", "db_error"); },
                ctx->proposalId, ctx->userId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_decision", "db_error"); },
        ctx->proposalId, ctx->coupleId, ctx->userId, outcome, ctx->key, ctx->fingerprint);
}

void decisionChecks(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kDecisionStateSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            int current = 0;
            int base = 0;
            bool decided = false;
            try {
                current = rows[0]["current_version"].as<int>();
                base = rows[0]["base_version"].as<int>();
                decided = rows[0]["decided"].as<bool>();
            } catch (const std::exception &) { failAt(ctx, "state", "column_format"); return; }
            if (ctx->decision == Decision::Withdraw ? ctx->userId != ctx->proposerId
                                                    : ctx->userId != ctx->creatorId) {
                reject(ctx, Outcome::Forbidden,
                       ctx->decision == Decision::Withdraw ? "只有提出申请的一方可以撤回" : "只有创建者可以处理修改申请");
                return;
            }
            if (decided) { reject(ctx, Outcome::ProposalDecided, "这条修改申请已经处理过了"); return; }
            if (ctx->decision == Decision::Approve && base != current) {
                reject(ctx, Outcome::ProposalStale, "内容已经更新，这条修改申请不能再同意");
                return;
            }
            decisionInsert(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->proposalId);
}

void decisionTarget(const CtxPtr &ctx) {
    std::string requested = ctx->anniversaryId;
    std::string proposal = ctx->proposalId;
    ctx->scope->tx().execSqlAsync(
        kDecisionTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["anniversary_id"].isNull()) { reject(ctx, Outcome::NotFound, "纪念日不存在"); return; }
            if (rows[0]["proposal_id"].isNull()) {
                reject(ctx, Outcome::ProposalNotFound, "修改申请不存在");
                return;
            }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->anniversaryId = rows[0]["anniversary_id"].as<std::string>();
                ctx->creatorId = rows[0]["creator_id"].as<std::string>();
                ctx->proposalId = rows[0]["proposal_id"].as<std::string>();
                ctx->proposerId = rows[0]["proposer_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint =
                AnniversaryService::decisionFingerprint(ctx->anniversaryId, ctx->proposalId, ctx->decision);
            lockThen(ctx, kAnniversaryLockSql, ctx->anniversaryId,
                     [ctx]() { classify(ctx, [ctx]() { decisionChecks(ctx); }); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, requested, proposal);
}

// ---- pin ----
void pinInsert(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kPinInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                reclassify(ctx, [ctx]() { failAt(ctx, "insert_pin", "conflict_unexplained"); });
                return;
            }
            try { ctx->ref2 = rows[0]["seq"].as<std::string>(); }
            catch (const std::exception &) { failAt(ctx, "insert_pin", "column_format"); return; }
            ctx->ref = ctx->userId;
            Json::Value detail(Json::objectValue);
            detail["anniversary_id"] =
                ctx->anniversaryId.empty() ? Json::Value(Json::nullValue) : Json::Value(ctx->anniversaryId);
            detail["seq"] = std::stoi(ctx->ref2);
            auditAndCommit(ctx, Outcome::Ok, detail);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert_pin", "db_error"); },
        ctx->userId, ctx->coupleId, ctx->anniversaryId, ctx->key, ctx->fingerprint);
}

void pinTarget(const CtxPtr &ctx) {
    std::string requested = ctx->clearPin ? std::string() : ctx->anniversaryId;
    ctx->scope->tx().execSqlAsync(
        kPinTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (!ctx->clearPin && rows[0]["anniversary_id"].isNull()) {
                reject(ctx, Outcome::NotFound, "纪念日不存在");
                return;
            }
            try {
                ctx->userId = rows[0]["user_id"].as<std::string>();  // canonical form for the lock
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->anniversaryId = ctx->clearPin ? std::string() : rows[0]["anniversary_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = AnniversaryService::pinFingerprint(
                ctx->clearPin ? std::nullopt : std::optional<std::string>(ctx->anniversaryId));
            // Replay returns the original pin facts and never appends or restores a pin.
            lockThen(ctx, kPinLockSql, ctx->userId, [ctx]() { classify(ctx, [ctx]() { pinInsert(ctx); }); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, requested);
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
                    complete(strong, makeStatus(Outcome::ResultUnknown, "", label(strong->op, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(Outcome::Error, "", label(strong->op, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "AnniversaryService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

bool startable(Op op, const AnniversaryService::Callback &cb) {
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(op, "pool", "not_ready")));
        return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

AnniversaryService &AnniversaryService::instance() {
    static AnniversaryService service;
    return service;
}

bool AnniversaryService::parseCreateRequest(const Json::Value &body, Terms &terms, std::string &message) {
    if (!onlyFields(body, {"title", "date", "kind"}, {"note"}, message)) return false;
    return parseTerms(body, terms, message);
}

bool AnniversaryService::parseRevisionRequest(const Json::Value &body, int &expectedVersion, Terms &terms,
                                              std::string &message) {
    if (!onlyFields(body, {"expected_version", "title", "date", "kind"}, {"note"}, message)) return false;
    if (!isStrictInteger(body["expected_version"]) || body["expected_version"].asInt() < 1) {
        message = "expected_version 必须是正整数";
        return false;
    }
    expectedVersion = body["expected_version"].asInt();
    return parseTerms(body, terms, message);
}

bool AnniversaryService::parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message) {
    if (!onlyFields(body, {"outcome"}, {}, message)) return false;
    const auto &v = body["outcome"];
    if (v.isString() && v.asString() == "approve") decision = Decision::Approve;
    else if (v.isString() && v.asString() == "reject") decision = Decision::Reject;
    else if (v.isString() && v.asString() == "withdraw") decision = Decision::Withdraw;
    else {
        message = "outcome 只能是 approve、reject 或 withdraw";
        return false;
    }
    return true;
}

bool AnniversaryService::parsePinRequest(const Json::Value &body, std::optional<std::string> &anniversaryId,
                                         std::string &message) {
    if (!onlyFields(body, {"anniversary_id"}, {}, message)) return false;
    const auto &v = body["anniversary_id"];
    if (v.isNull()) {
        anniversaryId.reset();
        return true;
    }
    if (!v.isString() || !TaskService::isCanonicalUuid(lower(v.asString()))) {
        message = "anniversary_id 必须是 uuid 或 null";
        return false;
    }
    anniversaryId = lower(v.asString());
    return true;
}

std::string AnniversaryService::createFingerprint(const Terms &terms) {
    return crypto::sha256Hex("anniversary-create:v1:" + termsCanonical(terms));
}

std::string AnniversaryService::editFingerprint(const std::string &anniversaryId, int expectedVersion,
                                                const Terms &terms) {
    return crypto::sha256Hex("anniversary-edit:v1:" + field("anniversary_id", anniversaryId) +
                             field("expected_version", std::to_string(expectedVersion)) + termsCanonical(terms));
}

std::string AnniversaryService::proposeFingerprint(const std::string &anniversaryId, int expectedVersion,
                                                   const Terms &terms) {
    return crypto::sha256Hex("anniversary-propose:v1:" + field("anniversary_id", anniversaryId) +
                             field("expected_version", std::to_string(expectedVersion)) + termsCanonical(terms));
}

std::string AnniversaryService::decisionFingerprint(const std::string &anniversaryId, const std::string &proposalId,
                                                    Decision decision) {
    return crypto::sha256Hex("anniversary-decide:v1:" + field("anniversary_id", anniversaryId) +
                             field("proposal_id", proposalId) + field("outcome", decisionName(decision)));
}

std::string AnniversaryService::pinFingerprint(const std::optional<std::string> &anniversaryId) {
    return crypto::sha256Hex("anniversary-pin:v1:" +
                             (anniversaryId ? field("anniversary_id", *anniversaryId) : std::string("anniversary_id=null;")));
}

const char *AnniversaryService::decisionName(Decision decision) {
    switch (decision) {
        case Decision::Approve: return "approve";
        case Decision::Reject: return "reject";
        case Decision::Withdraw: break;
    }
    return "withdraw";
}

const char *AnniversaryService::outcomeName(Decision decision) {
    switch (decision) {
        case Decision::Approve: return "approved";
        case Decision::Reject: return "rejected";
        case Decision::Withdraw: break;
    }
    return "withdrawn";
}

std::string AnniversaryService::encodeCursor(const Cursor &cursor) {
    return cursor.createdAt + "_" + cursor.id;
}

std::optional<AnniversaryService::Cursor> AnniversaryService::decodeCursor(const std::string &raw) {
    if (raw.size() != 64u || raw[27] != '_') return std::nullopt;
    Cursor cursor{raw.substr(0, 27), raw.substr(28)};
    if (!TaskService::isCursorTimestamp(cursor.createdAt) || !TaskService::isCanonicalUuid(cursor.id)) {
        return std::nullopt;
    }
    return cursor;
}

Json::Value AnniversaryService::computedJson(const Terms &terms, const std::string &asOfDate) {
    Json::Value out(Json::objectValue);
    out["days_since"] = Json::Value(Json::nullValue);
    out["next"] = Json::Value(Json::nullValue);
    out["days_until"] = Json::Value(Json::nullValue);
    const auto date = AnniversaryCalendar::parse(terms.date);
    const auto today = AnniversaryCalendar::parse(asOfDate);
    const auto kind = AnniversaryCalendar::parseKind(terms.kind);
    if (!date || !today || !kind) return out;
    const auto c = AnniversaryCalendar::compute(*kind, *date, *today);
    if (*kind == AnniversaryCalendar::Kind::Countdown) {
        out["days_until"] = static_cast<Json::Int64>(c.daysUntil);
        return out;
    }
    out["days_since"] = static_cast<Json::Int64>(c.daysSince);
    Json::Value next(Json::objectValue);
    next["date"] = AnniversaryCalendar::format(c.next);
    next["years"] = c.years;
    next["days_until"] = static_cast<Json::Int64>(c.nextDaysUntil);
    out["next"] = next;
    return out;
}

Json::Value AnniversaryService::anniversaryJson(const AnniversaryData &a, const std::string &asOfDate) {
    Json::Value out(Json::objectValue);
    out["id"] = a.id;
    out["creator_id"] = a.creatorId;
    out["version"] = a.version;
    out["title"] = a.terms.title;
    out["date"] = a.terms.date;
    out["kind"] = a.terms.kind;
    out["note"] = a.terms.note;
    out["created_at"] = a.createdAt;
    out["updated_at"] = a.updatedAt;
    out["pinned_by_me"] = a.pinnedByMe;
    out["computed"] = computedJson(a.terms, asOfDate);
    if (a.pending) {
        Json::Value p(Json::objectValue);
        p["id"] = a.pending->id;
        p["proposer_id"] = a.pending->proposerId;
        p["base_version"] = a.pending->baseVersion;
        p["title"] = a.pending->terms.title;
        p["date"] = a.pending->terms.date;
        p["kind"] = a.pending->terms.kind;
        p["note"] = a.pending->terms.note;
        p["created_at"] = a.pending->createdAt;
        out["pending_proposal"] = p;
    } else {
        out["pending_proposal"] = Json::Value(Json::nullValue);
    }
    if (a.lastDecision) {
        Json::Value d(Json::objectValue);
        d["proposal_id"] = a.lastDecision->proposalId;
        d["proposer_id"] = a.lastDecision->proposerId;
        d["outcome"] = a.lastDecision->outcome;
        d["decided_by"] = a.lastDecision->decidedBy;
        d["decided_at"] = a.lastDecision->decidedAt;
        out["last_decision"] = d;
    } else {
        out["last_decision"] = Json::Value(Json::nullValue);
    }
    return out;
}

Json::Value AnniversaryService::pinJson(const PinData &pin) {
    Json::Value out(Json::objectValue);
    out["anniversary_id"] = pin.anniversaryId ? Json::Value(*pin.anniversaryId) : Json::Value(Json::nullValue);
    out["seq"] = pin.seq;
    out["updated_at"] = pin.updatedAt.empty() ? Json::Value(Json::nullValue) : Json::Value(pin.updatedAt);
    return out;
}

Json::Value AnniversaryService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["mutation"] = status.mutation;
    if (status.hasNotification) body["notification"] = status.notification;
    if (status.isPin) {
        body["pin"] = pinJson(status.pin);
    } else {
        body["as_of_date"] = status.asOfDate;
        body["anniversary"] = anniversaryJson(status.data, status.asOfDate);
    }
    return body;
}

ErrorCode AnniversaryService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::NotFound: return ErrorCode::AnniversaryNotFound;
        case Outcome::Forbidden: return ErrorCode::AnniversaryEditForbidden;
        case Outcome::VersionConflict: return ErrorCode::AnniversaryVersionConflict;
        case Outcome::ProposalPending: return ErrorCode::AnniversaryProposalPending;
        case Outcome::ProposalNotFound: return ErrorCode::AnniversaryProposalNotFound;
        case Outcome::ProposalDecided: return ErrorCode::AnniversaryProposalDecided;
        case Outcome::ProposalStale: return ErrorCode::AnniversaryProposalStale;
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

int AnniversaryService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Created) return 201;
    if (outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void AnniversaryService::create(const std::string &userId, const std::string &key, const Terms &terms,
                                Callback cb, intent::Choice choice) {
    if (userId.empty() || terms.title.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Op::Create, "input", "invalid_request")));
        return;
    }
    if (!startable(Op::Create, cb)) return;
    auto ctx = std::make_shared<Ctx>();
    ctx->op = Op::Create;
    ctx->userId = userId;
    ctx->key = key;
    ctx->terms = terms;
    ctx->fingerprint = intent::fingerprint(createFingerprint(terms), choice);
    ctx->choice = choice;
    ctx->done = std::move(cb);
    run(ctx, &createMembership);
}

void AnniversaryService::edit(const std::string &userId, const std::string &anniversaryId, const std::string &key,
                              int expectedVersion, const Terms &terms, Callback cb, intent::Choice choice) {
    if (userId.empty() || anniversaryId.empty() || expectedVersion < 1 || terms.title.empty() ||
        !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Op::Edit, "input", "invalid_request")));
        return;
    }
    if (!startable(Op::Edit, cb)) return;
    auto ctx = std::make_shared<Ctx>();
    ctx->op = Op::Edit;
    ctx->userId = userId;
    ctx->anniversaryId = anniversaryId;
    ctx->key = key;
    ctx->expectedVersion = expectedVersion;
    ctx->terms = terms;
    ctx->choice = choice;
    ctx->done = std::move(cb);
    run(ctx, &revisionTarget);
}

void AnniversaryService::propose(const std::string &userId, const std::string &anniversaryId,
                                 const std::string &key, int expectedVersion, const Terms &terms, Callback cb) {
    if (userId.empty() || anniversaryId.empty() || expectedVersion < 1 || terms.title.empty() ||
        !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Op::Propose, "input", "invalid_request")));
        return;
    }
    if (!startable(Op::Propose, cb)) return;
    auto ctx = std::make_shared<Ctx>();
    ctx->op = Op::Propose;
    ctx->userId = userId;
    ctx->anniversaryId = anniversaryId;
    ctx->key = key;
    ctx->expectedVersion = expectedVersion;
    ctx->terms = terms;
    ctx->done = std::move(cb);
    run(ctx, &revisionTarget);
}

void AnniversaryService::decide(const std::string &userId, const std::string &anniversaryId,
                                const std::string &proposalId, const std::string &key, Decision decision,
                                Callback cb) {
    if (userId.empty() || anniversaryId.empty() || proposalId.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Op::Decide, "input", "invalid_request")));
        return;
    }
    if (!startable(Op::Decide, cb)) return;
    auto ctx = std::make_shared<Ctx>();
    ctx->op = Op::Decide;
    ctx->userId = userId;
    ctx->anniversaryId = anniversaryId;
    ctx->proposalId = proposalId;
    ctx->key = key;
    ctx->decision = decision;
    ctx->done = std::move(cb);
    run(ctx, &decisionTarget);
}

void AnniversaryService::pin(const std::string &userId, const std::string &key,
                             const std::optional<std::string> &anniversaryId, Callback cb) {
    if (userId.empty() || !TaskService::validIdempotencyKey(key) || (anniversaryId && anniversaryId->empty())) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Op::Pin, "input", "invalid_request")));
        return;
    }
    if (!startable(Op::Pin, cb)) return;
    auto ctx = std::make_shared<Ctx>();
    ctx->op = Op::Pin;
    ctx->userId = userId;
    ctx->key = key;
    ctx->clearPin = !anniversaryId;
    ctx->anniversaryId = anniversaryId.value_or("");
    ctx->done = std::move(cb);
    run(ctx, &pinTarget);
}

void AnniversaryService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor,
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
        early.detail = TaskService::failureDetail("anniversary_list", "pool", "not_ready");
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
            result.asOfDate = rows[0]["as_of_date"].as<std::string>();
            for (const auto &row : rows) {
                if (row["id"].isNull()) continue;  // a space without (more) items
                result.anniversaries.push_back(anniversaryFromRow(row));
            }
        } catch (const std::exception &) {
            result = ListResult{};
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("anniversary_list", "query", "column_format");
            (*done)(std::move(result));
            return;
        }
        // limit + 1 rows were requested: an extra row means another page exists.
        if (result.anniversaries.size() > static_cast<size_t>(limit)) {
            result.anniversaries.resize(static_cast<size_t>(limit));
            const auto &last = result.anniversaries.back();
            result.nextCursor = encodeCursor(Cursor{last.createdAt, last.id});
        }
        result.outcome = Outcome::Ok;
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult result;
        result.outcome = Outcome::Error;
        result.detail = TaskService::failureDetail("anniversary_list", "query", "db_error");
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

void AnniversaryService::get(const std::string &userId, const std::string &anniversaryId, Callback cb) {
    if (userId.empty() || anniversaryId.empty()) {
        cb(makeStatus(Outcome::Validation, "请求参数不合法"));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", TaskService::failureDetail("anniversary_get", "pool", "not_ready")));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { (*done)(makeStatus(Outcome::SpaceNotFound, "尚未加入空间")); return; }
            if (rows.size() != 1u) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("anniversary_get", "query", "row_count")));
                return;
            }
            if (rows[0]["id"].isNull()) { (*done)(makeStatus(Outcome::NotFound, "纪念日不存在")); return; }
            Status status = makeStatus(Outcome::Ok);
            try {
                status.asOfDate = rows[0]["as_of_date"].as<std::string>();
                status.data = anniversaryFromRow(rows[0]);
            } catch (const std::exception &) {
                (*done)(makeStatus(Outcome::Error, "",
                                   TaskService::failureDetail("anniversary_get", "query", "column_format")));
                return;
            }
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("anniversary_get", "query", "db_error")));
        },
        userId, anniversaryId);
}

void AnniversaryService::home(const std::string &userId, HomeCallback cb) {
    HomeResult early;
    if (userId.empty()) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = TaskService::failureDetail("anniversary_home", "pool", "not_ready");
        cb(std::move(early));
        return;
    }
    auto done = std::make_shared<HomeCallback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kHomeSql,
        [done](const drogon::orm::Result &rows) {
            HomeResult result;
            if (rows.size() == 0u) {
                result.outcome = Outcome::SpaceNotFound;
                result.message = "尚未加入空间";
                (*done)(std::move(result));
                return;
            }
            std::vector<AnniversaryData> items;
            std::vector<AnniversaryCalendar::Candidate> candidates;
            std::optional<AnniversaryCalendar::Civil> today;
            try {
                result.asOfDate = rows[0]["as_of_date"].as<std::string>();
                today = AnniversaryCalendar::parse(result.asOfDate);
                for (const auto &row : rows) {
                    if (row["id"].isNull()) continue;
                    items.push_back(anniversaryFromRow(row));
                    const auto &a = items.back();
                    const auto date = AnniversaryCalendar::parse(a.terms.date);
                    const auto kind = AnniversaryCalendar::parseKind(a.terms.kind);
                    if (!date || !kind) throw std::runtime_error("row");
                    candidates.push_back({a.id, *kind, *date, a.createdAt, a.pinnedByMe});
                }
            } catch (const std::exception &) {
                result = HomeResult{};
                result.outcome = Outcome::Error;
                result.detail = TaskService::failureDetail("anniversary_home", "query", "column_format");
                (*done)(std::move(result));
                return;
            }
            if (!today) {
                result = HomeResult{};
                result.outcome = Outcome::Error;
                result.detail = TaskService::failureDetail("anniversary_home", "query", "as_of_format");
                (*done)(std::move(result));
                return;
            }
            const auto selection = AnniversaryCalendar::select(candidates, *today);
            result.source = AnniversaryCalendar::sourceName(selection.source);
            if (selection.index) result.anniversary = items[*selection.index];
            result.outcome = Outcome::Ok;
            (*done)(std::move(result));
        },
        [done](const drogon::orm::DrogonDbException &) {
            HomeResult result;
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("anniversary_home", "query", "db_error");
            (*done)(std::move(result));
        },
        userId);
}

}  // namespace usapp
