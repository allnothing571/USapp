#pragma once
// D6 J2c2: journaled object cleanup (docs/phase3-deletion-journal.md Section 11). Fake journal and FakeObjectStore only, on a
// disposable database. Each case drives the real scheduler and media.cleanup job. In the full run other suites' plans share every
// round, so faults are aimed at this case's own media, and a negative check waits until this case's operation has met the journal.
#include <mutex>

#include "MediaDeletionDbTests.h"
#include "DeletionJournalUnitTests.h"
#include "core/Crypto.h"
#include "scheduler/LockKey.h"
#include "scheduler/jobs/MediaCleanupJob.h"

namespace usapptest::journalcleanup {
namespace md = usapptest::media;
namespace d = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace dj = usapp::deletionjournal;
using M = usapp::MediaService;
using MO = M::Outcome;

constexpr int kWait = 20000;

// The fake, made safe for the test thread to read while background callbacks write. Every journal call also records whether the
// cleanup-job lock, a photo lock or a deletion-key lock was held by anyone. That is meaningful only while no tick overlaps the round
// (a later tick's own transaction holds the job lock legitimately), so it is asserted only where no tick runs meanwhile.
struct CheckedJournal : usapptest::deletionjournalunit::FakeJournal {
    std::recursive_mutex guard;
    int lockedCalls = 0;
    std::map<std::string, int> seen;          // ensureIntent calls per target
    std::string loseIntentFor, failTerminalFor;
    void unlocked() {
        const auto key = usapp::scheduler::LockKey::forJob(usapp::scheduler::MediaCleanupJob::kJobKey).value();
        const auto rows = md::obs().client->execSqlSync(
            "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND granted AND "
            "((objsubid = 1 AND classid::bigint = (($1::bigint >> 32) & 4294967295) AND objid::bigint = ($1::bigint & 4294967295)) "
            "OR (objsubid = 2 AND classid IN (33018, 33025)))",
            std::to_string(key));
        if (rows[0]["n"].as<long long>() != 0) ++lockedCalls;
    }
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        std::lock_guard<std::recursive_mutex> hold(guard);
        unlocked();
        const auto &target = op.subjects.empty() ? std::string() : op.subjects[0].targetId;
        ++seen[target];
        if (!loseIntentFor.empty() && target == loseIntentFor) {
            loseIntentFor.clear();
            loseIntent = true;  // this call only: durable, receipt lost
        }
        FakeJournal::ensureIntent(op, std::move(callback));
    }
    void settle(const dj::Operation &op, dj::EffectState state, std::function<void(bool)> callback) override {
        std::lock_guard<std::recursive_mutex> hold(guard);
        unlocked();
        if (!failTerminalFor.empty() && !op.subjects.empty() && op.subjects[0].targetId == failTerminalFor) {
            ++settlements;
            callback(false);
            return;
        }
        FakeJournal::settle(op, state, std::move(callback));
    }
    void lookupOperation(const std::string &id, std::function<void(dj::LookupState, dj::Operation)> callback) override {
        std::lock_guard<std::recursive_mutex> hold(guard);
        FakeJournal::lookupOperation(id, std::move(callback));
    }
    template <typename F> void with(F f) {
        std::lock_guard<std::recursive_mutex> hold(guard);
        f();
    }
    int seenFor(const std::string &media) {
        std::lock_guard<std::recursive_mutex> hold(guard);
        const auto found = seen.find(media);
        return found == seen.end() ? 0 : found->second;
    }
    // This target's operations, in no particular order.
    std::vector<Entry> opsFor(const std::string &media) {
        std::lock_guard<std::recursive_mutex> hold(guard);
        std::vector<Entry> out;
        for (const auto &[id, entry] : entries) {
            if (!entry.operation.subjects.empty() && entry.operation.subjects[0].targetId == media) out.push_back(entry);
        }
        return out;
    }
    long long stateCount(const std::string &media, dj::IntentState state) {
        long long n = 0;
        for (const auto &entry : opsFor(media)) n += entry.state == state ? 1 : 0;
        return n;
    }
};

