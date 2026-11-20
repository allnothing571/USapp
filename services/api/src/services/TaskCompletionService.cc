// =====================================================================
// services/api/src/services/TaskCompletionService.cc
// =====================================================================
#include "services/TaskCompletionService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include "core/Crypto.h"
#include "db/Db.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
// 第 1 步：调用者成员关系（FOR SHARE OF m；任务行不加锁，D30 方案 A）与同空间的任务。
// 没有成员关系 -> 0 行；任务不存在或属于其他空间 -> task_id 为 NULL。
const char *kTaskForCallerSql =
    "SELECT m.couple_id::text AS couple_id, t.id::text AS task_id, t.assignee_id::text AS assignee_id, "
    "       t.reward_flowers, t.reward_sunshine, t.completion_mode, t.target_count, t.schedule_type "
    "FROM couple_members m "
    "LEFT JOIN tasks t ON t.couple_id = m.couple_id AND t.id = $2::uuid AND t.recurrence_id IS NULL "
    "WHERE m.user_id = $1::uuid "
    "FOR SHARE OF m";

// 第 2 步：抢占。没有冲突目标：业务来源唯一键与 (发起者, 类型, 键) 部分唯一索引都会使其跳过；
// 正在进行的冲突插入会让本语句等待，对方提交后跳过、回滚后插入。
const char *kClaimSql =
    "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, "
    "idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'TASK_REWARD', 'task', $4::text, "
    "NULLIF($5::text, ''), NULLIF($6::text, '')) "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

// 第 3 步：分类（不依赖 SQLSTATE）。
const char *kByKeySql =
    "SELECT id::text AS id, request_fingerprint FROM wallet_operations "
    "WHERE actor_user_id = $1::uuid AND operation_type = 'TASK_REWARD' AND idempotency_key = $2::text";

const char *kBySourceSql =
    "SELECT count(*)::bigint AS n FROM wallet_operations "
    "WHERE operation_type = 'TASK_REWARD' AND source_type = 'task' AND source_id = $1::text";

// 第 4 步：账户（升序逐个创建并加行锁；usapp_api 在 lock_token 上有列级 UPDATE，仅用于加锁）。
const char *kAccountInsertSql =
    "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES ($1::uuid, $2::text, $3::uuid) "
    "ON CONFLICT (user_id, asset_type) DO NOTHING";

const char *kAccountLockSql =
    "SELECT 1 AS locked FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = $2::text FOR UPDATE";

// 第 5 步：加锁之后读取最新流水（新语句取新快照，能看到上一个锁持有者已提交的流水）。
const char *kLatestEntrySql =
    "SELECT COALESCE((SELECT e.seq FROM wallet_ledger_entries e "
    "                  WHERE e.user_id = $1::uuid AND e.asset_type = $2::text "
    "                  ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS seq, "
    "       COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e "
    "                  WHERE e.user_id = $1::uuid AND e.asset_type = $2::text "
    "                  ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS balance";

// 第 7 步（D38）：一条语句读取一次 clock_timestamp() 作为决策时间并插入完成记录；
// 截止时间不晚于决策时间时不插入（0 行 -> TASK_EXPIRED）。0007 的完成记录守卫会再次校验。
const char *kDecisionInsertSql =
    "WITH n AS (SELECT clock_timestamp() AS t) "
    "INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers, "
    "reward_sunshine, completed_at) "
    "SELECT t.id, t.couple_id, $2::uuid, $3::uuid, t.reward_flowers, t.reward_sunshine, n.t "
    "FROM n, tasks t "
    "WHERE t.id = $1::uuid AND (t.deadline_at IS NULL OR n.t < t.deadline_at) "
    "RETURNING task_id::text AS task_id";

// 第 8 步：每条语句一条流水（0006 拒绝多行插入）；数值按计划 9.8 第 6 条以 int64 绑定并显式 ::bigint。
const char *kEntrySql =
    "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::bigint, $6::bigint, $7::bigint)";

// D32：固定的诊断语句，只读取本次服务生成的操作。恰好有一条流水时除以 0。
const char *kFaultProbeSql =
    "SELECT 1 / (SELECT count(*) - 1 FROM wallet_ledger_entries WHERE operation_id = $1::uuid) AS probe";

// 第 9 步：审计（同事务；detail 只含操作 id 与入账金额）。
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, 'task.completed', 'task', $3::text, $4::jsonb)";

// M6: a closed root (cancelled, or superseded by an approved edit) accepts no new action.
// Read under the per-root advisory lock, after key classification.
const char *kClosureSql = "SELECT reason FROM task_closures WHERE task_id = $1::uuid";

const char *kProgressKeySql =
    "SELECT p.id::text AS id, p.task_id::text AS task_id, COALESCE(t.recurrence_id, t.id)::text AS public_id, "
    "to_char(t.period_date, 'YYYY-MM-DD') AS period_date FROM task_progress_events p "
    "JOIN tasks t ON t.id = p.task_id WHERE p.user_id = $1::uuid AND p.idempotency_key = $2::text";
