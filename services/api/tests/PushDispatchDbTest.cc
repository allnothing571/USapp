// =====================================================================
// services/api/tests/PushDispatchDbTest.cc
//
// P2-5b: real scheduler + real PushDispatch + an ASYNCHRONOUS FAKE transport on literal usapp_test.
// P2-5c adds the real WorkerTransport against a stub-wired worker on the isolated network when
// USAPP_TEST_WORKER_URL is set; no push service is ever involved. The application work runs as usapp_api
// (USAPP_TEST_DSN); owner control operations run on a separate maintenance connection
// (USAPP_TEST_MAINT_CONNINFO), the only way to enable. Either missing: exit 77 (Skipped).
// The connection strings are never printed. Both connections must report usapp_test.
// Isolated committed fixtures (@m3-fixture.test); assertions target this run's ids only. Each
// case starts from a fresh enable, so earlier work is before its cutover. Ends disabled.
// Short leases, backoffs and TTLs are Policy values, not production fault switches; the sleeps
// run on the test thread only, never on an event-loop or ORM thread.
// =====================================================================
#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "TaskCompletionDbTests.h"
#include "PushDbTests.h"
#include "PushWorkerUnitTests.h"
#include "TestDatabaseGuard.h"
#include "TestSupport.h"
#include "config/AppConfig.h"
#include "core/Crypto.h"
#include "db/Db.h"
#include "scheduler/LockKey.h"
#include "scheduler/Scheduler.h"
#include "scheduler/jobs/PushDispatchJob.h"
#include "services/MessageService.h"
#include "services/NotificationIntent.h"
#include "services/PushDispatch.h"
#include "services/PushWorkerTransport.h"

namespace usapptest::pushdispatch {

namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace p = usapptest::push;
namespace d = usapp::push;
using usapp::MessageService;
using usapp::scheduler::Scheduler;
using ClientPtr = c::ClientPtr;

// ---------------------------------------------------------------- the fake
class FakeTransport : public d::Transport {
  public:
    explicit FakeTransport(ClientPtr observer) : observer_(std::move(observer)) { loop_.run(); }

    bool manual = false;  // hold callbacks until the test completes them (or never: a crash)
    double latency = 0.005;
    std::function<d::SendResult(const d::SendRequest &)> script;

    void send(const d::SendRequest &request, std::function<void(d::SendResult)> done) override {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            requests_.push_back(request);
            if (manual) held_.push_back({request, done});
        }
        // Another connection must already see this attempt: the claim transaction has committed.
        observer_->execSqlAsync(
            "SELECT count(*)::bigint AS n FROM push_attempts WHERE notification_id = $1::uuid "
            "AND generation_id = $2::uuid AND attempt_no = $3::integer",
            [this](const drogon::orm::Result &rows) {
                (rows[0]["n"].as<long long>() == 1 ? visible_ : invisible_).fetch_add(1);
            },
            [this](const drogon::orm::DrogonDbException &) { invisible_.fetch_add(1); },
            request.key.notificationId, request.key.generationId, std::to_string(request.key.attemptNo));
        if (manual) return;
        const d::SendResult result = script ? script(request) : d::SendResult{"accepted", 201, std::nullopt};
        loop_.getLoop()->runAfter(latency, [done, result]() { done(result); });
    }

    size_t sends() {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_.size();
    }
    std::vector<d::SendRequest> requests() {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_;
    }
    void completeHeld(const d::SendResult &result) {
        std::vector<Held> held;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            held.swap(held_);
        }
        for (auto &h : held) h.done(result);
    }
    long long visibleAtSend() const { return visible_.load(); }
    long long invisibleAtSend() const { return invisible_.load(); }

  private:
    struct Held {
        d::SendRequest request;
        std::function<void(d::SendResult)> done;
    };
    ClientPtr observer_;
    trantor::EventLoopThread loop_;
    std::mutex mutex_;
    std::vector<d::SendRequest> requests_;
    std::vector<Held> held_;
    std::atomic<long long> visible_{0};
    std::atomic<long long> invisible_{0};
};

// ---------------------------------------------------------------- environment
struct Env {
    ClientPtr pool;
    ClientPtr maint;
    std::optional<c::Observer> observer;
    std::shared_ptr<usapp::scheduler::PushDispatchJob> job;
    std::shared_ptr<d::Dispatcher> current;
};
inline Env env;

inline d::Policy fastPolicy() {
    d::Policy policy;
    policy.localEnabled = true;
    policy.vapidKeyId = p::vapid();
    policy.markerSecret = p::kSecret;
    policy.instanceId = "test-a";
    policy.leaseSeconds = 30.0;
    policy.backoffSeconds = {0.0, 0.0, 0.0, 0.0};
    policy.internalBackoffSeconds = 0.0;
    return policy;
}

inline std::shared_ptr<FakeTransport> newFake() { return std::make_shared<FakeTransport>(env.observer->client); }

inline std::shared_ptr<d::Dispatcher> use(const d::Policy &policy, std::shared_ptr<d::Transport> transport) {
    env.current = std::make_shared<d::Dispatcher>(policy, std::move(transport));
    env.job->setDispatcher(env.current);
    return env.current;
}

inline std::string controlState() {
    return env.maint->execSqlSync("SELECT c.state FROM usapp_push_control_state() c")[0]["state"].as<std::string>();
}
inline std::string cutoverText() {
    return env.maint->execSqlSync("SELECT c.cutover::text AS t FROM usapp_push_control_state() c")[0]["t"].as<std::string>();
}
inline long long controlRevisions() {
    return env.maint->execSqlSync("SELECT count(*)::bigint AS n FROM push_dispatch_control")[0]["n"].as<long long>();
}
// The documented owner operation (Section 13), on this isolated test database only.
inline void owner(const std::string &kind) {
    env.maint->execSqlSync("INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ($1, 'owner', 'p25b_test')",
                           kind);
}

