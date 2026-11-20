#pragma once

// M5-C: real services on literal usapp_test, after the migration/database guard.
// A custom range is one ordinary root counting task, so the M4 helpers apply.
// Isolated committed fixtures only; never resets.
#include "WeeklyTaskDbTests.h"

namespace usapptest::customrange {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using Service = usapp::TaskCompletionService;
using Outcome = Service::Outcome;
using daily::checkedExec;
using daily::day;

inline usapp::TaskService::CreateInput input(int target, const std::string &start, const std::string &end) {
    auto in = ts::simpleInput("M5 custom", 1, 3);
    in.completionMode = "count"; in.targetCount = target; in.scheduleType = "custom";
    in.startDate = start; in.deadlineLocalDate = end;
    return in;
}
inline std::string publish(const std::string &creator, int target, const std::string &start, const std::string &end) {
    const auto made = ts::runCreate(creator, input(target, start, end), c::newUuid());
    if (!made || made->outcome != usapp::TaskService::Outcome::Created)
        throw std::runtime_error("custom publish failed");
    return made->task.id;
}
inline long long businessWrites(const c::Observer &o, const std::string &id) {
    return c::scalar(o, "SELECT ((SELECT count(*) FROM task_progress_events WHERE task_id = $1::uuid) + "
        "(SELECT count(*) FROM task_completions WHERE task_id = $1::uuid) + "
        "(SELECT count(*) FROM wallet_operations WHERE source_type = 'task' AND source_id = $1::text) + "
        "(SELECT count(*) FROM audit_logs WHERE action IN ('task.progressed', 'task.completed') "
        "AND target_id = $1::text))::bigint AS n", id);
}

inline void runCustomRangeDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("M5-C: past start is refused; upcoming range is read-only and rejects progress; same-day range settles once") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto past = ts::runCreate(s.userA, input(2, day(observer, -1), day(observer, 1)), c::newUuid());
            CHECK(past.has_value());
            if (past) CHECK(past->outcome == usapp::TaskService::Outcome::Validation);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM tasks WHERE creator_id = $1::uuid", s.userA), 0);

            const auto upcoming = publish(s.userA, 2, day(observer, 1), day(observer, 3));
            for (int i = 0; i < 2; ++i) {
                const auto read = ts::runGet(s.userB, upcoming);
                CHECK(read.has_value());
                if (read) {
                    CHECK_EQ(read->task.scheduleType, std::string("custom"));
                    CHECK_EQ(read->task.status, std::string("upcoming"));
                    CHECK_EQ(read->task.startDate, day(observer, 1));
                    CHECK_EQ(read->task.deadlineLocalDate, day(observer, 3));
                }
            }
            CHECK(c::is(taskprogress::run(s.userB, upcoming, c::newUuid()), Outcome::NotStarted));
            CHECK(c::is(daily::run(s.userB, upcoming, day(observer), c::newUuid()), Outcome::Validation));
            CHECK_EQ(businessWrites(observer, upcoming), 0);

