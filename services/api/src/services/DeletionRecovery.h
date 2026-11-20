#pragma once

// J3a: recovery preparation protocol only. Every outcome keeps serving CLOSED.
// No production Source/release wiring; main has a refusal-only startup check. A hash-chain report alone is
// never an authorization to open a restored database.
#include "services/DeletionJournal.h"
#include <cstdint>
#include <limits>
#include <optional>

namespace usapp::deletionrecovery {

using deletionjournal::Kind;
struct Target {
    Kind kind;
    std::string id;
    auto identity() const { return std::tie(kind, id); }
    bool operator==(const Target &other) const { return identity() == other.identity(); }
};
struct Head {
    std::string journalId;
    uint64_t seq = 0;
    std::string hash;
    bool operator==(const Head &other) const { return journalId == other.journalId && seq == other.seq && hash == other.hash; }
};
// Only Source may attest that these sets came from the complete validated
// snapshot and an independently trusted latest checkpoint. They are NOT an allowlist.
struct Snapshot {
    Head head;
    std::vector<Target> hidden, purgeBarriers;
    std::vector<std::string> unresolved;
};
struct Request {
    // databaseId is an externally pinned restore incarnation, NOT a database
    // name or a token read solely from the restored backup (J4/J5).
    std::string databaseId, attemptId, journalId;
};
struct Fence {
    std::string databaseId, attemptId;
    uint64_t generation = 0;
};
enum class Receipt { Confirmed, Unknown, Conflict };
enum class Outcome { VerifiedClosed, Invalid, Blocked, Unknown, Conflict, HeadChanged };
struct Report {
    Outcome outcome;
    // Even VerifiedClosed is only preparation evidence. J3b/c must implement
    // actual PostgreSQL overlays, startup enforcement and a separate release proof.
    static constexpr bool servingAllowed = false;
};

inline bool validHead(const Head &h) {
    if (!deletionjournal::uuidShape(h.journalId) || h.seq > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) || h.hash.size() != 64) return false;
    for (char c : h.hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return (h.seq == 0) == (h.hash == std::string(64, '0'));
}
inline bool normalize(Snapshot &s) {
    if (!validHead(s.head)) return false;
    for (auto *targets : {&s.hidden, &s.purgeBarriers}) {
        std::sort(targets->begin(), targets->end(), [](const Target &a, const Target &b) { return a.identity() < b.identity(); });
        for (size_t i = 0; i < targets->size(); ++i) {
            const auto &t = (*targets)[i];
            if ((t.kind != Kind::Media && t.kind != Kind::Memory) || !deletionjournal::uuidShape(t.id) ||
                (i && t == (*targets)[i - 1])) return false;
        }
    }
    std::sort(s.unresolved.begin(), s.unresolved.end());
    for (size_t i = 0; i < s.unresolved.size(); ++i)
        if (!deletionjournal::uuidShape(s.unresolved[i]) || (i && s.unresolved[i] == s.unresolved[i - 1])) return false;
    return s.head.seq != 0 || (s.hidden.empty() && s.purgeBarriers.empty() && s.unresolved.empty());
}

class Source {
  public:
    virtual ~Source() = default;
    // Explicitly bind this reader owner to the externally pinned restore request, not just a journal id.
    // Matching syntax/context is not an attestation that an incarnation is currently authorized to serve.
    virtual bool binds(const Request &) const noexcept = 0;
    // Must perform the J1 chain/state validation AND independently pin the
    // latest trusted checkpoint; never deserialize an arbitrary plan as trusted.
    virtual void snapshot(const std::string &journalId, std::function<void(bool, Snapshot)>) = 0;
    virtual void currentHead(const std::string &journalId, std::function<void(bool, Head)>) = 0;
};
class Store {
  public:
    virtual ~Store() = default;
    // Confirmed means the CLOSED marker is durably committed. Every new close
    // invalidates older fences, including in-flight work from another process.
    virtual void close(const Request &, std::function<void(Receipt, Fence)>) = 0;
    // Atomically stage the FULL snapshot (including unresolved/purge barriers)
    // under that fence; never reopen targets just because a row is absent from it.
    virtual void stage(const Fence &, const Snapshot &, std::function<void(Receipt)>) = 0;
    // Read durable effects back. Must cover old idempotency reads and all media,
    // memory, history, proposal and parent-link paths, not just row counts.
    virtual void verify(const Fence &, const Snapshot &, std::function<void(Receipt)>) = 0;
    // Persist verification for this exact fence/head. This method MUST keep the
    // database CLOSED. An unknown commit is not a successful preparation.
    virtual void recordVerified(const Fence &, const Head &, std::function<void(Receipt)>) = 0;
};

// Like the J2 coordinator, callbacks for each flight must be serialized by the
// adapter executor; duplicates on that executor are ignored. No timeout scheduler
// is owned here: adapters must bound their own calls. A stalled callback cannot open serving.
class Coordinator {
    struct Flight : std::enable_shared_from_this<Flight> {
        enum class Step { Close, Snapshot, Stage, Verify, Head, Record, Done };
        Request request;
        Fence fence;
        Snapshot data;
        std::shared_ptr<Source> source;
        std::shared_ptr<Store> store;
        std::optional<Head> expectedHead;
        std::function<bool()> currentAuthority;
        std::function<void(Report)> done;
        Step step = Step::Close;
        void finish(Outcome result) {
            if (step == Step::Done) return;
            step = Step::Done;
            auto cb = std::move(done);
            cb({result});
        }
        bool receipt(Receipt r) {
            if (r == Receipt::Confirmed) return true;
            finish(r == Receipt::Conflict ? Outcome::Conflict : Outcome::Unknown);
            return false;
        }
        template <typename F> void invoke(Step expected, F call) {
            try { call(); } catch (...) { if (step == expected) finish(Outcome::Unknown); }
        }
        bool authorityCurrent() {
            if (!currentAuthority || currentAuthority()) return true;
            finish(Outcome::Blocked);
            return false;
        }
        void start() {
            auto self = shared_from_this();
            invoke(Step::Close, [&]() {
                store->close(request, [self](Receipt r, Fence f) {
                    if (self->step != Step::Close || !self->receipt(r)) return;
                    if (f.databaseId != self->request.databaseId || f.attemptId != self->request.attemptId || f.generation == 0) {
                        self->finish(Outcome::Conflict); return;
                    }
                    self->fence = std::move(f);
                    self->readSnapshot();
                });
            });
        }
        void readSnapshot() {
            step = Step::Snapshot;
            if (!source->binds(request)) { finish(Outcome::Blocked); return; }
            auto self = shared_from_this();
            invoke(Step::Snapshot, [&]() {
                source->snapshot(request.journalId, [self](bool trusted, Snapshot value) {
                    if (self->step != Step::Snapshot) return;
                    if (!trusted) { self->finish(Outcome::Blocked); return; }
                    if (!normalize(value) || value.head.journalId != self->request.journalId) { self->finish(Outcome::Invalid); return; }
                    if (self->expectedHead && !(value.head == *self->expectedHead)) {
                        self->finish(Outcome::HeadChanged); return;
                    }
                    self->data = std::move(value);
                    self->stage();
                });
            });
        }
        void stage() {
            step = Step::Stage;
            auto self = shared_from_this();
            invoke(Step::Stage, [&]() {
                if (!authorityCurrent()) return;
                store->stage(fence, data, [self](Receipt r) {
                    if (self->step != Step::Stage || !self->receipt(r)) return;
                    self->verify();
                });
            });
        }
        void verify() {
            step = Step::Verify;
            auto self = shared_from_this();
            invoke(Step::Verify, [&]() {
                store->verify(fence, data, [self](Receipt r) {
                    if (self->step != Step::Verify || !self->receipt(r)) return;
                    self->checkHead();
                });
            });
        }
        void checkHead() {
            step = Step::Head;
            if (!source->binds(request)) { finish(Outcome::Blocked); return; }
            auto self = shared_from_this();
            invoke(Step::Head, [&]() {
                source->currentHead(request.journalId, [self](bool trusted, Head h) {
                    if (self->step != Step::Head) return;
                    if (!trusted || !validHead(h)) { self->finish(Outcome::Blocked); return; }
                    if (!(h == self->data.head)) { self->finish(Outcome::HeadChanged); return; }
                    self->record();
                });
            });
        }
        void record() {
            step = Step::Record;
            auto self = shared_from_this();
            invoke(Step::Record, [&]() {
                if (!authorityCurrent()) return;
                store->recordVerified(fence, data.head, [self](Receipt r) {
                    if (self->step != Step::Record || !self->receipt(r)) return;
                    if (self->authorityCurrent()) self->finish(Outcome::VerifiedClosed);
                });
            });
        }
    };
    static void start(Request request, std::shared_ptr<Source> source, std::shared_ptr<Store> store,
                      std::optional<Head> expectedHead, std::function<void(Report)> done,
                      std::function<bool()> currentAuthority = {}) {
        if (!source || !store || !deletionjournal::uuidShape(request.databaseId) ||
            !deletionjournal::uuidShape(request.attemptId) || !deletionjournal::uuidShape(request.journalId) ||
            (expectedHead && (!validHead(*expectedHead) || expectedHead->journalId != request.journalId))) {
            done({Outcome::Invalid}); return;
        }
        auto flight = std::make_shared<Flight>();
        flight->request = std::move(request); flight->source = std::move(source); flight->store = std::move(store); flight->done = std::move(done);
        flight->expectedHead = std::move(expectedHead);
        flight->currentAuthority = std::move(currentAuthority);
        flight->start();
    }
  public:
    static void run(Request request, std::shared_ptr<Source> source, std::shared_ptr<Store> store,
                    std::function<void(Report)> done) {
        start(std::move(request), std::move(source), std::move(store), std::nullopt, std::move(done));
    }
    // The caller gets this pin from outside both the restored database and the reader being checked.
    // Matching it verifies recovery material only. Serving still requires a separate release decision.
    static void runPinned(Request request, Head expectedHead, std::shared_ptr<Source> source,
                          std::shared_ptr<Store> store, std::function<void(Report)> done,
                          std::function<bool()> currentAuthority = {}) {
        start(std::move(request), std::move(source), std::move(store), std::move(expectedHead),
              std::move(done), std::move(currentAuthority));
    }
};
} // namespace usapp::deletionrecovery