inline void waitUntil(const std::function<bool()> &condition, int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("bounded wait expired");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline d::RecordStatus record(const d::AttemptKey &key, const d::SendResult &result) {
    auto pending = std::make_shared<ts::Pending<d::RecordStatus>>();
    env.current->recordResult(key, result, [pending](d::RecordStatus s) { pending->deliver(std::move(s)); });
    auto out = pending->wait();
    if (!out) throw std::runtime_error("record timed out");
    return *out;
}

// A new enable (so a new cutover), with leases of earlier cases released.
inline void freshEnable() {
    if (controlState() != "disabled") owner("disable");
    const auto rows = env.observer->client->execSqlSync(
        "SELECT a.notification_id::text AS n, a.generation_id::text AS g, a.attempt_no AS k FROM push_attempts a "
        "WHERE a.lease_until > clock_timestamp() AND NOT EXISTS (SELECT 1 FROM push_attempt_observations o "
        "WHERE o.notification_id = a.notification_id AND o.generation_id = a.generation_id AND o.attempt_no = a.attempt_no) "
        "AND NOT EXISTS (SELECT 1 FROM push_attempt_expiries x WHERE x.notification_id = a.notification_id "
        "AND x.generation_id = a.generation_id AND x.attempt_no = a.attempt_no)");
    for (const auto &row : rows) {
        record({row["n"].as<std::string>(), row["g"].as<std::string>(), row["k"].as<int>()},
               {"transport_unavailable", std::nullopt, std::nullopt});
    }
    owner("enable");
}

inline std::vector<Scheduler::TickReport> tick() {
    auto reports = std::make_shared<std::vector<Scheduler::TickReport>>();
    auto done = std::make_shared<ts::Pending<bool>>();
    Scheduler::instance().tickOnce([reports](const Scheduler::TickReport &r) { reports->push_back(r); },
                                   [done]() { done->deliver(true); });
    if (!done->wait()) throw std::runtime_error("tick timed out");
    return *reports;
}
inline bool tickCompleted() {
    const auto reports = tick();
    return reports.size() == 1u && reports[0].outcome == Scheduler::TickOutcome::Completed;
}

// A tick's transaction without the afterCommit: the test decides when (and whether) to send.
inline d::TickReport claimOnly() {
    auto pending = std::make_shared<ts::Pending<d::TickReport>>();
    auto dispatcher = env.current;
    usapp::db::TxScope::begin(
        [pending, dispatcher](std::shared_ptr<usapp::db::TxScope> scope) {
            dispatcher->runTick(scope->tx(), [pending, scope](d::TickReport report) {
                if (!report.ok) {
                    scope->rollback([pending, report]() { pending->deliver(report); });
                    return;
                }
                scope->commit([pending, report]() { pending->deliver(report); });
            });
        },
        [pending](const std::string &) { pending->deliver(d::TickReport{}); });
    auto out = pending->wait();
    if (!out) throw std::runtime_error("claim timed out");
    return *out;
}

// A real optional notice from A to B through the message service; returns the notification id.
inline std::string notice(const std::string &author, const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<ts::Pending<MessageService::Status>>();
    usapp::intent::Choice choice;
    choice.provided = true;
    choice.value = true;
    MessageService::instance().create(author, key, "p25b", [pending](MessageService::Status s) {
        pending->deliver(std::move(s));
    }, choice);
    const auto status = pending->wait();
    if (!status || !status->success() || !status->notification["notification_id"].isString()) {
        throw std::runtime_error("notice not created");
    }
    return status->notification["notification_id"].asString();
}

inline long long count1(const std::string &sql, const std::string &a) {
    return env.observer->client->execSqlSync(sql, a)[0]["n"].as<long long>();
}
inline long long count2(const std::string &sql, const std::string &a, const std::string &b) {
    return env.observer->client->execSqlSync(sql, a, b)[0]["n"].as<long long>();
}
inline long long deliveries(const std::string &n) {
    return count1("SELECT count(*)::bigint AS n FROM push_deliveries WHERE notification_id = $1::uuid", n);
}
inline long long hasDelivery(const std::string &n, const std::string &g) {
    return count2("SELECT count(*)::bigint AS n FROM push_deliveries WHERE notification_id = $1::uuid AND generation_id = $2::uuid", n, g);
}
inline long long attempts(const std::string &n, const std::string &g) {
    return count2("SELECT count(*)::bigint AS n FROM push_attempts WHERE notification_id = $1::uuid AND generation_id = $2::uuid", n, g);
}
inline long long observations(const std::string &n, const std::string &g) {
    return count2("SELECT count(*)::bigint AS n FROM push_attempt_observations WHERE notification_id = $1::uuid AND generation_id = $2::uuid", n, g);
}
inline long long expiries(const std::string &n, const std::string &g) {
    return count2("SELECT count(*)::bigint AS n FROM push_attempt_expiries WHERE notification_id = $1::uuid AND generation_id = $2::uuid", n, g);
}
inline long long used(const std::string &n, const std::string &g) {
    return count2("SELECT usapp_push_provider_attempts($1::uuid, $2::uuid)::bigint AS n", n, g);
}
inline std::string outcome(const std::string &n, const std::string &g) {
    const auto rows = env.observer->client->execSqlSync(
        "SELECT final_state FROM push_delivery_outcomes WHERE notification_id = $1::uuid AND generation_id = $2::uuid", n, g);
    return rows.size() == 1u ? rows[0]["final_state"].as<std::string>() : std::string("none");
}
inline std::string observed(const std::string &n, const std::string &g, int k) {
    const auto rows = env.observer->client->execSqlSync(
        "SELECT outcome FROM push_attempt_observations WHERE notification_id = $1::uuid AND generation_id = $2::uuid "
        "AND attempt_no = $3::integer", n, g, std::to_string(k));
    return rows.size() == 1u ? rows[0]["outcome"].as<std::string>() : std::string("none");
}

struct Pair {
    ts::CommittedSpace space;
    std::string sessionB;
    std::string gen;       // B's device
    p::Device device;
};
inline Pair newPair(const ClientPtr &pool, std::optional<p::Device> device = std::nullopt) {
    Pair out;
    ts::commitSpace(pool, out.space, true);
    out.sessionB = p::commitSession(pool, out.space.userB);
    out.device = device ? *device : p::device();
    const auto registered = p::reg(out.space.userB, out.sessionB, c::newUuid(), out.device.reg);
    if (!p::is(registered, p::PO::Created)) throw std::runtime_error("device not registered");
    out.gen = p::gen(registered);
    return out;
}

// ---------------------------------------------------------------- 0041 fixtures
// The guards refuse a supplied future time, so only maintenance with triggers off can write what a chain leaves behind a clock that moved back.
inline void withTriggersOff(const std::string &body) {
    env.maint->execSqlSync("DO $off$ BEGIN PERFORM set_config('session_replication_role', 'replica', true); " + body + " END $off$");
}
// A notice for B, dated this many seconds ahead of the database clock.
inline std::string aheadNotice(const Pair &pair, double seconds) {
    const auto message = c::newUuid();
    const auto id = c::newUuid();
    const auto &s = pair.space;
    withTriggersOff(
        "INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at) VALUES ('" + message + "', '" + s.couple + "', '" + s.userA + "', '" + s.userB +
        "', 'p0041', '" + c::newUuid() + "', repeat('a', 64), clock_timestamp()); "
        "INSERT INTO notifications (id, couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id, created_at) VALUES ('" + id + "', '" + s.couple + "', '" + s.userA + "', '" + s.userB +
        "', 'message.created', 'message', '" + message + "', '" + message + "', clock_timestamp() + make_interval(secs => " + std::to_string(seconds) + "));");
    return id;
}
inline void aheadDelivery(const Pair &pair, const std::string &n) {
    withTriggersOff("INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id, created_at) SELECT id, '" + pair.gen + "', recipient_id, couple_id, created_at FROM notifications WHERE id = '" + n + "';");
}
// As the clock falling behind a notice leaves it: only the relation between the two matters here.
inline void moveNotice(const std::string &n, double seconds) {
    withTriggersOff("UPDATE notifications SET created_at = clock_timestamp() + make_interval(secs => " + std::to_string(seconds) + ") WHERE id = '" + n + "';");
}
inline bool noticeReached(const std::string &n) {
    return count1("SELECT count(*)::bigint AS n FROM notifications WHERE id = $1::uuid AND created_at <= clock_timestamp()", n) == 1;
}
inline bool expiryReached(const std::string &n, double lifetime) {
    return env.observer->client->execSqlSync("SELECT count(*)::bigint AS n FROM notifications WHERE id = $1::uuid AND created_at + make_interval(secs => $2::double precision) <= clock_timestamp()", n, std::to_string(lifetime))[0]["n"].as<long long>() == 1;
}
inline long long expiryMs(const std::string &n, int lifetime) {
    return env.observer->client->execSqlSync("SELECT floor(extract(epoch FROM created_at + make_interval(secs => $2::double precision)) * 1000)::bigint AS ms FROM notifications WHERE id = $1::uuid", n, std::to_string(lifetime))[0]["ms"].as<long long>();
}

// ---------------------------------------------------------------- P2-6 R5 fixtures
// A reminder for B, committed directly with its notice. The one-off's deadline, and so stale_at, is the given number of
// seconds away, truncated to the second; its 30-minute rule makes the instant already past.
struct Reminder {
    std::string task;
    std::string id;  // the notification
};
inline std::string commitOccurrenceNotice(const Pair &pair, const std::string &occurrenceSql, const std::string &target,
                                          const std::string &eventType, const std::string &targetKind) {
    auto tx = c::acquireTx(env.pool);
    const auto occ = c::exec(tx, occurrenceSql, target);
    if (!occ.ok || !occ.result || occ.result->size() != 1u) throw std::runtime_error("occurrence fixture failed");
    const auto made = c::exec(tx,
        "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, reminder_occurrence_id) "
        "VALUES ($1::uuid, NULL, $2::uuid, $3, $4, $5::uuid, $6::uuid) RETURNING id::text AS id",
        pair.space.couple, pair.space.userB, eventType, targetKind, target, (*occ.result)[0]["id"].as<std::string>());
    if (!made.ok || !made.result || made.result->size() != 1u) throw std::runtime_error("notice fixture failed");
    const std::string id = (*made.result)[0]["id"].as<std::string>();
    if (!c::commitTx(tx).value_or(false)) throw std::runtime_error("reminder fixture did not commit");
    return id;
}
inline Reminder taskReminder(const Pair &pair, int secondsToStale) {
    const auto task = c::newUuid();
    env.pool->execSqlSync(
        "WITH x AS (SELECT date_trunc('second', clock_timestamp() + make_interval(secs => $6::double precision)) AS at) "
        "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, "
        "create_fingerprint, deadline_local_date, deadline_local_time, deadline_at, created_at) "
        "SELECT $1::uuid, $2::uuid, $3::uuid, $4::uuid, 'p26r5', 1, 1, $5, repeat('b', 64), (x.at AT TIME ZONE 'Asia/Shanghai')::date, "
        "(x.at AT TIME ZONE 'Asia/Shanghai')::time, x.at, clock_timestamp() - interval '2 days' FROM x",
        task, pair.space.couple, pair.space.userA, pair.space.userB, c::newUuid(), std::to_string(secondsToStale));
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, idempotency_key, request_fingerprint, created_at) "
        "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'before_deadline', 30, $4, repeat('a', 64), clock_timestamp() - interval '1 day')",
        pair.space.couple, pair.space.userB, task, c::newUuid());
    const auto id = commitOccurrenceNotice(pair,
        "INSERT INTO reminder_occurrences (couple_id, owner_id, rule_id, event_kind, task_id, fire_at, stale_at) "
        "SELECT r.couple_id, r.owner_id, r.id, 'reminder.task', t.id, t.deadline_at - interval '30 minutes', t.deadline_at "
        "FROM tasks t JOIN reminder_rules r ON r.task_id = t.id WHERE t.id = $1::uuid RETURNING id::text AS id",
        task, "reminder.task", "task");
    return {task, id};
}
// A countdown due tomorrow, fired today by its one-day offset, at the minute just begun.
inline Reminder anniversaryReminder(const Pair &pair) {
    const auto item = c::newUuid();
    auto tx = c::acquireTx(env.pool);
    if (!c::exec(tx, "INSERT INTO anniversaries (id, couple_id, creator_id, created_at) VALUES ($1::uuid, $2::uuid, $3::uuid, clock_timestamp() - interval '2 days')",
                 item, pair.space.couple, pair.space.userA).ok ||
        !c::exec(tx, "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind, note, idempotency_key, request_fingerprint, created_at) "
                     "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, 'create', 'p26r5', (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + 1, 'countdown', '', $4, repeat('a', 64), clock_timestamp() - interval '2 days')",
                 item, pair.space.couple, pair.space.userA, c::newUuid()).ok ||
        !c::commitTx(tx).value_or(false)) {
        throw std::runtime_error("anniversary fixture failed");
    }
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, anniversary_id, enabled, mode, days_before_set, at_time, idempotency_key, request_fingerprint, created_at) "
        "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, true, 'anniversary_offsets', '{1}', date_trunc('minute', clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::time, $4, repeat('a', 64), clock_timestamp() - interval '1 day')",
        pair.space.couple, pair.space.userB, item, c::newUuid());
    const auto id = commitOccurrenceNotice(pair,
        "INSERT INTO reminder_occurrences (couple_id, owner_id, rule_id, event_kind, anniversary_id, anniversary_version, schedule_version, target_date, days_before, fire_at, stale_at) "
        "SELECT r.couple_id, r.owner_id, r.id, 'reminder.anniversary', r.anniversary_id, 1, 1, x.d + 1, 1, (x.d + r.at_time) AT TIME ZONE 'Asia/Shanghai', "
        "(x.d + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' FROM reminder_rules r, (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d) x "
        "WHERE r.anniversary_id = $1::uuid RETURNING id::text AS id",
        item, "reminder.anniversary", "anniversary");
    return {item, id};
}
// The next revision of the task's rule: disabled, or the same reminder at another time.
inline void taskRuleOff(const Pair &pair, const std::string &task) {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, idempotency_key, request_fingerprint) "
        "SELECT $1::uuid, $2::uuid, max(r.seq) + 1, $3::uuid, false, $4, repeat('a', 64) FROM reminder_rules r WHERE r.task_id = $3::uuid",
        pair.space.couple, pair.space.userB, task, c::newUuid());
}
inline void taskRuleRetimed(const Pair &pair, const std::string &task) {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, idempotency_key, request_fingerprint) "
        "SELECT $1::uuid, $2::uuid, max(r.seq) + 1, $3::uuid, true, 'before_deadline', 1, $4, repeat('a', 64) FROM reminder_rules r WHERE r.task_id = $3::uuid",
        pair.space.couple, pair.space.userB, task, c::newUuid());
}
inline void anniversaryRuleOff(const Pair &pair, const std::string &item) {
    env.pool->execSqlSync(
        "INSERT INTO reminder_rules (couple_id, owner_id, seq, anniversary_id, enabled, idempotency_key, request_fingerprint) "
        "VALUES ($1::uuid, $2::uuid, 2, $3::uuid, false, $4, repeat('a', 64))",
        pair.space.couple, pair.space.userB, item, c::newUuid());
}
// The next revision, N days from today, with the given title and note.
inline void reviseAnniversary(const Pair &pair, const std::string &item, int days, const std::string &title = "p26r5", const std::string &note = "") {
    env.pool->execSqlSync(
        "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind, note, idempotency_key, request_fingerprint) "
        "SELECT $1::uuid, $2::uuid, max(r.version) + 1, $3::uuid, 'edit', $4, (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + $5::integer, 'countdown', $6, $7, repeat('a', 64) "
        "FROM anniversary_revisions r WHERE r.anniversary_id = $1::uuid",
        item, pair.space.couple, pair.space.userA, title, std::to_string(days), note, c::newUuid());
}
inline std::string inboxRow(const std::string &n) {
    return env.observer->client->execSqlSync("SELECT to_jsonb(x)::text AS v FROM notifications x WHERE x.id = $1::uuid", n)[0]["v"].as<std::string>();
}
inline long long reads(const std::string &n) {
    return count1("SELECT count(*)::bigint AS n FROM notification_reads WHERE notification_id = $1::uuid", n);
}
inline long long staleMs(const std::string &n) {
    return env.observer->client->execSqlSync(
        "SELECT floor(extract(epoch FROM o.stale_at) * 1000)::bigint AS v FROM notifications x JOIN reminder_occurrences o ON o.id = x.reminder_occurrence_id "
        "WHERE x.id = $1::uuid", n)[0]["v"].as<long long>();
}
// Waits on the database clock until this much of the reminder's window is left; negative: past it.
inline void waitForWindow(const std::string &n, double secondsLeft) {
    waitUntil([&]() {
        return env.observer->client->execSqlSync(
            "SELECT extract(epoch FROM o.stale_at - clock_timestamp()) <= $2::double precision AS v FROM notifications x "
            "JOIN reminder_occurrences o ON o.id = x.reminder_occurrence_id WHERE x.id = $1::uuid", n, std::to_string(secondsLeft))[0]["v"].as<bool>();
    }, 8000);
}
inline std::vector<d::AttemptKey> claimsOf(const d::TickReport &report, const std::string &n) {
    std::vector<d::AttemptKey> out;
    for (const auto &key : report.claims) if (key.notificationId == n) out.push_back(key);
    return out;
}
// What a stale skip must have left: no request, no budget, no pause, the device as it was, the inbox as it was.
inline void checkStaleSkip(const std::string &n, const std::string &gen, int attempt, int providerAttempts, size_t sends, FakeTransport &fake,
                           const std::string &inbox, long long revisions) {
    waitUntil([&]() { return outcome(n, gen) == "source_stale"; });
    CHECK_EQ(observed(n, gen, attempt), std::string("source_stale_skipped"));
    CHECK_EQ(fake.sends(), sends);
    CHECK_EQ(used(n, gen), providerAttempts);
    CHECK_EQ(controlState(), std::string("enabled"));
    CHECK_EQ(controlRevisions(), revisions);
    CHECK_EQ(p::revokedReason(*env.observer, gen), std::string("none"));
    CHECK_EQ(inboxRow(n), inbox);
    CHECK_EQ(reads(n), 0);
    // Terminal: names its own attempt, and nothing more is claimed.
    CHECK_EQ(count2("SELECT count(*)::bigint AS n FROM push_delivery_outcomes WHERE notification_id = $1::uuid AND generation_id = $2::uuid "
                    "AND attempt_no IS NOT NULL", n, gen), 1);
    CHECK(tickCompleted());
    CHECK_EQ(attempts(n, gen), attempt);
}

inline void runReminderPush();

