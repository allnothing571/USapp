#pragma once

// Album rename-request history: GET /api/v1/albums/{albumId}/proposals through the real AlbumService, on literal usapp_test with
// FakeObjectStore for the photos. Runs as its own CTest target (album_history_db). The identical-timestamp fixture needs the
// maintenance connection that the memory deletion suite already uses. The route and its HTTP answers are covered through the real
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

namespace usapptest::albumproposalhistory {

namespace al = usapptest::album;
namespace ar = usapptest::albumrename;
namespace base = usapptest::memorydeletion;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using A = usapp::AlbumService;
using AO = A::Outcome;
using Decision = A::Decision;

inline const c::Observer &obs() { return md::obs(); }

inline std::optional<A::ListResult> page(const std::string &user, const std::string &album, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<A::Cursor> after;
    if (cursor) {
        after = A::decodeCursor(*cursor);
        if (!after) throw std::runtime_error("cursor");
    }
    auto p = std::make_shared<ts::Pending<A::ListResult>>();
    A::instance().listProposals(user, album, limit, after, [p](A::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline AO outcomeOf(const std::optional<A::ListResult> &r) { return r ? r->outcome : AO::Error; }
inline std::vector<std::string> walk(const std::string &user, const std::string &album, int limit) {
    std::vector<std::string> ids;
    std::optional<std::string> cursor;
    for (int pages = 0; pages < 50; ++pages) {
        const auto r = page(user, album, limit, cursor);
        if (!r || r->outcome != AO::Ok || r->items.size() > static_cast<size_t>(limit)) throw std::runtime_error("page");
        for (const auto &item : r->items) {
            const auto id = item["id"].asString();
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) throw std::runtime_error("repeat");
            ids.push_back(id);
        }
        if (!r->nextCursor) return ids;
        cursor = r->nextCursor;
    }
    throw std::runtime_error("walk");
}
inline const Json::Value *find(const A::ListResult &r, const std::string &id) {
    for (const auto &item : r.items) if (item["id"].asString() == id) return &item;
    return nullptr;
}
inline bool summaryShape(const Json::Value &item) {
    const std::set<std::string> expected = {"id", "album_id", "proposer_id", "base_version", "created_at", "status", "decided_by", "decided_at", "approved_version"};
    const auto names = item.getMemberNames();
    return std::set<std::string>(names.begin(), names.end()) == expected;
}
inline std::string written(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}
// Every row a list could plausibly touch, for one album and its space.
inline std::string footprint(const std::string &album, const std::string &couple) {
    return obs().client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM album_edit_proposals WHERE album_id = $1::uuid), "
        "(SELECT count(*) FROM album_edit_decisions d JOIN album_edit_proposals p ON p.id = d.proposal_id WHERE p.album_id = $1::uuid), "
        "(SELECT count(*) FROM album_revisions WHERE album_id = $1::uuid), (SELECT count(*) FROM album_texts WHERE couple_id = $2::uuid), "
        "(SELECT count(*) FROM album_items WHERE album_id = $1::uuid), (SELECT count(*) FROM album_item_removals WHERE album_id = $1::uuid), "
        "(SELECT count(*) FROM album_cover_events WHERE album_id = $1::uuid), (SELECT count(*) FROM album_photo_batches WHERE album_id = $1::uuid), "
        "(SELECT count(*) FROM media_bindings WHERE couple_id = $2::uuid), "
        "(SELECT count(*) FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id WHERE o.couple_id = $2::uuid), "
        "(SELECT count(*) FROM audit_logs))  AS v",
        album, couple)[0]["v"].as<std::string>();
}

inline void runAlbumProposalHistoryDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    const auto aged = [&](const ts::CommittedSpace &s, const std::string &uploader) { return md::agedReady(pool, s, uploader); };