            // Same-day range: one increment, then two concurrent final increments.
            const auto id = publish(s.userA, 2, day(observer), day(observer));
            const auto open = ts::runGet(s.userB, id);
            CHECK(open.has_value());
            if (open) CHECK_EQ(open->task.status, std::string("open"));
            const auto firstKey = c::newUuid();
            const auto first = taskprogress::run(s.userB, id, firstKey);
            CHECK(c::is(first, Outcome::Progressed));
            const auto a = taskprogress::start(s.userB, id, c::newUuid());
            const auto b = taskprogress::start(s.userB, id, c::newUuid());
            const auto x = a->wait(), y = b->wait();
            CHECK((c::is(x, Outcome::Completed) && c::is(y, Outcome::Ignored)) ||
                  (c::is(y, Outcome::Completed) && c::is(x, Outcome::Ignored)));
            CHECK_EQ(a->calls(), 1); CHECK_EQ(b->calls(), 1);
            const auto final = c::is(x, Outcome::Completed) ? x : y;
            if (final) CHECK_EQ(c::entriesForOp(observer, final->settlement.operationId), 2);
            CHECK_EQ(taskprogress::events(observer, id), 2);
            CHECK_EQ(c::completions(observer, id), 1);
            CHECK_EQ(c::taskOps(observer, id), 1);
            CHECK_EQ(c::completedAudits(observer, id), 1);
            const auto replay = taskprogress::run(s.userB, id, firstKey);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay && first) CHECK_EQ(taskprogress::body(*replay), taskprogress::body(*first));
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value());
            if (wallet) { CHECK_EQ(wallet->flower, 1); CHECK_EQ(wallet->sunshine, 3); }
        } catch (const std::exception &) { c::expectThat(false, "custom-normal", "case failed (details withheld)"); }
    }

    TEST_CASE("M5-C: an expired range replays committed progress, rejects new progress; SQL guards") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto start = day(observer, -3), end = day(observer, -1), key = c::newUuid();
            // API-unproducible historical fixture: ordinary inserts, honest past times.
            task::TaskRow range;
            range.scheduleType = "custom"; range.completionMode = "count"; range.targetCount = 2;
            range.flowers = 1; range.sunshine = 0;
            range.createdAtSql = "clock_timestamp() - interval '4 days'";
            range.startDateSql = "'" + start + "'::date";
            range.deadline = task::dateOnlyDeadline(end);
            auto tx = c::acquireTx(pool);
            checkedExec(tx, task::taskSql(range, s.couple, s.userA, s.userB));
            checkedExec(tx, "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, "
                "target_count, idempotency_key, recorded_at) VALUES ('" + range.id + "', '" + s.couple + "', '" +
                s.userB + "', 1, 2, '" + key + "', ('" + start +
                "'::date::timestamp AT TIME ZONE 'Asia/Shanghai') + interval '12 hours')");
            checkedExec(tx, "SET CONSTRAINTS ALL IMMEDIATE");
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("historical commit unconfirmed");

            const auto replay = taskprogress::run(s.userB, range.id, key);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay && replay->progress) CHECK_EQ(replay->progress->count, 1);
            CHECK(c::is(taskprogress::run(s.userB, range.id, c::newUuid()), Outcome::Expired));
            CHECK_EQ(taskprogress::events(observer, range.id), 1);
            CHECK_EQ(c::taskOps(observer, range.id), 0);
            const auto detail = ts::runGet(s.userA, range.id);
            CHECK(detail.has_value());
            if (detail) {
                CHECK_EQ(detail->task.status, std::string("expired"));
                CHECK_EQ(detail->task.startDate, start);
                CHECK_EQ(detail->task.progressCount, 1);
            }
            const auto history = ts::runList(s.userB, usapp::TaskService::View::History, std::nullopt, 10);
            CHECK(history.has_value());
            if (history) {
                int rows = 0;
                for (const auto &t : history->tasks) rows += t.id == range.id && t.startDate == start ? 1 : 0;
                CHECK_EQ(rows, 1);
            }

            // Direct SQL: progress before the start date is UC001; end before start violates the CHECK.
            const auto upcoming = publish(s.userA, 1, day(observer, 2), day(observer, 2));
            tx = c::acquireTx(pool);
            const auto early = wallet::exec(tx, wallet::expectFailureSql(
                "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, target_count, "
                "idempotency_key, recorded_at) VALUES ('" + upcoming + "', '" + s.couple + "', '" + s.userB +
                "', 1, 1, '" + c::newUuid() + "', clock_timestamp())"));
            CHECK(!early.ok);
            CHECK_EQ(wallet::parseFailure(early.error).sqlState, std::string("UC001"));
            c::rollbackTx(tx);
            task::TaskRow reversed = range;
            reversed.id = c::newUuid(); reversed.key = task::taskKey();
            reversed.createdAtSql = "clock_timestamp()";
            reversed.startDateSql = "'" + day(observer, 2) + "'::date";
            reversed.deadline = task::dateOnlyDeadline(day(observer, 1));
            tx = c::acquireTx(pool);
            const auto bad = wallet::exec(tx, wallet::expectFailureSql(task::taskSql(reversed, s.couple, s.userA, s.userB)));
            CHECK(!bad.ok);
            CHECK_EQ(wallet::parseFailure(bad.error).sqlState, std::string("23514"));
            c::rollbackTx(tx);
        } catch (const std::exception &) { c::expectThat(false, "custom-expired", "case failed (details withheld)"); }
    }
}
} // namespace usapptest::customrange
