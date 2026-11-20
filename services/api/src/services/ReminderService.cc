// =====================================================================
// services/api/src/services/ReminderService.cc
// =====================================================================
#include "services/ReminderService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AnniversaryCalendar.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// The immutable facts of one revision r.
#define USAPP_FACT_FIELDS                                                                                       \
    "'action', 'reminder_rule', 'rule_id', r.id, 'seq', r.seq, "                                                \
    "'target_kind', CASE WHEN r.task_id IS NOT NULL THEN 'task' ELSE 'anniversary' END, "                       \
    "'target_id', COALESCE(r.task_id, r.anniversary_id), 'enabled', r.enabled, 'mode', r.mode, "                \
    "'before_minutes', r.before_minutes, 'days_before', r.days_before, "                                        \
    "'at_time', to_char(r.at_time, 'HH24:MI'), 'at_local_date', to_char(r.at_local_date, 'YYYY-MM-DD'), "       \
    "'offsets', to_json(r.days_before_set), 'created_at', " USAPP_TS("r.created_at")

// Facts plus the current derived state; nothing is stored for these extra fields.
#define USAPP_CURRENT_JSON                                                                                      \
    "json_build_object(" USAPP_FACT_FIELDS ", 'origin', r.origin, "                                             \
    "'needs_reset', usapp_reminder_rule_needs_reset(r.id), 'effective_at', "                                    \
    "CASE WHEN r.task_id IS NOT NULL THEN " USAPP_TS("usapp_reminder_task_effective_at(r.owner_id, r.task_id)") " END, " \
    "'offset_effective_at', CASE WHEN r.anniversary_id IS NOT NULL AND r.enabled THEN "                         \
    "(SELECT json_agg(json_build_object('days_before', d, 'effective_at', "                                     \
    USAPP_TS("usapp_reminder_offset_effective_at(r.owner_id, r.anniversary_id, d)") ") ORDER BY d) "            \
    "FROM unnest(r.days_before_set) d) END, "                                                                   \
    "'next_fire_at', " USAPP_TS("CASE WHEN r.task_id IS NOT NULL "                                              \
                                "THEN usapp_reminder_task_next_fire(r.owner_id, r.task_id) "                    \
                                "ELSE usapp_reminder_anniversary_next_fire(r.owner_id, r.anniversary_id) END") ", " \
    "'fired_this_period', CASE WHEN r.task_id IS NOT NULL THEN EXISTS (SELECT 1 FROM reminder_occurrences o "   \
    "WHERE o.owner_id = r.owner_id AND o.task_id = r.task_id AND (o.period_date IS NULL OR o.period_date = "    \
    "(SELECT CASE t.schedule_type WHEN 'weekly' THEN date_trunc('week', x.d)::date ELSE x.d END "               \
    "FROM tasks t, (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d) x WHERE t.id = r.task_id))) END)"

const char *kTaskTargetSql =
    "SELECT m.couple_id::text AS couple_id, t.id::text AS target_id, t.assignee_id::text AS assignee_id, "
    "t.recurrence_id IS NOT NULL AS is_period FROM couple_members m "
    "LEFT JOIN tasks t ON t.couple_id = m.couple_id AND t.id = $2::uuid WHERE m.user_id = $1::uuid FOR SHARE OF m";
const char *kAnniversaryTargetSql =
    "SELECT m.couple_id::text AS couple_id, a.id::text AS target_id FROM couple_members m "
    "LEFT JOIN anniversaries a ON a.couple_id = m.couple_id AND a.id = $2::uuid WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kTaskLockSql = "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))";
const char *kAnniversaryLockSql = "SELECT pg_advisory_xact_lock(33010::integer, hashtext($1::text))";
const char *kRuleLockSql = "SELECT pg_advisory_xact_lock(33017::integer, hashtext($1 || ':' || $2))";

const char *kByKeySql =
    "SELECT id::text AS id, request_fingerprint FROM reminder_rules WHERE owner_id = $1::uuid AND idempotency_key = $2";

