// =====================================================================
// services/api/src/services/TaskChangeService.cc
// =====================================================================
#include "services/TaskChangeService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <set>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/NotificationWriter.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// Deadline instant of proposed terms: the 0007 rule (date only -> next local midnight).
#define USAPP_TERMS_DEADLINE                                                                          \
    "(CASE WHEN p.deadline_local_date IS NULL THEN NULL "                                             \
    "      WHEN p.deadline_local_time IS NULL "                                                       \
    "      THEN (p.deadline_local_date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' "                 \
    "      ELSE (p.deadline_local_date + p.deadline_local_time) AT TIME ZONE 'Asia/Shanghai' END)"

// Step 1 (propose): caller membership FOR SHARE, joined to a root task of the same space.
const char *kProposeTaskSql =
    "SELECT m.couple_id::text AS couple_id, t.id::text AS task_id, t.creator_id::text AS creator_id, "
    "t.schedule_type FROM couple_members m "
    "LEFT JOIN tasks t ON t.couple_id = m.couple_id AND t.id = $2::uuid AND t.recurrence_id IS NULL "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

// Step 1 (decide): membership FOR SHARE, joined to the proposal of this task and its terms.
const char *kDecideTaskSql =
    "SELECT m.couple_id::text AS couple_id, p.id::text AS proposal_id, p.task_id::text AS task_id, p.kind, "
    "p.proposer_id::text AS proposer_id, t.schedule_type, p.title, p.description, p.reward_flowers, p.reward_sunshine, p.completion_mode, "
    "p.target_count, to_char(p.deadline_local_date, 'YYYY-MM-DD') AS deadline_local_date, "
    "substr(p.deadline_local_time::text, 1, 5) AS deadline_local_time, "
    "to_char(p.start_local_date, 'YYYY-MM-DD') AS start_date "
    "FROM couple_members m "
    "LEFT JOIN task_change_proposals p ON p.couple_id = m.couple_id AND p.id = $3::uuid AND p.task_id = $2::uuid "
    "LEFT JOIN tasks t ON t.id = p.task_id "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

// Step 2: the per-root lock shared with progress, completion and period materialization.
const char *kLockSql = "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))";

const char *kProposalByKeySql =
    "SELECT id::text AS id, request_fingerprint FROM task_change_proposals "
    "WHERE proposer_id = $1::uuid AND idempotency_key = $2";

// Proposal-time state. The proposed dates are validated again at the decision.
const char *kProposeStateSql =
    "SELECT (SELECT k.reason FROM task_closures k WHERE k.task_id = t.id) AS closed_reason, "
    "EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) AS completed, "
    "(t.deadline_at IS NOT NULL AND t.deadline_at <= clock_timestamp()) AS expired, "
    "EXISTS (SELECT 1 FROM task_change_proposals p WHERE p.task_id = t.id "
    "        AND NOT EXISTS (SELECT 1 FROM task_change_decisions d WHERE d.proposal_id = p.id)) AS pending, "
    "(NULLIF($2::text, '') IS NULL OR "
    " (CASE WHEN NULLIF($3::text, '') IS NULL "
    "       THEN (NULLIF($2::text, '')::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' "
    "       ELSE (NULLIF($2::text, '')::date + NULLIF($3::text, '')::time) AT TIME ZONE 'Asia/Shanghai' END) "
    " > clock_timestamp()) AS end_ok, "
    "(NULLIF($4::text, '') IS NULL OR NULLIF($4::text, '')::date IS NOT DISTINCT FROM t.start_local_date "
    " OR NULLIF($4::text, '')::date >= (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date) AS start_ok "
    "FROM tasks t WHERE t.id = $1::uuid";

const char *kProposalInsertSql =
    "INSERT INTO task_change_proposals (id, couple_id, task_id, proposer_id, kind, title, description, "
    "reward_flowers, reward_sunshine, completion_mode, target_count, deadline_local_date, deadline_local_time, "
    "start_local_date, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, "
    "CASE WHEN $5::text = 'edit' THEN $6::text END, CASE WHEN $5::text = 'edit' THEN $7::text END, "
    "NULLIF($8::text, '')::integer, NULLIF($9::text, '')::integer, NULLIF($10::text, ''), "
    "NULLIF($11::text, '')::integer, NULLIF($12::text, '')::date, NULLIF($13::text, '')::time, "
    "NULLIF($14::text, '')::date, $15::text, $16::text) "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

