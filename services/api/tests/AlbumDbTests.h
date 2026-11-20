#pragma once

// P3-2a: the real AlbumService, MediaService, MemoryService, scheduler and cleanup job against FakeObjectStore, on literal usapp_test.
// Isolated committed fixtures; assertions target this run's ids only. The test pool has two connections, so anything that must hold
// or wait beside the service uses a dedicated connection of its own, and nothing here uses the pool while a race holds it.
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "MemoryDbTests.h"
#include "services/AlbumService.h"

namespace usapptest::album {

namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace w = usapptest::wallet;
using A = usapp::AlbumService;
using AO = A::Outcome;
using PendingA = ts::Pending<A::Status>;
using Ids = std::vector<std::string>;
using usapp::scheduler::Scheduler;

inline std::optional<c::Observer> holderConn;
inline std::optional<c::Observer> waiterConn;

inline const c::Observer &obs() { return md::obs(); }
inline long long countOf(const std::string &sql, const std::string &a, const std::string &b = "") { return c::scalar(obs(), sql, a, b); }

inline const char *kBindingsSql = "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid";
inline const char *kItemsSql = "SELECT count(*)::bigint AS n FROM album_items WHERE album_id = $1::uuid";
inline const char *kItemsOfMediaSql = "SELECT count(*)::bigint AS n FROM album_items WHERE media_id = $1::uuid";
inline const char *kBatchesSql = "SELECT count(*)::bigint AS n FROM album_photo_batches WHERE album_id = $1::uuid";
inline const char *kRemovalsSql = "SELECT count(*)::bigint AS n FROM album_item_removals WHERE album_id = $1::uuid";
inline const char *kCoversSql = "SELECT count(*)::bigint AS n FROM album_cover_events WHERE album_id = $1::uuid";
inline const char *kAuditSql = "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'album' AND target_id = $1::text AND action = $2::text";
inline const char *kAlbumsInCoupleSql = "SELECT count(*)::bigint AS n FROM albums WHERE couple_id = $1::uuid";
inline const char *kTextsInCoupleSql = "SELECT count(*)::bigint AS n FROM album_texts WHERE couple_id = $1::uuid";
inline const char *kPlansSql = "SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE media_id = $1::uuid AND scope = 'everything'";
inline const char *kBlockedSql =
    "SELECT count(*)::bigint AS n FROM pg_stat_activity a WHERE cardinality(pg_blocking_pids(a.pid)) > 0 AND a.query LIKE $1";
inline const char *kPlanInsertSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) VALUES ($1::uuid, 'everything', 'unattached', clock_timestamp())";
inline const char *kBindAlbumSql =
    "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ($1::uuid, $2::uuid, 'album', $3::uuid, $4::uuid)";
inline const char *kLockMedia = "SELECT pg_advisory_xact_lock(33018::integer, hashtext($1::text))";
inline const char *kLockAlbum = "SELECT pg_advisory_xact_lock(33021::integer, hashtext($1::text))";
inline const char *kMediaLockHeldSql =
    "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND granted AND classid = 33018 AND objsubid = 2 "
    "AND objid = (hashtext($1::text)::bigint & 4294967295)::text::oid";