const char *kProblemSql =
    "SELECT usapp_reminder_task_rule_problem($1::uuid, $2::boolean, NULLIF($3, ''), NULLIF($4, '')::integer, "
    "NULLIF($5, '')::integer, NULLIF($6, '')::time, NULLIF($7, '')::date, clock_timestamp()) AS problem";

const char *kInsertSql =
    "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, anniversary_id, enabled, mode, before_minutes, "
    "days_before, at_time, at_local_date, days_before_set, idempotency_key, request_fingerprint) "
    "SELECT $1::uuid, $2::uuid, COALESCE(max(x.seq), 0) + 1, NULLIF($3, '')::uuid, NULLIF($4, '')::uuid, $5::boolean, "
    "NULLIF($6, ''), NULLIF($7, '')::integer, NULLIF($8, '')::integer, NULLIF($9, '')::time, NULLIF($10, '')::date, "
    "NULLIF($11, '')::smallint[], $12::text, $13::text FROM reminder_rules x WHERE x.owner_id = $2::uuid "
    "AND x.task_id IS NOT DISTINCT FROM NULLIF($3, '')::uuid "
    "AND x.anniversary_id IS NOT DISTINCT FROM NULLIF($4, '')::uuid "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id, seq";

// The audit detail holds ids, seq, enabled and mode only: the reminder time is the owner's private choice.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, 'reminder.rule_saved', $3::text, $4::text, $5::jsonb)";

// One revision's facts and its target's current state; only the owner's own rows.
const char *kReadSql =
    "SELECT json_build_object(" USAPP_FACT_FIELDS ")::text AS mutation_json, "
    "(SELECT " USAPP_CURRENT_JSON " FROM reminder_rules r WHERE r.owner_id = f.owner_id "
    "AND r.task_id IS NOT DISTINCT FROM f.task_id AND r.anniversary_id IS NOT DISTINCT FROM f.anniversary_id "
    "ORDER BY r.seq DESC LIMIT 1)::text AS current_json FROM reminder_rules f "
    "CROSS JOIN LATERAL (SELECT f.*) r WHERE f.id = $1::uuid AND f.owner_id = $2::uuid";

// Each target's first revision gives an immutable page order. One row per entry, or one row with NULL
// columns when there are none; zero rows: no membership.
#define USAPP_LIST_SQL(filter, limit)                                                                           \
    "SELECT m.couple_id::text AS couple_id, p.* FROM couple_members m LEFT JOIN LATERAL ("                      \
    "SELECT " USAPP_TS("head.created_at") " AS sort_created_at, head.id::text AS sort_id, head.created_at AS ca, " \
    "head.id AS fid, (SELECT " USAPP_CURRENT_JSON " FROM reminder_rules r WHERE r.owner_id = head.owner_id "  \
    "AND r.task_id IS NOT DISTINCT FROM head.task_id AND r.anniversary_id IS NOT DISTINCT FROM head.anniversary_id " \
    "ORDER BY r.seq DESC LIMIT 1)::text AS current_json FROM reminder_rules head "                             \
    "WHERE head.owner_id = m.user_id AND head.couple_id = m.couple_id AND head.seq = 1 " filter              \
    " ORDER BY head.created_at DESC, head.id DESC LIMIT " limit ") p ON true "                                \
    "WHERE m.user_id = $1::uuid ORDER BY p.ca DESC, p.fid DESC"

const char *kListFirstSql = USAPP_LIST_SQL("", "$2::integer");
const char *kListAfterSql =
    USAPP_LIST_SQL("AND (head.created_at, head.id) < ($2::timestamptz, $3::uuid)", "$4::integer");

#undef USAPP_LIST_SQL
#undef USAPP_CURRENT_JSON
#undef USAPP_FACT_FIELDS
#undef USAPP_TS

using Outcome = ReminderService::Outcome;
using TargetKind = ReminderService::TargetKind;
using Terms = ReminderService::Terms;

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

bool isStrictInteger(const Json::Value &v) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt();
}

