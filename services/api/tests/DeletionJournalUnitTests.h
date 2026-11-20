#pragma once
#include "TestSupport.h"
#include "services/DeletionJournal.h"
#include <fstream>
#include <map>
#include <stdexcept>

namespace usapptest::deletionjournalunit {
using namespace usapp::deletionjournal;

inline std::string id(unsigned n) {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "00000000-0000-0000-0000-%012x", n);
    return buffer;
}
inline Operation sample(unsigned n = 1, Action action = Action::Delete) {
    return {id(100 + n), action, {{Kind::Memory, id(1), id(10), id(1000 + n)}}};
}

// In-memory fake only: no durability, authorization, DB locks, storage or time.
struct FakeJournal : Journal {
    struct Entry { Operation operation; IntentState state = IntentState::Ready; };
    struct Event { Subject subject; Action action; const char *phase; };
    std::map<std::string, Entry> entries;
    std::vector<Event> events;
    std::vector<std::string> trace;
    std::set<std::string> intents;
    std::set<std::tuple<Kind, std::string, std::string>> cycles;
    bool unavailable = false, loseIntent = false, loseTerminal = false, failTerminal = false, throwIntent = false;
    int ensures = 0, settlements = 0;
    void lookupOperation(const std::string &id, std::function<void(LookupState, Operation)> callback) override {
        if (unavailable) { callback(LookupState::Unknown, {}); return; }
        const auto found = entries.find(id);
        if (found == entries.end()) callback(LookupState::Missing, {});
        else callback(LookupState::Found, found->second.operation);
    }
    void ensureIntent(const Operation &op, std::function<void(IntentState)> callback) override {
        ensures++;
        trace.emplace_back("intent");
        if (throwIntent) throw std::runtime_error("fixed-test-fault");
        if (unavailable) { callback(IntentState::Unknown); return; }
        const auto found = entries.find(op.id);
        if (found != entries.end()) {
            callback(found->second.operation == op ? found->second.state : IntentState::Conflict); return;
        }
        // Validate the whole batch before accepting any row.
        for (const auto &subject : op.subjects) {
            const bool known = cycles.count({subject.kind, subject.targetId, subject.cycleId}) != 0;
            // Cleanup names a plan, not a deletion: it neither needs nor creates a cycle.
            if (intents.count(subject.intentId) || (op.action == Action::Cleanup ? false : op.action == Action::Delete ? known : !known)) {
                callback(IntentState::Conflict); return;
            }
        }
        entries.emplace(op.id, Entry{op});
        for (const auto &subject : op.subjects) {
            intents.insert(subject.intentId);
            if (op.action == Action::Delete) cycles.emplace(subject.kind, subject.targetId, subject.cycleId);
            events.push_back({subject, op.action, "intent"});
        }
        if (loseIntent) { loseIntent = false; callback(IntentState::Unknown); }
        else callback(IntentState::Ready);
    }
    void settle(const Operation &op, EffectState state, std::function<void(bool)> callback) override {
        settlements++;
        trace.emplace_back("terminal");
        auto found = entries.find(op.id);
        if (failTerminal || found == entries.end() || !(found->second.operation == op) ||
            (state != EffectState::Committed && state != EffectState::ProvenNotApplied)) { callback(false); return; }
        const auto target = state == EffectState::Committed ? IntentState::Committed : IntentState::Aborted;
        if (found->second.state != IntentState::Ready) { callback(found->second.state == target); return; }
        found->second.state = target;
        for (const auto &subject : op.subjects) events.push_back({subject, op.action, state == EffectState::Committed ? "committed" : "aborted"});
        if (loseTerminal) { loseTerminal = false; callback(false); }
        else callback(true);
    }
    void exportEvents(const std::string &path) const {
        std::ofstream out(path);
        if (!out) throw std::runtime_error("export-open");
        out << '[';
        bool first = true;
        for (const auto &event : events) {
            if (!first) out << ',';
            first = false;
            out << "{\"action\":\"" << (event.action == Action::Delete ? "delete" : event.action == Action::Undo ? "undo" :
                                         event.action == Action::Purge ? "purge" : "cleanup")
                << "\",\"phase\":\"" << event.phase << "\",\"target_kind\":\"" << (event.subject.kind == Kind::Memory ? "memory" : "media")
                << "\",\"target_id\":\"" << event.subject.targetId << "\",\"cycle_id\":\"" << event.subject.cycleId
                << "\",\"intent_id\":\"" << event.subject.intentId << "\"}";
        }
        out << "]\n";
        if (!out) throw std::runtime_error("export-write");
    }
};

