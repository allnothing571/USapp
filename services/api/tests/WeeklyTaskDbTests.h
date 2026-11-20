#pragma once

// M5-B: real services on literal usapp_test, after the migration/database guard.
// Reuses the M5-A helpers and isolated committed fixtures; never resets. The
// shared lock/settlement/D32 path is already covered by DailyTaskDbTests.h.
#include "DailyTaskDbTests.h"

namespace usapptest::weekly {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using Service = usapp::TaskCompletionService;
using Outcome = Service::Outcome;
using daily::checkedExec;
using daily::periods;
using daily::run;
using daily::start;

// Monday of the current Beijing week, shifted by whole weeks; plus an optional day offset.
inline std::string monday(const c::Observer &o, int weeks = 0, int days = 0) {
    return o.client->execSqlSync(
        "SELECT to_char(x.d - (EXTRACT(ISODOW FROM x.d)::integer - 1) + 7 * $1::integer + $2::integer, "
        "'YYYY-MM-DD') AS d FROM (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d) x",
        std::to_string(weeks), std::to_string(days))[0]["d"].as<std::string>();
}
inline std::string publish(const std::string &creator, int target) {
    auto in = daily::input(target);
    in.title = "M5 weekly";
    in.scheduleType = "weekly";
    const auto made = ts::runCreate(creator, in, c::newUuid());
    if (!made || made->outcome != usapp::TaskService::Outcome::Created)
        throw std::runtime_error("weekly publish failed");
    return made->task.id;
}

inline void runWeeklyTaskDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("M5-B: read-only current week, concurrent materialization and final increment, one settlement") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto id = publish(s.userA, 2), week = monday(observer), sunday = monday(observer, 0, 6);
            const auto read = ts::runGet(s.userB, id);
            CHECK(read.has_value());
            if (read) {
                CHECK_EQ(read->task.scheduleType, std::string("weekly"));
                CHECK_EQ(read->task.periodDate, week);
                CHECK_EQ(read->task.deadlineLocalDate, sunday);
                CHECK_EQ(read->task.progressCount, 0);
            }
            CHECK_EQ(periods(observer, id), 0);
            // A non-Monday identity is malformed (400), not a stale period.
            CHECK(c::is(run(s.userB, id, monday(observer, 0, 1), c::newUuid()), Outcome::Validation));
            CHECK_EQ(periods(observer, id), 0);
            const auto first = c::newUuid();
            const auto a = start(s.userB, id, week, first), b = start(s.userB, id, week, c::newUuid());
            const auto x = a->wait(), y = b->wait();
            CHECK((c::is(x, Outcome::Progressed) && c::is(y, Outcome::Completed)) ||
                  (c::is(y, Outcome::Progressed) && c::is(x, Outcome::Completed)));
            CHECK_EQ(a->calls(), 1); CHECK_EQ(b->calls(), 1);
            CHECK_EQ(periods(observer, id), 1);
            const auto child = daily::instance(observer, id, week);
            CHECK_EQ(taskprogress::events(observer, child), 2);
            CHECK_EQ(c::completions(observer, child), 1);
            CHECK_EQ(c::taskOps(observer, child), 1); CHECK_EQ(c::taskOps(observer, id), 0);
            const auto replay = run(s.userB, id, week, first);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay) CHECK_EQ(replay->periodDate, week);
            CHECK(c::is(run(s.userB, id, week, c::newUuid()), Outcome::Ignored));
            CHECK_EQ(c::completions(observer, child), 1);
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value());
            if (wallet) { CHECK_EQ(wallet->flower, 1); CHECK_EQ(wallet->sunshine, 3); }
        } catch (const std::exception &) { c::expectThat(false, "weekly-normal", "case failed (details withheld)"); }
    }

    TEST_CASE("M5-B: last week's key replays its own week; stale requests never count this week; SQL guards") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto thisWeek = monday(observer), lastWeek = monday(observer, -1), key = c::newUuid();
            // API-unproducible historical fixture: ordinary inserts, no UPDATE,
            // no clock override and no disabled constraints. Progress 1 of 2, unsettled.
            task::TaskRow root;
            root.scheduleType = "weekly"; root.completionMode = "count"; root.targetCount = 2;
            root.flowers = 1; root.sunshine = 0; root.createdAtSql = "clock_timestamp() - interval '14 days'";
            task::TaskRow old = root;
            old.id = c::newUuid(); old.key = task::taskKey(); old.scheduleType = "one_off";
            old.recurrenceId = root.id; old.periodDateSql = "'" + lastWeek + "'::date";
            const auto begin = "('" + lastWeek + "'::date::timestamp AT TIME ZONE 'Asia/Shanghai')";
            old.createdAtSql = begin + " + interval '8 hours'";
            old.deadline = task::rawDeadline("('" + lastWeek + "'::date + 6)", "NULL::time",
                "(('" + lastWeek + "'::date + 7)::timestamp AT TIME ZONE 'Asia/Shanghai')");
            auto tx = c::acquireTx(pool);
            checkedExec(tx, task::taskSql(root, s.couple, s.userA, s.userB));
            checkedExec(tx, task::taskSql(old, s.couple, s.userA, s.userB));
            checkedExec(tx, "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, "
                "target_count, idempotency_key, recorded_at) VALUES ('" + old.id + "', '" + s.couple + "', '" +
                s.userB + "', 1, 2, '" + key + "', " + begin + " + interval '12 hours')");
            checkedExec(tx, "SET CONSTRAINTS ALL IMMEDIATE");
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("historical commit unconfirmed");

            const auto current = ts::runGet(s.userB, root.id);
            CHECK(current.has_value());
            if (current) { CHECK_EQ(current->task.progressCount, 0); CHECK_EQ(current->task.periodDate, thisWeek); }
            const auto replay = run(s.userB, root.id, lastWeek, key);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay) {
                CHECK_EQ(replay->periodDate, lastWeek); CHECK_EQ(replay->task.id, root.id);
                if (replay->progress) CHECK_EQ(replay->progress->count, 1);
            }
            CHECK(c::is(run(s.userB, root.id, thisWeek, key), Outcome::KeyReused));
            CHECK(c::is(run(s.userB, root.id, lastWeek, c::newUuid()), Outcome::Expired));
            CHECK_EQ(periods(observer, root.id), 1);
            CHECK_EQ(taskprogress::events(observer, old.id), 1);
            const auto fresh = run(s.userB, root.id, thisWeek, c::newUuid());
            CHECK(c::is(fresh, Outcome::Progressed));
            if (fresh && fresh->progress) { CHECK_EQ(fresh->progress->count, 1); CHECK_EQ(fresh->periodDate, thisWeek); }
            CHECK_EQ(periods(observer, root.id), 2);
            const auto again = run(s.userB, root.id, lastWeek, key);
            CHECK(c::is(again, Outcome::Replayed));
            if (again && replay) CHECK_EQ(taskprogress::body(*again), taskprogress::body(*replay));
            const auto oldDetail = daily::get(s.userA, root.id, lastWeek);
            CHECK(oldDetail.has_value());
            if (oldDetail) {
                CHECK_EQ(oldDetail->task.periodDate, lastWeek);
                CHECK_EQ(oldDetail->task.status, std::string("expired"));
                CHECK_EQ(oldDetail->task.progressCount, 1);
            }
            const auto history = ts::runList(s.userB, usapp::TaskService::View::History, std::nullopt, 10);
            CHECK(history.has_value());
            if (history) {
                int lastWeekRows = 0, thisWeekRows = 0;
                for (const auto &t : history->tasks) {
                    if (t.id != root.id) continue;
                    lastWeekRows += t.periodDate == lastWeek ? 1 : 0;
                    thisWeekRows += t.periodDate == thisWeek ? 1 : 0;
                }
                CHECK_EQ(lastWeekRows, 1); CHECK_EQ(thisWeekRows, 0);
            }

            // Direct SQL: the weekly template never accepts progress (UD003), and a
            // stored week must start on Monday even when its snapshot matches (UD001).
            tx = c::acquireTx(pool);
            const auto onTemplate = wallet::exec(tx, wallet::expectFailureSql(
                "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, target_count, "
                "idempotency_key, recorded_at) VALUES ('" + root.id + "', '" + s.couple + "', '" + s.userB +
                "', 1, 2, '" + c::newUuid() + "', clock_timestamp())"));
            CHECK(!onTemplate.ok);
            CHECK_EQ(wallet::parseFailure(onTemplate.error).sqlState, std::string("UD003"));
            c::rollbackTx(tx);
            task::TaskRow tuesday = old;
            tuesday.id = c::newUuid(); tuesday.key = task::taskKey();
            const auto tue = monday(observer, -1, 1);
            tuesday.periodDateSql = "'" + tue + "'::date";
            tuesday.deadline = task::rawDeadline("('" + tue + "'::date + 6)", "NULL::time",
                "(('" + tue + "'::date + 7)::timestamp AT TIME ZONE 'Asia/Shanghai')");
            tuesday.createdAtSql = "('" + tue + "'::date::timestamp AT TIME ZONE 'Asia/Shanghai') + interval '8 hours'";
            tx = c::acquireTx(pool);
            const auto notMonday = wallet::exec(tx, wallet::expectFailureSql(
                task::taskSql(tuesday, s.couple, s.userA, s.userB)));
            CHECK(!notMonday.ok);
            CHECK_EQ(wallet::parseFailure(notMonday.error).sqlState, std::string("UD001"));
            c::rollbackTx(tx);
        } catch (const std::exception &) { c::expectThat(false, "weekly-rollover", "case failed (details withheld)"); }
    }
}
} // namespace usapptest::weekly