const char *kProgressStateSql =
    "SELECT COALESCE((SELECT p.progress_count FROM task_progress_events p WHERE p.task_id = t.id "
    "ORDER BY p.progress_count DESC LIMIT 1), 0) AS progress_count, "
    "EXISTS(SELECT 1 FROM task_completions c WHERE c.task_id = t.id) AS completed, "
    // A stored period is current inside [period start, deadline): today for daily,
    // this Monday-to-Monday week for weekly. Identical to the M5-A date test for daily.
    "(t.period_date IS NULL OR (clock_timestamp() >= (t.period_date::timestamp AT TIME ZONE 'Asia/Shanghai') "
    "AND clock_timestamp() < t.deadline_at)) AS current_period, "
    // M5-C: a custom range accepts actions from its start date 00:00 (Beijing).
    "(t.start_local_date IS NULL OR clock_timestamp() >= (t.start_local_date::timestamp AT TIME ZONE 'Asia/Shanghai')) AS started "
    "FROM tasks t WHERE t.id = $1::uuid";
const char *kProgressInsertSql =
    "WITH n AS (SELECT clock_timestamp() AS decision) "
    "INSERT INTO task_progress_events (id, task_id, couple_id, user_id, progress_count, target_count, "
    "idempotency_key, operation_id, recorded_at) "
    "SELECT $1::uuid, t.id, t.couple_id, $3::uuid, $4::integer, t.target_count, $5::text, "
    "NULLIF($6::text, '')::uuid, n.decision FROM tasks t, n "
    "WHERE t.id = $2::uuid AND (t.deadline_at IS NULL OR n.decision < t.deadline_at) "
    "AND (t.start_local_date IS NULL OR n.decision >= (t.start_local_date::timestamp AT TIME ZONE 'Asia/Shanghai')) "
    "ON CONFLICT (user_id, idempotency_key) DO NOTHING RETURNING id::text AS id";
const char *kCountCompletionSql =
    "INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers, "
    "reward_sunshine, completed_at) "
    "SELECT t.id, t.couple_id, p.user_id, p.operation_id, t.reward_flowers, t.reward_sunshine, p.recorded_at "
    "FROM task_progress_events p JOIN tasks t ON t.id = p.task_id WHERE p.id = $1::uuid";
const char *kProgressFactsSql =
    "SELECT p.id::text AS event_id, COALESCE(t.recurrence_id, t.id)::text AS task_id, p.progress_count, p.target_count, "
    "to_char(t.period_date, 'YYYY-MM-DD') AS period_date, "
    "to_char(p.recorded_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS recorded_at, "
    "p.operation_id::text AS operation_id, c.reward_flowers, c.reward_sunshine "
    "FROM task_progress_events p JOIN tasks t ON t.id = p.task_id "
    "LEFT JOIN task_completions c ON c.operation_id = p.operation_id "
    "WHERE p.id = $1::uuid";
const char *kProgressAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, 'task.progressed', 'task', $3::text, $4::jsonb)";

const char *kDailyExistingSql =
    "SELECT id::text AS id FROM tasks WHERE recurrence_id = $1::uuid AND period_date = $2::date";
// The requested period must be the current one at the decision time: today for
// daily (span 1), this week's Monday for weekly (span 7). A weekly period ends on
// Sunday (deadline_local_date) and expires at the next Monday 00:00 Beijing time.
const char *kDailyInsertSql =
    "WITH n AS (SELECT clock_timestamp() AS decision) "
    "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers, "
    "reward_sunshine, timezone, deadline_local_date, deadline_at, create_idempotency_key, create_fingerprint, "
    "created_at, completion_mode, target_count, recurrence_id, period_date) "
    "SELECT $3::uuid, t.couple_id, t.creator_id, t.assignee_id, t.title, t.description, t.reward_flowers, "
    "t.reward_sunshine, t.timezone, $2::date + (x.span - 1), ($2::date + x.span)::timestamp AT TIME ZONE 'Asia/Shanghai', "
    "$4::text, t.create_fingerprint, n.decision, t.completion_mode, t.target_count, t.id, $2::date "
    "FROM tasks t CROSS JOIN n CROSS JOIN LATERAL (SELECT CASE t.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END AS span, "
    "(n.decision AT TIME ZONE 'Asia/Shanghai')::date AS today) x "
    "WHERE t.id = $1::uuid AND t.schedule_type IN ('daily', 'weekly') AND t.recurrence_id IS NULL "
    "AND $2::date = x.today - CASE WHEN x.span = 7 THEN EXTRACT(ISODOW FROM x.today)::integer - 1 ELSE 0 END "
    "ON CONFLICT (recurrence_id, period_date) DO NOTHING RETURNING id::text AS id";