bool onlyFields(const Json::Value &body, std::initializer_list<const char *> required,
                std::initializer_list<const char *> optional, std::string &message) {
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

// The common start of both parsers. True and enabled=false means the body was exactly {enabled:false}.
bool parseEnabled(const Json::Value &body, Terms &terms, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是 JSON 对象";
        return false;
    }
    if (!body["enabled"].isBool()) {
        message = "enabled 必须是布尔值";
        return false;
    }
    terms = Terms{};
    terms.enabled = body["enabled"].asBool();
    if (!terms.enabled) return onlyFields(body, {"enabled"}, {}, message);
    return true;
}

bool parseTime(const Json::Value &value, std::string &out, std::string &message) {
    if (!value.isString() || !ReminderService::validTime(value.asString())) {
        message = "at_time 必须是 HH:MM（北京时间）";
        return false;
    }
    out = value.asString();
    return true;
}

ReminderService::Status makeStatus(Outcome outcome, std::string message = "", std::string detail = "") {
    ReminderService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

// Lifetime: Ctx --(shared)--> TxScope --(onError_, weak)--> Ctx, as in the other services. Every end goes through
// complete() exactly once; commitRequested is set before commit, after which an error is ResultUnknown.
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    TargetKind kind = TargetKind::Task;
    std::string targetId;
    std::string coupleId;
    std::string key;
    std::string fingerprint;
    Terms terms;
    std::string ruleId;
    ReminderService::Status result;
    ReminderService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};
using CtxPtr = std::shared_ptr<Ctx>;

std::string label(const char *stage, const char *reason) {
    return TaskService::failureDetail("reminder_rule", stage, reason);
}

void complete(const CtxPtr &ctx, ReminderService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "ReminderService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, ReminderService::Status status) {
    auto shared = std::make_shared<ReminderService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

void reject(const CtxPtr &ctx, Outcome outcome, const char *message) {
    rejectWith(ctx, makeStatus(outcome, message));
}

// The immutable facts of the rule revision and the target's current state, inside the transaction.
void readBack(const CtxPtr &ctx, Outcome outcome, std::function<void(ReminderService::Status)> next) {
    ctx->scope->tx().execSqlAsync(
        kReadSql,
        [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_back", "row_count"); return; }
            auto status = makeStatus(outcome);
            try {
                if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status.mutation) ||
                    !parseObject(rows[0]["current_json"].as<std::string>(), status.current)) {
                    failAt(ctx, "read_back", "json_format");
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "read_back", "column_format"); return; }
            next(std::move(status));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_back", "db_error"); },
        ctx->ruleId, ctx->userId);
}