const char *kDecisionByKeySql =
    "SELECT proposal_id::text AS proposal_id, request_fingerprint FROM task_change_decisions "
    "WHERE decided_by = $1::uuid AND idempotency_key = $2";

const char *kDecisionForProposalSql = "SELECT outcome FROM task_change_decisions WHERE proposal_id = $1::uuid";

// decided_at (clock_timestamp at this insert) is the decision time for every later check.
const char *kDecisionInsertSql =
    "INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, successor_task_id, "
    "idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, NULLIF($5::text, '')::uuid, $6::text, $7::text) "
    "ON CONFLICT DO NOTHING RETURNING proposal_id::text AS id";

// Approval state at the decision time. A stored period of a template exists only
// together with its first +1, so its existence means the current period has progress.
const char *kApproveStateSql =
    "SELECT (SELECT k.reason FROM task_closures k WHERE k.task_id = t.id) AS closed_reason, "
    "EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) AS completed, "
    "EXISTS (SELECT 1 FROM task_progress_events e WHERE e.task_id = t.id) AS progressed, "
    "(t.deadline_at IS NOT NULL AND t.deadline_at <= d.decided_at) AS expired, "
    "EXISTS (SELECT 1 FROM tasks i WHERE i.recurrence_id = t.id "
    "        AND d.decided_at >= (i.period_date::timestamp AT TIME ZONE 'Asia/Shanghai') "
    "        AND d.decided_at < i.deadline_at) AS period_started, "
    "(" USAPP_TERMS_DEADLINE " IS NULL OR " USAPP_TERMS_DEADLINE " > d.decided_at) AS end_ok, "
    "(p.start_local_date IS NULL OR p.start_local_date IS NOT DISTINCT FROM t.start_local_date "
    " OR p.start_local_date >= (d.decided_at AT TIME ZONE 'Asia/Shanghai')::date) AS start_ok "
    "FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id "
    "JOIN tasks t ON t.id = p.task_id WHERE d.proposal_id = $1::uuid";

// The successor is an ordinary immutable root task; created_at is the decision time.
const char *kSuccessorSql =
    "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers, "
    "reward_sunshine, timezone, deadline_local_date, deadline_local_time, deadline_at, create_idempotency_key, "
    "create_fingerprint, created_at, completion_mode, target_count, schedule_type, start_local_date) "
    "SELECT d.successor_task_id, t.couple_id, t.creator_id, t.assignee_id, p.title, p.description, "
    "p.reward_flowers, p.reward_sunshine, 'Asia/Shanghai', p.deadline_local_date, p.deadline_local_time, "
    USAPP_TERMS_DEADLINE ", 'chg:' || p.id::text, $2::text, d.decided_at, p.completion_mode, p.target_count, "
    "t.schedule_type, p.start_local_date "
    "FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id "
    "JOIN tasks t ON t.id = p.task_id WHERE d.proposal_id = $1::uuid RETURNING id::text AS id";

// P2-6 R4: the assignee's own latest rule, enabled or not, exactly as it is; nothing from the request. The guard
// verifies every fact again. Zero rows when the assignee has no rule. $2 is 1 except under the test fault.
const char *kRuleCopySql =
    "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, days_before, at_time, "
    "at_local_date, days_before_set, origin, source_rule_id, source_proposal_id) "
    "SELECT r.couple_id, r.owner_id, $2::integer, d.successor_task_id, r.enabled, r.mode, r.before_minutes, r.days_before, "
    "r.at_time, r.at_local_date, r.days_before_set, 'successor', r.id, d.proposal_id "
    "FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id JOIN tasks t ON t.id = p.task_id "
    "CROSS JOIN LATERAL (SELECT * FROM reminder_rules x WHERE x.owner_id = t.assignee_id AND x.task_id = t.id "
    "ORDER BY x.seq DESC LIMIT 1) r "
    "WHERE d.proposal_id = $1::uuid AND d.successor_task_id IS NOT NULL RETURNING id::text AS id";