// 第 10 步与重放：只由不可变的行构造响应事实（任务、完成记录、操作 id），不读余额。
const std::string &factsSql() {
    static const std::string sql = std::string("SELECT ") + TaskService::taskColumnsSql() +
                                   ", c.operation_id::text AS operation_id, "
                                   "c.reward_flowers AS settled_flowers, c.reward_sunshine AS settled_sunshine "
                                   "FROM task_completions c JOIN tasks t ON t.id = c.task_id "
                                   "WHERE c.operation_id = $1::uuid";
    return sql;
}

struct Asset {
    const char *type;
    std::int64_t reward = 0;
    std::int64_t cap = 0;
    std::int64_t prevSeq = 0;
    std::int64_t prevBalance = 0;
};

std::string newOperationId() {
    std::string hex = crypto::randomHex(16);
    hex[12] = '4';
    const char variant[] = {'8', '9', 'a', 'b'};
    hex[16] = variant[static_cast<unsigned char>(hex[16]) % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20, 12);
}

TaskCompletionService::Status makeStatus(TaskCompletionService::Outcome outcome,
                                         std::string message = "",
                                         std::string detail = "") {
    TaskCompletionService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

std::string label(const char *stage, const char *reason, bool progress = false) {
    return TaskService::failureDetail(progress ? "task_progress" : "task_complete", stage, reason);
}

// ---------------------------------------------------------------------
// 生命周期与所有权（与 M3-B TaskService 相同的模式）：
//   CompleteCtx --(shared)--> TxScope --(onError_，弱引用)--> CompleteCtx
// SQL 回调与提交回调强捕获 ctx；所有结束路径经 complete()，completed 保证恰好一次；
// done 随 ctx 释放。commitRequested 在 TxScope::commit 之前置位，此后的错误一律 ResultUnknown。
// ---------------------------------------------------------------------
struct CompleteCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    std::string requestedTaskId;
    std::string key;
    TaskCompletionService::SettlementFault fault = TaskCompletionService::SettlementFault::None;
    // 第 1 步之后
    std::string taskId;  // 数据库返回的小写规范 uuid
    std::string coupleId;
    std::string fingerprint;
    std::string operationId;
    std::vector<Asset> assets;  // 只含正奖励资产，按 asset_type 升序（flower < sunshine）
    std::size_t step = 0;       // 账户/流水逐个处理的游标
    TaskCompletionService::Status result;
    TaskCompletionService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
    bool progressAction = false;
    int targetCount = 1;
    int nextProgress = 0;
    std::string eventId;
    std::string publicTaskId;
    std::string periodDate;
    std::string scheduleType;
};

using CtxPtr = std::shared_ptr<CompleteCtx>;

void complete(const CtxPtr &ctx, TaskCompletionService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "TaskCompletionService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) {
        ctx->done(std::move(status));
    }
}

// 记录固定标签后失败；从不读取数据库异常对象。
void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(stage, reason, ctx->progressAction);
    ctx->scope->fail(ctx->failDetail);
}

// 提交前的业务拒绝：请求回滚（回调只表示“已请求”），然后结束。
void rejectWith(const CtxPtr &ctx, TaskCompletionService::Status status) {
    auto shared = std::make_shared<TaskCompletionService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

bool readFacts(const drogon::orm::Result &rows, TaskCompletionService::Status &status) {
    if (rows.size() != 1u) {
        return false;
    }
    try {
        status.task = TaskService::taskFromRow(rows[0]);
        status.settlement.operationId = rows[0]["operation_id"].as<std::string>();
        status.settlement.flower = rows[0]["settled_flowers"].as<int>();
        status.settlement.sunshine = rows[0]["settled_sunshine"].as<int>();
        status.hasSettlement = true;
        status.periodDate = status.task.periodDate;
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

std::string auditDetail(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["operation_id"] = ctx->operationId;
    std::int64_t flower = 0;
    std::int64_t sunshine = 0;
    for (const auto &asset : ctx->assets) {
        (std::string(asset.type) == "flower" ? flower : sunshine) = asset.reward;
    }
    detail["flower"] = Json::Int64(flower);
    detail["sunshine"] = Json::Int64(sunshine);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, detail);
}

// ---- 第 11 步：提交 ----
void stepCommit(const CtxPtr &ctx) {
    ctx->commitRequested = true;
    ctx->scope->commit([ctx]() {
        TaskCompletionService::Status status = std::move(ctx->result);
        status.outcome = ctx->progressAction && !status.hasSettlement
            ? TaskCompletionService::Outcome::Progressed : TaskCompletionService::Outcome::Completed;
        complete(ctx, std::move(status));
    });
}

void stepReadProgressFacts(const CtxPtr &ctx, bool replay = false);
void stepProgressAudit(const CtxPtr &ctx);
void stepInsertProgress(const CtxPtr &ctx);

// ---- 第 10 步：事务内读回响应事实 ----
void stepReadBack(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        factsSql(),
        [ctx](const drogon::orm::Result &rows) {
            if (!readFacts(rows, ctx->result)) {
                failAt(ctx, "read_back", "row_shape");
                return;
            }
            stepCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_back", "db_error"); },
        ctx->operationId);
}

// ---- 第 9 步：审计 ----
void stepAudit(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx](const drogon::orm::Result &) {
            if (ctx->progressAction) stepProgressAudit(ctx);
            else stepReadBack(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId,
        ctx->coupleId,
        ctx->taskId,
        auditDetail(ctx));
}

void stepEntry(const CtxPtr &ctx);

// ---- D32：第一条流水之后的固定诊断语句 ----
void stepFaultProbe(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kFaultProbeSql,
        [ctx](const drogon::orm::Result &) { stepEntry(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "fault_injection", "db_error"); },
        ctx->operationId);
}

// ---- 第 8 步：逐个写流水 ----
void stepEntry(const CtxPtr &ctx) {
    if (ctx->step >= ctx->assets.size()) {
        stepAudit(ctx);
        return;
    }
    const Asset &asset = ctx->assets[ctx->step];
    const std::int64_t seq = asset.prevSeq + 1;
    const std::int64_t amount = asset.reward;
    const std::int64_t balanceAfter = asset.prevBalance + asset.reward;
    ctx->scope->tx().execSqlAsync(
        kEntrySql,
        [ctx](const drogon::orm::Result &) {
            const bool first = ctx->step == 0;
            ++ctx->step;
            if (first && ctx->fault == TaskCompletionService::SettlementFault::FailAfterFirstEntry) {
                stepFaultProbe(ctx);
                return;
            }
            stepEntry(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "entry", "db_error"); },
        ctx->operationId,
        ctx->coupleId,
        ctx->userId,
        std::string(asset.type),
        seq,
        amount,
        balanceAfter);
}

// ---- 第 7 步：决策 + 完成记录（一条语句）----
void stepDecision(const CtxPtr &ctx) {
    if (ctx->progressAction) {
        stepInsertProgress(ctx);
        return;
    }
    ctx->scope->tx().execSqlAsync(
        kDecisionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Expired, "任务已过截止时间，无法完成"));
                return;
            }
            ctx->step = 0;
            stepEntry(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "decision", "db_error"); },
        ctx->taskId,
        ctx->userId,
        ctx->operationId);
}