inline void run() {
    const ClientPtr &pool = env.pool;

    TEST_CASE("P2-5b: control is owner-only; defaults and mismatches fail closed; startup writes nothing") {
        try {
            // Roles: usapp_api cannot enable, disable or resume, or forge an owner pause.
            if (controlState() != "disabled") owner("disable");
            for (const char *sql : {"INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'x')",
                                    "INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('resume', 'owner', 'x')",
                                    "INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'x')",
                                    "UPDATE push_dispatch_control SET reason_code = reason_code",
                                    "DELETE FROM push_dispatch_control"}) {
                bool denied = false;
                try { pool->execSqlSync(sql); } catch (const std::exception &) { denied = true; }
                CHECK(denied);
            }
            CHECK_EQ(controlState(), std::string("disabled"));

            auto pair = newPair(pool);
            auto fake = newFake();
            const auto before = controlRevisions();
            // Disabled: nothing, even though the local capability is complete.
            use(fastPolicy(), fake);
            const auto n0 = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n0), 0);
            // Building dispatchers and ticking never writes control rows.
            CHECK_EQ(controlRevisions(), before);

            freshEnable();
            const auto n1 = notice(pair.space.userA);
            // Enabled but the local flag is off, or no transport, or no key: no dispatch, no writes.
            auto off = fastPolicy();
            off.localEnabled = false;
            use(off, fake);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n1), 0);
            use(fastPolicy(), nullptr);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n1), 0);
            auto noKey = fastPolicy();
            noKey.vapidKeyId.clear();
            use(noKey, fake);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n1), 0);
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(controlState(), std::string("enabled"));
            // Once complete, the notice inside the window is still found (restart recovery).
            use(fastPolicy(), fake);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(n1, pair.gen), 1);
            waitUntil([&]() { return outcome(n1, pair.gen) == "accepted"; });
            CHECK_EQ(deliveries(n0), 0);  // never backfilled from before the cutover
        } catch (const std::exception &) {
            c::expectThat(false, "push-control", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: fan-out to every device of the recipient only; sends only after commit; replay adds nothing") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            const auto second = p::device("https://fcm.googleapis.com/fcm/send/");
            const auto g2 = p::gen(p::reg(pair.space.userB, pair.sessionB, c::newUuid(), second.reg));
            // The author's own device and another space's device get nothing.
            const auto sa = p::commitSession(pool, pair.space.userA);
            const auto ga = p::gen(p::reg(pair.space.userA, sa, c::newUuid(), p::device().reg));
            auto other = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const std::string key = c::newUuid();
            const auto n = notice(pair.space.userA, key);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n), 2);
            CHECK_EQ(hasDelivery(n, pair.gen), 1);
            CHECK_EQ(hasDelivery(n, g2), 1);
            CHECK_EQ(hasDelivery(n, ga), 0);
            CHECK_EQ(hasDelivery(n, other.gen), 0);
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted" && outcome(n, g2) == "accepted"; });
            CHECK_EQ(fake->sends(), 2u);
            waitUntil([&]() { return fake->visibleAtSend() + fake->invisibleAtSend() == 2; });
            CHECK_EQ(fake->visibleAtSend(), 2);
            CHECK_EQ(fake->invisibleAtSend(), 0);
            for (const auto &r : fake->requests()) {
                CHECK_EQ(r.eventType, std::string("message.created"));
                CHECK_EQ(r.marker, usapp::PushService::markerFor(p::kSecret, r.key.generationId));
            }
            // Provider acceptance is recorded as such and nothing more; a replay creates nothing.
            CHECK_EQ(observed(n, pair.gen, 1), std::string("accepted"));
            CHECK_EQ(notice(pair.space.userA, key), n);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n), 2);
            CHECK_EQ(attempts(n, pair.gen), 1);
            // Business rollback: no notice, so no delivery.
            auto tx = c::acquireTx(pool);
            const std::string m = c::newUuid();
            CHECK(c::exec(tx, "INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, "
                              "request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'x', $5, $6)",
                          m, pair.space.couple, pair.space.userA, pair.space.userB, "p25b-rollback-" + m.substr(0, 8),
                          std::string(64, 'a')).ok);
            const auto rolled = c::exec(tx, "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, "
                                            "target_kind, target_id, message_id) VALUES ($1::uuid, $2::uuid, $3::uuid, "
                                            "'message.created', 'message', $4::uuid, $4::uuid) RETURNING id::text AS id",
                                        pair.space.couple, pair.space.userA, pair.space.userB, m);
            CHECK(rolled.ok);
            const std::string rolledId = (*rolled.result)[0]["id"].as<std::string>();
            c::rollbackTx(tx);
            sleepMs(50);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(rolledId), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "push-fanout", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: insert-time rule: a transaction begun before activation is eligible; late commits are found") {
        try {
            freshEnable();
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sb = p::commitSession(pool, s.userB);
            auto fake = newFake();
            use(fastPolicy(), fake);

            // Block the author's membership statement: the create's transaction is already open.
            auto hold = c::acquireTx(env.maint);
            CHECK(c::exec(hold, "SELECT 1 FROM couple_members WHERE user_id = $1::uuid FOR UPDATE", s.userA).ok);
            auto pending = std::make_shared<ts::Pending<MessageService::Status>>();
            usapp::intent::Choice choice;
            choice.provided = true;
            choice.value = true;
            MessageService::instance().create(s.userA, c::newUuid(), "p25b", [pending](MessageService::Status st) {
                pending->deliver(std::move(st));
            }, choice);
            waitUntil([&]() {
                return env.observer->client->execSqlSync(
                    "SELECT count(*)::bigint AS n FROM pg_stat_activity WHERE wait_event_type = 'Lock' "
                    "AND query LIKE '%couple_members%FOR SHARE%'")[0]["n"].as<long long>() >= 1;
            });
            const auto xactStart = env.observer->client->execSqlSync(
                "SELECT min(xact_start)::text AS t FROM pg_stat_activity WHERE wait_event_type = 'Lock' "
                "AND query LIKE '%couple_members%FOR SHARE%'")[0]["t"].as<std::string>();
            // The recipient's device activates after that transaction began.
            const auto device = p::device();
            const auto g = p::gen(p::reg(s.userB, sb, c::newUuid(), device.reg));
            CHECK_EQ(count2("SELECT count(*)::bigint AS n FROM push_subscriptions WHERE id = $1::uuid "
                            "AND activated_at > $2::timestamptz", g, xactStart), 1);
            c::rollbackTx(hold);
            const auto status = pending->wait();
            CHECK(status && status->success());
            const auto n = status->notification["notification_id"].asString();
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(n, g), 1);  // inserted after activation: eligible
            waitUntil([&]() { return outcome(n, g) == "accepted"; });

            // Late commit: the notice row exists but its transaction waits on the preference lock
            // (the last step before commit). A tick then finds nothing; after the commit it does.
            auto lock = c::acquireTx(env.maint);
            CHECK(c::exec(lock, "SELECT pg_advisory_xact_lock(33012::integer, hashtext($1 || ':' || 'message_create'))", s.userA).ok);
            auto late = std::make_shared<ts::Pending<MessageService::Status>>();
            MessageService::instance().create(s.userA, c::newUuid(), "p25b late", [late](MessageService::Status st) {
                late->deliver(std::move(st));
            }, choice);
            waitUntil([&]() {
                return env.observer->client->execSqlSync(
                    "SELECT count(*)::bigint AS n FROM pg_stat_activity WHERE wait_event_type = 'Lock' "
                    "AND wait_event = 'advisory' AND query LIKE '%33012%'")[0]["n"].as<long long>() >= 1;
            });
            const auto deliveriesBefore = count1("SELECT count(*)::bigint AS n FROM push_deliveries WHERE generation_id = $1::uuid", g);
            CHECK(tickCompleted());
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_deliveries WHERE generation_id = $1::uuid", g), deliveriesBefore);
            c::rollbackTx(lock);
            const auto committed = late->wait();
            CHECK(committed && committed->success());
            const auto lateId = committed->notification["notification_id"].asString();
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(lateId, g), 1);
            waitUntil([&]() { return outcome(lateId, g) == "accepted"; });
        } catch (const std::exception &) {
            c::expectThat(false, "push-insert-time", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: cutover cases: initial, disable then enable, pause then resume, device re-enable, restart") {
        try {
            if (controlState() != "disabled") owner("disable");
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            // Initial cutover: inserted before the enable, never pushed.
            const auto before = notice(pair.space.userA);
            owner("enable");
            const auto cut = cutoverText();
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(before), 0);

            // Pause: fan-out continues, no claims, the cutover stays.
            owner("pause");
            const auto during = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(during, pair.gen), 1);
            CHECK_EQ(attempts(during, pair.gen), 0);
            // Restart recovery: a new dispatcher object after resume finishes the same pending work.
            owner("resume");
            CHECK_EQ(cutoverText(), cut);
            auto fake2 = newFake();
            use(fastPolicy(), fake2);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(during, pair.gen) == "accepted"; });

            // Disable then enable: a new cutover; pending work from before is skipped.
            owner("pause");
            const auto pendingOld = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(pendingOld, pair.gen), 1);
            owner("disable");
            const auto whileOff = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(whileOff), 0);
            owner("enable");
            CHECK(cutoverText() != cut);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(pendingOld, pair.gen), std::string("cutover_skipped"));
            CHECK_EQ(attempts(pendingOld, pair.gen), 0);
            CHECK_EQ(deliveries(whileOff), 0);

            // Device re-enable: the new generation excludes the gap.
            CHECK(p::is(p::revoke(pair.space.userB, pair.sessionB, pair.gen), p::PO::Ok));
            const auto gap = notice(pair.space.userA);
            const auto reenabled = p::reg(pair.space.userB, pair.sessionB, c::newUuid(), pair.device.reg);
            CHECK(p::is(reenabled, p::PO::Created));
            if (!p::is(reenabled, p::PO::Created)) {
                std::printf("PUSH_CUTOVER_DIAG stage=device_reregister outcome=%d\n",
                            reenabled ? static_cast<int>(reenabled->outcome) : -1);
                throw std::runtime_error("device re-enable registration refused");
            }
            const auto g2 = p::gen(reenabled);
            CHECK(!g2.empty());
            if (g2.empty()) throw std::runtime_error("device re-enable generation missing");
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(gap), 0);
            const auto after = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(after, g2), 1);
            CHECK_EQ(hasDelivery(after, pair.gen), 0);
            waitUntil([&]() { return outcome(after, g2) == "accepted"; });
        } catch (const std::exception &) {
            c::expectThat(false, "push-cutover", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: four live leases across ticks and instances; two per recipient per tick; fair order") {
        try {
            freshEnable();
            auto a = newPair(pool), b = newPair(pool), e = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            std::vector<std::string> ids;
            // Oldest first would give A three claims; only the per-recipient cap stops that.
            for (const auto *pair : {&a, &b, &e}) {
                for (int i = 0; i < 3; ++i) ids.push_back(notice(pair->space.userA));
            }
            // Another instance holds the job lock: this tick is skipped and writes nothing.
            auto other = c::acquireTx(env.maint);
            const auto lockKey = usapp::scheduler::LockKey::forJob("push.dispatch").value();
            const auto acquired = c::exec(other, "SELECT pg_try_advisory_xact_lock($1::bigint) AS ok", std::to_string(lockKey));
            CHECK(acquired.ok && (*acquired.result)[0]["ok"].as<bool>());
            const auto skipped = tick();
            CHECK(skipped.size() == 1u && skipped[0].outcome == Scheduler::TickOutcome::Skipped);
            CHECK_EQ(deliveries(ids[0]), 0);
            c::rollbackTx(other);
            waitUntil([&]() {
                return tickCompleted();
            });
            waitUntil([&]() { return fake->sends() == 4u; });
            // A later tick and two concurrent ticks cannot exceed the four live leases.
            auto first = std::make_shared<ts::Pending<bool>>();
            auto second = std::make_shared<ts::Pending<bool>>();
            Scheduler::instance().tickOnce(nullptr, [first]() { first->deliver(true); });
            Scheduler::instance().tickOnce(nullptr, [second]() { second->deliver(true); });
            CHECK(first->wait().has_value());
            CHECK(second->wait().has_value());
            CHECK(tickCompleted());
            sleepMs(100);
            CHECK_EQ(fake->sends(), 4u);
            std::vector<std::string> gens{a.gen, b.gen, e.gen};
            int total = 0;
            for (const auto &g : gens) {
                const auto n = count1("SELECT count(*)::bigint AS n FROM push_attempts WHERE generation_id = $1::uuid", g);
                CHECK(n <= 2);  // at most two claims per recipient in that tick
                total += static_cast<int>(n);
            }
            CHECK_EQ(total, 4);
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_attempts WHERE generation_id = $1::uuid", a.gen), 2);
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_attempts WHERE generation_id = $1::uuid", b.gen), 2);
            fake->completeHeld({"accepted", 201, std::nullopt});
            waitUntil([&]() {
                long long seen = 0;
                for (const auto &g : gens) {
                    seen += count1("SELECT count(*)::bigint AS n FROM push_attempt_observations WHERE generation_id = $1::uuid", g);
                }
                return seen == 4;
            });
            // Capacity released: the remaining recipient's older work goes first, again bounded.
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 8u; });
            sleepMs(50);
            CHECK_EQ(fake->sends(), 8u);
            fake->completeHeld({"accepted", 201, std::nullopt});
        } catch (const std::exception &) {
            c::expectThat(false, "push-capacity", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: lease expiry and crash recovery; five provider attempts; exhausted is never resurrected") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;  // a sender that never answers: a crash or a stall
            auto policy = fastPolicy();
            policy.leaseSeconds = 0.5;  // the fenced pre-send must still see a valid lease
            use(policy, fake);
            const auto n = notice(pair.space.userA);
            for (size_t k = 1; k <= 5; ++k) {
                CHECK(tickCompleted());
                waitUntil([&]() { return fake->sends() == k; });
                CHECK_EQ(attempts(n, pair.gen), static_cast<long long>(k));
                sleepMs(520);
            }
            CHECK(tickCompleted());
            CHECK_EQ(expiries(n, pair.gen), 5);
            CHECK_EQ(observations(n, pair.gen), 0);  // synthetic expiries never occupy the observation slot
            CHECK_EQ(used(n, pair.gen), 5);
            CHECK_EQ(outcome(n, pair.gen), std::string("exhausted"));
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n, pair.gen), 5);

            const d::AttemptKey k1{n, pair.gen, 1}, k2{n, pair.gen, 2};
            // A late accepted stays recorded, is distinguishable, and rewrites nothing.
            const auto late = record(k1, {"accepted", 201, std::nullopt});
            CHECK(late.ok && late.inserted);
            CHECK_EQ(late.finalState, std::string("exhausted"));
            CHECK_EQ(observed(n, pair.gen, 1), std::string("accepted"));
            CHECK_EQ(expiries(n, pair.gen), 5);
            CHECK_EQ(used(n, pair.gen), 5);
            // A late internal result lowers the recalculated budget, and still nothing is claimed.
            const auto internal = record(k2, {"busy", 503, std::nullopt});
            CHECK(internal.ok);
            CHECK_EQ(used(n, pair.gen), 4);
            CHECK(tickCompleted());
            sleepMs(30);
            CHECK_EQ(attempts(n, pair.gen), 5);
            CHECK_EQ(outcome(n, pair.gen), std::string("exhausted"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-lease", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: Retry-After and backoff; final transient attempt exhausts; duplicates repeat nothing") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            int calls = 0;
            fake->script = [&calls](const d::SendRequest &) -> d::SendResult {
                ++calls;
                if (calls == 1) return {"transient", 503, 1};  // Retry-After longer than the backoff
                if (calls == 2) return {"unknown", std::nullopt, std::nullopt};
                return {"accepted", 201, std::nullopt};
            };
            auto policy = fastPolicy();
            policy.backoffSeconds = {0.2, 0.3, 0.0, 0.0};
            use(policy, fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(n, pair.gen, 1) == "transient"; });
            sleepMs(300);  // past the 0.2 s backoff, before the 1 s Retry-After
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n, pair.gen), 1);
            sleepMs(800);
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n, pair.gen), 2);
            waitUntil([&]() { return observed(n, pair.gen, 2) == "unknown"; });
            CHECK(tickCompleted());  // inside the second backoff step
            CHECK_EQ(attempts(n, pair.gen), 2);
            sleepMs(320);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted"; });
            CHECK_EQ(used(n, pair.gen), 3);

            // Duplicate recording: the first observation stands; the different second one is ignored.
            const d::AttemptKey k3{n, pair.gen, 3};
            const auto dup = record(k3, {"rejected", 400, std::nullopt});
            CHECK(dup.ok);
            CHECK_FALSE(dup.inserted);
            CHECK_EQ(dup.storedOutcome, std::string("accepted"));
            CHECK_EQ(dup.finalState, std::string("accepted"));
            CHECK_EQ(observations(n, pair.gen), 3);

            // Every attempt transient: the fifth observation decides exhausted; no sixth attempt.
            auto fake2 = newFake();
            fake2->script = [](const d::SendRequest &) -> d::SendResult { return {"transient", 500, std::nullopt}; };
            use(fastPolicy(), fake2);
            const auto n2 = notice(pair.space.userA);
            for (int k = 1; k <= 5; ++k) {
                CHECK(tickCompleted());
                waitUntil([&]() { return observed(n2, pair.gen, k) == "transient"; });
            }
            waitUntil([&]() { return outcome(n2, pair.gen) == "exhausted"; });
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n2, pair.gen), 5);
            // A rejected result ends a delivery at once.
            auto fake3 = newFake();
            fake3->script = [](const d::SendRequest &) -> d::SendResult { return {"rejected", 413, std::nullopt}; };
            use(fastPolicy(), fake3);
            const auto n3 = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(n3, pair.gen) == "rejected"; });
            CHECK_EQ(used(n3, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-retry", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: internal failures use no budget but expire; TTL before and after attempts; late facts stay") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->script = [](const d::SendRequest &) -> d::SendResult { return {"busy", 503, std::nullopt}; };
            use(fastPolicy(), fake);
            const auto n = notice(pair.space.userA);
            for (int k = 1; k <= 6; ++k) {
                CHECK(tickCompleted());
                waitUntil([&]() { return observed(n, pair.gen, k) == "busy"; });
            }
            CHECK_EQ(attempts(n, pair.gen), 6);
            CHECK_EQ(used(n, pair.gen), 0);
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            auto shortTtl = fastPolicy();
            shortTtl.ttlSeconds = 0.3;
            sleepMs(320);
            use(shortTtl, fake);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(n, pair.gen), std::string("expired"));

            // TTL before the first attempt.
            owner("pause");
            use(fastPolicy(), fake);
            const auto first = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(first, pair.gen), 1);
            owner("resume");
            sleepMs(320);
            use(shortTtl, fake);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(first, pair.gen), std::string("expired"));
            CHECK_EQ(attempts(first, pair.gen), 0);

            // Section 6.4: attempt 1 -> synthetic expiry -> TTL -> a late accepted stays observed.
            auto held = newFake();
            held->manual = true;
            auto leasePolicy = fastPolicy();
            leasePolicy.leaseSeconds = 0.4;
            use(leasePolicy, held);
            const auto seq = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return held->sends() == 1u; });
            sleepMs(420);
            // Backoff keeps it from a second claim in this tick, so the sequence stays on attempt 1.
            leasePolicy.backoffSeconds = {3600.0, 3600.0, 3600.0, 3600.0};
            use(leasePolicy, held);
            CHECK(tickCompleted());
            CHECK_EQ(expiries(seq, pair.gen), 1);
            sleepMs(320);
            auto ttlAndLease = leasePolicy;
            ttlAndLease.ttlSeconds = 0.3;
            use(ttlAndLease, held);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(seq, pair.gen), std::string("expired"));
            const d::AttemptKey key{seq, pair.gen, 1};
            const auto late = record(key, {"accepted", 201, std::nullopt});
            CHECK(late.ok && late.inserted);
            CHECK_EQ(late.finalState, std::string("expired"));
            CHECK_EQ(observed(seq, pair.gen, 1), std::string("accepted"));
            CHECK_EQ(expiries(seq, pair.gen), 1);
            CHECK_EQ(used(seq, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ttl", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: gone revokes only its own generation, attributed to the provider; late gone after replacement") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 1u; });
            // The endpoint is replaced by G3 while attempt 1 for G1 is in flight.
            auto rekeyed = pair.device.reg;
            rekeyed.p256dh = pushunit::p256dhOtherThan(rekeyed.p256dh);
            const auto g3 = p::gen(p::reg(pair.space.userB, pair.sessionB, c::newUuid(), rekeyed));
            CHECK(g3 != pair.gen);
            const d::AttemptKey key{n, pair.gen, 1};
            const auto late = record(key, {"gone", 410, std::nullopt});
            CHECK(late.ok && late.inserted);
            CHECK_EQ(late.finalState, std::string("gone"));
            // G1 keeps its replacement revocation; G3 is untouched and still active.
            CHECK_EQ(p::revokedReason(*env.observer, pair.gen), "replaced|" + g3 + "|" + pair.space.userB);
            CHECK_EQ(p::revokedReason(*env.observer, g3), std::string("none"));
            CHECK_EQ(p::stateOf(p::list(pair.space.userB, pair.sessionB), g3), std::string("active"));
            // Duplicate gone: nothing repeats.
            const auto dup = record(key, {"gone", 410, std::nullopt});
            CHECK(dup.ok);
            CHECK_FALSE(dup.inserted);
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE subscription_id = $1::uuid", pair.gen), 1);

            // Plain gone on the current generation: provider attribution, no human actor.
            const auto n2 = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 2u; });
            const d::AttemptKey key2{n2, g3, 1};
            const auto gone = record(key2, {"gone", 404, std::nullopt});
            CHECK(gone.ok);
            CHECK_EQ(gone.finalState, std::string("gone"));
            const auto rows = env.observer->client->execSqlSync(
                "SELECT reason, revoked_by IS NULL AS no_actor, caused_by IS NULL AS no_cause, gone_notification_id::text AS src, "
                "gone_attempt_no FROM push_subscription_revocations WHERE subscription_id = $1::uuid", g3);
            CHECK(rows.size() == 1u);
            if (rows.size() == 1u) {
                CHECK_EQ(rows[0]["reason"].as<std::string>(), std::string("gone"));
                CHECK(rows[0]["no_actor"].as<bool>());
                CHECK(rows[0]["no_cause"].as<bool>());
                CHECK_EQ(rows[0]["src"].as<std::string>(), n2);
                CHECK_EQ(rows[0]["gone_attempt_no"].as<int>(), 1);
            }
            // The API shows it as such; a user revoke retry returns the original facts; slot freed.
            const auto listed = p::list(pair.space.userB, pair.sessionB);
            CHECK_FALSE(p::inDevices(listed, g3));
            CHECK_EQ(p::stateOf(listed, g3), std::string("revoked"));
            if (listed) for (const auto &g : listed->history) if (g.id == g3) CHECK_EQ(g.revokedReason, std::string("gone"));
            const auto replay = p::revoke(pair.space.userB, pair.sessionB, g3);
            CHECK(p::is(replay, p::PO::Replayed));
            if (replay) {
                CHECK_EQ(replay->revocation["reason"].asString(), std::string("gone"));
                CHECK(replay->revocation["revoked_by"].isNull());
            }
            // The same endpoint can register again as a new generation; no new notice reaches G3.
            const auto g4 = p::gen(p::reg(pair.space.userB, pair.sessionB, c::newUuid(), rekeyed));
            CHECK(g4 != g3);
            const auto n3 = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(n3, g3), 0);
            CHECK_EQ(hasDelivery(n3, g4), 1);
            fake->completeHeld({"accepted", 201, std::nullopt});
        } catch (const std::exception &) {
            c::expectThat(false, "push-gone", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: worker and provider auth failures pause distinctly; duplicates do not re-pause; resume keeps cutover") {
        try {
            freshEnable();
            const auto cut = cutoverText();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 1u; });
            const d::AttemptKey key{n, pair.gen, 1};
            CHECK(record(key, {"worker_auth_failed", 401, std::nullopt}).ok);
            CHECK_EQ(controlState(), std::string("paused"));
            CHECK_EQ(used(n, pair.gen), 0);
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            const auto reason = [&]() {
                return env.observer->client->execSqlSync(
                    "SELECT reason_code, actor FROM push_dispatch_control ORDER BY revision DESC LIMIT 1")[0];
            };
            CHECK_EQ(reason()["reason_code"].as<std::string>(), std::string("worker_auth_failed"));
            CHECK_EQ(reason()["actor"].as<std::string>(), std::string("dispatcher"));
            // Paused: fan-out continues, no new claims.
            const auto n2 = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(n2, pair.gen), 1);
            CHECK_EQ(attempts(n2, pair.gen), 0);
            CHECK_EQ(attempts(n, pair.gen), 1);

            owner("resume");
            CHECK_EQ(cutoverText(), cut);
            // A duplicate recording of the old failure must not pause again.
            const auto dup = record(key, {"worker_auth_failed", 401, std::nullopt});
            CHECK(dup.ok);
            CHECK_FALSE(dup.inserted);
            CHECK_EQ(controlState(), std::string("enabled"));

            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 3u; });
            const d::AttemptKey providerKey = [&]() {
                for (const auto &r : fake->requests()) {
                    if (r.key.notificationId == n && r.key.attemptNo == 2) return r.key;
                }
                throw std::runtime_error("attempt 2 missing");
            }();
            CHECK(record(providerKey, {"provider_auth_failed", 403, std::nullopt}).ok);
            CHECK_EQ(controlState(), std::string("paused"));
            CHECK_EQ(reason()["reason_code"].as<std::string>(), std::string("provider_auth_failed"));
            CHECK_EQ(used(n, pair.gen), 1);  // a provider answered: it counts, unlike the worker failure
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));  // retryable after resume
            CHECK_EQ(cutoverText(), cut);
            // Another observation while already paused writes no second pause.
            const auto revisions = controlRevisions();
            const d::AttemptKey n2Key{n2, pair.gen, 1};
            CHECK(record(n2Key, {"provider_auth_failed", 401, std::nullopt}).ok);
            CHECK_EQ(controlRevisions(), revisions);
            owner("resume");
            fake->completeHeld({"accepted", 201, std::nullopt});
        } catch (const std::exception &) {
            c::expectThat(false, "push-auth", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b repair R1: stale claims start no request; the exact attempt is fenced") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            auto shortLease = fastPolicy();
            shortLease.leaseSeconds = 0.2;
            use(shortLease, fake);

            // Claim k, let it expire, claim k+1, then run the old k's after-commit work.
            const auto n = notice(pair.space.userA);
            const auto first = claimOnly();
            CHECK_EQ(first.claims.size(), 1u);
            sleepMs(230);
            use(fastPolicy(), fake);
            const auto second = claimOnly();
            CHECK_EQ(second.claims.size(), 1u);
            if (!second.claims.empty()) CHECK_EQ(second.claims[0].attemptNo, 2);
            CHECK_EQ(expiries(n, pair.gen), 1);
            env.current->sendClaims(first.claims);
            waitUntil([&]() { return observed(n, pair.gen, 1) == "lease_lost_skipped"; });
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(used(n, pair.gen), 0);  // never sent, so no provider budget; no provider fact invented
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            // The current attempt is unaffected and sends once.
            env.current->sendClaims(second.claims);
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted"; });
            CHECK_EQ(fake->sends(), 1u);
            if (fake->sends() == 1u) CHECK_EQ(fake->requests()[0].key.attemptNo, 2);

            // Lease ended, no expiry row and no successor yet.
            use(shortLease, fake);
            const auto n2 = notice(pair.space.userA);
            const auto lapsed = claimOnly();
            CHECK_EQ(lapsed.claims.size(), 1u);
            sleepMs(230);
            env.current->sendClaims(lapsed.claims);
            waitUntil([&]() { return observed(n2, pair.gen, 1) == "lease_lost_skipped"; });
            CHECK_EQ(fake->sends(), 1u);
            CHECK_EQ(expiries(n2, pair.gen), 0);

            // Already observed: no request and no second fact. n2 stays out of the way by backoff.
            auto settled = fastPolicy();
            settled.internalBackoffSeconds = 3600.0;
            use(settled, fake);
            const auto n3 = notice(pair.space.userA);
            const auto done = claimOnly();
            CHECK_EQ(done.claims.size(), 1u);
            CHECK(record(done.claims.at(0), {"accepted", 201, std::nullopt}).ok);
            env.current->sendClaims(done.claims);
            // A claim from another instance and an attempt that does not exist: nothing at all.
            auto foreignPolicy = settled;
            foreignPolicy.instanceId = "test-b";
            const auto n4 = notice(pair.space.userA);
            const auto theirs = claimOnly();
            CHECK_EQ(theirs.claims.size(), 1u);
            use(foreignPolicy, fake);
            env.current->sendClaims(theirs.claims);
            env.current->sendClaims({d::AttemptKey{n3, pair.gen, 99}});
            sleepMs(150);
            CHECK_EQ(fake->sends(), 1u);
            CHECK_EQ(observed(n3, pair.gen, 1), std::string("accepted"));
            CHECK_EQ(observations(n3, pair.gen), 1);
            CHECK_EQ(observed(n4, pair.gen, 1), std::string("none"));
            CHECK_EQ(attempts(n3, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-fence", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b repair R2: a stale auth failure is stored but cannot pause a newer owner state") {
        try {
            const auto lastReason = []() {
                return env.observer->client->execSqlSync(
                    "SELECT reason_code FROM push_dispatch_control ORDER BY revision DESC LIMIT 1")[0]["reason_code"]
                    .as<std::string>();
            };
            const auto attemptOf = [](FakeTransport &f, const std::string &n) {
                for (const auto &r : f.requests()) {
                    if (r.key.notificationId == n) return r.key;
                }
                throw std::runtime_error("no request for the notice");
            };
            for (const char *reason : {"worker_auth_failed", "provider_auth_failed"}) {
                for (const bool viaDisable : {false, true}) {
                    freshEnable();
                    auto pair = newPair(pool);
                    auto fake = newFake();
                    fake->manual = true;
                    use(fastPolicy(), fake);
                    const auto oldNotice = notice(pair.space.userA);
                    CHECK(tickCompleted());
                    waitUntil([&]() { return fake->sends() == 1u; });
                    const auto oldKey = attemptOf(*fake, oldNotice);
                    // The response is held across an owner action.
                    if (viaDisable) {
                        owner("disable");
                        owner("enable");
                    } else {
                        owner("pause");
                        owner("resume");
                    }
                    const auto revisions = controlRevisions();
                    const auto stale = record(oldKey, {reason, 401, std::nullopt});
                    CHECK(stale.ok && stale.inserted);
                    CHECK_EQ(stale.storedOutcome, std::string(reason));  // the late fact is kept
                    CHECK_EQ(observed(oldNotice, pair.gen, 1), std::string(reason));
                    CHECK_EQ(controlState(), std::string("enabled"));
                    CHECK_EQ(controlRevisions(), revisions);
                    // Duplicate recording stays harmless.
                    const auto dup = record(oldKey, {reason, 401, std::nullopt});
                    CHECK(dup.ok);
                    CHECK_FALSE(dup.inserted);
                    CHECK_EQ(controlState(), std::string("enabled"));
                    CHECK_EQ(controlRevisions(), revisions);

                    // A failure of a claim made under the current revision still pauses.
                    const auto newNotice = notice(pair.space.userA);
                    CHECK(tickCompleted());
                    waitUntil([&]() {
                        for (const auto &r : fake->requests()) if (r.key.notificationId == newNotice) return true;
                        return false;
                    });
                    const auto current = record(attemptOf(*fake, newNotice), {reason, 401, std::nullopt});
                    CHECK(current.ok && current.inserted);
                    CHECK_EQ(controlState(), std::string("paused"));
                    CHECK_EQ(lastReason(), std::string(reason));
                    CHECK_EQ(controlRevisions(), revisions + 1);
                    // Its duplicate after an owner resume does not pause again.
                    owner("resume");
                    const auto again = record(attemptOf(*fake, newNotice), {reason, 401, std::nullopt});
                    CHECK(again.ok);
                    CHECK_FALSE(again.inserted);
                    CHECK_EQ(controlState(), std::string("enabled"));
                }
            }
        } catch (const std::exception &) {
            c::expectThat(false, "push-stale-pause", "case failed (details withheld)");
        }
    }

    // Only a stack seeded before 0024 has such attempts; a guard-assigned revocation can never be NULL.
    if (std::getenv("USAPP_TEST_EXPECT_PRE0024")) {
        TEST_CASE("P2-5b repair R2: attempts claimed before 0024 have no provenance and can never pause") {
            try {
                freshEnable();
                use(fastPolicy(), newFake());
                const auto rows = env.observer->client->execSqlSync(
                    "SELECT a.notification_id::text AS n, a.generation_id::text AS g, a.attempt_no AS k FROM push_attempts a "
                    "WHERE a.instance_id = 'p25b-pre-0024' AND a.control_revision IS NULL AND NOT EXISTS "
                    "(SELECT 1 FROM push_attempt_observations o WHERE o.notification_id = a.notification_id "
                    "AND o.generation_id = a.generation_id AND o.attempt_no = a.attempt_no) ORDER BY a.notification_id");
                CHECK(rows.size() == 2u);
                const auto revisions = controlRevisions();
                const char *reasons[] = {"worker_auth_failed", "provider_auth_failed"};
                for (size_t i = 0; i < rows.size() && i < 2u; ++i) {
                    const auto stored = record({rows[i]["n"].as<std::string>(), rows[i]["g"].as<std::string>(), rows[i]["k"].as<int>()},
                                               {reasons[i], 401, std::nullopt});
                    CHECK(stored.ok && stored.inserted);  // the late fact is kept
                    CHECK_EQ(stored.storedOutcome, std::string(reasons[i]));
                    CHECK_EQ(controlState(), std::string("enabled"));
                    CHECK_EQ(controlRevisions(), revisions);
                }
            } catch (const std::exception &) {
                c::expectThat(false, "push-null-provenance", "case failed (details withheld)");
            }
        }
    }

    TEST_CASE("P2-5b repair R3: already_active and gone serialize on the endpoint in either order") {
        try {
            const auto waitBlocked = [](const std::string &waitEvent, const std::string &queryLike) {
                waitUntil([&]() {
                    return count2("SELECT count(*)::bigint AS n FROM pg_stat_activity WHERE wait_event_type = 'Lock' "
                                  "AND wait_event = $1 AND query LIKE $2", waitEvent, queryLike) >= 1;
                });
            };
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 1u; });
            const d::AttemptKey key{n, pair.gen, 1};

            // Registration first: it has read G1 as the occupant and waits before its command.
            auto table = c::acquireTx(env.maint);
            CHECK(c::exec(table, "LOCK TABLE push_registration_commands IN SHARE MODE").ok);
            const std::string activeKey = c::newUuid();
            auto registering = p::startRegister(pair.space.userB, pair.sessionB, activeKey, pair.device.reg);
            waitBlocked("relation", "INSERT INTO push_registration_commands%");
            // Gone now waits for the endpoint lock instead of revoking under the registration.
            auto goneDone = std::make_shared<ts::Pending<d::RecordStatus>>();
            env.current->recordResult(key, {"gone", 410, std::nullopt},
                                      [goneDone](d::RecordStatus s) { goneDone->deliver(std::move(s)); });
            waitBlocked("advisory", "INSERT INTO push_subscription_revocations%");
            CHECK_FALSE(goneDone->readyWithin(std::chrono::milliseconds(100)));
            CHECK_EQ(p::revokedReason(*env.observer, pair.gen), std::string("none"));
            c::rollbackTx(table);
            const auto active = registering->wait();
            CHECK(p::is(active, p::PO::Ok));  // a defined result, never an internal error
            CHECK_EQ(p::cmdResult(active), std::string("already_active"));
            CHECK_EQ(p::cmdGen(active), pair.gen);
            const auto gone = goneDone->wait();
            CHECK(gone && gone->ok);
            if (gone) CHECK_EQ(gone->finalState, std::string("gone"));
            CHECK_EQ(count2("SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE subscription_id = $1::uuid "
                            "AND reason = $2", pair.gen, "gone"), 1);
            // Replay keeps the original command and shows the current state; nothing is restored.
            const auto replay = p::reg(pair.space.userB, pair.sessionB, activeKey, pair.device.reg);
            CHECK(p::is(replay, p::PO::Replayed));
            CHECK_EQ(p::cmdResult(replay), std::string("already_active"));
            CHECK_EQ(p::cmdGen(replay), pair.gen);
            if (replay) {
                CHECK_EQ(replay->generation.state, std::string("revoked"));
                CHECK(replay->marker.empty());
            }
            CHECK_EQ(p::activeOnEndpoint(*env.observer, pair.device.reg.endpoint), 0);

            // Gone first: it holds the endpoint lock; the registration waits, then reads it as revoked.
            const auto g2Status = p::reg(pair.space.userB, pair.sessionB, c::newUuid(), pair.device.reg);
            CHECK(p::is(g2Status, p::PO::Created));
            const auto g2 = p::gen(g2Status);
            CHECK(g2 != pair.gen);
            const auto n2 = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 2u; });
            const d::AttemptKey key2{n2, g2, 1};
            auto outcomes = c::acquireTx(env.maint);
            CHECK(c::exec(outcomes, "LOCK TABLE push_delivery_outcomes IN SHARE MODE").ok);
            auto gone2Done = std::make_shared<ts::Pending<d::RecordStatus>>();
            env.current->recordResult(key2, {"gone", 404, std::nullopt},
                                      [gone2Done](d::RecordStatus s) { gone2Done->deliver(std::move(s)); });
            waitBlocked("relation", "INSERT INTO push_delivery_outcomes%");
            auto later = p::startRegister(pair.space.userB, pair.sessionB, c::newUuid(), pair.device.reg);
            waitBlocked("advisory", "%33014%");
            c::rollbackTx(outcomes);
            const auto gone2 = gone2Done->wait();
            CHECK(gone2 && gone2->ok);
            const auto created = later->wait();
            CHECK(p::is(created, p::PO::Created));
            CHECK(p::gen(created) != g2);
            // Gone stays on its own generation; the new generation is the single active occupant.
            CHECK_EQ(count2("SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE subscription_id = $1::uuid "
                            "AND reason = $2", g2, "gone"), 1);
            CHECK_EQ(p::revokedReason(*env.observer, p::gen(created)), std::string("none"));
            CHECK_EQ(p::activeOnEndpoint(*env.observer, pair.device.reg.endpoint), 1);
            CHECK_EQ(p::counted(p::list(pair.space.userB, pair.sessionB)), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-gone-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5b: pre-send check: no send, an explicit state, nothing left claimed") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);

            // Logout between the claim's commit and the send: inactive.
            const auto n1 = notice(pair.space.userA);
            auto report = claimOnly();
            CHECK(report.ok);
            CHECK_EQ(report.claims.size(), 1u);
            CHECK_EQ(fake->sends(), 0u);  // the tick itself never sends
            p::logoutSession(pool, pair.sessionB);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(n1, pair.gen) == "inactive"; });
            CHECK_EQ(observed(n1, pair.gen, 1), std::string("inactive_skipped"));
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(used(n1, pair.gen), 0);

            // A session that ended before the claim: pre-claim inactive, no attempt.
            auto ended2 = newPair(pool);
            owner("pause");
            const auto n3 = notice(ended2.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(hasDelivery(n3, ended2.gen), 1);
            p::logoutSession(pool, ended2.sessionB);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(n3, ended2.gen), std::string("inactive"));
            CHECK_EQ(attempts(n3, ended2.gen), 0);
            owner("resume");

            // Pause after the claim: control_skipped, back to pending, no budget used.
            auto live = newPair(pool);
            const auto n4 = notice(live.space.userA);
            report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            owner("pause");
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(n4, live.gen, 1) == "control_skipped"; });
            CHECK_EQ(outcome(n4, live.gen), std::string("none"));
            CHECK_EQ(used(n4, live.gen), 0);
            CHECK_EQ(fake->sends(), 0u);
            // Resume: the same delivery is claimed again and sent.
            owner("resume");
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(n4, live.gen) == "accepted"; });
            CHECK_EQ(attempts(n4, live.gen), 2);

            // Disable and enable after the claim: the notice is before the new cutover.
            const auto n5 = notice(live.space.userA);
            report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            owner("disable");
            owner("enable");
            const auto sendsBefore = fake->sends();
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(n5, live.gen) == "cutover_skipped"; });
            CHECK_EQ(observed(n5, live.gen, 1), std::string("cutover_skipped"));
            CHECK_EQ(fake->sends(), sendsBefore);

            // Disabled, no new enable: control_skipped, nothing left claimed.
            const auto n6 = notice(live.space.userA);
            report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            owner("disable");
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(n6, live.gen, 1) == "control_skipped"; });
            owner("enable");

            // TTL between claim and send: ttl_skipped and expired.
            const auto n7 = notice(live.space.userA);
            report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            sleepMs(120);
            auto shortTtl = fastPolicy();
            shortTtl.ttlSeconds = 0.1;
            use(shortTtl, fake);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(n7, live.gen) == "expired"; });
            CHECK_EQ(observed(n7, live.gen, 1), std::string("ttl_skipped"));

            // A terminal outcome written meanwhile: terminal_skipped, outcome unchanged.
            use(fastPolicy(), fake);
            const auto n8 = notice(live.space.userA);
            report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            pool->execSqlSync("INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) "
                              "VALUES ($1::uuid, $2::uuid, 'expired')", n8, live.gen);
            const auto sendsMid = fake->sends();
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(n8, live.gen, 1) == "terminal_skipped"; });
            CHECK_EQ(outcome(n8, live.gen), std::string("expired"));
            CHECK_EQ(fake->sends(), sendsMid);
        } catch (const std::exception &) {
            c::expectThat(false, "push-presend", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c: each attempt carries the remaining TTL of one fixed expiry, never a fresh one") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->script = [](const d::SendRequest &) { return d::SendResult{"transient", 503, std::nullopt}; };
            auto policy = fastPolicy();
            policy.ttlSeconds = 3600.0;
            use(policy, fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(n, pair.gen, 1) == "transient"; });
            sleepMs(2100);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(n, pair.gen, 2) == "transient"; });
            const auto requests = fake->requests();
            CHECK_EQ(requests.size(), 2u);
            if (requests.size() == 2u) {
                CHECK(requests[0].ttlSeconds <= 3600);
                CHECK(requests[0].ttlSeconds >= 3590);
                CHECK(requests[1].ttlSeconds <= requests[0].ttlSeconds - 2);
                CHECK_EQ(requests[0].expiresAtMs, requests[1].expiresAtMs);
                const auto expected = env.observer->client->execSqlSync(
                    "SELECT floor(extract(epoch FROM created_at + interval '3600 seconds') * 1000)::bigint AS ms "
                    "FROM notifications WHERE id = $1::uuid", n)[0]["ms"].as<long long>();
                CHECK_EQ(requests[0].expiresAtMs, expected);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "push-ttl", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c: a contradictory worker response neither revokes a generation nor pauses dispatch") {
        try {
            namespace w = usapptest::pushworkerunit;
            struct Case {
                const char *outcome;
                const char *status;
            };
            for (const Case &contradiction : {Case{"gone", "201"}, Case{"provider_auth_failed", "410"}}) {
                freshEnable();
                auto pair = newPair(pool);
                w::StubWorker stub([contradiction](int fd, const std::string &text) {
                    Json::Value body;
                    Json::CharReaderBuilder builder;
                    std::string errors;
                    const std::string json = text.substr(text.find("\r\n\r\n") + 4);
                    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                    reader->parse(json.data(), json.data() + json.size(), &body, &errors);
                    const d::AttemptKey key{body["job"]["notification_id"].asString(), body["job"]["generation_id"].asString(),
                                            body["job"]["attempt_no"].asInt()};
                    w::writeAll(fd, w::httpResponse(200, w::resultJson(key, contradiction.outcome, contradiction.status)));
                });
                use(fastPolicy(), std::make_shared<d::WorkerTransport>(w::configFor(stub.url(), 5.0)));
                const auto n = notice(pair.space.userA);
                CHECK(tickCompleted());
                waitUntil([&]() { return observed(n, pair.gen, 1) == "unknown"; });
                CHECK_EQ(outcome(n, pair.gen), std::string("none"));
                CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE subscription_id = $1::uuid",
                                pair.gen), 0);
                CHECK_EQ(controlState(), std::string("enabled"));
            }
        } catch (const std::exception &) {
            c::expectThat(false, "push-contradiction", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c: under a second of lifetime left is never sent") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            auto policy = fastPolicy();
            policy.ttlSeconds = 0.9;
            use(policy, fake);
            const auto n = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(n, pair.gen) == "expired"; });
            CHECK_EQ(fake->sends(), 0u);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ttl-short", "case failed (details withheld)");
        }
    }

    TEST_CASE("0041: a notice ahead of the clock is deferred with nothing spent, and goes when the clock catches up, within its one expiry") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto n = aheadNotice(pair, 1.2);
            for (int i = 0; i < 2; ++i) {
                // Each tick re-evaluates it and leaves no trace: no delivery, attempt, outcome or send.
                CHECK(tickCompleted());
                CHECK_EQ(deliveries(n), 0);
                CHECK_EQ(attempts(n, pair.gen), 0);
                CHECK_EQ(outcome(n, pair.gen), std::string("none"));
                CHECK_EQ(fake->sends(), 0u);
            }
            waitUntil([&]() { return noticeReached(n); });
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(n), 1);
            waitUntil([&]() { return fake->sends() == 1u; });
            const auto sent = fake->requests();
            if (sent.size() == 1u) {
                CHECK(sent[0].ttlSeconds >= 1);
                CHECK(sent[0].ttlSeconds <= d::kMaxWorkerTtlSeconds);
                CHECK_EQ(sent[0].expiresAtMs, expiryMs(n, 86400));
            }
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted"; });
            CHECK_EQ(attempts(n, pair.gen), 1);
            CHECK_EQ(used(n, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ahead-fanout", "case failed (details withheld)");
        }
    }

    TEST_CASE("0041: a delivery that exists already is not claimed while its notice is ahead; the claim comes on a later tick") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto n = aheadNotice(pair, 1.2);
            aheadDelivery(pair, n);
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n, pair.gen), 0);
            CHECK_EQ(used(n, pair.gen), 0);
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            CHECK_EQ(fake->sends(), 0u);
            waitUntil([&]() { return noticeReached(n); });
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 1u; });
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted"; });
            CHECK_EQ(attempts(n, pair.gen), 1);
            CHECK_EQ(used(n, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ahead-claim", "case failed (details withheld)");
        }
    }

    TEST_CASE("0041: the clock moving back between claim and pre-send sends nothing and costs no budget; the retry goes") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto n = notice(pair.space.userA);
            const auto report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            // As the clock falling behind this notice would leave it.
            moveNotice(n, 1.2);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(n, pair.gen, 1) == "notice_ahead_skipped"; });
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(used(n, pair.gen), 0);
            CHECK_EQ(expiries(n, pair.gen), 0);
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            // Still ahead: the next tick does not claim it.
            CHECK(tickCompleted());
            CHECK_EQ(attempts(n, pair.gen), 1);
            CHECK_EQ(fake->sends(), 0u);
            waitUntil([&]() { return noticeReached(n); });
            CHECK(tickCompleted());
            waitUntil([&]() { return fake->sends() == 1u; });
            const auto sent = fake->requests();
            if (sent.size() == 1u) {
                CHECK(sent[0].ttlSeconds >= 1);
                CHECK(sent[0].ttlSeconds <= d::kMaxWorkerTtlSeconds);
                CHECK_EQ(sent[0].expiresAtMs, expiryMs(n, 86400));
            }
            waitUntil([&]() { return outcome(n, pair.gen) == "accepted"; });
            CHECK_EQ(attempts(n, pair.gen), 2);
            CHECK_EQ(used(n, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ahead-presend", "case failed (details withheld)");
        }
    }

    TEST_CASE("0041: deferral neither ends nor extends a lifetime: expiry stands, as does a delayed pre-send past it") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            auto policy = fastPolicy();
            policy.ttlSeconds = 0.8;
            use(policy, fake);
            // Deferred while ahead, not expired; expired from its own created_at + lifetime, with nothing sent or spent.
            const auto n = aheadNotice(pair, 0.5);
            aheadDelivery(pair, n);
            CHECK(tickCompleted());
            CHECK_EQ(outcome(n, pair.gen), std::string("none"));
            CHECK_EQ(attempts(n, pair.gen), 0);
            waitUntil([&]() { return expiryReached(n, 0.8); });
            CHECK(tickCompleted());
            CHECK_EQ(outcome(n, pair.gen), std::string("expired"));
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(used(n, pair.gen), 0);
            // A pre-send delayed past the expiry is ttl_skipped, as before.
            auto longer = fastPolicy();
            longer.ttlSeconds = 86400.0;
            use(longer, fake);
            const auto late = notice(pair.space.userA);
            const auto report = claimOnly();
            CHECK_EQ(report.claims.size(), 1u);
            auto brief = fastPolicy();
            brief.ttlSeconds = 0.1;
            use(brief, fake);
            sleepMs(150);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(late, pair.gen) == "expired"; });
            CHECK_EQ(observed(late, pair.gen, 1), std::string("ttl_skipped"));
            CHECK_EQ(fake->sends(), 0u);
            CHECK_EQ(used(late, pair.gen), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "push-ahead-expiry", "case failed (details withheld)");
        }
    }

    runReminderPush();
}