    TEST_CASE("Album rename history: both members list it, nobody else does, and listing writes nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto id = al::idOf(al::create(s.userA, "AH-NAME"));
            const auto empty = page(s.userB, id, 10);
            CHECK(outcomeOf(empty) == AO::Ok);
            if (empty && empty->outcome == AO::Ok) {
                CHECK_EQ(empty->items.size(), 0u);
                CHECK(!empty->nextCursor);
                CHECK_EQ(empty->currentVersion, 1LL);
            }
            const auto pending = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-ASKED"));
            CHECK(!pending.empty());
            const auto before = footprint(id, s.couple);
            for (const auto &user : {s.userA, s.userB}) {
                const auto r = page(user, id, 10);
                CHECK(outcomeOf(r) == AO::Ok);
                if (!r || r->outcome != AO::Ok) continue;
                CHECK_EQ(r->items.size(), 1u);
                const auto &item = r->items.front();
                CHECK(summaryShape(item));
                CHECK_EQ(item["id"].asString(), pending);
                CHECK_EQ(item["album_id"].asString(), id);
                CHECK_EQ(item["proposer_id"].asString(), s.userB);
                CHECK_EQ(item["base_version"].asInt(), 1);
                CHECK_EQ(item["status"].asString(), std::string("pending"));
                CHECK(item["decided_by"].isNull() && item["decided_at"].isNull() && item["approved_version"].isNull());
                CHECK(written(item).find("AH-") == std::string::npos);
            }
            // Another space's members, a user in no space, and ids that name nothing: what listAlbumPhotos answers.
            CHECK(outcomeOf(page(other.userA, id, 10)) == AO::NotFound);
            CHECK(outcomeOf(page(other.userB, id, 10)) == AO::NotFound);
            CHECK(ar::is(ar::getProposal(other.userA, id, pending), AO::ProposalNotFound));
            CHECK(outcomeOf(page(s.userA, c::newUuid(), 10)) == AO::NotFound);
            CHECK(outcomeOf(page(s.userA, pending, 10)) == AO::NotFound);
            const auto lonely = c::newUuid();
            ts::commitUser(pool, lonely, "ah_lonely");
            CHECK(outcomeOf(page(lonely, id, 10)) == AO::NotFound);
            CHECK_EQ(footprint(id, s.couple), before);
            CHECK(ar::is(ar::decide(s.userB, id, pending, Decision::Withdraw), AO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ah-access", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Album rename history: every status; photos and covers leave name versions alone; a later rename supersedes an approval") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "AH-ORIGINAL"));
            const auto rejected = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-ONE"));
            const auto rejectKey = al::newKey();
            CHECK(ar::is(ar::decide(s.userA, id, rejected, Decision::Reject, rejectKey), AO::Ok));
            const auto withdrawn = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-TWO"));
            CHECK(ar::is(ar::decide(s.userB, id, withdrawn, Decision::Withdraw), AO::Ok));
            const auto approved = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-THREE"));
            const auto approveKey = al::newKey();
            CHECK(ar::is(ar::decide(s.userA, id, approved, Decision::Approve, approveKey), AO::Ok));
            const auto afterApproval = page(s.userA, id, 10);
            CHECK(afterApproval && afterApproval->outcome == AO::Ok && afterApproval->currentVersion == 2);

            // Photos, a cover and a removal: none of them is a name version, and the history reads the same.
            const auto first = aged(s, s.userA);
            const auto second = aged(s, s.userB);
            CHECK(al::is(al::add(s.userA, id, {first}), AO::Created));
            CHECK(al::is(al::add(s.userB, id, {second}), AO::Created));
            CHECK(al::is(al::cover(s.userB, id, first), AO::Created));
            CHECK(al::is(al::removePhoto(s.userB, id, second), AO::Ok));
            CHECK_EQ(ar::versionOf(al::getAlbum(s.userB, id)), 2);
            const auto afterPhotos = page(s.userB, id, 10);
            CHECK(afterPhotos && afterApproval && afterPhotos->currentVersion == 2 && afterPhotos->items.size() == afterApproval->items.size());
            if (afterPhotos && afterApproval && afterPhotos->items.size() == afterApproval->items.size()) {
                for (size_t i = 0; i < afterPhotos->items.size(); ++i) CHECK(afterPhotos->items[i] == afterApproval->items[i]);
            }