// ---- 第 5、6 步：加锁之后逐个读取最新流水，然后检查上限 ----
void stepBalances(const CtxPtr &ctx) {
    if (ctx->step >= ctx->assets.size()) {
        for (const auto &asset : ctx->assets) {
            if (!TaskCompletionService::withinCap(asset.prevBalance, asset.reward, asset.cap)) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::BalanceLimit, "资产余额已接近上限，无法入账"));
                return;
            }
        }
        stepDecision(ctx);
        return;
    }
    const Asset &asset = ctx->assets[ctx->step];
    ctx->scope->tx().execSqlAsync(
        kLatestEntrySql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) {
                failAt(ctx, "balances", "row_shape");
                return;
            }
            try {
                ctx->assets[ctx->step].prevSeq = static_cast<std::int64_t>(rows[0]["seq"].as<long long>());
                ctx->assets[ctx->step].prevBalance = static_cast<std::int64_t>(rows[0]["balance"].as<long long>());
            } catch (const std::exception &) {
                failAt(ctx, "balances", "column_format");
                return;
            }
            ++ctx->step;
            stepBalances(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "balances", "db_error"); },
        ctx->userId,
        std::string(asset.type));
}

// ---- 第 4 步：逐个创建并锁定账户（升序）----
void stepAccounts(const CtxPtr &ctx);

void stepLockAccount(const CtxPtr &ctx) {
    const Asset &asset = ctx->assets[ctx->step];
    ctx->scope->tx().execSqlAsync(
        kAccountLockSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) {
                failAt(ctx, "accounts", "lock_missing");
                return;
            }
            ++ctx->step;
            stepAccounts(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "accounts", "db_error"); },
        ctx->userId,
        std::string(asset.type));
}

void stepAccounts(const CtxPtr &ctx) {
    if (ctx->step >= ctx->assets.size()) {
        ctx->step = 0;
        stepBalances(ctx);
        return;
    }
    const Asset &asset = ctx->assets[ctx->step];
    ctx->scope->tx().execSqlAsync(
        kAccountInsertSql,
        [ctx](const drogon::orm::Result &) { stepLockAccount(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "accounts", "db_error"); },
        ctx->userId,
        std::string(asset.type),
        ctx->coupleId);
}

// ---- 第 3 步：分类 ----
void stepReplay(const CtxPtr &ctx, const std::string &existingOperationId) {
    ctx->scope->tx().execSqlAsync(
        factsSql(),
        [ctx](const drogon::orm::Result &rows) {
            TaskCompletionService::Status status = makeStatus(TaskCompletionService::Outcome::Replayed);
            if (!readFacts(rows, status)) {
                failAt(ctx, "replay", "row_missing");
                return;
            }
            // 重放不重新判断截止时间与余额；只读，回滚即可。
            rejectWith(ctx, std::move(status));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "replay", "db_error"); },
        existingOperationId);
}

