// =====================================================================
// services/api/tests/TaskCompletionDbTests.h
//
// 阶段 1 M3-C1：真实 TaskCompletionService 的结算测试 R1-R9（docs/phase1-m3c-design.md
// 修订 3 第 6.3 节），由 DbGatedTest.cc 的 main 在 current_database() = 'usapp_test'
// 守卫之后调用。
//
// 夹具（D36，2026-10-14 批准）：每次运行使用随机 uuid 与 @m3-fixture.test 域名的用户、空间、
// 任务，并**永久提交**真实结算（任务来源的钱包操作、账户、流水、完成记录、审计）以及少量
// 非任务来源的夹具/持锁操作（source_type = 'wallet_test'）。断言只针对本次标识；
// 不断言全局计数或空表；不删除、不绕过触发器或权限、不重置数据库。
// 不含 D33 的临界余额夹具：真实服务路径上的 WALLET_BALANCE_LIMIT 拒绝本批不测试。
//
// 证据边界：提交前拒绝的“回滚回调”只表示已请求回滚；这里另外观察锁释放与已提交行不存在。
// 有界等待：服务调用、锁等待观察、截止时间等待与 teardown 都有上限。
// =====================================================================
#pragma once

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "TaskLedgerDbTests.h"
#include "TaskServiceDbTests.h"
#include "TestSupport.h"
#include "WalletLedgerDbTests.h"
#include "WalletLedgerProbe.h"
#include "services/TaskCompletionService.h"
#include "services/TaskService.h"
#include "services/WalletService.h"

namespace usapptest::taskcompletion {

using usapp::TaskCompletionService;
using usapp::TaskService;
using wallet::acquireTx;
using wallet::ClientPtr;
using wallet::commitTx;
using wallet::exec;
using wallet::expectThat;
using wallet::kExecTimeout;
using wallet::Millis;
using wallet::newUuid;
using wallet::Observer;
using wallet::rollbackTx;
using wallet::teardownEnded;
using wallet::TxPtr;
namespace ts = usapptest::taskservice;

using Fault = TaskCompletionService::SettlementFault;
using Outcome = TaskCompletionService::Outcome;
using PendingCompletion = ts::Pending<TaskCompletionService::Status>;

// ---------------------------------------------------------------- 服务调用
inline std::shared_ptr<PendingCompletion> startComplete(const std::string &userId,
                                                        const std::string &taskId,
                                                        const std::string &key,
                                                        Fault fault = Fault::None) {
    auto pending = std::make_shared<PendingCompletion>();
    TaskCompletionService::instance().completeTask(
        userId, taskId, key, [pending](TaskCompletionService::Status s) { pending->deliver(std::move(s)); }, fault);
    return pending;
}

inline std::optional<TaskCompletionService::Status> runComplete(const std::string &userId,
                                                                const std::string &taskId,
                                                                const std::string &key,
                                                                Fault fault = Fault::None) {
    return startComplete(userId, taskId, key, fault)->wait();
}

// 结果回调按值持有哨兵：结束后哨兵必须在有界时间内释放，且回调恰好一次。
struct Lifetime {
    std::optional<TaskCompletionService::Status> status;
    int calls = 0;
    bool released = false;
};

inline Lifetime runWithSentinel(const std::string &userId,
                                const std::string &taskId,
                                const std::string &key,
                                Fault fault = Fault::None,
                                const std::function<void()> &whileInFlight = nullptr) {
    auto pending = std::make_shared<PendingCompletion>();
    auto sentinel = std::make_shared<int>(3);
    const std::weak_ptr<int> watch = sentinel;
    TaskCompletionService::instance().completeTask(
        userId, taskId, key,
        [pending, sentinel](TaskCompletionService::Status s) { pending->deliver(std::move(s)); }, fault);
    sentinel.reset();
    if (whileInFlight) whileInFlight();
    Lifetime out;
    out.status = pending->wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!watch.expired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(Millis(10));
    }
    out.released = watch.expired();
    out.calls = pending->calls();
    return out;
}

inline bool is(const std::optional<TaskCompletionService::Status> &s, Outcome outcome) {
    return s.has_value() && s->outcome == outcome;
}

inline std::string bodyText(const TaskCompletionService::Status &status) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, TaskCompletionService::responseBody(status));
}

// 通过真实发布服务创建任务（产生 task.created 审计）；失败即抛异常。
inline std::string publish(const std::string &creator, const std::string &title, int flowers, int sunshine) {
    const auto created = ts::runCreate(creator, ts::simpleInput(title, flowers, sunshine), newUuid());
    if (!created || created->outcome != TaskService::Outcome::Created) {
        throw std::runtime_error("fixture publish failed");
    }
    return created->task.id;
}