// Same owner and key: an identical request replays its original facts and appends nothing; a different
// fingerprint is IDEMPOTENCY_KEY_REUSED. Runs before any mutable state check.
void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string stored;
            try {
                ctx->ruleId = rows[0]["id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                return;
            }
            readBack(ctx, Outcome::Replayed, [ctx](ReminderService::Status status) { rejectWith(ctx, std::move(status)); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

std::string text(const std::optional<int> &value) { return value ? std::to_string(*value) : std::string(); }

void insertRule(const CtxPtr &ctx) {
    const bool isTask = ctx->kind == TargetKind::Task;
    ctx->scope->tx().execSqlAsync(
        kRuleLockSql,
        [ctx, isTask](const drogon::orm::Result &) {
            const auto &terms = ctx->terms;
            ctx->scope->tx().execSqlAsync(
                kInsertSql,
                [ctx, isTask](const drogon::orm::Result &rows) {
                    if (rows.size() == 0u) {
                        // A concurrent same-key write committed first.
                        classify(ctx, [ctx]() { failAt(ctx, "insert", "conflict_unexplained"); });
                        return;
                    }
                    int seq = 0;
                    try {
                        ctx->ruleId = rows[0]["id"].as<std::string>();
                        seq = rows[0]["seq"].as<int>();
                    } catch (const std::exception &) { failAt(ctx, "insert", "column_format"); return; }
                    Json::Value detail(Json::objectValue);
                    detail["rule_id"] = ctx->ruleId;
                    detail["seq"] = seq;
                    detail["enabled"] = ctx->terms.enabled;
                    detail["mode"] = ctx->terms.mode.empty() ? Json::Value(Json::nullValue) : Json::Value(ctx->terms.mode);
                    ctx->scope->tx().execSqlAsync(
                        kAuditSql,
                        [ctx](const drogon::orm::Result &) {
                            readBack(ctx, Outcome::Created, [ctx](ReminderService::Status status) {
                                ctx->result = std::move(status);
                                ctx->commitRequested = true;
                                ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
                            });
                        },
                        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
                        ctx->userId, ctx->coupleId, std::string(isTask ? "task" : "anniversary"), ctx->targetId,
                        compactJson(detail));
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
                ctx->coupleId, ctx->userId, isTask ? ctx->targetId : std::string(),
                isTask ? std::string() : ctx->targetId, std::string(terms.enabled ? "true" : "false"), terms.mode,
                text(terms.beforeMinutes), text(terms.daysBefore), terms.atTime, terms.atLocalDate,
                ReminderService::offsetsLiteral(terms.offsets), ctx->key, ctx->fingerprint);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "rule_lock", "db_error"); },
        ctx->userId, ctx->targetId);
}

// Task state and schedule bounds: the guard's own definition, so the error is specific.
void taskChecks(const CtxPtr &ctx) {
    const auto &t = ctx->terms;
    ctx->scope->tx().execSqlAsync(
        kProblemSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            const std::string problem = rows[0]["problem"].isNull() ? std::string() : rows[0]["problem"].as<std::string>();
            if (problem == "closed") {
                reject(ctx, Outcome::TargetClosed, "这个任务已经结束，不能再设置提醒");
            } else if (problem == "invalid") {
                reject(ctx, Outcome::TimeInvalid, "这个提醒时间不适用于该任务");
            } else if (problem == "passed") {
                reject(ctx, Outcome::TimePassed, "这个提醒时间已经过去");
            } else if (!problem.empty()) {
                failAt(ctx, "state", "unknown_problem");
            } else {
                insertRule(ctx);
            }
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->targetId, std::string(t.enabled ? "true" : "false"), t.mode, text(t.beforeMinutes), text(t.daysBefore),
        t.atTime, t.atLocalDate);
}

void start(const CtxPtr &ctx) {
    const bool isTask = ctx->kind == TargetKind::Task;
    std::string requested = ctx->targetId;
    ctx->scope->tx().execSqlAsync(
        isTask ? kTaskTargetSql : kAnniversaryTargetSql,
        [ctx, isTask](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["target_id"].isNull()) {
                if (isTask) reject(ctx, Outcome::TaskNotFound, "任务不存在");
                else reject(ctx, Outcome::AnniversaryNotFound, "纪念日不存在");
                return;
            }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->targetId = rows[0]["target_id"].as<std::string>();  // canonical form
                if (isTask) {
                    if (rows[0]["assignee_id"].as<std::string>() != ctx->userId) {
                        reject(ctx, Outcome::NotAssignee, "只有任务接收者可以设置提醒");
                        return;
                    }
                    if (rows[0]["is_period"].as<bool>()) {
                        reject(ctx, Outcome::TimeInvalid, "请在重复任务本身上设置提醒");
                        return;
                    }
                }
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = ReminderService::fingerprint(ctx->kind, ctx->targetId, ctx->terms);
            ctx->scope->tx().execSqlAsync(
                isTask ? kTaskLockSql : kAnniversaryLockSql,
                [ctx, isTask](const drogon::orm::Result &) {
                    classify(ctx, [ctx, isTask]() {
                        if (isTask) taskChecks(ctx);
                        else insertRule(ctx);
                    });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "lock", "db_error"); }, ctx->targetId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, requested);
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

ReminderService &ReminderService::instance() {
    static ReminderService service;
    return service;
}

bool ReminderService::validTime(const std::string &t) {
    if (t.size() != 5u || t[2] != ':') return false;
    for (size_t i : {0u, 1u, 3u, 4u}) {
        if (t[i] < '0' || t[i] > '9') return false;
    }
    const int hours = (t[0] - '0') * 10 + (t[1] - '0');
    const int minutes = (t[3] - '0') * 10 + (t[4] - '0');
    return hours <= 23 && minutes <= 59;
}

bool ReminderService::parseTaskRequest(const Json::Value &body, Terms &terms, std::string &message) {
    if (!parseEnabled(body, terms, message)) return false;
    if (!terms.enabled) return true;
    if (!body["mode"].isString()) {
        message = "mode 只能是 before_deadline、local_time 或 absolute";
        return false;
    }
    terms.mode = body["mode"].asString();
    if (terms.mode == "before_deadline") {
        if (!onlyFields(body, {"enabled", "mode", "before_minutes"}, {}, message)) return false;
        const auto &v = body["before_minutes"];
        if (!isStrictInteger(v) || v.asInt() < 1 || v.asInt() > 10080) {
            message = "before_minutes 必须是 1 到 10080 的整数";
            return false;
        }
        terms.beforeMinutes = v.asInt();
        return true;
    }
    if (terms.mode == "local_time") {
        if (!onlyFields(body, {"enabled", "mode", "days_before", "at_time"}, {}, message)) return false;
        const auto &v = body["days_before"];
        if (!isStrictInteger(v) || v.asInt() < 0 || v.asInt() > 7) {
            message = "days_before 必须是 0 到 7 的整数";
            return false;
        }
        terms.daysBefore = v.asInt();
        return parseTime(body["at_time"], terms.atTime, message);
    }
    if (terms.mode == "absolute") {
        if (!onlyFields(body, {"enabled", "mode", "at_local_date", "at_time"}, {}, message)) return false;
        const auto &v = body["at_local_date"];
        const auto date = v.isString() ? AnniversaryCalendar::parse(v.asString()) : std::nullopt;
        if (!date || date->year < 1970 || date->year > 9998) {
            message = "at_local_date 必须是有效日期 YYYY-MM-DD";
            return false;
        }
        terms.atLocalDate = AnniversaryCalendar::format(*date);
        return parseTime(body["at_time"], terms.atTime, message);
    }
    message = "mode 只能是 before_deadline、local_time 或 absolute";
    return false;
}

bool ReminderService::parseAnniversaryRequest(const Json::Value &body, Terms &terms, std::string &message) {
    if (!parseEnabled(body, terms, message)) return false;
    if (!terms.enabled) return true;
    if (!onlyFields(body, {"enabled", "days_before"}, {"at_time"}, message)) return false;
    terms.mode = "anniversary_offsets";
    const auto &list = body["days_before"];
    if (!list.isArray() || list.empty() || list.size() > 3u) {
        message = "days_before 必须是 1 到 3 个提前天数";
        return false;
    }
    static const std::set<int> kAllowed{0, 1, 3, 7, 14, 30};
    std::set<int> seen;
    for (const auto &v : list) {
        if (!isStrictInteger(v) || !kAllowed.count(v.asInt()) || !seen.insert(v.asInt()).second) {
            message = "days_before 只能取 0、1、3、7、14、30，且不能重复";
            return false;
        }
    }
    terms.offsets.assign(seen.begin(), seen.end());  // ascending: one set has one spelling
    terms.atTime = "09:00";
    if (body.isMember("at_time")) return parseTime(body["at_time"], terms.atTime, message);
    return true;
}

std::string ReminderService::offsetsLiteral(const std::vector<int> &offsets) {
    if (offsets.empty()) return "";
    std::string out = "{";
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (i > 0) out += ",";
        out += std::to_string(offsets[i]);
    }
    return out + "}";
}

std::string ReminderService::fingerprint(TargetKind kind, const std::string &targetId, const Terms &t) {
    return crypto::sha256Hex("reminder-rule:v1:" + field("kind", kind == TargetKind::Task ? "task" : "anniversary") +
                             field("target", targetId) + field("enabled", t.enabled ? "true" : "false") +
                             field("mode", t.mode) + field("before_minutes", text(t.beforeMinutes)) +
                             field("days_before", text(t.daysBefore)) + field("at_time", t.atTime) +
                             field("at_local_date", t.atLocalDate) + field("offsets", offsetsLiteral(t.offsets)));
}

bool ReminderService::generationEnabled(bool remindersEnabled, bool schedulerEnabled) {
    return remindersEnabled && schedulerEnabled;
}

Json::Value ReminderService::listBody(const ListResult &result, bool generationEnabled) {
    Json::Value body(Json::objectValue);
    Json::Value items(Json::arrayValue);
    for (const auto &item : result.reminders) items.append(item);
    body["reminders"] = items;
    body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
    body["generation_enabled"] = generationEnabled;
    return body;
}

Json::Value ReminderService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["mutation"] = status.mutation;
    body["current"] = status.current;
    return body;
}

ErrorCode ReminderService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::TaskNotFound: return ErrorCode::TaskNotFound;
        case Outcome::AnniversaryNotFound: return ErrorCode::AnniversaryNotFound;
        case Outcome::NotAssignee: return ErrorCode::ReminderNotAssignee;
        case Outcome::TargetClosed: return ErrorCode::ReminderTargetClosed;
        case Outcome::TimeInvalid: return ErrorCode::ReminderTimeInvalid;
        case Outcome::TimePassed: return ErrorCode::ReminderTimePassed;
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

int ReminderService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Created) return 201;
    if (outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void ReminderService::set(const std::string &userId, TargetKind kind, const std::string &targetId,
                          const std::string &key, const Terms &terms, Callback cb) {
    if (userId.empty() || targetId.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label("input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label("pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->userId = userId;
    ctx->kind = kind;
    ctx->targetId = targetId;
    ctx->key = key;
    ctx->terms = terms;
    ctx->done = std::move(cb);
    std::weak_ptr<Ctx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            start(ctx);
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(Outcome::ResultUnknown, "", label("commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(Outcome::Error, "", label("transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "ReminderService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

void ReminderService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    if (userId.empty() || limit < 1 || limit > 100) {
        ListResult r;
        r.outcome = Outcome::Validation;
        r.message = "请求不合法";
        (*done)(std::move(r));
        return;
    }
    if (!db::Pool::instance().ready()) {
        ListResult r;
        r.detail = label("pool", "not_ready");
        (*done)(std::move(r));
        return;
    }
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult r;
        if (rows.size() == 0u) {
            r.outcome = Outcome::SpaceNotFound;
            r.message = "尚未加入空间";
            (*done)(std::move(r));
            return;
        }
        try {
            std::string lastCreatedAt, lastId;
            size_t present = 0;
            for (const auto &row : rows) {
                if (row["current_json"].isNull()) continue;
                ++present;
                // limit + 1 was fetched: the extra row only proves another page exists.
                if (static_cast<int>(present) > limit) continue;
                Json::Value item;
                if (!parseObject(row["current_json"].as<std::string>(), item)) throw std::runtime_error("json");
                r.reminders.push_back(std::move(item));
                lastCreatedAt = row["sort_created_at"].as<std::string>();
                lastId = row["sort_id"].as<std::string>();
            }
            if (static_cast<int>(present) > limit) r.nextCursor = lastCreatedAt + "_" + lastId;
            r.outcome = Outcome::Ok;
        } catch (const std::exception &) {
            r = ListResult{};
            r.detail = label("list", "column_format");
        }
        (*done)(std::move(r));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult r;
        r.detail = label("list", "db_error");
        (*done)(std::move(r));
    };
    const std::string fetch = std::to_string(limit + 1);
    if (cursor) {
        db::Pool::instance().client()->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId,
                                                    cursor->createdAt, cursor->id, fetch);
    } else {
        db::Pool::instance().client()->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId, fetch);
    }
}

}  // namespace usapp
