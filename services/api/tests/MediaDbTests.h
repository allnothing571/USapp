#pragma once

// P3-1a: the real MediaService, cleanup job and scheduler against FakeObjectStore, on literal usapp_test. No vendor and no
// credential is involved. Isolated committed fixtures (@m3-fixture.test spaces); assertions target this run's ids only.
#include <json/json.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "FakeObjectStore.h"
#include "TaskCompletionDbTests.h"
#include "config/AppConfig.h"
#include "scheduler/Scheduler.h"
#include "scheduler/jobs/MediaCleanupJob.h"
#include "services/MediaService.h"

namespace usapptest::media {

namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using M = usapp::MediaService;
using MO = M::Outcome;
using usapp::scheduler::Scheduler;

struct Env {
    c::ClientPtr pool;
    std::optional<c::Observer> observer;
    std::shared_ptr<FakeObjectStore> store;
    M::Policy policy;
};
inline Env env;

inline const c::Observer &obs() { return *env.observer; }
inline long long nowEpoch() {
    return obs().client->execSqlSync("SELECT floor(extract(epoch FROM clock_timestamp()))::bigint AS v")[0]["v"].as<long long>();
}
inline void waitUntil(const std::function<bool()> &condition, int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("bounded wait expired");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A new fake per case; markers and policy follow it.
inline std::shared_ptr<FakeObjectStore> useStore(const std::function<void(M::Policy &)> &adjust = nullptr) {
    env.store = std::make_shared<FakeObjectStore>();
    env.policy = M::Policy{};
    if (adjust) adjust(env.policy);
    M::instance().configure(env.policy, env.store);
    return env.store;
}
inline void tick() {
    auto done = std::make_shared<ts::Pending<bool>>();
    Scheduler::instance().tickOnce(nullptr, [done]() { done->deliver(true); });
    if (!done->wait()) throw std::runtime_error("tick timed out");
}
// The background round is skipped while an earlier one runs, so a condition is ticked towards.
inline void tickUntil(const std::function<bool()> &condition, int timeoutMs = 6000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        tick();
        if (condition()) return;
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("bounded tick wait expired");
        sleepMs(20);
    }
}
inline ts::CommittedSpace space() {
    ts::CommittedSpace s;
    ts::commitSpace(env.pool, s, true);
    return s;
}

using PendingStatus = ts::Pending<M::Status>;
inline std::optional<M::Status> grant(const std::string &user, const std::string &type = "image/png", int64_t size = 1000,
                                      const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingStatus>();
    M::instance().createUpload(user, key, type, size, [pending](M::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::shared_ptr<PendingStatus> startComplete(const std::string &user, const std::string &id, M::Fault fault = M::Fault::None) {
    auto pending = std::make_shared<PendingStatus>();
    M::instance().complete(user, id, [pending](M::Status s) { pending->deliver(std::move(s)); }, fault);
    return pending;
}
inline std::optional<M::Status> complete(const std::string &user, const std::string &id, M::Fault fault = M::Fault::None) {
    return startComplete(user, id, fault)->wait();
}
inline std::optional<M::Status> facts(const std::string &user, const std::string &id) {
    auto pending = std::make_shared<PendingStatus>();
    M::instance().facts(user, id, [pending](M::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<M::Status> content(const std::string &user, const std::string &id, const std::string &variant = "view") {
    auto pending = std::make_shared<PendingStatus>();
    M::instance().content(user, id, variant, [pending](M::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline bool is(const std::optional<M::Status> &s, MO o) { return s.has_value() && s->outcome == o; }
inline std::string stateOf(const std::optional<M::Status> &s) { return s && s->media.isObject() ? s->media["state"].asString() : std::string("none"); }
inline std::string idOf(const std::optional<M::Status> &s) { return s ? s->media["media_id"].asString() : std::string(); }
inline m::UploadForm formOf(const std::optional<M::Status> &s) {
    m::UploadForm form;
    if (!s || !s->upload.isObject()) throw std::runtime_error("no form");
    form.url = s->upload["url"].asString();
    for (const auto &name : s->upload["fields"].getMemberNames()) form.fields[name] = s->upload["fields"][name].asString();
    return form;
}
inline std::string stagingOf(const std::string &id) {
    return obs().client->execSqlSync("SELECT staging_key FROM media_objects WHERE id = $1::uuid", id)[0]["staging_key"].as<std::string>();
}
// Exactly what the store is told; a valid image of the declared size unless stated.
inline std::string image(const std::string &type = "image/png", const std::string &payload = "a", int64_t size = 1000) {
    return type + ":4032x3024:" + payload + ";size=" + std::to_string(size);
}
// Grant, then upload. Returns the id; the form stays usable.
struct Upload {
    std::string id;
    std::string staging;
    m::UploadForm form;
};
inline Upload uploaded(const std::string &user, const std::string &bytes = image()) {
    const auto made = grant(user);
    if (!is(made, MO::Created)) throw std::runtime_error("no grant outcome=" + std::to_string(made ? static_cast<int>(made->outcome) : -1));
    Upload out{idOf(made), "", formOf(made)};
    out.staging = stagingOf(out.id);
    const auto uploadNow = nowEpoch();
    if (!env.store->clientUpload(out.form, out.staging, bytes, uploadNow)) {
        const auto expiry = out.form.fields.find("x-test-expires");
        if (expiry != out.form.fields.end() && uploadNow >= std::stoll(expiry->second))
            throw std::runtime_error("upload refused: grant expired before fixture upload");
        throw std::runtime_error("upload refused: fake form validation");
    }
    return out;
}
inline long long count(const std::string &sql, const std::string &id) { return c::scalar(obs(), sql, id); }
inline long long attempts(const std::string &id) { return count("SELECT count(*)::bigint AS n FROM media_promotion_attempts WHERE media_id = $1::uuid", id); }
inline long long plans(const std::string &id) { return count("SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE media_id = $1::uuid", id); }
inline long long swept(const std::string &id) { return count("SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid AND kind = 'sweep'", id); }
// What a failed case may print. This helper's own fixed labels pass through, and a database error gives only its SQLSTATE: its text can hold values.
inline std::string exceptionCategory(const std::exception &failure) {
    if (const auto *sql = dynamic_cast<const drogon::orm::SqlError *>(&failure)) return "sql state=" + sql->sqlState();
    if (dynamic_cast<const drogon::orm::Failure *>(&failure)) return "db_failure";
    const std::string text = failure.what();
    for (const char *label : {"bounded wait expired", "bounded tick wait expired", "tick timed out", "no grant outcome=", "no form", "upload refused"}) {
        if (text.rfind(label, 0) == 0u && text.size() < 40u) return "test_helper: " + text;
    }
    return std::string("other: ") + (dynamic_cast<const std::runtime_error *>(&failure) ? "runtime_error" : "exception");
}
inline std::string readyKey(const std::string &id) {
    const auto rows = obs().client->execSqlSync(
        "SELECT a.final_key FROM media_upload_outcomes t JOIN media_promotion_attempts a ON a.media_id = t.media_id AND a.attempt_no = t.attempt_no "
        "WHERE t.media_id = $1::uuid AND t.outcome = 'ready'", id);
    return rows.size() == 1u ? rows[0]["final_key"].as<std::string>() : std::string();
}
inline std::string attemptKey(const std::string &id, int no) {
    const auto rows = obs().client->execSqlSync(
        "SELECT final_key FROM media_promotion_attempts WHERE media_id = $1::uuid AND attempt_no = $2::integer", id, std::to_string(no));
    return rows.size() == 1u ? rows[0]["final_key"].as<std::string>() : std::string();
}
inline std::string disposition(const std::string &key) {
    return obs().client->execSqlSync("SELECT usapp_media_key_disposition($1) AS v", key)[0]["v"].as<std::string>();
}
inline std::string lateRow(const std::string &key) {
    const auto rows = obs().client->execSqlSync("SELECT disposition || '/' || source AS v FROM media_late_objects WHERE object_key = $1", key);
    return rows.size() == 1u ? rows[0]["v"].as<std::string>() : std::string("none");
}
// What the READY reference actually serves.
inline std::string served(const std::string &user, const std::string &id, const std::string &variant = "view") {
    const auto read = content(user, id, variant);
    return is(read, MO::Ok) ? env.store->followRead(read->redirectUrl) : std::string();
}
inline bool clean() { return env.store->secondWritesToFinalKey.load() == 0 && env.store->signedStagingReads.load() == 0; }

// A READY object old enough for the unattached rule, committed directly, without a store: for the database-level races.
inline std::string agedReady(const c::ClientPtr &pool, const ts::CommittedSpace &s, const std::string &uploader = "") {
    const auto id = c::newUuid();
    const auto attemptKeyText = "media/" + s.couple + "/" + id + "/" + c::newUuid() + "/original";
    pool->execSqlSync(
        "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '9 days', $4, repeat('a', 64), clock_timestamp() - interval '9 days 00:10:00')",
        id, s.couple, uploader.empty() ? s.userA : uploader, c::newUuid());
    pool->execSqlSync(
        "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
        "VALUES ($1::uuid, 1, $2::uuid, $3, clock_timestamp() - interval '9 days')", id, s.couple, attemptKeyText);
    pool->execSqlSync(
        "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
        "VALUES ($1::uuid, $2::uuid, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '8 days')", id, s.couple);
    return id;
}

// Since 0030 a binding names a real memory: this one has one committed photo of its own, and is created in one transaction.
// Everything interpolated is a uuid this test generated.
inline std::string anchorMemory(const c::ClientPtr &pool, const ts::CommittedSpace &s) {
    const auto photo = agedReady(pool, s);
    const auto id = c::newUuid();
    pool->execSqlSync(
        "DO $anchor$ DECLARE t uuid; BEGIN "
        "INSERT INTO memories (id, couple_id, creator_id) VALUES ('" + id + "', '" + s.couple + "', '" + s.userA + "'); "
        "INSERT INTO memory_texts (couple_id, title) VALUES ('" + s.couple + "', 'anchor') RETURNING id INTO t; "
        "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint) "
        "VALUES ('" + id + "', '" + s.couple + "', 1, '" + s.userA + "', 'create', DATE '2026-01-01', t, '" + c::newUuid() + "', repeat('a', 64)); "
        "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
        "VALUES ('" + photo + "', '" + s.couple + "', 'memory', '" + id + "', '" + s.userA + "'); "
        "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) "
        "VALUES ('" + id + "', 1, 1, '" + photo + "', '" + s.couple + "'); END $anchor$");
    return id;
}

inline void prepareEnv(const c::ClientPtr &pool, const std::string &dsn) {
    env.pool = pool;
    env.observer = c::Observer::connect(dsn);
    usapp::AppConfig::instance().schedulerKeyNamespace = "usapp.test.p31a";
    Scheduler::instance().configure(false, 15000, "test-a");
    static bool registered = false;
    if (!registered) {
        Scheduler::instance().registerJob(std::make_shared<usapp::scheduler::MediaCleanupJob>());
        registered = true;
    }
}

inline void runMediaDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    prepareEnv(pool, dsn);

    TEST_CASE("P3-1a: with no store everything answers disabled, facts stay readable, and the job writes nothing") {
        try {
            M::instance().configure(M::Policy{}, nullptr);
            const auto s = space();
            CHECK(is(grant(s.userA), MO::Disabled));
            CHECK(is(complete(s.userA, c::newUuid()), MO::Disabled));
            CHECK(is(content(s.userA, c::newUuid()), MO::Disabled));
            CHECK(is(facts(s.userA, c::newUuid()), MO::NotFound));
            const auto before = obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM media_cleanup_plans")[0]["n"].as<long long>();
            tick();
            CHECK_EQ(obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM media_cleanup_plans")[0]["n"].as<long long>(), before);
            CHECK_EQ(obs().client->execSqlSync("SELECT detail->>'message' AS m FROM scheduler_ticks WHERE job_key = 'media.cleanup' ORDER BY id DESC LIMIT 1")[0]["m"].as<std::string>(),
                     std::string("media dispatch=off"));
        } catch (const std::exception &) {
            c::expectThat(false, "media-disabled", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: grant, upload, complete: READY names a write-once key, with facts from the store") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto u = uploaded(s.userA);
            CHECK(u.staging.rfind("staging/" + s.couple + "/" + u.id, 0) == 0u);
            const auto done = complete(s.userA, u.id);
            CHECK(is(done, MO::Ok));
            CHECK_EQ(stateOf(done), std::string("ready"));
            if (done) {
                CHECK_EQ(done->media["width"].asInt(), 4032);
                CHECK_EQ(done->media["height"].asInt(), 3024);
                CHECK(done->upload.isNull());
                // No response carries a key.
                Json::StreamWriterBuilder writer;
                CHECK(Json::writeString(writer, done->media).find("media/") == std::string::npos);
                CHECK(Json::writeString(writer, done->media).find("staging") == std::string::npos);
            }
            const auto key = readyKey(u.id);
            CHECK(key.rfind("media/" + s.couple + "/" + u.id + "/", 0) == 0u);
            CHECK(key != u.staging);
            CHECK_EQ(served(s.userA, u.id), image());
            // A second complete replays: no new attempt, no new copy.
            const auto copies = store->copies.load();
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            CHECK_EQ(store->copies.load(), copies);
            CHECK_EQ(attempts(u.id), 1);
            CHECK(clean());
        } catch (const std::exception &) {
            c::expectThat(false, "media-ready", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: grants are bounded and replay safely; a terminal object never receives a form") {
        try {
            useStore();
            const auto s = space();
            const std::string key = c::newUuid();
            const auto first = grant(s.userA, "image/png", 1000, key);
            CHECK(is(first, MO::Created));
            const auto again = grant(s.userA, "image/png", 1000, key);
            CHECK(is(again, MO::Replayed));
            CHECK_EQ(idOf(again), idOf(first));
            if (first && again) CHECK_EQ(first->upload["expires_at"].asString(), again->upload["expires_at"].asString());
            CHECK(is(grant(s.userA, "image/jpeg", 1000, key), MO::KeyReused));
            CHECK(is(grant(s.userA, "image/png", 2000, key), MO::KeyReused));
            // Terminal: a replay holds no form.
            CHECK(env.store->clientUpload(formOf(first), stagingOf(idOf(first)), image(), nowEpoch()));
            CHECK_EQ(stateOf(complete(s.userA, idOf(first))), std::string("ready"));
            const auto late = grant(s.userA, "image/png", 1000, key);
            CHECK(is(late, MO::Replayed));
            if (late) CHECK(late->upload.isNull());
            // Open grants: the 21st is refused and inserts nothing.
            for (int i = 0; i < 20; ++i) CHECK(is(grant(s.userB), MO::Created));
            CHECK(is(grant(s.userB), MO::LimitExceeded));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_objects WHERE uploader_id = $1::uuid", s.userB), 20);
        } catch (const std::exception &) {
            c::expectThat(false, "media-grants", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: a grant writes only its own staging key, never a final key, and not after expiry") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto u = uploaded(s.userA);
            CHECK(is(complete(s.userA, u.id), MO::Ok));
            const auto key = readyKey(u.id);
            const auto refused = store->refusedClientWrites.load();
            CHECK_FALSE(store->clientUpload(u.form, key, image("image/png", "replaced"), nowEpoch()));
            CHECK_FALSE(store->clientUpload(u.form, stagingOf(idOf(grant(s.userA))), image(), nowEpoch()));
            CHECK_FALSE(store->clientUpload(u.form, u.staging, image(), nowEpoch() + 100000));
            CHECK_EQ(store->refusedClientWrites.load(), refused + 3);
            CHECK_EQ(store->bytesAt(key), image());
        } catch (const std::exception &) {
            c::expectThat(false, "media-grant-scope", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: an overwrite between inspection and finalization, or after READY, cannot change what READY serves") {
        try {
            auto store = useStore();
            const auto s = space();
            // Between inspection and the READY commit, and at every other store step: the staging object changes each time.
            const auto u = uploaded(s.userA, image("image/png", "good"));
            int writes = 0;
            bool afterCopy = false;
            store->hook = [&](const std::string &op, const std::string &) {
                if (afterCopy) store->lateClientWrite(u.staging, image("image/png", "evil-" + std::to_string(++writes)));
                if (op == "copy") afterCopy = true;
            };
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            store->hook = nullptr;
            CHECK(writes >= 3);
            CHECK_EQ(served(s.userA, u.id), image("image/png", "good"));
            const auto crc = obs().client->execSqlSync("SELECT crc64 FROM media_upload_outcomes WHERE media_id = $1::uuid", u.id)[0]["crc64"].as<std::string>();
            CHECK_EQ(crc, usapp::crypto::sha256Hex(image("image/png", "good")).substr(0, 16));

            // After READY, with the grant still valid: the upload is accepted and changes nothing that can be read.
            CHECK(store->clientUpload(u.form, u.staging, image("image/png", "after-ready"), nowEpoch()));
            for (const char *variant : {"thumb", "view", "original"}) CHECK_EQ(served(s.userA, u.id, variant), image("image/png", "good"));

            // Overwritten before the copy by something undecodable: judged on the copy, so rejected, never READY.
            const auto bad = uploaded(s.userA, image("image/png", "fine"));
            store->hook = [&](const std::string &op, const std::string &) {
                if (op == "copy") store->lateClientWrite(bad.staging, "not an image;size=1000");
            };
            const auto rejected = complete(s.userA, bad.id);
            store->hook = nullptr;
            CHECK_EQ(stateOf(rejected), std::string("rejected"));
            if (rejected) CHECK_EQ(rejected->media["reason"].asString(), std::string("not_image"));
            CHECK(is(content(s.userA, bad.id), MO::NotFound));
            CHECK(clean());
        } catch (const std::exception &) {
            c::expectThat(false, "media-overwrite", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: decisions come from the bytes; an absent upload or an outage is retryable and never a rejection") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto made = grant(s.userA);
            const auto id = idOf(made);
            CHECK(is(complete(s.userA, id), MO::NotUploaded));
            CHECK_EQ(attempts(id), 0);
            store->storeDown = 1;
            CHECK(store->clientUpload(formOf(made), stagingOf(id), image(), nowEpoch()));
            CHECK(is(complete(s.userA, id), MO::StoreUnavailable));
            CHECK_EQ(attempts(id), 0);
            CHECK_EQ(stateOf(complete(s.userA, id)), std::string("ready"));

            const auto reasonFor = [&](const std::string &bytes) {
                const auto u = uploaded(s.userA, bytes);
                const auto done = complete(s.userA, u.id);
                CHECK_EQ(stateOf(done), std::string("rejected"));
                CHECK(is(content(s.userA, u.id), MO::NotFound));
                return done ? done->media["reason"].asString() : std::string();
            };
            CHECK_EQ(reasonFor(image("image/png", "a", 999)), std::string("size_mismatch"));
            CHECK_EQ(reasonFor(image("image/jpeg")), std::string("format_mismatch"));
            CHECK_EQ(reasonFor("text renamed;size=1000"), std::string("not_image"));
            CHECK_EQ(reasonFor("image/png:30001x4:a;size=1000"), std::string("dimensions"));
            // Terminal: another try on the same object changes nothing.
            const auto u = uploaded(s.userA, "text;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            store->lateClientWrite(u.staging, image());
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            CHECK_EQ(attempts(u.id), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "media-decisions", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: a copy that succeeds but reports failure leaves a known key, and the retry uses a new one") {
        try {
            auto store = useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            const auto s = space();
            const auto u = uploaded(s.userA);
            store->copyFailButApply = 1;
            CHECK(is(complete(s.userA, u.id), MO::StoreUnavailable));
            const auto lost = attemptKey(u.id, 1);
            CHECK(store->exists(lost));
            CHECK_EQ(disposition(lost), std::string("live"));
            CHECK(is(content(s.userA, u.id), MO::NotFound));
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            const auto key = readyKey(u.id);
            CHECK_EQ(attempts(u.id), 2);
            CHECK(key != lost);
            CHECK_EQ(key, attemptKey(u.id, 2));
            // Cleanup takes the lost copy and staging, and leaves READY alone.
            CHECK_EQ(disposition(lost), std::string("pending_sweep"));
            CHECK_EQ(disposition(key), std::string("keep"));
            sleepMs(1100);
            tickUntil([&]() { return swept(u.id) == 1; });
            CHECK_FALSE(store->exists(lost));
            CHECK_FALSE(store->exists(u.staging));
            CHECK(store->exists(key));
            CHECK_EQ(served(s.userA, u.id), image());
            CHECK(clean());
        } catch (const std::exception &) {
            c::expectThat(false, "media-copy-lost", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: store success followed by database failure recovers by a new attempt; repeated failure ends") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto u = uploaded(s.userA);
            const auto failed = complete(s.userA, u.id, M::Fault::FailBeforeOutcomeCommit);
            CHECK(is(failed, MO::Error));
            CHECK_EQ(readyKey(u.id), std::string());
            CHECK_EQ(plans(u.id), 0);
            CHECK(store->exists(attemptKey(u.id, 1)));
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            CHECK_EQ(readyKey(u.id), attemptKey(u.id, 2));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_upload_outcomes WHERE media_id = $1::uuid", u.id), 1);

            // A loop of failures uses up the attempts and ends, with nothing READY.
            const auto loop = uploaded(s.userA);
            for (int i = 0; i < 5; ++i) {
                store->copyFailButApply = 1;
                CHECK(is(complete(s.userA, loop.id), MO::StoreUnavailable));
            }
            const auto ended = complete(s.userA, loop.id);
            CHECK_EQ(stateOf(ended), std::string("rejected"));
            if (ended) CHECK_EQ(ended->media["reason"].asString(), std::string("attempts_exhausted"));
            CHECK_EQ(attempts(loop.id), 5);
            CHECK(is(content(s.userA, loop.id), MO::NotFound));
        } catch (const std::exception &) {
            c::expectThat(false, "media-db-failure", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: a final key changing during inspection is never READY and never a rejection") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto u = uploaded(s.userA);
            bool once = true;
            store->hook = [&](const std::string &op, const std::string &key) {
                if (op == "image_info" && once) {
                    once = false;
                    store->tamper(key, image("image/png", "tampered"));
                }
            };
            CHECK(is(complete(s.userA, u.id), MO::StoreUnavailable));
            store->hook = nullptr;
            CHECK_EQ(readyKey(u.id), std::string());
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_upload_outcomes WHERE media_id = $1::uuid", u.id), 0);
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            CHECK_EQ(served(s.userA, u.id), image());
        } catch (const std::exception &) {
            c::expectThat(false, "media-tamper", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: concurrent completion: one outcome naming bytes it inspected; the loser's key goes, the winner's stays") {
        try {
            auto store = useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            const auto s = space();
            const auto u = uploaded(s.userA, image("image/png", "one"));
            store->holdCopies = true;
            auto first = startComplete(s.userA, u.id);
            waitUntil([&]() { return store->heldCount() == 1u; });
            // Staging changes between the two: each attempt holds its own version.
            store->lateClientWrite(u.staging, image("image/png", "two"));
            auto second = startComplete(s.userA, u.id);
            waitUntil([&]() { return store->heldCount() == 2u; });
            store->holdCopies = false;
            store->releaseHeld();
            const auto a = first->wait();
            const auto b = second->wait();
            CHECK_EQ(stateOf(a), std::string("ready"));
            CHECK_EQ(stateOf(b), std::string("ready"));
            CHECK_EQ(attempts(u.id), 2);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_upload_outcomes WHERE media_id = $1::uuid", u.id), 1);
            const auto key = readyKey(u.id);
            const auto bytes = store->bytesAt(key);
            CHECK(bytes == image("image/png", "one") || bytes == image("image/png", "two"));
            sleepMs(1100);
            tickUntil([&]() { return swept(u.id) == 1; });
            CHECK_EQ(store->keysUnder("media/" + s.couple + "/" + u.id + "/").size(), 1u);
            CHECK(store->exists(key));
            CHECK_EQ(served(s.userA, u.id), bytes);
            CHECK(clean());
        } catch (const std::exception &) {
            c::expectThat(false, "media-concurrent", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: a delayed attempt resuming after cleanup swept its key never publishes READY, and removes what it wrote") {
        // Where it stopped, for the sanitized note below: this case failed once with no detail (design Section 11.4).
        const char *stage = "copy_held";
        try {
            // Delayed in the copy: it lands after the sweep has already run.
            auto store = useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            const auto s = space();
            const auto u = uploaded(s.userA, image("image/png", "good"));
            store->holdCopies = true;
            auto delayed = startComplete(s.userA, u.id);
            waitUntil([&]() { return store->heldCount() == 1u; });
            store->holdCopies = false;
            // A second completion, on bad bytes, rejects the object: cleanup starts.
            store->lateClientWrite(u.staging, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            CHECK_EQ(plans(u.id), 1);
            sleepMs(1100);
            tickUntil([&]() { return swept(u.id) == 1; });
            const auto dead = attemptKey(u.id, 1);
            CHECK_FALSE(store->exists(dead));
            CHECK_EQ(disposition(dead), std::string("late"));
            store->releaseHeld();
            const auto resumed = delayed->wait();
            CHECK_EQ(stateOf(resumed), std::string("rejected"));
            CHECK_FALSE(is(content(s.userA, u.id), MO::Ok));
            CHECK_EQ(readyKey(u.id), std::string());
            waitUntil([&]() { return !store->exists(dead); });
            // Removed by the attempt itself, or by the reconciler if a background round saw it first.
            waitUntil([&]() { return lateRow(dead).rfind("removed/", 0) == 0u; });

            // Delayed at the inspection, having passed it: its commit finds the plan.
            stage = "inspection_held";
            auto later = useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            const auto v = uploaded(s.userA, image("image/png", "good"));
            later->holdImageInfo = true;
            auto waiting = startComplete(s.userA, v.id);
            waitUntil([&]() { return later->heldCount() == 1u; });
            later->holdImageInfo = false;
            later->lateClientWrite(v.staging, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, v.id)), std::string("rejected"));
            sleepMs(1100);
            stage = "inspection_sweep";
            tickUntil([&]() { return swept(v.id) == 1; });
            later->releaseHeld();
            const auto after = waiting->wait();
            CHECK_FALSE(is(after, MO::Ok) && stateOf(after) == "ready");
            CHECK_EQ(readyKey(v.id), std::string());
            CHECK(clean());

            // Past the inspection with valid bytes, before any sweep: its commit finds the plan, and it removes its own key.
            stage = "third_upload";
            auto third = useStore();
            const auto w = uploaded(s.userA, image("image/png", "good"));
            stage = "third_held";
            third->holdImageInfo = true;
            auto passing = startComplete(s.userA, w.id);
            waitUntil([&]() { return third->heldCount() == 1u; });
            third->holdImageInfo = false;
            third->lateClientWrite(w.staging, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, w.id)), std::string("rejected"));
            CHECK_EQ(third->bytesAt(attemptKey(w.id, 1)), image("image/png", "good"));
            stage = "third_resume";
            third->releaseHeld();
            CHECK_EQ(stateOf(passing->wait()), std::string("rejected"));
            CHECK_EQ(readyKey(w.id), std::string());
            stage = "third_removed";
            waitUntil([&]() { return !third->exists(attemptKey(w.id, 1)); });
            stage = "third_late_row";
            waitUntil([&]() { return lateRow(attemptKey(w.id, 1)) == "removed/attempt_resume"; });
            CHECK(is(content(s.userA, w.id), MO::NotFound));

            // The database refuses it too: direct SQL cannot publish after a plan, or start another attempt.
            stage = "direct_sql";
            bool refused = false;
            try {
                pool->execSqlSync("INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag) "
                                  "VALUES ($1::uuid, $2::uuid, 'ready', 1, 'image/png', 4, 4, 1000, 'e')", v.id, s.couple);
            } catch (const std::exception &) { refused = true; }
            CHECK(refused);
        } catch (const std::exception &failure) {
            c::expectThat(false, "media-delayed", std::string("stage=") + stage + " category=" + exceptionCategory(failure));
        } catch (...) {
            c::expectThat(false, "media-delayed", std::string("stage=") + stage + " category=non_std_exception");
        }
    }

    TEST_CASE("P3-1a: late final objects with no one to resume are found by the reconciler; unknown keys are only reported") {
        try {
            auto store = useStore([](M::Policy &p) { p.grantSeconds = 1; p.sweepGraceSeconds = 0; });
            const auto s = space();
            // The copy reported failure and lands only after the sweep; the attempt that sent it is long gone.
            const auto u = uploaded(s.userA, image("image/png", "good"));
            store->copyFailThenLandLater = 1;
            CHECK(is(complete(s.userA, u.id), MO::StoreUnavailable));
            const auto late = attemptKey(u.id, 1);
            CHECK_FALSE(store->exists(late));
            store->lateClientWrite(u.staging, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            sleepMs(1100);
            tickUntil([&]() { return swept(u.id) == 1; });
            store->landLate();
            CHECK(store->exists(late));
            CHECK_EQ(disposition(late), std::string("late"));
            CHECK(is(content(s.userA, u.id), MO::NotFound));
            tickUntil([&]() { return !store->exists(late); });
            waitUntil([&]() { return lateRow(late) == "removed/reconcile"; });

            // A READY object with a late losing copy, and a process that stopped: the winner is untouched.
            const auto w = uploaded(s.userA, image("image/png", "w"));
            store->dropCopyCallback = true;
            auto stopped = startComplete(s.userA, w.id);  // never answers
            waitUntil([&]() { return attempts(w.id) == 1; });
            waitUntil([&]() { return store->copies.load() >= 3; });
            store->dropCopyCallback = false;
            sleepMs(30);
            CHECK_EQ(stateOf(complete(s.userA, w.id)), std::string("ready"));
            const auto winner = readyKey(w.id);
            sleepMs(1100);
            tickUntil([&]() { return swept(w.id) == 1; });
            store->landLate();
            const auto loser = attemptKey(w.id, 1);
            CHECK(store->exists(loser));
            tickUntil([&]() { return !store->exists(loser); });
            CHECK(store->exists(winner));
            CHECK_EQ(served(s.userA, w.id), image("image/png", "w"));

            // A key no row names: reported once, never removed. A live upload is left alone.
            const std::string stray = "media/" + s.couple + "/" + c::newUuid() + "/" + c::newUuid() + "/original";
            store->tamper(stray, "x");
            const auto live = uploaded(s.userA);
            tickUntil([&]() { return lateRow(stray) == "report_only/reconcile"; });
            CHECK(store->exists(stray));
            CHECK(store->exists(live.staging));
            // Direct SQL cannot call a live or READY key removed.
            for (const auto &key : {live.staging, winner}) {
                bool refused = false;
                try {
                    pool->execSqlSync("INSERT INTO media_late_objects (object_key, disposition, source) VALUES ($1, 'removed', 'reconcile')", key);
                } catch (const std::exception &) { refused = true; }
                CHECK(refused);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "media-late", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: cleanup of a rejected object while its grant is valid: first pass, a late upload, the sweep; never promotable") {
        try {
            auto store = useStore([](M::Policy &p) { p.grantSeconds = 2; p.sweepGraceSeconds = 0; });
            const auto s = space();
            const auto u = uploaded(s.userA, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            tickUntil([&]() {
                return count("SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid AND kind = 'first'", u.id) == 1;
            });
            CHECK_FALSE(store->exists(u.staging));
            CHECK_EQ(swept(u.id), 0);
            // The grant still works, and an upload lands after the delete. It cannot become anything.
            CHECK(store->clientUpload(u.form, u.staging, image(), nowEpoch()));
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            CHECK_EQ(attempts(u.id), 1);
            CHECK(is(content(s.userA, u.id), MO::NotFound));
            sleepMs(2100);
            tickUntil([&]() { return swept(u.id) == 1; });
            CHECK_FALSE(store->exists(u.staging));
            // After the sweep (a request in flight past every window): the reconciler finds it. Never readable meanwhile.
            store->lateClientWrite(u.staging, image());
            CHECK_EQ(disposition(u.staging), std::string("late"));
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("rejected"));
            tickUntil([&]() { return !store->exists(u.staging); });
            waitUntil([&]() { return lateRow(u.staging) == "removed/reconcile"; });

            // Abandoned: never completed, past every window. Plan, passes, gone.
            const auto aged = c::newUuid();
            pool->execSqlSync(
                "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
                "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '2 hours', $4, repeat('a', 64), clock_timestamp() - interval '2 hours 10 minutes')",
                aged, s.couple, s.userA, c::newUuid());
            const auto agedStaging = stagingOf(aged);
            store->lateClientWrite(agedStaging, image());
            tickUntil([&]() { return swept(aged) == 1; });
            CHECK_FALSE(store->exists(agedStaging));
            CHECK_EQ(obs().client->execSqlSync("SELECT reason FROM media_cleanup_plans WHERE media_id = $1::uuid", aged)[0]["reason"].as<std::string>(),
                     std::string("abandoned"));
            CHECK(is(complete(s.userA, aged), MO::GrantExpired));
            // Store failure: a pass is not recorded, and repeats.
            const auto again = uploaded(s.userA, "junk;size=1000");
            CHECK_EQ(stateOf(complete(s.userA, again.id)), std::string("rejected"));
            store->storeDown = 1;
            tick();
            tickUntil([&]() {
                return count("SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid", again.id) >= 1;
            });
            CHECK_FALSE(store->exists(again.staging));
        } catch (const std::exception &) {
            c::expectThat(false, "media-cleanup", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: attachment racing cleanup: in either order, a bound object is never planned and a planned one never bound") {
        try {
            useStore();
            const auto s = space();
            const std::string blocked =
                "SELECT count(*)::bigint AS n FROM pg_stat_activity a WHERE cardinality(pg_blocking_pids(a.pid)) > 0 AND a.query LIKE $1";
            const auto anchor = anchorMemory(pool, s);
            const auto bindSql = std::string(
                "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ($1::uuid, $2::uuid, 'memory', $4::uuid, $3::uuid)");
            const auto planSql = std::string(
                "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) VALUES ($1::uuid, 'everything', 'unattached', clock_timestamp())");

            // Attachment first: it holds the lock; the plan waits, then refuses.
            const auto first = agedReady(pool, s);
            auto attach = c::acquireTx(pool);
            CHECK(c::exec(attach, bindSql, first, s.couple, s.userA, anchor).ok);
            auto planning = std::make_shared<ts::Pending<bool>>();
            pool->execSqlAsync(planSql, [planning](const drogon::orm::Result &) { planning->deliver(true); },
                               [planning](const drogon::orm::DrogonDbException &) { planning->deliver(false); }, first);
            waitUntil([&]() { return c::scalar(obs(), blocked, "INSERT INTO media_cleanup_plans%") >= 1; });
            CHECK(c::commitTx(attach).value_or(false));
            const auto refusedPlan = planning->wait();
            CHECK(refusedPlan.has_value() && !*refusedPlan);
            CHECK_EQ(plans(first), 0);
            // Still bound, still eligible by age: the job leaves it alone.
            tick();
            CHECK_EQ(plans(first), 0);

            // Plan first: it holds the lock; the attachment waits, then refuses.
            const auto second = agedReady(pool, s);
            auto plan = c::acquireTx(pool);
            CHECK(c::exec(plan, planSql, second).ok);
            auto binding = std::make_shared<ts::Pending<bool>>();
            pool->execSqlAsync(bindSql, [binding](const drogon::orm::Result &) { binding->deliver(true); },
                               [binding](const drogon::orm::DrogonDbException &) { binding->deliver(false); }, second, s.couple, s.userA, anchor);
            waitUntil([&]() { return c::scalar(obs(), blocked, "INSERT INTO media_bindings%") >= 1; });
            CHECK(c::commitTx(plan).value_or(false));
            const auto refusedBinding = binding->wait();
            CHECK(refusedBinding.has_value() && !*refusedBinding);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid", second), 0);

            // Through the job: unattached and old enough is planned; a young READY object is not.
            const auto third = agedReady(pool, s);
            const auto young = uploaded(s.userA);
            CHECK_EQ(stateOf(complete(s.userA, young.id)), std::string("ready"));
            tickUntil([&]() { return plans(third) == 1; });
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE media_id = $1::uuid AND scope = 'everything'", young.id), 0);
            CHECK_EQ(disposition(readyKey(young.id)), std::string("keep"));
        } catch (const std::exception &) {
            c::expectThat(false, "media-attach-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: no database lock is held across a store call") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto u = uploaded(s.userA);
            store->holdCopies = true;
            auto waiting = startComplete(s.userA, u.id);
            waitUntil([&]() { return store->heldCount() == 1u; });
            auto tx = c::acquireTx(pool);
            const auto free = c::exec(tx, "SELECT pg_try_advisory_xact_lock(33018::integer, hashtext($1::text)) AS ok", u.id);
            CHECK(free.ok && (*free.result)[0]["ok"].as<bool>());
            c::rollbackTx(tx);
            store->holdCopies = false;
            store->releaseHeld();
            CHECK_EQ(stateOf(waiting->wait()), std::string("ready"));
        } catch (const std::exception &) {
            c::expectThat(false, "media-no-lock", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1a: same-couple isolation at every step, with no store call for a foreign request") {
        try {
            auto store = useStore();
            const auto s = space();
            const auto other = space();
            const auto u = uploaded(s.userA);
            const auto calls = store->calls.load();
            CHECK(is(complete(other.userA, u.id), MO::NotFound));
            CHECK(is(facts(other.userA, u.id), MO::NotFound));
            CHECK(is(content(other.userA, u.id), MO::NotFound));
            // The partner can neither complete the uploader's object nor see it before it is attached.
            CHECK(is(complete(s.userB, u.id), MO::NotFound));
            CHECK_EQ(store->calls.load(), calls);
            CHECK_EQ(attempts(u.id), 0);
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            CHECK(is(facts(s.userB, u.id), MO::NotFound));
            CHECK(is(content(s.userB, u.id), MO::NotFound));
            CHECK_EQ(served(s.userA, u.id), image());
            // Another couple's object cannot be bound, by anyone, even by direct SQL.
            for (const auto &couple : {other.couple}) {
                bool refused = false;
                try {
                    pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                                      "VALUES ($1::uuid, $2::uuid, 'memory', $4::uuid, $3::uuid)", u.id, couple, other.userA, anchorMemory(pool, other));
                } catch (const std::exception &) { refused = true; }
                CHECK(refused);
            }
            const auto anchor = anchorMemory(pool, s);
            pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                              "VALUES ($1::uuid, $2::uuid, 'memory', $4::uuid, $3::uuid)", u.id, s.couple, s.userA, anchor);
            // Bound: the partner reads the same bytes. Another couple still gets nothing. One home only.
            CHECK_EQ(stateOf(facts(s.userB, u.id)), std::string("ready"));
            CHECK_EQ(served(s.userB, u.id), image());
            CHECK(is(content(other.userA, u.id), MO::NotFound));
            bool twice = false;
            try {
                pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                                  "VALUES ($1::uuid, $2::uuid, 'memory', $4::uuid, $3::uuid)", u.id, s.couple, s.userA, anchor);
            } catch (const std::exception &) { twice = true; }
            CHECK(twice);
            // A signed URL changes nothing if altered: the transform and key are bound to it.
            const auto read = content(s.userA, u.id, "thumb");
            if (read) {
                auto altered = read->redirectUrl;
                altered.replace(altered.find("variant=thumb"), 13, "variant=view!");
                CHECK_EQ(store->followRead(altered), std::string());
                CHECK_EQ(store->followRead(read->redirectUrl), image());
            }
            // Same image, same URL within a bucket, so browsers can cache it.
            CHECK(clean());
            M::instance().configure(M::Policy{}, nullptr);
        } catch (const std::exception &) {
            c::expectThat(false, "media-isolation", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1d O8: only the uploader downloads the original; the partner gets variants; another couple gets nothing") {
        try {
            useStore();
            const auto s = space();
            const auto other = space();
            const auto u = uploaded(s.userA);
            CHECK_EQ(stateOf(complete(s.userA, u.id)), std::string("ready"));
            // Before attachment: the uploader alone, including the original.
            CHECK_EQ(served(s.userA, u.id, "original"), image());
            for (const char *variant : {"thumb", "view", "original"}) {
                CHECK(is(content(s.userB, u.id, variant), MO::NotFound));
                CHECK(is(content(other.userA, u.id, variant), MO::NotFound));
            }
            const auto anchor = anchorMemory(pool, s);
            pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                              "VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)", u.id, s.couple, anchor, s.userA);
            // Attached: variants for both members.
            for (const char *variant : {"thumb", "view"}) {
                CHECK_EQ(served(s.userA, u.id, variant), image());
                CHECK_EQ(served(s.userB, u.id, variant), image());
                CHECK(is(content(other.userA, u.id, variant), MO::NotFound));
                CHECK(is(content(other.userB, u.id, variant), MO::NotFound));
            }
            // The original stays the uploader's: same 404 as absent, and no URL is signed.
            CHECK_EQ(served(s.userA, u.id, "original"), image());
            const auto refused = content(s.userB, u.id, "original");
            CHECK(is(refused, MO::NotFound));
            if (refused) CHECK(refused->redirectUrl.empty());
            CHECK(is(content(other.userA, u.id, "original"), MO::NotFound));
            // Whichever member uploaded it: the rule follows the uploader, not the role of creator.
            const auto second = uploaded(s.userB);
            CHECK_EQ(stateOf(complete(s.userB, second.id)), std::string("ready"));
            pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                              "VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)", second.id, s.couple, anchorMemory(pool, s), s.userB);
            CHECK_EQ(served(s.userB, second.id, "original"), image());
            CHECK_EQ(served(s.userA, second.id, "view"), image());
            CHECK(is(content(s.userA, second.id, "original"), MO::NotFound));
            M::instance().configure(M::Policy{}, nullptr);
        } catch (const std::exception &) {
            c::expectThat(false, "media-o8", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::media