inline std::optional<usapp::WalletBalances> walletOf(const std::string &userId) {
    auto pending = std::make_shared<ts::Pending<std::pair<usapp::WalletService::Outcome, usapp::WalletBalances>>>();
    usapp::WalletService::instance().getOwnWallet(
        userId, [pending](usapp::WalletService::Outcome o, usapp::WalletBalances b) { pending->deliver({o, b}); });
    const auto reply = pending->wait();
    if (!reply || reply->first != usapp::WalletService::Outcome::Ok) return std::nullopt;
    return reply->second;
}

// ---------------------------------------------------------------- 观察者查询（只按本次标识过滤）
inline long long scalar(const Observer &o, const std::string &sql, const std::string &a, const std::string &b = "") {
    const auto rows = b.empty() ? o.client->execSqlSync(sql, a) : o.client->execSqlSync(sql, a, b);
    return rows[0]["n"].as<long long>();
}

inline long long taskOps(const Observer &o, const std::string &taskId) {
    return scalar(o,
                  "SELECT count(*)::bigint AS n FROM wallet_operations "
                  "WHERE operation_type = 'TASK_REWARD' AND source_type = 'task' AND source_id = $1",
                  taskId);
}

inline long long completions(const Observer &o, const std::string &taskId) {
    return scalar(o, "SELECT count(*)::bigint AS n FROM task_completions WHERE task_id = $1::uuid", taskId);
}

inline long long completedAudits(const Observer &o, const std::string &taskId) {
    return scalar(o,
                  "SELECT count(*)::bigint AS n FROM audit_logs "
                  "WHERE action = 'task.completed' AND target_type = 'task' AND target_id = $1",
                  taskId);
}

inline long long entriesForOp(const Observer &o, const std::string &opId) {
    return scalar(o, "SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE operation_id = $1::uuid", opId);
}

struct Latest {
    long long seq = 0;
    long long balance = 0;
    bool operator==(const Latest &other) const { return seq == other.seq && balance == other.balance; }
};

inline Latest latestEntry(const Observer &o, const std::string &userId, const std::string &asset) {
    const auto rows = o.client->execSqlSync(
        "SELECT COALESCE(max(seq), 0)::bigint AS seq, "
        "       COALESCE((SELECT balance_after FROM wallet_ledger_entries "
        "                  WHERE user_id = $1::uuid AND asset_type = $2 ORDER BY seq DESC LIMIT 1), 0)::bigint "
        "         AS balance "
        "FROM wallet_ledger_entries WHERE user_id = $1::uuid AND asset_type = $2",
        userId, asset);
    return Latest{rows[0]["seq"].as<long long>(), rows[0]["balance"].as<long long>()};
}

// 观察者用 NOWAIT 尝试锁住账户（单语句自动提交，取得即释放）；成功说明没有其他事务持有该行锁。
inline bool accountLockReleased(const Observer &o, const std::string &userId, const std::string &asset,
                                Millis timeout = Millis(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            o.client->execSqlSync(
                "SELECT 1 FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = $2 FOR UPDATE NOWAIT",
                userId, asset);
            return true;
        } catch (const std::exception &) {
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(wallet::kPollStep);
    }
}

// 有界观察：哪个后端正在等待专用持锁连接（holderPid）释放的锁。返回等待者的 PID，
// 超时返回 0。只认 pg_blocking_pids 中包含 holderPid、且处于 Lock 等待的后端，
// 因此“请求还没结束”本身不算证据。
inline int observeBlockedBy(const Observer &o, int holderPid, Millis timeout = Millis(2000)) {
    if (holderPid == 0) return 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            const auto rows = o.client->execSqlSync(
                "SELECT a.pid FROM pg_stat_activity a "
                "WHERE a.datname = current_database() AND a.pid <> pg_backend_pid() "
                "  AND a.wait_event_type = 'Lock' AND $1::integer = ANY(pg_blocking_pids(a.pid)) "
                "ORDER BY a.pid LIMIT 1",
                std::to_string(holderPid));
            if (rows.size() == 1u) return rows[0]["pid"].as<int>();
        } catch (const std::exception &) {
        }
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        std::this_thread::sleep_for(wallet::kPollStep);
    }
}