void stepClassifyBySource(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kBySourceSql,
        [ctx](const drogon::orm::Result &rows) {
            long long n = 0;
            try {
                n = rows.size() == 1u ? rows[0]["n"].as<long long>() : 0;
            } catch (const std::exception &) {
                failAt(ctx, "classify", "column_format");
                return;
            }
            if (n > 0) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::AlreadyCompleted, "任务已经完成"));
                return;
            }
            // ON CONFLICT DO NOTHING 只在已提交的冲突之后跳过；正常不会走到这里。
            failAt(ctx, "classify", "conflict_unexplained");
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->taskId);
}

void stepClassify(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kByKeySql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                stepClassifyBySource(ctx);
                return;
            }
            std::string existingId;
            std::string storedFingerprint;
            try {
                existingId = rows[0]["id"].as<std::string>();
                storedFingerprint = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) {
                failAt(ctx, "classify", "column_format");
                return;
            }
            if (storedFingerprint != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::KeyReused,
                                           "该请求标识已用于另一项操作"));
                return;
            }
            stepReplay(ctx, existingId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId,
        ctx->key);
}

// M6: reject a new action on a closed root; rollback leaves no business write.
void stepClosureCheck(const CtxPtr &ctx, std::function<void()> next) {
    ctx->scope->tx().execSqlAsync(
        kClosureSql,
        [ctx, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { next(); return; }
            std::string reason;
            try { reason = rows[0]["reason"].as<std::string>(); }
            catch (const std::exception &) { failAt(ctx, "closure", "column_format"); return; }
            if (reason == "cancelled") {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Cancelled, "任务已取消，不能再记录或完成"));
            } else {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Superseded,
                                           "任务已按确认的修改替换为新任务，请刷新后操作新任务"));
            }
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "closure", "db_error"); },
        ctx->publicTaskId);
}

// ---- 第 2 步：抢占 ----
void stepClaim(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kClaimSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                if (ctx->progressAction) failAt(ctx, "claim", "conflict_unexplained");
                else stepClassify(ctx);
                return;
            }
            // Counting actions checked the closure before their final claim.
            if (ctx->progressAction) {
                ctx->step = 0;
                stepAccounts(ctx);
                return;
            }
            stepClosureCheck(ctx, [ctx]() {
                ctx->step = 0;
                stepAccounts(ctx);
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "claim", "db_error"); },
        ctx->operationId,
        ctx->coupleId,
        ctx->userId,
        ctx->taskId,
        ctx->progressAction ? std::string() : ctx->key,
        ctx->progressAction ? std::string() : ctx->fingerprint);
}

// Counting actions share the exact account/entry/audit/commit path above. Only
// the event and final completion insertion differ; there is no nested transaction.
void stepReadProgressFacts(const CtxPtr &ctx, bool replay) {
    ctx->scope->tx().execSqlAsync(
        kProgressFactsSql,
        [ctx, replay](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "progress_read", "row_missing"); return; }
            try {
                auto &s = ctx->result;
                const auto &r = rows[0];
                s.task.id = r["task_id"].as<std::string>();
                s.periodDate = r["period_date"].isNull() ? "" : r["period_date"].as<std::string>();
                s.progress = TaskCompletionService::Status::Progress{
                    r["event_id"].as<std::string>(), r["progress_count"].as<int>(),
                    r["target_count"].as<int>(), r["recorded_at"].as<std::string>(), false};
                s.hasSettlement = !r["operation_id"].isNull();
                if (s.hasSettlement) {
                    s.settlement = {r["operation_id"].as<std::string>(),
                                    r["reward_flowers"].as<int>(), r["reward_sunshine"].as<int>()};
                }
            } catch (const std::exception &) { failAt(ctx, "progress_read", "column_format"); return; }
            if (replay) {
                ctx->result.outcome = TaskCompletionService::Outcome::Replayed;
                rejectWith(ctx, std::move(ctx->result));
            } else stepCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_read", "db_error"); },
        ctx->eventId);
}