const char *kClosureInsertSql =
    "INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at) "
    "SELECT p.task_id, p.couple_id, CASE p.kind WHEN 'cancel' THEN 'cancelled' ELSE 'superseded' END, p.id, "
    "d.decided_at FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id "
    "WHERE d.proposal_id = $1::uuid RETURNING task_id::text AS id";

// Audit details contain ids and kinds only, never titles or descriptions.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'task', $4::text, $5::jsonb)";

// Immutable facts: the proposal, plus its decision only for a decision response.
const char *kFactsSql =
    "SELECT json_build_object('id', p.id, 'task_id', p.task_id, 'kind', p.kind, 'proposer_id', p.proposer_id, "
    "'created_at', " USAPP_TS("p.created_at") ", 'title', p.title, 'description', p.description, "
    "'reward_flowers', p.reward_flowers, 'reward_sunshine', p.reward_sunshine, "
    "'completion_mode', p.completion_mode, 'target_count', p.target_count, "
    "'deadline_local_date', to_char(p.deadline_local_date, 'YYYY-MM-DD'), "
    "'deadline_local_time', substr(p.deadline_local_time::text, 1, 5), "
    "'start_date', to_char(p.start_local_date, 'YYYY-MM-DD'))::text AS proposal_json, "
    "(SELECT json_build_object('outcome', d.outcome, 'decided_by', d.decided_by, "
    "'decided_at', " USAPP_TS("d.decided_at") ", 'successor_task_id', d.successor_task_id)::text "
    " FROM task_change_decisions d WHERE d.proposal_id = p.id AND $2::boolean) AS decision_json, "
    "p.task_id::text AS task_id, "
    "(SELECT d.successor_task_id::text FROM task_change_decisions d "
    " WHERE d.proposal_id = p.id AND $2::boolean) AS successor_id "
    "FROM task_change_proposals p WHERE p.id = $1::uuid";

#undef USAPP_TERMS_DEADLINE
#undef USAPP_TS

