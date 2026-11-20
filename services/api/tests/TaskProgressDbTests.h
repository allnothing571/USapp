#pragma once

// M4: real service, committed per-run fixtures on literal usapp_test only.
// Called after the existing database/migration guard. No purge or reset.
#include "TaskCompletionDbTests.h"

namespace usapptest::taskprogress {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using Service = usapp::TaskCompletionService;
using Outcome = Service::Outcome;

inline std::shared_ptr<c::PendingCompletion> start(const std::string &user, const std::string &task,
                                                 const std::string &key,
                                                 Service::SettlementFault fault = Service::SettlementFault::None) {
    auto pending = std::make_shared<c::PendingCompletion>();
    Service::instance().progressTask(user, task, key,
        [pending](Service::Status s) { pending->deliver(std::move(s)); }, fault);
    return pending;
}

inline std::optional<Service::Status> run(const std::string &user, const std::string &task,
                                        const std::string &key) {
    return start(user, task, key)->wait();
}

inline std::string publish(const std::string &creator, int target, int flowers = 2, int sunshine = 3) {
    auto input = ts::simpleInput("M4 fixture", flowers, sunshine);
    input.completionMode = "count";
    input.targetCount = target;
    const auto result = ts::runCreate(creator, input, c::newUuid());
    if (!result || result->outcome != usapp::TaskService::Outcome::Created)
        throw std::runtime_error("count publication failed");
    return result->task.id;
}

inline long long events(const c::Observer &o, const std::string &id) {
    return c::scalar(o, "SELECT count(*)::bigint AS n FROM task_progress_events WHERE task_id = $1::uuid", id);
}
inline long long audits(const c::Observer &o, const std::string &id) {
    return c::scalar(o, "SELECT count(*)::bigint AS n FROM audit_logs "
                       "WHERE action = 'task.progressed' AND target_id = $1::text", id);
}
inline std::string body(const Service::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, Service::progressResponseBody(s));
}