struct FakeEffect : Effect {
    std::shared_ptr<FakeJournal> journal;
    std::map<std::string, Operation> applied;
    std::function<void(EffectState)> waiting;
    Operation pending;
    bool refuse = false, lostCommit = false, deferred = false, throwEffect = false;
    int calls = 0, writes = 0;
    explicit FakeEffect(std::shared_ptr<FakeJournal> j) : journal(std::move(j)) {}
    void applyOrClassify(const Operation &op, std::function<void(EffectState)> callback) override {
        calls++;
        journal->trace.emplace_back("effect");
        if (throwEffect) throw std::runtime_error("effect-unknown");
        CHECK(journal->entries.count(op.id) == 1);
        CHECK(journal->entries.at(op.id).operation == op);
        const auto found = applied.find(op.id);
        if (found != applied.end()) { callback(found->second == op ? EffectState::Committed : EffectState::Unknown); return; }
        if (waiting) { callback(EffectState::Unknown); return; }
        if (refuse) { callback(EffectState::ProvenNotApplied); return; }
        if (deferred) { pending = op; waiting = std::move(callback); return; }
        applied.emplace(op.id, op);
        writes++;
        if (lostCommit) { lostCommit = false; callback(EffectState::Unknown); }
        else callback(EffectState::Committed);
    }
    void finishDeferred() {
        applied.emplace(pending.id, pending);
        writes++;
        auto callback = std::move(waiting);
        waiting = {};
        callback(EffectState::Committed);
        // A broken adapter's duplicate callbacks must not double-answer.
        callback(EffectState::Unknown);
    }
};

struct Fixture {
    std::shared_ptr<FakeJournal> journal = std::make_shared<FakeJournal>();
    std::shared_ptr<FakeEffect> effect = std::make_shared<FakeEffect>(journal);
    Result run(Operation op = sample()) {
        Result answer = Result::Invalid;
        int answers = 0;
        Coordinator::run(std::move(op), journal, effect, [&](Result r) { answer = r; answers++; });
        CHECK_EQ(answers, 1);
        return answer;
    }
};