// Current derived state: the root's current-period row (for a closed template, its last period).
const std::string &currentTaskSql() {
    static const std::string sql = std::string("SELECT ") + TaskService::taskColumnsSql() +
                                   " FROM task_read_model t LEFT JOIN task_completions c ON c.task_id = t.id "
                                   "WHERE t.current_period AND COALESCE(t.recurrence_id, t.id) = $1::uuid";
    return sql;
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

TaskChangeService::Status makeStatus(TaskChangeService::Outcome outcome, std::string message = "",
                                     std::string detail = "") {
    TaskChangeService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

std::string label(bool decide, const char *stage, const char *reason) {
    return TaskService::failureDetail(decide ? "task_change_decide" : "task_change_propose", stage, reason);
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

// ---------------------------------------------------------------------
// Lifetime: ChangeCtx --(shared)--> TxScope --(onError_, weak)--> ChangeCtx, as in
// TaskService/TaskCompletionService. Every end goes through complete() exactly once;
// commitRequested is set before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
struct ChangeCtx {
    std::shared_ptr<db::TxScope> scope;
    bool decide = false;
    std::string userId;
    std::string taskId;
    std::string proposalId;
    std::string key;
    std::string fingerprint;
    std::string coupleId;
    TaskChangeService::ProposeInput input;  // propose
    TaskChangeService::Action action = TaskChangeService::Action::Reject;
    std::string kind;          // decide: the proposal kind
    std::string scheduleType;  // the root task's schedule type
    std::string proposerId;
    std::string successorId;
    std::string successorFingerprint;
    std::string copiedRuleId;
    TaskChangeService::Fault fault = TaskChangeService::Fault::None;
    TaskChangeService::Status result;
    TaskChangeService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<ChangeCtx>;

void complete(const CtxPtr &ctx, TaskChangeService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "TaskChangeService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->decide, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, TaskChangeService::Status status) {
    auto shared = std::make_shared<TaskChangeService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

using FactsCallback = std::function<void(TaskChangeService::Status)>;

// Reads one current task row (root or successor) inside the transaction.
void readCurrent(const CtxPtr &ctx, const std::string &id,
                 std::function<void(TaskService::TaskData)> next) {
    ctx->scope->tx().execSqlAsync(
        currentTaskSql(),
        [ctx, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_task", "row_count"); return; }
            TaskService::TaskData task;
            try { task = TaskService::taskFromRow(rows[0]); }
            catch (const std::exception &) { failAt(ctx, "read_task", "column_format"); return; }
            next(std::move(task));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_task", "db_error"); },
        id);
}

// Immutable change facts, then the current root row and (after an approved edit) the successor.
void readFacts(const CtxPtr &ctx, const std::string &proposalId, bool withDecision,
               TaskChangeService::Outcome outcome, FactsCallback next) {
    ctx->scope->tx().execSqlAsync(
        kFactsSql,
        [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_facts", "row_count"); return; }
            auto status = std::make_shared<TaskChangeService::Status>(makeStatus(outcome));
            std::string rootId;
            std::string successorId;
            try {
                Json::Value p;
                if (!parseObject(rows[0]["proposal_json"].as<std::string>(), p)) {
                    failAt(ctx, "read_facts", "json_format");
                    return;
                }
                Json::Value change(Json::objectValue);
                change["id"] = p["id"];
                change["task_id"] = p["task_id"];
                change["kind"] = p["kind"];
                change["proposer_id"] = p["proposer_id"];
                change["created_at"] = p["created_at"];
                change["terms"] = TaskService::changeTermsJson(p);
                change["decision"] = Json::Value(Json::nullValue);
                if (!rows[0]["decision_json"].isNull()) {
                    Json::Value d;
                    if (!parseObject(rows[0]["decision_json"].as<std::string>(), d)) {
                        failAt(ctx, "read_facts", "json_format");
                        return;
                    }
                    change["decision"] = d;
                }
                status->change = change;
                rootId = rows[0]["task_id"].as<std::string>();
                successorId = rows[0]["successor_id"].isNull() ? "" : rows[0]["successor_id"].as<std::string>();
            } catch (const std::exception &) {
                failAt(ctx, "read_facts", "column_format");
                return;
            }
            readCurrent(ctx, rootId, [ctx, status, successorId, next](TaskService::TaskData task) {
                status->task = std::move(task);
                if (successorId.empty()) { next(std::move(*status)); return; }
                readCurrent(ctx, successorId, [status, next](TaskService::TaskData successor) {
                    status->successor = std::move(successor);
                    next(std::move(*status));
                });
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_facts", "db_error"); },
        proposalId, std::string(withDecision ? "true" : "false"));
}

void replay(const CtxPtr &ctx, const std::string &proposalId) {
    // A replay rebuilds immutable facts and reports current state; it is read-only.
    readFacts(ctx, proposalId, ctx->decide, TaskChangeService::Outcome::Replayed,
              [ctx](TaskChangeService::Status status) { rejectWith(ctx, std::move(status)); });
}

void commitWithFacts(const CtxPtr &ctx, TaskChangeService::Outcome outcome) {
    readFacts(ctx, ctx->proposalId, ctx->decide, outcome, [ctx](TaskChangeService::Status status) {
        ctx->result = std::move(status);
        ctx->commitRequested = true;
        ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
    });
}

void audit(const CtxPtr &ctx, const char *action, const Json::Value &detail, std::function<void()> next) {
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx, next = std::move(next)](const drogon::orm::Result &) {
            writeNotification(ctx->scope, ctx->decide ? "task_decision" : "task_proposal", ctx->proposalId,
                              next, [ctx]() { failAt(ctx, "notification", "write_failed"); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId, ctx->coupleId, std::string(action), ctx->taskId, compactJson(detail));
}

TaskChangeService::Status closedStatus(const std::string &reason) {
    return reason == "cancelled"
        ? makeStatus(TaskChangeService::Outcome::Cancelled, "任务已取消")
        : makeStatus(TaskChangeService::Outcome::Superseded, "任务已被修改后的新任务替换，请刷新");
}

// ================================================================ propose
void proposeInsert(const CtxPtr &ctx);

void proposeClassify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kProposalByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string id;
            std::string stored;
            try {
                id = rows[0]["id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            replay(ctx, id);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void proposeState(const CtxPtr &ctx) {
    const bool edit = ctx->input.kind == "edit";
    // Non-const copies: Drogon's binder only accepts non-const string arguments by value.
    std::string endDate = edit ? ctx->input.terms.deadlineLocalDate : std::string();
    std::string endTime = edit ? ctx->input.terms.deadlineLocalTime : std::string();
    std::string startDate = edit ? ctx->input.terms.startDate : std::string();
    ctx->scope->tx().execSqlAsync(
        kProposeStateSql,
        [ctx, edit](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            try {
                const auto &r = rows[0];
                if (!r["closed_reason"].isNull()) { rejectWith(ctx, closedStatus(r["closed_reason"].as<std::string>())); return; }
                if (r["pending"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Pending,
                                               "这项任务已有一个待对方确认的变更，请先处理它"));
                    return;
                }
                // A template's completed period does not block changes to the template.
                const bool rootRecurring = ctx->scheduleType == "daily" || ctx->scheduleType == "weekly";
                if (!rootRecurring && r["completed"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::AlreadyCompleted, "任务已经完成，不能再修改或取消"));
                    return;
                }
                if (!rootRecurring && r["expired"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Expired, "任务已过期，不能再修改或取消"));
                    return;
                }
                if (edit && !r["end_ok"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Validation, "新的截止时间必须晚于当前时间"));
                    return;
                }
                if (edit && !r["start_ok"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Validation,
                                               "修改后的开始日期不能早于今天（保留原开始日期除外）"));
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "state", "column_format"); return; }
            proposeInsert(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->taskId, endDate, endTime, startDate);
}

void proposeInsert(const CtxPtr &ctx) {
    auto t = ctx->input.terms;  // non-const: the conditional arguments below must bind as std::string
    const bool edit = ctx->input.kind == "edit";
    ctx->proposalId = newUuid();
    ctx->scope->tx().execSqlAsync(
        kProposalInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // A concurrent same-key proposal on another task committed first.
                proposeClassify(ctx, [ctx]() { failAt(ctx, "insert", "conflict_unexplained"); });
                return;
            }
            Json::Value detail(Json::objectValue);
            detail["proposal_id"] = ctx->proposalId;
            detail["kind"] = ctx->input.kind;
            audit(ctx, "task.change.proposed", detail,
                  [ctx]() { commitWithFacts(ctx, TaskChangeService::Outcome::Proposed); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
        ctx->proposalId, ctx->coupleId, ctx->taskId, ctx->userId, ctx->input.kind,
        edit ? t.title : std::string(), edit ? t.description : std::string(),
        edit ? std::to_string(t.rewardFlowers) : std::string(), edit ? std::to_string(t.rewardSunshine) : std::string(),
        edit ? t.completionMode : std::string(), edit ? std::to_string(t.targetCount) : std::string(),
        edit ? t.deadlineLocalDate : std::string(), edit ? t.deadlineLocalTime : std::string(),
        edit ? t.startDate : std::string(), ctx->key, ctx->fingerprint);
}

void proposeTask(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kProposeTaskSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["task_id"].isNull()) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::TaskNotFound, "任务不存在"));
                return;
            }
            std::string creator;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->taskId = rows[0]["task_id"].as<std::string>();
                creator = rows[0]["creator_id"].as<std::string>();
                ctx->scheduleType = rows[0]["schedule_type"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            if (ctx->input.kind == "edit" && creator != ctx->userId) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Forbidden, "只有发布者可以提出修改"));
                return;
            }
            if (ctx->input.kind == "edit" && ctx->input.terms.scheduleType != ctx->scheduleType) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Validation,
                                           "不能修改重复方式；请取消后重新发布"));
                return;
            }
            ctx->fingerprint = TaskChangeService::proposeFingerprint(ctx->taskId, ctx->input);
            ctx->scope->tx().execSqlAsync(
                kLockSql,
                [ctx](const drogon::orm::Result &) {
                    // Replay first: a committed proposal replays even after later decisions.
                    proposeClassify(ctx, [ctx]() { proposeState(ctx); });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "task_lock", "db_error"); },
                ctx->taskId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->taskId);
}