inline long long msUntilDeadline(const Observer &o, const std::string &taskId) {
    const auto rows = o.client->execSqlSync(
        "SELECT GREATEST(0, CEIL(EXTRACT(EPOCH FROM (deadline_at - clock_timestamp())) * 1000))::bigint AS n "
        "FROM tasks WHERE id = $1::uuid",
        taskId);
    return rows[0]["n"].as<long long>();
}

// 独立单连接客户端上的持锁事务（不占用工作连接池）。
struct Holder {
    std::optional<Observer> conn;
    TxPtr tx;
    int pid = 0;
};

inline Holder lockAccount(const std::string &dsn, const std::string &userId, const std::string &asset) {
    Holder h;
    h.conn = Observer::connect(dsn);
    h.tx = acquireTx(h.conn->client);
    if (!h.tx) throw std::runtime_error("holder acquisition failed");
    const auto pid = exec(h.tx, "SELECT pg_backend_pid() AS pid");
    if (!pid.ok || !pid.result) throw std::runtime_error("holder pid failed");
    h.pid = (*pid.result)[0]["pid"].as<int>();
    const auto lock = exec(h.tx,
                           "SELECT 1 FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = $2 FOR UPDATE",
                           userId, asset);
    if (!lock.ok || !lock.result || lock.result->size() != 1u) throw std::runtime_error("holder lock failed");
    return h;
}

inline void endHolder(const Observer &observer, Holder &h, const std::string &label) {
    rollbackTx(h.tx);  // 已提交或已回滚时为无操作
    if (h.pid) {
        const auto teardown = teardownEnded(observer, h.pid);
        expectThat(teardown.ended, label, "holder teardown: " + teardown.observed);
        h.pid = 0;  // 只检查一次
    }
}

