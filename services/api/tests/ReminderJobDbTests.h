#pragma once

// P2-6 R2: the real scheduler and ReminderJob on literal usapp_test, with real completion, change, rule and
// anniversary services. Isolated committed fixtures (@m3-fixture.test spaces); assertions target this run's ids.
// The clock cannot be injected into the 0025 guards, so due fixtures are backdated rows whose instant is the
// minute just begun. Other instants are read with the scan's own clock parameter, which writes nothing.
// A case straddling Beijing midnight can fail spuriously.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <json/json.h>

#include "ReminderDbTests.h"
#include "config/AppConfig.h"
#include "db/Db.h"
#include "scheduler/Scheduler.h"
#include "scheduler/jobs/ReminderJob.h"
#include "services/ReminderGeneration.h"

namespace usapptest::reminderjob {
namespace a = usapptest::anniversary;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace d = usapptest::daily;
namespace n = usapptest::notification;
namespace r = usapptest::reminder;
namespace tc = usapptest::taskchange;
namespace gen = usapp::reminder;
using usapp::scheduler::Scheduler;
using usapp::scheduler::SchedulerJob;

// Registered after ReminderJob. Armed, it generates through the same code and then fails, so the real scheduler
// rolls back after the occurrences and notifications were inserted.
class FaultJob : public SchedulerJob {
  public:
    bool armed = false;
    std::string jobKey() const override { return "reminder.test.fault"; }
    void runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) override {
        if (!armed) {
            done(Result{});
            return;
        }
        gen::Policy policy;
        policy.enabled = true;
        auto generator = std::make_shared<gen::Generator>(policy);
        generator->runTick(tx, [generator, done](gen::TickReport report) {
            Result result;
            result.ok = false;
            result.detail = report.ok && report.created > 0 ? "forced_after_insert" : "fault_precondition";
            done(std::move(result));
        });
    }
};

struct Env {
    c::ClientPtr pool;
    std::string dsn;
    std::optional<c::Observer> observer;
    std::shared_ptr<usapp::scheduler::ReminderJob> job;
    std::shared_ptr<FaultJob> fault;
};
inline Env env;

inline const c::Observer &obs() { return *env.observer; }
inline void usePolicy(const gen::Policy &policy) { env.job->setGenerator(std::make_shared<gen::Generator>(policy)); }
inline gen::Policy onPolicy() {
    gen::Policy policy;
    policy.enabled = true;
    return policy;
}

using Reports = std::vector<Scheduler::TickReport>;
using PendingTick = ts::Pending<Reports>;
inline std::shared_ptr<PendingTick> startTick() {
    auto reports = std::make_shared<Reports>();
    auto pending = std::make_shared<PendingTick>();
    Scheduler::instance().tickOnce([reports](const Scheduler::TickReport &report) { reports->push_back(report); },
                                   [reports, pending]() { pending->deliver(*reports); });
    return pending;
}
inline Scheduler::TickReport reminderReport(const std::optional<Reports> &reports) {
    if (!reports) throw std::runtime_error("tick timed out");
    for (const auto &report : *reports) {
        if (report.jobKey == usapp::scheduler::ReminderJob::kJobKey) return report;
    }
    throw std::runtime_error("no reminder report");
}
inline bool completed(const Scheduler::TickReport &report) { return report.outcome == Scheduler::TickOutcome::Completed; }
inline Scheduler::TickReport tick() { return reminderReport(startTick()->wait()); }

inline std::string lastDetail() {
    return obs().client->execSqlSync(
        "SELECT detail->>'message' AS m FROM scheduler_ticks WHERE job_key = 'reminder.generate' ORDER BY id DESC LIMIT 1")[0]["m"]
        .as<std::string>();
}
inline long long counted(const std::string &detail, const std::string &name) {
    const auto at = detail.find(name + "=");
    if (at == std::string::npos) throw std::runtime_error("no count in tick detail");
    return std::stoll(detail.substr(at + name.size() + 1));
}
// A completed tick; returns how many occurrences it created.
inline long long tickCreated() {
    const auto report = tick();
    if (!completed(report)) throw std::runtime_error("tick not completed");
    return counted(lastDetail(), "created");
}
inline void drain() {
    for (int i = 0; i < 40 && tickCreated() > 0; ++i) {
    }
}

// Generation without the scheduler's job lock: only the domain locks and identity remain.
inline std::shared_ptr<ts::Pending<gen::TickReport>> startDirect() {
    auto pending = std::make_shared<ts::Pending<gen::TickReport>>();
    auto generator = std::make_shared<gen::Generator>(onPolicy());
    usapp::db::TxScope::begin(
        [pending, generator](std::shared_ptr<usapp::db::TxScope> scope) {
            generator->runTick(scope->tx(), [pending, scope, generator](gen::TickReport report) {
                if (!report.ok) {
                    scope->rollback([pending, report]() { pending->deliver(report); });
                    return;
                }
                scope->commit([pending, report]() { pending->deliver(report); });
            });
        },
        [pending](const std::string &) { pending->deliver(gen::TickReport{}); });
    return pending;
}

