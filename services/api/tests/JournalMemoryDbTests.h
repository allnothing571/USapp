#pragma once
#include "MemoryDeletionDbTests.h"
#include "MemoryDeleteRequestDbTests.h"
#include "DeletionJournalUnitTests.h"

namespace usapptest::journalmemory {
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace media = usapptest::mediadeletion;
namespace del = usapptest::memorydeletion;
namespace req = usapptest::memorydeleterequest;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace dj = usapp::deletionjournal;
using S = usapp::MemoryService;
using O = S::Outcome;

struct CheckedJournal : usapptest::deletionjournalunit::FakeJournal {
    int lookups = 0;
    void unlocked() {
        CHECK_EQ(md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid IN (33020,33018) AND granted")[0]["n"].as<long long>(), 0);
    }
    void lookupOperation(const std::string &id, std::function<void(dj::LookupState,dj::Operation)> callback) override {
        lookups++; unlocked(); FakeJournal::lookupOperation(id, std::move(callback));
    }
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        unlocked(); FakeJournal::ensureIntent(op, std::move(callback));
    }
    void settle(const dj::Operation &op, dj::EffectState state, std::function<void(bool)> callback) override {
        unlocked(); FakeJournal::settle(op, state, std::move(callback));
    }
};
struct PausedJournal : CheckedJournal {
    bool first = true;
    std::function<void(dj::IntentState)> waiting;
    std::shared_ptr<ts::Pending<bool>> paused = std::make_shared<ts::Pending<bool>>();
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        CheckedJournal::ensureIntent(op, [this, callback](dj::IntentState state) {
            if (first && state == dj::IntentState::Ready) { first = false; waiting = callback; paused->deliver(true); }
            else callback(state);
        });
    }
    void resume() { auto callback = std::move(waiting); waiting = {}; callback(dj::IntentState::Ready); }
};
struct ResetJournal { ~ResetJournal() { S::instance().configureDeletionJournal(nullptr); } };
inline void attempt(const std::function<void()> &body) {
    try { body(); } catch (const std::exception &e) {
        std::fprintf(stderr, "JOURNAL_MEMORY_TEST_FAILURE %s\n", md::exceptionCategory(e).c_str()); CHECK(false);
    }
}
inline std::string fresh(const ts::CommittedSpace &space, const std::vector<std::string> &photos) {
    const auto created = mm::create(space.userA, mm::terms("journal fixture", photos));
    if (!mm::is(created, O::Created)) throw std::runtime_error("create fixture");
    return mm::idOf(created);
}
inline long long links(const std::string &memory) {
    return md::count("SELECT count(*)::bigint AS n FROM memory_deletion_photo_cycles WHERE memory_id=$1::uuid", memory);
}
inline void assertLinkedIds(const dj::Operation &op) {
    for (const auto &subject : op.subjects) if (subject.kind == dj::Kind::Media) {
        const auto rows = md::obs().client->execSqlSync("SELECT media_deletion_id::text AS id FROM memory_deletion_photo_cycles WHERE media_id=$1::uuid AND media_deletion_id=$2::uuid", subject.targetId, subject.cycleId);
        CHECK_EQ(rows.size(), 1u);
    }
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("journal memory: authorization and non-destructive decisions precede external access") { attempt([&]() {
        ResetJournal reset; md::useStore();
        auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(), other = md::space();
        const auto photo = media::readyPhoto(s.userA), memory = fresh(s, {photo});
        CHECK(mm::is(del::erase(s.userB, memory), O::Forbidden));
        CHECK(mm::is(del::erase(other.userA, memory), O::NotFound));
        CHECK(mm::is(del::erase(c::newUuid(), memory), O::SpaceNotFound));
        const auto request = req::request(s.userB, memory);
        CHECK(mm::is(request, O::Created));
        CHECK(mm::is(req::decide(s.userB, memory, req::idOf(request), S::Decision::Approve), O::Forbidden));
        CHECK(mm::is(req::decide(s.userA, memory, req::idOf(request), S::Decision::Reject), O::Ok));
        const auto second = req::request(s.userB, memory);
        CHECK(mm::is(req::decide(s.userB, memory, req::idOf(second), S::Decision::Withdraw), O::Ok));
        CHECK_EQ(log->lookups, 0); CHECK_EQ(log->ensures, 0); CHECK_EQ(del::cycles(memory), 0);
    }); }
    TEST_CASE("journal memory: log unavailable or lost intent receipt makes no SQL cycle; retry keeps all IDs") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
        log->unavailable = true;
        CHECK(mm::is(del::erase(s.userA, memory, key), O::ResultUnknown)); CHECK_EQ(del::cycles(memory), 0);
        log->unavailable = false; log->loseIntent = true;
        CHECK(mm::is(del::erase(s.userA, memory, key), O::ResultUnknown)); CHECK_EQ(del::cycles(memory), 0); CHECK_EQ(media::deletions(p), 0);
        CHECK_EQ(log->entries.size(), 1u); const auto frozen = log->entries.begin()->second.operation;
        CHECK_EQ(frozen.subjects.size(), 2u);
        const auto deleted = del::erase(s.userA, memory, key); CHECK(mm::is(deleted, O::Created));
        CHECK_EQ(del::cycleOf(deleted), frozen.subjects[0].cycleId); assertLinkedIds(frozen);
        CHECK(mm::is(del::erase(s.userA, memory, key), O::Replayed)); CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(media::deletions(p), 1);
        CHECK(log->entries.begin()->second.operation == frozen);
    }); }
    TEST_CASE("journal memory: cascade covers history, leaves independent and partner photos; undo exact batch") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), historical = media::readyPhoto(s.userA), independent = media::readyPhoto(s.userA), partner = media::readyPhoto(s.userB);
        const auto memory = fresh(s, {p, historical, independent});
        CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("new", {p, independent})), O::Created));
        const auto proposal = mm::propose(s.userB, memory, 2, mm::terms("partner", {p, independent, partner}));
        CHECK(mm::is(proposal, O::Created));
        CHECK(mm::is(mm::decide(s.userA, memory, mm::proposalOf(proposal), S::Decision::Approve), O::Ok));
        CHECK(md::is(media::erase(s.userA, independent), usapp::MediaService::Outcome::Created));
        const auto deleted = del::erase(s.userA, memory); CHECK(mm::is(deleted, O::Created));
        CHECK_EQ(log->entries.begin()->second.operation.subjects.size(), 3u); assertLinkedIds(log->entries.begin()->second.operation);
        CHECK_EQ(links(memory), 2); CHECK_EQ(media::deletions(partner), 0); CHECK_EQ(media::deletions(independent), 1);
        CHECK(mm::is(mm::getMemory(s.userB, memory), O::NotFound));
        CHECK(md::is(md::content(s.userB, partner), usapp::MediaService::Outcome::Ok));
        CHECK(md::is(md::content(s.userA, partner), usapp::MediaService::Outcome::NotFound));
        CHECK(mm::is(del::undo(s.userA, memory, del::cycleOf(deleted)), O::Ok));
        CHECK_EQ(media::reversals(p), 1); CHECK_EQ(media::reversals(historical), 1); CHECK_EQ(media::reversals(independent), 0);
        CHECK(md::is(md::content(s.userA, independent), usapp::MediaService::Outcome::NotFound));
        CHECK(mm::is(mm::getMemory(s.userB, memory), O::Ok));
    }); }
    TEST_CASE("journal memory: lost or unavailable terminal acknowledgement replays one committed cascade") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space();
        for (bool lost : {true, false}) {
            const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
            log->loseTerminal = lost; log->failTerminal = !lost;
            CHECK(mm::is(del::erase(s.userA, memory, key), O::ResultUnknown)); CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(links(memory), 1);
            log->failTerminal = false;
            CHECK(mm::is(del::erase(s.userA, memory, key), O::Replayed));
            CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(media::deletions(p), 1); CHECK_EQ(del::audits(memory, "memory.deleted"), 1);
        }
    }); }
    TEST_CASE("journal memory: failed cascade rolls back pending-edit settlement and every cycle") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), extra = media::readyPhoto(s.userB), memory = fresh(s, {p}), key = c::newUuid();
        const auto proposal = mm::propose(s.userB, memory, 1, mm::terms("request", {p, extra})); CHECK(mm::is(proposal, O::Created));
        auto pending = std::make_shared<mm::PendingS>();
        S::instance().deleteMemory(s.userA, memory, key, [pending](S::Status s) { pending->deliver(std::move(s)); }, S::Fault::FailAfterCascade);
        CHECK(mm::is(pending->wait(), O::ResultUnknown));
        md::waitUntil([&]() { return del::cycles(memory) == 0 && media::deletions(p) == 0; });
        CHECK_EQ(links(memory), 0); CHECK_EQ(log->settlements, 0);
        CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM memory_edit_decisions WHERE proposal_id=$1::uuid", mm::proposalOf(proposal)), 0);
        CHECK(mm::is(del::erase(s.userA, memory, key), O::Created));
        CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM memory_edit_decisions WHERE proposal_id=$1::uuid AND outcome='rejected'", mm::proposalOf(proposal)), 1);
        CHECK_EQ(media::deletions(extra), 0); CHECK_EQ(links(memory), 1);
    }); }
    TEST_CASE("journal memory: approval decision and cascade atomic; failed approval retries original batch") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
        const auto request = req::request(s.userB, memory); const auto requestId = req::idOf(request);
        CHECK(mm::is(req::decide(s.userA, memory, requestId, S::Decision::Approve, key, S::Fault::FailDeleteApproval), O::ResultUnknown));
        md::waitUntil([&]() { return req::decisions(memory) == 0 && del::cycles(memory) == 0; });
        CHECK_EQ(media::deletions(p), 0); CHECK_EQ(log->settlements, 0);
        const auto approved = req::decide(s.userA, memory, requestId, S::Decision::Approve, key);
        CHECK(mm::is(approved, O::Ok)); CHECK_EQ(req::statusOf(approved), std::string("approved"));
        CHECK_EQ(req::decisions(memory), 1); CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(links(memory), 1);
        CHECK(mm::is(req::decide(s.userA, memory, requestId, S::Decision::Approve, key), O::Replayed));
        CHECK_EQ(req::decisions(memory), 1); CHECK_EQ(media::deletions(p), 1);
        CHECK(mm::is(del::erase(s.userA, memory, key), O::KeyReused));
    }); }
    TEST_CASE("journal memory: newly bound photo during log wait refuses frozen batch and retry never replaces it") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<PausedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), added = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
        const auto pending = del::startErase(s.userA, memory, key); CHECK(log->paused->wait().value_or(false));
        const auto frozen = log->entries.begin()->second.operation;
        CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("added", {p, added})), O::Created));
        log->resume(); CHECK(mm::is(pending->wait(), O::ResultUnknown));
        CHECK(mm::is(del::erase(s.userA, memory, key), O::ResultUnknown));
        CHECK_EQ(del::cycles(memory), 0); CHECK_EQ(media::deletions(p), 0); CHECK_EQ(media::deletions(added), 0);
        CHECK(log->entries.begin()->second.operation == frozen); CHECK_EQ(log->entries.size(), 1u);
        CHECK_EQ(log->settlements, 0);
        CHECK(mm::is(del::erase(s.userA, memory), O::Created)); CHECK_EQ(links(memory), 2);
    }); }
    TEST_CASE("journal memory: independently deleted photo during intent wait causes no partial memory effect") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<PausedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
        const auto pending = del::startErase(s.userA, memory, key); CHECK(log->paused->wait().value_or(false));
        CHECK(md::is(media::erase(s.userA, p), usapp::MediaService::Outcome::Created));
        log->resume(); CHECK(mm::is(pending->wait(), O::ResultUnknown));
        CHECK_EQ(del::cycles(memory), 0); CHECK_EQ(links(memory), 0); CHECK_EQ(media::deletions(p), 1);
        CHECK_EQ(log->settlements, 0);
    }); }
    TEST_CASE("journal memory: overlapping same-key requests use frozen batch, one decision and one cascade") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<PausedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p}), key = c::newUuid();
        const auto pending = del::startErase(s.userA, memory, key); CHECK(log->paused->wait().value_or(false));
        CHECK(mm::is(del::erase(s.userA, memory, key), O::Created));
        log->resume(); CHECK(mm::is(pending->wait(), O::Replayed));
        CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(media::deletions(p), 1); CHECK_EQ(links(memory), 1); CHECK_EQ(log->events.size(), 4u);
    }); }
    TEST_CASE("journal memory: uncertain undo, re-delete and old undo replay stay cycle scoped") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p});
        const auto first = del::erase(s.userA, memory); const auto cycle = del::cycleOf(first), key = c::newUuid();
        log->loseTerminal = true;
        CHECK(mm::is(del::undo(s.userA, memory, cycle, key), O::ResultUnknown));
        CHECK(mm::is(del::undo(s.userA, memory, cycle, key), O::Replayed));
        CHECK_EQ(media::reversals(p), 1);
        CHECK(mm::is(del::erase(s.userA, memory), O::Created));
        const auto old = del::undo(s.userA, memory, cycle, key); CHECK(mm::is(old, O::Replayed));
        CHECK_EQ(del::stateOf(old), std::string("undone")); CHECK_EQ(del::cycles(memory), 2);
        CHECK(mm::is(mm::getMemory(s.userA, memory), O::NotFound)); CHECK_EQ(media::deletions(p), 2); CHECK_EQ(media::reversals(p), 1);
    }); }
    TEST_CASE("journal memory: empty creator cascade neither re-deletes nor undoes an independent photo") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<CheckedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), memory = fresh(s, {p});
        CHECK(md::is(media::erase(s.userA, p), usapp::MediaService::Outcome::Created));
        const auto deleted = del::erase(s.userA, memory); CHECK(mm::is(deleted, O::Created));
        CHECK_EQ(log->entries.begin()->second.operation.subjects.size(), 1u); CHECK_EQ(links(memory), 0);
        CHECK(mm::is(del::undo(s.userA, memory, del::cycleOf(deleted)), O::Ok));
        CHECK_EQ(media::deletions(p), 1); CHECK_EQ(media::reversals(p), 0);
    }); }
    TEST_CASE("journal memory: same key cannot switch target or switch direct deletion to approval") { attempt([&]() {
        ResetJournal reset; md::useStore(); auto log = std::make_shared<PausedJournal>(); S::instance().configureDeletionJournal(log);
        const auto s = md::space(); const auto p = media::readyPhoto(s.userA), second = media::readyPhoto(s.userA);
        const auto memory = fresh(s, {p}), other = fresh(s, {second}), key = c::newUuid();
        const auto pending = del::startErase(s.userA, memory, key); CHECK(log->paused->wait().value_or(false));
        const auto frozen = log->entries.begin()->second.operation;
        CHECK(mm::is(del::erase(s.userA, other, key), O::KeyReused));
        const auto request = req::request(s.userB, memory);
        CHECK(mm::is(req::decide(s.userA, memory, req::idOf(request), S::Decision::Approve, key), O::KeyReused));
        CHECK_EQ(req::decisions(memory), 0); CHECK_EQ(del::cycles(other), 0);
        CHECK(log->entries.begin()->second.operation == frozen);
        log->resume(); CHECK(mm::is(pending->wait(), O::Created));
        CHECK_EQ(req::decisions(memory), 1); CHECK_EQ(del::cycles(memory), 1); CHECK_EQ(media::deletions(second), 0);
    }); }
    S::instance().configureDeletionJournal(nullptr);
    usapp::MediaService::instance().configure(usapp::MediaService::Policy{}, nullptr);
}
}  // namespace usapptest::journalmemory
