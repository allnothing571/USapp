#pragma once

// P3-1b: the real MemoryService, MediaService and cleanup job against FakeObjectStore, on literal usapp_test. Isolated committed
// fixtures; assertions target this run's ids only. The test pool has two connections, so anything that must hold or wait beside
// the service uses a dedicated connection of its own.
#include <json/json.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "MediaDbTests.h"
#include "services/MemoryService.h"

namespace usapptest::memory {

namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using Decision = S::Decision;
using PendingS = ts::Pending<S::Status>;

inline std::optional<c::Observer> holderConn;
inline std::optional<c::Observer> waiterConn;

inline const c::Observer &obs() { return md::obs(); }
inline long long countOf(const std::string &sql, const std::string &a) { return c::scalar(obs(), sql, a); }

inline const char *kRevisionsSql = "SELECT count(*)::bigint AS n FROM memory_revisions WHERE memory_id = $1::uuid";
inline const char *kBindingsSql = "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid";
inline const char *kDecisionsSql = "SELECT count(*)::bigint AS n FROM memory_edit_decisions WHERE proposal_id = $1::uuid";
inline const char *kMemoriesInCoupleSql = "SELECT count(*)::bigint AS n FROM memories WHERE couple_id = $1::uuid";
inline const char *kTextsInCoupleSql = "SELECT count(*)::bigint AS n FROM memory_texts WHERE couple_id = $1::uuid";
inline const char *kPlansSql = "SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE media_id = $1::uuid AND scope = 'everything'";
inline const char *kBlockedSql =
    "SELECT count(*)::bigint AS n FROM pg_stat_activity a WHERE cardinality(pg_blocking_pids(a.pid)) > 0 AND a.query LIKE $1";
inline const char *kPlanInsertSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) VALUES ($1::uuid, 'everything', 'unattached', clock_timestamp())";
inline const char *kLockMedia = "SELECT pg_advisory_xact_lock(33018::integer, hashtext($1::text))";
inline const char *kLockMemory = "SELECT pg_advisory_xact_lock(33020::integer, hashtext($1::text))";