// ---------------------------------------------------------------- 用例
inline void runTaskCompletionDbTests(const ClientPtr &pool, const std::string &dsn) {
    std::optional<Observer> observerHolder;
    try {
        observerHolder = Observer::connect(dsn);
    } catch (const std::exception &) {
        TEST_CASE("Task completion: observer acquisition") { CHECK(false); }
        return;
    }
    const auto &observer = *observerHolder;

    // 共享空间 S：A 发布，B 接收（R1、R9、R2、R4、R3、R5、R8 共用）。
    ts::CommittedSpace s;
    std::string t1, t2, t3;
    std::optional<TaskCompletionService::Status> c1;
    bool sharedReady = false;
    try {
        ts::commitSpace(pool, s, true);
        sharedReady = true;
    } catch (const std::exception &) {
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task completion R1+R9: 三种奖励组合的真实结算、同事务审计、余额与 M3-B 读取回归") {
        const std::string label = "completion-r1";
        try {
            if (!sharedReady) throw std::runtime_error("shared fixture missing");
            t1 = publish(s.userA, "R1 flower", 5, 0);
            t2 = publish(s.userA, "R1 sunshine", 0, 40);
            t3 = publish(s.userA, "R1 both", 2, 10);

            // 第一次完成带哨兵：回调恰好一次并被释放。
            const auto first = runWithSentinel(s.userB, t1, newUuid());
            CHECK(is(first.status, Outcome::Completed));
            CHECK_EQ(first.calls, 1);
            CHECK(first.released);
            c1 = first.status;
            const auto c2 = runComplete(s.userB, t2, newUuid());
            const auto c3 = runComplete(s.userB, t3, newUuid());
            CHECK(is(c2, Outcome::Completed));
            CHECK(is(c3, Outcome::Completed));
            if (!is(c1, Outcome::Completed) || !is(c2, Outcome::Completed) || !is(c3, Outcome::Completed)) {
                throw std::runtime_error("settlement failed");
            }

            struct Expect {
                const std::string &task;
                const TaskCompletionService::Status &status;
                int flower;
                int sunshine;
                long long entries;
            };
            const std::vector<Expect> expects = {
                {t1, *c1, 5, 0, 1}, {t2, *c2, 0, 40, 1}, {t3, *c3, 2, 10, 2}};
            for (const auto &e : expects) {
                CHECK_EQ(e.status.settlement.flower, e.flower);
                CHECK_EQ(e.status.settlement.sunshine, e.sunshine);
                CHECK_EQ(e.status.task.id, e.task);
                CHECK_EQ(e.status.task.status, std::string("completed"));
                CHECK(TaskService::isCursorTimestamp(e.status.task.completedAt));
                CHECK_EQ(taskOps(observer, e.task), 1);
                CHECK_EQ(completions(observer, e.task), 1);
                CHECK_EQ(entriesForOp(observer, e.status.settlement.operationId), e.entries);
                CHECK_EQ(completedAudits(observer, e.task), 1);
                // 完成记录、操作、流水与审计在同一事务（xmin 相同）；审计只含操作 id 与金额。
                const auto same = observer.client->execSqlSync(
                    "SELECT count(DISTINCT x)::integer AS n FROM ("
                    " SELECT c.xmin::text AS x FROM task_completions c WHERE c.task_id = $1::uuid"
                    " UNION ALL SELECT o.xmin::text FROM wallet_operations o WHERE o.id = $2::uuid"
                    " UNION ALL SELECT e.xmin::text FROM wallet_ledger_entries e WHERE e.operation_id = $2::uuid"
                    " UNION ALL SELECT a.xmin::text FROM audit_logs a"
                    "   WHERE a.action = 'task.completed' AND a.target_id = $1::text) s",
                    e.task, e.status.settlement.operationId);
                CHECK_EQ(same[0]["n"].as<int>(), 1);
                const auto audit = observer.client->execSqlSync(
                    "SELECT (SELECT string_agg(k, ',' ORDER BY k) FROM jsonb_object_keys(a.detail) k) AS keys, "
                    "       a.detail->>'operation_id' AS op, (position('R1' in a.detail::text) > 0) AS leaks, "
                    "       a.actor_id::text AS actor "
                    "FROM audit_logs a WHERE a.action = 'task.completed' AND a.target_id = $1",
                    e.task);
                if (audit.size() == 1u) {
                    CHECK_EQ(audit[0]["keys"].as<std::string>(), std::string("flower,operation_id,sunshine"));
                    CHECK_EQ(audit[0]["op"].as<std::string>(), e.status.settlement.operationId);
                    CHECK_FALSE(audit[0]["leaks"].as<bool>());
                    CHECK_EQ(audit[0]["actor"].as<std::string>(), s.userB);
                }
            }
            // 流水链：B 的小红花 5 -> 7，阳光值 40 -> 50（B 是本次新用户）。
            CHECK(latestEntry(observer, s.userB, "flower") == (Latest{2, 7}));
            CHECK(latestEntry(observer, s.userB, "sunshine") == (Latest{2, 50}));
            const auto wallet = walletOf(s.userB);
            CHECK(wallet.has_value());
            if (wallet) {
                CHECK_EQ(wallet->flower, 7);
                CHECK_EQ(wallet->sunshine, 50);
            }

            // R9：M3-B 读取路径看到已完成状态与相同的完成时间；不在 open，在 history。
            const auto detail = ts::runGet(s.userA, t1);
            CHECK(detail.has_value() && detail->outcome == TaskService::Outcome::Ok);
            if (detail) {
                CHECK_EQ(detail->task.status, std::string("completed"));
                CHECK_EQ(detail->task.completedAt, c1->task.completedAt);
            }
            const auto open = ts::runList(s.userA, TaskService::View::Open, std::nullopt, 100);
            const auto history = ts::runList(s.userB, TaskService::View::History, std::nullopt, 100);
            CHECK(open.has_value() && history.has_value());
            if (open && history) {
                const auto openIds = ts::idsOf(open->tasks);
                const auto historyIds = ts::idsOf(history->tasks);
                for (const auto &id : {t1, t2, t3}) {
                    CHECK(std::find(openIds.begin(), openIds.end(), id) == openIds.end());
                    CHECK(std::find(historyIds.begin(), historyIds.end(), id) != historyIds.end());
                }
                for (const auto &task : history->tasks) {
                    if (task.id == t1 || task.id == t2 || task.id == t3) {
                        CHECK_EQ(task.status, std::string("completed"));
                    }
                }
            }
        } catch (const std::exception &) {
            expectThat(false, label, "R1/R9 failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task completion R2+R4: 重放、键冲突、其他键、作用域、授权与错误详情") {
        const std::string label = "completion-r2-r4";
        try {
            if (!is(c1, Outcome::Completed)) throw std::runtime_error("R1 settlement missing");
            // 重放：另建一次已知键的完成，然后做一次无关的结算，再用同键重放。
            const std::string t4 = publish(s.userA, "R2 replay", 3, 0);
            const std::string key = newUuid();
            const auto original = runComplete(s.userB, t4, key);
            CHECK(is(original, Outcome::Completed));
            if (!is(original, Outcome::Completed)) throw std::runtime_error("R2 settlement failed");
            const std::string originalBody = bodyText(*original);
            const Latest flowerBefore = latestEntry(observer, s.userB, "flower");

            // 期间有另一项结算；之后同键重放：响应体逐字节相同，不含余额，不写新行或审计。
            const std::string t5 = publish(s.userA, "R2 intervening", 1, 0);
            CHECK(is(runComplete(s.userB, t5, newUuid()), Outcome::Completed));
            const auto replay = runWithSentinel(s.userB, t4, key);
            CHECK(is(replay.status, Outcome::Replayed));
            CHECK_EQ(replay.calls, 1);
            CHECK(replay.released);
            if (replay.status) CHECK_EQ(bodyText(*replay.status), originalBody);
            CHECK_EQ(originalBody.find("balance"), std::string::npos);
            CHECK_EQ(taskOps(observer, t4), 1);
            CHECK_EQ(completedAudits(observer, t4), 1);
            CHECK(latestEntry(observer, s.userB, "flower") == (Latest{flowerBefore.seq + 1, flowerBefore.balance + 1}));

            // 同键用于另一项任务：IDEMPOTENCY_KEY_REUSED；另一键完成已完成的任务：TASK_ALREADY_COMPLETED。
            const std::string t6 = publish(s.userA, "R2 other", 2, 0);
            CHECK(is(runComplete(s.userB, t6, key), Outcome::KeyReused));
            CHECK_EQ(taskOps(observer, t6), 0);
            CHECK(is(runComplete(s.userB, t4, newUuid()), Outcome::AlreadyCompleted));
            CHECK_EQ(taskOps(observer, t4), 1);

            // 同伴用相同的键值完成发给自己的任务：不同作用域，正常完成。
            const std::string t7 = publish(s.userB, "R2 partner", 4, 0);
            CHECK(is(runComplete(s.userA, t7, key), Outcome::Completed));

            // R4：发布者 403；其他空间成员 404；没有空间 404；不存在的任务 404；都不写入。
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, false);
            const std::string unbound = newUuid();
            ts::commitUser(pool, unbound, "m3c_unbound");
            const long long auditsBefore = completedAudits(observer, t6);
            CHECK(is(runComplete(s.userA, t6, newUuid()), Outcome::NotAssignee));
            CHECK(is(runComplete(other.userA, t6, newUuid()), Outcome::TaskNotFound));
            CHECK(is(runComplete(unbound, t6, newUuid()), Outcome::SpaceNotFound));
            CHECK(is(runComplete(s.userB, newUuid(), newUuid()), Outcome::TaskNotFound));
            CHECK(is(runComplete(s.userB, t6, "short"), Outcome::Validation));
            CHECK_EQ(taskOps(observer, t6), 0);
            CHECK_EQ(completions(observer, t6), 0);
            CHECK_EQ(completedAudits(observer, t6), auditsBefore);

            // 错误详情只含固定标签：非法 uuid 让数据库报错并回显输入，标记串不得进入 detail / message。
            const std::string marker = "MarkerEcho" + usapp::crypto::randomHex(4);
            const auto echo = runComplete("not-a-uuid-" + marker, t6, newUuid());
            CHECK(is(echo, Outcome::Error));
            if (echo) {
                CHECK_EQ(echo->detail, std::string("task_complete stage=membership reason=db_error"));
                CHECK_EQ(echo->detail.find(marker), std::string::npos);
                CHECK_EQ(echo->message.find(marker), std::string::npos);
            }
        } catch (const std::exception &) {
            expectThat(false, label, "R2/R4 failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task completion R3: 并发完成只入账一次（不同键 / 同一键）") {
        const std::string label = "completion-r3";
        try {
            if (!sharedReady) throw std::runtime_error("shared fixture missing");
            const std::string ta = publish(s.userA, "R3 different keys", 1, 0);
            auto a1 = startComplete(s.userB, ta, newUuid());
            auto a2 = startComplete(s.userB, ta, newUuid());
            const auto r1 = a1->wait(), r2 = a2->wait();
            CHECK(r1.has_value() && r2.has_value());
            if (r1 && r2) {
                const int done = (r1->outcome == Outcome::Completed) + (r2->outcome == Outcome::Completed);
                const int already = (r1->outcome == Outcome::AlreadyCompleted) +
                                    (r2->outcome == Outcome::AlreadyCompleted);
                CHECK_EQ(done, 1);
                CHECK_EQ(already, 1);
            }
            CHECK_EQ(taskOps(observer, ta), 1);
            CHECK_EQ(completedAudits(observer, ta), 1);

            const std::string tb = publish(s.userA, "R3 same key", 1, 0);
            const std::string key = newUuid();
            auto b1 = startComplete(s.userB, tb, key);
            auto b2 = startComplete(s.userB, tb, key);
            const auto q1 = b1->wait(), q2 = b2->wait();
            CHECK(q1.has_value() && q2.has_value());
            if (q1 && q2) {
                const int done = (q1->outcome == Outcome::Completed) + (q2->outcome == Outcome::Completed);
                const int replay = (q1->outcome == Outcome::Replayed) + (q2->outcome == Outcome::Replayed);
                CHECK_EQ(done, 1);
                CHECK_EQ(replay, 1);
                if (q1->success() && q2->success()) CHECK_EQ(bodyText(*q1), bodyText(*q2));
            }
            CHECK_EQ(taskOps(observer, tb), 1);
            CHECK_EQ(completedAudits(observer, tb), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "R3 failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task completion R5: D32 第一条流水之后失败，确认回滚，然后同键重试只结算一次") {
        const std::string label = "completion-r5";
        try {
            if (!sharedReady) throw std::runtime_error("shared fixture missing");
            const std::string task = publish(s.userA, "R5 both", 2, 3);
            const Latest flowerBefore = latestEntry(observer, s.userB, "flower");
            const Latest sunshineBefore = latestEntry(observer, s.userB, "sunshine");
            const std::string key = newUuid();

            // 持锁连接让带故障的完成请求停在账户锁上，以便取得它的后端 PID；然后放行。
            Holder probeHolder = lockAccount(dsn, s.userB, "flower");
            int faultedPid = 0;
            const auto faulted = runWithSentinel(s.userB, task, key, Fault::FailAfterFirstEntry, [&]() {
                faultedPid = observeBlockedBy(observer, probeHolder.pid);
                endHolder(observer, probeHolder, label);
            });
            endHolder(observer, probeHolder, label);  // 已结束时为无操作
            CHECK(faultedPid != 0);
            CHECK(is(faulted.status, Outcome::Error));
            CHECK_EQ(faulted.calls, 1);
            CHECK(faulted.released);
            if (faulted.status) {
                CHECK_EQ(faulted.status->detail, std::string("task_complete stage=fault_injection reason=db_error"));
            }
            // 回滚回调只表示“已请求”：先确认该后端的事务已结束，再观察行锁释放与已提交行不存在。
            if (faultedPid != 0) {
                const auto ended = teardownEnded(observer, faultedPid);
                expectThat(ended.ended, label, "faulted completion teardown: " + ended.observed);
            }
            CHECK(accountLockReleased(observer, s.userB, "flower"));
            CHECK(accountLockReleased(observer, s.userB, "sunshine"));
            CHECK_EQ(taskOps(observer, task), 0);
            CHECK_EQ(completions(observer, task), 0);
            CHECK_EQ(completedAudits(observer, task), 0);
            CHECK(latestEntry(observer, s.userB, "flower") == flowerBefore);
            CHECK(latestEntry(observer, s.userB, "sunshine") == sunshineBefore);

            const auto retry = runComplete(s.userB, task, key);
            CHECK(is(retry, Outcome::Completed));
            if (is(retry, Outcome::Completed)) {
                CHECK_EQ(entriesForOp(observer, retry->settlement.operationId), 2);
            }
            CHECK_EQ(taskOps(observer, task), 1);
            CHECK_EQ(completions(observer, task), 1);
            CHECK(latestEntry(observer, s.userB, "flower") == (Latest{flowerBefore.seq + 1, flowerBefore.balance + 2}));
            CHECK(latestEntry(observer, s.userB, "sunshine") ==
                  (Latest{sunshineBefore.seq + 1, sunshineBefore.balance + 3}));
        } catch (const std::exception &) {
            expectThat(false, label, "R5 failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    // R6：已存在账户上的串行化与提交后可见性。持锁事务在完成请求等待期间写入并提交一条
    // 合法的非任务来源流水（夹具操作，source_type = 'wallet_test'），完成请求的流水必须接在其后。
    // -----------------------------------------------------------------
    TEST_CASE("Task completion R6: 已存在账户的行锁串行化与提交后余额可见") {
        const std::string label = "completion-r6";
        Holder holder;
        try {
            ts::CommittedSpace h;
            ts::commitSpace(pool, h, true);
            TxPtr seed = acquireTx(pool);
            if (!seed) throw std::runtime_error("seed acquisition failed");
            const std::string opInit = newUuid();
            for (const auto &sql : {wallet::accountSql(h.userB, h.couple, "flower"),
                                    wallet::operationSql(opInit, h.couple, h.userB, "REDEMPTION_REFUND"),
                                    wallet::entrySql(opInit, h.couple, h.userB, "flower", 1, 5, 5)}) {
                if (!exec(seed, sql).ok) throw std::runtime_error("seed failed");
            }
            const auto seeded = commitTx(seed);
            if (!seeded || !*seeded) throw std::runtime_error("seed commit failed");
            const std::string task = publish(h.userA, "R6 serialized", 3, 0);

            holder = lockAccount(dsn, h.userB, "flower");
            auto completion = startComplete(h.userB, task, newUuid());
            CHECK_FALSE(completion->readyWithin(Millis(300)));
            const int waiter = observeBlockedBy(observer, holder.pid);
            CHECK(waiter != 0);

            const std::string opHold = newUuid();
            CHECK(exec(holder.tx, wallet::operationSql(opHold, h.couple, h.userB, "REDEMPTION_REFUND")).ok);
            CHECK(exec(holder.tx, wallet::entrySql(opHold, h.couple, h.userB, "flower", 2, 7, 12)).ok);
            const auto committed = commitTx(holder.tx);
            CHECK(committed.has_value() && *committed);

            const auto result = completion->wait();
            CHECK(is(result, Outcome::Completed));
            if (is(result, Outcome::Completed)) {
                const auto entry = observer.client->execSqlSync(
                    "SELECT seq, balance_after FROM wallet_ledger_entries WHERE operation_id = $1::uuid",
                    result->settlement.operationId);
                CHECK_EQ(entry.size(), 1u);
                if (entry.size() == 1u) {
                    CHECK_EQ(entry[0]["seq"].as<long long>(), 3);
                    CHECK_EQ(entry[0]["balance_after"].as<long long>(), 15);
                }
            }
        } catch (const std::exception &) {
            expectThat(false, label, "R6 failed (details withheld)");
        }
        endHolder(observer, holder, label);
    }

    // -----------------------------------------------------------------
    // R7：截止时间跨越等待。任务行是直接插入的（秒级截止时间是发布接口无法产生的，标注为测试行；
    // 其字段与 deadline_at 自洽，键与指纹随机且从不经发布接口重放）。
    // -----------------------------------------------------------------
    TEST_CASE("Task completion R7: 等锁越过截止时间 -> TASK_EXPIRED；截止前放行 -> 成功，截止后同键重放相同") {
        const std::string label = "completion-r7";
        Holder holderA, holderB;
        try {
            ts::CommittedSpace d;
            ts::commitSpace(pool, d, true);
            pool->execSqlSync(wallet::accountSql(d.userB, d.couple, "flower"));

            // Case A：持锁直到截止时间过去。
            task::TaskRow late;
            late.flowers = 1;
            late.sunshine = 0;
            late.deadline = task::relativeDeadline("2 seconds");
            pool->execSqlSync(task::taskSql(late, d.couple, d.userA, d.userB));
            holderA = lockAccount(dsn, d.userB, "flower");
            auto expired = startComplete(d.userB, late.id, newUuid());
            CHECK_FALSE(expired->readyWithin(Millis(200)));
            CHECK(observeBlockedBy(observer, holderA.pid) != 0);
            std::this_thread::sleep_for(Millis(std::min<long long>(msUntilDeadline(observer, late.id), 3000) + 150));
            rollbackTx(holderA.tx);
            const auto a = expired->wait();
            CHECK(is(a, Outcome::Expired));
            CHECK(accountLockReleased(observer, d.userB, "flower"));
            CHECK_EQ(taskOps(observer, late.id), 0);
            CHECK_EQ(completions(observer, late.id), 0);
            CHECK_EQ(completedAudits(observer, late.id), 0);
            endHolder(observer, holderA, label);

            // Case B：截止前放行 -> 成功且 completed_at < deadline_at；截止后同键重放 -> 相同响应体。
            task::TaskRow early;
            early.flowers = 2;
            early.sunshine = 0;
            early.deadline = task::relativeDeadline("3 seconds");
            pool->execSqlSync(task::taskSql(early, d.couple, d.userA, d.userB));
            holderB = lockAccount(dsn, d.userB, "flower");
            const std::string key = newUuid();
            auto inTime = startComplete(d.userB, early.id, key);
            CHECK_FALSE(inTime->readyWithin(Millis(200)));
            CHECK(observeBlockedBy(observer, holderB.pid) != 0);
            rollbackTx(holderB.tx);
            const auto b = inTime->wait();
            CHECK(is(b, Outcome::Completed));
            const auto order = observer.client->execSqlSync(
                "SELECT (c.completed_at < t.deadline_at) AS before FROM task_completions c "
                "JOIN tasks t ON t.id = c.task_id WHERE c.task_id = $1::uuid",
                early.id);
            CHECK(order.size() == 1u && order[0]["before"].as<bool>());
            std::this_thread::sleep_for(Millis(std::min<long long>(msUntilDeadline(observer, early.id), 4000) + 150));
            const auto replay = runComplete(d.userB, early.id, key);
            CHECK(is(replay, Outcome::Replayed));
            if (is(b, Outcome::Completed) && is(replay, Outcome::Replayed)) CHECK_EQ(bodyText(*replay), bodyText(*b));
            CHECK_EQ(taskOps(observer, early.id), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "R7 failed (details withheld)");
        }
        endHolder(observer, holderA, label);
        endHolder(observer, holderB, label);
    }

    // -----------------------------------------------------------------
    // R8：向 R1 已提交的小红花结算（B）追加链式合法的流水。
    // -----------------------------------------------------------------
    TEST_CASE("Task completion R8: 已提交结算上的追加攻击（另一成员 / 未奖励资产）被结算校验器拒绝") {
        const std::string label = "completion-r8";
        try {
            if (!is(c1, Outcome::Completed)) throw std::runtime_error("R1 settlement missing");
            const std::string op = c1->settlement.operationId;
            const Latest aFlower = latestEntry(observer, s.userA, "flower");
            const Latest bFlower = latestEntry(observer, s.userB, "flower");
            const Latest bSunshine = latestEntry(observer, s.userB, "sunshine");
            CHECK_EQ(entriesForOp(observer, op), 1);

            // 另一成员：链式合法，但 SET CONSTRAINTS ALL IMMEDIATE 时结算校验器报 UT007。
            wallet::CaseContext attack;
            attack.tx = acquireTx(pool);
            if (!attack.tx) throw std::runtime_error("attack acquisition failed");
            const auto pidA = exec(attack.tx, "SELECT pg_backend_pid() AS pid");
            if (!pidA.ok || !pidA.result) throw std::runtime_error("attack pid capture failed");
            attack.pid = (*pidA.result)[0]["pid"].as<int>();
            CHECK(exec(attack.tx, wallet::accountSql(s.userA, s.couple, "flower")).ok);
            CHECK(exec(attack.tx, "SELECT 1 FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = 'flower' "
                                  "FOR UPDATE", s.userA).ok);
            CHECK(exec(attack.tx, wallet::entrySql(op, s.couple, s.userA, "flower", aFlower.seq + 1, 1,
                                                   aFlower.balance + 1)).ok);
            const auto immediate = exec(attack.tx, wallet::expectFailureSql("SET CONSTRAINTS ALL IMMEDIATE"));
            CHECK(immediate.completed && !immediate.ok);
            CHECK_EQ(wallet::parseFailure(immediate.error).sqlState, std::string("UT007"));
            rollbackTx(attack.tx);
            CHECK(teardownEnded(observer, attack.pid).ended);

            // 未奖励资产：链式合法的阳光值流水，尝试真实提交；false 本身不是证据，另查已提交行。
            TxPtr tx = acquireTx(pool);
            if (!tx) throw std::runtime_error("attack acquisition failed");
            const auto pidB = exec(tx, "SELECT pg_backend_pid() AS pid");
            if (!pidB.ok || !pidB.result) throw std::runtime_error("attack pid capture failed");
            const int commitPid = (*pidB.result)[0]["pid"].as<int>();
            CHECK(exec(tx, "SELECT 1 FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = 'sunshine' "
                           "FOR UPDATE", s.userB).ok);
            CHECK(exec(tx, wallet::entrySql(op, s.couple, s.userB, "sunshine", bSunshine.seq + 1, 1,
                                            bSunshine.balance + 1)).ok);
            const auto committed = commitTx(tx);
            CHECK(committed.has_value() && !*committed);
            CHECK(teardownEnded(observer, commitPid).ended);

            CHECK_EQ(entriesForOp(observer, op), 1);
            CHECK(latestEntry(observer, s.userA, "flower") == aFlower);
            CHECK(latestEntry(observer, s.userB, "flower") == bFlower);
            CHECK(latestEntry(observer, s.userB, "sunshine") == bSunshine);
            CHECK_EQ(completions(observer, t1), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "R8 failed (details withheld)");
        }
    }
}

}  // namespace usapptest::taskcompletion