// ================================================================ decide
void decideClassify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kDecisionByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string proposal;
            std::string stored;
            try {
                proposal = rows[0]["proposal_id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            replay(ctx, proposal);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void decideAlready(const CtxPtr &ctx, std::function<void()> onUndecided) {
    ctx->scope->tx().execSqlAsync(
        kDecisionForProposalSql,
        [ctx, onUndecided = std::move(onUndecided)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onUndecided(); return; }
            std::string outcome;
            try { outcome = rows[0]["outcome"].as<std::string>(); }
            catch (const std::exception &) { failAt(ctx, "decided", "column_format"); return; }
            const char *text = outcome == "approved" ? "该变更已被同意" : outcome == "rejected" ? "该变更已被拒绝" : "该变更已撤回";
            rejectWith(ctx, makeStatus(TaskChangeService::Outcome::AlreadyDecided, text));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "decided", "db_error"); },
        ctx->proposalId);
}

void decideFinish(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["proposal_id"] = ctx->proposalId;
    detail["kind"] = ctx->kind;
    if (!ctx->successorId.empty()) detail["successor_task_id"] = ctx->successorId;
    if (!ctx->copiedRuleId.empty()) detail["reminder_rule_id"] = ctx->copiedRuleId;
    const char *action = ctx->action == TaskChangeService::Action::Approve ? "task.change.approved"
                       : ctx->action == TaskChangeService::Action::Reject ? "task.change.rejected"
                                                                          : "task.change.withdrawn";
    audit(ctx, action, detail, [ctx]() { commitWithFacts(ctx, TaskChangeService::Outcome::Decided); });
}