void stepProgressAudit(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["event_id"] = ctx->eventId;
    detail["progress_count"] = ctx->nextProgress;
    detail["target_count"] = ctx->targetCount;
    if (!ctx->periodDate.empty()) detail["period_date"] = ctx->periodDate;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ctx->scope->tx().execSqlAsync(
        kProgressAuditSql,
        [ctx](const drogon::orm::Result &) { stepReadProgressFacts(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_audit", "db_error"); },
        ctx->userId, ctx->coupleId, ctx->taskId, Json::writeString(writer, detail));
}

// Used both before a new event and after a cross-task key conflict. Under the
// per-task lock a same-task duplicate cannot advance progress a second time.
void stepProgressKey(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kProgressKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            try {
                const auto date = rows[0]["period_date"].isNull() ? "" : rows[0]["period_date"].as<std::string>();
                if (rows[0]["public_id"].as<std::string>() != ctx->publicTaskId || date != ctx->periodDate) {
                    rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::KeyReused,
                                               "该进度请求标识已用于另一项任务"));
                    return;
                }
                ctx->eventId = rows[0]["id"].as<std::string>();
                ctx->taskId = rows[0]["task_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "progress_key", "column_format"); return; }
            stepReadProgressFacts(ctx, true);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_key", "db_error"); },
        ctx->userId, ctx->key);
}

void stepInsertProgress(const CtxPtr &ctx) {
    const bool final = ctx->nextProgress == ctx->targetCount;
    ctx->scope->tx().execSqlAsync(
        kProgressInsertSql,
        [ctx, final](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                stepProgressKey(ctx, [ctx]() {
                    rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Expired, "任务已过截止时间，无法记录"));
                });
                return;
            }
            if (!final) { stepProgressAudit(ctx); return; }
            // A second statement sees the just-inserted final event. Its timestamp
            // is the decision time; the completion guard verifies the exact link.
            ctx->scope->tx().execSqlAsync(
                kCountCompletionSql,
                [ctx](const drogon::orm::Result &) { ctx->step = 0; stepEntry(ctx); },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_completion", "db_error"); },
                ctx->eventId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_insert", "db_error"); },
        ctx->eventId, ctx->taskId, ctx->userId, std::to_string(ctx->nextProgress), ctx->key,
        final ? ctx->operationId : std::string());
}

void stepProgressState(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kProgressStateSql,
        [ctx](const drogon::orm::Result &rows) {
            int count = 0;
            bool done = false;
            bool currentPeriod = true;
            bool started = true;
            try {
                if (rows.size() != 1u) { failAt(ctx, "progress_state", "row_missing"); return; }
                count = rows[0]["progress_count"].as<int>();
                done = rows[0]["completed"].as<bool>();
                currentPeriod = rows[0]["current_period"].as<bool>();
                started = rows[0]["started"].as<bool>();
            } catch (const std::exception &) { failAt(ctx, "progress_state", "column_format"); return; }
            if (done && count == ctx->targetCount) {
                ctx->scope->tx().execSqlAsync(
                    std::string("SELECT ") + TaskService::taskColumnsSql() +
                    ", c.operation_id::text AS operation_id, c.reward_flowers AS settled_flowers, "
                    "c.reward_sunshine AS settled_sunshine FROM tasks t JOIN task_completions c ON c.task_id = t.id "
                    "WHERE t.id = $1::uuid",
                    [ctx](const drogon::orm::Result &facts) {
                        auto s = makeStatus(TaskCompletionService::Outcome::Ignored);
                        if (!readFacts(facts, s)) { failAt(ctx, "progress_state", "row_shape"); return; }
                        s.progress = TaskCompletionService::Status::Progress{"", ctx->targetCount, ctx->targetCount, "", true};
                        rejectWith(ctx, std::move(s));
                    },
                    [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_state", "db_error"); },
                    ctx->taskId);
                return;
            }
            if (done || count < 0 || count >= ctx->targetCount) {
                failAt(ctx, "progress_state", "inconsistent"); return;
            }
            if (!currentPeriod) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Expired, "周期已变化，请刷新任务后重试"));
                return;
            }
            if (!started) {
                // Read-only so far: the rollback leaves no event, operation or audit.
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::NotStarted,
                                           "任务尚未开始，开始日期当天起才能记录"));
                return;
            }
            ctx->nextProgress = count + 1; // count < target <= INT_MAX, so no overflow.
            if (ctx->nextProgress == ctx->targetCount) stepClaim(ctx);
            else stepInsertProgress(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_state", "db_error"); },
        ctx->taskId);
}

void stepDailyPeriodLock(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))",
        [ctx](const drogon::orm::Result &) { stepProgressState(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "daily_lock", "db_error"); },
        ctx->taskId);
}

void stepDailyResolve(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kDailyExistingSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 1u) {
                ctx->taskId = rows[0]["id"].as<std::string>();
                stepDailyPeriodLock(ctx);
                return;
            }
            const auto periodId = newOperationId();
            // A private random publication key keeps the existing NOT NULL and
            // uniqueness rules intact; it is not the caller's progress key.
            ctx->scope->tx().execSqlAsync(
                kDailyInsertSql,
                [ctx](const drogon::orm::Result &inserted) {
                    if (inserted.size() == 0u) {
                        rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Expired,
                                                   "周期已变化，请刷新任务后重试"));
                        return;
                    }
                    ctx->taskId = inserted[0]["id"].as<std::string>();
                    Json::Value detail(Json::objectValue);
                    detail["task_id"] = ctx->publicTaskId;
                    detail["period_date"] = ctx->periodDate;
                    if (ctx->scheduleType == "weekly") detail["schedule_type"] = ctx->scheduleType;
                    Json::StreamWriterBuilder writer;
                    writer["indentation"] = "";
                    ctx->scope->tx().execSqlAsync(
                        "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
                        "VALUES ('user', $1::uuid, $2::uuid, 'task.period.created', 'task', $3::text, $4::jsonb)",
                        [ctx](const drogon::orm::Result &) { stepDailyPeriodLock(ctx); },
                        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "daily_audit", "db_error"); },
                        ctx->userId, ctx->coupleId, ctx->taskId, Json::writeString(writer, detail));
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "daily_insert", "db_error"); },
                ctx->publicTaskId, ctx->periodDate, periodId, crypto::randomHex(16));
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "daily_read", "db_error"); },
        ctx->publicTaskId, ctx->periodDate);
}

