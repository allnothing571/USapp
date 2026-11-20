#pragma once
#include "TestSupport.h"
#include "services/DeletionRecovery.h"
#include "RecoveryAuthorityFixture.h"
#include <map>
#include <optional>
#include <stdexcept>

namespace usapptest::deletionrecoveryunit {
namespace r = usapp::deletionrecovery;
inline std::string id(unsigned n) {
    char buf[37];
    std::snprintf(buf, sizeof(buf), "00000000-0000-0000-0000-%012x", n);
    return buf;
}
inline r::Snapshot sample() {
    return {{id(1), 8, std::string(64, 'a')}, {{r::Kind::Memory, id(10)}, {r::Kind::Media, id(11)}},
            {{r::Kind::Media, id(12)}}, {id(30)}};
}
struct Source : r::Source {
    r::Snapshot value = sample();
    std::optional<r::Head> head;
    bool trusted = true, headTrusted = true, throws = false, hold = false;
    int snapshots = 0, heads = 0;
    std::function<void(bool, r::Snapshot)> waiting;
    std::vector<std::string> *trace;
    explicit Source(std::vector<std::string> &events) : trace(&events) {}
    bool binds(const r::Request &) const noexcept override { return true; } // Explicit unpinned fake for legacy model cases only.
    void snapshot(const std::string &, std::function<void(bool, r::Snapshot)> cb) override {
        ++snapshots; trace->push_back("snapshot");
        if (throws) throw std::runtime_error("fixed-fault");
        if (hold) { waiting = std::move(cb); return; }
        cb(trusted, value);
        cb(false, {}); // adapter duplicate must not change the completed flight
    }
    void currentHead(const std::string &, std::function<void(bool, r::Head)> cb) override {
        ++heads; trace->push_back("head"); cb(headTrusted, head.value_or(value.head));
    }
    void release() { auto cb = std::move(waiting); waiting = {}; cb(trusted, value); }
};
// Fake durable state: retained across coordinator instances, not a real DB,
// process restart, crash-safe adapter, journal verifier or startup integration.
struct Store : r::Store {
    bool closed = true, corrupt = false, wrongFence = false, throws = false;
    uint64_t generation = 0;
    r::Fence current;
    r::Snapshot staged;
    std::optional<r::Head> verified;
    std::string failAt;
    r::Receipt failure = r::Receipt::Unknown;
    std::map<std::pair<r::Kind, std::string>, bool> quarantine;
    std::vector<std::string> *trace;
    explicit Store(std::vector<std::string> &events) : trace(&events) {}
    bool matches(const r::Fence &f) const {
        return f.databaseId == current.databaseId && f.attemptId == current.attemptId && f.generation == generation;
    }
    r::Receipt result(const std::string &stage) const { return failAt == stage ? failure : r::Receipt::Confirmed; }
    void close(const r::Request &req, std::function<void(r::Receipt, r::Fence)> cb) override {
        trace->push_back("close"); closed = true; verified.reset();
        current = {req.databaseId, req.attemptId, ++generation};
        auto sent = current;
        if (wrongFence) sent.attemptId = id(999);
        if (throws) throw std::runtime_error("fixed-fault");
        cb(result("close"), sent);
    }
    void stage(const r::Fence &f, const r::Snapshot &s, std::function<void(r::Receipt)> cb) override {
        trace->push_back("stage");
        if (!matches(f)) { cb(r::Receipt::Conflict); return; }
        staged = s;
        for (const auto *targets : {&s.hidden, &s.purgeBarriers})
            for (const auto &t : *targets) quarantine[{t.kind, t.id}] = true;
        cb(result("stage")); // unknown receipt can follow a durable write
        cb(r::Receipt::Confirmed);
    }
    void verify(const r::Fence &f, const r::Snapshot &s, std::function<void(r::Receipt)> cb) override {
        trace->push_back("verify");
        if (!matches(f)) { cb(r::Receipt::Conflict); return; }
        if (corrupt || !closed || !(s.head == staged.head) || s.hidden != staged.hidden ||
            s.purgeBarriers != staged.purgeBarriers || s.unresolved != staged.unresolved) { cb(r::Receipt::Unknown); return; }
        cb(result("verify"));
    }
    void recordVerified(const r::Fence &f, const r::Head &h, std::function<void(r::Receipt)> cb) override {
        trace->push_back("record");
        if (!matches(f)) { cb(r::Receipt::Conflict); return; }
        verified = h; cb(result("record"));
    }
};
struct Fixture {
    std::vector<std::string> trace;
    std::shared_ptr<Source> source = std::make_shared<Source>(trace);
    std::shared_ptr<Store> store = std::make_shared<Store>(trace);
    r::Request request{id(2), id(3), id(1)};
    int answers = 0;
    r::Outcome outcome = r::Outcome::Blocked;
    void run(std::optional<r::Head> pin = std::nullopt) {
        const auto done = [this](r::Report report) {
            ++answers; outcome = report.outcome; CHECK_FALSE(report.servingAllowed);
        };
        if (pin) r::Coordinator::runPinned(request, *pin, source, store, done);
        else r::Coordinator::run(request, source, store, done);
    }
};

inline void runDeletionRecoveryUnitTests() {
    TEST_CASE("J3c: signed restore proof is bound to its target and time window") {
        namespace a = r::authority;
        usapptest::recoveryauthority::Signer signer;
        Fixture f;
        const a::TargetBinding target{f.request, std::string(64, '1'), std::string(64, '2')};
        const a::Attestation issued{target, f.source->value.head, 100, 120};
        const auto signature = signer.sign(issued);
        // Known answer generated from an independent Node encoding of the v1 fields.
        constexpr char hex[] = "0123456789abcdef";
        std::string encoded;
        for (const auto byte : signature) {
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        }
        CHECK_EQ(encoded, "7a94d1c630fce24a20ac16baadff8f299cd5243ed25859bf710a45df304233e9d81d"
                          "a81209a8d5d8fd393055dbd1c2dab09858fe703dd7ed0ef02a905d107601");
        const auto attempt = [&](const a::TargetBinding &actual, const a::Attestation &claims,
                                 const a::PublicKey &key, int64_t now) {
            a::prepare(actual, key, claims, signature, f.source, f.store, [&](r::Report report) {
                ++f.answers;
                f.outcome = report.outcome;
                CHECK_FALSE(report.servingAllowed);
            }, [now] { return now; });
        };
        auto wrongKey = signer.publicKey;
        wrongKey[0] ^= 1;
        attempt(target, issued, wrongKey, 110);
        auto otherAttempt = target;
        otherAttempt.request.attemptId = id(98);
        attempt(otherAttempt, issued, signer.publicKey, 110);
        auto alteredCheckpoint = issued;
        alteredCheckpoint.checkpoint.hash = std::string(64, 'e');
        attempt(target, alteredCheckpoint, signer.publicKey, 110);
        attempt(target, issued, signer.publicKey, 99);
        attempt(target, issued, signer.publicKey, 120);
        CHECK_EQ(f.answers, 5);
        CHECK(f.outcome == r::Outcome::Blocked);
        CHECK(f.trace.empty());
        auto longLived = issued;
        longLived.expiresAt = 401;
        CHECK_FALSE(a::signingBytes(longLived).has_value());
        attempt(target, issued, signer.publicKey, 110);
        CHECK_EQ(f.answers, 6);
        CHECK(f.outcome == r::Outcome::VerifiedClosed);
    }
    TEST_CASE("J3c: expiry during the snapshot and clock rollback before recording stay closed") {
        namespace a = r::authority;
        usapptest::recoveryauthority::Signer signer;
        for (const bool expireDuringRead : {true, false}) {
            Fixture f;
            const a::TargetBinding target{f.request, std::string(64, '1'), std::string(64, '2')};
            const a::Attestation issued{target, f.source->value.head, 100, 120};
            f.source->hold = expireDuringRead;
            unsigned reads = 0;
            int64_t now = 110;
            a::prepare(target, signer.publicKey, issued, signer.sign(issued), f.source, f.store, [&](r::Report report) {
                ++f.answers;
                f.outcome = report.outcome;
                CHECK_FALSE(report.servingAllowed);
            }, [&] {
                ++reads;
                // The third read is just before the verification record.
                return !expireDuringRead && reads == 3 ? int64_t{109} : now;
            });
            if (expireDuringRead) {
                CHECK_EQ(f.answers, 0);
                now = 120;
                f.source->release();
                CHECK(f.store->quarantine.empty());
            } else {
                CHECK_FALSE(f.store->quarantine.empty());
            }
            CHECK_EQ(f.answers, 1);
            CHECK(f.outcome == r::Outcome::Blocked);
            CHECK(f.store->closed);
            CHECK_FALSE(f.store->verified.has_value());
        }
    }
    TEST_CASE("J3c: an independently pinned head gates staging and verification") {
        for (const auto outcome : {r::Outcome::VerifiedClosed, r::Outcome::HeadChanged, r::Outcome::Blocked,
                                   r::Outcome::Invalid}) {
            Fixture f;
            auto pin = f.source->value.head;
            if (outcome == r::Outcome::HeadChanged) f.source->value.head.hash = std::string(64, 'b');
            if (outcome == r::Outcome::Blocked) f.source->trusted = false;
            if (outcome == r::Outcome::Invalid) pin.journalId = id(99);
            f.run(pin);
            CHECK_EQ(f.answers, 1);
            CHECK(f.outcome == outcome);
            CHECK_EQ(f.store->verified.has_value(), outcome == r::Outcome::VerifiedClosed);
            if (outcome == r::Outcome::Invalid) CHECK(f.trace.empty());
            else if (outcome != r::Outcome::VerifiedClosed) {
                CHECK(f.trace == std::vector<std::string>({"close", "snapshot"}));
                CHECK(f.store->quarantine.empty());
            }
        }
    }
    TEST_CASE("J3c: a self-consistent older reader cannot prepare a pinned restore") {
        Fixture f;
        const auto pin = f.source->value.head;
        --f.source->value.head.seq;
        f.source->value.head.hash = std::string(64, 'b');
        f.source->head = f.source->value.head;
        f.run(pin);
        CHECK_EQ(f.answers, 1);
        CHECK(f.outcome == r::Outcome::HeadChanged);
        CHECK_EQ(f.source->heads, 0);
        CHECK(f.store->closed);
        CHECK(f.store->quarantine.empty());
        CHECK_FALSE(f.store->verified.has_value());
    }
    TEST_CASE("J3a: close, validate, stage, verify, fresh head, record; never open") {
        Fixture f; f.run();
        CHECK_EQ(f.answers, 1); CHECK(f.outcome == r::Outcome::VerifiedClosed);
        CHECK(f.trace == std::vector<std::string>({"close", "snapshot", "stage", "verify", "head", "record"}));
        CHECK(f.store->closed); CHECK(f.store->verified.has_value());
        CHECK_EQ(f.store->quarantine.size(), 3u);
        CHECK_EQ(f.store->staged.unresolved.size(), 1u);
    }
    TEST_CASE("J3a: uncertain durable steps never progress or grant serving") {
        for (const auto &step : {"close", "stage", "verify", "record"}) {
            Fixture f; f.store->failAt = step; f.run();
            CHECK_EQ(f.answers, 1); CHECK(f.outcome == r::Outcome::Unknown); CHECK(f.store->closed);
            if (std::string(step) == "close") CHECK_EQ(f.source->snapshots, 0);
            if (std::string(step) == "stage") CHECK_EQ(f.source->heads, 0);
            // A lost final receipt can leave a record: the outcome is still unknown and closed.
            if (std::string(step) == "record") CHECK(f.store->verified.has_value());
        }
    }
    TEST_CASE("J3a: source unavailable, exception, malformed snapshot or foreign journal stays closed") {
        for (int variant = 0; variant < 6; ++variant) {
            Fixture f;
            if (variant == 0) f.source->trusted = false;
            if (variant == 1) f.source->throws = true;
            if (variant == 2) f.source->value.head.journalId = id(99);
            if (variant == 3) f.source->value.hidden.push_back(f.source->value.hidden[0]);
            if (variant == 4) f.source->value.purgeBarriers[0].kind = static_cast<r::Kind>(99);
            if (variant == 5) f.source->value.unresolved.push_back(f.source->value.unresolved[0]);
            f.run(); CHECK_EQ(f.answers, 1); CHECK(f.outcome != r::Outcome::VerifiedClosed);
            CHECK(f.store->closed); CHECK(f.store->quarantine.empty()); CHECK_FALSE(f.store->verified.has_value());
        }
    }
    TEST_CASE("J3a: changed head, same-sequence fork, backward head and missing current checkpoint do not seal") {
        for (int variant = 0; variant < 5; ++variant) {
            Fixture f; auto h = f.source->value.head;
            if (variant == 0) ++h.seq;
            if (variant == 1) h.hash = std::string(64, 'b');
            if (variant == 2) --h.seq;
            if (variant == 3) h.journalId = id(88);
            if (variant == 4) f.source->headTrusted = false;
            f.source->head = h; f.run();
            CHECK(f.outcome == (variant == 4 ? r::Outcome::Blocked : r::Outcome::HeadChanged));
            CHECK_FALSE(f.store->verified.has_value()); CHECK(f.store->closed); CHECK_EQ(f.store->quarantine.size(), 3u);
        }
    }
    TEST_CASE("J3a: stale verification and damaged durable effects cannot pass") {
        Fixture f; f.store->verified = sample().head; f.store->corrupt = true; f.run();
        CHECK(f.outcome == r::Outcome::Unknown); CHECK_FALSE(f.store->verified.has_value()); CHECK(f.store->closed);
        Fixture other; other.store->wrongFence = true; other.run();
        CHECK(other.outcome == r::Outcome::Conflict); CHECK_EQ(other.source->snapshots, 0);
    }
    TEST_CASE("J3a: another recovery close fences off an in-flight earlier attempt") {
        Fixture f; f.source->hold = true; f.run(); CHECK_EQ(f.answers, 0); CHECK(f.store->closed);
        auto freshSource = std::make_shared<Source>(f.trace);
        int answers = 0;
        r::Coordinator::run({id(2), id(4), id(1)}, freshSource, f.store, [&](r::Report report) {
            ++answers; CHECK(report.outcome == r::Outcome::VerifiedClosed); CHECK_FALSE(report.servingAllowed);
        });
        CHECK_EQ(answers, 1); f.source->release();
        CHECK_EQ(f.answers, 1); CHECK(f.outcome == r::Outcome::Conflict); CHECK_EQ(f.store->generation, 2u); CHECK(f.store->closed);
    }
    TEST_CASE("J3a: retry after lost stage receipt revalidates source and preserves older quarantine") {
        Fixture f; f.store->failAt = "stage"; f.run();
        CHECK(f.outcome == r::Outcome::Unknown); CHECK_EQ(f.store->quarantine.size(), 3u);
        f.store->failAt.clear(); f.source->value.hidden.clear(); f.request.attemptId = id(5); f.run();
        CHECK(f.outcome == r::Outcome::VerifiedClosed); CHECK_EQ(f.answers, 2); CHECK_EQ(f.source->snapshots, 2);
        CHECK_EQ(f.store->quarantine.size(), 3u); CHECK(f.store->closed);
    }
    TEST_CASE("J3a: no default provider, malformed identity and empty checkpoint never grant serving") {
        Fixture f; f.source.reset(); f.run(); CHECK(f.outcome == r::Outcome::Invalid); CHECK(f.trace.empty());
        Fixture g; g.request.databaseId = "usapp_test"; g.run(); CHECK(g.outcome == r::Outcome::Invalid); CHECK(g.trace.empty());
        Fixture empty; empty.source->value = {{id(1), 0, std::string(64, '0')}, {}, {}, {}}; empty.run();
        CHECK(empty.outcome == r::Outcome::VerifiedClosed); CHECK(empty.store->closed);
        auto bad = sample(); bad.head.seq = 0; bad.head.hash = std::string(64, '0'); CHECK_FALSE(r::normalize(bad));
    }
}
} // namespace usapptest::deletionrecoveryunit