inline void runReminderPush() {
    const ClientPtr &pool = env.pool;

    TEST_CASE("P2-6 R5: a source change committed between the claim and the pre-send sends nothing and costs nothing") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto revisions = controlRevisions();
            const auto oneRound = [&](const Reminder &item, const std::function<void()> &change) {
                const auto inbox = inboxRow(item.id);
                const auto report = claimOnly();
                const auto mine = claimsOf(report, item.id);
                CHECK_EQ(mine.size(), 1u);
                CHECK_EQ(fake->sends(), 0u);
                change();
                env.current->sendClaims(report.claims);
                checkStaleSkip(item.id, pair.gen, 1, 0, 0u, *fake, inbox, revisions);
            };
            const auto disabled = taskReminder(pair, 600);
            oneRound(disabled, [&]() { taskRuleOff(pair, disabled.task); });
            const auto completed = taskReminder(pair, 600);
            oneRound(completed, [&]() { c::runComplete(pair.space.userB, completed.task, c::newUuid()); });
            const auto moved = anniversaryReminder(pair);
            oneRound(moved, [&]() { reviseAnniversary(pair, moved.task, 9); });
            const auto dayOff = anniversaryReminder(pair);
            oneRound(dayOff, [&]() { anniversaryRuleOff(pair, dayOff.task); });
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-stale", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: a time-only rule edit or a title and note edit does not stop a valid reminder") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto plain = taskReminder(pair, 600);
            const auto retimed = taskReminder(pair, 600);
            auto report = claimOnly();
            CHECK_EQ(report.claims.size(), 2u);
            taskRuleRetimed(pair, retimed.task);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(plain.id, pair.gen) == "accepted" && outcome(retimed.id, pair.gen) == "accepted"; });
            CHECK_EQ(fake->sends(), 2u);
            const auto cosmetic = anniversaryReminder(pair);
            report = claimOnly();
            CHECK_EQ(claimsOf(report, cosmetic.id).size(), 1u);
            reviseAnniversary(pair, cosmetic.task, 1, "renamed", "noted");
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(cosmetic.id, pair.gen) == "accepted"; });
            CHECK_EQ(fake->sends(), 3u);
            for (const auto &r : fake->requests()) {
                CHECK(r.eventType == "reminder.task" || r.eventType == "reminder.anniversary");
                CHECK(r.targetKind == "task" || r.targetKind == "anniversary");
            }
            // A disable reversed before the check: derived from the current rule, and no terminal state decided otherwise.
            const auto reversed = taskReminder(pair, 600);
            report = claimOnly();
            taskRuleOff(pair, reversed.task);
            taskRuleRetimed(pair, reversed.task);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(reversed.id, pair.gen) == "accepted"; });
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-valid", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: every retry is rechecked; a stale retry costs nothing more and a valid one keeps its one expiry") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->script = [](const d::SendRequest &) { return d::SendResult{"transient", 503, std::nullopt}; };
            use(fastPolicy(), fake);
            const auto revisions = controlRevisions();
            const auto item = taskReminder(pair, 600);
            const auto inbox = inboxRow(item.id);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(item.id, pair.gen, 1) == "transient"; });
            CHECK_EQ(used(item.id, pair.gen), 1);
            // Still valid: the retry is sent, with what remains of the same expiry.
            sleepMs(1050);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(item.id, pair.gen, 2) == "transient"; });
            CHECK_EQ(fake->sends(), 2u);
            const auto sent = fake->requests();
            if (sent.size() == 2u) {
                CHECK_EQ(sent[0].expiresAtMs, staleMs(item.id));
                CHECK_EQ(sent[1].expiresAtMs, sent[0].expiresAtMs);
                CHECK(sent[1].ttlSeconds < sent[0].ttlSeconds);
            }
            // Now stale: the third attempt is claimed, skipped, and the provider attempts stay at two.
            const auto report = claimOnly();
            CHECK_EQ(claimsOf(report, item.id).size(), 1u);
            taskRuleOff(pair, item.task);
            env.current->sendClaims(report.claims);
            checkStaleSkip(item.id, pair.gen, 3, 2, 2u, *fake, inbox, revisions);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-retry", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: a date moved away and back: a retry goes by the current schedule; a terminal delivery stays terminal") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            int calls = 0;
            fake->script = [&calls](const d::SendRequest &) -> d::SendResult {
                return ++calls == 1 ? d::SendResult{"transient", 503, std::nullopt} : d::SendResult{"accepted", 201, std::nullopt};
            };
            use(fastPolicy(), fake);
            // Away and back before the retry's check: sent, once, with no second occurrence.
            const auto returned = anniversaryReminder(pair);
            CHECK(tickCompleted());
            waitUntil([&]() { return observed(returned.id, pair.gen, 1) == "transient"; });
            reviseAnniversary(pair, returned.task, 9);
            reviseAnniversary(pair, returned.task, 1);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(returned.id, pair.gen) == "accepted"; });
            CHECK_EQ(fake->sends(), 2u);
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM reminder_occurrences WHERE anniversary_id = $1::uuid", returned.task), 1);

            // Checked while away: terminal. Moving back later brings nothing back.
            const auto revisions = controlRevisions();
            const auto decided = anniversaryReminder(pair);
            const auto inbox = inboxRow(decided.id);
            const auto report = claimOnly();
            CHECK_EQ(claimsOf(report, decided.id).size(), 1u);
            reviseAnniversary(pair, decided.task, 9);
            env.current->sendClaims(report.claims);
            checkStaleSkip(decided.id, pair.gen, 1, 0, 2u, *fake, inbox, revisions);
            reviseAnniversary(pair, decided.task, 1);
            CHECK(tickCompleted());
            sleepMs(50);
            CHECK_EQ(outcome(decided.id, pair.gen), std::string("source_stale"));
            CHECK_EQ(attempts(decided.id, pair.gen), 1);
            CHECK_EQ(fake->sends(), 2u);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-move", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: stale_at boundaries: sent with time left, the existing guard under a second, stale from the instant") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto other = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto revisions = controlRevisions();
            const auto under = taskReminder(pair, 3);
            const auto reached = taskReminder(pair, 3);
            const auto inTime = taskReminder(other, 3);
            const auto inbox = inboxRow(reached.id);
            const auto report = claimOnly();
            CHECK_EQ(report.claims.size(), 3u);
            // Time left: sent, with the window's remainder in both forms.
            env.current->sendClaims(claimsOf(report, inTime.id));
            waitUntil([&]() { return outcome(inTime.id, other.gen) == "accepted"; });
            CHECK_EQ(fake->sends(), 1u);
            if (fake->sends() == 1u) {
                CHECK(fake->requests()[0].ttlSeconds >= 1);
                CHECK(fake->requests()[0].ttlSeconds <= 3);
                CHECK_EQ(fake->requests()[0].expiresAtMs, staleMs(inTime.id));
            }
            // Under a second, source valid: the existing guard, ending as an ordinary expiry.
            waitForWindow(under.id, 0.7);
            env.current->sendClaims(claimsOf(report, under.id));
            waitUntil([&]() { return outcome(under.id, pair.gen) == "expired"; });
            CHECK_EQ(observed(under.id, pair.gen, 1), std::string("ttl_skipped"));
            CHECK_EQ(used(under.id, pair.gen), 0);
            CHECK_EQ(fake->sends(), 1u);
            // From stale_at, which is also this one-off's deadline: stale, never an ordinary expiry.
            waitForWindow(reached.id, -0.02);
            env.current->sendClaims(claimsOf(report, reached.id));
            checkStaleSkip(reached.id, pair.gen, 1, 0, 1u, *fake, inbox, revisions);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-boundary", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: precedence: every existing fence answers before a stale source") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            // Stale and past the delivery's lifetime: expired, as the tick's own sweep would say.
            const auto expired = taskReminder(pair, 600);
            auto report = claimOnly();
            taskRuleOff(pair, expired.task);
            sleepMs(120);
            auto shortTtl = fastPolicy();
            shortTtl.ttlSeconds = 0.1;
            use(shortTtl, fake);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return outcome(expired.id, pair.gen) == "expired"; });
            CHECK_EQ(observed(expired.id, pair.gen, 1), std::string("ttl_skipped"));

            // Stale and paused: nothing is decided until the retry after resume.
            use(fastPolicy(), fake);
            const auto paused = taskReminder(pair, 600);
            report = claimOnly();
            taskRuleOff(pair, paused.task);
            owner("pause");
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(paused.id, pair.gen, 1) == "control_skipped"; });
            CHECK_EQ(outcome(paused.id, pair.gen), std::string("none"));
            owner("resume");
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(paused.id, pair.gen) == "source_stale"; });
            CHECK_EQ(observed(paused.id, pair.gen, 2), std::string("source_stale_skipped"));

            // Stale after losing the lease: this claim says nothing about the source, and the next one does.
            auto shortLease = fastPolicy();
            shortLease.leaseSeconds = 0.2;
            use(shortLease, fake);
            const auto lost = taskReminder(pair, 600);
            const auto first = claimOnly();
            taskRuleOff(pair, lost.task);
            sleepMs(230);
            env.current->sendClaims(first.claims);
            waitUntil([&]() { return observed(lost.id, pair.gen, 1) == "lease_lost_skipped"; });
            CHECK_EQ(outcome(lost.id, pair.gen), std::string("none"));
            use(fastPolicy(), fake);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(lost.id, pair.gen) == "source_stale"; });

            // Stale with a terminal outcome already written: the outcome stands.
            const auto ended = taskReminder(pair, 600);
            report = claimOnly();
            taskRuleOff(pair, ended.task);
            pool->execSqlSync("INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES ($1::uuid, $2::uuid, 'expired')",
                              ended.id, pair.gen);
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return observed(ended.id, pair.gen, 1) == "terminal_skipped"; });
            CHECK_EQ(outcome(ended.id, pair.gen), std::string("expired"));

            // Another instance's claim on a stale source: nothing at all.
            const auto foreign = taskReminder(pair, 600);
            report = claimOnly();
            taskRuleOff(pair, foreign.task);
            auto otherInstance = fastPolicy();
            otherInstance.instanceId = "test-b";
            use(otherInstance, fake);
            env.current->sendClaims(report.claims);
            sleepMs(80);
            CHECK_EQ(observations(foreign.id, pair.gen), 0);
            CHECK_EQ(outcome(foreign.id, pair.gen), std::string("none"));

            // Stale and logged out: inactive, and the session's end is what is recorded.
            use(fastPolicy(), fake);
            CHECK(record(report.claims.at(0), {"transport_unavailable", std::nullopt, std::nullopt}).ok);
            const auto loggedOut = taskReminder(pair, 600);
            report = claimOnly();
            taskRuleOff(pair, loggedOut.task);
            p::logoutSession(pool, pair.sessionB);
            env.current->sendClaims(claimsOf(report, loggedOut.id));
            waitUntil([&]() { return outcome(loggedOut.id, pair.gen) == "inactive"; });
            CHECK_EQ(observed(loggedOut.id, pair.gen, 1), std::string("inactive_skipped"));
            CHECK_EQ(fake->sends(), 0u);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-precedence", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: both TTL and absolute expiry stop at the earlier of the lifetime and stale_at, up to the worker's request") {
        try {
            namespace w = usapptest::pushworkerunit;
            freshEnable();
            auto pair = newPair(pool);
            // stale_at first: the real transport's request to a fake worker carries it in both forms.
            std::mutex seenMutex;
            std::vector<Json::Value> seen;
            w::StubWorker stub([&seenMutex, &seen](int fd, const std::string &text) {
                Json::Value body;
                Json::CharReaderBuilder builder;
                std::string errors;
                const std::string json = text.substr(text.find("\r\n\r\n") + 4);
                const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                reader->parse(json.data(), json.data() + json.size(), &body, &errors);
                {
                    std::lock_guard<std::mutex> guard(seenMutex);
                    seen.push_back(body);
                }
                const d::AttemptKey key{body["job"]["notification_id"].asString(), body["job"]["generation_id"].asString(),
                                        body["job"]["attempt_no"].asInt()};
                w::writeAll(fd, w::httpResponse(200, w::resultJson(key, "accepted", "201")));
            });
            use(fastPolicy(), std::make_shared<d::WorkerTransport>(w::configFor(stub.url(), 5.0)));
            const auto item = taskReminder(pair, 600);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(item.id, pair.gen) == "accepted"; });
            {
                std::lock_guard<std::mutex> guard(seenMutex);
                CHECK_EQ(seen.size(), 1u);
                if (seen.size() == 1u) {
                    CHECK(seen[0]["ttl_seconds"].asInt() <= 600);
                    CHECK(seen[0]["ttl_seconds"].asInt() >= 590);
                    CHECK_EQ(seen[0]["expires_at_ms"].asInt64(), static_cast<Json::Int64>(staleMs(item.id)));
                }
            }

            // The lifetime first: unchanged. So is a notice that is no reminder.
            auto fake = newFake();
            auto policy = fastPolicy();
            policy.ttlSeconds = 100.0;
            use(policy, fake);
            const auto shorter = taskReminder(pair, 600);
            const auto message = notice(pair.space.userA);
            CHECK(tickCompleted());
            waitUntil([&]() { return outcome(shorter.id, pair.gen) == "accepted" && outcome(message, pair.gen) == "accepted"; });
            for (const auto &r : fake->requests()) {
                const auto expected = env.observer->client->execSqlSync(
                    "SELECT floor(extract(epoch FROM created_at + interval '100 seconds') * 1000)::bigint AS ms FROM notifications WHERE id = $1::uuid",
                    r.key.notificationId)[0]["ms"].as<long long>();
                CHECK_EQ(r.expiresAtMs, expected);
                CHECK(r.ttlSeconds <= 100);
                CHECK(r.ttlSeconds >= 90);
            }
            CHECK_EQ(fake->sends(), 2u);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-expiry", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: a notification that is no reminder cannot record the new values and is otherwise unchanged") {
        try {
            freshEnable();
            auto pair = newPair(pool);
            auto fake = newFake();
            fake->manual = true;
            use(fastPolicy(), fake);
            const auto message = notice(pair.space.userA);
            const auto report = claimOnly();
            const auto mine = claimsOf(report, message);
            CHECK_EQ(mine.size(), 1u);
            // A forged skip is refused: no observation, no terminal state; the attempt stays resolvable.
            bool refused = false;
            try {
                pool->execSqlSync("INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) "
                                  "VALUES ($1::uuid, $2::uuid, 1, 'source_stale_skipped', 'internal')", message, pair.gen);
            } catch (const std::exception &) {
                refused = true;
            }
            CHECK(refused);
            CHECK_EQ(observations(message, pair.gen), 0);
            CHECK_EQ(outcome(message, pair.gen), std::string("none"));
            env.current->sendClaims(report.claims);
            waitUntil([&]() { return fake->sends() == 1u; });
            fake->completeHeld({"accepted", 201, std::nullopt});
            waitUntil([&]() { return outcome(message, pair.gen) == "accepted"; });
            CHECK_EQ(used(message, pair.gen), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-push-nonreminder", "case failed (details withheld)");
        }
    }
}

