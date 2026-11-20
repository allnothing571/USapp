#pragma once

// P3-2b: album rename and rename requests through the real AlbumService, MediaService, scheduler and cleanup job, against
// FakeObjectStore, on literal usapp_test. Isolated committed fixtures; assertions target this run's ids only.
#include <json/json.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AlbumDbTests.h"

namespace usapptest::albumrename {

namespace al = usapptest::album;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using A = usapp::AlbumService;
using AO = A::Outcome;
using Decision = A::Decision;
using PendingA = ts::Pending<A::Status>;

inline const char *kRevisionsSql = "SELECT count(*)::bigint AS n FROM album_revisions WHERE album_id = $1::uuid";
inline const char *kProposalsSql = "SELECT count(*)::bigint AS n FROM album_edit_proposals WHERE album_id = $1::uuid";
inline const char *kDecisionsSql = "SELECT count(*)::bigint AS n FROM album_edit_decisions WHERE proposal_id = $1::uuid";
inline const char *kTextsSql = "SELECT count(*)::bigint AS n FROM album_texts WHERE couple_id = $1::uuid";
inline const char *kLockMemory = "SELECT pg_advisory_xact_lock(33020::integer, hashtext($1::text))";

inline std::shared_ptr<PendingA> startRename(const std::string &user, const std::string &album, int version, const std::string &name, const std::string &key = al::newKey()) {
    auto p = std::make_shared<PendingA>();
    A::instance().rename(user, album, key, version, name, [p](A::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingA> startPropose(const std::string &user, const std::string &album, int version, const std::string &name, const std::string &key = al::newKey()) {
    auto p = std::make_shared<PendingA>();
    A::instance().propose(user, album, key, version, name, [p](A::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingA> startDecide(const std::string &user, const std::string &album, const std::string &proposal, Decision d, const std::string &key = al::newKey(), A::Fault fault = A::Fault::None) {
    auto p = std::make_shared<PendingA>();
    A::instance().decide(user, album, proposal, key, d, [p](A::Status s) { p->deliver(std::move(s)); }, fault);
    return p;
}
inline std::optional<A::Status> rename(const std::string &user, const std::string &album, int version, const std::string &name, const std::string &key = al::newKey()) { return startRename(user, album, version, name, key)->wait(); }
inline std::optional<A::Status> propose(const std::string &user, const std::string &album, int version, const std::string &name, const std::string &key = al::newKey()) { return startPropose(user, album, version, name, key)->wait(); }
inline std::optional<A::Status> decide(const std::string &user, const std::string &album, const std::string &proposal, Decision d, const std::string &key = al::newKey(), A::Fault fault = A::Fault::None) { return startDecide(user, album, proposal, d, key, fault)->wait(); }
inline std::optional<A::Status> getProposal(const std::string &user, const std::string &album, const std::string &proposal) {
    auto p = std::make_shared<PendingA>();
    A::instance().getProposal(user, album, proposal, [p](A::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline bool is(const std::optional<A::Status> &s, AO o) { return s.has_value() && s->outcome == o; }
inline std::string proposalOf(const std::optional<A::Status> &s) { return s ? s->mutation["proposal_id"].asString() : std::string(); }
inline std::string nameOf(const std::optional<A::Status> &s) { return s ? s->album["name"].asString() : std::string(); }
inline int versionOf(const std::optional<A::Status> &s) { return s ? s->album["version"].asInt() : -1; }
inline long long countOf(const char *sql, const std::string &a) { return c::scalar(md::obs(), sql, a); }

inline void runAlbumRenameDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    al::holderConn = c::Observer::connect(dsn);
    al::waiterConn = c::Observer::connect(dsn);
    const auto aged = [&](const ts::CommittedSpace &s, const std::string &uploader = "") { return md::agedReady(pool, s, uploader); };

    TEST_CASE("P3-2b: the creator renames with the current version; history stays; the list does not move; a retry replays") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "原名"));
            const auto later = al::idOf(al::create(s.userA, "后建"));
            const std::string key = al::newKey();
            const auto renamed = rename(s.userA, id, 1, "新名", key);
            CHECK(is(renamed, AO::Created));
            CHECK_EQ(nameOf(renamed), std::string("新名"));
            CHECK_EQ(versionOf(renamed), 2);
            if (renamed) {
                CHECK_EQ(renamed->mutation["action"].asString(), std::string("edit"));
                CHECK_EQ(renamed->mutation["version"].asInt(), 2);
                CHECK(renamed->album["pending_proposal"].isNull());
            }
            CHECK_EQ(countOf(kRevisionsSql, id), 2);
            // The audit row holds ids only.
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.renamed"), 1);
            CHECK_EQ(c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = 'album.renamed' AND detail::text NOT LIKE '%新名%'", id), 1);
            // The list orders by creation, so a rename moves nothing.
            const auto page = al::listAlbums(s.userB, 10);
            if (page) {
                CHECK_EQ(page->items[0]["id"].asString(), later);
                CHECK_EQ(page->items[1]["name"].asString(), std::string("新名"));
                CHECK_EQ(page->items[1]["version"].asInt(), 2);
            }

            // Replay: the original facts and today's album, even after a later rename.
            CHECK(is(rename(s.userA, id, 2, "第三个"), AO::Created));
            const auto replay = rename(s.userA, id, 1, "新名", key);
            CHECK(is(replay, AO::Replayed));
            if (replay) {
                CHECK_EQ(replay->mutation["version"].asInt(), 2);
                CHECK_EQ(replay->album["name"].asString(), std::string("第三个"));
                CHECK_EQ(replay->album["version"].asInt(), 3);
            }
            CHECK_EQ(countOf(kRevisionsSql, id), 3);
            // A different body under the key, including a create or another album.
            CHECK(is(rename(s.userA, id, 1, "别的", key), AO::KeyReused));
            CHECK(is(rename(s.userA, id, 2, "新名", key), AO::KeyReused));
            CHECK(is(rename(s.userA, later, 1, "新名", key), AO::KeyReused));
            CHECK(is(al::create(s.userA, "新名", key), AO::KeyReused));
            const std::string createKey = al::newKey();
            const auto made = al::create(s.userA, "x", createKey);
            CHECK(is(rename(s.userA, al::idOf(made), 1, "y", createKey), AO::KeyReused));
            CHECK_EQ(versionOf(al::getAlbum(s.userA, al::idOf(made))), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-direct", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: both members' permissions, and another couple's album and requests answer exactly as absent ones") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto id = al::idOf(al::create(s.userA, "t"));
            // Each route is for one member.
            CHECK(is(rename(s.userB, id, 1, "u"), AO::EditForbidden));
            CHECK(is(propose(s.userA, id, 1, "u"), AO::EditForbidden));
            // Old and future versions; the same name.
            CHECK(is(rename(s.userA, id, 2, "u"), AO::VersionConflict));
            CHECK(is(propose(s.userB, id, 2, "u"), AO::VersionConflict));
            CHECK(is(rename(s.userA, id, 1, "t"), AO::Validation));
            CHECK(is(propose(s.userB, id, 1, "t"), AO::Validation));
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            CHECK_EQ(countOf(kProposalsSql, id), 0);
            CHECK_EQ(countOf(kTextsSql, s.couple), 1);

            const auto pending = propose(s.userB, id, 1, "申请");
            CHECK(is(pending, AO::Created));
            const auto pid = proposalOf(pending);
            // Another couple, and an absent album: the same answers, with nothing written.
            for (const auto &album : {id, c::newUuid()}) {
                CHECK(is(rename(other.userA, album, 1, "x"), AO::NotFound));
                CHECK(is(propose(other.userA, album, 1, "x"), AO::NotFound));
                CHECK(is(decide(other.userA, album, pid, Decision::Approve), AO::NotFound));
                CHECK(is(decide(other.userA, album, pid, Decision::Withdraw), AO::NotFound));
                CHECK(is(getProposal(other.userA, album, pid), AO::ProposalNotFound));
            }
            CHECK(is(getProposal(other.userA, id, c::newUuid()), AO::ProposalNotFound));
            const auto stranger = c::newUuid();
            CHECK(is(rename(stranger, id, 1, "x"), AO::SpaceNotFound));
            CHECK(is(decide(stranger, id, pid, Decision::Reject), AO::SpaceNotFound));
            CHECK(is(getProposal(stranger, id, pid), AO::ProposalNotFound));
            // A request of another album of the same couple.
            const auto second = al::idOf(al::create(s.userA, "second"));
            CHECK(is(decide(s.userA, second, pid, Decision::Approve), AO::ProposalNotFound));
            CHECK(is(decide(s.userA, id, c::newUuid(), Decision::Approve), AO::ProposalNotFound));
            CHECK(is(getProposal(s.userB, second, pid), AO::ProposalNotFound));
            // The wrong member for each outcome.
            CHECK(is(decide(s.userB, id, pid, Decision::Approve), AO::EditForbidden));
            CHECK(is(decide(s.userB, id, pid, Decision::Reject), AO::EditForbidden));
            CHECK(is(decide(s.userA, id, pid, Decision::Withdraw), AO::EditForbidden));
            CHECK_EQ(countOf(kDecisionsSql, pid), 0);
            CHECK_EQ(versionOf(al::getAlbum(s.userA, id)), 1);
            // Directly: a decision across couples.
            bool refused = false;
            try {
                pool->execSqlSync("INSERT INTO album_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint) "
                                  "VALUES ($1::uuid, $2::uuid, $3::uuid, 'rejected', $4, repeat('c', 64))", pid, other.couple, other.userA, al::newKey());
            } catch (const std::exception &) { refused = true; }
            CHECK(refused);
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-permissions", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: a request changes nothing until approved; approval applies exactly that name as a new revision") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "原来"));
            const std::string key = al::newKey();
            const auto pending = propose(s.userB, id, 1, "建议", key);
            CHECK(is(pending, AO::Created));
            const auto pid = proposalOf(pending);
            CHECK_EQ(nameOf(pending), std::string("原来"));
            CHECK_EQ(versionOf(pending), 1);
            if (pending) {
                CHECK_EQ(pending->album["pending_proposal"]["id"].asString(), pid);
                CHECK_EQ(pending->album["pending_proposal"]["proposer_id"].asString(), s.userB);
                CHECK_EQ(pending->album["pending_proposal"]["base_version"].asInt(), 1);
            }
            // Replay, reuse, one pending, and both members read V1 section 30's fields.
            CHECK(is(propose(s.userB, id, 1, "建议", key), AO::Replayed));
            CHECK(is(propose(s.userB, id, 1, "别的", key), AO::KeyReused));
            CHECK(is(propose(s.userB, id, 1, "第二个"), AO::ProposalPending));
            CHECK_EQ(countOf(kProposalsSql, id), 1);
            for (const auto &reader : {s.userA, s.userB}) {
                const auto read = getProposal(reader, id, pid);
                CHECK(is(read, AO::Ok));
                if (read) {
                    CHECK_EQ(read->proposal["status"].asString(), std::string("pending"));
                    CHECK_EQ(read->proposal["original_name"].asString(), std::string("原来"));
                    CHECK_EQ(read->proposal["proposed_name"].asString(), std::string("建议"));
                    CHECK_EQ(read->proposal["proposer_id"].asString(), s.userB);
                    CHECK(!read->proposal["created_at"].asString().empty());
                    CHECK(read->proposal["decided_by"].isNull());
                    CHECK(read->proposal["approved_version"].isNull());
                }
            }
            // While pending, a direct rename is refused, however current its version.
            CHECK(is(rename(s.userA, id, 1, "直接"), AO::ProposalPending));
            CHECK_EQ(countOf(kRevisionsSql, id), 1);

            const std::string decisionKey = al::newKey();
            const auto approved = decide(s.userA, id, pid, Decision::Approve, decisionKey);
            CHECK(is(approved, AO::Ok));
            CHECK_EQ(nameOf(approved), std::string("建议"));
            CHECK_EQ(versionOf(approved), 2);
            if (approved) {
                CHECK(approved->album["pending_proposal"].isNull());
                CHECK_EQ(approved->mutation["action"].asString(), std::string("decide"));
                CHECK_EQ(approved->mutation["outcome"].asString(), std::string("approved"));
                CHECK_EQ(approved->mutation["version"].asInt(), 2);
                CHECK_EQ(approved->album["creator_id"].asString(), s.userA);
            }
            CHECK_EQ(c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM album_revisions WHERE album_id = $1::uuid AND action = 'approve' AND actor_id = $2::uuid AND version = 2", id, s.userA), 1);
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.rename_approved"), 1);
            // The text is the proposal's own: nothing new was written for the approval.
            CHECK_EQ(countOf(kTextsSql, s.couple), 2);
            const auto review = getProposal(s.userB, id, pid);
            if (review) {
                CHECK_EQ(review->proposal["status"].asString(), std::string("approved"));
                CHECK_EQ(review->proposal["approved_version"].asInt(), 2);
                CHECK_EQ(review->proposal["decided_by"].asString(), s.userA);
                CHECK_EQ(review->proposal["original_name"].asString(), std::string("原来"));
            }

            // Direct renames work again; then every replay is read-only, with the original facts and today's album.
            CHECK(is(rename(s.userA, id, 2, "之后"), AO::Created));
            const auto replay = decide(s.userA, id, pid, Decision::Approve, decisionKey);
            CHECK(is(replay, AO::Replayed));
            if (replay) {
                CHECK_EQ(replay->mutation["version"].asInt(), 2);
                CHECK_EQ(replay->album["name"].asString(), std::string("之后"));
            }
            const auto proposeReplay = propose(s.userB, id, 1, "建议", key);
            CHECK(is(proposeReplay, AO::Replayed));
            CHECK_EQ(proposalOf(proposeReplay), pid);
            CHECK_EQ(countOf(kRevisionsSql, id), 3);
            CHECK_EQ(countOf(kDecisionsSql, pid), 1);
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.rename_approved"), 1);
            CHECK(is(decide(s.userA, id, pid, Decision::Reject, decisionKey), AO::KeyReused));
            CHECK(is(decide(s.userA, id, pid, Decision::Reject), AO::ProposalDecided));
            CHECK(is(decide(s.userB, id, pid, Decision::Withdraw), AO::ProposalDecided));
            // An old base, after all that.
            CHECK(is(propose(s.userB, id, 2, "旧版本"), AO::VersionConflict));
            CHECK(is(rename(s.userA, id, 2, "旧版本"), AO::VersionConflict));
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-approve", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: a rejection or withdrawal changes no name or version, stays readable, and frees the album for what comes next") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userB, "t"));
            // Here userB is the creator and userA the partner.
            const auto first = propose(s.userA, id, 1, "拒绝我");
            const auto firstId = proposalOf(first);
            const std::string key = al::newKey();
            const auto rejected = decide(s.userB, id, firstId, Decision::Reject, key);
            CHECK(is(rejected, AO::Ok));
            CHECK_EQ(nameOf(rejected), std::string("t"));
            CHECK_EQ(versionOf(rejected), 1);
            if (rejected) {
                CHECK(rejected->album["pending_proposal"].isNull());
                CHECK_EQ(rejected->mutation["outcome"].asString(), std::string("rejected"));
                CHECK(rejected->mutation["version"].isNull());
            }
            CHECK(is(decide(s.userB, id, firstId, Decision::Reject, key), AO::Replayed));
            const auto read = getProposal(s.userA, id, firstId);
            if (read) {
                CHECK_EQ(read->proposal["status"].asString(), std::string("rejected"));
                CHECK_EQ(read->proposal["proposed_name"].asString(), std::string("拒绝我"));
                CHECK(read->proposal["approved_version"].isNull());
            }
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.rename_rejected"), 1);

            const auto second = propose(s.userA, id, 1, "撤回我");
            CHECK(is(second, AO::Created));
            const auto withdrawn = decide(s.userA, id, proposalOf(second), Decision::Withdraw);
            CHECK(is(withdrawn, AO::Ok));
            CHECK_EQ(nameOf(withdrawn), std::string("t"));
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.rename_withdrawn"), 1);
            const auto readTwo = getProposal(s.userB, id, proposalOf(second));
            if (readTwo) CHECK_EQ(readTwo->proposal["status"].asString(), std::string("withdrawn"));
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            // The same requested name may be asked again, and the creator may rename.
            CHECK(is(propose(s.userA, id, 1, "拒绝我"), AO::Created));
            const auto third = al::getAlbum(s.userB, id);
            if (third) CHECK(is(decide(s.userB, id, third->album["pending_proposal"]["id"].asString(), Decision::Reject), AO::Ok));
            CHECK(is(rename(s.userB, id, 1, "直接"), AO::Created));
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-decline", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: name races: one wins, and no stale approval can be reached") {
        try {
            md::useStore();
            const auto s = md::space();
            // Two renames at one version.
            for (int round = 0; round < 3; ++round) {
                const auto id = al::idOf(al::create(s.userA, "t"));
                auto one = startRename(s.userA, id, 1, "a");
                auto two = startRename(s.userA, id, 1, "b");
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::VersionConflict)) || (is(y, AO::Created) && is(x, AO::VersionConflict)));
                CHECK_EQ(countOf(kRevisionsSql, id), 2);
            }
            // A rename against a request: exactly one lands, and the album is never left with a stale pending request.
            for (int round = 0; round < 5; ++round) {
                const auto id = al::idOf(al::create(s.userA, "t"));
                auto renaming = startRename(s.userA, id, 1, "a");
                auto proposing = startPropose(s.userB, id, 1, "b");
                const auto r = renaming->wait();
                const auto p = proposing->wait();
                CHECK((is(r, AO::Created) && is(p, AO::VersionConflict)) || (is(p, AO::Created) && is(r, AO::ProposalPending)));
                const auto now = al::getAlbum(s.userA, id);
                if (now && now->album["pending_proposal"].isObject()) {
                    CHECK_EQ(now->album["pending_proposal"]["base_version"].asInt(), now->album["version"].asInt());
                    CHECK(is(decide(s.userA, id, now->album["pending_proposal"]["id"].asString(), Decision::Approve), AO::Ok));
                }
            }
            // Two requests.
            for (int round = 0; round < 3; ++round) {
                const auto id = al::idOf(al::create(s.userA, "t"));
                auto one = startPropose(s.userB, id, 1, "a");
                auto two = startPropose(s.userB, id, 1, "b");
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::ProposalPending)) || (is(y, AO::Created) && is(x, AO::ProposalPending)));
                CHECK_EQ(countOf(kProposalsSql, id), 1);
            }
            // Approve, reject and withdraw at once.
            for (int round = 0; round < 5; ++round) {
                const auto id = al::idOf(al::create(s.userA, "t"));
                const auto pid = proposalOf(propose(s.userB, id, 1, "新"));
                auto approve = startDecide(s.userA, id, pid, Decision::Approve);
                auto rejecting = startDecide(s.userA, id, pid, Decision::Reject);
                auto withdraw = startDecide(s.userB, id, pid, Decision::Withdraw);
                const auto a = approve->wait();
                const auto r = rejecting->wait();
                const auto w = withdraw->wait();
                CHECK_EQ((is(a, AO::Ok) ? 1 : 0) + (is(r, AO::Ok) ? 1 : 0) + (is(w, AO::Ok) ? 1 : 0), 1);
                CHECK_EQ((is(a, AO::ProposalDecided) ? 1 : 0) + (is(r, AO::ProposalDecided) ? 1 : 0) + (is(w, AO::ProposalDecided) ? 1 : 0), 2);
                CHECK_EQ(countOf(kDecisionsSql, pid), 1);
                CHECK_EQ(countOf(kRevisionsSql, id), is(a, AO::Ok) ? 2 : 1);
                CHECK_EQ(nameOf(al::getAlbum(s.userA, id)), std::string(is(a, AO::Ok) ? "新" : "t"));
            }
            // The same key at once, on each route: one write, one replay.
            for (int round = 0; round < 3; ++round) {
                const auto id = al::idOf(al::create(s.userA, "t"));
                const std::string renameKey = al::newKey();
                auto one = startRename(s.userA, id, 1, "a", renameKey);
                auto two = startRename(s.userA, id, 1, "a", renameKey);
                const auto x = one->wait();
                const auto y = two->wait();
                CHECK((is(x, AO::Created) && is(y, AO::Replayed)) || (is(y, AO::Created) && is(x, AO::Replayed)));
                CHECK_EQ(countOf(kRevisionsSql, id), 2);

                const std::string proposeKey = al::newKey();
                auto three = startPropose(s.userB, id, 2, "b", proposeKey);
                auto four = startPropose(s.userB, id, 2, "b", proposeKey);
                const auto v = three->wait();
                const auto w = four->wait();
                CHECK((is(v, AO::Created) && is(w, AO::Replayed)) || (is(w, AO::Created) && is(v, AO::Replayed)));
                CHECK_EQ(proposalOf(v), proposalOf(w));
                CHECK_EQ(countOf(kProposalsSql, id), 1);

                const std::string decideKey = al::newKey();
                auto five = startDecide(s.userA, id, proposalOf(v), Decision::Approve, decideKey);
                auto six = startDecide(s.userA, id, proposalOf(v), Decision::Approve, decideKey);
                const auto d = five->wait();
                const auto e = six->wait();
                CHECK((is(d, AO::Ok) && is(e, AO::Replayed)) || (is(e, AO::Ok) && is(d, AO::Replayed)));
                CHECK_EQ(countOf(kRevisionsSql, id), 3);
                CHECK_EQ(countOf(kDecisionsSql, proposalOf(v)), 1);
            }
            // One key across two albums at once: one write, one told the key is taken.
            for (int round = 0; round < 3; ++round) {
                const auto one = al::idOf(al::create(s.userA, "t"));
                const auto two = al::idOf(al::create(s.userA, "t"));
                const std::string same = al::newKey();
                auto x = startRename(s.userA, one, 1, "a", same);
                auto y = startRename(s.userA, two, 1, "a", same);
                const auto a = x->wait();
                const auto b = y->wait();
                CHECK((is(a, AO::Created) && is(b, AO::KeyReused)) || (is(b, AO::Created) && is(a, AO::KeyReused)));
            }
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: photos and covers are independent of the name, while pending and while racing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "t"));
            const auto p1 = aged(s), p2 = aged(s), p3 = aged(s);
            CHECK(is(al::add(s.userA, id, {p1, p2}), AO::Created));
            const auto pending = propose(s.userB, id, 1, "新名");
            const auto pid = proposalOf(pending);
            // With a request pending: add, cover and removal all land, and none moves the version.
            const auto added = al::add(s.userA, id, {p3});
            CHECK(is(added, AO::Created));
            CHECK_EQ(versionOf(added), 1);
            const auto covered = al::cover(s.userB, id, p2);
            CHECK(is(covered, AO::Created));
            CHECK_EQ(versionOf(covered), 1);
            const auto removed = al::removePhoto(s.userA, id, p1);
            CHECK(is(removed, AO::Ok));
            CHECK_EQ(versionOf(removed), 1);
            if (removed) CHECK_EQ(removed->album["pending_proposal"]["id"].asString(), pid);
            // The base stays valid, and approval touches no photo or cover.
            const auto approved = decide(s.userA, id, pid, Decision::Approve);
            CHECK(is(approved, AO::Ok));
            CHECK_EQ(nameOf(approved), std::string("新名"));
            CHECK_EQ(al::coverOf(approved), p2);
            if (approved) CHECK_EQ(approved->album["photo_count"].asInt(), 2);
            CHECK_EQ(countOf(al::kCoversSql, id), 1);
            CHECK(al::photoIds(s.userB, id) == al::Ids({p3, p2}));
            // Replaying a write made under the old name reads today's album, with the original facts.
            const std::string key = al::newKey();
            const auto p4 = aged(s);
            CHECK(is(al::add(s.userA, id, {p4}, key), AO::Created));
            CHECK(is(rename(s.userA, id, 2, "再改"), AO::Created));
            const auto replay = al::add(s.userA, id, {p4}, key);
            CHECK(is(replay, AO::Replayed));
            CHECK_EQ(nameOf(replay), std::string("再改"));

            // Racing: each pair commits both, in either order, and the album shows both.
            for (int round = 0; round < 5; ++round) {
                const auto album = al::idOf(al::create(s.userA, "t"));
                const auto x = aged(s), y = aged(s), z = aged(s);
                CHECK(is(al::add(s.userA, album, {x, y}), AO::Created));
                const auto request = proposalOf(propose(s.userB, album, 1, "新"));
                auto approving = startDecide(s.userA, album, request, Decision::Approve);
                auto covering = al::startCover(s.userB, album, y);
                auto adding = al::startAdd(s.userA, album, {z});
                auto removing = al::startRemove(s.userA, album, x);
                CHECK(is(approving->wait(), AO::Ok));
                CHECK(is(covering->wait(), AO::Created));
                CHECK(is(adding->wait(), AO::Created));
                CHECK(is(removing->wait(), AO::Ok));
                const auto now = al::getAlbum(s.userA, album);
                CHECK_EQ(nameOf(now), std::string("新"));
                CHECK_EQ(versionOf(now), 2);
                CHECK_EQ(al::coverOf(now), y);
                if (now) CHECK_EQ(now->album["photo_count"].asInt(), 2);
            }
            // A direct rename against a cover and a removal.
            for (int round = 0; round < 3; ++round) {
                const auto album = al::idOf(al::create(s.userA, "t"));
                const auto x = aged(s), y = aged(s);
                CHECK(is(al::add(s.userA, album, {x, y}), AO::Created));
                auto renaming = startRename(s.userA, album, 1, "新");
                auto covering = al::startCover(s.userB, album, y);
                auto removing = al::startRemove(s.userA, album, x);
                CHECK(is(renaming->wait(), AO::Created));
                CHECK(is(covering->wait(), AO::Created));
                CHECK(is(removing->wait(), AO::Ok));
                const auto now = al::getAlbum(s.userB, album);
                CHECK_EQ(nameOf(now), std::string("新"));
                CHECK_EQ(al::coverOf(now), y);
            }
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-independence", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: name writes take only the album's lock, and completion and the cleanup job never wait on them") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "t"));
            const auto photo = aged(s);
            CHECK(is(al::add(s.userA, id, {photo}), AO::Created));

            // A held media lock and a held memory lock on this id block nothing here.
            auto hold = c::acquireTx(al::holderConn->client);
            CHECK(c::exec(hold, al::kLockMedia, photo).ok);
            CHECK(c::exec(hold, kLockMemory, id).ok);
            CHECK(is(rename(s.userA, id, 1, "a"), AO::Created));
            const auto pending = propose(s.userB, id, 2, "b");
            CHECK(is(pending, AO::Created));
            CHECK(is(decide(s.userA, id, proposalOf(pending), Decision::Approve), AO::Ok));
            CHECK(c::commitTx(hold).value_or(false));

            // A held album lock blocks each of them, by the blocked statement; completion and the job finish meanwhile.
            const auto u = md::uploaded(s.userA);
            const auto job = aged(s);
            auto albumHold = c::acquireTx(al::holderConn->client);
            CHECK(c::exec(albumHold, al::kLockAlbum, id).ok);
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
            md::tickUntil([&]() { return countOf(al::kPlansSql, job) == 1; });
            auto renaming = startRename(s.userA, id, 3, "c");
            al::waitBlocked("SELECT pg_advisory_xact_lock(33021");
            CHECK(c::commitTx(albumHold).value_or(false));
            CHECK(is(renaming->wait(), AO::Created));
            CHECK_EQ(versionOf(al::getAlbum(s.userB, id)), 4);
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-locks", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-2b: a failure after the approval's decision rolls back everything; the request stays pending and the same key succeeds") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "t"));
            const auto pid = proposalOf(propose(s.userB, id, 1, "新"));
            const auto textsBefore = countOf(kTextsSql, s.couple);
            const std::string key = al::newKey();
            CHECK(is(decide(s.userA, id, pid, Decision::Approve, key, A::Fault::FailAfterDecision), AO::Error));
            CHECK_EQ(countOf(kDecisionsSql, pid), 0);
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            CHECK_EQ(c::scalar(md::obs(), al::kAuditSql, id, "album.rename_approved"), 0);
            const auto after = al::getAlbum(s.userA, id);
            CHECK_EQ(nameOf(after), std::string("t"));
            if (after) CHECK_EQ(after->album["pending_proposal"]["id"].asString(), pid);
            const auto retried = decide(s.userA, id, pid, Decision::Approve, key);
            CHECK(is(retried, AO::Ok));
            CHECK_EQ(nameOf(retried), std::string("新"));
            CHECK_EQ(countOf(kTextsSql, s.couple), textsBefore);

            // A refused name write leaves no text behind.
            const auto beforeRefusal = countOf(kTextsSql, s.couple);
            CHECK(is(rename(s.userA, id, 1, "旧版本"), AO::VersionConflict));
            CHECK(is(propose(s.userB, id, 1, "旧版本"), AO::VersionConflict));
            CHECK_EQ(countOf(kTextsSql, s.couple), beforeRefusal);

            // The database refuses a direct rename while pending, whatever the service does.
            const auto request = propose(s.userB, id, 2, "再新");
            CHECK(is(request, AO::Created));
            auto tx = c::acquireTx(al::holderConn->client);
            const auto text = c::exec(tx, "INSERT INTO album_texts (couple_id, name) VALUES ($1::uuid, 'direct') RETURNING id::text AS id", s.couple);
            CHECK(text.ok);
            CHECK(al::refusedWith(c::exec(tx, "INSERT INTO album_revisions (album_id, couple_id, version, actor_id, action, text_id, idempotency_key, request_fingerprint) "
                                              "VALUES ($1::uuid, $2::uuid, 3, $3::uuid, 'edit', $4::uuid, $5, repeat('a', 64))",
                                          id, s.couple, s.userA, (*text.result)[0]["id"].as<std::string>(), al::newKey()), "USAPP_ALBUM_PROPOSAL_PENDING"));
            c::rollbackTx(tx);
            // And an approved decision with no revision does not survive a real commit.
            auto approveTx = c::acquireTx(al::holderConn->client);
            CHECK(c::exec(approveTx, "INSERT INTO album_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint) "
                                     "VALUES ($1::uuid, $2::uuid, $3::uuid, 'approved', $4, repeat('c', 64))", proposalOf(request), s.couple, s.userA, al::newKey()).ok);
            CHECK_FALSE(c::commitTx(approveTx).value_or(true));
            CHECK_EQ(countOf(kDecisionsSql, proposalOf(request)), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "album-rename-rollback", "case failed (details withheld)");
        }
    }

    al::holderConn.reset();
    al::waiterConn.reset();
}

}  // namespace usapptest::albumrename