inline std::string newKey() { return c::newUuid(); }
inline std::shared_ptr<PendingA> startCreate(const std::string &user, const std::string &name, const std::string &key = newKey()) {
    auto p = std::make_shared<PendingA>();
    A::instance().create(user, key, name, [p](A::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingA> startAdd(const std::string &user, const std::string &album, const Ids &ids, const std::string &key = newKey(), A::Fault fault = A::Fault::None) {
    auto p = std::make_shared<PendingA>();
    A::instance().addPhotos(user, album, key, ids, [p](A::Status s) { p->deliver(std::move(s)); }, fault);
    return p;
}
inline std::shared_ptr<PendingA> startRemove(const std::string &user, const std::string &album, const std::string &media, const std::string &key = newKey()) {
    auto p = std::make_shared<PendingA>();
    A::instance().removePhoto(user, album, media, key, [p](A::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingA> startCover(const std::string &user, const std::string &album, const std::string &media, const std::string &key = newKey()) {
    auto p = std::make_shared<PendingA>();
    A::instance().setCover(user, album, media, key, [p](A::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::optional<A::Status> create(const std::string &user, const std::string &name, const std::string &key = newKey()) { return startCreate(user, name, key)->wait(); }
inline std::optional<A::Status> add(const std::string &user, const std::string &album, const Ids &ids, const std::string &key = newKey(), A::Fault fault = A::Fault::None) {
    return startAdd(user, album, ids, key, fault)->wait();
}
inline std::optional<A::Status> removePhoto(const std::string &user, const std::string &album, const std::string &media, const std::string &key = newKey()) {
    return startRemove(user, album, media, key)->wait();
}
inline std::optional<A::Status> cover(const std::string &user, const std::string &album, const std::string &media, const std::string &key = newKey()) {
    return startCover(user, album, media, key)->wait();
}
inline std::optional<A::Status> getAlbum(const std::string &user, const std::string &album) {
    auto p = std::make_shared<PendingA>();
    A::instance().get(user, album, [p](A::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline std::optional<A::ListResult> listAlbums(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    auto p = std::make_shared<ts::Pending<A::ListResult>>();
    std::optional<A::Cursor> decoded;
    if (cursor) decoded = A::decodeCursor(*cursor);
    A::instance().list(user, limit, decoded, [p](A::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::optional<A::ListResult> listPhotos(const std::string &user, const std::string &album, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    auto p = std::make_shared<ts::Pending<A::ListResult>>();
    std::optional<int> decoded;
    if (cursor) decoded = A::decodePhotoCursor(*cursor);
    A::instance().listPhotos(user, album, limit, decoded, [p](A::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline bool is(const std::optional<A::Status> &s, AO o) { return s.has_value() && s->outcome == o; }
inline std::string idOf(const std::optional<A::Status> &s) { return s ? s->album["id"].asString() : std::string(); }
inline std::string coverOf(const std::optional<A::Status> &s) {
    return s && s->album["cover"].isObject() ? s->album["cover"]["media_id"].asString() : std::string();
}
inline Ids photoIds(const std::string &user, const std::string &album) {
    const auto page = listPhotos(user, album, 100);
    if (!page || page->outcome != AO::Ok) throw std::runtime_error("photo page");
    Ids out;
    for (const auto &item : page->items) out.push_back(item["media_id"].asString());
    return out;
}
inline std::string upper(std::string s) {
    for (auto &ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
inline std::string bindingRow(const std::string &media) {
    return obs().client->execSqlSync("SELECT row_to_json(b)::text AS r FROM media_bindings b WHERE b.media_id = $1::uuid", media)[0]["r"].as<std::string>();
}
// The blocked lock statements: the add's and the job's have the same text, so the count is what tells them apart.
inline void waitLockWaiters(long long n) {
    md::waitUntil([&]() { return c::scalar(obs(), kBlockedSql, "SELECT pg_advisory_xact_lock(33018%") >= n; });
}
inline void waitBlocked(const std::string &prefix) {
    md::waitUntil([&]() { return c::scalar(obs(), kBlockedSql, prefix + "%") >= 1; });
}
// One real scheduler round, not waited for here, and how the media job ended.
struct TickRun {
    std::shared_ptr<ts::Pending<bool>> done = std::make_shared<ts::Pending<bool>>();
    std::shared_ptr<std::string> outcome = std::make_shared<std::string>("none");
};
inline TickRun startTick() {
    TickRun run;
    auto outcome = run.outcome;
    auto done = run.done;
    Scheduler::instance().tickOnce(
        [outcome](const Scheduler::TickReport &report) {
            if (report.jobKey == "media.cleanup") *outcome = Scheduler::outcomeToString(report.outcome);
        },
        [done]() { done->deliver(true); });
    return run;
}
inline std::string lastPlanError(const std::string &media) {
    auto result = std::make_shared<ts::Pending<std::string>>();
    waiterConn->client->execSqlAsync(kPlanInsertSql, [result](const drogon::orm::Result &) { result->deliver(""); },
                                     [result](const drogon::orm::DrogonDbException &e) { result->deliver(e.base().what()); }, media);
    const auto got = result->wait();
    if (!got) throw std::runtime_error("plan timed out");
    return *got;
}
// Refused by SQL, with this marker in the error.
inline bool refusedWith(const w::ExecOutcome &outcome, const std::string &marker) {
    return outcome.completed && !outcome.ok && outcome.error.find(marker) != std::string::npos;
}

inline void runAlbumDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    holderConn = c::Observer::connect(dsn);
    waiterConn = c::Observer::connect(dsn);
    const auto aged = [&](const ts::CommittedSpace &s, const std::string &uploader = "") { return md::agedReady(pool, s, uploader); };

    TEST_CASE("P3-2a: create, read and list: an empty album has no cover; a retry replays; a changed retry is refused") {
        try {
            md::useStore();
            const auto s = md::space();
            const std::string key = newKey();
            const auto made = create(s.userA, "旅行", key);
            CHECK(is(made, AO::Created));
            const auto id = idOf(made);
            if (made) {
                CHECK_EQ(made->album["name"].asString(), std::string("旅行"));
                CHECK_EQ(made->album["creator_id"].asString(), s.userA);
                CHECK_EQ(made->album["photo_count"].asInt(), 0);
                CHECK(made->album["cover"].isNull());
                CHECK_EQ(made->mutation["action"].asString(), std::string("create"));
                CHECK_EQ(made->mutation["album_id"].asString(), id);
            }
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.created"), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = 'album.created' AND detail::text NOT LIKE '%旅行%'", id), 1);
            const auto replay = create(s.userA, "旅行", key);
            CHECK(is(replay, AO::Replayed));
            CHECK_EQ(idOf(replay), id);
            CHECK_EQ(countOf(kAlbumsInCoupleSql, s.couple), 1);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), 1);
            CHECK(is(create(s.userA, "别的", key), AO::KeyReused));

            // Both members read it, and an empty album lists.
            const auto read = getAlbum(s.userB, id);
            CHECK(is(read, AO::Ok));
            if (read) CHECK_EQ(read->album["name"].asString(), std::string("旅行"));
            const auto page = listAlbums(s.userB, 10);
            CHECK(page.has_value() && page->outcome == AO::Ok && page->items.size() == 1u && !page->nextCursor);
            CHECK(photoIds(s.userB, id).empty());

            // Either member creates; the same key by the other member is its own request.
            const auto second = create(s.userB, "旅行", key);
            CHECK(is(second, AO::Created));
            CHECK(idOf(second) != id);
            // Same key at once: one write, one replay.
            for (int round = 0; round < 3; ++round) {
                const std::string same = newKey();
                auto one = startCreate(s.userA, "同时", same);
                auto two = startCreate(s.userA, "同时", same);
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::Replayed)) || (is(y, AO::Created) && is(x, AO::Replayed)));
                CHECK_EQ(idOf(x), idOf(y));
            }
            CHECK_EQ(countOf(kAlbumsInCoupleSql, s.couple), 5);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), 5);
            // No space at all.
            const auto stranger = c::newUuid();
            CHECK(is(create(stranger, "x"), AO::SpaceNotFound));
            const auto strangerPage = listAlbums(stranger, 10);
            CHECK(strangerPage.has_value() && strangerPage->outcome == AO::SpaceNotFound);
        } catch (const std::exception &) {
            c::expectThat(false, "album-create", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: another couple's album and photos answer exactly as absent ones, with no store call, in the service and in the database") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto p = aged(s);
            const auto id = idOf(create(s.userA, "ours"));
            CHECK(is(add(s.userA, id, {p}), AO::Created));
            const auto theirs = aged(other);
            const auto theirAlbum = idOf(create(other.userA, "theirs"));
            const auto calls = store->calls.load();

            CHECK(is(getAlbum(other.userA, id), AO::NotFound));
            const auto foreignPhotos = listPhotos(other.userA, id, 10);
            CHECK(foreignPhotos.has_value() && foreignPhotos->outcome == AO::NotFound);
            CHECK(is(add(other.userA, id, {theirs}), AO::NotFound));
            CHECK(is(removePhoto(other.userA, id, p), AO::NotFound));
            CHECK(is(cover(other.userA, id, p), AO::NotFound));
            const auto absent = c::newUuid();
            CHECK(is(getAlbum(s.userA, absent), AO::NotFound));
            CHECK(is(add(s.userA, absent, {p}), AO::NotFound));
            CHECK(is(removePhoto(s.userA, absent, p), AO::NotFound));
            CHECK(is(cover(s.userA, absent, p), AO::NotFound));
            const auto strangerPhotos = listPhotos(c::newUuid(), id, 10);
            CHECK(strangerPhotos.has_value() && strangerPhotos->outcome == AO::NotFound);
            // Neither couple can use the other's photos: one answer, and nothing written.
            CHECK(is(add(s.userA, id, {theirs}), AO::PhotoUnavailable));
            CHECK(is(add(other.userA, theirAlbum, {p}), AO::PhotoUnavailable));
            CHECK(is(removePhoto(s.userA, id, theirs), AO::PhotoNotFound));
            CHECK(is(cover(s.userA, id, theirs), AO::PhotoNotFound));
            CHECK_EQ(countOf(kBindingsSql, theirs), 0);
            CHECK_EQ(countOf(kItemsOfMediaSql, theirs), 0);
            const auto page = listAlbums(other.userA, 10);
            if (page) CHECK_EQ(page->items.size(), 1u);
            CHECK_EQ(store->calls.load(), calls);

            // Directly: a binding to the other couple's album.
            bool refused = false;
            try {
                pool->execSqlSync(kBindAlbumSql, theirs, other.couple, id, other.userA);
            } catch (const std::exception &) { refused = true; }
            CHECK(refused);
            CHECK_EQ(countOf(kBindingsSql, theirs), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "album-cross-couple", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: only eligible READY photos of the caller's own enter, atomically; one home for good") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto mine = aged(s);
            const auto partners = aged(s, s.userB);
            // The other member's unattached photo, for the same answer as an absent one.
            CHECK(is(add(s.userA, id, {partners}), AO::PhotoUnavailable));
            CHECK(is(add(s.userA, id, {c::newUuid()}), AO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, partners), 0);

            // Not READY: a grant with no decision.
            const auto granted = md::grant(s.userA);
            CHECK(is(add(s.userA, id, {md::idOf(granted)}), AO::PhotoUnavailable));
            // Planned.
            const auto planned = aged(s);
            CHECK(lastPlanError(planned).empty());
            CHECK(is(add(s.userA, id, {planned}), AO::PhotoUnavailable));
            // All or nothing, whichever position the bad one has.
            const auto good = aged(s);
            CHECK(is(add(s.userA, id, {good, planned}), AO::PhotoUnavailable));
            CHECK(is(add(s.userA, id, {planned, good}), AO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, good), 0);
            CHECK_EQ(countOf(kBatchesSql, id), 0);
            CHECK_EQ(countOf(kItemsSql, id), 0);

            // A real upload, added: the partner sees it only from then on.
            const auto u = md::uploaded(s.userA);
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
            CHECK(md::is(md::facts(s.userB, u.id), md::MO::NotFound));
            CHECK(md::is(md::content(s.userB, u.id), md::MO::NotFound));
            const auto added = add(s.userA, id, {mine, u.id});
            CHECK(is(added, AO::Created));
            if (added) {
                CHECK_EQ(added->album["photo_count"].asInt(), 2);
                CHECK_EQ(added->mutation["action"].asString(), std::string("add_photos"));
                CHECK_EQ(added->mutation["media_ids"].size(), 2u);
                Json::StreamWriterBuilder writer;
                CHECK(Json::writeString(writer, added->album).find("media/") == std::string::npos);
                CHECK(Json::writeString(writer, added->album).find("staging") == std::string::npos);
            }
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid AND target_kind = 'album' AND target_id = $2::uuid", mine, id), 1);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photos_added"), 1);

            // One home: not this album again, not another, not a memory or a proposal.
            const auto otherAlbum = idOf(create(s.userB, "other"));
            CHECK(is(add(s.userA, id, {mine}), AO::PhotoUnavailable));
            CHECK(is(add(s.userA, otherAlbum, {mine}), AO::PhotoUnavailable));
            CHECK(mm::is(mm::create(s.userA, mm::terms("m", {mine})), mm::SO::PhotoUnavailable));
            const auto memoryPhoto = aged(s);
            const auto memory = mm::idOf(mm::create(s.userA, mm::terms("m", {memoryPhoto})));
            CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("n", {mine})), mm::SO::PhotoUnavailable));
            const auto partnerAlbumPhoto = aged(s, s.userB);
            CHECK(is(add(s.userB, otherAlbum, {partnerAlbumPhoto}), AO::Created));
            CHECK(mm::is(mm::propose(s.userB, memory, 1, mm::terms("n", {partnerAlbumPhoto})), mm::SO::PhotoUnavailable));

            // Memory photos stay out, current and historical: D12.
            CHECK(is(add(s.userA, id, {memoryPhoto}), AO::PhotoUnavailable));
            const auto replacement = aged(s);
            CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("n", {replacement})), mm::SO::Created));
            CHECK(is(add(s.userA, id, {memoryPhoto}), AO::PhotoUnavailable));
            CHECK_EQ(countOf(kItemsOfMediaSql, memoryPhoto), 0);

            // Reserved by a pending proposal: refused until released, then the uploader may add it.
            const auto reserved = aged(s, s.userB);
            const auto pending = mm::propose(s.userB, memory, 2, mm::terms("r", {reserved}));
            CHECK(mm::is(pending, mm::SO::Created));
            CHECK(is(add(s.userB, otherAlbum, {reserved}), AO::PhotoUnavailable));
            CHECK(mm::is(mm::decide(s.userA, memory, mm::proposalOf(pending), mm::Decision::Reject), mm::SO::Ok));
            CHECK(is(add(s.userB, otherAlbum, {reserved}), AO::Created));
        } catch (const std::exception &) {
            c::expectThat(false, "album-eligibility", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: partner access: variants while active; nothing new after removal; an issued URL lives on; the uploader keeps reading") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto u = md::uploaded(s.userA);
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
            const auto keep = aged(s);
            const auto id = idOf(create(s.userB, "t"));
            CHECK(is(add(s.userA, id, {u.id, keep}), AO::Created));

            // Active.
            CHECK_EQ(md::stateOf(md::facts(s.userB, u.id)), std::string("ready"));
            CHECK_EQ(md::served(s.userB, u.id, "view"), md::image());
            CHECK_EQ(md::served(s.userB, u.id, "thumb"), md::image());
            CHECK(md::is(md::content(s.userB, u.id, "original"), md::MO::NotFound));
            CHECK_EQ(md::served(s.userA, u.id, "original"), md::image());
            CHECK(md::is(md::facts(other.userA, u.id), md::MO::NotFound));
            CHECK(md::is(md::content(other.userA, u.id), md::MO::NotFound));

            // An URL issued while active.
            const auto issued = md::content(s.userB, u.id, "view");
            CHECK(md::is(issued, md::MO::Ok));
            const std::string issuedUrl = issued ? issued->redirectUrl : std::string();

            const auto binding = bindingRow(u.id);
            CHECK(is(removePhoto(s.userA, id, u.id), AO::Ok));

            // From the commit: no facts, no variant, nothing signed.
            CHECK(md::is(md::facts(s.userB, u.id), md::MO::NotFound));
            for (const char *variant : {"view", "thumb", "original"}) {
                const auto after = md::content(s.userB, u.id, variant);
                CHECK(md::is(after, md::MO::NotFound));
                if (after) CHECK(after->redirectUrl.empty());
            }
            // Against the fake, which does not model expiry: a URL issued earlier is not recalled. Real expiry is P3-2d.
            CHECK_EQ(store->followRead(issuedUrl), md::image());
            // The uploader keeps every variant, and nothing is lost: the binding is byte-identical and the key stays.
            CHECK_EQ(md::stateOf(md::facts(s.userA, u.id)), std::string("ready"));
            CHECK_EQ(md::served(s.userA, u.id, "view"), md::image());
            CHECK_EQ(md::served(s.userA, u.id, "original"), md::image());
            CHECK_EQ(bindingRow(u.id), binding);
            CHECK_EQ(md::disposition(md::readyKey(u.id)), std::string("keep"));
            // The other photo is untouched.
            CHECK_EQ(md::stateOf(md::facts(s.userB, keep)), std::string("ready"));
            CHECK(photoIds(s.userB, id) == Ids({keep}));
            // Another member can never bring it back, and neither can its uploader.
            CHECK(is(add(s.userA, id, {u.id}), AO::PhotoUnavailable));
            const auto elsewhere = idOf(create(s.userA, "elsewhere"));
            CHECK(is(add(s.userA, elsewhere, {u.id}), AO::PhotoUnavailable));
            CHECK(mm::is(mm::create(s.userA, mm::terms("m", {u.id})), mm::SO::PhotoUnavailable));
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "album-access", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: memory access is unchanged: current, only in history, reserved, released and unbound") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto current = aged(s);
            const auto dropped = aged(s);
            const auto id = mm::idOf(mm::create(s.userA, mm::terms("t", {dropped, current})));
            CHECK_EQ(md::stateOf(md::facts(s.userB, current)), std::string("ready"));
            CHECK(mm::is(mm::edit(s.userA, id, 1, mm::terms("u", {current})), mm::SO::Created));
            CHECK_EQ(md::stateOf(md::facts(s.userB, current)), std::string("ready"));
            CHECK_EQ(md::stateOf(md::facts(s.userB, dropped)), std::string("ready"));
            CHECK(md::is(md::content(s.userB, dropped, "original"), md::MO::NotFound));

            const auto proposed = aged(s, s.userB);
            CHECK(md::is(md::facts(s.userA, proposed), md::MO::NotFound));
            const auto pending = mm::propose(s.userB, id, 2, mm::terms("v", {proposed}));
            CHECK_EQ(md::stateOf(md::facts(s.userA, proposed)), std::string("ready"));
            CHECK(mm::is(mm::decide(s.userB, id, mm::proposalOf(pending), mm::Decision::Withdraw), mm::SO::Ok));
            CHECK(md::is(md::facts(s.userA, proposed), md::MO::NotFound));
            CHECK_EQ(md::stateOf(md::facts(s.userB, proposed)), std::string("ready"));

            const auto unbound = aged(s);
            CHECK(md::is(md::facts(s.userB, unbound), md::MO::NotFound));
            CHECK_EQ(md::stateOf(md::facts(s.userA, unbound)), std::string("ready"));
        } catch (const std::exception &) {
            c::expectThat(false, "album-memory-access", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: ids are normalized before anything hashes them: uppercase is the same photo, the same key and the same lock") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto ordered = mm::byLockOrder(aged(s), aged(s));
            // The body parser is the service's only door: uppercase ids come out canonical.
            Json::Value body(Json::objectValue);
            body["media_ids"] = Json::Value(Json::arrayValue);
            body["media_ids"].append(upper(ordered.first));
            body["media_ids"].append(upper(ordered.second));
            Ids parsed;
            std::string message;
            CHECK(A::parseAddRequest(body, parsed, message));
            CHECK(parsed == Ids({ordered.first, ordered.second}));
            CHECK_EQ(A::addFingerprint(id, parsed), A::addFingerprint(id, {ordered.first, ordered.second}));
            // Waits on the very lock the job's text form names.
            auto hold = c::acquireTx(holderConn->client);
            CHECK(c::exec(hold, kLockMedia, ordered.second).ok);
            const std::string key = newKey();
            auto adding = startAdd(s.userA, id, parsed, key);
            waitLockWaiters(1);
            CHECK(c::commitTx(hold).value_or(false));
            CHECK(is(adding->wait(), AO::Created));
            // A retry in the other spelling is a replay.
            CHECK(is(add(s.userA, id, {ordered.first, ordered.second}, key), AO::Replayed));
            CHECK_EQ(countOf(kBatchesSql, id), 1);

            // An uppercase path: the same removal, the same key, the same replay.
            const std::string removal = newKey();
            CHECK(is(removePhoto(s.userA, id, upper(ordered.first), removal), AO::Ok));
            CHECK(is(removePhoto(s.userA, id, ordered.first, removal), AO::Replayed));
            CHECK_EQ(countOf(kRemovalsSql, id), 1);
            const std::string chosen = newKey();
            CHECK(is(cover(s.userA, id, upper(ordered.second), chosen), AO::Created));
            CHECK(is(cover(s.userA, id, ordered.second, chosen), AO::Replayed));
            // An uppercase album id reaches the same album.
            CHECK(is(getAlbum(s.userB, upper(id)), AO::Ok));
            CHECK(is(cover(s.userB, upper(id), ordered.second), AO::Created));
            CHECK_EQ(countOf(kCoversSql, id), 2);
        } catch (const std::exception &) {
            c::expectThat(false, "album-normalization", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: removal: every row of the decision table, replay first, and concurrent removals") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto p = aged(s), q = aged(s, s.userB);
            CHECK(is(add(s.userA, id, {p}), AO::Created));
            CHECK(is(add(s.userB, id, {q}), AO::Created));

            // Absent; another couple's id is the same; the partner's active photo is forbidden, even to the album's creator.
            CHECK(is(removePhoto(s.userA, id, c::newUuid()), AO::PhotoNotFound));
            CHECK(is(removePhoto(s.userB, id, p), AO::PhotoForbidden));
            CHECK(is(removePhoto(s.userA, id, q), AO::PhotoForbidden));
            CHECK_EQ(countOf(kRemovalsSql, id), 0);

            // The uploader, directly.
            const std::string key = newKey();
            const auto removed = removePhoto(s.userA, id, p, key);
            CHECK(is(removed, AO::Ok));
            std::string removedAt;
            if (removed) {
                CHECK_EQ(removed->mutation["action"].asString(), std::string("remove_photo"));
                CHECK_EQ(removed->mutation["media_id"].asString(), p);
                removedAt = removed->mutation["created_at"].asString();
                CHECK_EQ(removed->album["photo_count"].asInt(), 1);
            }
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photo_removed"), 1);

            // Same key: a read-only replay with the original facts and today's album, even though it is already removed.
            CHECK(is(removePhoto(s.userB, id, q), AO::Ok));
            const auto replay = removePhoto(s.userA, id, p, key);
            CHECK(is(replay, AO::Replayed));
            if (replay) {
                CHECK_EQ(replay->mutation["created_at"].asString(), removedAt);
                CHECK_EQ(replay->album["photo_count"].asInt(), 0);
            }
            // Same key, different target.
            CHECK(is(removePhoto(s.userA, id, q, key), AO::KeyReused));
            const auto second = idOf(create(s.userA, "second"));
            CHECK(is(removePhoto(s.userA, second, p, key), AO::KeyReused));
            // A new key: the uploader learns it is already gone; the partner learns nothing.
            CHECK(is(removePhoto(s.userA, id, p), AO::PhotoRemoved));
            CHECK(is(removePhoto(s.userB, id, p), AO::PhotoNotFound));
            CHECK_EQ(countOf(kRemovalsSql, id), 2);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photo_removed"), 2);

            // At once. The same key: one removal, one replay. Different keys: one removal, one ALBUM_PHOTO_REMOVED.
            for (int round = 0; round < 3; ++round) {
                const auto r = aged(s);
                CHECK(is(add(s.userA, id, {r}), AO::Created));
                const std::string same = newKey();
                auto one = startRemove(s.userA, id, r, same);
                auto two = startRemove(s.userA, id, r, same);
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Ok) && is(y, AO::Replayed)) || (is(y, AO::Ok) && is(x, AO::Replayed)));
                const auto t = aged(s);
                CHECK(is(add(s.userA, id, {t}), AO::Created));
                auto three = startRemove(s.userA, id, t);
                auto four = startRemove(s.userA, id, t);
                const auto v = three->wait();
                const auto w = four->wait();
                CHECK((is(v, AO::Ok) && is(w, AO::PhotoRemoved)) || (is(w, AO::Ok) && is(v, AO::PhotoRemoved)));
            }
            CHECK_EQ(countOf(kRemovalsSql, id), 8);
        } catch (const std::exception &) {
            c::expectThat(false, "album-removal", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: duplicate and concurrent writes: replays write nothing, and contenders resolve without a deadlock") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto second = idOf(create(s.userA, "second"));
            const auto p1 = aged(s), p2 = aged(s);

            const std::string key = newKey();
            const auto first = add(s.userA, id, {p1, p2}, key);
            CHECK(is(first, AO::Created));
            const auto replay = add(s.userA, id, {p1, p2}, key);
            CHECK(is(replay, AO::Replayed));
            if (first && replay) CHECK_EQ(replay->mutation["batch_id"].asString(), first->mutation["batch_id"].asString());
            CHECK_EQ(countOf(kBatchesSql, id), 1);
            CHECK_EQ(countOf(kItemsSql, id), 2);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photos_added"), 1);
            // A different body under the key: another order, another album, another photo.
            CHECK(is(add(s.userA, id, {p2, p1}, key), AO::KeyReused));
            CHECK(is(add(s.userA, second, {p1, p2}, key), AO::KeyReused));
            CHECK(is(add(s.userA, id, {aged(s)}, key), AO::KeyReused));
            // Later changes do not change a replay's facts.
            CHECK(is(removePhoto(s.userA, id, p1), AO::Ok));
            const auto late = add(s.userA, id, {p1, p2}, key);
            CHECK(is(late, AO::Replayed));
            if (late) {
                CHECK_EQ(late->mutation["media_ids"].size(), 2u);
                CHECK_EQ(late->album["photo_count"].asInt(), 1);
            }

            // The same add key at once: one write, one replay.
            for (int round = 0; round < 3; ++round) {
                const auto a = aged(s), b = aged(s);
                const std::string same = newKey();
                auto one = startAdd(s.userA, id, {a, b}, same);
                auto two = startAdd(s.userA, id, {a, b}, same);
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::Replayed)) || (is(y, AO::Created) && is(x, AO::Replayed)));
                CHECK_EQ(countOf(kBindingsSql, a), 1);
                CHECK_EQ(countOf(kItemsOfMediaSql, b), 1);
            }
            // One key into two albums at once: one write, and the other is told the key is taken.
            for (int round = 0; round < 3; ++round) {
                const auto a = aged(s);
                const std::string same = newKey();
                auto one = startAdd(s.userA, id, {a}, same);
                auto two = startAdd(s.userA, second, {a}, same);
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::KeyReused)) || (is(y, AO::Created) && is(x, AO::KeyReused)));
                CHECK_EQ(countOf(kItemsOfMediaSql, a), 1);
            }
            // One photo into one album, and into two, under different keys: one wins, the other refuses.
            for (int round = 0; round < 3; ++round) {
                const auto a = aged(s);
                auto one = startAdd(s.userA, id, {a});
                auto two = startAdd(s.userA, round % 2 == 0 ? id : second, {a});
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::PhotoUnavailable)) || (is(y, AO::Created) && is(x, AO::PhotoUnavailable)));
                CHECK_EQ(countOf(kBindingsSql, a), 1);
            }
            // Overlapping sets in opposite orders, in different albums, so only the sorted media locks stand between them.
            for (int round = 0; round < 5; ++round) {
                const auto a = aged(s), b = aged(s);
                auto one = startAdd(s.userA, id, {a, b});
                auto two = startAdd(s.userA, second, {b, a});
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK(x.has_value() && y.has_value());
                CHECK((is(x, AO::Created) && is(y, AO::PhotoUnavailable)) || (is(y, AO::Created) && is(x, AO::PhotoUnavailable)));
            }
            // A cover racing a removal: never a removed cover.
            for (int round = 0; round < 5; ++round) {
                const auto a = aged(s), b = aged(s);
                CHECK(is(add(s.userA, second, {a, b}), AO::Created));
                auto setting = startCover(s.userB, second, a);
                auto removing = startRemove(s.userA, second, a);
                const auto x = setting->wait();
                const auto y = removing->wait();
                CHECK(is(x, AO::Created) || is(x, AO::PhotoNotFound));
                CHECK(is(y, AO::Ok));
                const auto read = getAlbum(s.userA, second);
                CHECK(is(read, AO::Ok));
                CHECK(coverOf(read) != a);
                CHECK_EQ(coverOf(read), b);
                // Leave it empty of this round's photos for the next one.
                CHECK(is(removePhoto(s.userA, second, b), AO::Ok));
            }
        } catch (const std::exception &) {
            c::expectThat(false, "album-concurrency", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: a failure after the binding rolls back the whole add, and the same key then succeeds") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto p1 = aged(s), p2 = aged(s);
            const std::string key = newKey();
            const auto failed = add(s.userA, id, {p1, p2}, key, A::Fault::FailAfterBinding);
            CHECK(is(failed, AO::Error));
            CHECK_EQ(countOf(kBatchesSql, id), 0);
            CHECK_EQ(countOf(kItemsSql, id), 0);
            CHECK_EQ(countOf(kBindingsSql, p1), 0);
            CHECK_EQ(countOf(kBindingsSql, p2), 0);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photos_added"), 0);
            const auto retried = add(s.userA, id, {p1, p2}, key);
            CHECK(is(retried, AO::Created));
            CHECK_EQ(countOf(kItemsSql, id), 2);
            CHECK(photoIds(s.userB, id) == Ids({p1, p2}));
        } catch (const std::exception &) {
            c::expectThat(false, "album-rollback", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: order and cover: the chosen order inside an add, newer adds first, stable pages, and the fallback rules") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto a = aged(s), b = aged(s), c3 = aged(s), d = aged(s, s.userB), e = aged(s, s.userB);
            const auto firstAdd = add(s.userA, id, {a, b, c3});
            CHECK(is(firstAdd, AO::Created));
            if (firstAdd) {
                const Ids facts = {firstAdd->mutation["media_ids"][0].asString(), firstAdd->mutation["media_ids"][1].asString(), firstAdd->mutation["media_ids"][2].asString()};
                CHECK(facts == Ids({a, b, c3}));
                // The first photo chosen is the fallback.
                CHECK_EQ(coverOf(firstAdd), a);
                CHECK(!firstAdd->album["cover"]["chosen"].asBool());
            }
            CHECK(photoIds(s.userB, id) == Ids({a, b, c3}));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM album_items WHERE album_id = $1::uuid AND media_id = $2::uuid AND seq = 3", id, a), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM album_items WHERE album_id = $1::uuid AND media_id = $2::uuid AND seq = 1", id, c3), 1);
            const auto secondAdd = add(s.userB, id, {d, e});
            CHECK(is(secondAdd, AO::Created));
            CHECK(photoIds(s.userA, id) == Ids({d, e, a, b, c3}));
            CHECK_EQ(coverOf(secondAdd), d);

            // Chosen by either member, and it stays through later adds.
            const std::string key = newKey();
            const auto chosen = cover(s.userB, id, b, key);
            CHECK(is(chosen, AO::Created));
            CHECK_EQ(coverOf(chosen), b);
            if (chosen) {
                CHECK(chosen->album["cover"]["chosen"].asBool());
                CHECK_EQ(chosen->mutation["event_no"].asInt(), 1);
            }
            const auto replayed = cover(s.userB, id, b, key);
            CHECK(is(replayed, AO::Replayed));
            if (replayed) CHECK_EQ(replayed->mutation["event_no"].asInt(), 1);
            CHECK(is(cover(s.userB, id, a, key), AO::KeyReused));
            CHECK_EQ(countOf(kCoversSql, id), 1);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.cover_set"), 1);
            const auto f = aged(s);
            CHECK(coverOf(add(s.userA, id, {f})) == b);
            // Removing the chosen one falls back to the first photo chosen in the latest add, with no new event.
            const auto fallback = removePhoto(s.userA, id, b);
            CHECK_EQ(coverOf(fallback), f);
            CHECK_EQ(countOf(kCoversSql, id), 1);
            if (fallback) CHECK(!fallback->album["cover"]["chosen"].asBool());
            // A removed, foreign or absent photo cannot be chosen.
            CHECK(is(cover(s.userA, id, b), AO::PhotoNotFound));
            CHECK(is(cover(s.userA, id, c::newUuid()), AO::PhotoNotFound));
            CHECK_EQ(countOf(kCoversSql, id), 1);
            // Removing everything leaves no cover, and listing agrees.
            for (const auto &p : {std::make_pair(s.userA, f), std::make_pair(s.userB, d), std::make_pair(s.userB, e), std::make_pair(s.userA, a), std::make_pair(s.userA, c3)}) {
                CHECK(is(removePhoto(p.first, id, p.second), AO::Ok));
            }
            const auto bare = getAlbum(s.userB, id);
            if (bare) {
                CHECK(bare->album["cover"].isNull());
                CHECK_EQ(bare->album["photo_count"].asInt(), 0);
            }
            CHECK(photoIds(s.userA, id).empty());

            // Photos page by seq: an insert and a removal during a walk neither repeat nor skip anything else.
            const auto walk = idOf(create(s.userA, "walk"));
            Ids photos;
            for (int i = 0; i < 5; ++i) photos.push_back(aged(s));
            CHECK(is(add(s.userA, walk, photos), AO::Created));
            Ids seen;
            std::optional<std::string> cursor;
            int pages = 0;
            do {
                const auto page = listPhotos(s.userB, walk, 2, cursor);
                if (!page || page->outcome != AO::Ok) throw std::runtime_error("page");
                for (const auto &item : page->items) seen.push_back(item["media_id"].asString());
                cursor = page->nextCursor;
                if (++pages == 1) {
                    CHECK(is(add(s.userA, walk, {aged(s)}), AO::Created));
                    CHECK(is(removePhoto(s.userA, walk, photos[3]), AO::Ok));
                }
            } while (cursor && pages < 10);
            CHECK(seen == Ids({photos[0], photos[1], photos[2], photos[4]}));
            const auto exact = listPhotos(s.userA, walk, 5);
            if (exact) {
                CHECK_EQ(exact->items.size(), 5u);
                CHECK(!exact->nextCursor);
            }

            // Albums page by creation, newest first, also across an insert.
            const auto other = md::space();
            Ids albums;
            for (int i = 0; i < 3; ++i) albums.push_back(idOf(create(other.userA, "a" + std::to_string(i))));
            Ids walked;
            cursor.reset();
            pages = 0;
            do {
                const auto page = listAlbums(other.userB, 2, cursor);
                if (!page || page->outcome != AO::Ok) throw std::runtime_error("page");
                for (const auto &item : page->items) walked.push_back(item["id"].asString());
                cursor = page->nextCursor;
                if (++pages == 1) CHECK(is(create(other.userB, "newer"), AO::Created));
            } while (cursor && pages < 10);
            CHECK(walked == Ids({albums[2], albums[1], albums[0]}));
            const auto all = listAlbums(other.userA, 4);
            if (all) {
                CHECK_EQ(all->items.size(), 4u);
                CHECK(!all->nextCursor);
                CHECK_EQ(all->items[0]["name"].asString(), std::string("newer"));
            }
        } catch (const std::exception &) {
            c::expectThat(false, "album-order", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: add first, then cleanup, through the real scheduler job: the job plans nothing for them, with no error") {
        try {
            md::useStore([](md::M::Policy &p) { p.planLimit = 1000; });
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto ordered = mm::byLockOrder(aged(s), aged(s));
            const auto &low = ordered.first;
            const auto &high = ordered.second;

            auto hold = c::acquireTx(holderConn->client);
            CHECK(c::exec(hold, kLockMedia, high).ok);
            // The add takes LOW and waits for HIGH.
            auto adding = startAdd(s.userA, id, {low, high});
            waitLockWaiters(1);
            CHECK_EQ(countOf(kMediaLockHeldSql, low), 1);
            // The job reads both as candidates, because the add is uncommitted, and waits for LOW.
            auto ticking = startTick();
            waitLockWaiters(2);
            CHECK(c::commitTx(hold).value_or(false));
            CHECK(is(adding->wait(), AO::Created));
            CHECK(ticking.done->wait().has_value());
            CHECK_EQ(*ticking.outcome, std::string("completed"));

            CHECK_EQ(countOf(kPlansSql, low), 0);
            CHECK_EQ(countOf(kPlansSql, high), 0);
            CHECK_EQ(countOf(kBindingsSql, low), 1);
            CHECK_EQ(countOf(kBindingsSql, high), 1);
            CHECK(photoIds(s.userB, id) == Ids({low, high}));
            CHECK_EQ(md::stateOf(md::facts(s.userB, low)), std::string("ready"));
            // A later round plans nothing for them either.
            md::tick();
            CHECK_EQ(countOf(kPlansSql, low), 0);
            CHECK_EQ(md::disposition(md::readyKey(low)), std::string("keep"));
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "album-race-add-first", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: cleanup first, then add, through the real scheduler job: the add refuses after the wait, leaving nothing") {
        try {
            md::useStore([](md::M::Policy &p) { p.planLimit = 1000; });
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));
            const auto ordered = mm::byLockOrder(aged(s), aged(s));
            const auto &low = ordered.first;
            const auto &high = ordered.second;
            const auto textsBefore = countOf(kTextsInCoupleSql, s.couple);

            auto hold = c::acquireTx(holderConn->client);
            CHECK(c::exec(hold, kLockMedia, high).ok);
            // The job reads both, takes LOW and waits for HIGH.
            auto ticking = startTick();
            waitLockWaiters(1);
            CHECK_EQ(countOf(kMediaLockHeldSql, low), 1);
            // The add waits for LOW, held by the job.
            const std::string key = newKey();
            auto adding = startAdd(s.userA, id, {low}, key);
            waitLockWaiters(2);
            CHECK(c::commitTx(hold).value_or(false));
            CHECK(ticking.done->wait().has_value());
            CHECK_EQ(*ticking.outcome, std::string("completed"));
            CHECK(is(adding->wait(), AO::PhotoUnavailable));

            CHECK_EQ(countOf(kPlansSql, low), 1);
            CHECK_EQ(countOf(kPlansSql, high), 1);
            CHECK_EQ(countOf(kBindingsSql, low), 0);
            CHECK_EQ(countOf(kItemsSql, id), 0);
            CHECK_EQ(countOf(kBatchesSql, id), 0);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), textsBefore);
            CHECK_EQ(c::scalar(obs(), kAuditSql, id, "album.photos_added"), 0);
            // The same key refuses again, with no batch to replay; the partner never saw it.
            CHECK(is(add(s.userA, id, {low}, key), AO::PhotoUnavailable));
            CHECK(md::is(md::facts(s.userB, low), md::MO::NotFound));
            // The sweep proceeds.
            md::tickUntil([&]() { return md::swept(low) == 1 && md::swept(high) == 1; });
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "album-race-plan-first", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: the direct-SQL pairs, removed photos against cleanup, and lock independence in both directions") {
        try {
            md::useStore([](md::M::Policy &p) { p.planLimit = 1000; });
            const auto s = md::space();
            const auto id = idOf(create(s.userA, "t"));

            // Add held, then a plan: the plan waits behind the add, then refuses on the binding.
            {
                const auto ordered = mm::byLockOrder(aged(s), aged(s));
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockMedia, ordered.second).ok);
                auto adding = startAdd(s.userA, id, {ordered.first, ordered.second});
                waitLockWaiters(1);
                auto planTx = c::acquireTx(waiterConn->client);
                auto planning = w::startExec(planTx, kPlanInsertSql, ordered.first);
                waitBlocked("INSERT INTO media_cleanup_plans");
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(is(adding->wait(), AO::Created));
                CHECK(refusedWith(planning->wait(), "USAPP_MEDIA_PLAN_BOUND"));
                c::rollbackTx(planTx);
                CHECK_EQ(countOf(kPlansSql, ordered.first), 0);
            }
            // Plan held, then a binding: the binding waits, then refuses on the plan.
            {
                const auto p = aged(s);
                auto plan = c::acquireTx(holderConn->client);
                CHECK(c::exec(plan, kPlanInsertSql, p).ok);
                auto bindTx = c::acquireTx(waiterConn->client);
                auto binding = w::startExec(bindTx, kBindAlbumSql, p, s.couple, id, s.userA);
                waitBlocked("INSERT INTO media_bindings");
                CHECK(c::commitTx(plan).value_or(false));
                CHECK(refusedWith(binding->wait(), "USAPP_MEDIA_BINDING_CLEANUP"));
                c::rollbackTx(bindTx);
                CHECK_EQ(countOf(kBindingsSql, p), 0);
            }

            // A removed photo, far past the window: neither the job nor direct SQL touches it. An unadded one beside it is planned.
            const auto removed = aged(s);
            const auto unadded = aged(s);
            CHECK(is(add(s.userA, id, {removed}), AO::Created));
            CHECK(is(removePhoto(s.userA, id, removed), AO::Ok));
            md::tickUntil([&]() { return countOf(kPlansSql, unadded) == 1; });
            CHECK_EQ(countOf(kPlansSql, removed), 0);
            CHECK(lastPlanError(removed).find("USAPP_MEDIA_PLAN_BOUND") != std::string::npos);
            CHECK_EQ(md::disposition(md::readyKey(removed)), std::string("keep"));

            // Holding the album lock: completion and the job finish; the album's writes wait.
            {
                const auto u = md::uploaded(s.userA);
                const auto job = aged(s);
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockAlbum, id).ok);
                CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
                md::tickUntil([&]() { return countOf(kPlansSql, job) == 1; });
                auto adding = startAdd(s.userA, id, {u.id});
                waitBlocked("SELECT pg_advisory_xact_lock(33021");
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(is(adding->wait(), AO::Created));
            }
            // Holding a media lock: a removal, which takes only the album's lock, finishes. Since D6 a cover takes 33018 after the album's
            // lock, so a deletion is entirely before it or entirely after it: it waits, then succeeds.
            {
                const auto p = aged(s), q = aged(s);
                CHECK(is(add(s.userA, id, {p, q}), AO::Created));
                {
                    auto hold = c::acquireTx(holderConn->client);
                    CHECK(c::exec(hold, kLockMedia, p).ok);
                    CHECK(is(removePhoto(s.userA, id, p), AO::Ok));
                    CHECK(c::commitTx(hold).value_or(false));
                }
                {
                    auto hold = c::acquireTx(holderConn->client);
                    CHECK(c::exec(hold, kLockMedia, q).ok);
                    auto covering = startCover(s.userB, id, q);
                    waitLockWaiters(1);
                    CHECK(c::commitTx(hold).value_or(false));
                    CHECK(is(covering->wait(), AO::Created));
                }
            }
            // The album branch takes 33021 before 33018: waiting on the album, it holds no media lock.
            {
                const auto p = aged(s);
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockAlbum, id).ok);
                auto bindTx = c::acquireTx(waiterConn->client);
                auto binding = w::startExec(bindTx, kBindAlbumSql, p, s.couple, id, s.userA);
                waitBlocked("INSERT INTO media_bindings");
                CHECK_EQ(countOf(kMediaLockHeldSql, p), 0);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(binding->wait().ok);
                CHECK_EQ(countOf(kMediaLockHeldSql, p), 1);
                c::rollbackTx(bindTx);
            }
            // The memory branch never takes 33021: holding that key on a memory's id blocks nothing.
            {
                const auto memory = mm::idOf(mm::create(s.userA, mm::terms("m", {aged(s)})));
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockAlbum, memory).ok);
                CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("n", {aged(s)})), mm::SO::Created));
                CHECK(c::commitTx(hold).value_or(false));
            }
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "album-direct-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2a: the deferred trigger leaves memory writes alone, and refuses an album binding with no matching item") {
        try {
            md::useStore();
            const auto s = md::space();
            // Service create, with 1 and with 9 photos.
            const auto one = aged(s);
            const auto single = mm::create(s.userA, mm::terms("one", {one}));
            CHECK(mm::is(single, mm::SO::Created));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid AND target_kind = 'memory' AND target_id = $2::uuid", one, mm::idOf(single)), 1);
            Ids nine;
            for (int i = 0; i < 9; ++i) nine.push_back(aged(s));
            const auto full = mm::create(s.userA, mm::terms("nine", nine));
            CHECK(mm::is(full, mm::SO::Created));
            if (full) CHECK_EQ(full->memory["photos"].size(), 9u);
            // Creator edit, retained and new.
            const auto id = mm::idOf(single);
            const auto extra = aged(s);
            const auto edited = mm::edit(s.userA, id, 1, mm::terms("edited", {extra, one}));
            CHECK(mm::is(edited, mm::SO::Created));
            CHECK_EQ(countOf(kBindingsSql, one), 1);
            CHECK_EQ(countOf(kBindingsSql, extra), 1);
            // Approval: a forced failure rolls everything back and the same key then succeeds; the approver binds; a replay writes nothing.
            const auto partner = aged(s, s.userB);
            const auto pending = mm::propose(s.userB, id, 2, mm::terms("proposed", {partner, one}));
            CHECK(mm::is(pending, mm::SO::Created));
            const auto proposal = mm::proposalOf(pending);
            const std::string key = newKey();
            CHECK(mm::is(mm::decide(s.userA, id, proposal, mm::Decision::Approve, key, mm::S::Fault::FailPhotoBinding), mm::SO::Error));
            CHECK_EQ(countOf(kBindingsSql, partner), 0);
            const auto approved = mm::decide(s.userA, id, proposal, mm::Decision::Approve, key);
            CHECK(mm::is(approved, mm::SO::Ok));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid AND bound_by = $2::uuid AND target_kind = 'memory'", partner, s.userA), 1);
            CHECK(mm::is(mm::decide(s.userA, id, proposal, mm::Decision::Approve, key), mm::SO::Replayed));
            CHECK_EQ(countOf(kBindingsSql, partner), 1);

            // Not queued: in one transaction, a memory with its binding and photo, then the deferred checks forced now.
            {
                const auto photo = aged(s);
                const auto memory = c::newUuid();
                auto tx = c::acquireTx(holderConn->client);
                CHECK(c::exec(tx, "INSERT INTO memories (id, couple_id, creator_id) VALUES ($1::uuid, $2::uuid, $3::uuid)", memory, s.couple, s.userA).ok);
                const auto text = c::exec(tx, "INSERT INTO memory_texts (couple_id, title) VALUES ($1::uuid, 'direct') RETURNING id::text AS id", s.couple);
                CHECK(text.ok);
                CHECK(c::exec(tx, "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint) "
                                  "VALUES ($1::uuid, $2::uuid, 1, $3::uuid, 'create', DATE '2026-01-01', $4::uuid, $5, repeat('a', 64))",
                              memory, s.couple, s.userA, (*text.result)[0]["id"].as<std::string>(), newKey()).ok);
                CHECK(c::exec(tx, "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)",
                              photo, s.couple, memory, s.userA).ok);
                CHECK(c::exec(tx, "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) VALUES ($1::uuid, 1, 1, $2::uuid, $3::uuid)",
                              memory, photo, s.couple).ok);
                CHECK(c::exec(tx, "SET CONSTRAINTS ALL IMMEDIATE").ok);
                c::rollbackTx(tx);
            }
            const auto album = idOf(create(s.userA, "a"));
            const auto albumTwo = idOf(create(s.userA, "b"));
            // An album binding with no item at all.
            {
                const auto photo = aged(s);
                auto tx = c::acquireTx(holderConn->client);
                CHECK(c::exec(tx, kBindAlbumSql, photo, s.couple, album, s.userA).ok);
                CHECK(refusedWith(c::exec(tx, "SET CONSTRAINTS ALL IMMEDIATE"), "USAPP_ALBUM_BINDING_ITEM"));
                c::rollbackTx(tx);
            }
            // At real commit, too: the commit fails and nothing stays.
            {
                const auto photo = aged(s);
                auto tx = c::acquireTx(holderConn->client);
                CHECK(c::exec(tx, kBindAlbumSql, photo, s.couple, album, s.userA).ok);
                CHECK_FALSE(c::commitTx(tx).value_or(true));
                CHECK_EQ(countOf(kBindingsSql, photo), 0);
            }
            const char *batchSql = "INSERT INTO album_photo_batches (id, album_id, couple_id, added_by, idempotency_key, request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, repeat('a', 64))";
            const char *itemSql = "INSERT INTO album_items (album_id, seq, couple_id, media_id, batch_id, added_by) VALUES ($1::uuid, 1, $2::uuid, $3::uuid, $4::uuid, $5::uuid)";
            // An item in a different album from its binding.
            {
                const auto photo = aged(s);
                const auto batch = c::newUuid();
                auto tx = c::acquireTx(holderConn->client);
                CHECK(c::exec(tx, kBindAlbumSql, photo, s.couple, album, s.userA).ok);
                CHECK(c::exec(tx, batchSql, batch, albumTwo, s.couple, s.userA, newKey()).ok);
                CHECK(refusedWith(c::exec(tx, itemSql, albumTwo, s.couple, photo, batch, s.userA), "USAPP_ALBUM_ITEM_UNBOUND"));
                c::rollbackTx(tx);
            }
            // bound_by differing from added_by.
            {
                const auto photo = aged(s);
                const auto batch = c::newUuid();
                auto tx = c::acquireTx(holderConn->client);
                CHECK(c::exec(tx, kBindAlbumSql, photo, s.couple, albumTwo, s.userA).ok);
                CHECK(c::exec(tx, batchSql, batch, albumTwo, s.couple, s.userB, newKey()).ok);
                CHECK(refusedWith(c::exec(tx, itemSql, albumTwo, s.couple, photo, batch, s.userB), "USAPP_ALBUM_ITEM_UNBOUND"));
                c::rollbackTx(tx);
            }
            // Its definition.
            CHECK_EQ(obs().client->execSqlSync(
                         "SELECT count(*)::bigint AS n FROM pg_trigger t WHERE t.tgrelid = 'public.media_bindings'::regclass AND t.tgname = 'media_bindings_album_item' "
                         "AND t.tgdeferrable AND t.tginitdeferred AND t.tgconstraint <> 0 AND pg_get_triggerdef(t.oid) LIKE '%AFTER INSERT ON%' "
                         "AND pg_get_triggerdef(t.oid) LIKE '%''album''%' AND pg_get_triggerdef(t.oid) NOT LIKE '%UPDATE%' AND pg_get_triggerdef(t.oid) NOT LIKE '%DELETE%'")[0]["n"].as<long long>(), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "album-deferred-trigger", "case failed (details withheld)");
        }
    }

    holderConn.reset();
    waiterConn.reset();
}

}  // namespace usapptest::album
