#pragma once
#include "MemoryEraseDbTests.h"
#include "DeletionJournalUnitTests.h"
#include "scheduler/LockKey.h"

namespace usapptest::journalpurge {
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace base = usapptest::memorydeletion;
namespace casc = usapptest::memorycascade;
namespace erased = usapptest::memoryerase;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace dj = usapp::deletionjournal;
using Job = usapp::scheduler::MemoryPurgeJob;
using S = usapp::MemoryService;

struct CheckedJournal : usapptest::deletionjournalunit::FakeJournal {
    void unlocked(const dj::Operation &op) {
        CHECK(op.action == dj::Action::Purge); CHECK_EQ(op.subjects.size(), 1u);
        const auto key = usapp::scheduler::LockKey::forJob(Job::kJobKey).value();
        const auto rows = md::obs().client->execSqlSync(
            "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype='advisory' AND granted AND "
            "((objsubid=1 AND classid::bigint=(($1::bigint >> 32) & 4294967295) AND objid::bigint=($1::bigint & 4294967295)) "
            "OR (objsubid=2 AND classid=33020 AND objid::bigint=(hashtext($2::text)::bigint & 4294967295)))",
            std::to_string(key), op.subjects[0].targetId);
        CHECK_EQ(rows[0]["n"].as<long long>(), 0);
    }
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        unlocked(op); FakeJournal::ensureIntent(op, std::move(callback));
    }
    void settle(const dj::Operation &op, dj::EffectState state, std::function<void(bool)> callback) override {
        unlocked(op); FakeJournal::settle(op, state, std::move(callback));
    }
};
struct PausedJournal : CheckedJournal {
    std::string focus;
    bool first = true;
    std::function<void(dj::IntentState)> waiting;
    std::shared_ptr<ts::Pending<bool>> paused = std::make_shared<ts::Pending<bool>>();
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        CheckedJournal::ensureIntent(op, [this, op, callback](dj::IntentState state) {
            if (first && op.subjects[0].cycleId == focus && state == dj::IntentState::Ready) {
                first = false; waiting = callback; paused->deliver(true);
            } else callback(state);
        });
    }
    void resume() { auto callback = std::move(waiting); waiting = {}; callback(dj::IntentState::Ready); }
};