// USAPP_TEST_EXPECT_NO_0028: the database lacks that migration; this is then the whole run. Off stays off and writes
// nothing; on, the tick refuses before claiming, so no lease expires into provider budget.
inline void runMissing0028() {
    TEST_CASE("P2-6 R5: without 0028 dispatch stays off when disabled and fails closed when enabled") {
        try {
            if (controlState() != "disabled") owner("disable");
            auto pair = newPair(env.pool);
            auto fake = newFake();
            use(fastPolicy(), fake);
            const auto whileOff = notice(pair.space.userA);
            CHECK(tickCompleted());
            CHECK_EQ(deliveries(whileOff), 0);
            freshEnable();
            const auto n = notice(pair.space.userA);
            const auto refused = claimOnly();
            CHECK_FALSE(refused.ok);
            CHECK_EQ(refused.detail, std::string("push_dispatch stage=preflight_missing_0028"));
            CHECK(refused.claims.empty());
            const auto reports = tick();
            CHECK(reports.size() == 1u && reports[0].outcome == Scheduler::TickOutcome::Failed);
            CHECK_EQ(deliveries(n), 0);
            CHECK_EQ(fake->sends(), 0u);
        } catch (const std::exception &) {
            c::expectThat(false, "push-missing-0028", "case failed (details withheld)");
        }
    }
}