inline S::Terms terms(const std::string &title, const std::vector<std::string> &photos, const std::string &date = "2026-03-01",
                      const std::string &description = "") {
    return S::Terms{date, title, description, photos};
}
inline std::shared_ptr<PendingS> startCreate(const std::string &user, const S::Terms &t, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<PendingS>();
    S::instance().create(user, key, t, [p](S::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingS> startEdit(const std::string &user, const std::string &id, int version, const S::Terms &t, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<PendingS>();
    S::instance().edit(user, id, key, version, t, [p](S::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingS> startPropose(const std::string &user, const std::string &id, int version, const S::Terms &t, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<PendingS>();
    S::instance().propose(user, id, key, version, t, [p](S::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::shared_ptr<PendingS> startDecide(const std::string &user, const std::string &id, const std::string &proposal, Decision d,
                                             const std::string &key = c::newUuid(), S::Fault fault = S::Fault::None) {
    auto p = std::make_shared<PendingS>();
    S::instance().decide(user, id, proposal, key, d, [p](S::Status s) { p->deliver(std::move(s)); }, fault);
    return p;
}
inline std::optional<S::Status> create(const std::string &user, const S::Terms &t, const std::string &key = c::newUuid()) { return startCreate(user, t, key)->wait(); }
inline std::optional<S::Status> edit(const std::string &user, const std::string &id, int version, const S::Terms &t, const std::string &key = c::newUuid()) { return startEdit(user, id, version, t, key)->wait(); }
inline std::optional<S::Status> propose(const std::string &user, const std::string &id, int version, const S::Terms &t, const std::string &key = c::newUuid()) { return startPropose(user, id, version, t, key)->wait(); }
inline std::optional<S::Status> decide(const std::string &user, const std::string &id, const std::string &proposal, Decision d, const std::string &key = c::newUuid(), S::Fault fault = S::Fault::None) {
    return startDecide(user, id, proposal, d, key, fault)->wait();
}
inline std::optional<S::Status> getMemory(const std::string &user, const std::string &id) {
    auto p = std::make_shared<PendingS>();
    S::instance().get(user, id, [p](S::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline std::optional<S::Status> getProposal(const std::string &user, const std::string &id, const std::string &proposal) {
    auto p = std::make_shared<PendingS>();
    S::instance().getProposal(user, id, proposal, [p](S::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline std::optional<S::ListResult> listPage(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    auto p = std::make_shared<ts::Pending<S::ListResult>>();
    std::optional<S::Cursor> decoded;
    if (cursor) decoded = S::decodeCursor(*cursor);
    S::instance().list(user, limit, decoded, [p](S::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline bool is(const std::optional<S::Status> &s, SO o) { return s.has_value() && s->outcome == o; }
inline std::string idOf(const std::optional<S::Status> &s) { return s ? s->memory["id"].asString() : std::string(); }
inline std::string proposalOf(const std::optional<S::Status> &s) { return s ? s->mutation["proposal_id"].asString() : std::string(); }
inline std::vector<std::string> photosOf(const Json::Value &item) {
    std::vector<std::string> out;
    for (const auto &p : item["photos"]) out.push_back(p["media_id"].asString());
    return out;
}
// Two ids in the order their locks are taken.
inline std::pair<std::string, std::string> byLockOrder(const std::string &a, const std::string &b) {
    const auto h = [](const std::string &id) { return c::scalar(obs(), "SELECT hashtext($1)::bigint AS n", id); };
    return h(a) < h(b) ? std::make_pair(a, b) : std::make_pair(b, a);
}
inline void waitBlocked(const std::string &queryPrefix) {
    md::waitUntil([&]() { return c::scalar(obs(), kBlockedSql, queryPrefix + "%") >= 1; });
}
inline std::shared_ptr<ts::Pending<bool>> startPlan(const std::string &media) {
    auto planning = std::make_shared<ts::Pending<bool>>();
    waiterConn->client->execSqlAsync(kPlanInsertSql, [planning](const drogon::orm::Result &) { planning->deliver(true); },
                                     [planning](const drogon::orm::DrogonDbException &) { planning->deliver(false); }, media);
    return planning;
}
inline bool planRefused(const std::string &media) {
    const auto result = startPlan(media)->wait();
    return result.has_value() && !*result;
}

// History with chosen ages, committed directly in one transaction: a memory by userB with one photo of theirs; a proposal by userA
// naming `proposed`, and, unless decidedAgo is empty, its rejection that long ago. The revision is the base, so a READY older than
// the decision is what makes the release clock observable.
inline std::string agedProposal(const ts::CommittedSpace &s, const std::string &proposed, const std::string &decidedAgo) {
    const auto photo = md::agedReady(md::env.pool, s, s.userB);
    const auto id = c::newUuid();
    const auto proposal = c::newUuid();
    std::string body =
        "DO $aged$ DECLARE t uuid; BEGIN "
        "INSERT INTO memories (id, couple_id, creator_id, created_at) VALUES ('" + id + "', '" + s.couple + "', '" + s.userB + "', clock_timestamp() - interval '30 days'); "
        "INSERT INTO memory_texts (couple_id, title) VALUES ('" + s.couple + "', 'aged') RETURNING id INTO t; "
        "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint, created_at) "
        "VALUES ('" + id + "', '" + s.couple + "', 1, '" + s.userB + "', 'create', DATE '2026-01-01', t, '" + c::newUuid() + "', repeat('a', 64), clock_timestamp() - interval '29 days'); "
        "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ('" + photo + "', '" + s.couple + "', 'memory', '" + id + "', '" + s.userB + "'); "
        "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) VALUES ('" + id + "', 1, 1, '" + photo + "', '" + s.couple + "'); "
        "INSERT INTO memory_edit_proposals (id, couple_id, memory_id, proposer_id, base_version, memory_date, text_id, idempotency_key, request_fingerprint, created_at) "
        "VALUES ('" + proposal + "', '" + s.couple + "', '" + id + "', '" + s.userA + "', 1, DATE '2026-01-02', t, '" + c::newUuid() + "', repeat('a', 64), clock_timestamp() - interval '20 days'); "
        "INSERT INTO memory_proposal_photos (proposal_id, position, media_id, couple_id) VALUES ('" + proposal + "', 1, '" + proposed + "', '" + s.couple + "'); ";
    if (!decidedAgo.empty()) {
        body += "INSERT INTO memory_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint, decided_at) "
                "VALUES ('" + proposal + "', '" + s.couple + "', '" + s.userB + "', 'rejected', '" + c::newUuid() + "', repeat('a', 64), clock_timestamp() - interval '" + decidedAgo + "'); ";
    }
    md::env.pool->execSqlSync(body + "END $aged$");
    return proposal;
}

inline void runMemoryDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    holderConn = c::Observer::connect(dsn);
    waiterConn = c::Observer::connect(dsn);
    const auto aged = [&](const ts::CommittedSpace &s, const std::string &uploader = "") { return md::agedReady(pool, s, uploader); };

    TEST_CASE("P3-1b: create keeps the date, text, creator and photo order; a retry replays; a changed retry is refused") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto p1 = aged(s), p2 = aged(s);
            const std::string key = c::newUuid();
            const auto made = create(s.userA, terms("海边", {p2, p1}, "2026-03-01", "第一行\n第二行"), key);
            CHECK(is(made, SO::Created));
            const auto id = idOf(made);
            if (made) {
                CHECK_EQ(made->memory["version"].asInt(), 1);
                CHECK_EQ(made->memory["creator_id"].asString(), s.userA);
                CHECK_EQ(made->memory["date"].asString(), std::string("2026-03-01"));
                CHECK_EQ(made->memory["title"].asString(), std::string("海边"));
                CHECK_EQ(made->memory["description"].asString(), std::string("第一行\n第二行"));
                CHECK(!made->memory["created_at"].asString().empty());
                CHECK(made->memory["pending_proposal"].isNull());
                CHECK(photosOf(made->memory) == std::vector<std::string>({p2, p1}));
                // No response carries a key.
                Json::StreamWriterBuilder writer;
                CHECK(Json::writeString(writer, made->memory).find("media/") == std::string::npos);
                CHECK(Json::writeString(writer, made->memory).find("staging") == std::string::npos);
            }
            CHECK_EQ(countOf(kBindingsSql, p1), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = 'memory.created' AND detail::text NOT LIKE '%海边%'", id), 1);

            const auto replay = create(s.userA, terms("海边", {p2, p1}, "2026-03-01", "第一行\n第二行"), key);
            CHECK(is(replay, SO::Replayed));
            CHECK_EQ(idOf(replay), id);
            CHECK_EQ(countOf(kMemoriesInCoupleSql, s.couple), 1);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), 1);
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            CHECK(is(create(s.userA, terms("另一个", {p2, p1}), key), SO::KeyReused));
            // A photo order change is a different request.
            CHECK(is(create(s.userA, terms("海边", {p1, p2}, "2026-03-01", "第一行\n第二行"), key), SO::KeyReused));

            // Both members read it; today is allowed, a future date is not, and the database agrees.
            const auto read = getMemory(s.userB, id);
            CHECK(is(read, SO::Ok));
            if (read) CHECK_EQ(read->memory["title"].asString(), std::string("海边"));
            CHECK(is(create(s.userA, terms("明天", {aged(s)}, "2099-01-01")), SO::Validation));
            CHECK(is(create(s.userA, terms("今天", {aged(s)}, obs().client->execSqlSync("SELECT to_char((clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date, 'YYYY-MM-DD') AS d")[0]["d"].as<std::string>())), SO::Created));

            // A real upload: the partner cannot read it until it is attached, and then reads the same bytes.
            const auto u = md::uploaded(s.userA);
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
            CHECK(md::is(md::content(s.userB, u.id), md::MO::NotFound));
            CHECK(is(create(s.userA, terms("上传", {u.id})), SO::Created));
            CHECK_EQ(md::served(s.userB, u.id), md::image());
        } catch (const std::exception &) {
            c::expectThat(false, "memory-create", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: same couple only, in the service and in the database") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto p = aged(s);
            const auto made = create(s.userA, terms("ours", {p}));
            const auto id = idOf(made);
            const auto proposal = propose(s.userB, id, 1, terms("partner", {aged(s, s.userB)}));
            CHECK(is(proposal, SO::Created));
            const auto pid = proposalOf(proposal);
            const auto theirs = aged(other);

            CHECK(is(getMemory(other.userA, id), SO::NotFound));
            CHECK(is(edit(other.userA, id, 1, terms("x", {theirs})), SO::NotFound));
            CHECK(is(propose(other.userA, id, 1, terms("x", {theirs})), SO::NotFound));
            CHECK(is(decide(other.userA, id, pid, Decision::Approve), SO::NotFound));
            CHECK(is(decide(other.userA, id, pid, Decision::Withdraw), SO::NotFound));
            CHECK(is(getProposal(other.userA, id, pid), SO::ProposalNotFound));
            const auto page = listPage(other.userA, 10);
            CHECK(page.has_value() && page->outcome == SO::Ok && page->memories.empty());
            // Neither couple can use the other's photos.
            CHECK(is(create(other.userA, terms("x", {p})), SO::PhotoUnavailable));
            CHECK(is(create(s.userA, terms("x", {theirs})), SO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, theirs), 0);
            // Not a member at all.
            const auto stranger = c::newUuid();
            CHECK(is(create(stranger, terms("x", {p})), SO::SpaceNotFound));
            const auto strangerPage = listPage(stranger, 10);
            CHECK(strangerPage.has_value() && strangerPage->outcome == SO::SpaceNotFound);
            CHECK(is(getMemory(stranger, id), SO::NotFound));

            // Directly: a binding to the other couple's memory, and a listed photo of the other couple.
            bool refused = false;
            try {
                pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)",
                                  theirs, other.couple, id, other.userA);
            } catch (const std::exception &) { refused = true; }
            CHECK(refused);
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "memory-cross-couple", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: the creator edits directly; the partner proposes; each is refused the other's action") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto p = aged(s);
            const auto id = idOf(create(s.userA, terms("t", {p})));
            const auto own = aged(s, s.userB);

            CHECK(is(edit(s.userB, id, 1, terms("u", {p})), SO::Forbidden));
            CHECK(is(propose(s.userA, id, 1, terms("u", {p})), SO::Forbidden));
            CHECK(is(edit(s.userA, id, 2, terms("u", {p})), SO::VersionConflict));
            CHECK(is(propose(s.userB, id, 2, terms("u", {p})), SO::VersionConflict));
            CHECK(is(edit(s.userA, id, 1, terms("t", {p})), SO::Validation));
            CHECK(is(propose(s.userB, id, 1, terms("t", {p})), SO::Validation));
            CHECK(is(edit(s.userA, id, 1, terms("u", {p}, "2099-01-01")), SO::Validation));
            // The other member's photo that nothing has attached.
            CHECK(is(edit(s.userA, id, 1, terms("u", {own})), SO::PhotoUnavailable));
            CHECK(is(propose(s.userB, id, 1, terms("u", {aged(s)})), SO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, own), 0);

            // A retained photo plus a new one; the key replays; the replay is not a second revision.
            const std::string key = c::newUuid();
            const auto extra = aged(s);
            const auto edited = edit(s.userA, id, 1, terms("u", {extra, p}), key);
            CHECK(is(edited, SO::Created));
            if (edited) CHECK_EQ(edited->memory["version"].asInt(), 2);
            CHECK(is(edit(s.userA, id, 1, terms("u", {extra, p}), key), SO::Replayed));
            CHECK_EQ(countOf(kRevisionsSql, id), 2);
            CHECK_EQ(countOf(kBindingsSql, p), 1);
            CHECK(is(edit(s.userA, id, 1, terms("v", {extra, p}), key), SO::KeyReused));

            // Pending: both directions are held; only the right member may decide.
            const auto pending = propose(s.userB, id, 2, terms("w", {own}));
            CHECK(is(pending, SO::Created));
            const auto pid = proposalOf(pending);
            CHECK(is(edit(s.userA, id, 2, terms("x", {p})), SO::ProposalPending));
            CHECK(is(propose(s.userB, id, 2, terms("x", {p})), SO::ProposalPending));
            CHECK(is(decide(s.userB, id, pid, Decision::Approve), SO::Forbidden));
            CHECK(is(decide(s.userB, id, pid, Decision::Reject), SO::Forbidden));
            CHECK(is(decide(s.userA, id, pid, Decision::Withdraw), SO::Forbidden));
            CHECK(is(decide(s.userA, id, c::newUuid(), Decision::Approve), SO::ProposalNotFound));
            CHECK(is(getProposal(s.userA, id, c::newUuid()), SO::ProposalNotFound));
            const auto read = getMemory(s.userA, id);
            if (read) CHECK_EQ(read->memory["pending_proposal"]["id"].asString(), pid);
        } catch (const std::exception &) {
            c::expectThat(false, "memory-permissions", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: approval commits the revision and bindings together; the original stays reviewable; a replay changes nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto p = aged(s);
            const auto id = idOf(create(s.userA, terms("原来", {p}, "2026-03-01", "原描述")));
            const auto mine = aged(s, s.userB);
            const auto pending = propose(s.userB, id, 1, terms("建议", {mine, p}, "2026-03-02", "新描述"));
            CHECK(is(pending, SO::Created));
            const auto pid = proposalOf(pending);

            // Pending: reserved, not bound; the creator can look at it; current state is untouched.
            CHECK_EQ(countOf(kBindingsSql, mine), 0);
            CHECK_EQ(md::stateOf(md::facts(s.userA, mine)), std::string("ready"));
            CHECK(planRefused(mine));
            const auto during = getMemory(s.userB, id);
            if (during) CHECK_EQ(during->memory["title"].asString(), std::string("原来"));

            const std::string key = c::newUuid();
            const auto approved = decide(s.userA, id, pid, Decision::Approve, key);
            CHECK(is(approved, SO::Ok));
            if (approved) {
                CHECK_EQ(approved->mutation["outcome"].asString(), std::string("approved"));
                CHECK_EQ(approved->mutation["version"].asInt(), 2);
                CHECK_EQ(approved->memory["version"].asInt(), 2);
                CHECK_EQ(approved->memory["title"].asString(), std::string("建议"));
                CHECK_EQ(approved->memory["date"].asString(), std::string("2026-03-02"));
                CHECK_EQ(approved->memory["creator_id"].asString(), s.userA);
                CHECK(approved->memory["pending_proposal"].isNull());
                CHECK(photosOf(approved->memory) == std::vector<std::string>({mine, p}));
            }
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid AND bound_by = $2::uuid", mine, s.userA), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revisions WHERE memory_id = $1::uuid AND action = 'approve' AND actor_id = $2::uuid", id, s.userA), 1);

            // Both revisions stay: the review reads the immutable original and suggestion.
            const auto review = getProposal(s.userB, id, pid);
            CHECK(is(review, SO::Ok));
            if (review) {
                CHECK_EQ(review->proposal["status"].asString(), std::string("approved"));
                CHECK_EQ(review->proposal["approved_version"].asInt(), 2);
                CHECK_EQ(review->proposal["original"]["title"].asString(), std::string("原来"));
                CHECK_EQ(review->proposal["original"]["description"].asString(), std::string("原描述"));
                CHECK_EQ(review->proposal["original"]["date"].asString(), std::string("2026-03-01"));
                CHECK(photosOf(review->proposal["original"]) == std::vector<std::string>({p}));
                CHECK_EQ(review->proposal["proposed"]["title"].asString(), std::string("建议"));
                CHECK(photosOf(review->proposal["proposed"]) == std::vector<std::string>({mine, p}));
            }

            // Replay: read-only, even after later state moved on.
            CHECK(is(edit(s.userA, id, 2, terms("再改", {p})), SO::Created));
            const auto replay = decide(s.userA, id, pid, Decision::Approve, key);
            CHECK(is(replay, SO::Replayed));
            if (replay) {
                CHECK_EQ(replay->mutation["version"].asInt(), 2);
                CHECK_EQ(replay->memory["version"].asInt(), 3);
            }
            CHECK_EQ(countOf(kRevisionsSql, id), 3);
            CHECK_EQ(countOf(kDecisionsSql, pid), 1);
            CHECK_EQ(countOf(kBindingsSql, mine), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = 'memory.edit_approved'", id), 1);
            CHECK(is(decide(s.userA, id, pid, Decision::Reject, key), SO::KeyReused));
            CHECK(is(decide(s.userA, id, pid, Decision::Reject), SO::ProposalDecided));
            CHECK(is(decide(s.userB, id, pid, Decision::Withdraw), SO::ProposalDecided));
            // The proposal's own key replays too, with no second proposal.
            const std::string proposeKey = c::newUuid();
            const auto again = aged(s, s.userB);
            const auto second = propose(s.userB, id, 3, terms("再建议", {again}), proposeKey);
            CHECK(is(second, SO::Created));
            const auto secondReplay = propose(s.userB, id, 3, terms("再建议", {again}), proposeKey);
            CHECK(is(secondReplay, SO::Replayed));
            CHECK_EQ(proposalOf(secondReplay), proposalOf(second));
            CHECK(is(propose(s.userB, id, 3, terms("别的", {again}), proposeKey), SO::KeyReused));
        } catch (const std::exception &) {
            c::expectThat(false, "memory-approve", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: a rejection or withdrawal releases the reservation, binds nothing, and restarts the unattached period") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
            const auto rejected = aged(s, s.userB);
            const auto pending = propose(s.userB, id, 1, terms("u", {rejected}));
            const auto pid = proposalOf(pending);
            CHECK(is(decide(s.userA, id, pid, Decision::Reject), SO::Ok));
            CHECK_EQ(countOf(kBindingsSql, rejected), 0);
            // Back to the uploader alone, while the rejected suggestion stays readable as a record.
            CHECK(md::is(md::facts(s.userA, rejected), md::MO::NotFound));
            CHECK_EQ(md::stateOf(md::facts(s.userB, rejected)), std::string("ready"));
            const auto review = getProposal(s.userA, id, pid);
            if (review) {
                CHECK_EQ(review->proposal["status"].asString(), std::string("rejected"));
                CHECK(photosOf(review->proposal["proposed"]) == std::vector<std::string>({rejected}));
                CHECK_EQ(review->proposal["proposed"]["title"].asString(), std::string("u"));
            }
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            // Eight days READY, but released just now: neither the guard nor the job takes it.
            CHECK(planRefused(rejected));
            md::tick();
            CHECK_EQ(countOf(kPlansSql, rejected), 0);

            // Withdrawn: the proposer only; the same release; usable again by the uploader, and no longer visible to the partner.
            const auto withdrawnPending = propose(s.userB, id, 1, terms("v", {rejected}));
            CHECK(is(withdrawnPending, SO::Created));
            CHECK_EQ(md::stateOf(md::facts(s.userA, rejected)), std::string("ready"));
            CHECK(is(decide(s.userB, id, proposalOf(withdrawnPending), Decision::Withdraw), SO::Ok));
            CHECK(md::is(md::facts(s.userA, rejected), md::MO::NotFound));
            CHECK(planRefused(rejected));
            // And it can go somewhere else entirely.
            const auto home = create(s.userB, terms("own", {rejected}));
            CHECK(is(home, SO::Created));
            CHECK_EQ(countOf(kBindingsSql, rejected), 1);

            // The clock itself, with committed history of chosen ages.
            const auto expired = md::agedReady(pool, s);
            agedProposal(s, expired, "8 days");
            const auto recent = md::agedReady(pool, s);
            agedProposal(s, recent, "3 days");
            const auto reserved = md::agedReady(pool, s);
            agedProposal(s, reserved, "");
            md::tickUntil([&]() { return countOf(kPlansSql, expired) == 1; });
            CHECK_EQ(countOf(kPlansSql, recent), 0);
            CHECK_EQ(countOf(kPlansSql, reserved), 0);
            CHECK(planRefused(recent));
            CHECK(planRefused(reserved));
        } catch (const std::exception &) {
            c::expectThat(false, "memory-release", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: history keeps its references: a photo dropped by an edit stays bound, readable, and out of cleanup") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto p1 = aged(s), p2 = aged(s), p3 = aged(s);
            const auto id = idOf(create(s.userA, terms("t", {p1, p2})));
            CHECK(is(edit(s.userA, id, 1, terms("t", {p2, p3})), SO::Created));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revision_photos WHERE memory_id = $1::uuid AND version = 1", id), 2);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revision_photos WHERE memory_id = $1::uuid AND media_id = $2::uuid", id, p1), 1);
            CHECK_EQ(countOf(kBindingsSql, p1), 1);
            CHECK(planRefused(p1));
            md::tick();
            CHECK_EQ(countOf(kPlansSql, p1), 0);
            CHECK_EQ(md::stateOf(md::facts(s.userB, p1)), std::string("ready"));
            // One home: never another memory, even though the current revision no longer shows it. Its own memory may take it back.
            CHECK(is(create(s.userA, terms("elsewhere", {p1})), SO::PhotoUnavailable));
            CHECK(is(edit(s.userA, id, 2, terms("t", {p1})), SO::Created));
            CHECK_EQ(countOf(kBindingsSql, p1), 1);
            // A committed revision takes no further photos, even a bound one.
            bool refused = false;
            try {
                pool->execSqlSync("INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) VALUES ($1::uuid, 1, 3, $2::uuid, $3::uuid)", id, p3, s.couple);
            } catch (const std::exception &) { refused = true; }
            CHECK(refused);
        } catch (const std::exception &) {
            c::expectThat(false, "memory-history", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: attachment racing cleanup, in both orders, through the real service") {
        try {
            md::useStore();
            const auto s = md::space();
            // Plan first: it holds the lock; create, and a proposal, wait and then refuse, leaving nothing behind.
            const auto first = aged(s);
            auto plan = c::acquireTx(holderConn->client);
            CHECK(c::exec(plan, kPlanInsertSql, first).ok);
            const auto memoriesBefore = countOf(kMemoriesInCoupleSql, s.couple);
            const auto textsBefore = countOf(kTextsInCoupleSql, s.couple);
            auto waiting = startCreate(s.userA, terms("late", {first}));
            waitBlocked("SELECT pg_advisory_xact_lock(33018");
            CHECK(c::commitTx(plan).value_or(false));
            CHECK(is(waiting->wait(), SO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, first), 0);
            CHECK_EQ(countOf(kMemoriesInCoupleSql, s.couple), memoriesBefore);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), textsBefore);

            const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
            const auto second = aged(s, s.userB);
            auto planTwo = c::acquireTx(holderConn->client);
            CHECK(c::exec(planTwo, kPlanInsertSql, second).ok);
            auto proposing = startPropose(s.userB, id, 1, terms("u", {second}));
            waitBlocked("SELECT pg_advisory_xact_lock(33018");
            CHECK(c::commitTx(planTwo).value_or(false));
            CHECK(is(proposing->wait(), SO::PhotoUnavailable));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_proposal_photos WHERE media_id = $1::uuid", second), 0);

            // Attachment first: create takes the lower lock, waits for the higher one this test holds. A plan on the lower one then
            // waits behind create. Release: create commits and the plan refuses. This also shows the service locks in sorted order.
            {
                const auto ordered = byLockOrder(aged(s), aged(s));
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockMedia, ordered.second).ok);
                auto creating = startCreate(s.userA, terms("won", {ordered.first, ordered.second}));
                waitBlocked("SELECT pg_advisory_xact_lock(33018");
                auto planning = startPlan(ordered.first);
                waitBlocked("INSERT INTO media_cleanup_plans");
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(is(creating->wait(), SO::Created));
                const auto result = planning->wait();
                CHECK(result.has_value() && !*result);
                CHECK_EQ(countOf(kPlansSql, ordered.first), 0);
                CHECK_EQ(countOf(kBindingsSql, ordered.first), 1);
            }

            // The same order for a reservation, and for approval: the plan loses to the proposal, and to the binding.
            {
                const auto ordered = byLockOrder(aged(s, s.userB), aged(s, s.userB));
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, kLockMedia, ordered.second).ok);
                auto reserving = startPropose(s.userB, id, 1, terms("v", {ordered.first, ordered.second}));
                waitBlocked("SELECT pg_advisory_xact_lock(33018");
                auto planning = startPlan(ordered.first);
                waitBlocked("INSERT INTO media_cleanup_plans");
                CHECK(c::commitTx(hold).value_or(false));
                const auto reservation = reserving->wait();
                CHECK(is(reservation, SO::Created));
                const auto result = planning->wait();
                CHECK(result.has_value() && !*result);
                CHECK_EQ(countOf(kPlansSql, ordered.first), 0);
                CHECK_EQ(countOf(kBindingsSql, ordered.first), 0);

                auto holdAgain = c::acquireTx(holderConn->client);
                CHECK(c::exec(holdAgain, kLockMedia, ordered.second).ok);
                auto approving = startDecide(s.userA, id, proposalOf(reservation), Decision::Approve);
                waitBlocked("SELECT pg_advisory_xact_lock(33018");
                auto planningAgain = startPlan(ordered.first);
                waitBlocked("INSERT INTO media_cleanup_plans");
                CHECK(c::commitTx(holdAgain).value_or(false));
                CHECK(is(approving->wait(), SO::Ok));
                const auto refused = planningAgain->wait();
                CHECK(refused.has_value() && !*refused);
                CHECK_EQ(countOf(kPlansSql, ordered.first), 0);
                CHECK_EQ(countOf(kBindingsSql, ordered.first), 1);
                CHECK_EQ(countOf(kBindingsSql, ordered.second), 1);
            }
            // The plan that won stands, once; the job adds nothing to it.
            md::tick();
            CHECK_EQ(countOf(kPlansSql, first), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "memory-cleanup-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: lock order is one-way: completion and cleanup never wait on a memory, and opposite photo orders cannot deadlock") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
            // Holding the memory lock: completion, the job and reads all finish, and the edit waits.
            const auto u = md::uploaded(s.userA);
            const auto job = aged(s);
            auto hold = c::acquireTx(holderConn->client);
            CHECK(c::exec(hold, kLockMemory, id).ok);
            CHECK_EQ(md::stateOf(md::complete(s.userA, u.id)), std::string("ready"));
            md::tickUntil([&]() { return countOf(kPlansSql, job) == 1; });
            CHECK(is(getMemory(s.userA, id), SO::Ok));
            auto editing = startEdit(s.userA, id, 1, terms("u", {u.id}));
            waitBlocked("SELECT pg_advisory_xact_lock(33020");
            CHECK(c::commitTx(hold).value_or(false));
            CHECK(is(editing->wait(), SO::Created));

            // The same two photos named in opposite orders, concurrently: one wins, the other refuses, neither deadlocks.
            for (int round = 0; round < 5; ++round) {
                const auto x = aged(s), y = aged(s);
                auto one = startCreate(s.userA, terms("one", {x, y}));
                auto two = startCreate(s.userA, terms("two", {y, x}));
                const auto a = one->wait();
                const auto b = two->wait();
                CHECK(a.has_value() && b.has_value());
                CHECK((is(a, SO::Created) && is(b, SO::PhotoUnavailable)) || (is(b, SO::Created) && is(a, SO::PhotoUnavailable)));
                CHECK_EQ(countOf(kBindingsSql, x), 1);
                CHECK_EQ(countOf(kBindingsSql, y), 1);
            }
            CHECK(md::clean());
        } catch (const std::exception &) {
            c::expectThat(false, "memory-lock-order", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: concurrent proposals and decisions: one wins, and the state is what that winner says") {
        try {
            md::useStore();
            const auto s = md::space();
            // The same create key twice at once: one write, one replay.
            for (int round = 0; round < 3; ++round) {
                const auto p = aged(s);
                const std::string key = c::newUuid();
                auto one = startCreate(s.userA, terms("same", {p}), key);
                auto two = startCreate(s.userA, terms("same", {p}), key);
                const auto a = one->wait();
                const auto b = two->wait();
                CHECK((is(a, SO::Created) && is(b, SO::Replayed)) || (is(b, SO::Created) && is(a, SO::Replayed)));
                CHECK_EQ(idOf(a), idOf(b));
                CHECK_EQ(countOf(kRevisionsSql, idOf(a)), 1);
            }
            // Two proposals at once: one pending.
            {
                const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
                auto one = startPropose(s.userB, id, 1, terms("a", {aged(s, s.userB)}));
                auto two = startPropose(s.userB, id, 1, terms("b", {aged(s, s.userB)}));
                const auto a = one->wait();
                const auto b = two->wait();
                CHECK((is(a, SO::Created) && is(b, SO::ProposalPending)) || (is(b, SO::Created) && is(a, SO::ProposalPending)));
                CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_edit_proposals WHERE memory_id = $1::uuid", id), 1);
            }
            // A creator edit against a partner proposal: exactly one of them lands.
            {
                const auto p = aged(s);
                const auto id = idOf(create(s.userA, terms("t", {p})));
                auto editing = startEdit(s.userA, id, 1, terms("edit", {p}));
                auto proposing = startPropose(s.userB, id, 1, terms("prop", {p}));
                const auto e = editing->wait();
                const auto r = proposing->wait();
                CHECK((is(e, SO::Created) && is(r, SO::VersionConflict)) || (is(r, SO::Created) && is(e, SO::ProposalPending)));
                CHECK_EQ(countOf(kRevisionsSql, id), is(e, SO::Created) ? 2 : 1);
            }
            // Approve, reject and withdraw at once.
            for (int round = 0; round < 4; ++round) {
                const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
                const auto photo = aged(s, s.userB);
                const auto pending = propose(s.userB, id, 1, terms("u", {photo}));
                const auto pid = proposalOf(pending);
                auto approve = startDecide(s.userA, id, pid, Decision::Approve);
                auto reject = startDecide(s.userA, id, pid, Decision::Reject);
                auto withdraw = startDecide(s.userB, id, pid, Decision::Withdraw);
                const auto a = approve->wait();
                const auto r = reject->wait();
                const auto w = withdraw->wait();
                const int wins = (is(a, SO::Ok) ? 1 : 0) + (is(r, SO::Ok) ? 1 : 0) + (is(w, SO::Ok) ? 1 : 0);
                CHECK_EQ(wins, 1);
                const int decided = (is(a, SO::ProposalDecided) ? 1 : 0) + (is(r, SO::ProposalDecided) ? 1 : 0) + (is(w, SO::ProposalDecided) ? 1 : 0);
                CHECK_EQ(decided, 2);
                CHECK_EQ(countOf(kDecisionsSql, pid), 1);
                CHECK_EQ(countOf(kRevisionsSql, id), is(a, SO::Ok) ? 2 : 1);
                CHECK_EQ(countOf(kBindingsSql, photo), is(a, SO::Ok) ? 1 : 0);
            }
            // The same approve key at once: one approval, one replay, one binding.
            {
                const auto id = idOf(create(s.userA, terms("t", {aged(s)})));
                const auto photo = aged(s, s.userB);
                const auto pid = proposalOf(propose(s.userB, id, 1, terms("u", {photo})));
                const std::string key = c::newUuid();
                auto one = startDecide(s.userA, id, pid, Decision::Approve, key);
                auto two = startDecide(s.userA, id, pid, Decision::Approve, key);
                const auto a = one->wait();
                const auto b = two->wait();
                CHECK((is(a, SO::Ok) && is(b, SO::Replayed)) || (is(b, SO::Ok) && is(a, SO::Replayed)));
                CHECK_EQ(countOf(kRevisionsSql, id), 2);
                CHECK_EQ(countOf(kBindingsSql, photo), 1);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "memory-concurrency", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: a failed photo binding rolls back the whole approval, or the whole edit") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto p = aged(s);
            const auto id = idOf(create(s.userA, terms("t", {p})));
            const auto photo = aged(s, s.userB);
            const auto pid = proposalOf(propose(s.userB, id, 1, terms("u", {photo})));
            const auto textsBefore = countOf(kTextsInCoupleSql, s.couple);

            const std::string key = c::newUuid();
            const auto failed = decide(s.userA, id, pid, Decision::Approve, key, S::Fault::FailPhotoBinding);
            CHECK(is(failed, SO::Error));
            CHECK_EQ(countOf(kDecisionsSql, pid), 0);
            CHECK_EQ(countOf(kRevisionsSql, id), 1);
            CHECK_EQ(countOf(kBindingsSql, photo), 0);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = 'memory.edit_approved'", id), 0);
            // Nothing happened, so the reservation holds, the proposal is still pending, and the same key still works.
            CHECK(planRefused(photo));
            const auto read = getMemory(s.userA, id);
            if (read) CHECK_EQ(read->memory["pending_proposal"]["id"].asString(), pid);
            const auto retried = decide(s.userA, id, pid, Decision::Approve, key);
            CHECK(is(retried, SO::Ok));
            if (retried) CHECK_EQ(retried->memory["version"].asInt(), 2);
            CHECK_EQ(countOf(kBindingsSql, photo), 1);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), textsBefore);

            // A real refusal part-way down a photo list: nothing of the edit survives.
            const auto good = aged(s);
            const auto planned = aged(s);
            CHECK(!planRefused(planned));
            CHECK(is(edit(s.userA, id, 2, terms("v", {good, planned})), SO::PhotoUnavailable));
            CHECK_EQ(countOf(kBindingsSql, good), 0);
            CHECK_EQ(countOf(kRevisionsSql, id), 2);
            CHECK_EQ(countOf(kTextsInCoupleSql, s.couple), textsBefore);
        } catch (const std::exception &) {
            c::expectThat(false, "memory-rollback", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1b: the timeline pages by date, stays stable across inserts, and never repeats or skips") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto empty = listPage(s.userA, 10);
            CHECK(empty.has_value() && empty->outcome == SO::Ok && empty->memories.empty() && !empty->nextCursor);
            // Three on one date, so pages split a tie.
            std::vector<std::string> expected;
            std::vector<std::string> sameDay;
            for (int i = 0; i < 3; ++i) sameDay.push_back(idOf(create(s.userA, terms("same" + std::to_string(i), {aged(s)}, "2026-03-01"))));
            const auto newer = idOf(create(s.userB, terms("newer", {aged(s, s.userB)}, "2026-04-01")));
            const auto older = idOf(create(s.userA, terms("older", {aged(s)}, "2026-02-01")));
            expected.push_back(newer);
            expected.insert(expected.end(), sameDay.rbegin(), sameDay.rend());
            expected.push_back(older);

            std::vector<std::string> seen;
            std::optional<std::string> cursor;
            int pages = 0;
            do {
                const auto page = listPage(s.userA, 2, cursor);
                if (!page || page->outcome != SO::Ok) throw std::runtime_error("page");
                for (const auto &item : page->memories) seen.push_back(item["id"].asString());
                cursor = page->nextCursor;
                if (++pages == 1) {
                    // Between pages: a new row ahead of the cursor, and an edit that keeps a later row's date.
                    CHECK(is(create(s.userA, terms("inserted", {aged(s)}, "2026-05-01")), SO::Created));
                    CHECK(is(edit(s.userA, sameDay[0], 1, terms("edited", {aged(s)}, "2026-03-01")), SO::Created));
                }
            } while (cursor && pages < 10);
            CHECK(seen == expected);
            // The next reader sees the new row first, the edit in place, and the partner the same list.
            const auto fresh = listPage(s.userB, 100);
            if (fresh) {
                CHECK_EQ(fresh->memories.size(), 6u);
                CHECK_EQ(fresh->memories[0]["title"].asString(), std::string("inserted"));
                CHECK(!fresh->nextCursor);
                CHECK_EQ(fresh->memories[4]["title"].asString(), std::string("edited"));
                CHECK_EQ(fresh->memories[4]["version"].asInt(), 2);
            }
            // Exactly a page: no further cursor.
            const auto exact = listPage(s.userA, 6);
            if (exact) {
                CHECK_EQ(exact->memories.size(), 6u);
                CHECK(!exact->nextCursor);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "memory-timeline", "case failed (details withheld)");
        }
    }
    holderConn.reset();
    waiterConn.reset();
}

}  // namespace usapptest::memory