// Holds the first ready intent for one target until the test resumes it: a classification can change while it waits.
struct PausedJournal : CheckedJournal {
    std::string armed;
    std::function<void(dj::IntentState)> waiting;
    bool holding = false;
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        std::lock_guard<std::recursive_mutex> hold(guard);
        CheckedJournal::ensureIntent(op, [this, op, callback](dj::IntentState state) {
            if (!armed.empty() && op.subjects[0].targetId == armed && state == dj::IntentState::Ready) {
                armed.clear();
                waiting = callback;
                holding = true;
            } else {
                callback(state);
            }
        });
    }
    void arm(const std::string &target) { with([&]() { armed = target; }); }
    bool isHolding() {
        std::lock_guard<std::recursive_mutex> hold(guard);
        return holding;
    }
    void resume() {
        std::function<void(dj::IntentState)> callback;
        with([&]() { callback = std::move(waiting); waiting = {}; holding = false; });
        callback(dj::IntentState::Ready);
    }
};

struct Configured {
    explicit Configured(std::shared_ptr<dj::Journal> journal) {
        md::waitUntil([]() { return !M::cleanupBusy(); }, kWait);
        M::instance().configureDeletionJournal(std::move(journal));
    }
    ~Configured() {
        M::setCleanupFaultForTest(nullptr);
        try { md::waitUntil([]() { return !M::cleanupBusy(); }, kWait); } catch (...) {}
        M::instance().configureDeletionJournal(nullptr);
    }
};

// A fresh provider facade over the same durable fake contents simulates losing all process-local cursors.
struct ReopenedJournal : dj::Journal {
    std::shared_ptr<CheckedJournal> log;
    explicit ReopenedJournal(std::shared_ptr<CheckedJournal> value) : log(std::move(value)) {}
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> cb) override { log->ensureIntent(op, std::move(cb)); }
    void settle(const dj::Operation &op, dj::EffectState state, std::function<void(bool)> cb) override { log->settle(op, state, std::move(cb)); }
    void lookupOperation(const std::string &id, std::function<void(dj::LookupState, dj::Operation)> cb) override { log->lookupOperation(id, std::move(cb)); }
};
inline std::string cleanupId(const std::string &seed, const char *label) {
    auto hash = usapp::crypto::sha256Hex(std::string("usapp.media-journal.v1:") + label + ":" + seed);
    hash[12] = '8'; hash[16] = 'a';
    return hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-" + hash.substr(12, 4) + "-" + hash.substr(16, 4) + "-" + hash.substr(20, 12);
}
inline dj::Operation lateEpoch(dj::Operation exemplar, const std::string &key, int epoch) {
    const auto seed = "late|reconcile|" + key + "|epoch=" + std::to_string(epoch);
    exemplar.id = cleanupId(seed, "cleanup-operation");
    exemplar.subjects[0].intentId = cleanupId(seed, "cleanup-intent");
    return exemplar;
}

