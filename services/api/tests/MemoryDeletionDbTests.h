#pragma once

// D6 M1a: the real MemoryService, MediaService and cleanup job against FakeObjectStore, on literal usapp_test. No vendor and no credential
// is involved. A deadline is placed by committing a cycle whose deleted_at is chosen, on a memory old enough to allow it. The plan detail
// read lives in its controller, so it is covered through the real API binary in ReminderLiveApiTest.h.
#include <json/json.h>

#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "MediaDeletionDbTests.h"

namespace usapptest::memorydeletion {

namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace del = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using MO = usapp::MediaService::Outcome;
using Decision = S::Decision;

inline std::optional<c::Observer> holderConn;

inline const c::Observer &obs() { return md::obs(); }
inline std::shared_ptr<mm::PendingS> startErase(const std::string &user, const std::string &id, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().deleteMemory(user, id, key, [p](S::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::optional<S::Status> erase(const std::string &user, const std::string &id, const std::string &key = c::newUuid()) { return startErase(user, id, key)->wait(); }
inline std::optional<S::Status> undo(const std::string &user, const std::string &id, const std::string &deletion, const std::string &key = c::newUuid(), S::Fault fault = S::Fault::None) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().undoDeletion(user, id, deletion, key, [p](S::Status s) { p->deliver(std::move(s)); }, fault);
    return p->wait();
}
inline std::optional<S::ListResult> recoveryPage(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<std::pair<std::string, std::string>> after;
    if (cursor) {
        const auto decoded = usapp::AlbumService::decodeCursor(*cursor);
        if (!decoded) throw std::runtime_error("cursor");
        after = std::make_pair(decoded->createdAt, decoded->id);
    }
    auto p = std::make_shared<ts::Pending<S::ListResult>>();
    S::instance().listDeletions(user, limit, after, [p](S::ListResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::vector<std::string> recoveryIds(const std::string &user) {
    const auto page = recoveryPage(user, 100);
    if (!page || page->outcome != SO::Ok) throw std::runtime_error("page");
    std::vector<std::string> ids;
    for (const auto &item : page->memories) ids.push_back(item["deletion_id"].asString());
    return ids;
}
inline bool lists(const std::string &user, const std::string &deletion) {
    const auto ids = recoveryIds(user);
    return std::find(ids.begin(), ids.end(), deletion) != ids.end();
}
inline bool memoryListed(const std::string &user, const std::string &memory) {
    const auto page = mm::listPage(user, 100);
    if (!page || page->outcome != SO::Ok) throw std::runtime_error("list");
    for (const auto &item : page->memories) if (item["id"].asString() == memory) return true;
    return false;
}
inline std::string cycleOf(const std::optional<S::Status> &s) { return s ? s->deletion["deletion_id"].asString() : std::string(); }
inline std::string stateOf(const std::optional<S::Status> &s) { return s ? s->deletion["state"].asString() : std::string(); }
inline std::string written(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}
inline long long cycles(const std::string &id) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletions WHERE memory_id = $1::uuid", id); }
inline long long reversals(const std::string &id) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletion_reversals WHERE memory_id = $1::uuid", id); }
inline long long revisions(const std::string &id) { return c::scalar(obs(), mm::kRevisionsSql, id); }
inline long long audits(const std::string &id, const std::string &action) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'memory' AND target_id = $1::text AND action = $2::text", id, action);
}
inline bool reservedNow(const std::string &media) {
    return obs().client->execSqlSync("SELECT usapp_media_pending_reference($1::uuid) IS NOT NULL AS v", media)[0]["v"].as<bool>();
}
inline void waitMemoryLockWaiters(long long n) {
    md::waitUntil([&]() { return c::scalar(obs(), mm::kBlockedSql, "SELECT pg_advisory_xact_lock(33020%") >= n; });
}
// A memory by userB, old enough that any cycle age below is allowed. Committed directly: only that can have a chosen age.
inline std::string agedMemory(const ts::CommittedSpace &s) {
    const auto photo = md::agedReady(md::env.pool, s, s.userB);
    const auto id = c::newUuid();
    md::env.pool->execSqlSync(
        "DO $aged$ DECLARE t uuid; BEGIN "
        "INSERT INTO memories (id, couple_id, creator_id, created_at) VALUES ('" + id + "', '" + s.couple + "', '" + s.userB + "', clock_timestamp() - interval '30 days'); "
        "INSERT INTO memory_texts (couple_id, title) VALUES ('" + s.couple + "', 'aged') RETURNING id INTO t; "
        "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint, created_at) "
        "VALUES ('" + id + "', '" + s.couple + "', 1, '" + s.userB + "', 'create', DATE '2026-01-01', t, '" + c::newUuid() + "', repeat('a', 64), clock_timestamp() - interval '29 days'); "
        "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ('" + photo + "', '" + s.couple + "', 'memory', '" + id + "', '" + s.userB + "'); "
        "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) VALUES ('" + id + "', 1, 1, '" + photo + "', '" + s.couple + "'); "
        "END $aged$");
    return id;
}
// A maintenance connection for the rows only a state from before 0038, or an impossible one, can have. It lives as long as the process.
inline const drogon::orm::DbClientPtr &maintClient() {
    static const auto *client = []() {
        const char *info = std::getenv("USAPP_TEST_MAINT_CONNINFO");
        if (!info || !*info) throw std::runtime_error("this case needs USAPP_TEST_MAINT_CONNINFO");
        auto *made = new drogon::orm::DbClientPtr(drogon::orm::DbClient::newPgClient(info, 1));
        (*made)->setTimeout(5.0);
        return made;
    }();
    return *client;
}
// One statement with triggers off, in its own transaction, and the connection may still be coming up.
inline void asMaintenance(const std::string &body) {
    for (int tries = 0;; ++tries) {
        try {
            maintClient()->execSqlSync("DO $maint$ BEGIN PERFORM set_config('session_replication_role', 'replica', true); " + body + " END $maint$");
            return;
        } catch (const std::exception &) {
            if (tries >= 50) throw;
            md::sleepMs(20);
        }
    }
}
// A cycle exactly as M1a and M1b wrote them: no linked photo cycles, and none of the commit check that 0038 adds, since those rows predate it.
// Deleted `ago` before the database's now, with the deadline that implies. The age is written by the test itself, never from outside.
inline std::string cycleAged(const std::string &id, const std::string &couple, const std::string &user, const std::string &ago, const std::string &key = c::newUuid()) {
    const auto cycle = c::newUuid();
    asMaintenance(
        "INSERT INTO memory_deletions (id, memory_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
        "SELECT '" + cycle + "', '" + id + "', '" + couple + "', '" + user + "', 1 + COALESCE((SELECT max(x.cycle_no) FROM memory_deletions x WHERE x.memory_id = '" + id + "'), 0), '" + key +
        "', '" + S::deletionFingerprint(id) + "', n.t, n.t + interval '168 hours' FROM (SELECT clock_timestamp() - interval '" + ago + "' AS t) n;");
    return cycle;
}
// A finished plan by this actor, through the same database functions as the service.
inline std::string finishedPlan(const std::string &actor) {
    const auto id = obs().client->execSqlSync(
        "SELECT usapp_shared_plan_write($1::uuid, NULL::uuid, 'create', $2, $3::jsonb) #>> '{plan,id}' AS id",
        actor, c::newUuid(), std::string(R"({"body":{"title":"PLAN-OWN-TEXT","date":null,"time":null,"place":null,"note":"PLAN-OWN-NOTE"}})"))[0]["id"].as<std::string>();
    for (const char *from : {"want", "plan"}) {
        obs().client->execSqlSync("SELECT usapp_shared_plan_write($1::uuid, $2::uuid, 'advance', $3, $4::jsonb)", actor, id, c::newUuid(),
                                  std::string(R"({"expected_status":")") + from + R"("})");
    }
    return id;
}
inline std::string planRead(const std::string &plan) {
    return obs().client->execSqlSync("SELECT usapp_shared_plan_read($1::uuid)::text AS r", plan)[0]["r"].as<std::string>();
}
inline Json::Value attach(const std::string &actor, const std::string &plan, const std::string &key, const std::string &memory) {
    const auto text = obs().client->execSqlSync("SELECT usapp_plan_memory_attach($1::uuid, $2::uuid, $3, $4::uuid)::text AS r", actor, plan, key, memory)[0]["r"].as<std::string>();
    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(text);
    if (!Json::parseFromStream(reader, stream, &parsed, &errors)) throw std::runtime_error("attach json");
    return parsed;
}
inline std::optional<S::Status> forPlan(const std::string &user, const std::string &plan, const std::string &key, const S::Terms &t) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().createForPlan(user, plan, key, t, [p](S::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}

inline void runMemoryDeletionDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    holderConn = c::Observer::connect(dsn);
    const std::string kTitle = "M1A-SENTINEL-TITLE";
    const std::string kDescription = "M1A-SENTINEL-DESCRIPTION";

    TEST_CASE("D6 M1a: only the creator deletes and undoes; the answer holds only cycle facts") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto made = mm::create(s.userB, mm::terms(kTitle, {photo}, "2026-03-01", kDescription));
            CHECK(mm::is(made, SO::Created));
            const auto id = mm::idOf(made);
            const auto spare = mm::idOf(mm::create(s.userB, mm::terms("Spare", {del::readyPhoto(s.userB)})));

            // Refused before anything is written.
            CHECK(mm::is(erase(s.userA, id), SO::Forbidden));
            CHECK(mm::is(erase(other.userA, id), SO::NotFound));
            CHECK(mm::is(erase(s.userB, c::newUuid()), SO::NotFound));
            CHECK_EQ(cycles(id), 0);
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));

            const auto key = c::newUuid();
            const auto deleted = erase(s.userB, id, key);
            CHECK(mm::is(deleted, SO::Created));
            if (!deleted) return;
            const auto cycle = cycleOf(deleted);
            CHECK(!cycle.empty());
            CHECK_EQ(deleted->deletion.size(), 7u);
            for (const char *field : {"deletion_id", "memory_id", "cycle_no", "deleted_at", "purge_after", "undone_at", "state"}) {
                CHECK(deleted->deletion.isMember(field));
            }
            CHECK_EQ(deleted->deletion["memory_id"].asString(), id);
            CHECK_EQ(deleted->deletion["cycle_no"].asInt(), 1);
            CHECK(deleted->deletion["undone_at"].isNull());
            CHECK_EQ(stateOf(deleted), std::string("deleted"));
            // The whole response, as the controller sends it.
            const auto answer = written(S::responseBody(*deleted));
            CHECK(answer.find(kTitle) == std::string::npos);
            CHECK(answer.find(kDescription) == std::string::npos);
            CHECK(answer.find(photo) == std::string::npos);
            CHECK(answer.find("photos") == std::string::npos);
            CHECK_EQ(S::responseBody(*deleted).size(), 1u);
            CHECK_EQ(audits(id, "memory.deleted"), 1);

            // Same key: this cycle, and no second one. A new key: the creator is told, the partner sees an absent memory.
            const auto replay = erase(s.userB, id, key);
            CHECK(mm::is(replay, SO::Replayed));
            CHECK_EQ(cycleOf(replay), cycle);
            if (replay) CHECK(written(S::responseBody(*replay)).find(kTitle) == std::string::npos);
            CHECK(mm::is(erase(s.userB, id), SO::AlreadyDeleted));
            CHECK(mm::is(erase(s.userA, id), SO::NotFound));
            CHECK(mm::is(erase(other.userA, id), SO::NotFound));
            CHECK_EQ(cycles(id), 1);

            // Undo: nobody else, and not through another memory's id.
            CHECK(mm::is(undo(s.userA, id, cycle), SO::NotFound));
            CHECK(mm::is(undo(other.userA, id, cycle), SO::NotFound));
            CHECK(mm::is(undo(s.userB, spare, cycle), SO::NotFound));
            CHECK(mm::is(undo(s.userB, id, c::newUuid()), SO::NotFound));
            CHECK_EQ(reversals(id), 0);
            const auto undoKey = c::newUuid();
            const auto undone = undo(s.userB, id, cycle, undoKey);
            CHECK(mm::is(undone, SO::Ok));
            CHECK_EQ(stateOf(undone), std::string("undone"));
            if (undone) {
                CHECK(undone->deletion["undone_at"].isString());
                CHECK(written(S::responseBody(*undone)).find(kTitle) == std::string::npos);
            }
            CHECK_EQ(audits(id, "memory.deletion_undone"), 1);
            CHECK(mm::is(undo(s.userB, id, cycle, undoKey), SO::Replayed));
            CHECK(mm::is(undo(s.userB, id, cycle), SO::DeletionNotActive));
            CHECK_EQ(reversals(id), 1);
            const auto back = mm::getMemory(s.userA, id);
            CHECK(mm::is(back, SO::Ok));
            if (back) CHECK_EQ(back->memory["title"].asString(), kTitle);
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-permissions", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: a key is classified before state; a changed target is refused; keys belong to one actor") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto x = mm::idOf(mm::create(s.userB, mm::terms("X", {del::readyPhoto(s.userB)})));
            const auto y = mm::idOf(mm::create(s.userB, mm::terms("Y", {del::readyPhoto(s.userB)})));
            const auto z = mm::idOf(mm::create(s.userB, mm::terms("Z", {del::readyPhoto(s.userB)})));
            const auto partners = mm::idOf(mm::create(s.userA, mm::terms("P", {del::readyPhoto(s.userA)})));
            const auto key = c::newUuid();
            const auto first = cycleOf(erase(s.userB, x, key));
            CHECK(!first.empty());
            // Another target, visible: refused, and it was not deleted.
            CHECK(mm::is(erase(s.userB, y, key), SO::KeyReused));
            CHECK_EQ(cycles(y), 0);
            // Another target, already deleted: the key answers first, not its state.
            const auto yCycle = cycleOf(erase(s.userB, y));
            CHECK(mm::is(erase(s.userB, y, key), SO::KeyReused));
            // The same text in another actor's hands is another key.
            CHECK(mm::is(erase(s.userA, partners, key), SO::Created));

            const auto undoKey = c::newUuid();
            CHECK(mm::is(undo(s.userB, x, first, undoKey), SO::Ok));
            CHECK(mm::is(undo(s.userB, y, yCycle, undoKey), SO::KeyReused));
            CHECK(mm::is(mm::getMemory(s.userB, y), SO::NotFound));
            // The key still answers about what it did, after the memory changed state. It is a fact of that cycle, not of today.
            const auto zCycle = cycleOf(erase(s.userB, z));
            CHECK(mm::is(undo(s.userB, z, zCycle, undoKey), SO::KeyReused));
            CHECK(mm::is(undo(s.userB, x, first, undoKey), SO::Replayed));
            const auto oldDelete = erase(s.userB, x, key);
            CHECK(mm::is(oldDelete, SO::Replayed));
            CHECK_EQ(stateOf(oldDelete), std::string("undone"));
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-keys", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: no read path shows a deleted memory, including history and every replay; an undo brings them back") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto proposed = del::readyPhoto(s.userA);
            const auto createKey = c::newUuid();
            const auto createTerms = mm::terms(kTitle, {photo}, "2026-03-01", kDescription);
            const auto made = mm::create(s.userB, createTerms, createKey);
            const auto id = mm::idOf(made);
            const auto editKey = c::newUuid();
            const auto editTerms = mm::terms(kTitle, {photo}, "2026-03-02", kDescription);
            CHECK(mm::is(mm::edit(s.userB, id, 1, editTerms, editKey), SO::Created));
            const auto proposeKey = c::newUuid();
            const auto proposeTerms = mm::terms(kTitle + " proposed", {photo, proposed});
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 2, proposeTerms, proposeKey));
            CHECK(!proposal.empty());
            const auto decideKey = c::newUuid();
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Reject, decideKey), SO::Ok));
            CHECK(mm::is(mm::create(s.userB, createTerms, createKey), SO::Replayed));
            CHECK(mm::is(mm::getProposal(s.userA, id, proposal), SO::Ok));
            CHECK(memoryListed(s.userA, id));

            const auto cycle = cycleOf(erase(s.userB, id));
            CHECK(!cycle.empty());
            for (const auto &user : {s.userB, s.userA}) {
                const auto got = mm::getMemory(user, id);
                CHECK(mm::is(got, SO::NotFound));
                if (got) CHECK(got->memory.isNull());
                const auto history = mm::getProposal(user, id, proposal);
                // The same answer as a proposal of an absent memory.
                CHECK(mm::is(history, SO::ProposalNotFound));
                CHECK(mm::is(mm::getProposal(user, c::newUuid(), proposal), SO::ProposalNotFound));
                if (history) CHECK(history->proposal.isNull());
                CHECK(!memoryListed(user, id));
            }
            CHECK(mm::is(mm::getMemory(other.userA, id), SO::NotFound));

            // Every same-key replay of what made it answers as if it were absent, and shows nothing.
            for (const auto &replay : {mm::create(s.userB, createTerms, createKey), mm::edit(s.userB, id, 1, editTerms, editKey),
                                       mm::propose(s.userA, id, 2, proposeTerms, proposeKey), mm::decide(s.userB, id, proposal, Decision::Reject, decideKey)}) {
                CHECK(mm::is(replay, SO::NotFound));
                if (replay) {
                    CHECK(replay->memory.isNull());
                    CHECK(replay->mutation.isNull());
                    CHECK(written(S::responseBody(*replay)).find(kTitle) == std::string::npos);
                }
            }
            // A changed payload on the same key is still refused as one.
            auto changed = createTerms;
            changed.title = "Changed";
            CHECK(mm::is(mm::create(s.userB, changed, createKey), SO::KeyReused));

            // After the undo everything is as it was, the replays too.
            CHECK(mm::is(undo(s.userB, id, cycle), SO::Ok));
            CHECK(mm::is(mm::create(s.userB, createTerms, createKey), SO::Replayed));
            CHECK(mm::is(mm::edit(s.userB, id, 1, editTerms, editKey), SO::Replayed));
            CHECK(mm::is(mm::getProposal(s.userB, id, proposal), SO::Ok));
            CHECK(memoryListed(s.userA, id));
            const auto back = mm::getMemory(s.userA, id);
            CHECK(mm::is(back, SO::Ok));
            if (back) {
                CHECK_EQ(back->memory["version"].asInt(), 2);
                CHECK_EQ(back->memory["description"].asString(), kDescription);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-reads", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: edits, proposals and decisions are refused on a deleted memory; deletion rejects a pending request for good") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto reserved = del::readyPhoto(s.userA);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("Pending", {photo})));
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms("Asked", {photo, reserved})));
            CHECK(!proposal.empty());
            CHECK(reservedNow(reserved));

            const auto cycle = cycleOf(erase(s.userB, id));
            CHECK(!cycle.empty());
            // Decided by the creator, in the same commit, and released.
            CHECK_EQ(c::scalar(obs(), mm::kDecisionsSql, proposal), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_edit_decisions WHERE proposal_id = $1::uuid AND outcome = 'rejected' AND decided_by = $2::uuid", proposal, s.userB), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'memory.edit_rejected' AND target_id = $1::text AND detail->>'by_deletion' = 'true'", id), 1);
            CHECK(!reservedNow(reserved));

            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms("Edit", {photo})), SO::NotFound));
            CHECK(mm::is(mm::propose(s.userA, id, 1, mm::terms("Again", {photo})), SO::NotFound));
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Approve), SO::NotFound));
            CHECK(mm::is(mm::decide(s.userA, id, proposal, Decision::Withdraw), SO::NotFound));
            CHECK_EQ(revisions(id), 1);
            CHECK_EQ(c::scalar(obs(), mm::kBindingsSql, reserved), 0);

            // The freed photo is its uploader's to use again, before and regardless of any undo.
            const auto reuse = mm::create(s.userA, mm::terms("Reused", {reserved}));
            CHECK(mm::is(reuse, SO::Created));

            // An undo restores the memory, not the request.
            CHECK(mm::is(undo(s.userB, id, cycle), SO::Ok));
            const auto after = mm::getProposal(s.userB, id, proposal);
            CHECK(mm::is(after, SO::Ok));
            if (after) CHECK_EQ(after->proposal["status"].asString(), std::string("rejected"));
            CHECK_EQ(c::scalar(obs(), mm::kDecisionsSql, proposal), 1);
            CHECK(!reservedNow(reserved));
            const auto current = mm::getMemory(s.userB, id);
            if (current) {
                CHECK_EQ(current->memory["version"].asInt(), 1);
                CHECK_EQ(current->memory["title"].asString(), std::string("Pending"));
            }
            // The memory works again, and a new request can be made.
            CHECK(mm::is(mm::propose(s.userA, id, 1, mm::terms("New ask", {photo})), SO::Created));
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-pending", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: a deletion and a competing write serialize on the memory lock, in both orders") {
        try {
            md::useStore();
            const auto s = md::space();
            // The holder lets both queue; the lock queue is first come, first served.
            const auto raced = [&](const std::string &id, const std::function<std::shared_ptr<mm::PendingS>()> &first,
                                   const std::function<std::shared_ptr<mm::PendingS>()> &second) {
                auto hold = c::acquireTx(holderConn->client);
                CHECK(c::exec(hold, mm::kLockMemory, id).ok);
                auto one = first();
                waitMemoryLockWaiters(1);
                auto two = second();
                waitMemoryLockWaiters(2);
                CHECK(c::commitTx(hold).value_or(false));
                const auto a = one->wait();
                const auto b = two->wait();
                return std::make_pair(a, b);
            };
            const auto fresh = [&](const std::string &title) {
                const auto photo = del::readyPhoto(s.userB);
                return std::make_pair(mm::idOf(mm::create(s.userB, mm::terms(title, {photo}))), photo);
            };

            {   // The deletion first: the edit finds it gone.
                const auto [id, photo] = fresh("delete then edit");
                auto [deleted, edited] = raced(id, [&, id = id]() { return startErase(s.userB, id); },
                                               [&, id = id, photo = photo]() { return mm::startEdit(s.userB, id, 1, mm::terms("late", {photo})); });
                CHECK(mm::is(deleted, SO::Created));
                CHECK(mm::is(edited, SO::NotFound));
                CHECK_EQ(revisions(id), 1);
            }
            {   // The edit first: it counts, and the deletion follows it.
                const auto [id, photo] = fresh("edit then delete");
                auto [edited, deleted] = raced(id, [&, id = id, photo = photo]() { return mm::startEdit(s.userB, id, 1, mm::terms("in time", {photo})); },
                                               [&, id = id]() { return startErase(s.userB, id); });
                CHECK(mm::is(edited, SO::Created));
                CHECK(mm::is(deleted, SO::Created));
                CHECK_EQ(revisions(id), 2);
            }
            {   // The deletion first: the proposal finds it gone and reserves nothing.
                const auto [id, photo] = fresh("delete then propose");
                const auto theirs = del::readyPhoto(s.userA);
                auto [deleted, proposed] = raced(id, [&, id = id]() { return startErase(s.userB, id); },
                                                 [&, id = id, photo = photo]() { return mm::startPropose(s.userA, id, 1, mm::terms("late", {photo, theirs})); });
                CHECK(mm::is(deleted, SO::Created));
                CHECK(mm::is(proposed, SO::NotFound));
                CHECK(!reservedNow(theirs));
            }
            {   // The proposal first: the deletion rejects it and releases what it reserved.
                const auto [id, photo] = fresh("propose then delete");
                const auto theirs = del::readyPhoto(s.userA);
                auto [proposed, deleted] = raced(id, [&, id = id, photo = photo]() { return mm::startPropose(s.userA, id, 1, mm::terms("in time", {photo, theirs})); },
                                                 [&, id = id]() { return startErase(s.userB, id); });
                CHECK(mm::is(proposed, SO::Created));
                CHECK(mm::is(deleted, SO::Created));
                CHECK(!reservedNow(theirs));
                CHECK_EQ(c::scalar(obs(), mm::kDecisionsSql, mm::proposalOf(proposed)), 1);
            }
            {   // The deletion first: it rejected the request the approval came for.
                const auto [id, photo] = fresh("delete then approve");
                const auto theirs = del::readyPhoto(s.userA);
                const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms("asked", {photo, theirs})));
                auto [deleted, approved] = raced(id, [&, id = id]() { return startErase(s.userB, id); },
                                                 [&, id = id]() { return mm::startDecide(s.userB, id, proposal, Decision::Approve); });
                CHECK(mm::is(deleted, SO::Created));
                CHECK(mm::is(approved, SO::NotFound));
                CHECK_EQ(revisions(id), 1);
                CHECK_EQ(c::scalar(obs(), mm::kBindingsSql, theirs), 0);
            }
            {   // The approval first: it stands, then the deletion finds nothing pending.
                const auto [id, photo] = fresh("approve then delete");
                const auto theirs = del::readyPhoto(s.userA);
                const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms("asked", {photo, theirs})));
                auto [approved, deleted] = raced(id, [&, id = id]() { return mm::startDecide(s.userB, id, proposal, Decision::Approve); },
                                                 [&, id = id]() { return startErase(s.userB, id); });
                CHECK(mm::is(approved, SO::Ok));
                CHECK(mm::is(deleted, SO::Created));
                CHECK_EQ(revisions(id), 2);
                CHECK_EQ(c::scalar(obs(), mm::kBindingsSql, theirs), 1);
            }
            {   // Two deletions with different keys: one cycle.
                const auto [id, photo] = fresh("delete twice");
                (void)photo;
                auto [one, two] = raced(id, [&, id = id]() { return startErase(s.userB, id); }, [&, id = id]() { return startErase(s.userB, id); });
                CHECK(mm::is(one, SO::Created));
                CHECK(mm::is(two, SO::AlreadyDeleted));
                CHECK_EQ(cycles(id), 1);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: undo closes at the deadline, the guard holds it when the precheck is passed, and nothing is purged after it") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto open = agedMemory(s);
            const auto openCycle = cycleAged(open, s.couple, s.userB, "167 hours 59 minutes");
            CHECK(lists(s.userB, openCycle));
            CHECK(mm::is(undo(s.userB, open, openCycle), SO::Ok));
            CHECK(mm::is(mm::getMemory(s.userB, open), SO::Ok));

            // The fault alone refuses nothing: only a passed deadline does.
            const auto forced = agedMemory(s);
            const auto forcedCycle = cycleAged(forced, s.couple, s.userB, "167 hours 59 minutes");
            CHECK(mm::is(undo(s.userB, forced, forcedCycle, c::newUuid(), S::Fault::IgnoreUndoPrecheck), SO::Ok));

            const auto closed = agedMemory(s);
            const auto closedKey = c::newUuid();
            const auto closedCycle = cycleAged(closed, s.couple, s.userB, "168 hours 1 second", closedKey);
            CHECK(!lists(s.userB, closedCycle));
            CHECK(mm::is(undo(s.userB, closed, closedCycle), SO::UndoClosed));
            // The race: the service believed it open, and the guard read the clock again.
            CHECK(mm::is(undo(s.userB, closed, closedCycle, c::newUuid(), S::Fault::IgnoreUndoPrecheck), SO::UndoClosed));
            CHECK_EQ(reversals(closed), 0);
            CHECK(mm::is(mm::getMemory(s.userB, closed), SO::NotFound));
            CHECK(mm::is(erase(s.userB, closed), SO::AlreadyDeleted));
            const auto closedReplay = erase(s.userB, closed, closedKey);
            CHECK(mm::is(closedReplay, SO::Replayed));
            CHECK_EQ(stateOf(closedReplay), std::string("deleted"));

            // A cycle from before 0038 retains: past the deadline a cleanup round plans nothing, and the row and its text stay.
            const auto photos = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", closed);
            CHECK_EQ(photos.size(), 1u);
            const std::string photo = photos.size() == 1u ? photos[0]["id"].as<std::string>() : std::string();
            int rounds = 0;
            md::tickUntil([&]() { return ++rounds >= 2; });
            CHECK_EQ(c::scalar(obs(), mm::kPlansSql, photo), 0);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_cleanup_passes WHERE media_id = $1::uuid AND scope = 'everything'", photo), 0);
            CHECK_EQ(del::deletions(photo), 0);
            CHECK_EQ(del::bindings(photo), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revisions r JOIN memory_texts t ON t.id = r.text_id WHERE r.memory_id = $1::uuid AND t.title = 'aged'", closed), 1);

            // On the real clock, across the deadline: one undone in time, one not.
            const auto late = agedMemory(s);
            const auto lateCycle = cycleAged(late, s.couple, s.userB, "167 hours 59 minutes 58 seconds");
            const auto ontime = agedMemory(s);
            const auto ontimeCycle = cycleAged(ontime, s.couple, s.userB, "167 hours 59 minutes 58 seconds");
            const auto ontimeKey = c::newUuid();
            CHECK(mm::is(undo(s.userB, ontime, ontimeCycle, ontimeKey), SO::Ok));
            md::waitUntil([&]() {
                return obs().client->execSqlSync("SELECT clock_timestamp() >= purge_after AS v FROM memory_deletions WHERE id = $1::uuid", lateCycle)[0]["v"].as<bool>();
            });
            CHECK(mm::is(undo(s.userB, late, lateCycle), SO::UndoClosed));
            // Its own answer stays after the deadline, and a new key is told the cycle is over rather than the window.
            CHECK(mm::is(undo(s.userB, ontime, ontimeCycle, ontimeKey), SO::Replayed));
            CHECK(mm::is(undo(s.userB, ontime, ontimeCycle), SO::DeletionNotActive));
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-deadline", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: repeated cycles are separate; a stale undo restores nothing; a replay says nothing about visibility today") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("Cycles", {del::readyPhoto(s.userB)})));
            const auto firstKey = c::newUuid();
            const auto first = erase(s.userB, id, firstKey);
            const auto firstCycle = cycleOf(first);
            const auto firstUndoKey = c::newUuid();
            CHECK(mm::is(undo(s.userB, id, firstCycle, firstUndoKey), SO::Ok));

            const auto secondKey = c::newUuid();
            const auto second = erase(s.userB, id, secondKey);
            CHECK(mm::is(second, SO::Created));
            const auto secondCycle = cycleOf(second);
            CHECK(firstCycle != secondCycle);
            if (second) CHECK_EQ(second->deletion["cycle_no"].asInt(), 2);
            if (first && second) CHECK(second->deletion["purge_after"].asString() != first->deletion["purge_after"].asString());

            // The old cycle cannot undo the new one, under any key.
            CHECK(mm::is(undo(s.userB, id, firstCycle), SO::DeletionNotActive));
            CHECK(mm::is(mm::getMemory(s.userB, id), SO::NotFound));
            // Its replays answer "undone" while the memory is hidden: they are about that cycle.
            const auto undoReplay = undo(s.userB, id, firstCycle, firstUndoKey);
            CHECK(mm::is(undoReplay, SO::Replayed));
            CHECK_EQ(stateOf(undoReplay), std::string("undone"));
            if (undoReplay) CHECK(!S::responseBody(*undoReplay).isMember("memory"));
            const auto deleteReplay = erase(s.userB, id, firstKey);
            CHECK(mm::is(deleteReplay, SO::Replayed));
            CHECK_EQ(stateOf(deleteReplay), std::string("undone"));
            CHECK(mm::is(mm::getMemory(s.userB, id), SO::NotFound));
            CHECK_EQ(reversals(id), 1);

            // Only the new cycle is listed, and it undoes.
            CHECK(!lists(s.userB, firstCycle));
            CHECK(lists(s.userB, secondCycle));
            CHECK(mm::is(undo(s.userB, id, secondCycle), SO::Ok));
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
            CHECK_EQ(stateOf(erase(s.userB, id, secondKey)), std::string("undone"));
            CHECK_EQ(cycles(id), 2);
            CHECK_EQ(reversals(id), 2);
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-cycles", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: the recovery list is the creator's, paged, with only the title and no photo") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            std::vector<std::string> ids, deletions;
            std::vector<std::string> photos;
            for (const char *title : {"One", "Two", "Three"}) {
                const auto photo = del::readyPhoto(s.userB);
                photos.push_back(photo);
                ids.push_back(mm::idOf(mm::create(s.userB, mm::terms(title, {photo}, "2026-03-01", kDescription))));
            }
            // The title it shows is the current one.
            CHECK(mm::is(mm::edit(s.userB, ids[0], 1, mm::terms("One renamed", {photos[0]}, "2026-03-01", kDescription)), SO::Created));
            for (const auto &id : ids) deletions.push_back(cycleOf(erase(s.userB, id)));

            const auto first = recoveryPage(s.userB, 2);
            CHECK(first && first->outcome == SO::Ok && first->memories.size() == 2u && first->nextCursor.has_value());
            if (!first || first->memories.size() != 2u || !first->nextCursor) return;
            CHECK_EQ(first->memories[0]["deletion_id"].asString(), deletions[2]);
            CHECK_EQ(first->memories[1]["deletion_id"].asString(), deletions[1]);
            CHECK_EQ(first->memories[0].size(), 5u);
            CHECK_EQ(first->memories[0]["memory_id"].asString(), ids[2]);
            CHECK_EQ(first->memories[0]["title"].asString(), std::string("Three"));
            CHECK(first->memories[0]["deleted_at"].isString());
            CHECK(first->memories[0]["purge_after"].isString());
            const auto rest = recoveryPage(s.userB, 2, first->nextCursor);
            CHECK(rest && rest->memories.size() == 1u && !rest->nextCursor);
            if (rest && rest->memories.size() == 1u) {
                CHECK_EQ(rest->memories[0]["deletion_id"].asString(), deletions[0]);
                CHECK_EQ(rest->memories[0]["title"].asString(), std::string("One renamed"));
                const auto raw = written(rest->memories[0]);
                CHECK(raw.find(kDescription) == std::string::npos);
                CHECK(raw.find(photos[0]) == std::string::npos);
            }
            // Nobody else's.
            CHECK(recoveryIds(s.userA).empty());
            CHECK(recoveryIds(other.userA).empty());
            // An undone cycle leaves it.
            CHECK(mm::is(undo(s.userB, ids[1], deletions[1]), SO::Ok));
            const auto left = recoveryIds(s.userB);
            CHECK_EQ(left.size(), 2u);
            CHECK(!lists(s.userB, deletions[1]));
            CHECK(mm::getMemory(s.userB, ids[1]).has_value());
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-recovery", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a, as M1c refines it: a partner's photo stays with its uploader; its creator loses it; the creator's goes with the memory; text stays; an undo restores both") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto mine = del::readyPhoto(s.userB);
            const auto theirs = del::readyPhoto(s.userA);
            const auto textsBefore = c::scalar(obs(), mm::kTextsInCoupleSql, s.couple);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {mine}, "2026-03-01", kDescription)));
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kTitle, {mine, theirs}, "2026-03-01", kDescription)));
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Approve), SO::Ok));
            // Before: each reads the other's, through the memory.
            CHECK(md::is(md::content(s.userB, theirs), MO::Ok));
            CHECK(md::is(md::content(s.userA, mine), MO::Ok));
            const auto textsWith = c::scalar(obs(), mm::kTextsInCoupleSql, s.couple);
            CHECK(textsWith > textsBefore);

            const auto cycle = cycleOf(erase(s.userB, id));
            CHECK(!cycle.empty());
            // The creator loses the partner's photo with no signing; the partner loses the creator's. Each keeps their own.
            const int signings = store->readSignings.load();
            CHECK(md::is(md::content(s.userB, theirs), MO::NotFound));
            CHECK(md::is(md::facts(s.userB, theirs), MO::NotFound));
            CHECK(md::is(md::content(s.userA, mine), MO::NotFound));
            CHECK(md::is(md::facts(s.userA, mine), MO::NotFound));
            CHECK_EQ(store->readSignings.load(), signings);
            const auto keptTheirs = md::content(s.userA, theirs);
            CHECK(md::is(keptTheirs, MO::Ok));
            CHECK(md::is(md::facts(s.userA, theirs), MO::Ok));
            // M1c: the creator's own photo is deleted with the memory, for the creator too.
            CHECK(md::is(md::content(s.userB, mine), MO::NotFound));
            // The bytes are there, nothing is planned, and no text was touched or added.
            if (keptTheirs) CHECK_EQ(store->followRead(keptTheirs->redirectUrl), md::image());
            CHECK_EQ(del::deletions(mine), 1);
            CHECK_EQ(del::deletions(theirs), 0);
            for (const auto &photo : {mine, theirs}) {
                // Whole-object plans only: a READY photo's staging copy has its own, unrelated plan.
                CHECK_EQ(c::scalar(obs(), mm::kPlansSql, photo), 0);
                CHECK_EQ(del::bindings(photo), 1);
            }
            CHECK_EQ(c::scalar(obs(), mm::kTextsInCoupleSql, s.couple), textsWith);
            CHECK_EQ(obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM memory_revisions r JOIN memory_texts t ON t.id = r.text_id WHERE r.memory_id = $1::uuid AND t.title = $2 AND t.description = $3", id, kTitle, kDescription)[0]["n"].as<long long>(), 2);
            // A new binding to it is refused, for its uploader too.
            const auto loose = del::readyPhoto(s.userB);
            CHECK(del::refused([&]() {
                obs().client->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)",
                                          loose, s.couple, id, s.userB);
            }));
            CHECK_EQ(del::bindings(loose), 0);

            CHECK(mm::is(undo(s.userB, id, cycle), SO::Ok));
            CHECK(md::is(md::content(s.userB, theirs), MO::Ok));
            CHECK(md::is(md::content(s.userA, mine), MO::Ok));
            // Inside the window the creator's bytes were never touched: what the M1a form of this case read directly while deleted.
            const auto restoredMine = md::content(s.userB, mine);
            CHECK(md::is(restoredMine, MO::Ok));
            if (restoredMine) CHECK_EQ(store->followRead(restoredMine->redirectUrl), md::image());
            const auto back = mm::getMemory(s.userA, id);
            CHECK(mm::is(back, SO::Ok));
            if (back) CHECK_EQ(mm::photosOf(back->memory).size(), 2u);
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-shared-media", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: the media recovery list withholds a deleted parent, and names it again after the undo") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto mine = del::readyPhoto(s.userB);
            const auto theirs = del::readyPhoto(s.userA);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {mine}, "2026-03-01", kDescription)));
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(kTitle, {mine, theirs}, "2026-03-01", kDescription)));
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Approve), SO::Ok));
            const auto photoCycle = del::cycleOf(del::erase(s.userA, theirs));
            CHECK(!photoCycle.empty());
            const auto contextOf = [&]() {
                const auto page = del::undoablePage(s.userA, 100);
                if (!page || page->outcome != MO::Ok) throw std::runtime_error("page");
                for (const auto &item : page->items) if (item["deletion_id"].asString() == photoCycle) return item;
                throw std::runtime_error("row");
            };
            const auto named = contextOf()["context"];
            CHECK_EQ(named["kind"].asString(), std::string("memory"));
            CHECK_EQ(named["target_id"].asString(), id);
            CHECK_EQ(named["label"].asString(), kTitle);

            const auto cycle = cycleOf(erase(s.userB, id));
            CHECK(!cycle.empty());
            const auto row = contextOf();
            const auto hidden = row["context"];
            CHECK_EQ(hidden["kind"].asString(), std::string("unavailable"));
            CHECK(hidden["relation"].isNull() && hidden["target_id"].isNull() && hidden["label"].isNull() && hidden["position"].isNull());
            const auto raw = written(row);
            CHECK(raw.find(id) == std::string::npos);
            CHECK(raw.find(kTitle) == std::string::npos);
            // Its own photo stays the uploader's to undo.
            CHECK(row["media_id"].asString() == theirs);

            CHECK(mm::is(undo(s.userB, id, cycle), SO::Ok));
            const auto again = contextOf()["context"];
            CHECK_EQ(again["kind"].asString(), std::string("memory"));
            CHECK_EQ(again["target_id"].asString(), id);
            CHECK_EQ(again["label"].asString(), kTitle);
            CHECK(md::is(del::undo(s.userA, theirs, photoCycle), MO::Ok));
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-media-context", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1a: a plan cannot link a deleted memory; a plan-create replay shows nothing; the plan's own text never changes") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            // A memory made from a plan, then deleted.
            const auto plan = finishedPlan(s.userB);
            const auto planBefore = planRead(plan);
            const auto eventsBefore = c::scalar(obs(), "SELECT count(*)::bigint AS n FROM shared_plan_events WHERE plan_id = $1::uuid", plan);
            CHECK(planBefore.find("PLAN-OWN-TEXT") != std::string::npos);
            const auto key = c::newUuid();
            const auto terms = mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kDescription);
            const auto made = forPlan(s.userB, plan, key, terms);
            CHECK(mm::is(made, SO::Created));
            const auto id = mm::idOf(made);
            CHECK(mm::is(forPlan(s.userB, plan, key, terms), SO::Replayed));
            const auto linkedPlan = planRead(plan);

            const auto cycle = cycleOf(erase(s.userB, id));
            CHECK(!cycle.empty());
            const auto replay = forPlan(s.userB, plan, key, terms);
            CHECK(mm::is(replay, SO::NotFound));
            if (replay) {
                CHECK(replay->memory.isNull());
                CHECK(written(S::responseBody(*replay)).find(kTitle) == std::string::npos);
            }
            // The link stays, the plan reads byte for byte the same, and no plan event was written.
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM shared_plan_memory_links WHERE plan_id = $1::uuid AND memory_id = $2::uuid", plan, id), 1);
            CHECK_EQ(planRead(plan), linkedPlan);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM shared_plan_events WHERE plan_id = $1::uuid", plan), eventsBefore);

            // A new link to a deleted memory looks like a link to an absent one, and writes nothing.
            const auto second = finishedPlan(s.userA);
            const auto attachKey = c::newUuid();
            const auto target = mm::idOf(mm::create(s.userB, mm::terms("Attach target", {del::readyPhoto(s.userB)})));
            const auto targetCycle = cycleOf(erase(s.userB, target));
            CHECK_EQ(attach(s.userA, second, attachKey, target)["error"].asString(), std::string("not_found"));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM shared_plan_memory_links WHERE plan_id = $1::uuid", second), 0);
            // The direct route is refused by the guard.
            CHECK(del::refused([&]() {
                obs().client->execSqlSync(
                    "INSERT INTO shared_plan_memory_links (plan_id, couple_id, memory_id, actor_id, mode, request_key) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'existing', $5)",
                    second, s.couple, target, s.userA, c::newUuid());
            }));
            // The outsider learns nothing different.
            const auto outsiderPlan = finishedPlan(other.userA);
            CHECK_EQ(attach(other.userA, outsiderPlan, c::newUuid(), target)["error"].asString(), std::string("not_found"));

            // After the undo the link can be made, and its later replay carries only what the caller sent, even once deleted again.
            CHECK(mm::is(undo(s.userB, target, targetCycle), SO::Ok));
            const auto linked = attach(s.userA, second, attachKey, target);
            CHECK_EQ(linked["memory_id"].asString(), target);
            CHECK(mm::is(erase(s.userB, target), SO::Created));
            const auto attachReplay = attach(s.userA, second, attachKey, target);
            CHECK_EQ(attachReplay["memory_id"].asString(), target);
            CHECK(attachReplay["replayed"].asBool());
            CHECK_EQ(attachReplay.size(), 3u);

            // The first memory's undo needs nothing else: the replay shows it again.
            CHECK(mm::is(undo(s.userB, id, cycle), SO::Ok));
            CHECK(mm::is(forPlan(s.userB, plan, key, terms), SO::Replayed));
        } catch (const std::exception &) {
            c::expectThat(false, "m1a-plans", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::memorydeletion