void stepProgressLock(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))",
        [ctx](const drogon::orm::Result &) {
            // Replay first; only a new action is refused on a closed root (no period is materialized).
            stepProgressKey(ctx, [ctx]() {
                stepClosureCheck(ctx, [ctx]() {
                    if (ctx->periodDate.empty()) stepProgressState(ctx);
                    else stepDailyResolve(ctx);
                });
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "progress_lock", "db_error"); },
        ctx->taskId);
}

// M6 (D30 revisited): single completion takes the same per-task advisory lock as
// counting and change decisions, before its claim (membership -> task lock -> claim -> accounts).
void stepSingleLock(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))",
        [ctx](const drogon::orm::Result &) { stepClaim(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "task_lock", "db_error"); },
        ctx->taskId);
}

// ---- 第 1 步：成员关系与任务 ----
void stepTask(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kTaskForCallerSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) {
                failAt(ctx, "membership", "row_shape");
                return;
            }
            if (rows[0]["task_id"].isNull()) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::TaskNotFound, "任务不存在"));
                return;
            }
            std::string assignee;
            int flowers = 0;
            int sunshine = 0;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->taskId = rows[0]["task_id"].as<std::string>();
                ctx->publicTaskId = ctx->taskId;
                assignee = rows[0]["assignee_id"].as<std::string>();
                flowers = rows[0]["reward_flowers"].as<int>();
                sunshine = rows[0]["reward_sunshine"].as<int>();
            } catch (const std::exception &) {
                failAt(ctx, "membership", "column_format");
                return;
            }
            if (assignee != ctx->userId) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::NotAssignee, "只有任务的接收者可以完成"));
                return;
            }
            std::string mode;
            try {
                mode = rows[0]["completion_mode"].as<std::string>();
                ctx->targetCount = rows[0]["target_count"].as<int>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            if (mode != (ctx->progressAction ? "count" : "single")) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::WrongMode,
                    ctx->progressAction ? "单次任务请使用完成操作" : "计数任务请使用 +1 操作"));
                return;
            }
            const auto schedule = rows[0]["schedule_type"].as<std::string>();
            const bool recurring = schedule == "daily" || schedule == "weekly";
            if (recurring == ctx->periodDate.empty()) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Validation,
                    schedule == "weekly" ? "每周重复的任务需要页面显示的周期，请刷新任务"
                                         : "每天重复的任务需要页面显示的周期日期，请刷新任务"));
                return;
            }
            // A weekly period is identified by its Monday; any other date is malformed,
            // not a stale period, and is rejected before key classification.
            if (schedule == "weekly" && !TaskService::isWeekStart(ctx->periodDate)) {
                rejectWith(ctx, makeStatus(TaskCompletionService::Outcome::Validation,
                    "每周任务的周期必须从周一开始，请刷新任务"));
                return;
            }
            ctx->scheduleType = schedule;
            ctx->fingerprint = TaskCompletionService::completeFingerprint(ctx->taskId);
            ctx->assets.clear();
            if (flowers > 0) {
                ctx->assets.push_back(Asset{"flower", flowers, TaskCompletionService::kMaxBalanceFlowers});
            }
            if (sunshine > 0) {
                ctx->assets.push_back(Asset{"sunshine", sunshine, TaskCompletionService::kMaxBalanceSunshine});
            }
            if (ctx->assets.empty()) {
                failAt(ctx, "membership", "no_reward");  // 0007 的 CHECK 保证不会发生
                return;
            }
            if (ctx->progressAction) stepProgressLock(ctx);
            else stepSingleLock(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId,
        ctx->requestedTaskId);
}

}  // namespace

// =====================================================================
// 纯函数
// =====================================================================

TaskCompletionService &TaskCompletionService::instance() {
    static TaskCompletionService service;
    return service;
}

std::string TaskCompletionService::canonicalCompleteString(const std::string &taskId) {
    return "complete-task:v1:task_id=" + std::to_string(taskId.size()) + ":" + taskId + ";";
}

std::string TaskCompletionService::completeFingerprint(const std::string &taskId) {
    return crypto::sha256Hex(canonicalCompleteString(taskId));
}

bool TaskCompletionService::isEmptyCompletionBody(const std::string &raw) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::string compact;
    for (const char c : raw) {
        if (!isSpace(c)) {
            compact.push_back(c);
        }
    }
    return compact.empty() || compact == "{}";
}

bool TaskCompletionService::withinCap(std::int64_t balance, std::int64_t reward, std::int64_t cap) {
    if (balance < 0 || reward < 0) {
        return false;
    }
    return balance <= cap - reward;
}

