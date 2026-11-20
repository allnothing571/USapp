// =====================================================================
// services/api/src/services/NotificationIntent.cc
// =====================================================================
#include "services/NotificationIntent.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "core/Crypto.h"

namespace usapp::intent {

namespace {

// Source facts of the operation: space, actor and the partner at the time (NULL without one).
const char *kSourceSql =
    "SELECT couple_id::text AS couple_id, actor_id::text AS actor_id, recipient_id::text AS recipient_id "
    "FROM usapp_notification_optional_source($1, $2::uuid, NULLIF($3, '')::integer)";

const char *kNoticeSql =
    "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, "
    "task_id, anniversary_id, anniversary_version, message_id, message_reply_id, memory_id, shared_plan_id, album_batch_id) "
    "SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, "
    "CASE WHEN $1 = 'task' THEN $2::uuid END, "
    "CASE WHEN $1 = 'anniversary_revision' THEN $2::uuid END, "
    "CASE WHEN $1 = 'anniversary_revision' THEN NULLIF($3, '')::integer END, "
    "CASE WHEN $1 = 'message' THEN $2::uuid END, "
    "CASE WHEN $1 = 'message_reply' THEN $2::uuid END, "
    "CASE WHEN $1 = 'memory' THEN $2::uuid END, "
    "CASE WHEN $1 = 'shared_plan' THEN $2::uuid END, "
    "CASE WHEN $1 = 'album_batch' THEN $2::uuid END "
    "FROM usapp_notification_optional_source($1, $2::uuid, NULLIF($3, '')::integer) "
    "WHERE recipient_id IS NOT NULL RETURNING id::text AS id";

// Serializes the remembered-choice sequence per (actor, operation); taken after domain locks.
const char *kPreferenceLockSql = "SELECT pg_advisory_xact_lock(33012::integer, hashtext($1 || ':' || $2))";

// $1 couple, $2 actor, $3 operation, $4 task, $5 anniversary, $6 version, $7 message, $8 reply,
// $9 explicit, $10 requested, $11 recipient, $12 notification, $13 allocate preference, $14 memory, $15 shared plan, $16 album batch.
const char *kIntentSql =
    "INSERT INTO notification_intents (couple_id, actor_id, operation, task_id, anniversary_id, "
    "anniversary_version, message_id, message_reply_id, choice_explicit, requested, recipient_id, "
    "notification_id, preference_seq, memory_id, shared_plan_id, album_batch_id) "
    "SELECT $1::uuid, $2::uuid, $3::text, NULLIF($4, '')::uuid, NULLIF($5, '')::uuid, NULLIF($6, '')::integer, "
    "NULLIF($7, '')::uuid, NULLIF($8, '')::uuid, $9::boolean, $10::boolean, NULLIF($11, '')::uuid, "
    "NULLIF($12, '')::uuid, CASE WHEN $13::boolean THEN (SELECT COALESCE(max(x.preference_seq), 0) + 1 "
    "FROM notification_intents x WHERE x.actor_id = $2::uuid AND x.operation = $3::text "
    "AND x.preference_seq IS NOT NULL) END, NULLIF($14, '')::uuid, NULLIF($15, '')::uuid, NULLIF($16, '')::uuid "
    "RETURNING id::text AS id";

const char *kSourcePredicateProbeSql =
    "SELECT f.operation IS NOT NULL AS found, f.operation IS NOT DISTINCT FROM $4::text AS operation_match, "
    "f.couple_id IS NOT DISTINCT FROM $5::uuid AS couple_match, f.actor_id IS NOT DISTINCT FROM $6::uuid AS actor_match, "
    "f.recipient_id IS NOT DISTINCT FROM NULLIF($7, '')::uuid AS recipient_match, "
    "COALESCE((extract(epoch FROM clock_timestamp() - f.occurred_at) * 1000000)::bigint, 0) AS source_age_us, "
    "clock_timestamp()::text AS probed_at "
    "FROM (SELECT 1) one LEFT JOIN usapp_notification_optional_source($1, $2::uuid, NULLIF($3, '')::integer) f ON true";
const char *kClockSinceProbeSql = "SELECT (extract(epoch FROM clock_timestamp() - $1::timestamptz) * 1000000)::bigint AS us";

#define USAPP_INTENT_BY(column) \
    "SELECT requested, (recipient_id IS NOT NULL) AS has_recipient, notification_id::text AS notification_id " \
    "FROM notification_intents WHERE " column

const char *kByTaskSql = USAPP_INTENT_BY("task_id = $1::uuid");
const char *kByRevisionSql = USAPP_INTENT_BY("anniversary_id = $1::uuid AND anniversary_version = $2::integer");
const char *kByMessageSql = USAPP_INTENT_BY("message_id = $1::uuid");
const char *kByReplySql = USAPP_INTENT_BY("message_reply_id = $1::uuid");
const char *kByMemorySql = USAPP_INTENT_BY("memory_id = $1::uuid");
const char *kBySharedPlanSql = USAPP_INTENT_BY("shared_plan_id = $1::uuid");
const char *kByAlbumBatchSql = USAPP_INTENT_BY("album_batch_id = $1::uuid");

#undef USAPP_INTENT_BY

const char *kChoicesSql =
    "SELECT o.operation, i.requested AS enabled, i.preference_seq "
    "FROM (VALUES ('task_publish'), ('anniversary_create'), ('anniversary_edit'), ('message_create'), "
    "('message_reply')) o(operation) "
    "LEFT JOIN LATERAL (SELECT x.requested, x.preference_seq FROM notification_intents x "
    "WHERE x.actor_id = $1::uuid AND x.operation = o.operation AND x.preference_seq IS NOT NULL "
    "ORDER BY x.preference_seq DESC LIMIT 1) i ON true "
    "WHERE EXISTS (SELECT 1 FROM couple_members m WHERE m.user_id = $1::uuid)";

const char *familyOf(Operation operation) {
    switch (operation) {
        case Operation::TaskPublish: return "task";
        case Operation::AnniversaryCreate:
        case Operation::AnniversaryEdit: return "anniversary_revision";
        case Operation::MessageCreate: return "message";
        case Operation::MemoryCreate: return "memory";
        case Operation::SharedPlanCreate: return "shared_plan";
        case Operation::AlbumPhotosAdd: return "album_batch";
        case Operation::MessageReply: break;
    }
    return "message_reply";
}

std::string sourceIdOf(Operation operation, const Source &s) {
    switch (operation) {
        case Operation::TaskPublish: return s.taskId;
        case Operation::AnniversaryCreate:
        case Operation::AnniversaryEdit: return s.anniversaryId;
        case Operation::MessageCreate: return s.messageId;
        case Operation::MemoryCreate: return s.memoryId;
        case Operation::SharedPlanCreate: return s.sharedPlanId;
        case Operation::AlbumPhotosAdd: return s.albumBatchId;
        case Operation::MessageReply: break;
    }
    return s.replyMessageId;
}

bool isRevision(Operation operation) {
    return operation == Operation::AnniversaryCreate || operation == Operation::AnniversaryEdit;
}

std::string versionOf(Operation operation, const Source &s) {
    return isRevision(operation) ? std::to_string(s.anniversaryVersion) : std::string();
}

struct RecordCtx {
    std::shared_ptr<db::TxScope> scope;
    Operation operation = Operation::TaskPublish;
    Source source;
    Choice choice;
    std::string coupleId;
    std::string actorId;
    std::string recipientId;
    std::string notificationId;
    FactsCallback next;
    std::function<void()> failed;
    SourceFailureSink sink;  // empty in production
    SourcePredicates probe;
    std::string probedAt;
};

std::mutex &sinkMutex() {
    static std::mutex mutex;
    return mutex;
}
SourceFailureSink &sinkSlot() {
    static SourceFailureSink sink;
    return sink;
}

// The transaction is over, so this asks the pool for the clock. Whatever the answer, the write still fails as it would have.
void reportSourceFailure(const std::shared_ptr<RecordCtx> &c) {
    auto report = [c](bool known, long long us) {
        c->probe.clockAfterProbeUs = known ? us : 0;
        c->sink(describeSourceFailure(operationName(c->operation), c->probe) + (known ? "" : " clock_after_probe=unread"));
        c->failed();
    };
    db::Pool::instance().client()->execSqlAsync(
        kClockSinceProbeSql,
        [report](const drogon::orm::Result &rows) {
            try {
                report(true, rows.at(0)["us"].as<long long>());
            } catch (const std::exception &) {
                report(false, 0);
            }
        },
        [report](const drogon::orm::DrogonDbException &) { report(false, 0); }, c->probedAt);
}

void writeIntent(const std::shared_ptr<RecordCtx> &c) {
    const bool hasRecipient = !c->recipientId.empty();
    const bool remember = c->choice.provided && hasRecipient && remembersChoice(c->operation);
    std::string op = operationName(c->operation);
    std::string task = c->operation == Operation::TaskPublish ? c->source.taskId : std::string();
    std::string anniversary = isRevision(c->operation) ? c->source.anniversaryId : std::string();
    std::string version = versionOf(c->operation, c->source);
    std::string message = c->operation == Operation::MessageCreate ? c->source.messageId : std::string();
    std::string reply = c->operation == Operation::MessageReply ? c->source.replyMessageId : std::string();
    std::string explicitChoice = c->choice.provided ? "true" : "false";
    std::string requested = c->choice.requested() ? "true" : "false";
    std::string allocate = remember ? "true" : "false";
    std::string memory = c->operation == Operation::MemoryCreate ? c->source.memoryId : std::string();
    std::string plan = c->operation == Operation::SharedPlanCreate ? c->source.sharedPlanId : std::string();
    std::string batch = c->operation == Operation::AlbumPhotosAdd ? c->source.albumBatchId : std::string();
    c->scope->tx().execSqlAsync(
        kIntentSql,
        [c, hasRecipient](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { c->failed(); return; }
            c->next(facts(c->choice.requested(), hasRecipient, c->notificationId));
        },
        [c](const drogon::orm::DrogonDbException &failure) {
            if (c->sink && std::string(failure.base().what()).find("USAPP_NOTIFICATION_INTENT_SOURCE") != std::string::npos) {
                reportSourceFailure(c);
                return;
            }
            c->failed();
        },
        c->coupleId, c->actorId, op, task, anniversary, version, message, reply, explicitChoice, requested,
        c->recipientId, c->notificationId, allocate, memory, plan, batch);
}

void insertIntent(const std::shared_ptr<RecordCtx> &c) {
    if (!c->sink) {
        writeIntent(c);
        return;
    }
    c->scope->tx().execSqlAsync(
        kSourcePredicateProbeSql,
        [c](const drogon::orm::Result &rows) {
            try {
                const auto &row = rows.at(0);
                c->probe.found = row["found"].as<bool>();
                c->probe.operation = row["operation_match"].as<bool>();
                c->probe.couple = row["couple_match"].as<bool>();
                c->probe.actor = row["actor_match"].as<bool>();
                c->probe.recipient = row["recipient_match"].as<bool>();
                c->probe.sourceAgeUs = row["source_age_us"].as<long long>();
                c->probedAt = row["probed_at"].as<std::string>();
            } catch (const std::exception &) {
                c->failed();
                return;
            }
            writeIntent(c);
        },
        [c](const drogon::orm::DrogonDbException &) { c->failed(); }, std::string(familyOf(c->operation)), sourceIdOf(c->operation, c->source),
        versionOf(c->operation, c->source), std::string(operationName(c->operation)), c->coupleId, c->actorId, c->recipientId);
}

void lockThenInsert(const std::shared_ptr<RecordCtx> &c) {
    if (!(c->choice.provided && !c->recipientId.empty() && remembersChoice(c->operation))) {
        insertIntent(c);
        return;
    }
    std::string op = operationName(c->operation);
    c->scope->tx().execSqlAsync(
        kPreferenceLockSql, [c](const drogon::orm::Result &) { insertIntent(c); },
        [c](const drogon::orm::DrogonDbException &) { c->failed(); }, c->actorId, op);
}

void readIntent(const std::shared_ptr<db::TxScope> &scope, const char *sql, std::vector<std::string> params,
                FactsCallback next, std::function<void()> failed) {
    auto onRows = [next, failed](const drogon::orm::Result &rows) {
        if (rows.size() == 0u) { next(Json::Value(Json::nullValue)); return; }
        if (rows.size() != 1u) { failed(); return; }
        try {
            next(facts(rows[0]["requested"].as<bool>(), rows[0]["has_recipient"].as<bool>(),
                       rows[0]["notification_id"].isNull() ? std::string()
                                                           : rows[0]["notification_id"].as<std::string>()));
        } catch (const std::exception &) { failed(); }
    };
    auto onError = [failed](const drogon::orm::DrogonDbException &) { failed(); };
    if (params.size() == 2u) {
        scope->tx().execSqlAsync(sql, std::move(onRows), std::move(onError), params[0], params[1]);
    } else {
        scope->tx().execSqlAsync(sql, std::move(onRows), std::move(onError), params[0]);
    }
}

}  // namespace

const char *operationName(Operation operation) {
    switch (operation) {
        case Operation::TaskPublish: return "task_publish";
        case Operation::AnniversaryCreate: return "anniversary_create";
        case Operation::AnniversaryEdit: return "anniversary_edit";
        case Operation::MessageCreate: return "message_create";
        case Operation::MemoryCreate: return "memory_create";
        case Operation::SharedPlanCreate: return "shared_plan_create";
        case Operation::AlbumPhotosAdd: return "album_photos_add";
        case Operation::MessageReply: break;
    }
    return "message_reply";
}

bool remembersChoice(Operation operation) {
    return operation == Operation::TaskPublish || operation == Operation::AnniversaryCreate || operation == Operation::AnniversaryEdit ||
           operation == Operation::MessageCreate || operation == Operation::MessageReply;
}

void setSourceFailureSink(SourceFailureSink sink) {
    std::lock_guard<std::mutex> guard(sinkMutex());
    sinkSlot() = std::move(sink);
}

const char *sourcePredicateProbeSql() { return kSourcePredicateProbeSql; }

std::string describeSourceFailure(const char *operation, const SourcePredicates &p) {
    const char *failed = !p.found ? "source_found"
                         : !p.operation ? "operation"
                         : !p.couple ? "couple"
                         : !p.actor ? "actor"
                         : !p.recipient ? "recipient"
                         : p.sourceAgeUs < 0 ? "time_created_before_source"
                         : p.clockAfterProbeUs < 0 ? "time_clock_regressed"
                                                   : "time_unresolved";
    return std::string("failed_predicate=") + failed + " operation=" + operation + " source_found=" + (p.found ? "true" : "false") +
           " operation_match=" + (p.operation ? "true" : "false") + " couple_match=" + (p.couple ? "true" : "false") + " actor_match=" + (p.actor ? "true" : "false") +
           " recipient_match=" + (p.recipient ? "true" : "false") + " source_age_us=" + std::to_string(p.sourceAgeUs) + " clock_after_probe_us=" + std::to_string(p.clockAfterProbeUs);
}

bool takeChoice(Json::Value &body, Choice &out, std::string &message) {
    out = Choice{};
    if (!body.isObject() || !body.isMember("notify_partner")) return true;
    const Json::Value value = body["notify_partner"];
    if (!value.isBool()) {
        message = "notify_partner 必须是布尔值";
        return false;
    }
    out.provided = true;
    out.value = value.asBool();
    body.removeMember("notify_partner");
    return true;
}

std::string fingerprint(const std::string &legacy, const Choice &choice) {
    if (!choice.requested()) return legacy;
    return crypto::sha256Hex("notify-partner:v1:true:" + legacy);
}

Json::Value facts(bool requested, bool hasRecipient, const std::string &notificationId) {
    Json::Value out(Json::objectValue);
    out["requested"] = requested;
    out["created"] = !notificationId.empty();
    if (!notificationId.empty()) out["reason"] = "delivered_to_inbox";
    else if (requested && !hasRecipient) out["reason"] = "no_partner";
    else out["reason"] = "not_requested";
    out["notification_id"] = notificationId.empty() ? Json::Value(Json::nullValue) : Json::Value(notificationId);
    return out;
}

void record(const std::shared_ptr<db::TxScope> &scope, Operation operation, const Source &source,
            const Choice &choice, FactsCallback next, std::function<void()> failed) {
    auto c = std::make_shared<RecordCtx>();
    c->scope = scope;
    c->operation = operation;
    c->source = source;
    c->choice = choice;
    c->next = std::move(next);
    c->failed = std::move(failed);
    {
        std::lock_guard<std::mutex> guard(sinkMutex());
        c->sink = sinkSlot();
    }
    std::string family = familyOf(operation);
    std::string id = sourceIdOf(operation, source);
    std::string version = versionOf(operation, source);
    scope->tx().execSqlAsync(
        kSourceSql,
        [c, family, id, version](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { c->failed(); return; }
            try {
                c->coupleId = rows[0]["couple_id"].as<std::string>();
                c->actorId = rows[0]["actor_id"].as<std::string>();
                c->recipientId = rows[0]["recipient_id"].isNull() ? std::string()
                                                                   : rows[0]["recipient_id"].as<std::string>();
            } catch (const std::exception &) { c->failed(); return; }
            if (!c->choice.requested() || c->recipientId.empty()) {
                lockThenInsert(c);
                return;
            }
            // Notice first: the intent's guard and composite FK verify this exact link.
            c->scope->tx().execSqlAsync(
                kNoticeSql,
                [c](const drogon::orm::Result &notices) {
                    if (notices.size() != 1u) { c->failed(); return; }
                    try { c->notificationId = notices[0]["id"].as<std::string>(); }
                    catch (const std::exception &) { c->failed(); return; }
                    lockThenInsert(c);
                },
                [c](const drogon::orm::DrogonDbException &) { c->failed(); }, family, id, version);
        },
        [c](const drogon::orm::DrogonDbException &) { c->failed(); }, family, id, version);
}

void readOriginal(const std::shared_ptr<db::TxScope> &scope, Operation operation, const Source &source,
                  FactsCallback next, std::function<void()> failed) {
    switch (operation) {
        case Operation::TaskPublish:
            readIntent(scope, kByTaskSql, {source.taskId}, std::move(next), std::move(failed));
            return;
        case Operation::AnniversaryCreate:
        case Operation::AnniversaryEdit:
            readIntent(scope, kByRevisionSql, {source.anniversaryId, std::to_string(source.anniversaryVersion)},
                       std::move(next), std::move(failed));
            return;
        case Operation::MessageCreate:
            readIntent(scope, kByMessageSql, {source.messageId}, std::move(next), std::move(failed));
            return;
        case Operation::MessageReply:
            readIntent(scope, kByReplySql, {source.replyMessageId}, std::move(next), std::move(failed));
            return;
        case Operation::MemoryCreate:
            readIntent(scope, kByMemorySql, {source.memoryId}, std::move(next), std::move(failed));
            return;
        case Operation::SharedPlanCreate:
            readIntent(scope, kBySharedPlanSql, {source.sharedPlanId}, std::move(next), std::move(failed));
            return;
        case Operation::AlbumPhotosAdd:
            readIntent(scope, kByAlbumBatchSql, {source.albumBatchId}, std::move(next), std::move(failed));
            return;
    }
}

void readChoices(const std::string &userId, std::function<void(ChoicesResult)> cb) {
    if (userId.empty() || !db::Pool::instance().ready()) {
        cb(ChoicesResult{});
        return;
    }
    auto done = std::make_shared<std::function<void(ChoicesResult)>>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kChoicesSql,
        [done](const drogon::orm::Result &rows) {
            ChoicesResult result;
            if (rows.size() == 0u) {
                result.status = 404;  // not a member of a space
                (*done)(std::move(result));
                return;
            }
            try {
                Json::Value choices(Json::objectValue);
                for (const auto &row : rows) {
                    Json::Value item(Json::objectValue);
                    const bool known = !row["preference_seq"].isNull();
                    item["enabled"] = known && row["enabled"].as<bool>();
                    item["seq"] = known ? Json::Value(row["preference_seq"].as<int>()) : Json::Value(Json::nullValue);
                    choices[row["operation"].as<std::string>()] = item;
                }
                result.body["choices"] = choices;
                result.status = 200;
            } catch (const std::exception &) { result = ChoicesResult{}; }
            (*done)(std::move(result));
        },
        [done](const drogon::orm::DrogonDbException &) { (*done)(ChoicesResult{}); }, userId);
}

}  // namespace usapp::intent