// Explicit fake enrollment of this disposable DB's history, not a production
// import protocol. No text, raw key, actor or credential enters the fake log.
inline void enroll(CheckedJournal &log) {
    const auto rows = md::obs().client->execSqlSync(
        "SELECT d.id::text AS cycle, d.memory_id::text AS memory, EXISTS(SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id=d.id) AS undone FROM memory_deletions d ORDER BY d.id");
    for (const auto &row : rows) {
        const auto memory = row["memory"].as<std::string>(), cycle = row["cycle"].as<std::string>();
        for (bool undo : {false, true}) {
            if (undo && !row["undone"].as<bool>()) continue;
            dj::Operation op{c::newUuid(), undo ? dj::Action::Undo : dj::Action::Delete,
                             {{dj::Kind::Memory, memory, cycle, c::newUuid()}}};
            bool ready = false, committed = false;
            log.FakeJournal::ensureIntent(op, [&](dj::IntentState state) { ready = state == dj::IntentState::Ready; });
            CHECK(ready);
            log.FakeJournal::settle(op, dj::EffectState::Committed, [&](bool ok) { committed = ok; }); CHECK(committed);
        }
    }
}
inline std::shared_ptr<Job> registeredJob;
struct Fixture {
    std::shared_ptr<Job> job;
    explicit Fixture(std::shared_ptr<CheckedJournal> log) {
        if (!registeredJob) {
            registeredJob = std::make_shared<Job>(false);
            usapp::scheduler::Scheduler::instance().registerJob(registeredJob);
        }
        job = registeredJob;
        job->setEnabled(false);
        enroll(*log); job->configureJournal(std::move(log));
    }
    ~Fixture() { job->setEnabled(false); }
    void tick(bool enabled = true) {
        // Isolate TEXT erasure: the object cleanup job has no store this round.
        usapp::MediaService::instance().configure(usapp::MediaService::Policy{}, nullptr);
        job->setEnabled(enabled); md::tick();
        md::waitUntil([&]() { return !job->journalBusy(); }, 10000);
    }
};
inline void attempt(const std::function<void()> &body) {
    try { body(); } catch (const std::exception &e) {
        std::fprintf(stderr, "JOURNAL_PURGE_TEST_FAILURE %s\n", md::exceptionCategory(e).c_str()); CHECK(false);
    }
}
inline std::pair<std::string,std::string> expired(const ts::CommittedSpace &s) {
    const auto memory = base::agedMemory(s);
    return {memory, casc::cascadeAged(memory, s.couple, s.userB, "8 days")};
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("journal purge: off writes nothing; unavailable/lost intent cannot erase") { attempt([&]() {
        md::useStore(); const auto s = md::space(); const auto target = expired(s);
        auto log = std::make_shared<CheckedJournal>(); Fixture f(log);
        const auto before = erased::textsOf(target.first); const int calls = log->ensures;
        f.tick(false); CHECK_EQ(log->ensures, calls); CHECK_EQ(erased::purges(target.first), 0);
        log->unavailable = true; f.tick(); CHECK_EQ(erased::textsOf(target.first), before); CHECK_EQ(erased::purges(target.first), 0);
        log->unavailable = false; log->loseIntent = true; f.tick();
        CHECK_EQ(erased::purges(target.first), 0); CHECK_EQ(erased::textsOf(target.first), before);
        f.tick(); CHECK_EQ(erased::purges(target.first), 1); CHECK_EQ(erased::unerasedTexts(target.first), 0);
        CHECK_EQ(erased::unerasedFingerprints(target.first), 0);
    }); }
    TEST_CASE("journal purge: final-ack failure leaves a record; bounded later ticks reconcile absent due row") { attempt([&]() {
        md::useStore(); const auto s = md::space(); const auto target = expired(s);
        auto log = std::make_shared<CheckedJournal>(); Fixture f(log);
        log->failTerminal = true; f.tick(); CHECK_EQ(erased::purges(target.first), 1);
        CHECK(f.job->journalReport().unknown > 0);
        CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM usapp_memory_purge_due() WHERE memory_id=$1::uuid",target.first), 0);
        log->failTerminal = false;
        const auto historical = md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM memory_purges")[0]["n"].as<long long>();
        const auto bound = (historical + Job::kBatch - 1) / Job::kBatch + 1;
        bool confirmed = false;
        long long rounds = 0;
        while (!confirmed && rounds < bound) {
            f.tick(); rounds++;
            for (const auto &entry : log->entries) if (entry.second.operation.action == dj::Action::Purge && entry.second.operation.subjects[0].cycleId == target.second)
                confirmed = entry.second.state == dj::IntentState::Committed;
        }
        std::printf("JOURNAL_RECONCILE_HISTORY=%lld bound=%lld rounds=%lld\n", historical, bound, rounds);
        CHECK_EQ(erased::purges(target.first), 1);
        CHECK(confirmed);
    }); }
    TEST_CASE("journal purge: disable while intent waits prevents erase and leaves intent unresolved") { attempt([&]() {
        md::useStore(); const auto s = md::space(); const auto target = expired(s);
        auto log = std::make_shared<PausedJournal>(); log->focus = target.second; Fixture f(log);
        usapp::MediaService::instance().configure(usapp::MediaService::Policy{}, nullptr);
        f.job->setEnabled(true); md::tick(); CHECK(log->paused->wait().value_or(false)); CHECK(f.job->journalBusy());
        const auto calls = log->ensures; md::tick(); CHECK_EQ(log->ensures, calls);  // busy round does not launch another
        f.job->setEnabled(false); log->resume(); md::waitUntil([&]() { return !f.job->journalBusy(); });
        CHECK_EQ(erased::purges(target.first), 0); CHECK(erased::unerasedTexts(target.first) > 0);
        f.tick(); CHECK_EQ(erased::purges(target.first), 1);
    }); }
    TEST_CASE("journal purge: shared-text condition is rechecked after external intent") { attempt([&]() {
        md::useStore(); const auto s = md::space(); const auto target = expired(s);
        const auto livePhoto = usapptest::mediadeletion::readyPhoto(s.userB);
        auto log = std::make_shared<PausedJournal>(); log->focus = target.second; Fixture f(log);
        usapp::MediaService::instance().configure(usapp::MediaService::Policy{}, nullptr);
        f.job->setEnabled(true); md::tick(); CHECK(log->paused->wait().value_or(false));
        const auto before = erased::textsOf(target.first);
        const auto live = c::newUuid();
        pool->execSqlSync(
            "DO $share$ BEGIN INSERT INTO memories(id,couple_id,creator_id) VALUES ('"+live+"','"+s.couple+"','"+s.userB+"'); "
            "INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint) "
            "SELECT '"+live+"','"+s.couple+"',1,'"+s.userB+"','create',DATE '2026-01-01',text_id,'"+c::newUuid()+"',repeat('a',64) FROM memory_revisions WHERE memory_id='"+target.first+"' AND version=1; "
            "INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES ('"+livePhoto+"','"+s.couple+"','memory','"+live+"','"+s.userB+"'); "
            "INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES ('"+live+"',1,1,'"+livePhoto+"','"+s.couple+"'); END $share$");
        log->resume(); md::waitUntil([&]() { return !f.job->journalBusy(); });
        CHECK_EQ(erased::purges(target.first), 0); CHECK_EQ(erased::textsOf(target.first), before);
        CHECK_EQ(erased::unerasedTexts(live), 1);
        bool pending = false;
        for (const auto &entry : log->entries) if (entry.second.operation.action == dj::Action::Purge && entry.second.operation.subjects[0].cycleId == target.second)
            pending = entry.second.state == dj::IntentState::Ready;
        CHECK(pending);
    }); }
    TEST_CASE("journal purge: open and reversed cycles are not offered; scheduler rollback cannot erase") { attempt([&]() {
        md::useStore(); const auto s = md::space(); const auto open = base::agedMemory(s);
        const auto cycle = casc::cascadeAged(open, s.couple, s.userB, "1 hour");
        CHECK(mm::is(base::undo(s.userB, open, cycle), S::Outcome::Ok));
        const auto stillOpen = base::agedMemory(s); const auto active = casc::cascadeAged(stillOpen, s.couple, s.userB, "1 hour");
        const auto target = expired(s); auto log = std::make_shared<CheckedJournal>(); Fixture f(log);
        f.job->setEnabled(true);
        auto tx = c::acquireTx(pool);
        auto prepared = std::make_shared<ts::Pending<usapp::scheduler::SchedulerJob::Result>>();
        f.job->runLocked(*tx, [prepared](auto result) { prepared->deliver(std::move(result)); });
        const auto result = prepared->wait(); CHECK(result.has_value()); CHECK(result && static_cast<bool>(result->afterCommit));
        tx->rollback(); tx.reset();  // do NOT execute afterCommit on the rollback path
        CHECK_EQ(erased::purges(target.first), 0);
        f.tick(); CHECK_EQ(erased::purges(target.first), 1);
        CHECK_EQ(erased::purges(open), 0); CHECK_EQ(erased::purges(stillOpen), 0);
        for (const auto &entry : log->entries) if (entry.second.operation.action == dj::Action::Purge)
            CHECK(entry.second.operation.subjects[0].cycleId != active && entry.second.operation.subjects[0].cycleId != cycle);
    }); }
    TEST_CASE("journal purge: due batch stays 50 and bounded reconciliation wraps past 50 old records") { attempt([&]() {
        md::useStore(); const auto s = md::space(); std::vector<std::pair<std::string,std::string>> targets;
        for (int i=0;i<53;i++) targets.push_back(expired(s));
        auto log = std::make_shared<CheckedJournal>(); Fixture f(log); log->failTerminal = true;
        f.tick(); int erasedCount = 0; for (const auto &target : targets) erasedCount += erased::purges(target.first) == 1;
        CHECK(erasedCount <= Job::kBatch); CHECK(erasedCount > 0);
        f.tick(); for (const auto &target : targets) CHECK_EQ(erased::purges(target.first), 1);
        log->failTerminal = false;
        const auto historical = md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM memory_purges")[0]["n"].as<long long>();
        const auto bound = (historical + Job::kBatch - 1) / Job::kBatch + 1;
        for(long long round=0;round<bound;round++) f.tick();
        for (const auto &target : targets) {
            bool confirmed = false;
            for (const auto &entry : log->entries) if(entry.second.operation.action==dj::Action::Purge && entry.second.operation.subjects[0].cycleId==target.second)
                confirmed=entry.second.state==dj::IntentState::Committed;
            CHECK(confirmed); CHECK_EQ(erased::unerasedTexts(target.first),0); CHECK_EQ(erased::unerasedFingerprints(target.first),0);
        }
    }); }
    usapp::MediaService::instance().configure(usapp::MediaService::Policy{}, nullptr);
}
}  // namespace usapptest::journalpurge