inline void idle() { md::waitUntil([]() { return !M::cleanupBusy(); }, kWait); }
// A tick, then the store-side round it started. No tick overlaps it.
inline void round() {
    idle();
    md::tick();
    md::sleepMs(30);
    idle();
}
// Whole rounds, one after another, until the condition holds after one: no further round can run before the caller looks.
inline void roundsUntil(const std::function<bool()> &condition, int timeoutMs = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        round();
        if (condition()) return;
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("bounded tick wait expired");
    }
}
inline bool withinRounds(const std::function<bool()> &condition, int limit = 4) {
    for (int i = 0; i < limit; ++i) {
        round();
        if (condition()) return true;
    }
    return false;
}
// Rounds until this media's operation has met the journal `more` more times.
inline void untilSeen(CheckedJournal &log, const std::string &media, int more = 1) {
    const int target = log.seenFor(media) + more;
    roundsUntil([&]() { return log.seenFor(media) >= target; });
}
inline long long removalsOf(const std::string &key) {
    long long n = 0;
    for (const auto &k : md::env.store->removalRequests()) n += k == key ? 1 : 0;
    return n;
}
inline long long passes(const std::string &id, const char *kind) {
    return md::count(std::string("SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid AND kind = '") + kind + "'", id);
}
inline std::string rejected(const ts::CommittedSpace &s) {
    const auto u = md::uploaded(s.userA, "junk;size=1000");
    if (md::stateOf(md::complete(s.userA, u.id)) != "rejected") throw std::runtime_error("not rejected");
    return u.id;
}
// READY on attempt 2, with a losing attempt 1, not old enough to be unattached, and its own leftovers plan. Committed directly.
inline std::string readyWithLoser(const ts::CommittedSpace &s) {
    const auto id = c::newUuid();
    auto &pool = md::env.pool;
    pool->execSqlSync(
        "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '1 hour', $4, repeat('a', 64), clock_timestamp() - interval '1 hour 10 minutes')",
        id, s.couple, s.userA, c::newUuid());
    for (const char *no : {"1", "2"}) {
        pool->execSqlSync(
            "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
            "VALUES ($1::uuid, $3::integer, $2::uuid, 'media/' || $2 || '/' || $1 || '/' || gen_random_uuid() || '/original', clock_timestamp() - interval '1 hour')",
            id, s.couple, std::string(no));
    }
    pool->execSqlSync(
        "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
        "VALUES ($1::uuid, $2::uuid, 'ready', 2, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '50 minutes')", id, s.couple);
    pool->execSqlSync("INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) VALUES ($1::uuid, 'leftovers', 'ready', clock_timestamp())", id);
    md::env.store->lateClientWrite(md::stagingOf(id), md::image());
    md::env.store->lateClientWrite(md::attemptKey(id, 1), md::image());
    md::env.store->lateClientWrite(md::attemptKey(id, 2), md::image());
    return id;
}
// One statement with triggers off, as maintenance would change the facts under a waiting intent. This run's database only.
inline const drogon::orm::DbClientPtr &maintClient() {
    static drogon::orm::DbClientPtr client;
    if (!client) {
        const char *info = std::getenv("USAPP_TEST_MAINT_CONNINFO");
        if (!info || !*info) throw std::runtime_error("this case needs USAPP_TEST_MAINT_CONNINFO");
        client = drogon::orm::DbClient::newPgClient(info, 1);
    }
    return client;
}
inline void maintenance(const std::string &body) {
    for (int tries = 0;; ++tries) {
        try {
            maintClient()->execSqlSync("DO $maint$ BEGIN PERFORM set_config('session_replication_role', 'replica', true); " + body + " END $maint$");
            return;
        } catch (const std::exception &) {
            if (tries >= 50) throw;
            md::sleepMs(20);
        }
    }
}
// Explicit fake enrollment of one deletion cycle, as a journaled deletion would have left it. Not a production import.
inline void enroll(CheckedJournal &log, const std::string &media, const std::string &cycle) {
    dj::Operation op{c::newUuid(), dj::Action::Delete, {{dj::Kind::Media, media, cycle, c::newUuid()}}};
    bool ready = false, committed = false;
    log.with([&]() {
        log.FakeJournal::ensureIntent(op, [&](dj::IntentState state) { ready = state == dj::IntentState::Ready; });
        log.FakeJournal::settle(op, dj::EffectState::Committed, [&](bool ok) { committed = ok; });
    });
    CHECK(ready);
    CHECK(committed);
}
inline void attempt(const char *label, const std::function<void()> &body) {
    try { body(); } catch (const std::exception &e) {
        c::expectThat(false, std::string("journal-cleanup-") + label, md::exceptionCategory(e));
    }
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);

    TEST_CASE("journal cleanup: an unavailable or unconfirmed intent reaches no store removal; the retry reuses one operation") { attempt("intent", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 3600; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = rejected(s);
        const auto staging = md::stagingOf(id), attempt1 = md::attemptKey(id, 1);
        log->with([&]() { log->unavailable = true; });
        untilSeen(*log, id);
        CHECK_EQ(removalsOf(staging) + removalsOf(attempt1), 0);
        CHECK_EQ(passes(id, "first"), 0);
        CHECK(log->opsFor(id).empty());
        log->with([&]() { log->unavailable = false; log->loseIntentFor = id; });  // durable, receipt lost
        untilSeen(*log, id);
        CHECK_EQ(removalsOf(staging) + removalsOf(attempt1), 0);
        CHECK_EQ(log->opsFor(id).size(), 1u);
        CHECK_EQ(log->stateCount(id, dj::IntentState::Ready), 1);
        roundsUntil([&]() { return passes(id, "first") == 1; });
        idle();
        CHECK_FALSE(store->exists(staging));
        CHECK_FALSE(store->exists(attempt1));
        const auto ops = log->opsFor(id);
        CHECK_EQ(ops.size(), 1u);  // the same identity, retried
        if (ops.size() == 1u) {
            CHECK(ops[0].state == dj::IntentState::Committed);
            CHECK(ops[0].operation.action == dj::Action::Cleanup);  // no user deletion behind a rejected upload
            CHECK_EQ(ops[0].operation.requestFingerprint.size(), 64u);
            CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM media_deletions WHERE id::text = $1", ops[0].operation.subjects[0].cycleId), 0);
        }
    }); }

    TEST_CASE("journal cleanup: first pass, a re-upload removed by the sweep, and a key that lands again removed again") { attempt("repeat", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.grantSeconds = 3; p.sweepGraceSeconds = 0; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto u = md::uploaded(s.userA, "junk;size=1000");
        CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("rejected"));
        roundsUntil([&]() { return passes(u.id, "first") == 1; }, kWait);
        CHECK_FALSE(store->exists(u.staging));
        // The grant still works: an upload lands after the first pass. Only the sweep, a separate operation, removes it.
        CHECK(store->clientUpload(u.form, u.staging, md::image(), md::nowEpoch()));
        roundsUntil([&]() { return passes(u.id, "sweep") == 1; });
        idle();
        CHECK_FALSE(store->exists(u.staging));
        CHECK_EQ(log->stateCount(u.id, dj::IntentState::Committed), 2);
        // Late, after everything: the reconciler, journaled.
        store->lateClientWrite(u.staging, md::image());
        CHECK_EQ(md::disposition(u.staging), std::string("late"));
        roundsUntil([&]() { return !store->exists(u.staging); }, kWait);
        md::waitUntil([&]() { return md::lateRow(u.staging) == "removed/reconcile"; });
        md::waitUntil([&]() { return log->stateCount(u.id, dj::IntentState::Committed) == 3; }, kWait);
        // The same key lands again: the earlier confirmed removal must not suppress this one.
        store->lateClientWrite(u.staging, md::image());
        roundsUntil([&]() { return !store->exists(u.staging); }, kWait);
        md::waitUntil([&]() { return log->stateCount(u.id, dj::IntentState::Committed) == 4; }, kWait);
        CHECK_EQ(log->opsFor(u.id).size(), 4u);
        // With the journal down, a late key stays: the reconciler meets the journal and removes nothing.
        idle();
        store->lateClientWrite(u.staging, md::image());
        const auto before = removalsOf(u.staging);
        log->with([&]() { log->unavailable = true; });
        untilSeen(*log, u.id, 2);
        CHECK(store->exists(u.staging));
        CHECK_EQ(removalsOf(u.staging), before);
        log->with([&]() { log->unavailable = false; });
        roundsUntil([&]() { return !store->exists(u.staging); }, kWait);
    }); }

    TEST_CASE("journal cleanup: a lost store reply and a partial failure leave the pass unrecorded; the same operation completes it") { attempt("store", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 3600; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = rejected(s);
        const auto staging = md::stagingOf(id), attempt1 = md::attemptKey(id, 1);
        store->failRemovalOf(attempt1);  // fails, no effect
        roundsUntil([&]() { return !store->exists(staging); });
        idle();
        CHECK_EQ(passes(id, "first"), 0);
        CHECK(store->exists(attempt1));
        CHECK_EQ(log->stateCount(id, dj::IntentState::Ready), 1);
        store->clearRemovalFailures();
        store->loseRemovalReplyOf(attempt1);  // takes effect, the reply is lost
        roundsUntil([&]() { return !store->exists(attempt1); });
        idle();
        CHECK_EQ(passes(id, "first"), 0);
        CHECK_EQ(log->stateCount(id, dj::IntentState::Ready), 1);
        roundsUntil([&]() { return passes(id, "first") == 1; });
        idle();
        CHECK_EQ(log->opsFor(id).size(), 1u);
        CHECK_EQ(log->stateCount(id, dj::IntentState::Committed), 1);
    }); }

    TEST_CASE("journal cleanup: a missing pass record repeats the operation; a missing final acknowledgement is reconciled later") { attempt("ack", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 3600; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = rejected(s);
        M::setCleanupFaultForTest([](const std::string &stage) { return stage == "pass_record"; });
        roundsUntil([&]() { return !store->exists(md::stagingOf(id)); });
        idle();
        CHECK_EQ(passes(id, "first"), 0);
        CHECK_EQ(log->stateCount(id, dj::IntentState::Ready), 1);
        M::setCleanupFaultForTest(nullptr);
        roundsUntil([&]() { return passes(id, "first") == 1; });
        idle();
        CHECK_EQ(log->opsFor(id).size(), 1u);
        CHECK_EQ(log->stateCount(id, dj::IntentState::Committed), 1);

        // Removed and recorded, but the terminal fails: normal selection no longer returns this pass.
        const auto other = rejected(s);
        log->with([&]() { log->failTerminalFor = other; });
        roundsUntil([&]() { return passes(other, "first") == 1; });
        idle();
        CHECK_EQ(log->stateCount(other, dj::IntentState::Ready), 1);
        log->with([&]() { log->failTerminalFor.clear(); });
        const auto before = M::cleanupJournalReport().acknowledged;
        roundsUntil([&]() { return log->stateCount(other, dj::IntentState::Committed) == 1; }, kWait);
        CHECK(M::cleanupJournalReport().acknowledged > before);
        CHECK_EQ(log->opsFor(other).size(), 1u);
    }); }

    TEST_CASE("journal cleanup: a late removal's lost acknowledgement is reconciled from its record and the store") { attempt("late-ack", [&]() {
        // Signed grants have whole-second expiry. A one-second grant can leave almost no setup window after flooring;
        // this case tests late acknowledgement reconciliation, not grant-boundary timing. Keep real expiry enforcement.
        auto store = md::useStore([](M::Policy &p) { p.grantSeconds = 3; p.sweepGraceSeconds = 0; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto u = md::uploaded(s.userA, "junk;size=1000");
        CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("rejected"));
        roundsUntil([&]() { return passes(u.id, "sweep") == 1; });
        idle();
        store->lateClientWrite(u.staging, md::image());
        log->with([&]() { log->failTerminalFor = u.id; });
        roundsUntil([&]() { return !store->exists(u.staging); }, kWait);
        md::waitUntil([&]() { return md::lateRow(u.staging) == "removed/reconcile"; });
        idle();
        CHECK(log->stateCount(u.id, dj::IntentState::Ready) >= 1);
        log->with([&]() { log->failTerminalFor.clear(); });
        roundsUntil([&]() { return log->stateCount(u.id, dj::IntentState::Ready) == 0; }, kWait);
        CHECK_FALSE(store->exists(u.staging));
    }); }

    TEST_CASE("journal cleanup: epoch work and acknowledgement continue beyond one window, including after restart") { attempt("epoch-window", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = rejected(s), key = md::stagingOf(id);
        roundsUntil([&]() { return passes(id, "sweep") == 1; });
        store->lateClientWrite(key, md::image());
        roundsUntil([&]() { return !store->exists(key); });
        const auto zeroId = cleanupId("late|reconcile|" + key + "|epoch=0", "cleanup-operation");
        dj::Operation exemplar;
        log->with([&]() { exemplar = log->entries.at(zeroId).operation; });
        CHECK(exemplar == lateEpoch(exemplar, key, 0));
        // Model seventeen earlier confirmed landings without seventeen rounds of unrelated database work.
        for (int epoch = 1; epoch <= 16; ++epoch) {
            const auto op = lateEpoch(exemplar, key, epoch);
            log->with([&]() {
                log->FakeJournal::ensureIntent(op, [&](dj::IntentState state) { CHECK(state == dj::IntentState::Ready); });
                log->FakeJournal::settle(op, dj::EffectState::Committed, [&](bool ok) { CHECK(ok); });
            });
        }
        M::instance().configureDeletionJournal(std::make_shared<ReopenedJournal>(log));
        log->with([&]() { log->failTerminalFor = id; });
        store->lateClientWrite(key, md::image());
        const auto before = removalsOf(key), exhausted = M::cleanupJournalReport().exhausted;
        const int seen = log->seenFor(id);
        round();
        CHECK(store->exists(key));
        CHECK_EQ(removalsOf(key), before);
        CHECK(M::cleanupJournalReport().exhausted > exhausted);
        // One work window, one acknowledgement window, and the two pass acknowledgements at most.
        CHECK(log->seenFor(id) - seen <= 34);
        const bool progressed = withinRounds([&]() { return !store->exists(key); });
        CHECK(progressed);
        if (!progressed) return;
        const auto pending = lateEpoch(exemplar, key, 17).id;
        log->with([&]() { CHECK(log->entries.at(pending).state == dj::IntentState::Ready); log->failTerminalFor.clear(); });
        const auto removed = removalsOf(key);
        M::instance().configureDeletionJournal(std::make_shared<ReopenedJournal>(log));
        CHECK(withinRounds([&]() {
            bool committed = false;
            log->with([&]() { committed = log->entries.at(pending).state == dj::IntentState::Committed; });
            return committed;
        }));
        CHECK_EQ(removalsOf(key), removed); // recovery beyond epoch 15 is confirm-only
        store->lateClientWrite(key, md::image());
        roundsUntil([&]() { return !store->exists(key); });
        log->with([&]() { CHECK(log->entries.at(lateEpoch(exemplar, key, 18).id).state == dj::IntentState::Committed); });
    }); }

    TEST_CASE("journal cleanup: final acknowledgements recover across both source orders without removing again") { attempt("cross-source", [&]() {
        for (bool resumeFirst : {true, false}) {
            auto store = md::useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            auto log = std::make_shared<CheckedJournal>();
            Configured on(log);
            const auto s = md::space();
            const auto u = md::uploaded(s.userA, md::image());
            store->holdImageInfo = true;
            auto held = md::startComplete(s.userA, u.id);
            md::waitUntil([&]() { return store->heldCount() == 1u; });
            store->holdImageInfo = false;
            store->lateClientWrite(u.staging, "junk;size=1000");
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("rejected"));
            const auto key = md::attemptKey(u.id, 1);
            if (resumeFirst) {
                store->releaseHeld();
                CHECK_EQ(md::stateOf(held->wait()), std::string("rejected"));
                md::waitUntil([&]() { return md::lateRow(key) == "removed/attempt_resume"; });
                roundsUntil([&]() { return passes(u.id, "sweep") == 1; });
                log->with([&]() { log->failTerminalFor = u.id; });
                store->lateClientWrite(key, md::image());
                roundsUntil([&]() { return !store->exists(key); });
            } else {
                roundsUntil([&]() { return passes(u.id, "sweep") == 1; });
                store->lateClientWrite(key, md::image());
                roundsUntil([&]() { return md::lateRow(key) == "removed/reconcile"; });
                log->with([&]() { log->failTerminalFor = u.id; });
                store->lateClientWrite(key, md::image());
                store->releaseHeld();
                CHECK_EQ(md::stateOf(held->wait()), std::string("rejected"));
                md::waitUntil([&]() { return !store->exists(key); });
            }
            idle();
            CHECK_EQ(md::lateRow(key), resumeFirst ? std::string("removed/attempt_resume") : std::string("removed/reconcile"));
            CHECK_EQ(log->stateCount(u.id, dj::IntentState::Ready), 1);
            const auto before = removalsOf(key);
            log->with([&]() { log->failTerminalFor.clear(); });
            CHECK(withinRounds([&]() { return log->stateCount(u.id, dj::IntentState::Ready) == 0; }));
            CHECK_EQ(removalsOf(key), before);
        }
    }); }

    TEST_CASE("journal cleanup: a key reclassified keep or unknown while its intent waits is not removed") { attempt("reclassify", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 0; });
        auto log = std::make_shared<PausedJournal>();
        Configured on(log);
        const auto s = md::space();
        // Keep: while the leftovers pass's intent waits, the database makes the losing attempt the winner. The pass's key set is no
        // longer the one its intent names, so nothing is removed. No other key turns late meanwhile: the plan has no sweep yet.
        const auto id = readyWithLoser(s);
        const auto loser = md::attemptKey(id, 1), winner = md::attemptKey(id, 2), staging = md::stagingOf(id);
        log->arm(id);
        md::tickUntil([&]() { return log->isHolding(); }, kWait);
        maintenance("UPDATE media_upload_outcomes SET attempt_no = 1 WHERE media_id = '" + id + "';");
        CHECK_EQ(md::disposition(loser), std::string("keep"));
        const auto refusedBefore = M::cleanupJournalReport().refused;
        log->resume();
        idle();
        CHECK(store->exists(loser));
        CHECK(store->exists(winner));
        CHECK(store->exists(staging));
        CHECK_EQ(removalsOf(loser) + removalsOf(winner) + removalsOf(staging), 0);
        CHECK_EQ(passes(id, "sweep"), 0);
        CHECK(M::cleanupJournalReport().refused > refusedBefore);
        // The facts return: the same unresolved operation completes, and the READY winner stays.
        maintenance("UPDATE media_upload_outcomes SET attempt_no = 2 WHERE media_id = '" + id + "';");
        roundsUntil([&]() { return passes(id, "sweep") == 1; });
        idle();
        CHECK_FALSE(store->exists(loser));
        CHECK_FALSE(store->exists(staging));
        CHECK(store->exists(winner));
        CHECK_EQ(md::served(s.userA, id), md::image());
        CHECK_EQ(log->opsFor(id).size(), 1u);

        // Unknown: the losing copy lands late, and while the reconciler's intent waits its attempt row is gone. Never removed, reported only.
        store->lateClientWrite(loser, md::image());
        CHECK_EQ(md::disposition(loser), std::string("late"));
        log->arm(id);
        md::tickUntil([&]() { return log->isHolding(); }, kWait);
        maintenance("DELETE FROM media_promotion_attempts WHERE media_id = '" + id + "' AND attempt_no = 1;");
        CHECK_EQ(md::disposition(loser), std::string("unknown"));
        const auto requests = removalsOf(loser);
        log->resume();
        idle();
        CHECK(store->exists(loser));
        roundsUntil([&]() { return md::lateRow(loser) == "report_only/reconcile"; }, kWait);
        CHECK(store->exists(loser));
        CHECK_EQ(removalsOf(loser), requests);
        CHECK(store->exists(winner));
    }); }

    TEST_CASE("journal cleanup: a plan gone while its pass intent waits removes nothing (live)") { attempt("live", [&]() {
        auto store = md::useStore();
        auto log = std::make_shared<PausedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto aged = c::newUuid();
        md::env.pool->execSqlSync(
            "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '2 hours', $4, repeat('a', 64), clock_timestamp() - interval '2 hours 10 minutes')",
            aged, s.couple, s.userA, c::newUuid());
        const auto staging = md::stagingOf(aged);
        store->lateClientWrite(staging, md::image());
        log->arm(aged);
        md::tickUntil([&]() { return log->isHolding(); }, kWait);
        CHECK_EQ(md::plans(aged), 1);
        maintenance("DELETE FROM media_cleanup_plans WHERE media_id = '" + aged + "';");
        CHECK_EQ(md::disposition(staging), std::string("live"));
        log->resume();
        idle();
        CHECK(store->exists(staging));
        CHECK_EQ(removalsOf(staging), 0);
        CHECK_EQ(passes(aged, "first") + passes(aged, "sweep"), 0);
    }); }

    TEST_CASE("journal cleanup: leftovers keep the READY winner; a user deletion purges on its exact cycle; an unjournaled one is never purged") { attempt("reasons", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 0; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = readyWithLoser(s);
        roundsUntil([&]() { return passes(id, "sweep") == 1; });
        idle();
        CHECK_FALSE(store->exists(md::attemptKey(id, 1)));
        CHECK_FALSE(store->exists(md::stagingOf(id)));
        CHECK(store->exists(md::attemptKey(id, 2)));
        CHECK_EQ(md::served(s.userA, id), md::image());
        for (const auto &entry : log->opsFor(id)) CHECK(entry.operation.action == dj::Action::Cleanup);

        const auto journaled = d::agedWithObjects(s, s.userA);
        const auto cycle = d::cycleAged(journaled, s.couple, s.userA, "8 days");
        enroll(*log, journaled, cycle);
        const auto bare = d::agedWithObjects(s, s.userA);
        d::cycleAged(bare, s.couple, s.userA, "8 days");
        roundsUntil([&]() { return passes(journaled, "sweep") == 1; }, kWait);
        CHECK_FALSE(store->exists(md::attemptKey(journaled, 1)));
        bool purged = false;
        for (const auto &entry : log->opsFor(journaled)) {
            if (entry.operation.action == dj::Action::Purge) {
                purged = true;
                CHECK_EQ(entry.operation.subjects[0].cycleId, cycle);
                CHECK(entry.state == dj::IntentState::Committed);
            }
        }
        CHECK(purged);
        // No delete intent for this cycle: the conflict has no occupied operation, so stay at epoch zero and back off
        // immediately. Do not churn through new identities to evade the missing enrollment.
        roundsUntil([&]() { return log->seenFor(bare) >= 1; });
        CHECK_EQ(log->seenFor(bare), 1);
        const auto backedOff = M::cleanupJournalReport().backedOff;
        roundsUntil([&]() { return M::cleanupJournalReport().backedOff > backedOff; }, kWait);
        CHECK_EQ(log->seenFor(bare), 1);
        CHECK(store->exists(md::attemptKey(bare, 1)));
        CHECK(store->exists(md::stagingOf(bare)));
        CHECK_EQ(passes(bare, "first") + passes(bare, "sweep"), 0);
        CHECK(log->opsFor(bare).empty());
    }); }

    TEST_CASE("journal cleanup: a changed payload under one identity conflicts; the next epoch does the work, the old intent is left") { attempt("payload", [&]() {
        auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 3600; });
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto id = rejected(s);
        log->with([&]() { log->loseIntentFor = id; });
        roundsUntil([&]() { return log->opsFor(id).size() == 1u; });
        idle();
        CHECK_EQ(passes(id, "first"), 0);
        const auto first = log->opsFor(id)[0].operation.id;
        // As if another payload had been bound to this identity.
        log->with([&]() { log->entries.at(first).operation.requestFingerprint = std::string(64, 'f'); });
        const auto conflicts = M::cleanupJournalReport().conflicts;
        roundsUntil([&]() { return passes(id, "first") == 1; });
        idle();
        CHECK_FALSE(store->exists(md::stagingOf(id)));
        CHECK(M::cleanupJournalReport().conflicts > conflicts);
        CHECK_EQ(log->opsFor(id).size(), 2u);
        log->with([&]() { CHECK(log->entries.at(first).state == dj::IntentState::Ready); });
        CHECK_EQ(log->stateCount(id, dj::IntentState::Committed), 1);
    }); }

    TEST_CASE("journal cleanup: a resumed attempt removes its own key only through the journal; no lock is held at any journal call") { attempt("resume", [&]() {
        auto store = md::useStore();
        auto log = std::make_shared<CheckedJournal>();
        Configured on(log);
        const auto s = md::space();
        const auto w = md::uploaded(s.userA, md::image("image/png", "good"));
        store->holdImageInfo = true;
        auto passing = md::startComplete(s.userA, w.id);
        md::waitUntil([&]() { return store->heldCount() == 1u; });
        store->holdImageInfo = false;
        store->lateClientWrite(w.staging, "junk;size=1000");
        CHECK_EQ(md::stateOf(md::complete(s.userA, w.id)), std::string("rejected"));
        const auto own = md::attemptKey(w.id, 1);
        store->releaseHeld();
        CHECK_EQ(md::stateOf(passing->wait()), std::string("rejected"));
        md::waitUntil([&]() { return !store->exists(own); });
        md::waitUntil([&]() { return md::lateRow(own) == "removed/attempt_resume"; });
        CHECK_EQ(log->stateCount(w.id, dj::IntentState::Committed), 1);

        // With the journal down the resumed attempt removes nothing; its key waits for the sweep.
        const auto v = md::uploaded(s.userA, md::image("image/png", "good"));
        store->holdImageInfo = true;
        auto waiting = md::startComplete(s.userA, v.id);
        md::waitUntil([&]() { return store->heldCount() == 1u; });
        store->holdImageInfo = false;
        store->lateClientWrite(v.staging, "junk;size=1000");
        CHECK_EQ(md::stateOf(md::complete(s.userA, v.id)), std::string("rejected"));
        log->with([&]() { log->unavailable = true; });
        store->releaseHeld();
        CHECK_EQ(md::stateOf(waiting->wait()), std::string("rejected"));
        const auto kept = md::attemptKey(v.id, 1);
        CHECK(store->exists(kept));
        CHECK_EQ(removalsOf(kept), 0);
        log->with([&]() { log->unavailable = false; });
        // No tick ran in this case, so the lock check is meaningful here.
        CHECK_EQ(log->lockedCalls, 0);
    }); }
}

}  // namespace usapptest::journalcleanup