// ---------------------------------------------------------------- fixtures
inline std::string today() { return d::day(obs()); }
inline std::string minuteNow() { return r::minuteNow(obs()); }
// A few minutes ago, clamped to today's first minute.
inline std::string minuteBack(int minutes) {
    return obs().client->execSqlSync(
        "SELECT to_char(GREATEST((clock_timestamp() AT TIME ZONE 'Asia/Shanghai') - make_interval(mins => $1::integer), "
        "((clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date)::timestamp), 'HH24:MI') AS v",
        std::to_string(minutes))[0]["v"].as<std::string>();
}
inline ts::CommittedSpace space() {
    ts::CommittedSpace s;
    ts::commitSpace(env.pool, s, true);
    return s;
}
inline std::string directTask(const ts::CommittedSpace &s, const std::string &schedule = "one_off", int target = 2) {
    const auto id = c::newUuid();
    if (schedule == "one_off") {
        env.pool->execSqlSync(
            "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, "
            "create_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'due', 1, 1, $5, repeat('b', 64), "
            "clock_timestamp() - interval '2 days')",
            id, s.couple, s.userA, s.userB, c::newUuid());
    } else {
        env.pool->execSqlSync(
            "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, "
            "create_fingerprint, completion_mode, target_count, schedule_type, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, "
            "'due', 1, 1, $5, repeat('b', 64), 'count', $7::integer, $6, clock_timestamp() - interval '2 days')",
            id, s.couple, s.userA, s.userB, c::newUuid(), schedule, std::to_string(target));
    }
    return id;
}
// The one-off with no deadline: its only mode is an absolute date and time.
inline void absoluteRule(const ts::CommittedSpace &s, const std::string &task, const std::string &date, const std::string &at) {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, at_local_date, at_time, idempotency_key, "
        "request_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'absolute', $4::date, $5::time, $6, repeat('a', 64), "
        "clock_timestamp() - interval '1 day')",
        s.couple, s.userB, task, date, at, c::newUuid());
}
inline void localRule(const ts::CommittedSpace &s, const std::string &task, int days, const std::string &at, int seq = 1,
                      const std::string &age = "1 day") {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, days_before, at_time, idempotency_key, "
        "request_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, $3::integer, $4::uuid, true, 'local_time', $5::integer, $6::time, $7, "
        "repeat('a', 64), clock_timestamp() - $8::interval)",
        s.couple, s.userB, std::to_string(seq), task, std::to_string(days), at, c::newUuid(), age);
}
inline std::string dueOneOff(const ts::CommittedSpace &s) {
    const auto task = directTask(s);
    absoluteRule(s, task, today(), minuteNow());
    return task;
}
inline std::string dueDaily(const ts::CommittedSpace &s, int target = 2) {
    const auto task = directTask(s, "daily", target);
    localRule(s, task, 0, minuteNow());
    return task;
}
inline std::string dueWeekly(const ts::CommittedSpace &s) {
    const auto task = directTask(s, "weekly");
    const auto days = obs().client->execSqlSync(
        "SELECT (7 - EXTRACT(ISODOW FROM (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date)::integer)::text AS v")[0]["v"].as<std::string>();
    localRule(s, task, std::stoi(days), minuteNow());
    return task;
}
struct Item {
    std::string id;
    std::string base;
};
// Directly, with explicit civil values and a creation time, so any past instant can be read.
inline Item directAnniversary(const ts::CommittedSpace &s, const std::string &base, const std::string &kind, const std::string &createdAt) {
    Item item{c::newUuid(), base};
    auto tx = c::acquireTx(env.pool);
    if (!c::exec(tx, "INSERT INTO anniversaries (id, couple_id, creator_id, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::timestamptz)",
                 item.id, s.couple, s.userA, createdAt).ok ||
        !c::exec(tx, "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind, note, "
                     "idempotency_key, request_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, 1, $3::uuid, 'create', 'due', $4::date, $5, '', $6, "
                     "repeat('a', 64), $7::timestamptz)",
                 item.id, s.couple, s.userA, base, kind, c::newUuid(), createdAt).ok ||
        !c::commitTx(tx).value_or(false)) {
        throw std::runtime_error("anniversary fixture failed");
    }
    return item;
}
inline void offsetRule(const ts::CommittedSpace &s, const std::string &owner, const std::string &anniversary, const std::string &set,
                       const std::string &at, const std::string &createdAt) {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, anniversary_id, enabled, mode, days_before_set, at_time, idempotency_key, "
        "request_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'anniversary_offsets', $4::smallint[], $5::time, $6, "
        "repeat('a', 64), $7::timestamptz)",
        s.couple, owner, anniversary, set, at, c::newUuid(), createdAt);
}
inline std::string ago(const std::string &interval) {
    return obs().client->execSqlSync("SELECT (clock_timestamp() - $1::interval)::text AS v", interval)[0]["v"].as<std::string>();
}
// Base four years before the target, so the target is a real yearly date.
inline std::string baseFor(int offsetFromToday) {
    return obs().client->execSqlSync(
        "SELECT to_char(((clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + $1::integer - interval '4 years')::date, 'YYYY-MM-DD') AS v",
        std::to_string(offsetFromToday))[0]["v"].as<std::string>();
}
inline Item dueAnniversary(const ts::CommittedSpace &s, const std::string &owner, int offset, const std::string &set = "",
                           const std::string &kind = "anniversary") {
    const auto base = kind == "countdown" ? d::day(obs(), offset) : baseFor(offset);
    const auto item = directAnniversary(s, base, kind, ago("3 days"));
    offsetRule(s, owner, item.id, set.empty() ? "{" + std::to_string(offset) + "}" : set, minuteNow(), ago("1 day"));
    return item;
}

inline long long occurrences(const std::string &target) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM reminder_occurrences WHERE task_id = $1::uuid OR anniversary_id = $1::uuid", target);
}
inline long long noticesFor(const std::string &target) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM notifications n JOIN reminder_occurrences o ON o.id = n.reminder_occurrence_id "
                            "WHERE o.task_id = $1::uuid OR o.anniversary_id = $1::uuid", target);
}
inline long long pushRowsFor(const std::string &recipient) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM push_deliveries p JOIN notifications x ON x.id = p.notification_id "
                            "WHERE x.recipient_id = $1::uuid", recipient);
}
inline bool firedOnce(const std::string &target) { return occurrences(target) == 1 && noticesFor(target) == 1; }
inline bool nothing(const std::string &target) { return occurrences(target) == 0 && noticesFor(target) == 0; }

// The scan as of an instant: reads only.
struct Candidate {
    std::string kind, targetId, ownerId, periodDate, targetDate, fireAt;
    int daysBefore = -1;
    std::string identity() const {
        return kind + "|" + targetId + "|" + ownerId + "|" + periodDate + "|" + targetDate + "|" + std::to_string(daysBefore);
    }
};
inline std::vector<Candidate> scanAt(const std::string &at, int limit = 100000) {
    const auto rows = obs().client->execSqlSync(gen::kScanSql, at, std::to_string(limit));
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    const auto text = rows[0]["candidates"].as<std::string>();
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &parsed, &errors)) throw std::runtime_error("scan not JSON");
    std::vector<Candidate> out;
    for (const auto &item : parsed) {
        Candidate one;
        one.kind = item["kind"].asString();
        one.targetId = item["target_id"].asString();
        one.ownerId = item["owner_id"].asString();
        one.periodDate = item["period_date"].isNull() ? "" : item["period_date"].asString();
        one.targetDate = item["target_date"].isNull() ? "" : item["target_date"].asString();
        one.fireAt = item["fire_at"].asString();
        one.daysBefore = item["days_before"].isNull() ? -1 : item["days_before"].asInt();
        out.push_back(one);
    }
    return out;
}
inline std::vector<Candidate> scanFor(const std::string &at, const std::string &target) {
    std::vector<Candidate> out;
    for (const auto &one : scanAt(at)) {
        if (one.targetId == target) out.push_back(one);
    }
    return out;
}