inline void runTaskProgressDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    auto observer = c::Observer::connect(dsn);
    ts::CommittedSpace space;
    try { ts::commitSpace(pool, space, true); }
    catch (const std::exception &) { c::expectThat(false, "m4-setup", "fixture failed"); return; }

    TEST_CASE("M4: below-target events, immutable replay, final atomic settlement and ignored excess") {
        try {
            const auto id = publish(space.userA, 3);
            const auto key = c::newUuid();
            const auto first = run(space.userB, id, key);
            CHECK(c::is(first, Outcome::Progressed));
            if (!first || !first->progress) throw std::runtime_error("missing progress");
            const auto original = body(*first);
            CHECK_EQ(first->progress->count, 1);
            CHECK_FALSE(first->hasSettlement);
            CHECK_EQ(c::taskOps(observer, id), 0);
            CHECK_EQ(c::completions(observer, id), 0);
            const auto second = run(space.userB, id, c::newUuid());
            CHECK(c::is(second, Outcome::Progressed));
            const auto replay = run(space.userB, id, key);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay) CHECK_EQ(body(*replay), original);
            CHECK_EQ(events(observer, id), 2);
            const auto last = run(space.userB, id, c::newUuid());
            CHECK(c::is(last, Outcome::Completed));
            if (!last || !last->progress) throw std::runtime_error("missing settlement");
            CHECK_EQ(last->progress->count, 3);
            CHECK_EQ(c::taskOps(observer, id), 1);
            CHECK_EQ(c::completions(observer, id), 1);
            CHECK_EQ(c::entriesForOp(observer, last->settlement.operationId), 2);
            CHECK_EQ(audits(observer, id), 3);
            CHECK_EQ(c::completedAudits(observer, id), 1);
            const auto atomic = observer.client->execSqlSync(
                "SELECT count(DISTINCT x)::integer AS n FROM ("
                " SELECT p.xmin::text AS x FROM task_progress_events p WHERE p.id = $2::uuid"
                " UNION ALL SELECT c.xmin::text FROM task_completions c WHERE c.task_id = $1::uuid"
                " UNION ALL SELECT o.xmin::text FROM wallet_operations o WHERE o.source_type = 'task' AND o.source_id = $1::text"
                " UNION ALL SELECT e.xmin::text FROM wallet_ledger_entries e WHERE e.operation_id = $3::uuid"
                " UNION ALL SELECT a.xmin::text FROM audit_logs a WHERE a.target_id = $1::text AND ("
                " a.action = 'task.completed' OR (a.action = 'task.progressed' AND a.detail->>'event_id' = $2::text))) x",
                id, last->progress->eventId, last->settlement.operationId);
            CHECK_EQ(atomic[0]["n"].as<int>(), 1);
            const auto ignored = run(space.userB, id, c::newUuid());
            CHECK(c::is(ignored, Outcome::Ignored));
            CHECK_EQ(events(observer, id), 3);
            CHECK_EQ(audits(observer, id), 3);
            const auto old = run(space.userB, id, key);
            if (old) CHECK_EQ(body(*old), original);
            const auto wallet = c::walletOf(space.userB);
            CHECK(wallet.has_value());
            if (wallet) { CHECK_EQ(wallet->flower, 2); CHECK_EQ(wallet->sunshine, 3); }
            const auto detail = ts::runGet(space.userA, id);
            CHECK(detail.has_value());
            if (detail) { CHECK_EQ(detail->task.progressCount, 3); CHECK_EQ(detail->task.status, std::string("completed")); }

            // Progress and single completion keys have independent domains.
            const auto single = c::publish(space.userA, "M4 legacy single", 1, 0);
            CHECK(c::is(c::runComplete(space.userB, single, key), Outcome::Completed));
            CHECK(c::is(run(space.userB, single, c::newUuid()), Outcome::WrongMode));
            const auto other = publish(space.userA, 1);
            CHECK(c::is(run(space.userB, other, key), Outcome::KeyReused));
            CHECK_EQ(events(observer, other), 0);
            CHECK_EQ(c::taskOps(observer, other), 0);
        } catch (const std::exception &) { c::expectThat(false, "m4-normal", "case failed (details withheld)"); }
    }

    TEST_CASE("M4: concurrent same-key increments and different-key final increments settle once") {
        try {
            const auto id = publish(space.userA, 2, 1, 0);
            const auto key = c::newUuid();
            const auto p1 = start(space.userB, id, key);
            const auto p2 = start(space.userB, id, key);
            const auto a = p1->wait(), b = p2->wait();
            CHECK((c::is(a, Outcome::Progressed) && c::is(b, Outcome::Replayed)) ||
                  (c::is(b, Outcome::Progressed) && c::is(a, Outcome::Replayed)));
            if (a && b) CHECK_EQ(body(*a), body(*b));
            CHECK_EQ(p1->calls(), 1); CHECK_EQ(p2->calls(), 1);
            const auto p3 = start(space.userB, id, c::newUuid());
            const auto p4 = start(space.userB, id, c::newUuid());
            const auto x = p3->wait(), y = p4->wait();
            CHECK((c::is(x, Outcome::Completed) && c::is(y, Outcome::Ignored)) ||
                  (c::is(y, Outcome::Completed) && c::is(x, Outcome::Ignored)));
            CHECK_EQ(events(observer, id), 2);
            CHECK_EQ(c::taskOps(observer, id), 1);
            CHECK_EQ(c::completedAudits(observer, id), 1);
        } catch (const std::exception &) { c::expectThat(false, "m4-concurrency", "case failed (details withheld)"); }
    }

    TEST_CASE("M4: final partial-write failure rolls back the event and releases its callback; retry settles once") {
        c::Holder holder;
        try {
            const auto id = publish(space.userA, 1, 1, 2);
            const auto key = c::newUuid();
            const auto flower = c::latestEntry(observer, space.userB, "flower");
            const auto sun = c::latestEntry(observer, space.userB, "sunshine");
            holder = c::lockAccount(dsn, space.userB, "flower");
            auto pending = std::make_shared<c::PendingCompletion>();
            auto sentinel = std::make_shared<int>(1);
            std::weak_ptr<int> watch = sentinel;
            Service::instance().progressTask(space.userB, id, key,
                [pending, sentinel](Service::Status s) { pending->deliver(std::move(s)); },
                Service::SettlementFault::FailAfterFirstEntry);
            sentinel.reset();
            const int pid = c::observeBlockedBy(observer, holder.pid);
            CHECK(pid != 0);
            c::endHolder(observer, holder, "m4-fault");
            const auto failed = pending->wait();
            CHECK(c::is(failed, Outcome::Error));
            if (failed) CHECK_EQ(failed->detail, std::string("task_progress stage=fault_injection reason=db_error"));
            CHECK_EQ(pending->calls(), 1);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!watch.expired() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(c::Millis(10));
            CHECK(watch.expired());
            if (pid) CHECK(c::teardownEnded(observer, pid).ended);
            CHECK(c::accountLockReleased(observer, space.userB, "flower"));
            CHECK(c::accountLockReleased(observer, space.userB, "sunshine"));
            CHECK_EQ(events(observer, id), 0); CHECK_EQ(audits(observer, id), 0);
            CHECK_EQ(c::taskOps(observer, id), 0); CHECK_EQ(c::completions(observer, id), 0);
            CHECK_EQ(c::completedAudits(observer, id), 0);
            CHECK(c::latestEntry(observer, space.userB, "flower") == flower);
            CHECK(c::latestEntry(observer, space.userB, "sunshine") == sun);
            CHECK(c::is(run(space.userB, id, key), Outcome::Completed));
            CHECK_EQ(events(observer, id), 1); CHECK_EQ(c::taskOps(observer, id), 1);
        } catch (const std::exception &) { c::expectThat(false, "m4-fault", "case failed (details withheld)"); }
        c::endHolder(observer, holder, "m4-fault");
    }

    TEST_CASE("M4: authorization, expired progress and SQL bypass guards") {
        try {
            const auto id = publish(space.userA, 2);
            CHECK(c::is(run(space.userA, id, c::newUuid()), Outcome::NotAssignee));
            CHECK(c::is(c::runComplete(space.userB, id, c::newUuid()), Outcome::WrongMode));
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, false);
            CHECK(c::is(run(other.userA, id, c::newUuid()), Outcome::TaskNotFound));
            const auto unbound = c::newUuid();
            ts::commitUser(pool, unbound, "m4_unbound");
            CHECK(c::is(run(unbound, id, c::newUuid()), Outcome::SpaceNotFound));
            task::TaskRow expired;
            expired.completionMode = "count"; expired.targetCount = 2;
            expired.deadline = task::relativeDeadline("-1 hour");
            pool->execSqlSync(task::taskSql(expired, space.couple, space.userA, space.userB));
            CHECK(c::is(run(space.userB, expired.id, c::newUuid()), Outcome::Expired));
            CHECK_EQ(events(observer, expired.id), 0);
            CHECK_EQ(c::taskOps(observer, expired.id), 0);

            // A valid task-sourced operation is not sufficient to complete a count task.
            auto tx = c::acquireTx(pool);
            const auto op = c::newUuid();
            CHECK(wallet::exec(tx, task::taskOperationSql(op, space.couple, space.userB, id)).ok);
            const auto rejected = wallet::exec(tx, wallet::expectFailureSql(
                task::completionSql(id, space.couple, space.userB, op, 2, 3)));
            CHECK(!rejected.ok);
            CHECK_EQ(wallet::parseFailure(rejected.error).sqlState, std::string("UP003"));
            c::rollbackTx(tx);
            CHECK_EQ(c::taskOps(observer, id), 0);
            CHECK_EQ(events(observer, id), 0);

            // The database itself rejects a skipped counter, independently of HTTP.
            tx = c::acquireTx(pool);
            const auto gap = wallet::exec(tx, wallet::expectFailureSql(
                "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, "
                "target_count, idempotency_key, recorded_at) VALUES ('" + id + "'::uuid, '" +
                space.couple + "'::uuid, '" + space.userB + "'::uuid, 2, 2, '" + c::newUuid() +
                "', clock_timestamp())"));
            CHECK(!gap.ok);
            CHECK_EQ(wallet::parseFailure(gap.error).sqlState, std::string("UP002"));
            c::rollbackTx(tx);

            // A final event cannot survive a deferred check without its settlement.
            const auto unfinished = publish(space.userA, 1);
            tx = c::acquireTx(pool);
            const auto finalOp = c::newUuid();
            CHECK(wallet::exec(tx, task::taskOperationSql(finalOp, space.couple, space.userB, unfinished)).ok);
            CHECK(wallet::exec(tx,
                "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, "
                "target_count, idempotency_key, operation_id, recorded_at) VALUES ('" + unfinished +
                "'::uuid, '" + space.couple + "'::uuid, '" + space.userB + "'::uuid, 1, 1, '" +
                c::newUuid() + "', '" + finalOp + "'::uuid, clock_timestamp())").ok);
            const auto incomplete = wallet::exec(tx, wallet::expectFailureSql(
                "SET CONSTRAINTS task_progress_events_count_valid IMMEDIATE"));
            CHECK(!incomplete.ok);
            CHECK_EQ(wallet::parseFailure(incomplete.error).sqlState, std::string("UP003"));
            c::rollbackTx(tx);
            CHECK_EQ(events(observer, unfinished), 0);
            CHECK_EQ(c::taskOps(observer, unfinished), 0);
        } catch (const std::exception &) { c::expectThat(false, "m4-guards", "case failed (details withheld)"); }
    }
}
} // namespace usapptest::taskprogress