Json::Value TaskCompletionService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["task"] = TaskService::taskJson(status.task);
    Json::Value settlement(Json::objectValue);
    settlement["operation_id"] = status.settlement.operationId;
    settlement["flower"] = status.settlement.flower;
    settlement["sunshine"] = status.settlement.sunshine;
    body["settlement"] = settlement;
    return body;
}

Json::Value TaskCompletionService::progressResponseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["task_id"] = status.task.id;
    const auto &p = *status.progress;
    body["event_id"] = p.eventId.empty() ? Json::Value(Json::nullValue) : Json::Value(p.eventId);
    body["progress_count"] = p.count;
    body["target_count"] = p.target;
    body["recorded_at"] = p.recordedAt.empty() ? Json::Value(Json::nullValue) : Json::Value(p.recordedAt);
    body["completed"] = p.count == p.target;
    body["ignored"] = p.ignored;
    if (!status.periodDate.empty()) body["period_date"] = status.periodDate;
    if (status.hasSettlement) body["settlement"] = responseBody(status)["settlement"];
    else body["settlement"] = Json::Value(Json::nullValue);
    return body;
}

ErrorCode TaskCompletionService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation:
            return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound:
            return ErrorCode::SpaceNotFound;
        case Outcome::TaskNotFound:
            return ErrorCode::TaskNotFound;
        case Outcome::NotAssignee:
            return ErrorCode::TaskNotAssignee;
        case Outcome::AlreadyCompleted:
            return ErrorCode::TaskAlreadyCompleted;
        case Outcome::KeyReused:
            return ErrorCode::IdempotencyKeyReused;
        case Outcome::Expired:
            return ErrorCode::TaskExpired;
        case Outcome::BalanceLimit:
            return ErrorCode::WalletBalanceLimit;
        case Outcome::WrongMode:
            return ErrorCode::TaskCompletionModeInvalid;
        case Outcome::NotStarted:
            return ErrorCode::TaskNotStarted;
        case Outcome::Cancelled:
            return ErrorCode::TaskCancelled;
        case Outcome::Superseded:
            return ErrorCode::TaskSuperseded;
        case Outcome::ResultUnknown:
            return ErrorCode::ResultUnknown;
        case Outcome::Completed:
        case Outcome::Replayed:
        case Outcome::Progressed:
        case Outcome::Ignored:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int TaskCompletionService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Completed || outcome == Outcome::Replayed ||
        outcome == Outcome::Progressed || outcome == Outcome::Ignored) {
        return 200;
    }
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// 结算
// =====================================================================

void TaskCompletionService::completeTask(const std::string &userId,
                                         const std::string &taskId,
                                         const std::string &idempotencyKey,
                                         Callback cb,
                                         SettlementFault fault) {
    runTaskAction(userId, taskId, idempotencyKey, std::move(cb), fault, false);
}

void TaskCompletionService::progressTask(const std::string &userId, const std::string &taskId,
                                        const std::string &idempotencyKey, Callback cb, SettlementFault fault) {
    runTaskAction(userId, taskId, idempotencyKey, std::move(cb), fault, true);
}

void TaskCompletionService::progressDailyTask(const std::string &userId, const std::string &taskId,
                                             const std::string &key, const std::string &periodDate,
                                             Callback cb, SettlementFault fault) {
    if (!TaskService::validPeriodDate(periodDate)) {
        cb(makeStatus(Outcome::Validation, "周期日期不合法"));
        return;
    }
    runTaskAction(userId, taskId, key, std::move(cb), fault, true, periodDate);
}

void TaskCompletionService::runTaskAction(const std::string &userId, const std::string &taskId,
                                         const std::string &idempotencyKey, Callback cb,
                                         SettlementFault fault, bool progress, const std::string &periodDate) {
    if (userId.empty() || taskId.empty() || !TaskService::validIdempotencyKey(idempotencyKey)) {
        cb(makeStatus(Outcome::Validation, "请求标识不合法", label("input", "invalid_request", progress)));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label("pool", "not_ready", progress)));
        return;
    }

    auto ctx = std::make_shared<CompleteCtx>();
    ctx->userId = userId;
    ctx->requestedTaskId = taskId;
    ctx->key = idempotencyKey;
    ctx->fault = fault;
    ctx->progressAction = progress;
    ctx->periodDate = periodDate;
    if (progress) ctx->eventId = newOperationId();
    ctx->operationId = newOperationId();
    ctx->done = std::move(cb);

    // onReady 只在 begin() 内同步调用，可以强捕获；保存进 TxScope 的错误回调只弱捕获，
    // 并忽略收到的消息（可能含事务创建异常文本），只依据本服务记录的状态生成固定标签。
    std::weak_ptr<CompleteCtx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            stepTask(ctx);
        },
        [weakCtx](const std::string &) {
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(Outcome::ResultUnknown, "", label("commit", "commit_unconfirmed", strong->progressAction)));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(Outcome::Error, "", label("transaction", "begin_failed", strong->progressAction)));
                }
                return;
            }
            LOG_ERROR << "TaskCompletionService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

}  // namespace usapp
