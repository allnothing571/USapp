#pragma once

// Memory edit-request history: GET /api/v1/memories/{memoryId}/proposals through the real MemoryService, on literal usapp_test with
// FakeObjectStore for the photos. Runs inside the memory deletion target, after the erase suite, so the environment and the maintenance
// connection are ready. The route, its filter and its HTTP answers are covered through the real API binary in ReminderLiveApiTest.h.
#include <json/json.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "MemoryEraseDbTests.h"

namespace usapptest::memoryproposalhistory {

namespace base = usapptest::memorydeletion;
namespace erase = usapptest::memoryerase;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace del = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using Decision = S::Decision;

inline const c::Observer &obs() { return md::obs(); }

inline std::optional<S::ListResult> page(const std::string &user, const std::string &memory, int limit,
                                         const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<std::pair<std::string, std::string>> after;
    if (cursor) {
        const auto decoded = usapp::AlbumService::decodeCursor(*cursor);
        if (!decoded) throw std::runtime_error("cursor");
        after = std::make_pair(decoded->createdAt, decoded->id);
    }
    auto p = std::make_shared<ts::Pending<S::ListResult>>();
    S::instance().listProposals(user, memory, limit, after, [p](S::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline SO outcomeOf(const std::optional<S::ListResult> &r) { return r ? r->outcome : SO::Error; }
// Every page of a walk at `limit`, in order; throws on a failed page, a repeat, or a walk that does not end.
inline std::vector<std::string> walk(const std::string &user, const std::string &memory, int limit) {
    std::vector<std::string> ids;
    std::optional<std::string> cursor;
    for (int pages = 0; pages < 50; ++pages) {
        const auto r = page(user, memory, limit, cursor);
        if (!r || r->outcome != SO::Ok || r->memories.size() > static_cast<size_t>(limit)) throw std::runtime_error("page");
        for (const auto &item : r->memories) {
            const auto id = item["id"].asString();
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) throw std::runtime_error("repeat");
            ids.push_back(id);
        }
        if (!r->nextCursor) return ids;
        cursor = r->nextCursor;
    }
    throw std::runtime_error("walk");
}
inline const Json::Value *find(const S::ListResult &r, const std::string &id) {
    for (const auto &item : r.memories) if (item["id"].asString() == id) return &item;
    return nullptr;
}
// A summary has exactly these members: no text, photo, key or fingerprint.
inline bool summaryShape(const Json::Value &item) {
    const std::set<std::string> expected = {"id", "memory_id", "proposer_id", "base_version", "created_at", "status", "decided_by", "decided_at", "approved_version"};
    const auto names = item.getMemberNames();
    return std::set<std::string>(names.begin(), names.end()) == expected;
}
// Every row a list could plausibly touch, for one memory and its space.
inline std::string footprint(const std::string &memory, const std::string &couple) {
    return obs().client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM memory_edit_proposals WHERE memory_id = $1::uuid), "
        "(SELECT count(*) FROM memory_edit_decisions d JOIN memory_edit_proposals p ON p.id = d.proposal_id WHERE p.memory_id = $1::uuid), "
        "(SELECT count(*) FROM memory_revisions WHERE memory_id = $1::uuid), (SELECT count(*) FROM media_bindings WHERE couple_id = $2::uuid), "
        "(SELECT count(*) FROM memory_proposal_photos WHERE couple_id = $2::uuid), (SELECT count(*) FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id WHERE o.couple_id = $2::uuid), "
        "(SELECT count(*) FROM audit_logs), (SELECT count(*) FROM memory_deletions WHERE memory_id = $1::uuid), "
        "(SELECT string_agg(COALESCE(usapp_media_pending_reference(o.id)::text, '-'), ',' ORDER BY o.id) FROM media_objects o WHERE o.couple_id = $2::uuid)) AS v",
        memory, couple)[0]["v"].as<std::string>();
}

inline void runMemoryProposalHistoryDbTests() {
    const std::string kAsked = "PH-SENTINEL-ASKED";

    TEST_CASE("Memory edit history: both members list it, nobody else does, and listing writes nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("PH-TITLE", {photo})));
            const auto empty = page(s.userA, id, 10);
            CHECK(outcomeOf(empty) == SO::Ok);
            if (empty && empty->outcome == SO::Ok) {
                CHECK_EQ(empty->memories.size(), 0u);
                CHECK(!empty->nextCursor);
                CHECK_EQ(empty->currentVersion, 1LL);
            }
            const auto offered = del::readyPhoto(s.userA);
            const auto pending = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked, {photo, offered})));
            CHECK(!pending.empty());
            const auto before = footprint(id, s.couple);
            for (const auto &user : {s.userA, s.userB}) {
                const auto r = page(user, id, 10);
                CHECK(outcomeOf(r) == SO::Ok);
                if (!r || r->outcome != SO::Ok) continue;
                CHECK_EQ(r->memories.size(), 1u);
                const auto &item = r->memories.front();
                CHECK(summaryShape(item));
                CHECK_EQ(item["id"].asString(), pending);
                CHECK_EQ(item["memory_id"].asString(), id);
                CHECK_EQ(item["proposer_id"].asString(), s.userA);
                CHECK_EQ(item["base_version"].asInt(), 1);
                CHECK_EQ(item["status"].asString(), std::string("pending"));
                CHECK(item["decided_by"].isNull() && item["decided_at"].isNull() && item["approved_version"].isNull());
                CHECK(base::written(item).find("PH-") == std::string::npos);
                CHECK(base::written(item).find(offered) == std::string::npos);
            }
            // Another space's members, a user in no space, and ids that name nothing: the same answers as the memory reads.
            CHECK(outcomeOf(page(other.userA, id, 10)) == SO::NotFound);
            CHECK(outcomeOf(page(other.userB, id, 10)) == SO::NotFound);
            CHECK(mm::is(mm::getProposal(other.userA, id, pending), SO::ProposalNotFound));
            CHECK(outcomeOf(page(s.userA, c::newUuid(), 10)) == SO::NotFound);
            CHECK(outcomeOf(page(s.userA, pending, 10)) == SO::NotFound);
            const auto lonely = c::newUuid();
            ts::commitUser(md::env.pool, lonely, "ph_lonely");
            CHECK(outcomeOf(page(lonely, id, 10)) == SO::SpaceNotFound);
            // Nothing written, nothing reserved or released, by any of those reads.
            CHECK_EQ(footprint(id, s.couple), before);
            CHECK(base::reservedNow(offered));
            CHECK(mm::is(mm::decide(s.userA, id, pending, Decision::Withdraw), SO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ph-access", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Memory edit history: every status, newest first; an old approval stays a fact after a newer revision; replays unchanged") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("PH-TITLE", {photo})));
            const auto rejected = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-1", {photo})));
            const auto rejectKey = c::newUuid();
            CHECK(mm::is(mm::decide(s.userB, id, rejected, Decision::Reject, rejectKey), SO::Ok));
            const auto withdrawn = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-2", {photo})));
            CHECK(mm::is(mm::decide(s.userA, id, withdrawn, Decision::Withdraw), SO::Ok));
            const auto approved = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-3", {photo})));
            const auto approveKey = c::newUuid();
            CHECK(mm::is(mm::decide(s.userB, id, approved, Decision::Approve, approveKey), SO::Ok));
            // The creator then replaces that approved version.
            CHECK(mm::is(mm::edit(s.userB, id, 2, mm::terms("PH-LATER", {photo})), SO::Created));
            const auto pending = mm::proposalOf(mm::propose(s.userA, id, 3, mm::terms(kAsked + "-4", {photo})));

            const auto r = page(s.userA, id, 10);
            CHECK(outcomeOf(r) == SO::Ok);
            if (r && r->outcome == SO::Ok) {
                CHECK_EQ(r->currentVersion, 3LL);
                std::vector<std::string> order;
                for (const auto &item : r->memories) order.push_back(item["id"].asString());
                CHECK(order == std::vector<std::string>({pending, approved, withdrawn, rejected}));
                const auto *a = find(*r, approved), *rj = find(*r, rejected), *w = find(*r, withdrawn), *p = find(*r, pending);
                CHECK(a && rj && w && p);
                if (a && rj && w && p) {
                    CHECK_EQ((*a)["status"].asString(), std::string("approved"));
                    CHECK_EQ((*a)["approved_version"].asInt(), 2);
                    CHECK_EQ((*a)["decided_by"].asString(), s.userB);
                    CHECK(!(*a)["decided_at"].isNull());
                    CHECK_EQ((*rj)["status"].asString(), std::string("rejected"));
                    CHECK_EQ((*rj)["decided_by"].asString(), s.userB);
                    CHECK((*rj)["approved_version"].isNull());
                    CHECK_EQ((*w)["status"].asString(), std::string("withdrawn"));
                    CHECK_EQ((*w)["decided_by"].asString(), s.userA);
                    CHECK_EQ((*p)["status"].asString(), std::string("pending"));
                    CHECK_EQ((*p)["base_version"].asInt(), 3);
                    // Historical: version 2 was approved, and is no longer the current one.
                    CHECK((*a)["approved_version"].asInt() < r->currentVersion);
                    for (const auto &item : r->memories) CHECK(summaryShape(item));
                }
            }
            // A fresh read by the proposer later on finds the decided ones, each with its detail.
            for (const auto &pid : {rejected, withdrawn, approved}) {
                const auto detail = mm::getProposal(s.userA, id, pid);
                CHECK(mm::is(detail, SO::Ok));
                const auto *listed = r ? find(*r, pid) : nullptr;
                CHECK(detail && listed && detail->proposal["status"] == (*listed)["status"]);
            }
            // Decision replay and refusal are what they were, and the list reads the same afterwards.
            const auto before = footprint(id, s.couple);
            CHECK(mm::is(mm::decide(s.userB, id, rejected, Decision::Reject, rejectKey), SO::Replayed));
            CHECK(mm::is(mm::decide(s.userB, id, approved, Decision::Approve, approveKey), SO::Replayed));
            CHECK(mm::is(mm::decide(s.userB, id, approved, Decision::Reject), SO::ProposalDecided));
            CHECK_EQ(footprint(id, s.couple), before);
            const auto again = page(s.userB, id, 10);
            CHECK(again && r && again->outcome == SO::Ok && again->memories.size() == r->memories.size());
            if (again && r && again->memories.size() == r->memories.size()) {
                for (size_t i = 0; i < r->memories.size(); ++i) CHECK(again->memories[i] == r->memories[i]);
            }
            CHECK(mm::is(mm::decide(s.userA, id, pending, Decision::Withdraw), SO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "ph-statuses", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Memory edit history: pages are stable when creation times are identical, and later proposals never enter a walk") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("PH-TITLE", {photo})));
            std::vector<std::string> made;
            for (int i = 0; i < 5; ++i) {
                const auto pid = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-" + std::to_string(i), {photo})));
                made.push_back(pid);
                if (i < 4) CHECK(mm::is(mm::decide(s.userB, id, pid, Decision::Reject), SO::Ok));
            }
            // One instant for all five: only the id orders them. A fixture written with triggers off; nothing else is changed.
            base::asMaintenance("UPDATE memory_edit_proposals SET created_at = (SELECT min(x.created_at) FROM memory_edit_proposals x WHERE x.memory_id = '" + id +
                                "') WHERE memory_id = '" + id + "';");
            CHECK_EQ(c::scalar(obs(), "SELECT count(DISTINCT created_at)::bigint AS n FROM memory_edit_proposals WHERE memory_id = $1::uuid", id), 1);
            auto expected = made;
            std::sort(expected.begin(), expected.end(), std::greater<std::string>());
            for (int limit : {1, 2, 3, 5, 100}) CHECK(walk(s.userB, id, limit) == expected);
            // A walk in progress: the open proposal is decided and a newer one is made between its pages.
            const auto first = page(s.userA, id, 2);
            CHECK(first && first->outcome == SO::Ok && first->nextCursor);
            if (first && first->nextCursor) {
                CHECK(mm::is(mm::decide(s.userA, id, made.back(), Decision::Withdraw), SO::Ok));
                const auto newer = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-new", {photo})));
                std::vector<std::string> seen;
                for (const auto &item : first->memories) seen.push_back(item["id"].asString());
                std::optional<std::string> cursor = first->nextCursor;
                while (cursor) {
                    const auto next = page(s.userA, id, 2, cursor);
                    if (!next || next->outcome != SO::Ok) throw std::runtime_error("page");
                    for (const auto &item : next->memories) seen.push_back(item["id"].asString());
                    cursor = next->nextCursor;
                }
                CHECK(seen == expected);
                const auto fresh = walk(s.userA, id, 2);
                CHECK_EQ(fresh.size(), 6u);
                CHECK(!fresh.empty() && fresh.front() == newer);
                CHECK(mm::is(mm::decide(s.userA, id, newer, Decision::Withdraw), SO::Ok));
            }
            // A position that names no row still pages strictly after it.
            CHECK(outcomeOf(page(s.userA, id, 2, usapp::AlbumService::encodeCursor({"2000-01-01T00:00:00.000000Z", c::newUuid()}))) == SO::Ok);
        } catch (const std::exception &failure) {
            c::expectThat(false, "ph-pages", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Memory edit history: a deleted memory hides its list and details; undo shows them again; a purged one stays hidden") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("PH-TITLE", {photo})));
            const auto decided = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked, {photo})));
            CHECK(mm::is(mm::decide(s.userB, id, decided, Decision::Reject), SO::Ok));
            const auto open = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kAsked + "-open", {photo})));
            const auto cycle = base::cycleOf(base::erase(s.userB, id));
            CHECK(!cycle.empty());
            for (const auto &user : {s.userA, s.userB}) {
                CHECK(outcomeOf(page(user, id, 10)) == SO::NotFound);
                CHECK(mm::is(mm::getProposal(user, id, decided), SO::ProposalNotFound));
                CHECK(mm::is(mm::getProposal(user, id, open), SO::ProposalNotFound));
            }
            // Undo: visible again, and the deletion's own rejection of the open request is part of its history.
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::Ok));
            const auto back = page(s.userA, id, 10);
            CHECK(outcomeOf(back) == SO::Ok);
            if (back && back->outcome == SO::Ok) {
                CHECK_EQ(back->memories.size(), 2u);
                const auto *o = find(*back, open);
                CHECK(o && (*o)["status"].asString() == "rejected");
            }
            // An expired cycle, then the irreversible erasure: every read stays hidden, for both members.
            const auto expired = base::cycleAged(id, s.couple, s.userB, "169 hours");
            erase::eraseNow(expired);
            CHECK_EQ(erase::purges(id), 1);
            for (const auto &user : {s.userA, s.userB}) {
                CHECK(outcomeOf(page(user, id, 10)) == SO::NotFound);
                CHECK(mm::is(mm::getProposal(user, id, decided), SO::ProposalNotFound));
                CHECK(mm::is(mm::getProposal(user, id, open), SO::ProposalNotFound));
                CHECK(mm::is(mm::getMemory(user, id), SO::NotFound));
            }
        } catch (const std::exception &failure) {
            c::expectThat(false, "ph-deleted", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::memoryproposalhistory