// Separate from the CTest run (its 30 s limit stands): this mode runs only when the isolated script
// sets USAPP_TEST_WORKER_URL, against the stub-wired and production-entry workers on that network.
inline void runWorkerIntegration(const char *workerUrl, const char *workerToken, const char *productionEntry) {
    const ClientPtr &pool = env.pool;
    const auto transportFor = [](const std::string &url, const std::string &token, double deadline = 15.0) {
        d::WorkerConfig config;
        config.url = url;
        config.token = token;
        config.deadlineSeconds = deadline;
        return std::make_shared<d::WorkerTransport>(config);
    };
    // A device whose endpoint path tells the fake provider what to do. Never a real provider: the worker
    // under test resolves every allowlisted name to its in-process stub.
    const auto deviceFor = [](const std::string &behaviour) {
        return p::Device{{"https://fcm.googleapis.com/fcm/send/" + behaviour + "-" + usapp::crypto::randomHex(8),
                          pushunit::kPublishedPoints[0], pushunit::authSecret()}};
    };
    struct Sent { std::string n; std::string g; Pair pair; };
    const auto sendOne = [&](const p::Device &device, const std::shared_ptr<d::Transport> &transport, double ttl = 86400.0) {
        freshEnable();
        Sent sent{"", "", newPair(pool, device)};
        auto policy = fastPolicy();
        policy.ttlSeconds = ttl;
        use(policy, transport);
        sent.n = notice(sent.pair.space.userA);
        sent.g = sent.pair.gen;
        if (!tickCompleted()) throw std::runtime_error("tick failed");
        return sent;
    };
    const auto stored = [&](const Sent &s, const char *column) {
        const auto rows = env.observer->client->execSqlSync(
            std::string("SELECT COALESCE(") + column + "::text, 'null') AS v FROM push_attempt_observations "
            "WHERE notification_id = $1::uuid AND generation_id = $2::uuid AND attempt_no = 1", s.n, s.g);
        return rows.size() == 1u ? rows[0]["v"].as<std::string>() : std::string("none");
    };

    TEST_CASE("P2-5c worker: scheduler to worker to stub provider; the payload decrypts and the TTL is the remaining one") {
        try {
            auto worker = transportFor(workerUrl, workerToken);
            // RFC 8291's user agent keys, so the stub provider can really decrypt and check the payload.
            const p::Device rfc{{"https://fcm.googleapis.com/fcm/send/verify-600-" + usapp::crypto::randomHex(8),
                                 pushunit::kPublishedPoints[0], "BTBZMqHH6r4Tts7J_aSIgg"}};
            const auto s = sendOne(rfc, worker, 600.0);
            waitUntil([&]() { return outcome(s.n, s.g) == "accepted"; }, 12000);
            CHECK_EQ(observed(s.n, s.g, 1), std::string("accepted"));
            CHECK_EQ(stored(s, "http_status"), std::string("201"));
            CHECK_EQ(used(s.n, s.g), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-worker-verify", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c worker: provider results reach the P2-5b rules as stored observations") {
        try {
            auto worker = transportFor(workerUrl, workerToken);
            const auto gone = sendOne(deviceFor("gone"), worker);
            waitUntil([&]() { return outcome(gone.n, gone.g) == "gone"; }, 12000);
            CHECK_EQ(count1("SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE subscription_id = $1::uuid "
                            "AND reason = 'gone'", gone.g), 1);

            const auto retry = sendOne(deviceFor("retry"), worker);
            waitUntil([&]() { return observed(retry.n, retry.g, 1) == "transient"; }, 12000);
            CHECK_EQ(stored(retry, "http_status"), std::string("429"));
            CHECK_EQ(stored(retry, "retry_after_s"), std::string("120"));
            CHECK_EQ(outcome(retry.n, retry.g), std::string("none"));
            CHECK_EQ(used(retry.n, retry.g), 1);

            const auto redirect = sendOne(deviceFor("redirect"), worker);
            waitUntil([&]() { return outcome(redirect.n, redirect.g) == "rejected"; }, 12000);
            CHECK_EQ(stored(redirect, "http_status"), std::string("302"));

            const auto reset = sendOne(deviceFor("reset"), worker);
            waitUntil([&]() { return observed(reset.n, reset.g, 1) == "unknown"; }, 12000);
            CHECK_EQ(used(reset.n, reset.g), 1);
            CHECK_EQ(outcome(reset.n, reset.g), std::string("none"));

            // The worker's own ten-second deadline over a provider that never answers.
            const auto slow = sendOne(deviceFor("slow"), worker);
            waitUntil([&]() { return observed(slow.n, slow.g, 1) == "unknown"; }, 14000);
            CHECK_EQ(stored(slow, "http_status"), std::string("null"));

            const auto auth = sendOne(deviceFor("auth"), worker);
            waitUntil([&]() { return observed(auth.n, auth.g, 1) == "provider_auth_failed"; }, 12000);
            waitUntil([&]() { return controlState() == "paused"; }, 4000);
            CHECK_EQ(env.maint->execSqlSync("SELECT reason_code FROM push_dispatch_control ORDER BY revision DESC LIMIT 1")[0]
                         ["reason_code"].as<std::string>(), std::string("provider_auth_failed"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-worker-provider", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c worker: a wrong token and an unreachable worker are internal, spend no budget") {
        try {
            auto wrong = transportFor(workerUrl, std::string(40, 'w'));
            const auto s = sendOne(deviceFor("accept"), wrong);
            waitUntil([&]() { return observed(s.n, s.g, 1) == "worker_auth_failed"; }, 12000);
            CHECK_EQ(used(s.n, s.g), 0);
            waitUntil([&]() { return controlState() == "paused"; }, 4000);

            auto unreachable = transportFor("http://worker-does-not-exist.invalid:8090", workerToken);
            const auto u = sendOne(deviceFor("accept"), unreachable);
            waitUntil([&]() { return observed(u.n, u.g, 1) == "transport_unavailable"; }, 12000);
            CHECK_EQ(used(u.n, u.g), 0);
            CHECK_EQ(outcome(u.n, u.g), std::string("none"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-worker-internal", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5c worker: the production entry refuses non-public addresses at the real boundary") {
        try {
            // The production process with its default policy. /etc/hosts points the allowlisted name
            // at loopback, so no lookup leaves the container and nothing can be reached.
            auto production = transportFor(productionEntry, workerToken);
            const auto s = sendOne(deviceFor("accept"), production);
            waitUntil([&]() { return observed(s.n, s.g, 1) == "transport_unavailable"; }, 12000);
            CHECK_EQ(used(s.n, s.g), 0);
            CHECK_EQ(outcome(s.n, s.g), std::string("none"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-worker-production", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::pushdispatch

int main() {
    using namespace usapp;
    namespace t = usapptest::pushdispatch;
    usapptest::installCrashDiagnostics();
    const char *dsn = std::getenv("USAPP_TEST_DSN");
    const char *maint = std::getenv("USAPP_TEST_MAINT_CONNINFO");
    if (!dsn || !*dsn || !maint || !*maint) {
        return usapptest::skip("P2-5b dispatch tests need USAPP_TEST_DSN and USAPP_TEST_MAINT_CONNINFO "
                               "(deploy/docker-compose.test-maint.yml).",
                               77);
    }
    try {
        db::Pool::instance().init(dsn, 4);
        db::Pool::instance().client()->setTimeout(5.0);
        t::env.pool = db::Pool::instance().client();
        t::env.maint = drogon::orm::DbClient::newPgClient(maint, 3);
        t::env.maint->setTimeout(5.0);
    } catch (const std::exception &) {
        std::cerr << "[ERROR] database clients could not be created (details withheld)\n";
        return 1;
    }
    const auto databaseOf = [](const usapptest::pushdispatch::ClientPtr &client) -> std::string {
        // Connections are established asynchronously; a bounded retry, without printing anything.
        for (int i = 0; i < 100; ++i) {
            try {
                return client->execSqlSync("SELECT current_database()::text AS name")[0]["name"].as<std::string>();
            } catch (const std::exception &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        return "";
    };
    // Both connections must be on literal usapp_test before anything is written.
    if (!usapptest::isExpectedTestDatabase(databaseOf(t::env.pool)) ||
        !usapptest::isExpectedTestDatabase(databaseOf(t::env.maint))) {
        std::cerr << "[SECURITY ALERT] refusing to run: both connections must report usapp_test\n";
        return 1;
    }
    try {
        const auto role = t::env.pool->execSqlSync("SELECT current_user::text AS r")[0]["r"].as<std::string>();
        const auto super = t::env.maint->execSqlSync("SELECT current_setting('is_superuser') AS s")[0]["s"].as<std::string>();
        const auto version = t::env.pool->execSqlSync(
            "SELECT count(*)::bigint AS n FROM schema_migrations WHERE version = '0024_push_dispatch_repairs'")[0]["n"].as<long long>();
        const auto stale = t::env.pool->execSqlSync(
            "SELECT count(*)::bigint AS n FROM schema_migrations WHERE version = '0028_reminder_push_stale'")[0]["n"].as<long long>();
        if (role != "usapp_api" || super != "on" || version != 1 || stale != (std::getenv("USAPP_TEST_EXPECT_NO_0028") ? 0 : 1)) {
            std::cerr << "[ERROR] unexpected roles or migration state\n";
            return 1;
        }
        t::env.observer = usapptest::taskcompletion::Observer::connect(dsn);
    } catch (const std::exception &) {
        std::cerr << "[ERROR] environment check failed (details withheld)\n";
        return 1;
    }

    AppConfig::instance().schedulerKeyNamespace = "usapp.test.p25b";
    scheduler::Scheduler::instance().configure(false, 15000, "test-a");
    t::env.job = std::make_shared<scheduler::PushDispatchJob>(
        std::make_shared<push::Dispatcher>(t::fastPolicy(), nullptr));
    t::env.current = std::make_shared<push::Dispatcher>(t::fastPolicy(), nullptr);
    scheduler::Scheduler::instance().registerJob(t::env.job);

    const char *workerUrl = std::getenv("USAPP_TEST_WORKER_URL");
    if (std::getenv("USAPP_TEST_EXPECT_NO_0028")) {
        t::runMissing0028();
    } else if (std::getenv("USAPP_TEST_ONLY_REMINDER_PUSH")) {
        t::runReminderPush();
    } else if (workerUrl && *workerUrl) {
        const char *token = std::getenv("USAPP_TEST_WORKER_TOKEN");
        const char *production = std::getenv("USAPP_TEST_PROD_WORKER_URL");
        if (!token || !*token || !production || !*production) {
            std::cerr << "[ERROR] worker integration needs its token and the production worker URL\n";
            return 1;
        }
        t::runWorkerIntegration(workerUrl, token, production);
    } else {
        t::run();
    }

    // The default remains off: this database ends disabled.
    try {
        if (t::controlState() != "disabled") t::owner("disable");
    } catch (const std::exception &) {
        std::cerr << "[ERROR] could not return control to disabled\n";
        return 1;
    }
    const int code = usapptest::finish();
    // Fake loops and ORM threads are torn down by process exit; nothing else is pending.
    std::cout.flush();
    std::fflush(nullptr);
    std::_Exit(code);
}