// An advisory lock held on its own connection, for deterministic interleaving.
inline c::Holder hold(int ns, const std::string &text) {
    c::Holder h;
    h.conn = c::Observer::connect(env.dsn);
    h.tx = c::acquireTx(h.conn->client);
    if (!h.tx) throw std::runtime_error("holder acquisition failed");
    const auto pid = c::exec(h.tx, "SELECT pg_backend_pid() AS pid");
    if (!pid.ok || !pid.result) throw std::runtime_error("holder pid failed");
    h.pid = (*pid.result)[0]["pid"].as<int>();
    if (!c::exec(h.tx, "SELECT pg_advisory_xact_lock($1::integer, hashtext($2::text))", std::to_string(ns), text).ok) {
        throw std::runtime_error("holder lock failed");
    }
    return h;
}
inline void release(c::Holder &h, const std::string &label) {
    c::commitTx(h.tx);
    c::endHolder(obs(), h, label);
}

// Both interleavings of one write and one tick, each blocked behind the holder until both are queued. The write's
// waitable is type-erased. In either order the tick has already scanned the candidate before the lock.
using StartWrite = std::function<std::function<void()>()>;
inline void writerThenTick(int ns, const std::string &target, const StartWrite &write) {
    auto holder = hold(ns, target);
    auto finishWrite = write();
    CHECK(tc::waiters(obs(), holder.pid, 1));
    auto ticking = startTick();
    CHECK(tc::waiters(obs(), holder.pid, 2));
    release(holder, "reminder-race");
    finishWrite();
    CHECK(completed(reminderReport(ticking->wait())));
    CHECK_EQ(counted(lastDetail(), "scanned"), 1);
    CHECK_EQ(counted(lastDetail(), "created"), 0);
}
inline void tickThenWriter(int ns, const std::string &target, const StartWrite &write) {
    auto holder = hold(ns, target);
    auto ticking = startTick();
    CHECK(tc::waiters(obs(), holder.pid, 1));
    auto finishWrite = write();
    CHECK(tc::waiters(obs(), holder.pid, 2));
    release(holder, "reminder-race");
    CHECK(completed(reminderReport(ticking->wait())));
    CHECK_EQ(counted(lastDetail(), "created"), 1);
    finishWrite();
}