void copyReminderRule(const CtxPtr &ctx) {
    if (ctx->successorId.empty()) { decideFinish(ctx); return; }
    ctx->scope->tx().execSqlAsync(
        kRuleCopySql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() > 1u) { failAt(ctx, "rule_copy", "row_count"); return; }
            try {
                if (rows.size() == 1u) ctx->copiedRuleId = rows[0]["id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "rule_copy", "column_format"); return; }
            decideFinish(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "rule_copy", "db_error"); },
        ctx->proposalId, std::string(ctx->fault == TaskChangeService::Fault::RuleCopyRefused ? "2" : "1"));
}

void approveApply(const CtxPtr &ctx) {
    auto closure = [ctx]() {
        ctx->scope->tx().execSqlAsync(
            kClosureInsertSql,
            [ctx](const drogon::orm::Result &rows) {
                if (rows.size() != 1u) { failAt(ctx, "closure", "row_count"); return; }
                copyReminderRule(ctx);
            },
            [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "closure", "db_error"); },
            ctx->proposalId);
    };
    if (ctx->successorId.empty()) { closure(); return; }
    ctx->scope->tx().execSqlAsync(
        kSuccessorSql,
        [ctx, closure](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "successor", "row_count"); return; }
            closure();
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "successor", "db_error"); },
        ctx->proposalId, ctx->successorFingerprint);
}

void approveState(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kApproveStateSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "approve_state", "row_count"); return; }
            const bool edit = ctx->kind == "edit";
            try {
                const auto &r = rows[0];
                const bool recurring = ctx->scheduleType == "daily" || ctx->scheduleType == "weekly";
                if (!r["closed_reason"].isNull()) { rejectWith(ctx, closedStatus(r["closed_reason"].as<std::string>())); return; }
                if (!recurring && r["completed"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::AlreadyCompleted, "任务已经完成，不能再修改或取消"));
                    return;
                }
                if (!recurring && r["expired"].as<bool>()) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Expired, "任务已过期，不能再修改或取消"));
                    return;
                }
                if (edit && (recurring ? r["period_started"].as<bool>() : r["progressed"].as<bool>())) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::HasProgress,
                        recurring ? "本周期已有进度，修改只能在下一周期开始、尚无进度时同意"
                                  : "任务已有进度，不能修改；可以拒绝或撤回，或改为取消任务"));
                    return;
                }
                if (edit && (!r["end_ok"].as<bool>() || !r["start_ok"].as<bool>())) {
                    rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Expired,
                                               "提议的日期已过，无法同意；请拒绝后重新提议"));
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "approve_state", "column_format"); return; }
            approveApply(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "approve_state", "db_error"); },
        ctx->proposalId);
}

