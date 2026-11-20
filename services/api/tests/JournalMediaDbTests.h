#pragma once
#include "MediaDeletionDbTests.h"
#include "DeletionJournalUnitTests.h"

namespace usapptest::journalmedia {
namespace md = usapptest::media;
namespace d = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace dj = usapp::deletionjournal;
using M = usapp::MediaService;
using O = M::Outcome;

struct CheckedJournal : usapptest::deletionjournalunit::FakeJournal {
    void unlocked() {
        CHECK_EQ(md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33025 AND granted")[0]["n"].as<long long>(), 0);
    }
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        unlocked();
        FakeJournal::ensureIntent(op, std::move(callback));
    }
    void settle(const dj::Operation &op, dj::EffectState state, std::function<void(bool)> callback) override {
        unlocked();
        FakeJournal::settle(op, state, std::move(callback));
    }
};

struct PausedJournal : CheckedJournal {
    std::function<void(dj::IntentState)> waiting;
    std::shared_ptr<ts::Pending<bool>> paused = std::make_shared<ts::Pending<bool>>();
    bool first = true;
    void ensureIntent(const dj::Operation &op, std::function<void(dj::IntentState)> callback) override {
        CheckedJournal::ensureIntent(op, [this, callback](dj::IntentState state) {
            if (first && state == dj::IntentState::Ready) {
                first = false; waiting = callback; paused->deliver(true);
            } else callback(state);
        });
    }
};

struct ResetJournal {
    ~ResetJournal() { M::instance().configureDeletionJournal(nullptr); }
};

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("journal media: unauthorized preparations never enter the external log") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<CheckedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space(), other = md::space();
            const auto photo = d::readyPhoto(s.userA);
            CHECK(md::is(d::erase(s.userB, photo), O::NotFound));
            CHECK(md::is(d::erase(other.userA, photo), O::NotFound));
            CHECK(md::is(d::erase(s.userA, c::newUuid()), O::NotFound));
            CHECK(md::is(d::erase(c::newUuid(), photo), O::SpaceNotFound));
            CHECK_EQ(log->ensures, 0);
            CHECK_EQ(d::deletions(photo), 0);
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("journal media: unavailable and lost intent receipt write no cycle; retry uses the prepared cycle") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<CheckedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space();
            const auto photo = d::readyPhoto(s.userA), key = c::newUuid();
            log->unavailable = true;
            CHECK(md::is(d::erase(s.userA, photo, key), O::ResultUnknown));
            CHECK_EQ(d::deletions(photo), 0);
            log->unavailable = false; log->loseIntent = true;
            CHECK(md::is(d::erase(s.userA, photo, key), O::ResultUnknown));
            CHECK_EQ(d::deletions(photo), 0);
            CHECK_EQ(log->entries.size(), 1u);
            const auto expected = log->entries.begin()->second.operation.subjects[0].cycleId;
            const auto deleted = d::erase(s.userA, photo, key);
            CHECK(md::is(deleted, O::Created));
            CHECK_EQ(d::cycleOf(deleted), expected);
            CHECK(md::is(d::erase(s.userA, photo, key), O::Replayed));
            CHECK_EQ(d::deletions(photo), 1);
            CHECK_EQ(log->events.size(), 2u);
            const auto second = d::readyPhoto(s.userA);
            const auto calls = log->ensures;
            CHECK(md::is(d::erase(s.userA, second, key), O::KeyReused));
            CHECK_EQ(log->ensures, calls);
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("journal media: SQL committed but terminal receipt lost or unavailable never re-deletes") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<CheckedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space();
            for (bool lost : {true, false}) {
                const auto photo = d::readyPhoto(s.userA), key = c::newUuid();
                log->loseTerminal = lost; log->failTerminal = !lost;
                CHECK(md::is(d::erase(s.userA, photo, key), O::ResultUnknown));
                CHECK_EQ(d::deletions(photo), 1);
                log->failTerminal = false;
                const auto replay = d::erase(s.userA, photo, key);
                CHECK(md::is(replay, O::Replayed));
                CHECK_EQ(d::deletions(photo), 1);
                CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'media.deleted' AND target_id::text = $1::text", photo), 1);
            }
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("journal media: exact undo and old-key replay cannot restore a later deletion") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<CheckedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space();
            const auto photo = d::readyPhoto(s.userA);
            const auto first = d::erase(s.userA, photo);
            CHECK(md::is(first, O::Created));
            const auto cycle = d::cycleOf(first), key = c::newUuid();
            const auto before = log->ensures;
            CHECK(md::is(d::undo(s.userB, photo, cycle), O::NotFound));
            CHECK_EQ(log->ensures, before);
            log->loseTerminal = true;
            CHECK(md::is(d::undo(s.userA, photo, cycle, key), O::ResultUnknown));
            CHECK(md::is(d::undo(s.userA, photo, cycle, key), O::Replayed));
            CHECK_EQ(d::reversals(photo), 1);
            const auto later = d::erase(s.userA, photo);
            CHECK(md::is(later, O::Created));
            CHECK(d::cycleOf(later) != cycle);
            const auto old = d::undo(s.userA, photo, cycle, key);
            CHECK(md::is(old, O::Replayed));
            CHECK_EQ(d::stateOf(old), std::string("undone"));
            CHECK(md::is(md::content(s.userA, photo), O::NotFound));
            CHECK_EQ(d::deletions(photo), 2);
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("journal media: a state refusal never falsely acknowledges an abort") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<CheckedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space();
            const auto photo = d::readyPhoto(s.userA);
            CHECK(md::is(d::erase(s.userA, photo), O::Created));
            CHECK(md::is(d::erase(s.userA, photo), O::AlreadyDeleted));
            CHECK_EQ(log->entries.size(), 2u);
            int pending = 0, aborted = 0;
            for (const auto &entry : log->entries) {
                pending += entry.second.state == dj::IntentState::Ready;
                aborted += entry.second.state == dj::IntentState::Aborted;
            }
            CHECK_EQ(pending, 1); CHECK_EQ(aborted, 0);
            CHECK_EQ(d::deletions(photo), 1);
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("journal media: overlapping same-key calls across a paused intent have one SQL effect") {
        try {
            ResetJournal reset;
            md::useStore();
            auto log = std::make_shared<PausedJournal>();
            M::instance().configureDeletionJournal(log);
            const auto s = md::space();
            const auto photo = d::readyPhoto(s.userA), key = c::newUuid();
            auto first = std::make_shared<md::PendingStatus>();
            M::instance().deletePhoto(s.userA, photo, key, [first](M::Status s) { first->deliver(std::move(s)); });
            CHECK(log->paused->wait().value_or(false));
            CHECK_EQ(d::deletions(photo), 0);
            CHECK(md::is(d::erase(s.userA, photo, key), O::Created));
            log->waiting(dj::IntentState::Ready);
            CHECK(md::is(first->wait(), O::Replayed));
            CHECK_EQ(d::deletions(photo), 1);
            CHECK_EQ(log->events.size(), 2u);
        } catch (const std::exception &) { CHECK(false); }
    }
    M::instance().configureDeletionJournal(nullptr);
    M::instance().configure(M::Policy{}, nullptr);
}
}  // namespace usapptest::journalmedia