inline void runDeletionJournalUnitTests() {
    TEST_CASE("journal: durable intent, effect, terminal; no rewrite on replay") {
        Fixture f;
        CHECK(f.run() == Result::Committed);
        CHECK((f.journal->trace == std::vector<std::string>{"intent", "effect", "terminal"}));
        CHECK(f.run() == Result::Committed);
        CHECK_EQ(f.effect->calls, 1);
        CHECK_EQ(f.effect->writes, 1);
        CHECK_EQ(f.journal->events.size(), 2u);
    }
    TEST_CASE("journal: missing dependencies and malformed or duplicate subjects fail before external calls") {
        Fixture f;
        for (unsigned mode = 0; mode != 6; ++mode) {
            auto op = sample();
            if (mode == 0) op.id = "private-invalid";
            if (mode == 1) op.subjects.clear();
            if (mode == 2) op.subjects.push_back(op.subjects[0]);
            if (mode == 3) op.action = static_cast<Action>(99);
            if (mode == 4) op.subjects[0].kind = static_cast<Kind>(99);
            if (mode == 5) op.subjects.push_back({Kind::Memory, id(1), id(11), id(2001)});
            CHECK(f.run(op) == Result::Invalid);
        }
        int count = 0;
        Coordinator::run(sample(), nullptr, f.effect, [&](Result r) { CHECK(r == Result::Invalid); count++; });
        CHECK_EQ(count, 1);
        CHECK_EQ(f.journal->ensures, 0);
        CHECK_EQ(f.effect->calls, 0);
    }
    TEST_CASE("journal: no effect when intent is unavailable or throws") {
        Fixture f;
        f.journal->unavailable = true;
        CHECK(f.run() == Result::JournalUnavailable);
        CHECK_EQ(f.effect->calls, 0);
        f.journal->unavailable = false;
        f.journal->throwIntent = true;
        CHECK(f.run() == Result::JournalUnavailable);
        CHECK_EQ(f.effect->calls, 0);
    }
    TEST_CASE("journal: lost intent receipt; same prepared IDs recover with one effect") {
        Fixture f;
        f.journal->loseIntent = true;
        CHECK(f.run() == Result::JournalUnavailable);
        CHECK_EQ(f.effect->calls, 0);
        CHECK_EQ(f.journal->events.size(), 1u);
        CHECK(f.run() == Result::Committed);
        CHECK_EQ(f.effect->writes, 1);
        CHECK_EQ(f.journal->events.size(), 2u);
    }
    TEST_CASE("journal: uncertain commit stays pending and retry classifies, not reapplies") {
        Fixture f;
        f.effect->lostCommit = true;
        CHECK(f.run() == Result::Unknown);
        CHECK_EQ(f.journal->settlements, 0);
        CHECK(f.journal->entries.at(sample().id).state == IntentState::Ready);
        CHECK(f.run() == Result::Committed);
        CHECK_EQ(f.effect->writes, 1);
        CHECK_EQ(f.effect->calls, 2);
    }
    TEST_CASE("journal: unknown effect exception is never an abort proof") {
        Fixture f;
        f.effect->throwEffect = true;
        CHECK(f.run() == Result::Unknown);
        CHECK_EQ(f.journal->settlements, 0);
        CHECK_EQ(f.journal->events.size(), 1u);
    }
    TEST_CASE("journal: refused revalidation aborts whole batch, never a partial cascade") {
        Fixture f;
        auto op = sample();
        op.subjects.push_back({Kind::Media, id(2), id(20), id(2001)});
        op.subjects.push_back({Kind::Media, id(3), id(30), id(3001)});
        f.effect->refuse = true;
        CHECK(f.run(op) == Result::ProvenNotApplied);
        CHECK_EQ(f.effect->writes, 0);
        CHECK_EQ(f.journal->events.size(), 6u);
        CHECK(f.run(op) == Result::ProvenNotApplied);
        CHECK_EQ(f.effect->calls, 1);
    }
    TEST_CASE("journal: lost terminal receipt and failed acknowledgement never repeat effect") {
        Fixture f;
        f.journal->failTerminal = true;
        CHECK(f.run() == Result::Unknown);
        CHECK_EQ(f.effect->writes, 1);
        f.journal->failTerminal = false;
        f.journal->loseTerminal = true;
        CHECK(f.run() == Result::Unknown);
        CHECK(f.run() == Result::Committed);
        CHECK_EQ(f.effect->writes, 1);
        CHECK_EQ(f.effect->calls, 2);
    }
    TEST_CASE("journal: immutable batch and subject identity; normalized photo order") {
        Fixture f;
        auto op = sample();
        op.subjects.push_back({Kind::Media, id(2), id(20), id(2001)});
        CHECK(f.run(op) == Result::Committed);
        auto reversed = op;
        std::reverse(reversed.subjects.begin(), reversed.subjects.end());
        CHECK(f.run(reversed) == Result::Committed);
        auto changed = op;
        changed.subjects[0].targetId = id(99);
        CHECK(f.run(changed) == Result::Conflict);
        auto reused = sample(2);
        CHECK(f.run(reused) == Result::Conflict);  // another delete on the same cycle
        CHECK_EQ(f.effect->writes, 1);
    }
    TEST_CASE("journal: concurrent retry stays unknown until the first effect settles") {
        Fixture f;
        f.effect->deferred = true;
        int answers = 0;
        Coordinator::run(sample(), f.journal, f.effect, [&](Result r) { CHECK(r == Result::Committed); answers++; });
        CHECK_EQ(answers, 0);
        CHECK(f.run() == Result::Unknown);
        f.effect->finishDeferred();
        CHECK_EQ(answers, 1);
        CHECK_EQ(f.effect->writes, 1);
        CHECK(f.run() == Result::Committed);
        CHECK_EQ(f.effect->writes, 1);
    }
    TEST_CASE("journal: cleanup is media only, needs no deletion cycle, and each pass is its own operation") {
        Fixture f;
        auto memoryCleanup = sample(5, Action::Cleanup);
        CHECK(f.run(memoryCleanup) == Result::Invalid);
        Operation first{id(501), Action::Cleanup, {{Kind::Media, id(5), id(50), id(5001)}}, std::string(64, 'c')};
        CHECK(f.run(first) == Result::Committed);
        Operation sweep{id(502), Action::Cleanup, {{Kind::Media, id(5), id(50), id(5002)}}, std::string(64, 'd')};
        CHECK(f.run(sweep) == Result::Committed);
        CHECK_EQ(f.effect->writes, 2);
        // A terminal operation is never run again: a later removal needs a new identity.
        CHECK(f.run(first) == Result::Committed);
        CHECK_EQ(f.effect->writes, 2);
        auto changed = first;
        changed.requestFingerprint = std::string(64, 'e');
        CHECK(f.run(changed) == Result::Conflict);
    }
    TEST_CASE("journal: terminal states cannot flip; unknown cycle cannot purge") {
        Fixture f;
        CHECK(f.run(sample(2, Action::Purge)) == Result::Conflict);
        CHECK_EQ(f.effect->writes, 0);
        CHECK(f.run() == Result::Committed);
        bool answer = true;
        auto op = sample();
        normalize(op);
        f.journal->settle(op, EffectState::ProvenNotApplied, [&](bool ok) { answer = ok; });
        CHECK(!answer);
    }
    TEST_CASE("journal: a later subject conflict refuses the entire intent batch") {
        Fixture f;
        auto photo = sample();
        photo.subjects = {{Kind::Media, id(2), id(20), id(2001)}};
        CHECK(f.run(photo) == Result::Committed);
        const auto count = f.journal->events.size();
        auto batch = sample(3);
        batch.subjects.push_back({Kind::Media, id(2), id(20), id(2003)});
        CHECK(f.run(batch) == Result::Conflict);
        CHECK_EQ(f.journal->events.size(), count);
        CHECK_EQ(f.journal->entries.count(batch.id), 0u);
        CHECK_EQ(f.effect->calls, 1);
        CHECK_EQ(f.effect->writes, 1);
    }
    TEST_CASE("journal: lookup recovers frozen batch and fingerprint cannot change") {
        Fixture f;
        auto op = sample();
        op.requestFingerprint = std::string(64, 'a');
        f.journal->loseIntent = true;
        CHECK(f.run(op) == Result::JournalUnavailable);
        int calls = 0;
        f.journal->lookupOperation(op.id, [&](LookupState state, Operation found) {
            CHECK(state == LookupState::Found); CHECK(found == op); calls++;
        });
        CHECK_EQ(calls, 1);
        auto changed = op;
        changed.requestFingerprint = std::string(64, 'b');
        CHECK(f.run(changed) == Result::Conflict);
        CHECK_EQ(f.effect->writes, 0);
        CHECK(f.run(op) == Result::Committed);
        CHECK_EQ(f.effect->writes, 1);
        op.requestFingerprint = "invalid";
        CHECK(f.run(op) == Result::Invalid);
    }
}

inline void exportScenario(const std::string &path) {
    Fixture f;
    auto deletion = sample();
    deletion.subjects.push_back({Kind::Media, id(2), id(20), id(2001)});
    deletion.subjects.push_back({Kind::Media, id(3), id(30), id(3001)});
    CHECK(f.run(deletion) == Result::Committed);
    auto undo = deletion;
    undo.id = id(102);
    undo.action = Action::Undo;
    for (auto &s : undo.subjects) s.intentId = id(9000 + static_cast<unsigned>(&s - undo.subjects.data()));
    CHECK(f.run(undo) == Result::Committed);
    auto again = sample(3);
    again.subjects[0].cycleId = id(11);
    CHECK(f.run(again) == Result::Committed);
    auto purge = again;
    purge.id = id(104);
    purge.action = Action::Purge;
    purge.subjects[0].intentId = id(1004);
    f.effect->lostCommit = true;
    CHECK(f.run(purge) == Result::Unknown);
    f.journal->exportEvents(path);
}
}  // namespace usapptest::deletionjournalunit