void decideInsert(const CtxPtr &ctx) {
    const bool approve = ctx->action == TaskChangeService::Action::Approve;
    if (approve && ctx->kind == "edit") ctx->successorId = newUuid();
    std::string outcome = approve ? "approved" : ctx->action == TaskChangeService::Action::Reject ? "rejected" : "withdrawn";
    ctx->scope->tx().execSqlAsync(
        kDecisionInsertSql,
        [ctx, approve](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // Another transaction committed this key first (on another task's lock).
                decideClassify(ctx, [ctx]() { decideAlready(ctx, [ctx]() { failAt(ctx, "insert", "conflict_unexplained"); }); });
                return;
            }
            if (approve) approveState(ctx);
            else decideFinish(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
        ctx->proposalId, ctx->coupleId, ctx->userId, outcome, ctx->successorId, ctx->key, ctx->fingerprint);
}

void decideTask(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kDecideTaskSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["proposal_id"].isNull()) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::ChangeNotFound, "变更不存在"));
                return;
            }
            try {
                const auto &r = rows[0];
                ctx->coupleId = r["couple_id"].as<std::string>();
                // Canonical ids from the database: the lock key and fingerprint never depend on path case.
                ctx->proposalId = r["proposal_id"].as<std::string>();
                ctx->taskId = r["task_id"].as<std::string>();
                ctx->kind = r["kind"].as<std::string>();
                ctx->proposerId = r["proposer_id"].as<std::string>();
                ctx->scheduleType = r["schedule_type"].as<std::string>();
                if (ctx->kind == "edit") {
                    TaskService::CreateInput terms;
                    terms.title = r["title"].as<std::string>();
                    terms.description = r["description"].as<std::string>();
                    terms.rewardFlowers = r["reward_flowers"].as<int>();
                    terms.rewardSunshine = r["reward_sunshine"].as<int>();
                    terms.completionMode = r["completion_mode"].as<std::string>();
                    terms.targetCount = r["target_count"].as<int>();
                    terms.scheduleType = ctx->scheduleType;
                    terms.deadlineLocalDate = r["deadline_local_date"].isNull() ? "" : r["deadline_local_date"].as<std::string>();
                    terms.deadlineLocalTime = r["deadline_local_time"].isNull() ? "" : r["deadline_local_time"].as<std::string>();
                    terms.startDate = r["start_date"].isNull() ? "" : r["start_date"].as<std::string>();
                    ctx->successorFingerprint = TaskService::createFingerprint(terms);
                }
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            const bool withdraw = ctx->action == TaskChangeService::Action::Withdraw;
            if (withdraw != (ctx->proposerId == ctx->userId)) {
                rejectWith(ctx, makeStatus(TaskChangeService::Outcome::Forbidden,
                    withdraw ? "只有提出变更的一方可以撤回" : "变更需要由另一方同意或拒绝"));
                return;
            }
            ctx->fingerprint = TaskChangeService::decisionFingerprint(ctx->proposalId, ctx->action);
            ctx->scope->tx().execSqlAsync(
                kLockSql,
                [ctx](const drogon::orm::Result &) {
                    // Replay first; then an already decided proposal; then the new decision.
                    decideClassify(ctx, [ctx]() { decideAlready(ctx, [ctx]() { decideInsert(ctx); }); });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "task_lock", "db_error"); },
                ctx->taskId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->taskId, ctx->proposalId);
}

