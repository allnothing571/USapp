#pragma once

// Unattached-upload recovery: GET /api/v1/media/unattached through the real MediaService, AlbumService and MemoryService, on literal
// usapp_test with FakeObjectStore. Runs as its own CTest target (unattached_db), outside db_gated. The identical-timestamp fixture
// needs the maintenance connection the memory deletion suite already uses. The route and its HTTP answers are covered through the real
// API binary in ReminderLiveApiTest.h.
#include <json/json.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "AlbumRenameDbTests.h"
#include "MemoryDeletionDbTests.h"

namespace usapptest::mediaunattached {

namespace al = usapptest::album;
namespace base = usapptest::memorydeletion;
namespace del = usapptest::mediadeletion;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using M = usapp::MediaService;
using MO = M::Outcome;
using A = usapp::AlbumService;
using AO = A::Outcome;
using SO = usapp::MemoryService::Outcome;
using Decision = usapp::MemoryService::Decision;

inline const c::Observer &obs() { return md::obs(); }

inline std::optional<M::DeletionPage> page(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<std::pair<std::string, std::string>> after;
    if (cursor) {
        const auto decoded = A::decodeCursor(*cursor);
        if (!decoded) throw std::runtime_error("cursor");
        after = std::make_pair(decoded->createdAt, decoded->id);
    }
    auto p = std::make_shared<ts::Pending<M::DeletionPage>>();
    M::instance().listUnattached(user, limit, after, [p](M::DeletionPage r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::vector<std::string> listed(const std::string &user) {
    std::vector<std::string> ids;
    std::optional<std::string> cursor;
    for (int pages = 0; pages < 50; ++pages) {
        const auto r = page(user, 100, cursor);
        if (!r || r->outcome != MO::Ok) throw std::runtime_error("page");
        for (const auto &item : r->items) ids.push_back(item["media_id"].asString());
        if (!r->nextCursor) return ids;
        cursor = r->nextCursor;
    }
    throw std::runtime_error("walk");
}
inline bool has(const std::vector<std::string> &ids, const std::string &id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
inline std::vector<std::string> walk(const std::string &user, int limit, const std::set<std::string> &only) {
    std::vector<std::string> ids;
    std::optional<std::string> cursor;
    for (int pages = 0; pages < 200; ++pages) {
        const auto r = page(user, limit, cursor);
        if (!r || r->outcome != MO::Ok || r->items.size() > static_cast<size_t>(limit)) throw std::runtime_error("page");
        for (const auto &item : r->items) {
            const auto id = item["media_id"].asString();
            if (has(ids, id)) throw std::runtime_error("repeat");
            if (only.count(id)) ids.push_back(id);
        }
        if (!r->nextCursor) return ids;
        cursor = r->nextCursor;
    }
    throw std::runtime_error("walk");
}
inline std::string written(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}
// Everything a list could plausibly move for one couple: grants and deadlines, outcomes, plans, bindings, reservations, deletions.
inline std::string footprint(const std::string &couple) {
    return obs().client->execSqlSync(
        "SELECT concat_ws('|', "
        "(SELECT string_agg(o.id || ':' || o.expires_at::text || ':' || o.created_at::text || ':' || COALESCE(usapp_media_pending_reference(o.id)::text, '-'), ',' ORDER BY o.id) "
        "  FROM media_objects o WHERE o.couple_id = $1::uuid), "
        "(SELECT string_agg(t.media_id || ':' || t.outcome || ':' || t.decided_at::text, ',' ORDER BY t.media_id) FROM media_upload_outcomes t WHERE t.couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id WHERE o.couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_bindings WHERE couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_deletions WHERE couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_promotion_attempts WHERE couple_id = $1::uuid), (SELECT count(*) FROM audit_logs)) AS v",
        couple)[0]["v"].as<std::string>();
}
inline bool plan(const std::string &media) {
    try {
        md::env.pool->execSqlSync(mm::kPlanInsertSql, media);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

inline void runMediaUnattachedDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);

    TEST_CASE("Unattached uploads: only the uploader's own READY photos with no home, reservation, plan or deletion; listing writes nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto ready = del::readyPhoto(s.userA);
            const auto partners = del::readyPhoto(s.userB);
            const auto strangers = del::readyPhoto(other.userA);
            const auto granted = md::idOf(md::grant(s.userA));
            const auto rejected = md::uploaded(s.userA, "text;size=1000");
            CHECK_EQ(md::stateOf(md::complete(s.userA, rejected.id)), std::string("rejected"));
            // Bound to a memory; added to an album and then removed (still bound: one home, forever).
            const auto inMemory = del::readyPhoto(s.userA);
            CHECK(mm::is(mm::create(s.userA, mm::terms("UA-MEMORY", {inMemory})), SO::Created));
            const auto removed = del::readyPhoto(s.userA);
            const auto album = al::idOf(al::create(s.userA, "UA-ALBUM"));
            CHECK(al::is(al::add(s.userA, album, {removed}), AO::Created));
            CHECK(al::is(al::removePhoto(s.userA, album, removed), AO::Ok));
            // Reserved by a pending proposal of the partner's memory.
            const auto theirMemory = mm::idOf(mm::create(s.userB, mm::terms("UA-THEIRS", {partners})));
            const auto partnersOther = del::readyPhoto(s.userB);
            CHECK(mm::is(mm::create(s.userB, mm::terms("UA-OTHER", {partnersOther})), SO::Created));
            const auto reserved = del::readyPhoto(s.userA);
            const auto proposal = mm::proposalOf(mm::propose(s.userA, theirMemory, 1, mm::terms("UA-ASK", {partners, reserved})));
            CHECK(!proposal.empty());
            // Deleted by its uploader.
            const auto deleted = del::readyPhoto(s.userA);
            const auto cycle = del::cycleOf(del::erase(s.userA, deleted));
            CHECK(!cycle.empty());
            // Old enough for an unattached plan, then planned.
            const auto planned = md::agedReady(pool, s, s.userA);
            CHECK(plan(planned));
            const auto agedFree = md::agedReady(pool, s, s.userA);

            const auto before = footprint(s.couple);
            const auto mine = listed(s.userA);
            CHECK(has(mine, ready));
            CHECK(has(mine, agedFree));
            for (const auto &excluded : {partners, strangers, granted, rejected.id, inMemory, removed, reserved, deleted, planned}) CHECK(!has(mine, excluded));
            CHECK_EQ(mine.size(), 2u);
            const auto first = page(s.userA, 100);
            if (first && first->outcome == MO::Ok) {
                for (const auto &item : first->items) {
                    const auto names = item.getMemberNames();
                    CHECK(std::set<std::string>(names.begin(), names.end()) == std::set<std::string>({"media_id", "uploaded_at", "width", "height"}));
                    const auto text = written(item);
                    CHECK(text.find("staging/") == std::string::npos && text.find("media/") == std::string::npos);
                }
            }
            // The partner's own list has only the partner's own free photos; another space sees nothing of this one.
            const auto theirs = listed(s.userB);
            CHECK(!has(theirs, ready) && !has(theirs, agedFree) && !has(theirs, partners) && !has(theirs, partnersOther));
            CHECK(!has(listed(other.userA), ready));
            CHECK(has(listed(other.userA), strangers));
            const auto lonely = c::newUuid();
            ts::commitUser(pool, lonely, "ua_lonely");
            const auto none = page(lonely, 10);
            CHECK(none && none->outcome == MO::SpaceNotFound);
            // Reads moved no deadline, grant, plan, binding, reservation or deletion.
            CHECK_EQ(footprint(s.couple), before);

            // A rejected proposal releases its photo, which may be chosen again; an undone deletion returns its photo too.
            CHECK(mm::is(mm::decide(s.userB, theirMemory, proposal, Decision::Reject), SO::Ok));
            CHECK(has(listed(s.userA), reserved));
            CHECK(md::is(del::undo(s.userA, deleted, cycle), MO::Ok));
            CHECK(has(listed(s.userA), deleted));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ua-classes", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Unattached uploads: a listed photo is a snapshot; attachment and cleanup each decide, in either order") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto album = al::idOf(al::create(s.userA, "UA-ALBUM"));
            // Cleanup first: the listed photo is planned, so the attachment that follows is refused and the list drops it.
            const auto cleaned = md::agedReady(pool, s, s.userA);
            CHECK(has(listed(s.userA), cleaned));
            CHECK(plan(cleaned));
            CHECK(al::is(al::add(s.userA, album, {cleaned}), AO::PhotoUnavailable));
            CHECK(mm::is(mm::create(s.userA, mm::terms("UA-LATE", {cleaned})), SO::PhotoUnavailable));
            CHECK(!has(listed(s.userA), cleaned));
            // Attachment first: the listed photo is added, so the plan that follows is refused and the list drops it.
            const auto attached = md::agedReady(pool, s, s.userA);
            CHECK(has(listed(s.userA), attached));
            CHECK(al::is(al::add(s.userA, album, {attached}), AO::Created));
            CHECK(!plan(attached));
            CHECK(!has(listed(s.userA), attached));
            // The same through a memory: attached once, it is never offered again, and a second home is refused.
            const auto memoryPhoto = md::agedReady(pool, s, s.userA);
            CHECK(has(listed(s.userA), memoryPhoto));
            CHECK(mm::is(mm::create(s.userA, mm::terms("UA-HOME", {memoryPhoto})), SO::Created));
            CHECK(!plan(memoryPhoto));
            CHECK(!has(listed(s.userA), memoryPhoto));
            CHECK(al::is(al::add(s.userA, album, {memoryPhoto}), AO::PhotoUnavailable));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ua-races", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Unattached uploads: pages are stable when upload times are identical, and a fresh read finds them again") {
        try {
            md::useStore();
            const auto s = md::space();
            std::vector<std::string> made;
            for (int i = 0; i < 5; ++i) made.push_back(del::readyPhoto(s.userA));
            std::string ids;
            for (const auto &id : made) ids += (ids.empty() ? "'" : ",'") + id + "'";
            // One instant for all five: only the id orders them. A fixture written with triggers off; nothing else is changed.
            base::asMaintenance("UPDATE media_objects SET created_at = (SELECT min(x.created_at) FROM media_objects x WHERE x.id IN (" + ids +
                                ")) WHERE id IN (" + ids + ");");
            CHECK_EQ(c::scalar(obs(), "SELECT count(DISTINCT created_at)::bigint AS n FROM media_objects WHERE couple_id = $1::uuid AND uploader_id = $2::uuid",
                               s.couple, s.userA), 1);
            auto expected = made;
            std::sort(expected.begin(), expected.end(), std::greater<std::string>());
            const std::set<std::string> only(made.begin(), made.end());
            for (int limit : {1, 2, 3, 5, 100}) CHECK(walk(s.userA, limit, only) == expected);
            // A newer upload never enters a walk in progress.
            const auto start = page(s.userA, 2);
            CHECK(start && start->outcome == MO::Ok && start->nextCursor);
            if (start && start->nextCursor) {
                const auto newer = del::readyPhoto(s.userA);
                std::vector<std::string> seen;
                for (const auto &item : start->items) seen.push_back(item["media_id"].asString());
                std::optional<std::string> cursor = start->nextCursor;
                while (cursor) {
                    const auto next = page(s.userA, 2, cursor);
                    if (!next || next->outcome != MO::Ok) throw std::runtime_error("page");
                    for (const auto &item : next->items) seen.push_back(item["media_id"].asString());
                    cursor = next->nextCursor;
                }
                CHECK(seen == expected);
                CHECK(!has(seen, newer));
                // A later, independent read (a new session reads the same server state) finds all six.
                const auto fresh = listed(s.userA);
                CHECK_EQ(fresh.size(), 6u);
                CHECK(!fresh.empty() && fresh.front() == newer);
            }
        } catch (const std::exception &failure) {
            c::expectThat(false, "ua-pages", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::mediaunattached
