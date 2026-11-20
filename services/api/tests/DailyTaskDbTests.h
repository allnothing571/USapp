#pragma once

// M5-A: real services on literal usapp_test, after the migration/database guard.
// Reuse existing bounded observers and isolated committed fixtures; never reset.
#include "TaskProgressDbTests.h"

namespace usapptest::daily {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using Service = usapp::TaskCompletionService;
using Outcome = Service::Outcome;

inline std::string day(const c::Observer &o, int offset = 0) {
    return o.client->execSqlSync(
        "SELECT to_char((clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + $1::integer, 'YYYY-MM-DD') AS d",
        std::to_string(offset))[0]["d"].as<std::string>();
}
inline std::shared_ptr<c::PendingCompletion> start(const std::string &user, const std::string &id,
                                                 const std::string &date, const std::string &key) {
    auto pending = std::make_shared<c::PendingCompletion>();
    Service::instance().progressDailyTask(user, id, key, date,
        [pending](Service::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<Service::Status> run(const std::string &user, const std::string &id,
                                        const std::string &date, const std::string &key) {
    return start(user, id, date, key)->wait();
}
inline usapp::TaskService::CreateInput input(int target) {
    auto in = ts::simpleInput("M5 daily", 1, 3);
    in.completionMode = "count"; in.targetCount = target; in.scheduleType = "daily";
    return in;
}
inline std::string publish(const std::string &creator, int target, const std::string &key = c::newUuid()) {
    const auto made = ts::runCreate(creator, input(target), key);
    if (!made || made->outcome != usapp::TaskService::Outcome::Created)
        throw std::runtime_error("daily publish failed");
    return made->task.id;
}
inline long long periods(const c::Observer &o, const std::string &root) {
    return c::scalar(o, "SELECT count(*)::bigint AS n FROM tasks WHERE recurrence_id = $1::uuid", root);
}
inline std::string instance(const c::Observer &o, const std::string &root, const std::string &date) {
    const auto rows = o.client->execSqlSync(
        "SELECT id::text AS id FROM tasks WHERE recurrence_id = $1::uuid AND period_date = $2::date", root, date);
    if (rows.size() != 1u) throw std::runtime_error("period missing");
    return rows[0]["id"].as<std::string>();
}
inline std::optional<usapp::TaskService::Status> get(const std::string &user, const std::string &id,
                                                   const std::string &date) {
    auto pending = std::make_shared<ts::PendingStatus>();
    usapp::TaskService::instance().getTask(user, id,
        [pending](usapp::TaskService::Status s) { pending->deliver(std::move(s)); }, date);
    return pending->wait();
}
inline void checkedExec(const c::TxPtr &tx, const std::string &sql) {
    if (!wallet::exec(tx, sql).ok) throw std::runtime_error("daily fixture statement failed");
}

inline void runDailyTaskDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("M5: read-only virtual period, concurrent first progress and one final settlement") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto publicationKey = c::newUuid();
            const auto id = publish(s.userA, 2, publicationKey), date = day(observer), key = c::newUuid();
            for (int i = 0; i < 2; ++i) {
                const auto read = ts::runGet(s.userB, id);
                CHECK(read.has_value());
                if (read) {
                    CHECK_EQ(read->task.id, id); CHECK_EQ(read->task.periodDate, date);
                    CHECK_EQ(read->task.progressCount, 0);
                }
            }
            CHECK_EQ(periods(observer, id), 0);
            const auto a = start(s.userB, id, date, key), b = start(s.userB, id, date, key);
            const auto x = a->wait(), y = b->wait();
            CHECK((c::is(x, Outcome::Progressed) && c::is(y, Outcome::Replayed)) ||
                  (c::is(y, Outcome::Progressed) && c::is(x, Outcome::Replayed)));
            if (x && y) CHECK_EQ(taskprogress::body(*x), taskprogress::body(*y));
            CHECK_EQ(a->calls(), 1); CHECK_EQ(b->calls(), 1);
            CHECK_EQ(periods(observer, id), 1);
            const auto child = instance(observer, id, date);
            CHECK_EQ(taskprogress::events(observer, child), 1);
            CHECK_EQ(c::taskOps(observer, child), 0);
            const auto publicationReplay = ts::runCreate(s.userA, input(2), publicationKey);
            CHECK(publicationReplay.has_value());
            if (publicationReplay) {
                CHECK(publicationReplay->outcome == usapp::TaskService::Outcome::Replayed);
                CHECK_EQ(publicationReplay->task.id, id);
                CHECK_EQ(publicationReplay->task.progressCount, 1);
            }
            const auto final = run(s.userB, id, date, c::newUuid());
            CHECK(c::is(final, Outcome::Completed));
            if (final) {
                CHECK_EQ(final->task.id, id); CHECK_EQ(final->periodDate, date);
                CHECK_EQ(c::entriesForOp(observer, final->settlement.operationId), 2);
            }
            CHECK(c::is(run(s.userB, id, date, c::newUuid()), Outcome::Ignored));
            CHECK_EQ(c::taskOps(observer, child), 1); CHECK_EQ(c::taskOps(observer, id), 0);
            CHECK_EQ(c::completions(observer, child), 1);
            CHECK_EQ(taskprogress::events(observer, child), 2);
            CHECK_EQ(taskprogress::audits(observer, child), 2);
            CHECK_EQ(c::completedAudits(observer, child), 1);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM audit_logs "
                "WHERE action = 'task.period.created' AND target_id = $1::text", child), 1);
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value());
            if (wallet) { CHECK_EQ(wallet->flower, 1); CHECK_EQ(wallet->sunshine, 3); }
        } catch (const std::exception &) { c::expectThat(false, "daily-normal", "case failed (details withheld)"); }
    }

    TEST_CASE("M5: old-period replay, current-day reset and two history pages for one template") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto today = day(observer), yesterday = day(observer, -1), key = c::newUuid();
            // API-unproducible historical fixture: ordinary inserts, no UPDATE,
            // no clock override and no disabled constraints. Today's API uses real time.
            task::TaskRow root;
            root.scheduleType = "daily"; root.completionMode = "count"; root.targetCount = 1;
            root.flowers = 1; root.sunshine = 0; root.createdAtSql = "clock_timestamp() - interval '2 days'";
            task::TaskRow old = root;
            old.id = c::newUuid(); old.key = task::taskKey(); old.scheduleType = "one_off";
            old.recurrenceId = root.id; old.periodDateSql = "'" + yesterday + "'::date";
            const auto begin = "('" + yesterday + "'::date::timestamp AT TIME ZONE 'Asia/Shanghai')";
            const auto end = "(('" + yesterday + "'::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai')";
            old.createdAtSql = begin + " + interval '8 hours'";
            old.deadline = task::rawDeadline(old.periodDateSql, "NULL::time", end);
            const auto op = c::newUuid(), event = c::newUuid();
            auto tx = c::acquireTx(pool);
            checkedExec(tx, task::taskSql(root, s.couple, s.userA, s.userB));
            checkedExec(tx, task::taskSql(old, s.couple, s.userA, s.userB));
            checkedExec(tx, wallet::accountSql(s.userB, s.couple, "flower"));
            checkedExec(tx, task::taskOperationSql(op, s.couple, s.userB, old.id));
            checkedExec(tx, "INSERT INTO task_progress_events (id, task_id, couple_id, user_id, progress_count, "
                "target_count, idempotency_key, operation_id, recorded_at) VALUES ('" + event + "', '" +
                old.id + "', '" + s.couple + "', '" + s.userB + "', 1, 1, '" + key + "', '" + op +
                "', " + begin + " + interval '12 hours')");
            checkedExec(tx, task::completionSql(old.id, s.couple, s.userB, op, 1, 0, begin + " + interval '12 hours'"));
            checkedExec(tx, wallet::entrySql(op, s.couple, s.userB, "flower", 1, 1, 1));
            checkedExec(tx, "SET CONSTRAINTS ALL IMMEDIATE");
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("historical commit unconfirmed");
            const auto current = ts::runGet(s.userB, root.id);
            CHECK(current.has_value());
            if (current) { CHECK_EQ(current->task.progressCount, 0); CHECK_EQ(current->task.periodDate, today); }
            const auto replay = run(s.userB, root.id, yesterday, key);
            CHECK(c::is(replay, Outcome::Replayed));
            if (replay) { CHECK_EQ(replay->periodDate, yesterday); CHECK_EQ(replay->task.id, root.id); }
            CHECK(c::is(run(s.userB, root.id, today, key), Outcome::KeyReused));
            CHECK_EQ(periods(observer, root.id), 1);
            CHECK(c::is(run(s.userB, root.id, yesterday, c::newUuid()), Outcome::Ignored));
            CHECK(c::is(run(s.userB, root.id, today, c::newUuid()), Outcome::Completed));
            CHECK_EQ(periods(observer, root.id), 2);
            const auto again = run(s.userB, root.id, yesterday, key);
            CHECK(c::is(again, Outcome::Replayed));
            if (again && replay) CHECK_EQ(taskprogress::body(*again), taskprogress::body(*replay));
            const auto oldDetail = get(s.userA, root.id, yesterday);
            CHECK(oldDetail.has_value());
            if (oldDetail) CHECK_EQ(oldDetail->task.periodDate, yesterday);
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value()); if (wallet) CHECK_EQ(wallet->flower, 2);
            const auto p1 = ts::runList(s.userB, usapp::TaskService::View::History, std::nullopt, 1);
            CHECK(p1.has_value());
            if (!p1 || !p1->nextCursor || p1->tasks.size() != 1u) throw std::runtime_error("history page missing");
            const auto cursor = usapp::TaskService::decodeCursor(*p1->nextCursor, usapp::TaskService::View::History);
            CHECK(cursor.has_value());
            if (cursor) CHECK_EQ(cursor->id, instance(observer, root.id, today));
            const auto p2 = ts::runList(s.userB, usapp::TaskService::View::History, cursor, 1);
            CHECK(p2.has_value());
            if (p2 && p2->tasks.size() == 1u) {
                CHECK_EQ(p1->tasks[0].id, root.id); CHECK_EQ(p2->tasks[0].id, root.id);
                CHECK_NE(p1->tasks[0].periodDate, p2->tasks[0].periodDate);
            } else CHECK(false);
        } catch (const std::exception &) { c::expectThat(false, "daily-rollover", "case failed (details withheld)"); }
    }

    TEST_CASE("M5: final fault rolls back a new period; stale requests and template SQL are rejected") {
        c::Holder holder;
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto id = publish(s.userA, 1), date = day(observer), key = c::newUuid();
            CHECK(c::is(run(s.userA, id, date, c::newUuid()), Outcome::NotAssignee));
            CHECK(c::is(taskprogress::run(s.userB, id, c::newUuid()), Outcome::Validation));
            CHECK(c::is(run(s.userB, id, day(observer, -1), c::newUuid()), Outcome::Expired));
            CHECK_EQ(periods(observer, id), 0);
            pool->execSqlSync(wallet::accountSql(s.userB, s.couple, "flower"));
            pool->execSqlSync(wallet::accountSql(s.userB, s.couple, "sunshine"));
            holder = c::lockAccount(dsn, s.userB, "flower");
            auto pending = std::make_shared<c::PendingCompletion>();
            auto sentinel = std::make_shared<int>(1);
            std::weak_ptr<int> watch = sentinel;
            Service::instance().progressDailyTask(s.userB, id, key, date,
                [pending, sentinel](Service::Status status) { pending->deliver(std::move(status)); },
                Service::SettlementFault::FailAfterFirstEntry);
            sentinel.reset();
            const int pid = c::observeBlockedBy(observer, holder.pid);
            CHECK(pid != 0);
            c::endHolder(observer, holder, "daily-fault");
            CHECK(c::is(pending->wait(), Outcome::Error)); CHECK_EQ(pending->calls(), 1);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!watch.expired() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(c::Millis(10));
            CHECK(watch.expired());
            if (pid) CHECK(c::teardownEnded(observer, pid).ended);
            CHECK(c::accountLockReleased(observer, s.userB, "flower"));
            CHECK_EQ(periods(observer, id), 0);
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value()); if (wallet) { CHECK_EQ(wallet->flower, 0); CHECK_EQ(wallet->sunshine, 0); }
            CHECK(c::is(run(s.userB, id, date, key), Outcome::Completed));
            CHECK_EQ(periods(observer, id), 1);

            auto tx = c::acquireTx(pool);
            const auto rejected = wallet::exec(tx, wallet::expectFailureSql(
                "INSERT INTO task_progress_events (task_id, couple_id, user_id, progress_count, target_count, "
                "idempotency_key, recorded_at) VALUES ('" + id + "', '" + s.couple + "', '" + s.userB +
                "', 1, 1, '" + c::newUuid() + "', clock_timestamp())"));
            CHECK(!rejected.ok);
            CHECK_EQ(wallet::parseFailure(rejected.error).sqlState, std::string("UD003"));
            c::rollbackTx(tx);

            task::TaskRow badPeriod;
            badPeriod.recurrenceId = id;
            badPeriod.periodDateSql = "'" + date + "'::date";
            badPeriod.completionMode = "count";
            badPeriod.targetCount = 1;
            badPeriod.titleSql = "'wrong daily snapshot'";
            badPeriod.deadline = task::rawDeadline(badPeriod.periodDateSql, "NULL::time",
                "(('" + date + "'::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai')");
            tx = c::acquireTx(pool);
            const auto mismatch = wallet::exec(tx, wallet::expectFailureSql(
                task::taskSql(badPeriod, s.couple, s.userA, s.userB)));
            CHECK(!mismatch.ok);
            CHECK_EQ(wallet::parseFailure(mismatch.error).sqlState, std::string("UD001"));
            c::rollbackTx(tx);
        } catch (const std::exception &) { c::expectThat(false, "daily-fault", "case failed (details withheld)"); }
        c::endHolder(observer, holder, "daily-fault");
    }
}
} // namespace usapptest::daily
