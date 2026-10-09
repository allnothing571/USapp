// =====================================================================
// services/api/tests/TaskLedgerDbTests.h
//
// 阶段 1 M3-A：任务表与任务结算一致性的数据库回归测试（由 DbGatedTest.cc 的 main 调用）。
//
// 沿用 M2-A 的做法（WalletLedgerProbe.h / WalletLedgerDbTests.h）：
//   * 以 usapp_api 运行，目标库由 DbGatedTest 的 current_database() 守卫限定为 usapp_test；
//   * 每个“预期失败”占用一个独立事务，目标语句经 DO 包装暴露 SQLSTATE，
//     DO 仍以错误结束，Drogon 回滚整个事务；不使用 ROLLBACK TO SAVEPOINT；
//   * 正向用例以 SET CONSTRAINTS ALL IMMEDIATE 触发延迟校验后回滚，从不提交；
//   * 独立观察者确认服务器端事务结束、锁已释放，且没有可见的已提交夹具或任务行；
//   * 唯一的真实提交尝试是一个“非法结算”，提交必须失败。
//
// M3-A 限制（如实记录，不冒充已覆盖）：
//   * 所有成功夹具都只在回滚事务内存在。“对已真正提交的完成记录，在另一个事务里追加
//     流水”的攻击需要已提交的结算，属于 M3-C 的要求；这里只在同一回滚事务中，
//     先让一次合法结算通过 SET CONSTRAINTS ALL IMMEDIATE，再追加并重新校验。
//   * 没有任务服务、没有接口、没有并发结算：这些属于 M3-B / M3-C。
// =====================================================================
#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "WalletLedgerDbTests.h"
#include "WalletLedgerProbe.h"
#include "db/TaskPrivilegePolicy.h"

