#pragma once

// D6: the real MediaService, MemoryService, AlbumService and cleanup job against FakeObjectStore, on literal usapp_test. No vendor and
// no credential is involved. Rules read clock_timestamp(), so a deadline is placed by committing a cycle whose deleted_at is chosen,
// and the exact instant is covered through usapp_media_undo_open. Assertions target this run's ids only.
#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "AlbumDbTests.h"

namespace usapptest::mediadeletion {

namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace al = usapptest::album;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using M = usapp::MediaService;
using MO = M::Outcome;
using SO = usapp::MemoryService::Outcome;
using AO = usapp::AlbumService::Outcome;

inline std::optional<c::Observer> holderConn;

inline const c::Observer &obs() { return md::obs(); }
inline std::optional<M::Status> erase(const std::string &user, const std::string &id, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<md::PendingStatus>();
    M::instance().deletePhoto(user, id, key, [p](M::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline std::shared_ptr<md::PendingStatus> startUndo(const std::string &user, const std::string &id, const std::string &deletion, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<md::PendingStatus>();
    M::instance().undoDeletion(user, id, deletion, key, [p](M::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::optional<M::Status> undo(const std::string &user, const std::string &id, const std::string &deletion, const std::string &key = c::newUuid()) {
    return startUndo(user, id, deletion, key)->wait();
}
inline std::optional<M::DeletionPage> undoablePage(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<std::pair<std::string, std::string>> after;
    if (cursor) {
        const auto decoded = usapp::AlbumService::decodeCursor(*cursor);
        if (!decoded) throw std::runtime_error("cursor");
        after = std::make_pair(decoded->createdAt, decoded->id);
    }
    auto p = std::make_shared<ts::Pending<M::DeletionPage>>();
    M::instance().listUndoable(user, limit, after, [p](M::DeletionPage page) { p->deliver(std::move(page)); });
    return p->wait();
}
// Every deletion id of a whole walk, in order.
inline std::vector<std::string> undoableIds(const std::string &user, int limit = 100) {
    std::vector<std::string> ids;
    std::optional<std::string> cursor;
    do {
        const auto page = undoablePage(user, limit, cursor);
        if (!page || page->outcome != MO::Ok) throw std::runtime_error("page");
        for (const auto &item : page->items) ids.push_back(item["deletion_id"].asString());
        cursor = page->nextCursor;
    } while (cursor);
    return ids;
}
inline std::shared_ptr<md::PendingStatus> startContent(const std::string &user, const std::string &id, const std::string &variant = "view") {
    auto p = std::make_shared<md::PendingStatus>();
    M::instance().content(user, id, variant, [p](M::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::string cycleOf(const std::optional<M::Status> &s) { return s ? s->deletion["deletion_id"].asString() : std::string(); }
inline std::string stateOf(const std::optional<M::Status> &s) { return s ? s->deletion["state"].asString() : std::string(); }
inline long long deletions(const std::string &id) { return md::count("SELECT count(*)::bigint AS n FROM media_deletions WHERE media_id = $1::uuid", id); }
inline long long reversals(const std::string &id) { return md::count("SELECT count(*)::bigint AS n FROM media_deletion_reversals WHERE media_id = $1::uuid", id); }
inline long long purgePlans(const std::string &id) { return md::count("SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE media_id = $1::uuid AND reason = 'user_deleted'", id); }
inline long long sweeps(const std::string &id) { return md::count("SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid AND scope = 'everything' AND kind = 'sweep'", id); }
inline long long bindings(const std::string &id) { return md::count("SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid", id); }

// Grant, upload, complete: READY, through the service.
inline std::string readyPhoto(const std::string &user) {
    const auto u = md::uploaded(user);
    if (!md::is(md::complete(user, u.id), MO::Ok)) throw std::runtime_error("not ready");
    return u.id;
}
// Deleted `ago` before the database's now, with the deadline that implies. Only a cycle committed directly can have a chosen age. The
// age is written as an interval by the test itself, never from outside.
inline std::string cycleAged(const std::string &id, const std::string &couple, const std::string &user, const std::string &ago, const std::string &key = c::newUuid()) {
    return obs().client->execSqlSync(
        "INSERT INTO media_deletions (media_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
        "SELECT $1::uuid, $2::uuid, $3::uuid, 1 + COALESCE((SELECT max(x.cycle_no) FROM media_deletions x WHERE x.media_id = $1::uuid), 0), $4, $6, n.t, n.t + interval '168 hours' "
        "FROM (SELECT clock_timestamp() - $5::interval AS t) n RETURNING id::text AS id",
        id, couple, user, key, ago, M::deletionFingerprint(id))[0]["id"].as<std::string>();
}
// An aged READY photo whose objects really exist in the fake.
inline std::string agedWithObjects(const ts::CommittedSpace &s, const std::string &uploader) {
    const auto id = md::agedReady(md::env.pool, s, uploader);
    md::env.store->lateClientWrite(md::stagingOf(id), md::image());
    md::env.store->lateClientWrite(md::attemptKey(id, 1), md::image());
    return id;
}
// The same, with a losing attempt beside the READY one: every key of the object is a target of a purge.
inline std::string agedTwoAttempts(const ts::CommittedSpace &s, const std::string &uploader) {
    const auto id = c::newUuid();
    auto &pool = md::env.pool;
    pool->execSqlSync(
        "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '9 days', $4, repeat('a', 64), clock_timestamp() - interval '9 days 00:10:00')",
        id, s.couple, uploader, c::newUuid());
    for (const char *no : {"1", "2"}) {
        pool->execSqlSync(
            "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
            "VALUES ($1::uuid, $3::integer, $2::uuid, 'media/' || $2 || '/' || $1 || '/' || gen_random_uuid() || '/original', clock_timestamp() - interval '9 days')",
            id, s.couple, std::string(no));
    }
    pool->execSqlSync(
        "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
        "VALUES ($1::uuid, $2::uuid, 'ready', 2, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '8 days')", id, s.couple);
    md::env.store->lateClientWrite(md::stagingOf(id), md::image());
    md::env.store->lateClientWrite(md::attemptKey(id, 1), md::image());
    md::env.store->lateClientWrite(md::attemptKey(id, 2), md::image());
    return id;
}
inline const char *kPlanUserDeleted =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before, deletion_id) VALUES ($1::uuid, 'everything', 'user_deleted', clock_timestamp(), $2::uuid)";
inline bool refused(const std::function<void()> &action) {
    try {
        action();
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
inline bool undoOpenAt(const std::string &deadline, const std::string &now) {
    return obs().client->execSqlSync("SELECT usapp_media_undo_open($1::timestamptz, $2::timestamptz) AS v", deadline, now)[0]["v"].as<bool>();
}
inline void waitPastDeadline(const std::string &deletion) {
    md::waitUntil([&]() {
        return obs().client->execSqlSync("SELECT clock_timestamp() >= purge_after AS v FROM media_deletions WHERE id = $1::uuid", deletion)[0]["v"].as<bool>();
    });
}

inline void runMediaDeletionDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    holderConn = c::Observer::connect(dsn);

    TEST_CASE("D6: uploader only; partner and another space cannot delete, undo or read; every read stops, with no signing") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            // A's photo in a memory B created: approved by B, so B may read it.
            const auto mine = readyPhoto(s.userB);
            const auto theirs = readyPhoto(s.userA);
            const auto memory = mm::create(s.userB, mm::terms("B's memory", {mine}));
            CHECK(mm::is(memory, SO::Created));
            const auto proposal = mm::propose(s.userA, mm::idOf(memory), 1, mm::terms("with A's photo", {mine, theirs}));
            CHECK(mm::is(proposal, SO::Created));
            CHECK(mm::is(mm::decide(s.userB, mm::idOf(memory), mm::proposalOf(proposal), mm::Decision::Approve), SO::Ok));
            CHECK(md::is(md::content(s.userB, theirs, "view"), MO::Ok));
            CHECK_EQ(bindings(theirs), 1);
            const auto bindingBefore = al::bindingRow(theirs);

            // Refused before anything is written; each looks like an absent photo.
            CHECK(md::is(erase(s.userB, theirs), MO::NotFound));
            CHECK(md::is(erase(other.userA, theirs), MO::NotFound));
            CHECK(md::is(erase(s.userA, c::newUuid()), MO::NotFound));
            CHECK_EQ(deletions(theirs), 0);

            const auto deleted = erase(s.userA, theirs);
            CHECK(md::is(deleted, MO::Created));
            CHECK_EQ(stateOf(deleted), std::string("deleted"));
            Json::StreamWriterBuilder writer;
            if (deleted) CHECK(Json::writeString(writer, deleted->deletion).find("media/") == std::string::npos);
            CHECK_EQ(al::bindingRow(theirs), bindingBefore);

            const int signedBefore = store->readSignings.load();
            for (const auto &user : {s.userA, s.userB, other.userA}) {
                CHECK(md::is(md::facts(user, theirs), MO::NotFound));
                for (const char *variant : {"thumb", "view", "original"}) CHECK(md::is(md::content(user, theirs, variant), MO::NotFound));
            }
            CHECK(md::is(md::complete(s.userA, theirs), MO::NotFound));
            CHECK_EQ(store->readSignings.load(), signedBefore);

            // Neither the partner nor another space can undo; a cycle under the wrong photo does not exist.
            CHECK(md::is(undo(s.userB, theirs, cycleOf(deleted)), MO::NotFound));
            CHECK(md::is(undo(other.userA, theirs, cycleOf(deleted)), MO::NotFound));
            CHECK(md::is(undo(s.userA, mine, cycleOf(deleted)), MO::NotFound));
            CHECK_EQ(reversals(theirs), 0);

            // The original deleter restores exactly what was there.
            CHECK(md::is(undo(s.userA, theirs, cycleOf(deleted)), MO::Ok));
            CHECK(md::is(md::content(s.userB, theirs, "view"), MO::Ok));
            CHECK(md::is(md::content(s.userB, theirs, "original"), MO::NotFound));
            CHECK(md::is(md::content(s.userA, theirs, "original"), MO::Ok));
            CHECK_EQ(al::bindingRow(theirs), bindingBefore);
            // Not READY: nothing to delete. No store is needed to delete, and the job then writes nothing.
            const auto granted = md::grant(s.userA);
            CHECK(md::is(erase(s.userA, md::idOf(granted)), MO::NotFound));
            M::instance().configure(M::Policy{}, nullptr);
            CHECK(md::is(erase(s.userB, mine), MO::Created));
            md::useStore();
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "d6-permissions", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: a read that starts after the commit signs nothing, while one finished earlier keeps its URL until the purge") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto id = agedWithObjects(s, s.userA);
            const auto issued = md::content(s.userA, id, "view");
            CHECK(md::is(issued, MO::Ok));
            const std::string url = issued ? issued->redirectUrl : std::string();
            CHECK_EQ(store->followRead(url), md::image());

            // A read waiting behind the uncommitted deletion sees its commit and refuses.
            const auto cycle = c::newUuid();
            {
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold,
                    "INSERT INTO media_deletions (id, media_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
                    "SELECT $1::uuid, $2::uuid, $3::uuid, $4::uuid, 1, $5, repeat('a', 64), n.t, n.t + interval '168 hours' FROM (SELECT clock_timestamp() AS t) n",
                    cycle, id, s.couple, s.userA, c::newUuid()).ok);
                const int before = store->readSignings.load();
                auto reading = startContent(s.userA, id);
                al::waitLockWaiters(1);
                CHECK_EQ(store->readSignings.load(), before);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(md::is(reading->wait(), MO::NotFound));
                CHECK_EQ(store->readSignings.load(), before);
            }
            // The URL already issued is not recalled by the deletion: this is the documented difference.
            CHECK_EQ(store->followRead(url), md::image());
            CHECK(md::is(md::content(s.userA, id, "view"), MO::NotFound));
            // An undo restores new reads.
            CHECK(md::is(undo(s.userA, id, cycle), MO::Ok));
            CHECK(md::is(md::content(s.userA, id, "view"), MO::Ok));

            // A URL issued before a deletion stops working when the object is purged, and not before.
            const auto purged = agedWithObjects(s, s.userA);
            const auto lastUrl = md::content(s.userA, purged, "view");
            CHECK(md::is(lastUrl, MO::Ok));
            const std::string oldUrl = lastUrl ? lastUrl->redirectUrl : std::string();
            cycleAged(purged, s.couple, s.userA, "168 hours 1 second");
            CHECK_EQ(store->followRead(oldUrl), md::image());
            md::tickUntil([&]() { return sweeps(purged) == 1; });
            CHECK_EQ(store->followRead(oldUrl), std::string());
        } catch (const std::exception &) {
            c::expectThat(false, "d6-read-order", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: an album hides a deleted photo, its count and its cover falls back; an undo restores all of it") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto first = readyPhoto(s.userA);
            const auto second = readyPhoto(s.userA);
            const auto partner = readyPhoto(s.userB);
            const auto album = al::create(s.userA, "cover");
            const auto id = al::idOf(album);
            CHECK(al::is(al::add(s.userA, id, {first, second}), AO::Created));
            CHECK(al::is(al::add(s.userB, id, {partner}), AO::Created));
            CHECK(al::is(al::cover(s.userB, id, second), AO::Created));
            CHECK_EQ(al::coverOf(al::getAlbum(s.userB, id)), second);

            const auto deleted = erase(s.userA, second);
            CHECK(md::is(deleted, MO::Created));
            const auto without = al::getAlbum(s.userB, id);
            // Back to the highest active photo, not chosen.
            CHECK_EQ(al::coverOf(without), partner);
            if (without) CHECK(!without->album["cover"]["chosen"].asBool());
            if (without) CHECK_EQ(without->album["photo_count"].asInt(), 2);
            CHECK_EQ(al::photoIds(s.userA, id).size(), 2u);
            // Neither member can choose it; the partner may not even see it to remove; the item stays.
            CHECK(al::is(al::cover(s.userB, id, second), AO::PhotoNotFound));
            CHECK(al::is(al::cover(s.userA, id, second), AO::PhotoNotFound));
            CHECK(al::is(al::removePhoto(s.userB, id, second), AO::PhotoNotFound));
            CHECK_EQ(c::scalar(obs(), al::kItemsOfMediaSql, second), 1);
            // Not rebound: the home is still this album.
            CHECK_EQ(bindings(second), 1);

            CHECK(md::is(undo(s.userA, second, cycleOf(deleted)), MO::Ok));
            const auto restored = al::getAlbum(s.userB, id);
            CHECK_EQ(al::coverOf(restored), second);
            if (restored) CHECK(restored->album["cover"]["chosen"].asBool());
            if (restored) CHECK_EQ(restored->album["photo_count"].asInt(), 3);

            // Every photo gone: an empty album, not an error.
            for (const auto &[user, photo] : {std::pair{s.userA, first}, std::pair{s.userA, second}, std::pair{s.userB, partner}}) {
                CHECK(md::is(erase(user, photo), MO::Created));
            }
            const auto empty = al::getAlbum(s.userA, id);
            if (empty) CHECK(empty->album["cover"].isNull());
            if (empty) CHECK_EQ(empty->album["photo_count"].asInt(), 0);
            CHECK(al::photoIds(s.userA, id).empty());
            // An unattached photo, once deleted, cannot join.
            const auto loose = readyPhoto(s.userA);
            CHECK(md::is(erase(s.userA, loose), MO::Created));
            CHECK(al::is(al::add(s.userA, id, {loose}), AO::PhotoUnavailable));
            CHECK_EQ(bindings(loose), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "d6-album", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: a memory keeps its text and positions when every photo is deleted, and an edit cannot keep a deleted photo") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto one = readyPhoto(s.userA);
            const auto two = readyPhoto(s.userA);
            const auto made = mm::create(s.userA, mm::terms("Kept title", {one, two}, "2026-03-01", "Kept words"));
            const auto id = mm::idOf(made);
            CHECK(mm::is(made, SO::Created));
            CHECK(md::is(erase(s.userA, one), MO::Created));
            CHECK(md::is(erase(s.userA, two), MO::Created));
            const int signedBefore = store->readSignings.load();
            for (const auto &user : {s.userA, s.userB}) {
                const auto read = mm::getMemory(user, id);
                CHECK(mm::is(read, SO::Ok));
                if (!read) continue;
                CHECK_EQ(read->memory["title"].asString(), std::string("Kept title"));
                CHECK_EQ(read->memory["description"].asString(), std::string("Kept words"));
                CHECK_EQ(read->memory["version"].asInt(), 1);
                CHECK_EQ(read->memory["photos"].size(), 2u);
                for (Json::ArrayIndex i = 0; i < read->memory["photos"].size(); ++i) {
                    const auto &photo = read->memory["photos"][i];
                    CHECK_EQ(photo["position"].asInt(), static_cast<int>(i) + 1);
                    CHECK(photo["deleted"].asBool());
                    CHECK(photo["width"].isNull());
                    CHECK(photo["height"].isNull());
                    CHECK_EQ(photo["media_id"].asString(), i == 0 ? one : two);
                    CHECK(md::is(md::content(user, photo["media_id"].asString()), MO::NotFound));
                }
                Json::StreamWriterBuilder writer;
                CHECK(Json::writeString(writer, read->memory).find("media/") == std::string::npos);
            }
            CHECK_EQ(store->readSignings.load(), signedBefore);
            const auto page = mm::listPage(s.userB, 50);
            CHECK(page.has_value());
            // An edit may not keep it, and can replace it. The history stays.
            CHECK(mm::is(mm::edit(s.userA, id, 1, mm::terms("Edited", {one})), SO::PhotoUnavailable));
            CHECK_EQ(c::scalar(obs(), mm::kRevisionsSql, id), 1);
            const auto fresh = readyPhoto(s.userA);
            CHECK(mm::is(mm::edit(s.userA, id, 1, mm::terms("Edited", {fresh})), SO::Created));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revision_photos WHERE memory_id = $1::uuid AND version = 1", id), 2);
            // A new memory cannot name one.
            CHECK(mm::is(mm::create(s.userA, mm::terms("New", {one})), SO::PhotoUnavailable));
        } catch (const std::exception &) {
            c::expectThat(false, "d6-memory", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: a pending proposal naming a deleted photo cannot be approved; an undo approves nothing, and then it can") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto base = readyPhoto(s.userB);
            const auto reserved = readyPhoto(s.userA);
            const auto made = mm::create(s.userB, mm::terms("Base", {base}));
            const auto id = mm::idOf(made);
            const auto proposal = mm::propose(s.userA, id, 1, mm::terms("Proposed", {base, reserved}));
            CHECK(mm::is(proposal, SO::Created));
            const auto proposalId = mm::proposalOf(proposal);

            // Reserved does not stop its uploader. It stays unbound.
            const auto deleted = erase(s.userA, reserved);
            CHECK(md::is(deleted, MO::Created));
            CHECK(mm::is(mm::decide(s.userB, id, proposalId, mm::Decision::Approve), SO::PhotoUnavailable));
            CHECK_EQ(c::scalar(obs(), mm::kDecisionsSql, proposalId), 0);
            CHECK_EQ(c::scalar(obs(), mm::kRevisionsSql, id), 1);
            CHECK_EQ(bindings(reserved), 0);
            // A review shows the placeholder.
            const auto review = mm::getProposal(s.userB, id, proposalId);
            if (review) CHECK(review->proposal["proposed"]["photos"][1]["deleted"].asBool());
            // No new proposal can name it either, and the pending one is still there.
            CHECK(md::is(undo(s.userA, reserved, cycleOf(deleted)), MO::Ok));
            CHECK_EQ(c::scalar(obs(), mm::kDecisionsSql, proposalId), 0);
            CHECK_EQ(c::scalar(obs(), mm::kRevisionsSql, id), 1);
            CHECK(mm::is(mm::decide(s.userB, id, proposalId, mm::Decision::Approve), SO::Ok));
            CHECK_EQ(c::scalar(obs(), mm::kRevisionsSql, id), 2);

            // A kept photo deleted by its uploader refuses in the same way, and a rejection still works.
            const auto second = mm::propose(s.userA, id, 2, mm::terms("Again", {base}));
            CHECK(mm::is(second, SO::Created));
            CHECK(md::is(erase(s.userB, base), MO::Created));
            CHECK(mm::is(mm::decide(s.userB, id, mm::proposalOf(second), mm::Decision::Approve), SO::PhotoUnavailable));
            CHECK(mm::is(mm::decide(s.userB, id, mm::proposalOf(second), mm::Decision::Reject), SO::Ok));
            CHECK(mm::is(mm::propose(s.userA, id, 2, mm::terms("Naming it", {base})), SO::PhotoUnavailable));
        } catch (const std::exception &) {
            c::expectThat(false, "d6-approval", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: same-key replay moves no deadline and touches no later cycle; stale undo refuses; repeated cycles each get their own deadline") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = readyPhoto(s.userA);
            const auto otherPhoto = readyPhoto(s.userA);
            const std::string deleteKey = c::newUuid(), undoKey = c::newUuid();

            const auto first = erase(s.userA, id, deleteKey);
            CHECK(md::is(first, MO::Created));
            const auto deadline = first ? first->deletion["purge_after"].asString() : std::string();
            CHECK(!deadline.empty());
            const auto replay = erase(s.userA, id, deleteKey);
            CHECK(md::is(replay, MO::Replayed));
            CHECK_EQ(cycleOf(replay), cycleOf(first));
            if (replay) CHECK_EQ(replay->deletion["purge_after"].asString(), deadline);
            CHECK_EQ(deletions(id), 1);
            // The same key for another target is a conflict, and writes nothing.
            CHECK(md::is(erase(s.userA, otherPhoto, deleteKey), MO::KeyReused));
            CHECK_EQ(deletions(otherPhoto), 0);
            // A second deletion with a new key.
            CHECK(md::is(erase(s.userA, id), MO::AlreadyDeleted));
            CHECK_EQ(deletions(id), 1);

            const auto undone = undo(s.userA, id, cycleOf(first), undoKey);
            CHECK(md::is(undone, MO::Ok));
            CHECK_EQ(stateOf(undone), std::string("undone"));
            CHECK(md::is(undo(s.userA, id, c::newUuid(), undoKey), MO::KeyReused));

            // Cycle 2.
            const auto second = erase(s.userA, id);
            CHECK(md::is(second, MO::Created));
            if (second) CHECK_EQ(second->deletion["cycle_no"].asInt(), 2);
            CHECK(cycleOf(second) != cycleOf(first));
            const auto secondDeadline = second ? second->deletion["purge_after"].asString() : std::string();
            CHECK(secondDeadline > deadline);

            // Everything about cycle 1 answers about cycle 1 and leaves cycle 2 standing.
            const auto oldDelete = erase(s.userA, id, deleteKey);
            CHECK(md::is(oldDelete, MO::Replayed));
            CHECK_EQ(cycleOf(oldDelete), cycleOf(first));
            CHECK_EQ(stateOf(oldDelete), std::string("undone"));
            if (oldDelete) CHECK_EQ(oldDelete->deletion["purge_after"].asString(), deadline);
            const auto oldUndo = undo(s.userA, id, cycleOf(first), undoKey);
            CHECK(md::is(oldUndo, MO::Replayed));
            CHECK_EQ(cycleOf(oldUndo), cycleOf(first));
            CHECK(md::is(undo(s.userA, id, cycleOf(first)), MO::DeletionNotActive));
            CHECK_EQ(reversals(id), 1);
            CHECK(md::is(md::content(s.userA, id), MO::NotFound));

            // Cycle 2's own undo, and its replay, still work.
            // Keys belong to one operation: the delete key may also name an undo.
            const std::string secondKey = deleteKey;
            CHECK(md::is(undo(s.userA, id, cycleOf(second), secondKey), MO::Ok));
            CHECK(md::is(md::content(s.userA, id), MO::Ok));
            CHECK(md::is(undo(s.userA, id, cycleOf(second), secondKey), MO::Replayed));
            CHECK_EQ(reversals(id), 2);
            // An old replay does not re-delete a restored photo.
            CHECK(md::is(erase(s.userA, id, deleteKey), MO::Replayed));
            CHECK(md::is(md::content(s.userA, id), MO::Ok));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'media' AND target_id = $1::text AND action = 'media.deleted'", id), 2);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'media' AND target_id = $1::text AND action = 'media.deletion_undone'", id), 2);
        } catch (const std::exception &) {
            c::expectThat(false, "d6-replay", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: the deadline is exact: open strictly before it, closed at it, and a purge cannot be planned early") {
        try {
            md::useStore();
            const auto s = md::space();
            const std::string at = "2026-06-01 12:00:00+00";
            CHECK(undoOpenAt(at, "2026-06-01 11:59:59.999999+00"));
            CHECK_FALSE(undoOpenAt(at, at));
            CHECK_FALSE(undoOpenAt(at, "2026-06-01 12:00:00.000001+00"));

            // A minute left: undo works, and nothing can be planned.
            const auto open = md::agedReady(pool, s, s.userA);
            const auto openCycle = cycleAged(open, s.couple, s.userA, "167 hours 59 minutes");
            CHECK(refused([&]() { obs().client->execSqlSync(kPlanUserDeleted, open, openCycle); }));
            md::tick();
            CHECK_EQ(purgePlans(open), 0);
            CHECK(md::is(undo(s.userA, open, openCycle), MO::Ok));

            // A second past: no plan yet, and undo is already closed. The clock alone closes it.
            const auto closed = md::agedReady(pool, s, s.userA);
            const auto closedCycle = cycleAged(closed, s.couple, s.userA, "168 hours 1 second");
            CHECK(md::is(undo(s.userA, closed, closedCycle), MO::UndoClosed));
            CHECK_EQ(reversals(closed), 0);
            CHECK(refused([&]() {
                obs().client->execSqlSync(
                    "INSERT INTO media_deletion_reversals (deletion_id, media_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, repeat('a', 64))", closedCycle, closed, s.couple, s.userA, c::newUuid());
            }));
        } catch (const std::exception &) {
            c::expectThat(false, "d6-deadline", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: undo and the purge race on 33018 in both orders") {
        try {
            md::useStore();
            const auto s = md::space();

            // Undo first: it commits under the lock while the job waits, having read the cycle as due. The job then plans nothing.
            {
                const auto id = md::agedReady(pool, s, s.userA);
                const auto cycle = cycleAged(id, s.couple, s.userA, "167 hours 59 minutes 58 seconds");
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold,
                    "INSERT INTO media_deletion_reversals (deletion_id, media_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, repeat('a', 64))", cycle, id, s.couple, s.userA, c::newUuid()).ok);
                waitPastDeadline(cycle);
                auto ticking = al::startTick();
                al::waitLockWaiters(1);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(ticking.done->wait().has_value());
                CHECK_EQ(*ticking.outcome, std::string("completed"));
                CHECK_EQ(purgePlans(id), 0);
                CHECK(md::is(md::facts(s.userA, id), MO::Ok));
            }

            // The purge first: the plan commits under the lock while the undo waits. The undo then refuses, and nothing changes.
            {
                const auto id = md::agedReady(pool, s, s.userA);
                const auto cycle = cycleAged(id, s.couple, s.userA, "168 hours 1 second");
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kPlanUserDeleted, id, cycle).ok);
                auto undoing = startUndo(s.userA, id, cycle);
                al::waitLockWaiters(1);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(md::is(undoing->wait(), MO::UndoClosed));
                CHECK_EQ(reversals(id), 0);
                CHECK_EQ(purgePlans(id), 1);
                CHECK(md::is(md::facts(s.userA, id), MO::NotFound));
            }

            // A deletion and a binding race on the same lock: whichever is second refuses.
            {
                const auto id = readyPhoto(s.userA);
                const auto album = al::idOf(al::create(s.userA, "race"));
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, mm::kLockMedia, id).ok);
                auto p = std::make_shared<md::PendingStatus>();
                M::instance().deletePhoto(s.userA, id, c::newUuid(), [p](M::Status status) { p->deliver(std::move(status)); });
                al::waitLockWaiters(1);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(md::is(p->wait(), MO::Created));
                CHECK(al::is(al::add(s.userA, album, {id}), AO::PhotoUnavailable));
                CHECK_EQ(bindings(id), 0);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "d6-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: ordinary cleanup stays closed; only the exact expired, unreversed cycle may plan a bound or reserved photo") {
        try {
            md::useStore();
            const auto s = md::space();

            // Bound: ordinary reasons refuse, deleted or not, and so does a cycle that is not due.
            const auto memory = md::anchorMemory(pool, s);
            const auto bound = obs().client->execSqlSync("SELECT media_id::text AS v FROM memory_revision_photos WHERE memory_id = $1::uuid", memory)[0]["v"].as<std::string>();
            const auto boundCycle = cycleAged(bound, s.couple, s.userA, "168 hours 1 second");
            CHECK(refused([&]() { obs().client->execSqlSync(mm::kPlanInsertSql, bound); }));

            // Another cycle's id is refused: the foreign key ties a plan to its own photo.
            const auto unrelated = md::agedReady(pool, s, s.userA);
            const auto unrelatedCycle = cycleAged(unrelated, s.couple, s.userA, "168 hours 1 second");
            CHECK(refused([&]() { obs().client->execSqlSync(kPlanUserDeleted, bound, unrelatedCycle); }));
            // A reason of user_deleted with no cycle fails the shape.
            CHECK(refused([&]() {
                obs().client->execSqlSync("INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) VALUES ($1::uuid, 'everything', 'user_deleted', clock_timestamp())", bound);
            }));

            // The job plans the due, exact cycle of a bound photo. The binding stays, and a reserved photo goes the same way.
            const auto reservedPhoto = md::agedReady(pool, s, s.userA);
            mm::agedProposal(s, reservedPhoto, "");
            const auto reservedCycle = cycleAged(reservedPhoto, s.couple, s.userA, "168 hours 1 second");
            md::tickUntil([&]() { return purgePlans(bound) == 1 && purgePlans(reservedPhoto) == 1 && purgePlans(unrelated) == 1; });
            CHECK_EQ(bindings(bound), 1);
            CHECK(!boundCycle.empty() && !reservedCycle.empty());

            // An unbound photo, older than the unattached rule, stays out of ordinary cleanup while deleted. Its undo restarts the period.
            const auto loose = md::agedReady(pool, s, s.userA);
            const auto looseCycle = cycleAged(loose, s.couple, s.userA, "1 hour");
            CHECK(refused([&]() { obs().client->execSqlSync(mm::kPlanInsertSql, loose); }));
            md::tick();
            CHECK_EQ(md::plans(loose), 0);
            CHECK(md::is(undo(s.userA, loose, looseCycle), MO::Ok));
            CHECK(refused([&]() { obs().client->execSqlSync(mm::kPlanInsertSql, loose); }));
            md::tick();
            CHECK_EQ(md::plans(loose), 0);
            CHECK(md::is(md::facts(s.userA, loose), MO::Ok));

            // An old, undone cycle is not due just because its deadline passes while a newer cycle runs.
            const auto cycled = md::agedReady(pool, s, s.userA);
            const auto oldCycle = cycleAged(cycled, s.couple, s.userA, "167 hours 59 minutes 58 seconds");
            CHECK(md::is(undo(s.userA, cycled, oldCycle), MO::Ok));
            CHECK(md::is(erase(s.userA, cycled), MO::Created));
            waitPastDeadline(oldCycle);
            CHECK(refused([&]() { obs().client->execSqlSync(kPlanUserDeleted, cycled, oldCycle); }));
            md::tick();
            CHECK_EQ(purgePlans(cycled), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "d6-guards", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: the plan commits before any store call; partial and failed deletion retry the same keys; completion needs all of them") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto id = agedTwoAttempts(s, s.userA);
            // Bound and not deleted: never eligible, so its READY key must survive everything below.
            const auto bystanderMemory = md::anchorMemory(pool, s);
            const auto bystander = obs().client->execSqlSync("SELECT media_id::text AS v FROM memory_revision_photos WHERE memory_id = $1::uuid", bystanderMemory)[0]["v"].as<std::string>();
            store->lateClientWrite(md::attemptKey(bystander, 1), md::image());
            const auto staging = md::stagingOf(id);
            const auto loser = md::attemptKey(id, 1);
            const auto final1 = md::attemptKey(id, 2);
            const auto cycle = cycleAged(id, s.couple, s.userA, "168 hours 1 second");

            std::atomic<long long> plansSeenAtFirstCall{-1};
            store->hook = [&](const std::string &op, const std::string &key) {
                if (op != "remove" || key.find(id) == std::string::npos) return;
                long long expected = -1;
                plansSeenAtFirstCall.compare_exchange_strong(expected, purgePlans(id));
            };
            store->failRemovalOf(final1);
            md::tickUntil([&]() {
                const auto asked = store->removalRequests();
                return std::count(asked.begin(), asked.end(), final1) >= 1 && std::count(asked.begin(), asked.end(), staging) >= 1 &&
                       std::count(asked.begin(), asked.end(), loser) >= 1;
            });
            CHECK_EQ(plansSeenAtFirstCall.load(), 1);
            store->hook = nullptr;
            // Partial: staging and the losing attempt are gone, the original is not, and nothing says finished.
            md::waitUntil([&]() { return !store->exists(staging) && !store->exists(loser); });
            CHECK(store->exists(final1));
            CHECK_EQ(sweeps(id), 0);
            CHECK_EQ(purgePlans(id), 1);
            CHECK(md::is(undo(s.userA, id, cycle), MO::UndoClosed));
            CHECK(md::is(md::content(s.userA, id), MO::NotFound));

            // A whole store outage, then the retry of exactly the same keys. An object already gone counts as confirmed.
            store->storeDown = 2;
            const auto before = store->removalRequests().size();
            md::tickUntil([&]() { return store->removalRequests().size() >= before + 2u; });
            CHECK_EQ(sweeps(id), 0);
            store->clearRemovalFailures();
            store->removeSaysNotFound = true;
            md::tickUntil([&]() { return sweeps(id) == 1; });
            CHECK_FALSE(store->exists(final1));
            CHECK_EQ(purgePlans(id), 1);
            const auto targets = store->removalRequests();
            for (const auto &key : targets) {
                if (key.find(id) != std::string::npos) CHECK(key == staging || key == loser || key == final1);
            }
            CHECK(std::count(targets.begin(), targets.end(), final1) >= 2);
            CHECK(std::count(targets.begin(), targets.end(), staging) >= 2);

            // Finished: nothing more is asked of the store, the binding-free row stays as evidence, and the other photo is untouched.
            const auto asked = store->removalRequests().size();
            md::tick();
            md::tick();
            const auto askedAfter = store->removalRequests();
            for (size_t i = asked; i < askedAfter.size(); ++i) CHECK(askedAfter[i].find(id) == std::string::npos);
            CHECK(store->exists(md::attemptKey(bystander, 1)));
            CHECK_EQ(deletions(bystander), 0);
            CHECK_EQ(md::disposition(md::attemptKey(bystander, 1)), std::string("keep"));
            CHECK_EQ(obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM media_objects WHERE id = $1::uuid", id)[0]["n"].as<long long>(), 1);

            // A late object after the sweep follows the existing rule: named late, removed by the reconciler, recorded.
            store->lateClientWrite(final1, md::image());
            CHECK_EQ(md::disposition(final1), std::string("late"));
            md::tickUntil([&]() { return !store->exists(final1); }, 8000);
            CHECK_EQ(md::lateRow(final1), std::string("removed/reconcile"));
        } catch (const std::exception &) {
            c::expectThat(false, "d6-purge", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: a purged cycle reports its state to a replay, and an uploaded photo through the whole path is gone") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto id = agedWithObjects(s, s.userA);
            const std::string key = c::newUuid();
            const auto first = erase(s.userA, id, key);
            CHECK(md::is(first, MO::Created));
            CHECK(md::is(undo(s.userA, id, cycleOf(first)), MO::Ok));
            // A later cycle may not be older than the last undo, so an undo can never be used to reach an old deadline.
            CHECK(refused([&]() { cycleAged(id, s.couple, s.userA, "168 hours 1 second"); }));
            const auto second = erase(s.userA, id);
            CHECK(md::is(second, MO::Created));
            CHECK(md::is(erase(s.userA, id, key), MO::Replayed));
            CHECK(store->exists(md::attemptKey(id, 1)));

            // A fresh aged photo goes all the way: a replay of its cycle then says purged.
            const auto gone = agedWithObjects(s, s.userA);
            const std::string goneKey = c::newUuid();
            const auto cycle = cycleAged(gone, s.couple, s.userA, "168 hours 1 second", goneKey);
            const std::string finalKey = md::attemptKey(gone, 1);
            md::tickUntil([&]() { return purgePlans(gone) == 1; });
            md::tickUntil([&]() { return sweeps(gone) == 1; });
            CHECK_FALSE(store->exists(finalKey));
            const auto replayed = erase(s.userA, gone, goneKey);
            CHECK(md::is(replayed, MO::Replayed));
            CHECK_EQ(stateOf(replayed), std::string("purged"));
            const auto facts = obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE deletion_id = $1::uuid", cycle);
            CHECK_EQ(facts[0]["n"].as<long long>(), 1);
            CHECK(md::is(undo(s.userA, gone, cycle), MO::UndoClosed));
            CHECK(md::is(erase(s.userA, gone), MO::AlreadyDeleted));
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "d6-purged", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: a deadline or plan that arrives after the service looked answers the undo-closed conflict, from the guard itself") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto attempt = [&](const std::string &id, const std::string &cycle) {
                auto p = std::make_shared<md::PendingStatus>();
                M::instance().undoDeletion(s.userA, id, cycle, c::newUuid(), [p](M::Status status) { p->deliver(std::move(status)); }, M::Fault::IgnoreUndoPrecheck);
                return p->wait();
            };
            // The fault reaches the insert with the precheck ignored, so what answers is the reversal guard.
            const auto expired = md::agedReady(pool, s, s.userA);
            const auto expiredCycle = cycleAged(expired, s.couple, s.userA, "168 hours 1 second");
            const auto late = attempt(expired, expiredCycle);
            CHECK(md::is(late, MO::UndoClosed));
            if (late) CHECK(late->deletion.isNull());
            CHECK_EQ(reversals(expired), 0);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'media' AND target_id = $1::text AND action = 'media.deletion_undone'", expired), 0);
            CHECK(md::is(md::facts(s.userA, expired), MO::NotFound));

            const auto planned = md::agedReady(pool, s, s.userA);
            const auto plannedCycle = cycleAged(planned, s.couple, s.userA, "168 hours 1 second");
            obs().client->execSqlSync(kPlanUserDeleted, planned, plannedCycle);
            CHECK(md::is(attempt(planned, plannedCycle), MO::UndoClosed));
            CHECK_EQ(reversals(planned), 0);

            // The failed statement left nothing behind: the connection and the same photo still work.
            const auto open = md::agedReady(pool, s, s.userA);
            const auto openCycle = cycleAged(open, s.couple, s.userA, "1 hour");
            CHECK(md::is(attempt(open, openCycle), MO::Ok));
            CHECK_EQ(reversals(open), 1);
            // The fault bypasses only the window and plan precheck: a cycle already undone keeps its own answer.
            CHECK(md::is(attempt(open, openCycle), MO::DeletionNotActive));
            CHECK_EQ(reversals(open), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "d6-undo-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: the undoable list pages the caller's own open cycles, exactly the ones an undo would accept") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            // An empty space answers an empty page, and someone in no space gets the usual refusal.
            const auto none = undoablePage(s.userA, 10);
            CHECK(none && none->outcome == MO::Ok && none->items.empty() && !none->nextCursor);
            const auto spaceless = c::newUuid();
            pool->execSqlSync("INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $1 || '@m3-fixture.test', $1 || '@m3-fixture.test')", spaceless);
            const auto outside = undoablePage(spaceless, 10);
            CHECK(outside && outside->outcome == MO::SpaceNotFound);

            const auto p1 = readyPhoto(s.userA), p2 = readyPhoto(s.userA), p3 = readyPhoto(s.userA);
            const auto d1 = cycleOf(erase(s.userA, p1));
            const auto d2 = cycleOf(erase(s.userA, p2));
            const auto d3 = cycleOf(erase(s.userA, p3));
            // Newest first, walked one row at a time and in pages, with no gap or repeat.
            const std::vector<std::string> newestFirst{d3, d2, d1};
            CHECK(undoableIds(s.userA, 1) == newestFirst);
            CHECK(undoableIds(s.userA, 2) == newestFirst);
            CHECK(undoableIds(s.userA, 3) == newestFirst);
            const auto first = undoablePage(s.userA, 2);
            CHECK(first && first->items.size() == 2u && first->nextCursor.has_value());
            if (first && !first->items.empty()) {
                // Exactly these facts, and nothing that could read the image.
                const auto &item = first->items[0];
                CHECK_EQ(item.size(), 6u);
                for (const char *name : {"deletion_id", "media_id", "deleted_at", "purge_after", "uploaded_at", "context"}) CHECK(item.isMember(name));
                CHECK_EQ(item["context"].size(), 5u);
                for (const char *name : {"width", "height", "content_type", "thumbnail", "url", "filename"}) CHECK(!item.isMember(name) && !item["context"].isMember(name));
                CHECK_EQ(item["media_id"].asString(), p3);
                Json::StreamWriterBuilder writer;
                const auto text = Json::writeString(writer, item);
                CHECK(text.find("media/") == std::string::npos);
                CHECK(text.find("staging") == std::string::npos);
                CHECK(text.find("fake://") == std::string::npos);
            }
            // A row leaving mid-walk moves nothing else.
            if (first && first->nextCursor) {
                CHECK(md::is(undo(s.userA, p2, d2), MO::Ok));
                const auto rest = undoablePage(s.userA, 2, first->nextCursor);
                CHECK(rest && rest->items.size() == 1u && !rest->nextCursor);
                if (rest && rest->items.size() == 1u) CHECK_EQ(rest->items[0]["deletion_id"].asString(), d1);
            }

            // The partner and another space see none of it, and their own lists stay their own.
            CHECK(undoableIds(s.userB).empty());
            CHECK(undoableIds(other.userA).empty());
            const auto partnerPhoto = readyPhoto(s.userB);
            const auto partnerCycle = cycleOf(erase(s.userB, partnerPhoto));
            CHECK(undoableIds(s.userB) == std::vector<std::string>{partnerCycle});
            CHECK(undoableIds(s.userA) == (std::vector<std::string>{d3, d1}));
            CHECK(undoableIds(other.userA).empty());

            // Undo then re-delete: only the newer cycle, and an undo removes it again.
            const auto replaced = cycleOf(erase(s.userA, p2));
            CHECK(replaced != d2);
            CHECK(undoableIds(s.userA) == (std::vector<std::string>{replaced, d3, d1}));
            CHECK(md::is(undo(s.userA, p2, replaced), MO::Ok));
            CHECK(undoableIds(s.userA) == (std::vector<std::string>{d3, d1}));

            // Expired with no plan yet, and purging: out, and an undo agrees.
            const auto late = md::space();
            const auto expired = md::agedReady(pool, late, late.userA);
            const auto expiredCycle = cycleAged(expired, late.couple, late.userA, "168 hours 1 second");
            const auto open = md::agedReady(pool, late, late.userA);
            const auto openCycle = cycleAged(open, late.couple, late.userA, "167 hours");
            CHECK(undoableIds(late.userA) == std::vector<std::string>{openCycle});
            CHECK(md::is(undo(late.userA, expired, expiredCycle), MO::UndoClosed));
            const auto purging = md::agedReady(pool, late, late.userA);
            const auto purgingCycle = cycleAged(purging, late.couple, late.userA, "168 hours 1 second");
            obs().client->execSqlSync(kPlanUserDeleted, purging, purgingCycle);
            CHECK(undoableIds(late.userA) == std::vector<std::string>{openCycle});
            CHECK(md::is(undo(late.userA, purging, purgingCycle), MO::UndoClosed));
            // What it lists, an undo accepts.
            CHECK(md::is(undo(late.userA, open, openCycle), MO::Ok));
            CHECK(undoableIds(late.userA).empty());
        } catch (const std::exception &) {
            c::expectThat(false, "d6-list", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: the recovery list names a readable parent, its current text and a reliable position, and nothing else") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto contextOf = [&](const std::string &user, const std::string &deletion) {
                const auto page = undoablePage(user, 100);
                if (!page || page->outcome != MO::Ok) throw std::runtime_error("page");
                for (const auto &item : page->items) if (item["deletion_id"].asString() == deletion) return item["context"];
                throw std::runtime_error("row");
            };
            const auto rawOf = [&](const std::string &user) {
                const auto page = undoablePage(user, 100);
                Json::StreamWriterBuilder writer;
                std::string all;
                if (page) for (const auto &item : page->items) all += Json::writeString(writer, item);
                return all;
            };

            // Unbound: generic, with the upload time.
            const auto loose = readyPhoto(s.userA);
            const auto looseCycle = cycleOf(erase(s.userA, loose));
            const auto none = contextOf(s.userA, looseCycle);
            CHECK_EQ(none["kind"].asString(), std::string("none"));
            CHECK(none["relation"].isNull() && none["target_id"].isNull() && none["label"].isNull() && none["position"].isNull());
            const auto uploaded = obs().client->execSqlSync(
                "SELECT to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS v FROM media_objects WHERE id = $1::uuid", loose)[0]["v"].as<std::string>();
            const auto loosePage = undoablePage(s.userA, 1);
            if (loosePage && !loosePage->items.empty()) CHECK_EQ(loosePage->items[0]["uploaded_at"].asString(), uploaded);

            // A memory: its current title and the position its current revision states.
            const auto one = readyPhoto(s.userA), two = readyPhoto(s.userA), three = readyPhoto(s.userA);
            const auto made = mm::create(s.userA, mm::terms("First title", {one, two}));
            const auto memoryId = mm::idOf(made);
            CHECK(mm::is(made, SO::Created));
            const auto twoCycle = cycleOf(erase(s.userA, two));
            const auto inCurrent = contextOf(s.userA, twoCycle);
            CHECK_EQ(inCurrent["kind"].asString(), std::string("memory"));
            CHECK_EQ(inCurrent["relation"].asString(), std::string("current"));
            CHECK_EQ(inCurrent["target_id"].asString(), memoryId);
            CHECK_EQ(inCurrent["label"].asString(), std::string("First title"));
            CHECK_EQ(inCurrent["position"].asInt(), 2);

            // Only an older revision holds it now: the current title, and no position. The photo moved is read from the new revision.
            CHECK(mm::is(mm::edit(s.userA, memoryId, 1, mm::terms("New title", {three, one})), SO::Created));
            const auto history = contextOf(s.userA, twoCycle);
            CHECK_EQ(history["kind"].asString(), std::string("memory"));
            CHECK_EQ(history["relation"].asString(), std::string("former"));
            CHECK_EQ(history["target_id"].asString(), memoryId);
            CHECK_EQ(history["label"].asString(), std::string("New title"));
            CHECK(history["position"].isNull());
            const auto oneCycle = cycleOf(erase(s.userA, one));
            const auto moved = contextOf(s.userA, oneCycle);
            CHECK_EQ(moved["relation"].asString(), std::string("current"));
            CHECK_EQ(moved["position"].asInt(), 2);

            // A photo in the partner's memory: its uploader can read that parent, and sees its title.
            const auto theirs = readyPhoto(s.userB);
            const auto theirMemory = mm::idOf(mm::create(s.userB, mm::terms("Partner's memory", {theirs})));
            const auto mine = readyPhoto(s.userA);
            const auto proposal = mm::propose(s.userA, theirMemory, 1, mm::terms("Partner's memory", {theirs, mine}));
            CHECK(mm::is(proposal, SO::Created));

            // A pending request is not context: reserved and unbound reads as unbound, and names nothing.
            const auto reservedCycle = cycleOf(erase(s.userA, mine));
            const auto reserved = contextOf(s.userA, reservedCycle);
            CHECK_EQ(reserved["kind"].asString(), std::string("none"));
            CHECK(reserved["target_id"].isNull() && reserved["label"].isNull());
            CHECK(md::is(undo(s.userA, mine, reservedCycle), MO::Ok));
            CHECK(mm::is(mm::decide(s.userB, theirMemory, mm::proposalOf(proposal), mm::Decision::Approve), SO::Ok));
            const auto approvedCycle = cycleOf(erase(s.userA, mine));
            const auto approved = contextOf(s.userA, approvedCycle);
            CHECK_EQ(approved["label"].asString(), std::string("Partner's memory"));
            CHECK_EQ(approved["relation"].asString(), std::string("current"));
            CHECK_EQ(approved["position"].asInt(), 2);

            // An album: its current name, never a position, active or removed.
            const auto albumPhoto = readyPhoto(s.userA), removedPhoto = readyPhoto(s.userA);
            const auto albumId = al::idOf(al::create(s.userA, "Trip"));
            CHECK(al::is(al::add(s.userA, albumId, {albumPhoto, removedPhoto}), AO::Created));
            const auto albumCycle = cycleOf(erase(s.userA, albumPhoto));
            const auto active = contextOf(s.userA, albumCycle);
            CHECK_EQ(active["kind"].asString(), std::string("album"));
            CHECK_EQ(active["relation"].asString(), std::string("current"));
            CHECK_EQ(active["target_id"].asString(), albumId);
            CHECK_EQ(active["label"].asString(), std::string("Trip"));
            CHECK(active["position"].isNull());
            CHECK(al::is(al::removePhoto(s.userA, albumId, removedPhoto), AO::Ok));
            const auto removedCycle = cycleOf(erase(s.userA, removedPhoto));
            const auto removed = contextOf(s.userA, removedCycle);
            CHECK_EQ(removed["relation"].asString(), std::string("former"));
            CHECK_EQ(removed["label"].asString(), std::string("Trip"));
            CHECK(removed["position"].isNull());

            // The partner lists none of this, and another space nothing at all, though the partner can read the parents.
            CHECK(undoableIds(s.userB).empty());
            const auto other = md::space();
            CHECK(undoableIds(other.userA).empty());
            const auto partnerOwn = readyPhoto(s.userB);
            CHECK(al::is(al::add(s.userB, albumId, {partnerOwn}), AO::Created));
            const auto partnerCycle = cycleOf(erase(s.userB, partnerOwn));
            CHECK(undoableIds(s.userB) == std::vector<std::string>{partnerCycle});
            CHECK_EQ(contextOf(s.userB, partnerCycle)["label"].asString(), std::string("Trip"));
            CHECK(rawOf(s.userB).find("New title") == std::string::npos);

            // Pages stay stable with one row per cycle, whatever the binding.
            const auto walked = undoableIds(s.userA, 1);
            CHECK(walked == undoableIds(s.userA, 100));
            // Loose, two, one, the approved photo, and both album photos. The reserved cycle was undone.
            CHECK_EQ(walked.size(), 6u);
            std::set<std::string> distinct(walked.begin(), walked.end());
            CHECK_EQ(distinct.size(), walked.size());

            const auto all = rawOf(s.userA);
            CHECK(all.find("media/") == std::string::npos);
            CHECK(all.find("staging") == std::string::npos);

            // A binding that names a parent this caller cannot read, or one that no longer exists. No code path makes these, and guards refuse
            // them, so only a maintenance connection can place them; both must read as one generic answer, with nothing of the parent.
            const char *maint = std::getenv("USAPP_TEST_MAINT_CONNINFO");
            if (!maint || !*maint) throw std::runtime_error("the unavailable-parent case needs USAPP_TEST_MAINT_CONNINFO");
            const auto maintenance = drogon::orm::DbClient::newPgClient(maint, 1);
            maintenance->setTimeout(5.0);
            const auto foreignMemory = mm::idOf(mm::create(other.userA, mm::terms("Secret title", {readyPhoto(other.userA)})));
            const auto foreignAlbum = al::idOf(al::create(other.userA, "Secret name"));
            const auto place = [&](const std::string &photo, const std::string &kind, const std::string &target) {
                for (int tries = 0;; ++tries) {
                    try {
                        maintenance->execSqlSync(
                            "DO $place$ BEGIN PERFORM set_config('session_replication_role', 'replica', true); "
                            "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                            "VALUES ('" + photo + "', '" + s.couple + "', '" + kind + "', '" + target + "', '" + s.userA + "'); END $place$");
                        return;
                    } catch (const std::exception &) {
                        // The client connects asynchronously.
                        if (tries >= 50) throw;
                        md::sleepMs(100);
                    }
                }
            };
            for (const auto &[kind, target] : {std::pair<std::string, std::string>{"memory", foreignMemory}, {"album", foreignAlbum}, {"memory", c::newUuid()}, {"album", c::newUuid()}}) {
                const auto photo = readyPhoto(s.userA);
                place(photo, kind, target);
                const auto cycle = cycleOf(erase(s.userA, photo));
                const auto hidden = contextOf(s.userA, cycle);
                CHECK_EQ(hidden["kind"].asString(), std::string("unavailable"));
                CHECK(hidden["relation"].isNull() && hidden["target_id"].isNull() && hidden["label"].isNull() && hidden["position"].isNull());
                CHECK(rawOf(s.userA).find(target) == std::string::npos);
            }
            const auto leaked = rawOf(s.userA);
            CHECK(leaked.find("Secret") == std::string::npos);
            std::printf("D6 unavailable parent: covered\n");
        } catch (const std::exception &) {
            c::expectThat(false, "d6-context", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6: fingerprints name the target") {
        CHECK(M::deletionFingerprint("a") != M::deletionFingerprint("b"));
        CHECK(M::undoFingerprint("a", "b") != M::undoFingerprint("a", "c"));
        CHECK(M::undoFingerprint("ab", "c") != M::undoFingerprint("a", "bc"));
        CHECK_EQ(M::deletionFingerprint("a").size(), 64u);
    }

    M::instance().configure(M::Policy{}, nullptr);
    holderConn.reset();
}

}  // namespace usapptest::mediadeletion