void begin(const CtxPtr &ctx, void (*first)(const CtxPtr &)) {
    std::weak_ptr<ChangeCtx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx, first](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            first(ctx);
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(TaskChangeService::Outcome::ResultUnknown, "",
                                                label(strong->decide, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(TaskChangeService::Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(TaskChangeService::Outcome::Error, "",
                                                label(strong->decide, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "TaskChangeService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

TaskChangeService &TaskChangeService::instance() {
    static TaskChangeService service;
    return service;
}

bool TaskChangeService::parseProposeRequest(const Json::Value &body, ProposeInput &out, std::string &message) {
    out = ProposeInput{};
    if (!body.isObject()) {
        message = "请求体必须是一个 JSON 对象";
        return false;
    }
    for (const auto &name : body.getMemberNames()) {
        if (name != "kind" && name != "terms") {
            message = "请求体包含不支持的字段";
            return false;
        }
    }
    const std::string kind = body["kind"].isString() ? body["kind"].asString() : "";
    if (kind != "edit" && kind != "cancel") {
        message = "变更类型必须是 edit 或 cancel";
        return false;
    }
    out.kind = kind;
    if (kind == "cancel") {
        if (body.isMember("terms")) {
            message = "取消任务不需要提供新内容";
            return false;
        }
        return true;
    }
    if (!body.isMember("terms") || !body["terms"].isObject()) {
        message = "修改任务需要提供完整的新内容";
        return false;
    }
    return TaskService::parseCreateRequest(body["terms"], out.terms, message);
}

bool TaskChangeService::parseDecisionRequest(const Json::Value &body, Action &out, std::string &message) {
    if (!body.isObject() || body.size() != 1u || !body["action"].isString()) {
        message = "请求体只能包含 action";
        return false;
    }
    const auto action = body["action"].asString();
    if (action == "approve") out = Action::Approve;
    else if (action == "reject") out = Action::Reject;
    else if (action == "withdraw") out = Action::Withdraw;
    else {
        message = "action 必须是 approve、reject 或 withdraw";
        return false;
    }
    return true;
}

const char *TaskChangeService::actionName(Action action) {
    switch (action) {
        case Action::Approve: return "approve";
        case Action::Reject: return "reject";
        case Action::Withdraw: return "withdraw";
    }
    return "reject";
}

std::string TaskChangeService::proposeFingerprint(const std::string &taskId, const ProposeInput &input) {
    std::string canonical = "task-change:v1:task_id=" + std::to_string(taskId.size()) + ":" + taskId +
                            ";kind=" + std::to_string(input.kind.size()) + ":" + input.kind + ";";
    if (input.kind == "edit") canonical += TaskService::canonicalCreateString(input.terms);
    return crypto::sha256Hex(canonical);
}

std::string TaskChangeService::decisionFingerprint(const std::string &proposalId, Action action) {
    const std::string name = actionName(action);
    return crypto::sha256Hex("task-change-decision:v1:proposal_id=" + std::to_string(proposalId.size()) + ":" +
                             proposalId + ";action=" + std::to_string(name.size()) + ":" + name + ";");
}

Json::Value TaskChangeService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["change"] = status.change;
    body["task"] = TaskService::taskJson(status.task);
    body["successor"] = status.successor ? TaskService::taskJson(*status.successor) : Json::Value(Json::nullValue);
    return body;
}

ErrorCode TaskChangeService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::TaskNotFound: return ErrorCode::TaskNotFound;
        case Outcome::ChangeNotFound: return ErrorCode::TaskChangeNotFound;
        case Outcome::Forbidden: return ErrorCode::TaskChangeForbidden;
        case Outcome::Pending: return ErrorCode::TaskChangePending;
        case Outcome::HasProgress: return ErrorCode::TaskHasProgress;
        case Outcome::AlreadyDecided: return ErrorCode::TaskChangeDecided;
        case Outcome::AlreadyCompleted: return ErrorCode::TaskAlreadyCompleted;
        case Outcome::Expired: return ErrorCode::TaskExpired;
        case Outcome::Cancelled: return ErrorCode::TaskCancelled;
        case Outcome::Superseded: return ErrorCode::TaskSuperseded;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::Proposed:
        case Outcome::Decided:
        case Outcome::Replayed:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int TaskChangeService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Proposed) return 201;
    if (outcome == Outcome::Decided || outcome == Outcome::Replayed) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void TaskChangeService::propose(const std::string &userId, const std::string &taskId, const std::string &key,
                                const ProposeInput &input, Callback cb) {
    if (userId.empty() || taskId.empty() || !TaskService::validIdempotencyKey(key) ||
        (input.kind != "edit" && input.kind != "cancel")) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(false, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(false, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<ChangeCtx>();
    ctx->userId = userId;
    ctx->taskId = taskId;
    ctx->key = key;
    ctx->input = input;
    ctx->done = std::move(cb);
    begin(ctx, &proposeTask);
}

void TaskChangeService::decide(const std::string &userId, const std::string &taskId, const std::string &proposalId,
                               const std::string &key, Action action, Callback cb, Fault fault) {
    if (userId.empty() || taskId.empty() || proposalId.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(true, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(true, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<ChangeCtx>();
    ctx->decide = true;
    ctx->userId = userId;
    ctx->taskId = taskId;
    ctx->proposalId = proposalId;
    ctx->key = key;
    ctx->action = action;
    ctx->fault = fault;
    ctx->done = std::move(cb);
    begin(ctx, &decideTask);
}

}  // namespace usapp
