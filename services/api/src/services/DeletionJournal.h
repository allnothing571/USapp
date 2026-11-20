#pragma once

// D6 J2a: asynchronous, store-independent write-ahead protocol. Services accept injected
// adapters; main still has no production provider (only a startup refusal gate). Call after authorization without a DB
// transaction. A production Effect adapter must classify its stable key and
// revalidate the whole prepared target set inside its own transaction.
#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace usapp::deletionjournal {

// Cleanup (J2c2): object removal that belongs to no user deletion cycle (rejected, abandoned, unattached,
// leftover attempts). Media only. Its cycle id names the cleanup plan, never a user deletion, and needs no
// prior Delete. Repeated passes and late removals are new operations, never retries of a terminal one.
enum class Action { Delete, Undo, Purge, Cleanup };
enum class Kind { Memory, Media };
struct Subject {
    Kind kind;
    std::string targetId, cycleId, intentId;
    auto identity() const { return std::tie(kind, targetId, cycleId, intentId); }
    bool operator==(const Subject &other) const { return identity() == other.identity(); }
};
struct Operation {
    std::string id;
    Action action;
    std::vector<Subject> subjects;
    std::string requestFingerprint = {};  // optional for legacy clients; no raw keys/content
    bool operator==(const Operation &other) const {
        return id == other.id && action == other.action && subjects == other.subjects && requestFingerprint == other.requestFingerprint;
    }
};

inline bool uuidShape(const std::string &id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

inline bool normalize(Operation &operation) {
    if (!uuidShape(operation.id) || operation.subjects.empty() ||
        (operation.action != Action::Delete && operation.action != Action::Undo && operation.action != Action::Purge &&
         operation.action != Action::Cleanup)) return false;
    if (!operation.requestFingerprint.empty()) {
        if (operation.requestFingerprint.size() != 64) return false;
        for (char c : operation.requestFingerprint) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    std::sort(operation.subjects.begin(), operation.subjects.end(),
              [](const Subject &a, const Subject &b) { return a.identity() < b.identity(); });
    std::set<std::string> intents;
    std::set<std::pair<Kind, std::string>> targets;
    for (const auto &subject : operation.subjects) {
        if ((subject.kind != Kind::Memory && subject.kind != Kind::Media) || !uuidShape(subject.targetId) ||
            !uuidShape(subject.cycleId) || !uuidShape(subject.intentId) ||
            !intents.insert(subject.intentId).second || !targets.emplace(subject.kind, subject.targetId).second ||
            (operation.action == Action::Cleanup && subject.kind != Kind::Media)) return false;
    }
    return true;
}

enum class IntentState { Ready, Committed, Aborted, Unknown, Conflict };
enum class EffectState { Committed, ProvenNotApplied, Unknown };
enum class LookupState { Missing, Found, Unknown };
enum class Result { Committed, ProvenNotApplied, Unknown, JournalUnavailable, Conflict, Invalid };

class Journal {
  public:
    virtual ~Journal() = default;
    // Atomic durable acceptance of ALL subjects, bound immutably to operation
    // id; repeated payload is read-only, changed payload is Conflict. Ready
    // must mean persisted, not queued. A lost receipt is Unknown even if saved.
    virtual void ensureIntent(const Operation &, std::function<void(IntentState)>) = 0;
    // Atomic terminal acknowledgement of the SAME batch. true means persisted
    // or the identical terminal state already exists. Never flip a terminal.
    virtual void settle(const Operation &, EffectState, std::function<void(bool)>) = 0;
    // Dynamic cascades must reuse their original frozen batch on retry. Missing
    // is an authoritative lookup result; unsupported/unavailable is Unknown.
    virtual void lookupOperation(const std::string &, std::function<void(LookupState, Operation)> callback) {
        callback(LookupState::Unknown, {});
    }
};

class Effect {
  public:
    virtual ~Effect() = default;
    // Must be idempotent across processes/restarts, not merely this coordinator.
    // Under a stable-key fence: classify -> verify unchanged authorized target
    // set -> apply once -> confirm commit. No journal call inside the DB tx.
    // Changed cascade membership must return ProvenNotApplied WITHOUT applying
    // a subset or adding unjournaled targets. Unknown is not a rollback proof.
    virtual void applyOrClassify(const Operation &, std::function<void(EffectState)>) = 0;
};

class Coordinator {
  public:
    static void run(Operation operation, std::shared_ptr<Journal> journal, std::shared_ptr<Effect> effect,
                    std::function<void(Result)> done) {
        if (!normalize(operation) || !journal || !effect) { done(Result::Invalid); return; }
        auto flight = std::make_shared<Flight>(std::move(operation), std::move(journal), std::move(effect), std::move(done));
        flight->start();
    }

  private:
    // Adapter callbacks for each flight must be serialized by its executor.
    // Shared adapters may support multiple flights. Duplicate callbacks on the
    // same executor are ignored; this class does not promise thread safety.
    struct Flight : std::enable_shared_from_this<Flight> {
        enum class Step { Intent, Effect, Terminal, Done };
        Operation operation;
        std::shared_ptr<Journal> journal;
        std::shared_ptr<Effect> effect;
        std::function<void(Result)> done;
        Step step = Step::Intent;
        Flight(Operation op, std::shared_ptr<Journal> j, std::shared_ptr<Effect> e, std::function<void(Result)> d)
            : operation(std::move(op)), journal(std::move(j)), effect(std::move(e)), done(std::move(d)) {}
        void finish(Result result) {
            if (step == Step::Done) return;
            step = Step::Done;
            auto callback = std::move(done);
            callback(result);
        }
        void start() {
            auto self = shared_from_this();
            try {
                journal->ensureIntent(operation, [self](IntentState state) { self->intentReady(state); });
            } catch (...) {
                if (step == Step::Intent) finish(Result::JournalUnavailable);
            }
        }
        void intentReady(IntentState state) {
            if (step != Step::Intent) return;
            switch (state) {
                case IntentState::Committed: finish(Result::Committed); return;
                case IntentState::Aborted: finish(Result::ProvenNotApplied); return;
                case IntentState::Conflict: finish(Result::Conflict); return;
                case IntentState::Ready: break;
                default: finish(Result::JournalUnavailable); return;
            }
            step = Step::Effect;
            auto self = shared_from_this();
            try {
                effect->applyOrClassify(operation, [self](EffectState result) { self->effectReady(result); });
            } catch (...) {
                if (step == Step::Effect) finish(Result::Unknown);
            }
        }
        void effectReady(EffectState state) {
            if (step != Step::Effect) return;
            if (state != EffectState::Committed && state != EffectState::ProvenNotApplied) {
                finish(Result::Unknown); return;
            }
            step = Step::Terminal;
            auto self = shared_from_this();
            try {
                journal->settle(operation, state, [self, state](bool durable) {
                    if (self->step != Step::Terminal) return;
                    self->finish(!durable ? Result::Unknown : state == EffectState::Committed ?
                                 Result::Committed : Result::ProvenNotApplied);
                });
            } catch (...) {
                if (step == Step::Terminal) finish(Result::Unknown);
            }
        }
    };
};
}  // namespace usapp::deletionjournal