inline void runReminderJobDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    env.pool = pool;
    env.dsn = dsn;
    env.observer = c::Observer::connect(dsn);
    usapp::AppConfig::instance().schedulerKeyNamespace = "usapp.test.p26r2";
    Scheduler::instance().configure(false, 15000, "test-a");
    gen::Policy off;  // the production default
    env.job = std::make_shared<usapp::scheduler::ReminderJob>(std::make_shared<gen::Generator>(off));
    env.fault = std::make_shared<FaultJob>();

    TEST_CASE("P2-6 R2: off is the default and writes no reminder; registering writes nothing") {
        try {
            CHECK_FALSE(gen::Policy{}.enabled);
            const auto s = space();
            const auto task = dueOneOff(s);
            const auto before = obs().client->execSqlSync(
                "SELECT (SELECT count(*) FROM reminder_occurrences) || '/' || (SELECT count(*) FROM notifications) || '/' || "
                "(SELECT count(*) FROM scheduler_ticks) AS v")[0]["v"].as<std::string>();
            Scheduler::instance().registerJob(env.job);
            Scheduler::instance().registerJob(env.fault);
            const auto registered = obs().client->execSqlSync(
                "SELECT (SELECT count(*) FROM reminder_occurrences) || '/' || (SELECT count(*) FROM notifications) || '/' || "
                "(SELECT count(*) FROM scheduler_ticks) AS v")[0]["v"].as<std::string>();
            CHECK_EQ(registered, before);
            const auto occurrencesBefore = c::scalar(obs(), "SELECT count(*)::bigint AS n FROM reminder_occurrences WHERE $1::text <> ''", task);
            CHECK(completed(tick()));
            CHECK_EQ(lastDetail(), std::string("reminders dispatch=off"));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM reminder_occurrences WHERE $1::text <> ''", task), occurrencesBefore);
            CHECK(nothing(task));
            usePolicy(onPolicy());
            drain();  // whatever earlier suites left due
            CHECK(firedOnce(task));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-off", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: tasks fire once in the current period, create no period row and need no Push") {
        try {
            const auto s = space();
            const auto once = dueOneOff(s);
            const auto daily = dueDaily(s);
            const auto weekly = dueWeekly(s);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM push_subscriptions WHERE user_id = $1::uuid", s.userB), 0);
            CHECK_EQ(tickCreated(), 3);
            for (const auto &id : {once, daily, weekly}) CHECK(firedOnce(id));
            // Untouched periods stay unmaterialized.
            CHECK_EQ(d::periods(obs(), daily), 0);
            CHECK_EQ(d::periods(obs(), weekly), 0);
            // Period identity: the Beijing date, and that week's Monday.
            const auto periodOf = [](const std::string &id) {
                return obs().client->execSqlSync("SELECT period_date::text AS v FROM reminder_occurrences WHERE task_id = $1::uuid", id)[0]["v"].as<std::string>();
            };
            CHECK_EQ(periodOf(daily), today());
            CHECK_EQ(periodOf(weekly),
                     obs().client->execSqlSync("SELECT date_trunc('week', (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date)::date::text AS v")[0]["v"].as<std::string>());
            // A retry creates nothing.
            CHECK_EQ(tickCreated(), 0);
            for (const auto &id : {once, daily, weekly}) CHECK(firedOnce(id));
            // The inbox shows them, to the owner only; generation creates no push rows.
            const auto inbox = n::list(s.userB);
            CHECK(inbox && inbox->status == 200);
            if (inbox) {
                CHECK_EQ(inbox->body["notifications"].size(), 3u);
                for (const auto &item : inbox->body["notifications"]) {
                    CHECK(item["actor_id"].isNull());
                    CHECK_EQ(item["event_type"].asString(), std::string("reminder.task"));
                }
            }
            const auto unread = n::unreadCount(s.userB);
            if (unread) CHECK_EQ(unread->body["unread_count"].asInt(), 3);
            const auto partner = n::list(s.userA);
            if (partner) CHECK_EQ(partner->body["notifications"].size(), 0u);
            CHECK_EQ(pushRowsFor(s.userB), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-tasks", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: partial progress and pending proposals fire; completion, closure and expiry do not") {
        try {
            const auto s = space();
            const auto date = today();
            // Partial progress keeps it due, and materializes only because the service did.
            const auto partial = dueDaily(s);
            d::run(s.userB, partial, date, c::newUuid());
            CHECK_EQ(d::periods(obs(), partial), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_completions k JOIN tasks p ON p.id = k.task_id WHERE p.recurrence_id = $1::uuid", partial), 0);
            // A completed period does not fire.
            const auto finished = dueDaily(s);
            d::run(s.userB, finished, date, c::newUuid());
            d::run(s.userB, finished, date, c::newUuid());
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_completions k JOIN tasks p ON p.id = k.task_id WHERE p.recurrence_id = $1::uuid", finished), 1);
            const auto done = dueOneOff(s);
            c::runComplete(s.userB, done, c::newUuid());
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_completions WHERE task_id = $1::uuid", done), 1);
            // Pending proposals change nothing until approved.
            const auto pendingCancel = dueOneOff(s);
            CHECK(tc::is(tc::propose(s.userA, pendingCancel, tc::cancelInput()), tc::CO::Proposed));
            const auto pendingEdit = dueOneOff(s);
            CHECK(tc::is(tc::propose(s.userA, pendingEdit, tc::editInput(ts::simpleInput("edited", 1, 1))), tc::CO::Proposed));
            // Cancelled and superseded roots do not fire.
            const auto cancelled = dueOneOff(s);
            const auto cancel = tc::propose(s.userA, cancelled, tc::cancelInput());
            CHECK(tc::is(tc::decide(s.userB, cancelled, tc::idOf(cancel), usapp::TaskChangeService::Action::Approve), tc::CO::Decided));
            const auto superseded = dueOneOff(s);
            const auto edit = tc::propose(s.userA, superseded, tc::editInput(ts::simpleInput("edited", 1, 1)));
            CHECK(tc::is(tc::decide(s.userB, superseded, tc::idOf(edit), usapp::TaskChangeService::Action::Approve), tc::CO::Decided));
            CHECK_EQ(tc::closures(obs(), superseded), 1);
            // Expired: date-only deadline yesterday, so the window closed at today's midnight.
            const auto expired = c::newUuid();
            const auto yesterday = d::day(obs(), -1);
            pool->execSqlSync(
                "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, timezone, deadline_local_date, deadline_at, "
                "create_idempotency_key, create_fingerprint, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'gone', 1, 1, 'Asia/Shanghai', "
                "$5::date, (($5::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai'), $6, repeat('b', 64), clock_timestamp() - interval '3 days')",
                expired, s.couple, s.userA, s.userB, yesterday, c::newUuid());
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, idempotency_key, request_fingerprint, created_at) "
                "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'before_deadline', 30, $4, repeat('a', 64), clock_timestamp() - interval '2 days')",
                s.couple, s.userB, expired, c::newUuid());
            // Past the two-hour grace, with no deadline: permanently skipped, no catch-up.
            const auto late = directTask(s);
            const auto lateFire = obs().client->execSqlSync(
                "SELECT to_char((clock_timestamp() - interval '3 hours') AT TIME ZONE 'Asia/Shanghai', 'YYYY-MM-DD') AS d, "
                "to_char((clock_timestamp() - interval '3 hours') AT TIME ZONE 'Asia/Shanghai', 'HH24:MI') AS t");
            absoluteRule(s, late, lateFire[0]["d"].as<std::string>(), lateFire[0]["t"].as<std::string>());
            CHECK_EQ(tickCreated(), 3);
            for (const auto &id : {partial, pendingCancel, pendingEdit}) CHECK(firedOnce(id));
            for (const auto &id : {finished, done, cancelled, superseded, expired, late}) CHECK(nothing(id));
            // Only the completed period is suppressed: the next day's is due as of its own time.
            CHECK_EQ(scanFor(d::day(obs(), 1) + " " + minuteNow() + ":30+08", finished).size(), 1u);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-task-states", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: rule semantics: identical re-save fires; a change or re-enable is new; no refire") {
        try {
            const auto s = space();
            // Identical re-save after the instant.
            const auto same = directTask(s, "daily");
            const auto at = minuteNow();
            localRule(s, same, 0, at);
            CHECK(r::is(r::setTask(s.userB, same, r::localTime(0, at)), r::RO::Created));
            // Disabled, and disabled then re-enabled after the instant: nothing retroactive.
            const auto disabled = dueDaily(s);
            CHECK(r::is(r::setTask(s.userB, disabled, r::disabled()), r::RO::Created));
            const auto reenabled = directTask(s, "daily");
            localRule(s, reenabled, 0, at);
            CHECK(r::is(r::setTask(s.userB, reenabled, r::disabled()), r::RO::Created));
            CHECK(r::is(r::setTask(s.userB, reenabled, r::localTime(0, at)), r::RO::Created));
            // Saved after the instant: this period is not eligible.
            const auto saved = directTask(s, "daily");
            CHECK(r::is(r::setTask(s.userB, saved, r::localTime(0, minuteBack(2))), r::RO::Created));
            const auto refire = directTask(s, "daily");
            localRule(s, refire, 0, at, 1, "2 hours");
            CHECK_EQ(tickCreated(), 2);
            CHECK(firedOnce(same));
            CHECK(firedOnce(refire));
            for (const auto &id : {disabled, reenabled, saved}) CHECK(nothing(id));
            // Each stays due in its own next period, unless disabled.
            const auto nextDay = d::day(obs(), 1) + " " + minuteNow() + ":30+08";
            CHECK_EQ(scanFor(nextDay, reenabled).size(), 1u);
            CHECK_EQ(scanFor(nextDay, saved).size(), 1u);
            CHECK_EQ(scanFor(nextDay, disabled).size(), 0u);
            // No refire: a changed time, already effective and passed, finds the fired identity taken.
            localRule(s, refire, 0, minuteBack(1), 2, "1 hour");
            CHECK_EQ(scanFor("", refire).size(), 0u);
            CHECK_EQ(tickCreated(), 0);
            CHECK(firedOnce(refire));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-rules", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: anniversaries: cosmetic edits fire; schedule changes do not; offsets and members are separate") {
        try {
            const auto s = space();
            // Cosmetic revision after the instant.
            const auto cosmetic = dueAnniversary(s, s.userB, 1);
            a::edit(s.userA, cosmetic.id, 1, a::terms("renamed", cosmetic.base, "anniversary", "note"));
            CHECK_EQ(a::count(obs(), "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE anniversary_id = $1::uuid", cosmetic.id), 2);
            // Date change after the instant, away and back, and kind change.
            const auto moved = dueAnniversary(s, s.userB, 1);
            a::edit(s.userA, moved.id, 1, a::terms("moved", baseFor(3), "anniversary"));
            const auto awayBack = dueAnniversary(s, s.userB, 1);
            a::edit(s.userA, awayBack.id, 1, a::terms("away", baseFor(3), "anniversary"));
            a::edit(s.userA, awayBack.id, 2, a::terms("back", awayBack.base, "anniversary"));
            const auto kind = dueAnniversary(s, s.userB, 1);
            a::edit(s.userA, kind.id, 1, a::terms("kind", d::day(obs(), 1), "countdown"));
            CHECK_EQ(a::count(obs(), "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE anniversary_id = $1::uuid", kind.id), 2);
            // Rule writes: adding an offset leaves the due one; removing it stops it; identical re-save; disable and re-enable.
            const auto at = minuteNow();
            const auto added = directAnniversary(s, baseFor(1), "anniversary", ago("3 days"));
            offsetRule(s, s.userB, added.id, "{1}", at, ago("1 day"));
            CHECK(r::is(r::setAnniversary(s.userB, added.id, r::offsets({1, 7}, at)), r::RO::Created));
            const auto removed = directAnniversary(s, baseFor(1), "anniversary", ago("3 days"));
            offsetRule(s, s.userB, removed.id, "{1,3}", at, ago("1 day"));
            CHECK(r::is(r::setAnniversary(s.userB, removed.id, r::offsets({3}, at)), r::RO::Created));
            const auto resaved = directAnniversary(s, baseFor(1), "anniversary", ago("3 days"));
            offsetRule(s, s.userB, resaved.id, "{1}", at, ago("1 day"));
            CHECK(r::is(r::setAnniversary(s.userB, resaved.id, r::offsets({1}, at)), r::RO::Created));
            const auto toggled = directAnniversary(s, baseFor(1), "anniversary", ago("3 days"));
            offsetRule(s, s.userB, toggled.id, "{1}", at, ago("1 day"));
            CHECK(r::is(r::setAnniversary(s.userB, toggled.id, r::disabled()), r::RO::Created));
            CHECK(r::is(r::setAnniversary(s.userB, toggled.id, r::offsets({1}, at)), r::RO::Created));
            // A countdown, and both members' own rules on one item.
            const auto countdown = dueAnniversary(s, s.userB, 3, "", "countdown");
            const auto shared = dueAnniversary(s, s.userB, 7);
            offsetRule(s, s.userA, shared.id, "{7}", at, ago("1 day"));
            // A rule cannot fire from another space's member: refused at the foreign key.
            const auto other = space();
            auto refused = c::acquireTx(pool);
            CHECK_FALSE(c::exec(refused,
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, anniversary_id, enabled, mode, days_before_set, at_time, idempotency_key, "
                "request_fingerprint) VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'anniversary_offsets', '{7}', '09:00', $4, repeat('a', 64))",
                s.couple, other.userA, shared.id, c::newUuid()).ok);
            c::rollbackTx(refused);

            CHECK_EQ(tickCreated(), 6);
            for (const auto &item : {cosmetic.id, added.id, resaved.id, countdown.id}) CHECK(firedOnce(item));
            for (const auto &item : {moved.id, awayBack.id, kind.id, removed.id, toggled.id}) CHECK(nothing(item));
            CHECK_EQ(occurrences(shared.id), 2);
            CHECK_EQ(noticesFor(shared.id), 2);
            for (const auto &member : {s.userA, s.userB}) {
                CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM notifications WHERE recipient_id = $1::uuid AND target_id = $2::uuid", member, shared.id), 1);
            }
            const auto foreign = n::list(other.userA);
            if (foreign) CHECK_EQ(foreign->body["notifications"].size(), 0u);
            // Version facts: the cosmetic edit is the latest version of a run starting at version 1.
            const auto versions = obs().client->execSqlSync(
                "SELECT anniversary_version AS l, schedule_version AS f, target_date::text AS t, days_before AS o FROM reminder_occurrences WHERE anniversary_id = $1::uuid",
                cosmetic.id);
            CHECK_EQ(versions[0]["l"].as<int>(), 2);
            CHECK_EQ(versions[0]["f"].as<int>(), 1);
            CHECK_EQ(versions[0]["t"].as<std::string>(), d::day(obs(), 1));
            CHECK_EQ(versions[0]["o"].as<int>(), 1);
            // A retry creates nothing.
            CHECK_EQ(tickCreated(), 0);
            CHECK(firedOnce(cosmetic.id));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-anniversaries", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: the scan's calendar: leap days, year boundaries, periods and midnight") {
        try {
            const auto s = space();
            const std::string old = "2019-01-01 00:00:00+08";
            const auto rule = [&](const std::string &base, const std::string &kind, const std::string &set) {
                const auto item = directAnniversary(s, base, kind, old);
                offsetRule(s, s.userB, item.id, set, "09:00", old);
                return item.id;
            };
            const auto leap = rule("2020-02-29", "anniversary", "{1}");
            // A non-leap year's Feb 28 is the target; a leap year's Feb 28 is not.
            CHECK_EQ(scanFor("2021-02-27 09:30:00+08", leap).size(), 1u);
            if (const auto hit = scanFor("2021-02-27 09:30:00+08", leap); hit.size() == 1u) {
                CHECK_EQ(hit[0].targetDate, std::string("2021-02-28"));
                CHECK_EQ(hit[0].daysBefore, 1);
            }
            CHECK_EQ(scanFor("2024-02-27 09:30:00+08", leap).size(), 0u);
            CHECK_EQ(scanFor("2024-02-28 09:30:00+08", leap).size(), 1u);
            if (const auto hit = scanFor("2024-02-28 09:30:00+08", leap); hit.size() == 1u) CHECK_EQ(hit[0].targetDate, std::string("2024-02-29"));
            // Offsets crossing December 31, and March 1 reached from February 29.
            const auto newYear = rule("2020-01-05", "anniversary", "{14}");
            if (const auto hit = scanFor("2023-12-22 09:30:00+08", newYear); hit.size() == 1u) {
                CHECK_EQ(hit[0].targetDate, std::string("2024-01-05"));
            } else {
                c::expectThat(false, "reminder-calendar", "year boundary candidate missing");
            }
            const auto march = rule("2020-03-01", "anniversary", "{1}");
            CHECK_EQ(scanFor("2024-02-29 09:30:00+08", march).size(), 1u);
            CHECK_EQ(scanFor("2023-02-28 09:30:00+08", march).size(), 1u);
            // Before the time and after the day: nothing. A rule never fires for a date before it existed.
            CHECK_EQ(scanFor("2021-02-27 08:59:00+08", leap).size(), 0u);
            CHECK_EQ(scanFor("2021-02-28 00:00:00+08", leap).size(), 0u);
            CHECK_EQ(scanFor("2018-02-27 09:30:00+08", leap).size(), 0u);
            // A countdown fires once; an offset's earlier-year target before the event does not exist.
            const auto once = rule("2022-03-10", "countdown", "{1}");
            CHECK_EQ(scanFor("2022-03-09 09:30:00+08", once).size(), 1u);
            CHECK_EQ(scanFor("2023-03-09 09:30:00+08", once).size(), 0u);
            CHECK_EQ(scanFor("2021-03-09 09:30:00+08", once).size(), 0u);

            // Daily "30 minutes before": late inside the window, never after midnight, the next day independent.
            const auto backdate = [&](const std::string &schedule) {
                const auto task = c::newUuid();
                pool->execSqlSync(
                    "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, "
                    "create_fingerprint, completion_mode, target_count, schedule_type, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, "
                    "'cal', 1, 1, $5, repeat('b', 64), 'count', 2, $6, $7::timestamptz)", task, s.couple, s.userA, s.userB, c::newUuid(), schedule, old);
                return task;
            };
            const auto beforeRule = [&](const std::string &task, int minutes) {
                pool->execSqlSync(
                    "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, idempotency_key, request_fingerprint, created_at) "
                    "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'before_deadline', $4::integer, $5, repeat('a', 64), $6::timestamptz)",
                    s.couple, s.userB, task, std::to_string(minutes), c::newUuid(), old);
            };
            const auto daily = backdate("daily");
            beforeRule(daily, 30);
            CHECK_EQ(scanFor("2024-06-10 23:29:00+08", daily).size(), 0u);
            if (const auto hit = scanFor("2024-06-10 23:45:00+08", daily); hit.size() == 1u) {
                CHECK_EQ(hit[0].periodDate, std::string("2024-06-10"));
            } else {
                c::expectThat(false, "reminder-calendar", "late daily candidate missing");
            }
            CHECK_EQ(scanFor("2024-06-11 00:00:00+08", daily).size(), 0u);
            // Weekly at the widest bound: Monday 00:00 of its own week, and the week before has none.
            const auto weekly = backdate("weekly");
            beforeRule(weekly, 10080);
            if (const auto hit = scanFor("2024-06-10 00:00:00+08", weekly); hit.size() == 1u) {
                CHECK_EQ(hit[0].periodDate, std::string("2024-06-10"));
            } else {
                c::expectThat(false, "reminder-calendar", "weekly candidate missing");
            }
            CHECK_EQ(scanFor("2024-06-09 23:59:00+08", weekly).size(), 0u);

            // A real tick whose scan ran before midnight: its candidate is dropped, not moved to today's period.
            drain();
            const auto spanning = directTask(s, "daily");
            localRule(s, spanning, 0, "23:30", 1, "36 hours");
            const auto yesterday = d::day(obs(), -1);
            auto span = onPolicy();
            span.scanAtForTest = yesterday + " 23:45:00+08";
            CHECK_EQ(scanFor(span.scanAtForTest, spanning).size(), 1u);
            usePolicy(span);
            const auto spanned = tick();
            usePolicy(onPolicy());
            CHECK(completed(spanned));
            CHECK(counted(lastDetail(), "scanned") >= 1);
            CHECK_EQ(counted(lastDetail(), "created"), 0);
            CHECK(nothing(spanning));
            CHECK_EQ(d::periods(obs(), spanning), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-calendar", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: concurrent ticks and retries: one occurrence and one notification per identity") {
        try {
            drain();
            const auto s = space();
            const auto task = dueOneOff(s);
            // The second scheduler tick finds the job lock taken, while the first waits on the target.
            auto holder = hold(33004, task);
            auto first = startTick();
            CHECK(tc::waiters(obs(), holder.pid, 1));
            // Read as soon as reminder.generate reports: the rest of this tick needs the pool's connection the first holds.
            auto skipped = std::make_shared<ts::Pending<Scheduler::TickOutcome>>();
            auto chain = std::make_shared<ts::Pending<bool>>();
            Scheduler::instance().tickOnce(
                [skipped](const Scheduler::TickReport &report) {
                    if (report.jobKey == usapp::scheduler::ReminderJob::kJobKey) skipped->deliver(report.outcome);
                },
                [chain]() { chain->deliver(true); });
            const auto second = skipped->wait();
            CHECK(second.has_value() && *second == Scheduler::TickOutcome::Skipped);
            release(holder, "reminder-ticks");
            CHECK(completed(reminderReport(first->wait())));
            CHECK(chain->wait().has_value());
            CHECK(firedOnce(task));
            CHECK_EQ(tickCreated(), 0);
            // Without the job lock (say a mismatched key across instances): the domain lock and a fresh snapshot still give one.
            const auto race = dueOneOff(s);
            auto gate = hold(33004, race);
            auto one = startDirect();
            CHECK(tc::waiters(obs(), gate.pid, 1));
            auto two = startDirect();
            CHECK(tc::waiters(obs(), gate.pid, 2));
            release(gate, "reminder-direct");
            const auto oneReport = one->wait();
            const auto twoReport = two->wait();
            CHECK(oneReport && oneReport->ok);
            CHECK(twoReport && twoReport->ok);
            if (oneReport && twoReport) {
                CHECK_EQ(oneReport->created + twoReport->created, 1u);
                CHECK_EQ(oneReport->scanned + twoReport->scanned, 2u);
            }
            CHECK(firedOnce(race));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-concurrency", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: a failure after insertion rolls back occurrence and notification; a stuck holder bounds the wait") {
        try {
            drain();
            const auto s = space();
            const auto task = dueOneOff(s);
            usePolicy(gen::Policy{});
            env.fault->armed = true;
            const auto reports = startTick()->wait();
            env.fault->armed = false;
            usePolicy(onPolicy());
            CHECK(reports.has_value());
            bool forced = false;
            if (reports) {
                for (const auto &report : *reports) {
                    if (report.jobKey == env.fault->jobKey()) {
                        forced = report.outcome == Scheduler::TickOutcome::Failed && report.detail == "forced_after_insert";
                    }
                }
            }
            CHECK(forced);
            CHECK(nothing(task));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM notifications WHERE target_id = $1::uuid", task), 0);
            // Nothing partial blocks the retry.
            CHECK_EQ(tickCreated(), 1);
            CHECK(firedOnce(task));

            // Lock order: while 33017 is held, the tick holds the target's domain lock and waits only for 33017.
            const auto ordered = dueOneOff(s);
            auto rule = hold(33017, s.userB + ":" + ordered);
            auto waiting = startTick();
            CHECK(tc::waiters(obs(), rule.pid, 1));
            CHECK_EQ(c::scalar(obs(),
                "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33004 AND granted "
                "AND objid::bigint = (hashtext($1::text)::bigint & 4294967295)", ordered), 1);
            release(rule, "reminder-order");
            CHECK(completed(reminderReport(waiting->wait())));
            CHECK(firedOnce(ordered));

            // A stuck holder fails the tick with a fixed label; nothing commits, and the next tick succeeds.
            const auto stuck = dueOneOff(s);
            auto quick = onPolicy();
            quick.lockTimeoutMs = 150;
            usePolicy(quick);
            auto blocker = hold(33004, stuck);
            const auto timedOut = tick();
            usePolicy(onPolicy());
            CHECK(timedOut.outcome == Scheduler::TickOutcome::Failed);
            CHECK_EQ(timedOut.detail, std::string("reminder_generate stage=lock"));
            CHECK(nothing(stuck));
            release(blocker, "reminder-timeout");
            CHECK_EQ(tickCreated(), 1);
            CHECK(firedOnce(stuck));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-rollback", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: races in both orders with real completion, rule and anniversary writes") {
        try {
            drain();
            const auto s = space();
            const auto completeWrite = [&](const std::string &task) -> StartWrite {
                return [&s, task]() -> std::function<void()> {
                    auto pending = c::startComplete(s.userB, task, c::newUuid());
                    return [pending]() { CHECK(pending->wait().has_value()); };
                };
            };
            const auto disableWrite = [&](const std::string &task) -> StartWrite {
                return [&s, task]() -> std::function<void()> {
                    auto pending = r::start(s.userB, r::Kind::Task, task, r::disabled(), c::newUuid());
                    return [pending]() { CHECK(r::is(pending->wait(), r::RO::Created)); };
                };
            };
            const auto dateWrite = [&](const std::string &id) -> StartWrite {
                return [&s, id]() -> std::function<void()> {
                    auto pending = std::make_shared<a::PendingStatus>();
                    a::AS::instance().edit(s.userA, id, c::newUuid(), 1, a::terms("moved", baseFor(3), "anniversary"),
                                           [pending](a::AS::Status status) { pending->deliver(std::move(status)); });
                    return [pending]() { CHECK(pending->wait().has_value()); };
                };
            };
            const auto completions = [](const std::string &task) {
                return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_completions WHERE task_id = $1::uuid", task);
            };

            // Write first: the tick scanned before the write, and re-evaluation after the lock sees its commit.
            const auto t1 = dueOneOff(s);
            writerThenTick(33004, t1, completeWrite(t1));
            CHECK_EQ(completions(t1), 1);
            CHECK(nothing(t1));
            const auto t2 = dueOneOff(s);
            writerThenTick(33004, t2, disableWrite(t2));
            CHECK(nothing(t2));
            const auto i1 = dueAnniversary(s, s.userB, 1);
            writerThenTick(33010, i1.id, dateWrite(i1.id));
            CHECK_EQ(a::count(obs(), "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE anniversary_id = $1::uuid", i1.id), 2);
            CHECK(nothing(i1.id));

            // Tick first: the reminder is the earlier fact; the write then succeeds, and neither deadlocks.
            const auto t3 = dueOneOff(s);
            tickThenWriter(33004, t3, completeWrite(t3));
            CHECK(firedOnce(t3));
            CHECK_EQ(completions(t3), 1);
            const auto t4 = dueOneOff(s);
            tickThenWriter(33004, t4, disableWrite(t4));
            CHECK(firedOnce(t4));
            CHECK_EQ(r::rules(obs(), t4), 2);
            const auto i2 = dueAnniversary(s, s.userB, 1);
            tickThenWriter(33010, i2.id, dateWrite(i2.id));
            CHECK(firedOnce(i2.id));
            CHECK_EQ(a::count(obs(), "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE anniversary_id = $1::uuid", i2.id), 2);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R2: 250 due drain as 100, 100, 50, 0 in total order; ineligible and fired rows take no slot") {
        try {
            drain();
            const auto s = space();
            const auto date = today();
            const auto at = minuteBack(2);
            // 150 one-offs for one member, 100 anniversaries for the other (rule sets of two offsets, one matching).
            pool->execSqlSync(
                "INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, create_fingerprint, created_at) "
                "SELECT $1::uuid, $2::uuid, $3::uuid, 'bulk', 1, 1, gen_random_uuid()::text, repeat('b', 64), clock_timestamp() - interval '2 days' FROM generate_series(1, 150)",
                s.couple, s.userA, s.userB);
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, at_local_date, at_time, idempotency_key, request_fingerprint, created_at) "
                "SELECT t.couple_id, t.assignee_id, 1, t.id, true, 'absolute', $2::date, $3::time, gen_random_uuid()::text, repeat('a', 64), clock_timestamp() - interval '1 day' "
                "FROM tasks t WHERE t.couple_id = $1::uuid AND t.title = 'bulk'", s.couple, date, at);
            auto seed = c::acquireTx(pool);
            const auto madeItems = c::exec(seed,
                "INSERT INTO anniversaries (couple_id, creator_id, created_at) SELECT $1::uuid, $2::uuid, clock_timestamp() - interval '3 days' FROM generate_series(1, 100)",
                s.couple, s.userA);
            const auto madeRevisions = c::exec(seed,
                "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind, note, idempotency_key, request_fingerprint, created_at) "
                "SELECT a.id, a.couple_id, 1, a.creator_id, 'create', 'bulk', "
                "  CASE WHEN row_number() OVER (ORDER BY a.id) % 2 = 0 THEN $2::date ELSE $3::date END, 'anniversary', '', gen_random_uuid()::text, repeat('a', 64), a.created_at "
                "FROM anniversaries a WHERE a.couple_id = $1::uuid", s.couple, baseFor(0), baseFor(1));
            CHECK(madeItems.ok);
            CHECK(madeRevisions.ok);
            c::commitTx(seed);
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, anniversary_id, enabled, mode, days_before_set, at_time, idempotency_key, request_fingerprint, created_at) "
                "SELECT a.couple_id, a.creator_id, 1, a.id, true, 'anniversary_offsets', '{0,1}', $2::time, gen_random_uuid()::text, repeat('a', 64), clock_timestamp() - interval '1 day' "
                "FROM anniversaries a WHERE a.couple_id = $1::uuid", s.couple, at);

            // The expected order, read from the scan and checked against the documented keys.
            auto all = scanAt("");
            std::vector<std::string> expected;
            for (const auto &one : all) expected.push_back(one.identity());
            if (expected.size() != 250u) throw std::runtime_error("unexpected candidate count");
            const auto sorted = std::is_sorted(all.begin(), all.end(), [](const Candidate &x, const Candidate &y) {
                const auto rank = [](const Candidate &v) { return v.kind == "task" ? 0 : 1; };
                if (x.ownerId != y.ownerId) return x.ownerId < y.ownerId;
                if (rank(x) != rank(y)) return rank(x) < rank(y);
                return x.targetId < y.targetId;
            });
            CHECK(sorted);

            const auto selected = [&]() {
                std::set<std::string> out;
                const auto rows = obs().client->execSqlSync(
                    "SELECT CASE o.event_kind WHEN 'reminder.task' THEN 'task' ELSE 'anniversary' END || '|' || COALESCE(o.task_id, o.anniversary_id)::text || '|' || "
                    "o.owner_id::text || '|' || COALESCE(o.period_date::text, '') || '|' || COALESCE(o.target_date::text, '') || '|' || COALESCE(o.days_before, -1)::text AS v "
                    "FROM reminder_occurrences o WHERE o.couple_id = $1::uuid", s.couple);
                for (const auto &row : rows) out.insert(row["v"].as<std::string>());
                return out;
            };
            // One target held in the first batch: a real write queued behind the tick measures what a user waits.
            const auto first = all.front();
            const bool firstIsTask = first.kind == "task";
            auto contended = hold(firstIsTask ? 33004 : 33010, first.targetId);
            auto firstTick = startTick();
            CHECK(tc::waiters(obs(), contended.pid, 1));
            std::function<void()> queuedWrite;
            if (firstIsTask) {
                auto pending = c::startComplete(first.ownerId, first.targetId, c::newUuid());
                queuedWrite = [pending]() { CHECK(pending->wait().has_value()); };
            } else {
                auto pending = r::start(first.ownerId, r::Kind::Anniversary, first.targetId, r::offsets({0, 1}, at), c::newUuid());
                queuedWrite = [pending]() { CHECK(r::is(pending->wait(), r::RO::Created)); };
            }
            CHECK(tc::waiters(obs(), contended.pid, 2));
            const auto released = std::chrono::steady_clock::now();
            release(contended, "reminder-bulk");
            queuedWrite();
            const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - released).count();
            CHECK(completed(reminderReport(firstTick->wait())));
            const auto heldFirst = counted(lastDetail(), "lock_held_ms");
            CHECK_EQ(counted(lastDetail(), "created"), 100);
            std::set<std::string> want(expected.begin(), expected.begin() + 100);
            CHECK(selected() == want);

            const auto started = std::chrono::steady_clock::now();
            CHECK_EQ(tickCreated(), 100);
            const auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            const auto heldSecond = counted(lastDetail(), "lock_held_ms");
            want.insert(expected.begin() + 100, expected.begin() + 200);
            CHECK(selected() == want);
            CHECK_EQ(tickCreated(), 50);
            want.insert(expected.begin() + 200, expected.end());
            CHECK(selected() == want);
            CHECK_EQ(tickCreated(), 0);
            CHECK_EQ(counted(lastDetail(), "scanned"), 0);
            CHECK(selected() == want);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid AND reminder_occurrence_id IS NOT NULL", s.couple), 250);
            CHECK_EQ(pushRowsFor(s.userB), 0);
            std::printf("[ INFO ] reminder measure: 100-row tick wall %lld ms, lock_held %lld ms; first batch lock_held %lld ms including the holder; "
                        "a write queued behind it returned %lld ms after release\n",
                        static_cast<long long>(tickMs), static_cast<long long>(heldSecond), static_cast<long long>(heldFirst),
                        static_cast<long long>(waitedMs));

            // 100 ineligible, plus the 250 fired above, cannot block one eligible candidate.
            pool->execSqlSync(
                "INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, create_fingerprint, "
                "completion_mode, target_count, schedule_type, created_at) SELECT $1::uuid, $2::uuid, $3::uuid, 'idle', 1, 1, gen_random_uuid()::text, repeat('b', 64), "
                "'count', 2, 'daily', clock_timestamp() - interval '2 days' FROM generate_series(1, 100)", s.couple, s.userA, s.userB);
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, days_before, at_time, idempotency_key, request_fingerprint, created_at) "
                "SELECT t.couple_id, t.assignee_id, 1, t.id, true, 'local_time', 0, $2::time, gen_random_uuid()::text, repeat('a', 64), clock_timestamp() - interval '1 day' "
                "FROM tasks t WHERE t.couple_id = $1::uuid AND t.title = 'idle'", s.couple, at);
            // Half disabled; half with a rule saved after their instant, so neither is eligible.
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, idempotency_key, request_fingerprint) "
                "SELECT t.couple_id, t.assignee_id, 2, t.id, false, gen_random_uuid()::text, repeat('a', 64) FROM ("
                "  SELECT x.*, row_number() OVER (ORDER BY x.id) AS rn FROM tasks x WHERE x.couple_id = $1::uuid AND x.title = 'idle') t WHERE t.rn % 2 = 0", s.couple);
            pool->execSqlSync(
                "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, days_before, at_time, idempotency_key, request_fingerprint) "
                "SELECT t.couple_id, t.assignee_id, 2, t.id, true, 'local_time', 0, $2::time, gen_random_uuid()::text, repeat('a', 64) FROM ("
                "  SELECT x.*, row_number() OVER (ORDER BY x.id) AS rn FROM tasks x WHERE x.couple_id = $1::uuid AND x.title = 'idle') t WHERE t.rn % 2 = 1", s.couple, minuteBack(1));
            CHECK_EQ(scanAt("").size(), 0u);
            const auto eligible = dueOneOff(s);
            CHECK_EQ(scanAt("").size(), 1u);
            CHECK_EQ(tickCreated(), 1);
            CHECK_EQ(counted(lastDetail(), "scanned"), 1);
            CHECK(firedOnce(eligible));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-drain", "case failed (details withheld)");
        }
    }
}