            // The creator renames again: version 2 stays a fact, and is no longer the current name.
            CHECK(ar::is(ar::rename(s.userA, id, 2, "AH-LATER"), AO::Created));
            const auto pending = ar::proposalOf(ar::propose(s.userB, id, 3, "AH-FOUR"));
            const auto r = page(s.userB, id, 10);
            CHECK(outcomeOf(r) == AO::Ok);
            if (r && r->outcome == AO::Ok) {
                CHECK_EQ(r->currentVersion, 3LL);
                std::vector<std::string> order;
                for (const auto &item : r->items) order.push_back(item["id"].asString());
                CHECK(order == std::vector<std::string>({pending, approved, withdrawn, rejected}));
                const auto *a = find(*r, approved), *rj = find(*r, rejected), *w = find(*r, withdrawn), *p = find(*r, pending);
                CHECK(a && rj && w && p);
                if (a && rj && w && p) {
                    CHECK_EQ((*a)["status"].asString(), std::string("approved"));
                    CHECK_EQ((*a)["approved_version"].asInt(), 2);
                    CHECK_EQ((*a)["decided_by"].asString(), s.userA);
                    CHECK((*a)["approved_version"].asInt() < r->currentVersion);
                    CHECK_EQ((*rj)["status"].asString(), std::string("rejected"));
                    CHECK_EQ((*rj)["decided_by"].asString(), s.userA);
                    CHECK((*rj)["approved_version"].isNull());
                    CHECK_EQ((*w)["status"].asString(), std::string("withdrawn"));
                    CHECK_EQ((*w)["decided_by"].asString(), s.userB);
                    CHECK_EQ((*p)["status"].asString(), std::string("pending"));
                    CHECK_EQ((*p)["base_version"].asInt(), 3);
                    for (const auto &item : r->items) CHECK(summaryShape(item));
                }
            }
            // A fresh read of each decided request's detail: the names, and the same status as the list.
            for (const auto &pid : {rejected, withdrawn, approved}) {
                const auto detail = ar::getProposal(s.userB, id, pid);
                const auto *listed = r ? find(*r, pid) : nullptr;
                CHECK(ar::is(detail, AO::Ok) && listed && detail->proposal["status"] == (*listed)["status"]);
            }
            const auto approvedDetail = ar::getProposal(s.userA, id, approved);
            CHECK(approvedDetail && approvedDetail->proposal["original_name"].asString() == "AH-ORIGINAL" &&
                  approvedDetail->proposal["proposed_name"].asString() == "AH-THREE");
            // Replays and a conflicting decision are what they were; neither they nor the list writes anything.
            const auto before = footprint(id, s.couple);
            CHECK(ar::is(ar::decide(s.userA, id, rejected, Decision::Reject, rejectKey), AO::Replayed));
            CHECK(ar::is(ar::decide(s.userA, id, approved, Decision::Approve, approveKey), AO::Replayed));
            CHECK(ar::is(ar::decide(s.userA, id, approved, Decision::Reject), AO::ProposalDecided));
            CHECK_EQ(footprint(id, s.couple), before);
            const auto again = page(s.userA, id, 10);
            CHECK(again && r && again->items.size() == r->items.size() && again->currentVersion == r->currentVersion);
            if (again && r && again->items.size() == r->items.size()) {
                for (size_t i = 0; i < r->items.size(); ++i) CHECK(again->items[i] == r->items[i]);
            }
            CHECK(ar::is(ar::decide(s.userB, id, pending, Decision::Withdraw), AO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ah-statuses", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Album rename history: pages are stable when creation times are identical, and later requests never enter a walk") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "AH-NAME"));
            std::vector<std::string> made;
            for (int i = 0; i < 5; ++i) {
                const auto pid = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-" + std::to_string(i)));
                made.push_back(pid);
                if (i < 4) CHECK(ar::is(ar::decide(s.userA, id, pid, Decision::Reject), AO::Ok));
            }
            // One instant for all five: only the id orders them. A fixture written with triggers off; nothing else is changed.
            base::asMaintenance("UPDATE album_edit_proposals SET created_at = (SELECT min(x.created_at) FROM album_edit_proposals x WHERE x.album_id = '" + id +
                                "') WHERE album_id = '" + id + "';");
            CHECK_EQ(c::scalar(obs(), "SELECT count(DISTINCT created_at)::bigint AS n FROM album_edit_proposals WHERE album_id = $1::uuid", id), 1);
            auto expected = made;
            std::sort(expected.begin(), expected.end(), std::greater<std::string>());
            for (int limit : {1, 2, 3, 5, 100}) CHECK(walk(s.userA, id, limit) == expected);
            const auto first = page(s.userB, id, 2);
            CHECK(first && first->outcome == AO::Ok && first->nextCursor);
            if (first && first->nextCursor) {
                CHECK(ar::is(ar::decide(s.userB, id, made.back(), Decision::Withdraw), AO::Ok));
                const auto newer = ar::proposalOf(ar::propose(s.userB, id, 1, "AH-NEW"));
                std::vector<std::string> seen;
                for (const auto &item : first->items) seen.push_back(item["id"].asString());
                std::optional<std::string> cursor = first->nextCursor;
                while (cursor) {
                    const auto next = page(s.userB, id, 2, cursor);
                    if (!next || next->outcome != AO::Ok) throw std::runtime_error("page");
                    for (const auto &item : next->items) seen.push_back(item["id"].asString());
                    cursor = next->nextCursor;
                }
                CHECK(seen == expected);
                const auto fresh = walk(s.userB, id, 2);
                CHECK_EQ(fresh.size(), 6u);
                CHECK(!fresh.empty() && fresh.front() == newer);
                CHECK(ar::is(ar::decide(s.userB, id, newer, Decision::Withdraw), AO::Ok));
            }
            CHECK(outcomeOf(page(s.userA, id, 2, A::encodeCursor({"2000-01-01T00:00:00.000000Z", c::newUuid()}))) == AO::Ok);
        } catch (const std::exception &failure) {
            c::expectThat(false, "ah-pages", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::albumproposalhistory
