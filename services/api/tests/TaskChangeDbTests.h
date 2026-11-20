#pragma once

// M6: real services on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test spaces); never resets. Races use a
// dedicated holder of the per-root advisory lock so both winning orders are exercised:
// PostgreSQL grants queued lock waiters in arrival order.
#include "CustomRangeDbTests.h"
#include "services/TaskChangeService.h"

namespace usapptest::taskchange {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using Change = usapp::TaskChangeService;
using CO = Change::Outcome;
using Outcome = usapp::TaskCompletionService::Outcome;
using PendingChange = ts::Pending<Change::Status>;
using daily::checkedExec;
using daily::day;

inline std::shared_ptr<PendingChange> startPropose(const std::string &user, const std::string &task,
                                                   const Change::ProposeInput &input, const std::string &key) {
    auto pending = std::make_shared<PendingChange>();
    Change::instance().propose(user, task, key, input, [pending](Change::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<Change::Status> propose(const std::string &user, const std::string &task,
                                             const Change::ProposeInput &input, const std::string &key = c::newUuid()) {
    return startPropose(user, task, input, key)->wait();
}
inline std::shared_ptr<PendingChange> startDecide(const std::string &user, const std::string &task,
                                                  const std::string &proposal, Change::Action action,
                                                  const std::string &key) {
    auto pending = std::make_shared<PendingChange>();
    Change::instance().decide(user, task, proposal, key, action,
                              [pending](Change::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<Change::Status> decide(const std::string &user, const std::string &task,
                                            const std::string &proposal, Change::Action action,
                                            const std::string &key = c::newUuid()) {
    return startDecide(user, task, proposal, action, key)->wait();
}
inline bool is(const std::optional<Change::Status> &s, CO outcome) { return s.has_value() && s->outcome == outcome; }
inline Change::ProposeInput cancelInput() {
    Change::ProposeInput in;
    in.kind = "cancel";
    return in;
}
inline Change::ProposeInput editInput(const usapp::TaskService::CreateInput &terms) {
    Change::ProposeInput in;
    in.kind = "edit";
    in.terms = terms;
    return in;
}
inline std::string idOf(const std::optional<Change::Status> &s) {
    return s ? s->change["id"].asString() : std::string();
}
inline std::string facts(const Change::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, s.change);
}
inline long long count(const c::Observer &o, const std::string &sql, const std::string &id) {
    return c::scalar(o, sql, id);
}
inline long long decisions(const c::Observer &o, const std::string &task) {
    return count(o, "SELECT count(*)::bigint AS n FROM task_change_decisions d JOIN task_change_proposals p "
                    "ON p.id = d.proposal_id WHERE p.task_id = $1::uuid", task);
}
inline long long closures(const c::Observer &o, const std::string &task) {
    return count(o, "SELECT count(*)::bigint AS n FROM task_closures WHERE task_id = $1::uuid", task);
}

// Dedicated holder of the per-root advisory lock (separate single-connection client).
inline c::Holder lockTask(const std::string &dsn, const std::string &taskId) {
    c::Holder h;
    h.conn = c::Observer::connect(dsn);
    h.tx = c::acquireTx(h.conn->client);
    if (!h.tx) throw std::runtime_error("holder acquisition failed");
    const auto pid = c::exec(h.tx, "SELECT pg_backend_pid() AS pid");
    if (!pid.ok || !pid.result) throw std::runtime_error("holder pid failed");
    h.pid = (*pid.result)[0]["pid"].as<int>();
    const auto lock = c::exec(h.tx, "SELECT pg_advisory_xact_lock(33004::integer, hashtext($1::text))", taskId);
    if (!lock.ok) throw std::runtime_error("holder lock failed");
    return h;
}
// Bounded wait until n backends are queued behind the holder.
inline bool waiters(const c::Observer &o, int holderPid, long long n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            const auto rows = o.client->execSqlSync(
                "SELECT count(*)::bigint AS n FROM pg_stat_activity a WHERE a.datname = current_database() "
                "AND a.pid <> pg_backend_pid() AND a.wait_event_type = 'Lock' "
                "AND $1::integer = ANY(pg_blocking_pids(a.pid))", std::to_string(holderPid));
            if (rows[0]["n"].as<long long>() >= n) return true;
        } catch (const std::exception &) {
        }
        std::this_thread::sleep_for(wallet::kPollStep);
    }
    return false;
}

// Historical/API-unproducible root fixture committed with ordinary inserts.
inline std::string commitRow(const c::ClientPtr &pool, task::TaskRow row, const ts::CommittedSpace &s) {
    auto tx = c::acquireTx(pool);
    checkedExec(tx, task::taskSql(row, s.couple, s.userA, s.userB));
    const auto committed = c::commitTx(tx);
    if (!committed || !*committed) throw std::runtime_error("fixture commit unconfirmed");
    return row.id;
}

inline usapp::TaskService::CreateInput single(const std::string &title, int flowers) {
    return ts::simpleInput(title, flowers, 0);
}

// One race: an approval of `proposal` against a completion/+1 on `task`, with a fixed queue order.
struct Race {
    std::optional<Change::Status> approval;
    std::optional<usapp::TaskCompletionService::Status> action;
};
inline Race race(const c::Observer &o, const std::string &dsn, const ts::CommittedSpace &s, const std::string &task,
                 const std::string &proposal, const std::string &approver, bool actionFirst, bool count) {
    auto holder = lockTask(dsn, task);
    auto startAction = [&]() {
        return count ? taskprogress::start(s.userB, task, c::newUuid()) : c::startComplete(s.userB, task, c::newUuid());
    };
    std::shared_ptr<c::PendingCompletion> action;
    std::shared_ptr<PendingChange> approval;
    if (actionFirst) {
        action = startAction();
        c::expectThat(waiters(o, holder.pid, 1), "m6-race", "action not queued");
        approval = startDecide(approver, task, proposal, Change::Action::Approve, c::newUuid());
    } else {
        approval = startDecide(approver, task, proposal, Change::Action::Approve, c::newUuid());
        c::expectThat(waiters(o, holder.pid, 1), "m6-race", "approval not queued");
        action = startAction();
    }
    c::expectThat(waiters(o, holder.pid, 2), "m6-race", "second request not queued");
    c::endHolder(o, holder, "m6-race");
    Race r;
    r.action = action->wait();
    r.approval = approval->wait();
    CHECK_EQ(action->calls(), 1);
    CHECK_EQ(approval->calls(), 1);
    return r;
}

inline void runTaskChangeDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    using A = Change::Action;

    TEST_CASE("M6: confirmed edit supersedes a one-off; facts replay, old actions refused, successor settles once") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto id = c::publish(s.userA, "M6 edit", 5, 0);
            const auto edit = editInput(single("M6 edited", 8));
            const auto proposeKey = c::newUuid();
            const auto proposed = propose(s.userA, id, edit, proposeKey);
            CHECK(is(proposed, CO::Proposed));
            const auto pid = idOf(proposed);
            if (proposed) {
                CHECK(proposed->change["decision"].isNull());
                CHECK_EQ(proposed->task.status, std::string("open"));
                CHECK_FALSE(proposed->task.pendingChangeJson.empty());
            }
            const auto again = propose(s.userA, id, edit, proposeKey);
            CHECK(is(again, CO::Replayed));
            if (again && proposed) CHECK_EQ(facts(*again), facts(*proposed));
            CHECK(is(propose(s.userA, id, editInput(single("M6 other", 9)), proposeKey), CO::KeyReused));
            CHECK(is(propose(s.userB, id, cancelInput()), CO::Pending));
            CHECK(is(propose(s.userB, id, edit), CO::Forbidden));
            CHECK(is(propose(other.userA, id, cancelInput()), CO::TaskNotFound));
            CHECK(is(decide(s.userA, id, pid, A::Approve), CO::Forbidden));
            CHECK(is(decide(s.userB, id, pid, A::Withdraw), CO::Forbidden));
            CHECK(is(decide(s.userB, id, c::newUuid(), A::Approve), CO::ChangeNotFound));

            const auto approveKey = c::newUuid();
            const auto approved = decide(s.userB, id, pid, A::Approve, approveKey);
            CHECK(is(approved, CO::Decided));
            std::string successor;
            if (approved) {
                CHECK_EQ(approved->change["decision"]["outcome"].asString(), std::string("approved"));
                CHECK_EQ(approved->task.status, std::string("superseded"));
                CHECK(approved->successor.has_value());
                if (approved->successor) {
                    successor = approved->successor->id;
                    CHECK_EQ(approved->successor->status, std::string("open"));
                    CHECK_EQ(approved->successor->rewardFlowers, 8);
                    CHECK_EQ(approved->successor->replaces, id);
                    CHECK_EQ(approved->change["decision"]["successor_task_id"].asString(), successor);
                }
            }
            const auto replay = decide(s.userB, id, pid, A::Approve, approveKey);
            CHECK(is(replay, CO::Replayed));
            if (replay && approved) {
                CHECK_EQ(facts(*replay), facts(*approved));  // immutable facts only
                CHECK_EQ(replay->task.status, std::string("superseded"));
            }
            CHECK(is(decide(s.userB, id, pid, A::Reject), CO::AlreadyDecided));
            CHECK(is(propose(s.userB, id, cancelInput()), CO::Superseded));

            CHECK(c::is(c::runComplete(s.userB, id, c::newUuid()), Outcome::Superseded));
            CHECK_EQ(c::taskOps(observer, id), 0);
            CHECK_EQ(c::completions(observer, id), 0);
            const auto done = c::runComplete(s.userB, successor, c::newUuid());
            CHECK(c::is(done, Outcome::Completed));
            if (done) CHECK_EQ(done->settlement.flower, 8);
            CHECK_EQ(c::taskOps(observer, successor), 1);
            CHECK(is(propose(s.userB, successor, cancelInput()), CO::AlreadyCompleted));

            const auto history = ts::runList(s.userA, usapp::TaskService::View::History, std::nullopt, 20);
            CHECK(history.has_value());
            if (history) {
                int rows = 0;
                for (const auto &t : history->tasks) rows += t.id == id && t.status == "superseded" ? 1 : 0;
                CHECK_EQ(rows, 1);
            }
            const auto open = ts::runList(s.userB, usapp::TaskService::View::Open, std::nullopt, 20);
            CHECK(open.has_value());
            if (open) for (const auto &t : open->tasks) CHECK_NE(t.id, id);
        } catch (const std::exception &) { c::expectThat(false, "m6-edit", "case failed (details withheld)"); }
    }

    TEST_CASE("M6: edit vs completion race in both orders (single task and target-1 count task)") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            for (const bool count : {false, true}) {
                for (const bool actionFirst : {true, false}) {
                    const auto id = count ? taskprogress::publish(s.userA, 1, 2, 0) : c::publish(s.userA, "M6 race", 2, 0);
                    auto terms = single("M6 race edit", 3);
                    if (count) { terms.completionMode = "count"; terms.targetCount = 1; }
                    const auto pid = idOf(propose(s.userA, id, editInput(terms)));
                    const auto r = race(observer, dsn, s, id, pid, s.userB, actionFirst, count);
                    if (actionFirst) {
                        CHECK(c::is(r.action, Outcome::Completed));
                        CHECK(is(r.approval, CO::AlreadyCompleted));
                        CHECK_EQ(c::taskOps(observer, id), 1);
                        CHECK_EQ(closures(observer, id), 0);
                        CHECK_EQ(decisions(observer, id), 0);
                        // The blocked proposal stays decidable: the proposer withdraws it.
                        CHECK(is(decide(s.userA, id, pid, A::Withdraw), CO::Decided));
                    } else {
                        CHECK(is(r.approval, CO::Decided));
                        CHECK(c::is(r.action, Outcome::Superseded));
                        CHECK_EQ(c::taskOps(observer, id), 0);
                        CHECK_EQ(c::completions(observer, id), 0);
                        CHECK_EQ(closures(observer, id), 1);
                    }
                }
            }
        } catch (const std::exception &) { c::expectThat(false, "m6-edit-race", "case failed (details withheld)"); }
    }

    TEST_CASE("M6: cancel vs final +1 race on a task with progress; progress kept, no partial reward") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            for (const bool actionFirst : {true, false}) {
                const auto id = taskprogress::publish(s.userA, 2, 2, 0);
                CHECK(c::is(taskprogress::run(s.userB, id, c::newUuid()), Outcome::Progressed));
                const auto pid = idOf(propose(s.userA, id, cancelInput()));
                const auto r = race(observer, dsn, s, id, pid, s.userB, actionFirst, true);
                if (actionFirst) {
                    CHECK(c::is(r.action, Outcome::Completed));
                    CHECK(is(r.approval, CO::AlreadyCompleted));
                    CHECK_EQ(c::taskOps(observer, id), 1);
                    CHECK_EQ(closures(observer, id), 0);
                    CHECK(is(decide(s.userB, id, pid, A::Reject), CO::Decided));
                } else {
                    CHECK(is(r.approval, CO::Decided));
                    CHECK(c::is(r.action, Outcome::Cancelled));
                    CHECK_EQ(taskprogress::events(observer, id), 1);
                    CHECK_EQ(c::taskOps(observer, id), 0);
                    const auto read = ts::runGet(s.userA, id);
                    CHECK(read.has_value());
                    if (read) {
                        CHECK_EQ(read->task.status, std::string("cancelled"));
                        CHECK_EQ(read->task.progressCount, 1);
                    }
                }
            }
        } catch (const std::exception &) { c::expectThat(false, "m6-cancel-race", "case failed (details withheld)"); }
    }

    TEST_CASE("M6: progress blocks an edit (still rejectable); expired task refuses proposals") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto id = taskprogress::publish(s.userA, 3, 2, 0);
            CHECK(c::is(taskprogress::run(s.userB, id, c::newUuid()), Outcome::Progressed));
            auto terms = single("M6 blocked", 2);
            terms.completionMode = "count";
            terms.targetCount = 2;
            const auto proposed = propose(s.userA, id, editInput(terms));
            CHECK(is(proposed, CO::Proposed));
            const auto read = ts::runGet(s.userB, id);
            if (read) {
                const auto json = usapp::TaskService::taskJson(read->task);
                CHECK_EQ(json["pending_change"]["blocked_reason"].asString(), std::string("progress"));
            }
            CHECK(is(decide(s.userB, id, idOf(proposed), A::Approve), CO::HasProgress));
            CHECK_EQ(decisions(observer, id), 0);
            CHECK(is(decide(s.userB, id, idOf(proposed), A::Reject), CO::Decided));
            CHECK_EQ(taskprogress::events(observer, id), 1);

            task::TaskRow past;
            past.flowers = 1; past.sunshine = 0;
            past.createdAtSql = "clock_timestamp() - interval '2 hours'";
            past.deadline = task::relativeDeadline("-1 hour");
            const auto expired = commitRow(pool, past, s);
            CHECK(is(propose(s.userB, expired, cancelInput()), CO::Expired));
        } catch (const std::exception &) { c::expectThat(false, "m6-blocked", "case failed (details withheld)"); }
    }

    TEST_CASE("M6: daily/weekly templates: edit blocked by current progress; cancel keeps a completed period") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto today = day(observer);
            const auto root = daily::publish(s.userA, 1);
            CHECK(c::is(daily::run(s.userB, root, today, c::newUuid()), Outcome::Completed));
            auto terms = daily::input(1);
            terms.rewardFlowers = 4;
            const auto edit = propose(s.userA, root, editInput(terms));
            CHECK(is(edit, CO::Proposed));
            CHECK(is(decide(s.userB, root, idOf(edit), A::Approve), CO::HasProgress));
            CHECK(is(decide(s.userA, root, idOf(edit), A::Withdraw), CO::Decided));
            const auto cancel = propose(s.userB, root, cancelInput());
            CHECK(is(decide(s.userA, root, idOf(cancel), A::Approve), CO::Decided));
            const auto period = daily::get(s.userA, root, today);
            CHECK(period.has_value());
            if (period) {
                CHECK_EQ(period->task.status, std::string("completed"));  // the completed period keeps its reward
                const auto json = usapp::TaskService::taskJson(period->task);
                CHECK_EQ(json["closure"]["reason"].asString(), std::string("cancelled"));
            }
            CHECK_EQ(c::completions(observer, daily::instance(observer, root, today)), 1);
            CHECK(c::is(daily::run(s.userB, root, today, c::newUuid()), Outcome::Cancelled));

            // A cancelled template with nothing stored: no new period, even by direct SQL (UX007).
            const auto idle = daily::publish(s.userA, 1);
            const auto idleCancel = propose(s.userA, idle, cancelInput());
            CHECK(is(decide(s.userB, idle, idOf(idleCancel), A::Approve), CO::Decided));
            const auto current = ts::runGet(s.userB, idle);
            if (current) CHECK_EQ(current->task.status, std::string("cancelled"));
            CHECK(c::is(daily::run(s.userB, idle, today, c::newUuid()), Outcome::Cancelled));
            CHECK_EQ(daily::periods(observer, idle), 0);
            auto tx = c::acquireTx(pool);
            const auto direct = wallet::exec(tx, wallet::expectFailureSql(
                "INSERT INTO tasks (couple_id, creator_id, assignee_id, title, description, reward_flowers, "
                "reward_sunshine, timezone, deadline_local_date, deadline_at, create_idempotency_key, "
                "create_fingerprint, created_at, completion_mode, target_count, recurrence_id, period_date) "
                "SELECT r.couple_id, r.creator_id, r.assignee_id, r.title, r.description, r.reward_flowers, "
                "r.reward_sunshine, r.timezone, '" + today + "'::date, ('" + today + "'::date + 1)::timestamp "
                "AT TIME ZONE 'Asia/Shanghai', '" + task::taskKey() + "', r.create_fingerprint, clock_timestamp(), "
                "r.completion_mode, r.target_count, r.id, '" + today + "'::date FROM tasks r WHERE r.id = '" + idle + "'"));
            CHECK(!direct.ok);
            CHECK_EQ(wallet::parseFailure(direct.error).sqlState, std::string("UX007"));
            c::rollbackTx(tx);

            // Weekly edit before any progress: the successor template starts at this week.
            auto weekly = ts::simpleInput("M6 weekly", 1, 0);
            weekly.completionMode = "count"; weekly.targetCount = 2; weekly.scheduleType = "weekly";
            const auto made = ts::runCreate(s.userA, weekly, c::newUuid());
            if (!made || made->outcome != usapp::TaskService::Outcome::Created) throw std::runtime_error("weekly");
            const auto week = made->task.id;
            weekly.rewardFlowers = 5;
            const auto wEdit = propose(s.userA, week, editInput(weekly));
            const auto wApproved = decide(s.userB, week, idOf(wEdit), A::Approve);
            CHECK(is(wApproved, CO::Decided));
            if (wApproved && wApproved->successor) {
                CHECK_EQ(wApproved->successor->scheduleType, std::string("weekly"));
                CHECK_EQ(wApproved->successor->rewardFlowers, 5);
                CHECK_EQ(wApproved->successor->status, std::string("open"));
                CHECK_EQ(wApproved->task.status, std::string("superseded"));
            }
        } catch (const std::exception &) { c::expectThat(false, "m6-recurring", "case failed (details withheld)"); }
    }

    TEST_CASE("M6: a started custom range keeps its start; a changed start must not be in the past") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            auto rangeRow = [&](void) {
                task::TaskRow r;
                r.scheduleType = "custom"; r.completionMode = "count"; r.targetCount = 2;
                r.flowers = 1; r.sunshine = 0;
                r.createdAtSql = "clock_timestamp() - interval '2 days'";
                r.startDateSql = "'" + day(observer, -1) + "'::date";
                r.deadline = task::dateOnlyDeadline(day(observer, 2));
                return r;
            };
            const auto started = commitRow(pool, rangeRow(), s);
            auto keep = customrange::input(3, day(observer, -1), day(observer, 3));
            const auto kept = propose(s.userA, started, editInput(keep));
            CHECK(is(kept, CO::Proposed));
            const auto approved = decide(s.userB, started, idOf(kept), A::Approve);
            CHECK(is(approved, CO::Decided));
            if (approved && approved->successor) {
                CHECK_EQ(approved->successor->startDate, day(observer, -1));
                CHECK_EQ(approved->successor->targetCount, 3);
            }
            const auto other = commitRow(pool, rangeRow(), s);
            auto moved = customrange::input(3, day(observer, -2), day(observer, 3));
            CHECK(is(propose(s.userA, other, editInput(moved)), CO::Validation));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM task_change_proposals WHERE task_id = $1::uuid",
                           other), 0);
        } catch (const std::exception &) { c::expectThat(false, "m6-custom", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::taskchange