// USAPP_TEST_EXPECT_NO_0026 / _0027: the database lacks that migration. Before the migration gate, this is the
// whole run: a due rule exists, and an enabled job writes nothing and fails with a fixed label.
inline void runMissingMigration(const c::ClientPtr &pool, const std::string &dsn, const std::string &missing, const std::string &present) {
    env.pool = pool;
    env.dsn = dsn;
    env.observer = c::Observer::connect(dsn);
    usapp::AppConfig::instance().schedulerKeyNamespace = "usapp.test.p26r2";
    Scheduler::instance().configure(false, 15000, "test-a");
    env.job = std::make_shared<usapp::scheduler::ReminderJob>(std::make_shared<gen::Generator>(gen::Policy{}));
    Scheduler::instance().registerJob(env.job);

    const std::string label = "reminder_generate stage=preflight_missing_" + missing.substr(0, 4);
    TEST_CASE("P2-6 R2: without a required migration the job fails closed; off still writes nothing") {
        try {
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM schema_migrations WHERE version = $1", missing), 0);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM schema_migrations WHERE version = $1", present), 1);
            const auto s = space();
            const auto task = dueOneOff(s);
            const auto totals = []() {
                return obs().client->execSqlSync("SELECT (SELECT count(*) FROM reminder_occurrences) || '/' || (SELECT count(*) FROM notifications) AS v")[0]["v"].as<std::string>();
            };
            const auto before = totals();
            CHECK(completed(tick()));
            CHECK_EQ(lastDetail(), std::string("reminders dispatch=off"));
            usePolicy(onPolicy());
            const auto refused = tick();
            CHECK(refused.outcome == Scheduler::TickOutcome::Failed);
            CHECK_EQ(refused.detail, label);
            CHECK_EQ(totals(), before);
            CHECK(nothing(task));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM scheduler_ticks WHERE job_key = 'reminder.generate' AND outcome = 'failed' "
                                      "AND detail->>'message' = $1", label), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-missing-0026", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::reminderjob