namespace usapptest::task {

using wallet::accountSql;
using wallet::CaseContext;
using wallet::ClientPtr;
using wallet::commitTx;
using wallet::entrySql;
using wallet::exec;
using wallet::ExecOutcome;
using wallet::expectFailureSql;
using wallet::expectThat;
using wallet::FailureCode;
using wallet::Fixture;
using wallet::newUuid;
using wallet::Observer;
using wallet::parseFailure;
using wallet::uuidLiteral;

// ---------------------------------------------------------------- 字面量 SQL 构造
inline std::string taskKey() {
    return "task-test-key-" + usapp::crypto::randomHex(8);  // 30 个字符，满足 16-128
}

inline std::string taskFingerprint() {
    return usapp::crypto::randomHex(32);  // 64 个小写十六进制字符
}

// 截止时间的三种写法：无截止；相对当前时刻（本地时间截到秒）；只有本地日期。
struct Deadline {
    enum class Kind { None, Relative, DateOnly, Raw } kind = Kind::None;
    std::string value;  // Relative: interval 文本（如 '1 day'、'-1 hour'）；DateOnly: 'YYYY-MM-DD'
    // Raw：直接给出三个 SQL 表达式（用于不一致的对抗用例）
    std::string rawDate = "NULL::date";
    std::string rawTime = "NULL::time";
    std::string rawAt = "NULL::timestamptz";
};

inline Deadline noDeadline() { return Deadline{}; }

inline Deadline relativeDeadline(const std::string &interval) {
    Deadline d;
    d.kind = Deadline::Kind::Relative;
    d.value = interval;
    return d;
}

inline Deadline dateOnlyDeadline(const std::string &localDate) {
    Deadline d;
    d.kind = Deadline::Kind::DateOnly;
    d.value = localDate;
    return d;
}

inline Deadline rawDeadline(const std::string &date, const std::string &time, const std::string &at) {
    Deadline d;
    d.kind = Deadline::Kind::Raw;
    d.rawDate = date;
    d.rawTime = time;
    d.rawAt = at;
    return d;
}

struct TaskRow {
    std::string id = newUuid();
    int flowers = 5;
    int sunshine = 40;
    std::string titleSql = "'Read one chapter'";
    // 描述含 LF：证明 LF 被允许。
    std::string descriptionSql = "'First line' || chr(10) || 'second line'";
    Deadline deadline = noDeadline();
    std::string key = taskKey();
    std::string fingerprint = taskFingerprint();
    std::string timezoneSql = "'Asia/Shanghai'";
};

inline std::string deadlineSource(const Deadline &d) {
    switch (d.kind) {
        case Deadline::Kind::None:
            return "(SELECT NULL::date AS ld, NULL::time AS lt, NULL::timestamptz AS at) d";
        case Deadline::Kind::Relative:
            return "(SELECT x.l::date AS ld, x.l::time AS lt, "
                   "(x.l::date + x.l::time) AT TIME ZONE 'Asia/Shanghai' AS at "
                   "FROM (SELECT date_trunc('second', (clock_timestamp() + interval '" +
                   d.value + "') AT TIME ZONE 'Asia/Shanghai') AS l) x) d";
        case Deadline::Kind::DateOnly:
            return "(SELECT '" + d.value + "'::date AS ld, NULL::time AS lt, ('" + d.value +
                   "'::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' AS at) d";
        case Deadline::Kind::Raw:
            return "(SELECT " + d.rawDate + " AS ld, " + d.rawTime + " AS lt, " + d.rawAt + " AS at) d";
    }
    return "";
}

inline std::string taskSql(const TaskRow &t,
                           const std::string &coupleId,
                           const std::string &creatorId,
                           const std::string &assigneeId) {
    return "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers, "
           "reward_sunshine, timezone, deadline_local_date, deadline_local_time, deadline_at, "
           "create_idempotency_key, create_fingerprint) SELECT " +
           uuidLiteral(t.id) + ", " + uuidLiteral(coupleId) + ", " + uuidLiteral(creatorId) + ", " +
           uuidLiteral(assigneeId) + ", " + t.titleSql + ", " + t.descriptionSql + ", " +
           std::to_string(t.flowers) + ", " + std::to_string(t.sunshine) + ", " + t.timezoneSql +
           ", d.ld, d.lt, d.at, '" + t.key + "', '" + t.fingerprint + "' FROM " + deadlineSource(t.deadline);
}

inline std::string taskOperationSql(const std::string &opId,
                                    const std::string &coupleId,
                                    const std::string &actorId,
                                    const std::string &taskId,
                                    const std::string &type = "TASK_REWARD",
                                    const std::string &sourceType = "task") {
    return "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id) "
           "VALUES (" +
           uuidLiteral(opId) + ", " + uuidLiteral(coupleId) + ", " + uuidLiteral(actorId) + ", '" + type +
           "', '" + sourceType + "', '" + taskId + "')";
}

inline std::string completionSql(const std::string &taskId,
                                 const std::string &coupleId,
                                 const std::string &completedBy,
                                 const std::string &opId,
                                 int flowers,
                                 int sunshine,
                                 const std::string &completedAtSql = "clock_timestamp()") {
    return "INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers, "
           "reward_sunshine, completed_at) VALUES (" +
           uuidLiteral(taskId) + ", " + uuidLiteral(coupleId) + ", " + uuidLiteral(completedBy) + ", " +
           uuidLiteral(opId) + ", " + std::to_string(flowers) + ", " + std::to_string(sunshine) + ", " +
           completedAtSql + ")";
}

// 一次合法结算（A 发布、B 完成，空间一），顺序与设计第 4.3 节一致：
// 账户 -> 操作 -> 完成记录 -> 每个正奖励资产一条流水（各自一条语句）。
inline std::vector<std::string> validSettlement(const Fixture &f, const TaskRow &t, const std::string &opId) {
    std::vector<std::string> sql;
    sql.push_back(taskSql(t, f.coupleOne, f.userA, f.userB));
    if (t.flowers > 0) sql.push_back(accountSql(f.userB, f.coupleOne, "flower"));
    if (t.sunshine > 0) sql.push_back(accountSql(f.userB, f.coupleOne, "sunshine"));
    sql.push_back(taskOperationSql(opId, f.coupleOne, f.userB, t.id));
    sql.push_back(completionSql(t.id, f.coupleOne, f.userB, opId, t.flowers, t.sunshine));
    if (t.flowers > 0) sql.push_back(entrySql(opId, f.coupleOne, f.userB, "flower", 1, t.flowers, t.flowers));
    if (t.sunshine > 0) sql.push_back(entrySql(opId, f.coupleOne, f.userB, "sunshine", 1, t.sunshine, t.sunshine));
    return sql;
}

inline std::vector<std::string> plus(std::vector<std::string> base, const std::vector<std::string> &more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}

// ---------------------------------------------------------------- 已提交行检查
// 只能证明“观察者看不到已提交的任务行”，不能证明回滚已完成（见 teardown 证据）。
inline long long committedTaskRowsFor(const Observer &observer, const Fixture &f) {
    long long total = 0;
    for (const auto &couple : {f.coupleOne, f.coupleTwo}) {
        const auto rows = observer.client->execSqlSync(
            "SELECT ((SELECT count(*) FROM tasks WHERE couple_id = $1::uuid) "
            "      + (SELECT count(*) FROM task_completions WHERE couple_id = $1::uuid))::bigint AS n",
            couple);
        total += rows[0]["n"].as<long long>();
    }
    return total;
}

inline void closeTaskCase(const Observer &observer, CaseContext &ctx, const std::string &label) {
    wallet::closeCase(observer, ctx, label);  // 回滚、teardown 证据、已提交夹具/钱包行检查
    try {
        const long long committed = committedTaskRowsFor(observer, ctx.f);
        expectThat(committed == 0, label, "观察者看到已提交的任务行: " + std::to_string(committed));
    } catch (const std::exception &e) {
        expectThat(false, label, std::string("任务行检查失败: ") + e.what());
    }
}

using SqlList = std::function<std::vector<std::string>(const Fixture &)>;
using SqlOne = std::function<std::string(const Fixture &)>;

inline void expectTaskFailure(const ClientPtr &pool,
                              const Observer &observer,
                              const std::string &label,
                              const SqlList &setup,
                              const SqlOne &failing,
                              const std::string &expectedState,
                              const std::string &expectedConstraint = "") {
    CaseContext ctx;
    if (wallet::openCase(pool, ctx, label) && wallet::runSetup(ctx, setup(ctx.f), label)) {
        const ExecOutcome out = exec(ctx.tx, expectFailureSql(failing(ctx.f)));
        if (!out.completed) {
            expectThat(false, label, "目标语句在有界时间内没有结果");
        } else if (out.ok) {
            expectThat(false, label, "目标语句意外成功");
        } else {
            const FailureCode code = parseFailure(out.error);
            expectThat(!code.unexpectedSuccess, label, "目标语句意外成功（DO 块报告）");
            expectThat(code.sqlState == expectedState, label,
                       "SQLSTATE 期望 " + expectedState + " 实际 " +
                           (code.sqlState.empty() ? "<无标记>" : code.sqlState));
            if (!expectedConstraint.empty()) {
                expectThat(code.constraint == expectedConstraint, label,
                           "约束期望 " + expectedConstraint + " 实际 " + code.constraint);
            }
        }
    }
    closeTaskCase(observer, ctx, label);
}

inline void expectTaskSuccess(const ClientPtr &pool,
                              const Observer &observer,
                              const std::string &label,
                              const SqlList &setup,
                              const std::function<void(CaseContext &)> &verify = nullptr) {
    CaseContext ctx;
    if (wallet::openCase(pool, ctx, label) && wallet::runSetup(ctx, setup(ctx.f), label)) {
        const ExecOutcome immediate = exec(ctx.tx, "SET CONSTRAINTS ALL IMMEDIATE");
        expectThat(immediate.completed && immediate.ok, label, "SET CONSTRAINTS ALL IMMEDIATE 未成功");
        if (immediate.completed && immediate.ok && verify) {
            verify(ctx);
        }
    }
    closeTaskCase(observer, ctx, label);
}

// ---------------------------------------------------------------- 用例
inline void runTaskLedgerDbTests(const ClientPtr &pool, const std::string &dsn) {
    std::optional<Observer> observerHolder;
    try {
        observerHolder = Observer::connect(dsn);
    } catch (const std::exception &e) {
        TEST_CASE("Task: 观察者连接") {
            expectThat(false, "task-observer", e.what());
        }
        return;
    }
    const Observer &observer = *observerHolder;

    // T1. 任务表有效权限矩阵（与钱包检查共用身份要求；本阶段不接入 API 启动）
    TEST_CASE("Task: 有效权限矩阵（SELECT/INSERT only；push/public 无；无列级 UPDATE）") {
        auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto future = promise->get_future();
        usapp::db::TaskPrivilegePolicy::check(
            pool,
            [promise](usapp::db::WalletPrivilegeReport report) {
                try {
                    promise->set_value({report.ok(), usapp::db::TaskPrivilegePolicy::summary(report)});
                } catch (...) {
                }
            },
            [promise](const std::string &error) {
                try {
                    promise->set_value({false, "query error: " + error});
                } catch (...) {
                }
            });
        if (future.wait_for(wallet::kExecTimeout) != std::future_status::ready) {
            expectThat(false, "task-privilege-matrix", "矩阵查询在有界时间内没有结果");
        } else {
            const auto result = future.get();
            usapptest::note(result.second);  // 只含角色名、表/列/权限名与布尔值
            expectThat(result.first, "task-privilege-matrix", "任务表有效权限矩阵与设计第 3.1 节不符");
        }
    }

    // T2. 触发器目录
    TEST_CASE("Task: 触发器存在且启用") {
        const std::vector<std::pair<std::string, std::string>> expected = {
            {"tasks", "tasks_append_only"},
            {"task_completions", "task_completions_append_only"},
            {"task_completions", "task_completions_guard"},
            {"task_completions", "task_completions_settlement_valid"},
            {"wallet_operations", "wallet_operations_task_settlement_valid"},
            {"wallet_ledger_entries", "wallet_ledger_entries_task_settlement_valid"},
        };
        for (const auto &item : expected) {
            try {
                const auto rows = observer.client->execSqlSync(
                    "SELECT t.tgenabled::text AS enabled FROM pg_trigger t "
                    "JOIN pg_class c ON c.oid = t.tgrelid "
                    "WHERE c.relname = $1 AND t.tgname = $2 AND NOT t.tgisinternal",
                    item.first, item.second);
                expectThat(rows.size() == 1u, item.second, "触发器不存在");
                if (rows.size() == 1u) {
                    expectThat(rows[0]["enabled"].as<std::string>() == "O", item.second, "触发器未处于启用状态 O");
                }
            } catch (const std::exception &e) {
                expectThat(false, item.second, std::string("目录查询失败: ") + e.what());
            }
        }
    }

    // T3. 正向：合法结算（双资产、单资产）、非任务操作兼容、截止时间计算
    TEST_CASE("Task: 合法结算通过延迟校验；非任务钱包操作不受影响") {
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskSuccess(pool, observer, "valid-settlement-both-assets",
                              [t, op](const Fixture &f) { return validSettlement(f, t, op); });
        }
        {
            TaskRow t;
            t.flowers = 3;
            t.sunshine = 0;
            const std::string op = newUuid();
            expectTaskSuccess(pool, observer, "valid-settlement-flower-only",
                              [t, op](const Fixture &f) { return validSettlement(f, t, op); });
        }
        {
            TaskRow t;
            t.flowers = 0;
            t.sunshine = 100000;
            t.deadline = relativeDeadline("1 day");
            const std::string op = newUuid();
            expectTaskSuccess(pool, observer, "valid-settlement-sunshine-only-with-deadline",
                              [t, op](const Fixture &f) { return validSettlement(f, t, op); });
        }
        {
            // M2-A 风格的非任务操作（source_type = 'wallet_test'，类型 TASK_REWARD）不受任务校验影响。
            const std::string op = newUuid();
            expectTaskSuccess(pool, observer, "non-task-operation-compatible", [op](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, "flower"),
                                                wallet::operationSql(op, f.coupleOne, f.userA),
                                                entrySql(op, f.coupleOne, f.userA, "flower", 1, 7, 7)};
            });
        }
        {
            // 只有日期：截止于次日本地 0 点；发布时不要求截止时间在未来（过去日期也能插入）。
            TaskRow t;
            t.deadline = dateOnlyDeadline("2026-10-20");
            TaskRow past;
            past.deadline = relativeDeadline("-1 hour");
            expectTaskSuccess(
                pool, observer, "deadline-date-only-and-past-deadline-insert",
                [t, past](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    taskSql(past, f.coupleOne, f.userB, f.userA)};
                },
                [t](CaseContext &ctx) {
                    const ExecOutcome out = exec(
                        ctx.tx,
                        "SELECT (deadline_at = timestamptz '2026-10-21 00:00:00+08') AS ok FROM tasks WHERE id = $1::uuid",
                        t.id);
                    expectThat(out.completed && out.ok && out.result && out.result->size() == 1u &&
                                   (*out.result)[0]["ok"].as<bool>(),
                               "deadline-date-only", "只有日期的截止时刻应为次日本地 0 点");
                });
        }
    }

    // T4. 任务约束（D22/D22a/D23/D5/D7/D25）
    TEST_CASE("Task: 奖励、标题、描述、截止时间、成员与幂等约束") {
        struct Case {
            std::string label;
            std::function<void(TaskRow &)> mutate;
            std::string state;
            std::string constraint;
        };
        const std::vector<Case> cases = {
            {"rewards-both-zero", [](TaskRow &t) { t.flowers = 0; t.sunshine = 0; }, "23514", "tasks_reward_positive"},
            {"flowers-above-999", [](TaskRow &t) { t.flowers = 1000; }, "23514", "tasks_reward_flowers_range"},
            {"flowers-negative", [](TaskRow &t) { t.flowers = -1; }, "23514", "tasks_reward_flowers_range"},
            {"sunshine-above-100000", [](TaskRow &t) { t.sunshine = 100001; }, "23514", "tasks_reward_sunshine_range"},
            {"title-empty", [](TaskRow &t) { t.titleSql = "''"; }, "23514", "tasks_title_shape"},
            {"title-41-code-points", [](TaskRow &t) { t.titleSql = "repeat(chr(21517), 41)"; }, "23514", "tasks_title_shape"},
            {"title-untrimmed", [](TaskRow &t) { t.titleSql = "' padded'"; }, "23514", "tasks_title_shape"},
            {"title-line-break", [](TaskRow &t) { t.titleSql = "'a' || chr(10) || 'b'"; }, "23514", "tasks_title_shape"},
            {"title-control-char", [](TaskRow &t) { t.titleSql = "'a' || chr(7) || 'b'"; }, "23514", "tasks_title_shape"},
            {"description-501-code-points", [](TaskRow &t) { t.descriptionSql = "repeat('x', 501)"; }, "23514", "tasks_description_shape"},
            {"description-carriage-return", [](TaskRow &t) { t.descriptionSql = "'a' || chr(13) || chr(10) || 'b'"; }, "23514", "tasks_description_shape"},
            {"description-tab", [](TaskRow &t) { t.descriptionSql = "'a' || chr(9) || 'b'"; }, "23514", "tasks_description_shape"},
            {"timezone-unsupported", [](TaskRow &t) { t.timezoneSql = "'UTC'"; }, "23514", "tasks_timezone_supported"},
            {"deadline-time-without-date",
             [](TaskRow &t) { t.deadline = rawDeadline("NULL::date", "time '23:00'", "NULL::timestamptz"); },
             "23514", "tasks_deadline_time_requires_date"},
            {"deadline-date-without-instant",
             [](TaskRow &t) { t.deadline = rawDeadline("date '2026-10-20'", "NULL::time", "NULL::timestamptz"); },
             "23514", "tasks_deadline_consistency"},
            {"deadline-instant-mismatch",
             [](TaskRow &t) {
                 t.deadline = rawDeadline("date '2026-10-20'", "time '23:00'", "timestamptz '2026-10-20 23:00:00+00'");
             },
             "23514", "tasks_deadline_at_matches_local"},
            {"create-key-too-short", [](TaskRow &t) { t.key = "short-key-15chr"; }, "23514", "tasks_create_key_shape"},
            {"create-fingerprint-bad", [](TaskRow &t) { t.fingerprint = "XYZ"; }, "23514", "tasks_create_fingerprint_shape"},
        };
        for (const auto &c : cases) {
            TaskRow t;
            c.mutate(t);
            expectTaskFailure(pool, observer, c.label, [](const Fixture &) { return std::vector<std::string>{}; },
                              [t](const Fixture &f) { return taskSql(t, f.coupleOne, f.userA, f.userB); },
                              c.state, c.constraint);
        }
        {
            TaskRow t;
            expectTaskFailure(pool, observer, "self-assignment",
                              [](const Fixture &) { return std::vector<std::string>{}; },
                              [t](const Fixture &f) { return taskSql(t, f.coupleOne, f.userA, f.userA); },
                              "23514", "tasks_creator_not_assignee");
        }
        {
            TaskRow t;
            expectTaskFailure(pool, observer, "assignee-not-member",
                              [](const Fixture &) { return std::vector<std::string>{}; },
                              [t](const Fixture &f) { return taskSql(t, f.coupleOne, f.userA, f.userC); },
                              "23503", "tasks_assignee_member_fk");
        }
        {
            TaskRow first;
            TaskRow second;
            second.key = first.key;
            expectTaskFailure(pool, observer, "create-key-duplicate",
                              [first](const Fixture &f) {
                                  return std::vector<std::string>{taskSql(first, f.coupleOne, f.userA, f.userB)};
                              },
                              [second](const Fixture &f) { return taskSql(second, f.coupleOne, f.userA, f.userB); },
                              "23505", "tasks_create_key_unique");
        }
    }

    // T5. 完成记录的立即校验（D34 第 1 项）
    TEST_CASE("Task: 完成记录立即校验（任务/空间、接收者、快照、操作链接、时间）") {
        auto base = [](const TaskRow &t, const std::string &op, const std::string &actor) {
            return [t, op, actor](const Fixture &f) {
                const std::string actorId = actor == "A" ? f.userA : f.userB;
                return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                taskOperationSql(op, f.coupleOne, actorId, t.id)};
            };
        };
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-by-publisher", base(t, op, "A"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userA, op, t.flowers, t.sunshine);
                              },
                              "UT003");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-snapshot-mismatch", base(t, op, "B"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers - 1, t.sunshine);
                              },
                              "UT004");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-missing-task",
                              [](const Fixture &) { return std::vector<std::string>{}; },
                              [t, op](const Fixture &f) {
                                  return completionSql(newUuid(), f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                              },
                              "UT002");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-couple-mismatch", base(t, op, "B"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleTwo, f.userB, op, t.flowers, t.sunshine);
                              },
                              "UT002");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(
                pool, observer, "completion-operation-wrong-type",
                [t, op](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    taskOperationSql(op, f.coupleOne, f.userB, t.id, "REWARD_REDEMPTION")};
                },
                [t, op](const Fixture &f) {
                    return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                },
                "UT005");
        }
        {
            // 同一空间里另一个任务的操作（来源指向别的任务）不能用来完成本任务。
            TaskRow t;
            TaskRow other;
            const std::string op = newUuid();
            expectTaskFailure(
                pool, observer, "completion-operation-other-task",
                [t, other, op](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    taskSql(other, f.coupleOne, f.userA, f.userB),
                                                    taskOperationSql(op, f.coupleOne, f.userB, other.id)};
                },
                [t, op](const Fixture &f) {
                    return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                },
                "UT005");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-operation-actor-is-publisher", base(t, op, "A"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                              },
                              "UT005");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-operation-missing",
                              [t](const Fixture &f) {
                                  return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB)};
                              },
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                              },
                              "UT005");
        }
        {
            TaskRow t;
            t.deadline = relativeDeadline("-1 hour");
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-at-or-after-deadline", base(t, op, "B"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine);
                              },
                              "UT006");
        }
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "completion-in-the-future", base(t, op, "B"),
                              [t, op](const Fixture &f) {
                                  return completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine,
                                                       "clock_timestamp() + interval '1 hour'");
                              },
                              "UT006");
        }
        {
            // 同一任务的第二个任务奖励操作被 0006 的业务来源唯一键拒绝。
            TaskRow t;
            const std::string op = newUuid();
            const std::string op2 = newUuid();
            expectTaskFailure(pool, observer, "second-operation-same-task",
                              [t, op](const Fixture &f) { return validSettlement(f, t, op); },
                              [t, op2](const Fixture &f) {
                                  return taskOperationSql(op2, f.coupleOne, f.userB, t.id);
                              },
                              "23505", "wallet_operations_source_unique");
        }
        {
            // 同一任务的第二条完成记录：引用另一个（来源不同）的操作时，立即校验先拒绝。
            TaskRow t;
            TaskRow other;
            other.flowers = t.flowers;
            other.sunshine = t.sunshine;
            const std::string op = newUuid();
            const std::string otherOp = newUuid();
            expectTaskFailure(
                pool, observer, "operation-reused-for-second-completion",
                [t, other, op, otherOp](const Fixture &f) {
                    return plus(validSettlement(f, t, op),
                                {taskSql(other, f.coupleOne, f.userA, f.userB),
                                 taskOperationSql(otherOp, f.coupleOne, f.userB, other.id)});
                },
                [other, op](const Fixture &f) {
                    return completionSql(other.id, f.coupleOne, f.userB, op, other.flowers, other.sunshine);
                },
                "UT005");
        }
    }

    // T6. 共享结算校验器（延迟；D34 第 2 项）
    TEST_CASE("Task: 结算条目集合与完成记录不符时在 SET CONSTRAINTS IMMEDIATE 被拒绝") {
        const SqlOne immediate = [](const Fixture &) { return std::string("SET CONSTRAINTS ALL IMMEDIATE"); };
        {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(
                pool, observer, "task-operation-without-completion",
                [t, op](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    accountSql(f.userB, f.coupleOne, "flower"),
                                                    taskOperationSql(op, f.coupleOne, f.userB, t.id),
                                                    entrySql(op, f.coupleOne, f.userB, "flower", 1, t.flowers, t.flowers)};
                },
                immediate, "UT007");
        }
        {
            // 奖励两种资产但只有小红花流水（0006 的“操作至少一条流水”满足，任务校验不满足）。
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(
                pool, observer, "missing-entry-for-rewarded-asset",
                [t, op](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    accountSql(f.userB, f.coupleOne, "flower"),
                                                    taskOperationSql(op, f.coupleOne, f.userB, t.id),
                                                    completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine),
                                                    entrySql(op, f.coupleOne, f.userB, "flower", 1, t.flowers, t.flowers)};
                },
                immediate, "UT007");
        }
        {
            TaskRow t;
            t.sunshine = 0;
            const std::string op = newUuid();
            expectTaskFailure(
                pool, observer, "amount-differs-from-snapshot",
                [t, op](const Fixture &f) {
                    return std::vector<std::string>{taskSql(t, f.coupleOne, f.userA, f.userB),
                                                    accountSql(f.userB, f.coupleOne, "flower"),
                                                    taskOperationSql(op, f.coupleOne, f.userB, t.id),
                                                    completionSql(t.id, f.coupleOne, f.userB, op, t.flowers, t.sunshine),
                                                    entrySql(op, f.coupleOne, f.userB, "flower", 1, t.flowers - 1,
                                                             t.flowers - 1)};
                },
                immediate, "UT007");
        }
        {
            // 合法结算之外，同一事务再给另一成员记一条。
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "extra-entry-other-member-same-transaction",
                              [t, op](const Fixture &f) {
                                  return plus(validSettlement(f, t, op),
                                              {accountSql(f.userA, f.coupleOne, "flower"),
                                               entrySql(op, f.coupleOne, f.userA, "flower", 1, 1, 1)});
                              },
                              immediate, "UT007");
        }
        {
            TaskRow t;
            t.sunshine = 0;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "extra-entry-unrewarded-asset-same-transaction",
                              [t, op](const Fixture &f) {
                                  return plus(validSettlement(f, t, op),
                                              {accountSql(f.userB, f.coupleOne, "sunshine"),
                                               entrySql(op, f.coupleOne, f.userB, "sunshine", 1, 1, 1)});
                              },
                              immediate, "UT007");
        }
        {
            // 后追加：先让合法结算通过 IMMEDIATE，再恢复延迟、追加另一成员的流水、再次 IMMEDIATE。
            // 这是“追加到已校验的结算”在回滚事务内的近似；跨事务追加到真正已提交的结算属于 M3-C。
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "late-append-other-member-after-valid-settlement",
                              [t, op](const Fixture &f) {
                                  return plus(validSettlement(f, t, op),
                                              {"SET CONSTRAINTS ALL IMMEDIATE", "SET CONSTRAINTS ALL DEFERRED",
                                               accountSql(f.userA, f.coupleOne, "flower"),
                                               entrySql(op, f.coupleOne, f.userA, "flower", 1, 1, 1)});
                              },
                              immediate, "UT007");
        }
        {
            TaskRow t;
            t.sunshine = 0;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, "late-append-unrewarded-asset-after-valid-settlement",
                              [t, op](const Fixture &f) {
                                  return plus(validSettlement(f, t, op),
                                              {"SET CONSTRAINTS ALL IMMEDIATE", "SET CONSTRAINTS ALL DEFERRED",
                                               accountSql(f.userB, f.coupleOne, "sunshine"),
                                               entrySql(op, f.coupleOne, f.userB, "sunshine", 1, 1, 1)});
                              },
                              immediate, "UT007");
        }
        {
            // 真实 COMMIT：非法结算（多记给另一成员）提交必须失败。若校验失效，夹具会被永久提交；
            // 用例会报告残留的已提交行（此时应视为严重缺陷）。
            const std::string label = "invalid-settlement-commit-rejected";
            TaskRow t;
            const std::string op = newUuid();
            CaseContext ctx;
            if (wallet::openCase(pool, ctx, label) &&
                wallet::runSetup(ctx,
                                 plus(validSettlement(ctx.f, t, op),
                                      {accountSql(ctx.f.userA, ctx.f.coupleOne, "flower"),
                                       entrySql(op, ctx.f.coupleOne, ctx.f.userA, "flower", 1, 1, 1)}),
                                 label)) {
                const std::optional<bool> committed = commitTx(ctx.tx);
                expectThat(committed.has_value(), label, "提交结果在有界时间内未返回");
                if (committed.has_value()) {
                    expectThat(!*committed, label, "提交意外成功：usapp_test 中已残留本用例的已提交夹具与任务行");
                }
            }
            closeTaskCase(observer, ctx, label);
        }
    }

    // T7. usapp_api 的只追加结果：权限屏障（42501）。维护角色上的触发器拒绝由 psql 脚本验证。
    TEST_CASE("Task: usapp_api 对任务表的 UPDATE / DELETE / TRUNCATE 为 42501") {
        struct Attempt {
            std::string label;
            std::function<std::string(const Fixture &, const TaskRow &)> sql;
        };
        const std::vector<Attempt> attempts = {
            {"update-tasks", [](const Fixture &, const TaskRow &t) {
                 return "UPDATE tasks SET title = title WHERE id = " + uuidLiteral(t.id);
             }},
            {"delete-tasks", [](const Fixture &, const TaskRow &t) {
                 return "DELETE FROM tasks WHERE id = " + uuidLiteral(t.id);
             }},
            {"truncate-tasks", [](const Fixture &, const TaskRow &) { return std::string("TRUNCATE tasks"); }},
            {"update-task-completions", [](const Fixture &, const TaskRow &t) {
                 return "UPDATE task_completions SET reward_flowers = reward_flowers WHERE task_id = " +
                        uuidLiteral(t.id);
             }},
            {"delete-task-completions", [](const Fixture &, const TaskRow &t) {
                 return "DELETE FROM task_completions WHERE task_id = " + uuidLiteral(t.id);
             }},
            {"truncate-task-completions",
             [](const Fixture &, const TaskRow &) { return std::string("TRUNCATE task_completions"); }},
        };
        for (const auto &a : attempts) {
            TaskRow t;
            const std::string op = newUuid();
            expectTaskFailure(pool, observer, a.label, [t, op](const Fixture &f) { return validSettlement(f, t, op); },
                              [a, t](const Fixture &f) { return a.sql(f, t); }, "42501");
        }
    }
}

}  // namespace usapptest::task
