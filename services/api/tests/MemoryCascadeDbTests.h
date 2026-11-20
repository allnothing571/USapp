#pragma once

// D6 M1c: the creator-photo cascade and the retained list, through the real MemoryService, MediaService and cleanup job against FakeObjectStore, on
// literal usapp_test. Runs inside the memory deletion target, after that suite has prepared its environment. A state no code path can reach is
// placed with triggers off by a maintenance connection, and says so.
#include <json/json.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MemoryDeleteRequestDbTests.h"

namespace usapptest::memorycascade {

namespace base = usapptest::memorydeletion;
namespace req = usapptest::memorydeleterequest;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace del = usapptest::mediadeletion;
namespace al = usapptest::album;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using M = usapp::MediaService;
using MO = M::Outcome;
using Decision = S::Decision;

inline std::optional<c::Observer> planConn;

inline const c::Observer &obs() { return base::obs(); }
inline long long linksOf(const std::string &memoryCycle) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletion_photo_cycles WHERE memory_deletion_id = $1::uuid", memoryCycle); }
inline long long linksOfMemory(const std::string &memory) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletion_photo_cycles WHERE memory_id = $1::uuid", memory); }
inline std::string linkedCycle(const std::string &memoryCycle, const std::string &photo) {
    const auto rows = obs().client->execSqlSync("SELECT media_deletion_id::text AS id FROM memory_deletion_photo_cycles WHERE memory_deletion_id = $1::uuid AND media_id = $2::uuid", memoryCycle, photo);
    return rows.size() == 1u ? rows[0]["id"].as<std::string>() : std::string();
}
inline bool deletedNow(const std::string &photo) { return obs().client->execSqlSync("SELECT usapp_media_deleted_now($1::uuid) AS v", photo)[0]["v"].as<bool>(); }
inline long long wholePlans(const std::string &photo) { return c::scalar(obs(), mm::kPlansSql, photo); }
inline long long linkedRowsInCouple(const std::string &couple) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletion_photo_cycles WHERE couple_id = $1::uuid", couple); }
inline std::optional<M::DeletionPage> retainedPage(const std::string &user, int limit, const std::optional<std::string> &cursor = std::nullopt) {
    std::optional<std::pair<std::string, std::string>> after;
    if (cursor) {
        const auto decoded = usapp::AlbumService::decodeCursor(*cursor);
        if (!decoded) throw std::runtime_error("cursor");
        after = std::make_pair(decoded->createdAt, decoded->id);
    }
    auto p = std::make_shared<ts::Pending<M::DeletionPage>>();
    M::instance().listRetained(user, limit, after, [p](M::DeletionPage page) { p->deliver(std::move(page)); });
    return p->wait();
}
inline std::vector<std::string> retainedIds(const std::string &user) {
    const auto page = retainedPage(user, 100);
    if (!page || page->outcome != MO::Ok) throw std::runtime_error("page");
    std::vector<std::string> ids;
    for (const auto &item : page->items) ids.push_back(item["media_id"].asString());
    return ids;
}
inline bool has(const std::vector<std::string> &ids, const std::string &id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
inline std::optional<S::Status> eraseWithFault(const std::string &user, const std::string &id, const std::string &key, S::Fault fault) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().deleteMemory(user, id, key, [p](S::Status s) { p->deliver(std::move(s)); }, fault);
    return p->wait();
}
// A memory by userB with its creator's photos, and the partner's added by an approved request.
struct Fixture {
    std::string memory, theirs;
    std::vector<std::string> mine;
};
inline Fixture memoryWithPartnerPhoto(const ts::CommittedSpace &s, int own = 2) {
    Fixture f;
    for (int i = 0; i < own; ++i) f.mine.push_back(del::readyPhoto(s.userB));
    f.theirs = del::readyPhoto(s.userA);
    f.memory = mm::idOf(mm::create(s.userB, mm::terms("Cascade", f.mine)));
    auto all = f.mine;
    all.push_back(f.theirs);
    const auto proposal = mm::proposalOf(mm::propose(s.userA, f.memory, 1, mm::terms("Cascade", all)));
    if (proposal.empty() || !mm::is(mm::decide(s.userB, f.memory, proposal, Decision::Approve), SO::Ok)) throw std::runtime_error("fixture");
    return f;
}
// A memory cycle with its real cascade, deleted `ago` before now, in one committed transaction. Only a committed row can have a chosen age.
inline std::string cascadeAged(const std::string &id, const std::string &couple, const std::string &user, const std::string &ago) {
    const auto cycle = c::newUuid();
    obs().client->execSqlSync(
        "DO $aged$ BEGIN "
        "INSERT INTO memory_deletions (id, memory_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
        "SELECT '" + cycle + "', '" + id + "', '" + couple + "', '" + user + "', 1 + COALESCE((SELECT max(x.cycle_no) FROM memory_deletions x WHERE x.memory_id = '" + id + "'), 0), '" + c::newUuid() +
        "', repeat('a', 64), n.t, n.t + interval '168 hours' FROM (SELECT clock_timestamp() - interval '" + ago + "' AS t) n; "
        "PERFORM usapp_memory_cascade_photos('" + cycle + "'); END $aged$");
    return cycle;
}
// Bound to that memory, as an approval would have done. An old fixture has no proposal to approve.
inline void bindPartnerPhoto(const std::string &photo, const std::string &couple, const std::string &memory, const std::string &creator) {
    base::asMaintenance("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ('" + photo + "', '" + couple + "', 'memory', '" + memory + "', '" + creator + "');");
}
inline std::shared_ptr<ts::Pending<bool>> startPurgePlan(const std::string &photo, const std::string &cycle) {
    auto planning = std::make_shared<ts::Pending<bool>>();
    planConn->client->execSqlAsync(
        "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before, deletion_id) VALUES ($1::uuid, 'everything', 'user_deleted', clock_timestamp(), $2::uuid)",
        [planning](const drogon::orm::Result &) { planning->deliver(true); },
        [planning](const drogon::orm::DrogonDbException &) { planning->deliver(false); }, photo, cycle);
    return planning;
}
inline void waitBlockedSessions(long long n) {
    md::waitUntil([&]() { return c::scalar(obs(), mm::kBlockedSql, "%") >= n; });
}

inline void runMemoryCascadeDbTests(const std::string &dsn) {
    planConn = c::Observer::connect(dsn);

    TEST_CASE("D6 M1c: a direct deletion links the creator's photos, history included, with its own time; not the partner's, nor one already deleted") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto current = del::readyPhoto(s.userB), history = del::readyPhoto(s.userB), kept = del::readyPhoto(s.userB), independent = del::readyPhoto(s.userB);
            const auto theirs = del::readyPhoto(s.userA);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("History", {current, history})));
            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms("History", {current, kept})), SO::Created));
            CHECK(mm::is(mm::edit(s.userB, id, 2, mm::terms("History", {current, kept, independent})), SO::Created));
            const auto independentCycle = del::cycleOf(del::erase(s.userB, independent));
            CHECK(!independentCycle.empty());
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 3, mm::terms("History", {current, kept, theirs})));
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Approve), SO::Ok));
            CHECK(md::is(md::content(s.userB, theirs), MO::Ok));

            const auto key = c::newUuid();
            const auto deleted = base::erase(s.userB, id, key);
            CHECK(mm::is(deleted, SO::Created));
            const auto cycle = base::cycleOf(deleted);
            CHECK_EQ(linksOf(cycle), 3);
            for (const auto &photo : {current, history, kept}) {
                CHECK(!linkedCycle(cycle, photo).empty());
                CHECK(deletedNow(photo));
                CHECK(md::is(md::content(s.userB, photo), MO::NotFound));
                CHECK(md::is(md::content(s.userA, photo), MO::NotFound));
                CHECK_EQ(wholePlans(photo), 0);
            }
            // One time and one deadline, to the microsecond.
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_deletion_photo_cycles l JOIN memory_deletions x ON x.id = l.memory_deletion_id "
                                      "JOIN media_deletions d ON d.id = l.media_deletion_id WHERE l.memory_deletion_id = $1::uuid AND d.deleted_at = x.deleted_at AND d.purge_after = x.purge_after AND d.deleted_by = x.deleted_by", cycle), 3);
            // Not the partner's, whose uploader keeps it; the creator loses it, with no signing.
            CHECK_EQ(linkedCycle(cycle, theirs), std::string());
            CHECK(!deletedNow(theirs));
            const int signings = store->readSignings.load();
            CHECK(md::is(md::content(s.userB, theirs), MO::NotFound));
            CHECK_EQ(store->readSignings.load(), signings);
            const auto keptTheirs = md::content(s.userA, theirs);
            CHECK(md::is(keptTheirs, MO::Ok));
            if (keptTheirs) CHECK_EQ(store->followRead(keptTheirs->redirectUrl), md::image());
            // Not the photo that was already deleted on its own: one cycle, and it is not linked.
            CHECK_EQ(linkedCycle(cycle, independent), std::string());
            CHECK_EQ(del::deletions(independent), 1);

            // Linked cycles are not the standalone list's, and do not undo by themselves. The independent one stays in both.
            const auto listed = del::undoableIds(s.userB);
            CHECK_EQ(listed.size(), 1u);
            CHECK(has(listed, independentCycle));
            for (const auto &photo : {current, history, kept}) {
                CHECK(md::is(del::undo(s.userB, photo, linkedCycle(cycle, photo)), MO::DeletionLinked));
                CHECK(md::is(del::erase(s.userB, photo), MO::AlreadyDeleted));
            }
            CHECK(md::is(del::undo(s.userA, current, linkedCycle(cycle, current)), MO::NotFound));
            CHECK_EQ(del::reversals(current), 0);

            // Same key: this cycle, with nothing new.
            const auto replay = base::erase(s.userB, id, key);
            CHECK(mm::is(replay, SO::Replayed));
            CHECK_EQ(base::cycleOf(replay), cycle);
            CHECK_EQ(linksOf(cycle), 3);
            CHECK_EQ(del::deletions(current), 1);

            // The undo brings back exactly those three.
            const auto undoKey = c::newUuid();
            CHECK(mm::is(base::undo(s.userB, id, cycle, undoKey), SO::Ok));
            for (const auto &photo : {current, history, kept}) {
                CHECK(!deletedNow(photo));
                CHECK_EQ(del::reversals(photo), 1);
                CHECK(md::is(md::content(s.userB, photo), MO::Ok));
            }
            CHECK(md::is(md::content(s.userA, current), MO::Ok));
            CHECK(md::is(md::content(s.userB, theirs), MO::Ok));
            CHECK(deletedNow(independent));
            CHECK_EQ(del::reversals(independent), 0);
            CHECK(has(del::undoableIds(s.userB), independentCycle));
            const auto back = mm::getMemory(s.userA, id);
            CHECK(mm::is(back, SO::Ok));
            CHECK(mm::is(base::undo(s.userB, id, cycle, undoKey), SO::Replayed));
            CHECK_EQ(del::reversals(current), 1);
            CHECK(md::is(del::undo(s.userB, current, linkedCycle(cycle, current)), MO::DeletionNotActive));
            CHECK(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text AND action = $2::text AND detail->>'photos' = '3'", id, std::string("memory.deleted")) == 1);
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-direct", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: an approved request and a settled direct deletion cascade the same way; a failure in either leaves no cycle of any kind") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto f = memoryWithPartnerPhoto(s);
            const auto requestId = req::idOf(req::request(s.userA, f.memory));
            const auto key = c::newUuid();
            CHECK(mm::is(req::decide(s.userB, f.memory, requestId, Decision::Approve, key, S::Fault::FailDeleteApproval), SO::Error));
            CHECK_EQ(linksOfMemory(f.memory), 0);
            CHECK_EQ(base::cycles(f.memory), 0);
            CHECK_EQ(req::decisions(f.memory), 0);
            for (const auto &photo : f.mine) {
                CHECK_EQ(del::deletions(photo), 0);
                CHECK(md::is(md::content(s.userB, photo), MO::Ok));
            }
            const auto approved = req::decide(s.userB, f.memory, requestId, Decision::Approve, key);
            CHECK(mm::is(approved, SO::Ok));
            const auto cycle = base::cycleOf(approved);
            CHECK_EQ(linksOf(cycle), 2);
            for (const auto &photo : f.mine) CHECK(deletedNow(photo));
            CHECK(!deletedNow(f.theirs));
            CHECK(has(retainedIds(s.userA), f.theirs));
            CHECK(retainedIds(s.userB).empty());
            // The replay writes nothing more; the undo restores them all; the decision stays.
            CHECK(mm::is(req::decide(s.userB, f.memory, requestId, Decision::Approve, key), SO::Replayed));
            CHECK_EQ(linksOf(cycle), 2);
            CHECK(mm::is(base::undo(s.userB, f.memory, cycle), SO::Ok));
            for (const auto &photo : f.mine) CHECK(!deletedNow(photo));
            CHECK_EQ(req::statusOf(req::read(s.userA, f.memory, requestId)), std::string("approved"));

            // A direct deletion that settles a pending request.
            const auto pending = req::idOf(req::request(s.userA, f.memory));
            const auto deleteKey = c::newUuid();
            CHECK(mm::is(eraseWithFault(s.userB, f.memory, deleteKey, S::Fault::FailAfterCascade), SO::Error));
            CHECK_EQ(base::cycles(f.memory), 1);
            CHECK_EQ(linksOfMemory(f.memory), 2);
            CHECK_EQ(req::statusOf(req::read(s.userA, f.memory, pending)), std::string("pending"));
            for (const auto &photo : f.mine) {
                CHECK(!deletedNow(photo));
                CHECK_EQ(del::deletions(photo), 1);
            }
            const auto settled = base::erase(s.userB, f.memory, deleteKey);
            CHECK(mm::is(settled, SO::Created));
            CHECK_EQ(linksOf(base::cycleOf(settled)), 2);
            for (const auto &photo : f.mine) CHECK_EQ(del::deletions(photo), 2);
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-paths", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: undo is all or nothing, including when one linked photo has become irreversible") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto f = memoryWithPartnerPhoto(s, 3);
            const auto cycle = base::cycleOf(base::erase(s.userB, f.memory));
            CHECK_EQ(linksOf(cycle), 3);
            // No code path plans a purge before its deadline, and the deadline is shared, so this one row is placed with triggers off.
            const auto irreversible = f.mine[1];
            base::asMaintenance("INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before, deletion_id) VALUES ('" + irreversible + "', 'everything', 'user_deleted', clock_timestamp(), '" + linkedCycle(cycle, irreversible) + "');");
            // The verification script accepts an early plan only if its exact ids were declared here, and refuses any other.
            std::printf("EARLY_PLAN_FIXTURE media_id=%s deletion_id=%s case=%s\n", irreversible.c_str(), linkedCycle(cycle, irreversible).c_str(),
                        "D6 M1c: undo is all or nothing, including when one linked photo has become irreversible");
            std::fflush(stdout);
            const auto refusedNow = [&](S::Fault fault) {
                CHECK(mm::is(base::undo(s.userB, f.memory, cycle, c::newUuid(), fault), SO::UndoClosed));
                CHECK_EQ(base::reversals(f.memory), 0);
                CHECK(mm::is(mm::getMemory(s.userB, f.memory), SO::NotFound));
                for (const auto &photo : f.mine) {
                    CHECK_EQ(del::reversals(photo), 0);
                    CHECK(deletedNow(photo));
                }
            };
            // The service's own read, and then the guard's, as if the plan committed just after that read.
            refusedNow(S::Fault::None);
            refusedNow(S::Fault::IgnoreUndoPrecheck);
            CHECK(base::lists(s.userB, cycle));
            CHECK(!retainedIds(s.userA).empty());

            // Direct SQL cannot do half of it either, at commit.
            auto half = c::acquireTx(md::env.pool);
            CHECK(c::exec(half, "INSERT INTO memory_deletion_reversals (deletion_id, memory_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
                                "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, repeat('a', 64))", cycle, f.memory, s.couple, s.userB, c::newUuid()).ok);
            const auto immediate = c::exec(half, usapptest::wallet::expectFailureSql("SET CONSTRAINTS ALL IMMEDIATE"));
            CHECK(immediate.completed && !immediate.ok);
            CHECK_EQ(usapptest::wallet::parseFailure(immediate.error).sqlState, std::string("UE007"));
            c::rollbackTx(half);
            // And a linked reversal alone is refused outright.
            CHECK(del::refused([&]() {
                obs().client->execSqlSync("INSERT INTO media_deletion_reversals (deletion_id, media_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
                                          "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, repeat('a', 64))", linkedCycle(cycle, f.mine[0]), f.mine[0], s.couple, s.userB, c::newUuid());
            }));
            CHECK_EQ(del::reversals(f.mine[0]), 0);
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-all-or-nothing", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: memory undo and a purge plan serialize on the photo lock, in both orders") {
        try {
            md::useStore();
            const auto s = md::space();
            {   // The undo reaches the lock first, inside the window: it restores the photo, and the plan finds it reversed.
                const auto photo = del::readyPhoto(s.userB);
                const auto id = mm::idOf(mm::create(s.userB, mm::terms("Undo first", {photo})));
                const auto cycle = base::cycleOf(base::erase(s.userB, id));
                const auto mediaCycle = linkedCycle(cycle, photo);
                auto hold = c::acquireTx(base::holderConn->client);
                CHECK(c::exec(hold, mm::kLockMedia, photo).ok);
                auto undoing = std::make_shared<mm::PendingS>();
                S::instance().undoDeletion(s.userB, id, cycle, c::newUuid(), [undoing](S::Status status) { undoing->deliver(std::move(status)); });
                waitBlockedSessions(1);
                auto planning = startPurgePlan(photo, mediaCycle);
                waitBlockedSessions(2);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(mm::is(undoing->wait(), SO::Ok));
                const auto planned = planning->wait();
                CHECK(planned.has_value() && !*planned);
                CHECK_EQ(wholePlans(photo), 0);
                CHECK(!deletedNow(photo));
                CHECK(mm::is(mm::getMemory(s.userB, id), SO::Ok));
            }
            {   // The plan reaches the lock first, past the deadline: the undo, told to pass its own reads, is refused entire.
                const auto id = base::agedMemory(s);
                const auto photo = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", id)[0]["id"].as<std::string>();
                const auto cycle = cascadeAged(id, s.couple, s.userB, "168 hours 1 second");
                const auto mediaCycle = linkedCycle(cycle, photo);
                CHECK(!mediaCycle.empty());
                auto hold = c::acquireTx(base::holderConn->client);
                CHECK(c::exec(hold, mm::kLockMedia, photo).ok);
                auto planning = startPurgePlan(photo, mediaCycle);
                waitBlockedSessions(1);
                auto undoing = std::make_shared<mm::PendingS>();
                S::instance().undoDeletion(s.userB, id, cycle, c::newUuid(), [undoing](S::Status status) { undoing->deliver(std::move(status)); }, S::Fault::IgnoreUndoPrecheck);
                waitBlockedSessions(2);
                CHECK(c::commitTx(hold).value_or(false));
                const auto planned = planning->wait();
                CHECK(planned.has_value() && *planned);
                CHECK(mm::is(undoing->wait(), SO::UndoClosed));
                CHECK_EQ(base::reversals(id), 0);
                CHECK_EQ(del::reversals(photo), 0);
                CHECK(mm::is(mm::getMemory(s.userB, id), SO::NotFound));
                CHECK(deletedNow(photo));
            }
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-lock-orders", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: after expiry the media job purges the linked photo by its own rules; the partner's stays with its uploader; undo is closed") {
        try {
            auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 0; });
            const auto s = md::space();
            const auto id = base::agedMemory(s);
            const auto photo = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", id)[0]["id"].as<std::string>();
            const auto stagingOf = md::stagingOf(photo);
            store->lateClientWrite(stagingOf, md::image());
            store->lateClientWrite(md::attemptKey(photo, 1), md::image());
            const auto partners = del::agedWithObjects(s, s.userA);
            bindPartnerPhoto(partners, s.couple, id, s.userB);

            // Inside the window the job plans nothing.
            const auto open = base::agedMemory(s);
            const auto openPhoto = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", open)[0]["id"].as<std::string>();
            const auto openCycle = cascadeAged(open, s.couple, s.userB, "167 hours 59 minutes");

            const auto cycle = cascadeAged(id, s.couple, s.userB, "168 hours 1 second");
            const auto mediaCycle = linkedCycle(cycle, photo);
            CHECK_EQ(linksOf(cycle), 1);
            md::tickUntil([&]() { return del::sweeps(photo) == 1; });
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM media_cleanup_plans WHERE deletion_id = $1::uuid AND reason = $2::text", mediaCycle, std::string("user_deleted")), 1);
            CHECK(!store->exists(md::attemptKey(photo, 1)));
            CHECK_EQ(wholePlans(openPhoto), 0);
            CHECK(!deletedNow(partners));
            CHECK_EQ(wholePlans(partners), 0);
            CHECK(store->exists(md::attemptKey(partners, 1)));
            const auto keptRead = md::content(s.userA, partners);
            CHECK(md::is(keptRead, MO::Ok));
            CHECK(has(retainedIds(s.userA), partners));

            // Closed, whole, for good. The memory stays deleted, and the replay of that cycle says so.
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::UndoClosed));
            CHECK(mm::is(base::undo(s.userB, id, cycle, c::newUuid(), S::Fault::IgnoreUndoPrecheck), SO::UndoClosed));
            CHECK_EQ(base::reversals(id), 0);
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::NotFound));
            CHECK(md::is(del::undo(s.userB, photo, mediaCycle), MO::DeletionLinked));
            CHECK(md::is(md::content(s.userB, photo), MO::NotFound));

            // The text stays until M1d.
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_revisions r JOIN memory_texts t ON t.id = r.text_id WHERE r.memory_id = $1::uuid AND t.title = 'aged'", id), 1);
            // The open one still undoes, whole.
            CHECK(mm::is(base::undo(s.userB, open, openCycle), SO::Ok));
            CHECK(!deletedNow(openPhoto));
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-purge", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: on the real clock, a linked cycle closes exactly with its memory: undone in time, or not at all") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photoOf = [](const std::string &memory) {
                return obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", memory)[0]["id"].as<std::string>();
            };
            const auto late = base::agedMemory(s);
            const auto latePhoto = photoOf(late);
            const auto lateCycle = cascadeAged(late, s.couple, s.userB, "167 hours 59 minutes 58 seconds");
            const auto onTime = base::agedMemory(s);
            const auto onTimePhoto = photoOf(onTime);
            const auto onTimeCycle = cascadeAged(onTime, s.couple, s.userB, "167 hours 59 minutes 58 seconds");
            const auto key = c::newUuid();
            CHECK(base::lists(s.userB, lateCycle));
            CHECK(mm::is(base::undo(s.userB, onTime, onTimeCycle, key), SO::Ok));
            CHECK(!deletedNow(onTimePhoto));
            md::waitUntil([&]() {
                return obs().client->execSqlSync("SELECT clock_timestamp() >= purge_after AS v FROM memory_deletions WHERE id = $1::uuid", lateCycle)[0]["v"].as<bool>();
            });
            CHECK(!base::lists(s.userB, lateCycle));
            CHECK(mm::is(base::undo(s.userB, late, lateCycle), SO::UndoClosed));
            CHECK(mm::is(base::undo(s.userB, late, lateCycle, c::newUuid(), S::Fault::IgnoreUndoPrecheck), SO::UndoClosed));
            CHECK(mm::is(base::erase(s.userB, late), SO::AlreadyDeleted));
            CHECK_EQ(base::reversals(late), 0);
            CHECK_EQ(del::reversals(latePhoto), 0);
            CHECK(deletedNow(latePhoto));
            // The one undone in time keeps its answer after the deadline, and a new key is told the cycle is over, not the window.
            CHECK(mm::is(base::undo(s.userB, onTime, onTimeCycle, key), SO::Replayed));
            CHECK(mm::is(base::undo(s.userB, onTime, onTimeCycle), SO::DeletionNotActive));
            CHECK(!deletedNow(onTimePhoto));
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-real-clock", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: a cycle from before the upgrade is not cascaded or backdated: nothing is planned, its creator keeps the photos and can act on them") {
        try {
            auto store = md::useStore([](M::Policy &p) { p.sweepGraceSeconds = 0; });
            const auto s = md::space();
            const auto expired = base::agedMemory(s);
            const auto expiredPhoto = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", expired)[0]["id"].as<std::string>();
            const auto expiredCycle = base::cycleAged(expired, s.couple, s.userB, "200 hours");
            const auto open = base::agedMemory(s);
            const auto openPhoto = obs().client->execSqlSync("SELECT media_id::text AS id FROM media_bindings WHERE target_id = $1::uuid", open)[0]["id"].as<std::string>();
            const auto openCycle = base::cycleAged(open, s.couple, s.userB, "10 hours");

            int rounds = 0;
            md::tickUntil([&]() { return ++rounds >= 2; });
            for (const auto &photo : {expiredPhoto, openPhoto}) {
                CHECK_EQ(del::deletions(photo), 0);
                CHECK_EQ(wholePlans(photo), 0);
                CHECK(!deletedNow(photo));
                CHECK(md::is(md::facts(s.userB, photo), MO::Ok));
                // The partner still has no way in: the memory is deleted.
                CHECK(md::is(md::facts(s.userA, photo), MO::NotFound));
            }
            CHECK_EQ(linksOf(expiredCycle), 0);
            CHECK_EQ(linksOf(openCycle), 0);
            const auto listed = retainedIds(s.userB);
            CHECK(has(listed, expiredPhoto) && has(listed, openPhoto));
            CHECK(retainedIds(s.userA).empty());

            // The creator may delete one by hand; that is an independent cycle, and it does not come back with the memory.
            const auto byHand = del::cycleOf(del::erase(s.userB, openPhoto));
            CHECK(!byHand.empty());
            CHECK(has(del::undoableIds(s.userB), byHand));
            CHECK(!has(retainedIds(s.userB), openPhoto));

            // The expired one's undo stays closed, by the same 0036 rule; the open one's is the 0036 undo, with nothing linked.
            CHECK(mm::is(base::undo(s.userB, expired, expiredCycle), SO::UndoClosed));
            CHECK(mm::is(base::undo(s.userB, open, openCycle), SO::Ok));
            CHECK(deletedNow(openPhoto));
            CHECK_EQ(del::reversals(openPhoto), 0);

            // A new cycle of that memory is a new-style one.
            CHECK(md::is(del::undo(s.userB, openPhoto, byHand), MO::Ok));
            const auto next = base::cycleOf(base::erase(s.userB, open));
            CHECK_EQ(linksOf(next), 1);
            CHECK(deletedNow(openPhoto));
            CHECK(mm::is(base::undo(s.userB, open, next), SO::Ok));
            (void)store;
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-upgrade", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: repeated cycles each link their own photo cycles; independent deletions keep their own lives; an old cycle affects nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto f = memoryWithPartnerPhoto(s);
            const auto firstKey = c::newUuid();
            const auto first = base::cycleOf(base::erase(s.userB, f.memory, firstKey));
            const auto firstMedia = linkedCycle(first, f.mine[0]);
            CHECK(mm::is(base::undo(s.userB, f.memory, first), SO::Ok));

            // Deleted on its own between cycles: the next cycle leaves it out, and its undo leaves it deleted.
            const auto aside = del::cycleOf(del::erase(s.userB, f.mine[1]));
            CHECK(!aside.empty());
            const auto second = base::cycleOf(base::erase(s.userB, f.memory));
            CHECK(!second.empty() && second != first);
            CHECK_EQ(linksOf(second), 1);
            CHECK_EQ(linkedCycle(second, f.mine[1]), std::string());
            const auto secondMedia = linkedCycle(second, f.mine[0]);
            CHECK(secondMedia != firstMedia);
            CHECK_EQ(c::scalar(obs(), "SELECT cycle_no::bigint AS n FROM media_deletions WHERE id = $1::uuid", secondMedia), 2);

            // The old cycle touches nothing, by either route.
            CHECK(mm::is(base::undo(s.userB, f.memory, first), SO::DeletionNotActive));
            CHECK(md::is(del::undo(s.userB, f.mine[0], firstMedia), MO::DeletionNotActive));
            CHECK(md::is(del::undo(s.userB, f.mine[0], secondMedia), MO::DeletionLinked));
            CHECK(deletedNow(f.mine[0]));
            const auto oldReplay = base::erase(s.userB, f.memory, firstKey);
            CHECK(mm::is(oldReplay, SO::Replayed));
            CHECK_EQ(base::stateOf(oldReplay), std::string("undone"));
            CHECK(mm::is(mm::getMemory(s.userB, f.memory), SO::NotFound));

            // Undone on its own during the deletion: it becomes the uploader's retained photo, and the memory's undo does not touch it.
            CHECK(md::is(del::undo(s.userB, f.mine[1], aside), MO::Ok));
            CHECK(has(retainedIds(s.userB), f.mine[1]));
            CHECK(mm::is(base::undo(s.userB, f.memory, second), SO::Ok));
            CHECK(!deletedNow(f.mine[0]));
            CHECK(!deletedNow(f.mine[1]));
            CHECK_EQ(del::reversals(f.mine[0]), 2);
            CHECK_EQ(del::reversals(f.mine[1]), 2);
            CHECK(retainedIds(s.userB).empty());

            // The partner deletes their own at any time; it is not the memory's to restore.
            const auto third = base::cycleOf(base::erase(s.userB, f.memory));
            const auto partnerCycle = del::cycleOf(del::erase(s.userA, f.theirs));
            CHECK(!partnerCycle.empty());
            CHECK(retainedIds(s.userA).empty());
            CHECK(mm::is(base::undo(s.userB, f.memory, third), SO::Ok));
            CHECK(deletedNow(f.theirs));
            CHECK(has(del::undoableIds(s.userA), partnerCycle));
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-cycles", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1c: the retained list is the uploader's alone, paged, with nothing of the parent") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            std::vector<std::string> mine;
            const std::string title = "M1C-SENTINEL-TITLE";
            std::vector<std::string> theirs;
            for (int i = 0; i < 3; ++i) {
                theirs.push_back(del::readyPhoto(s.userA));
                md::sleepMs(2);
            }
            const auto own = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(title, {own})));
            auto all = theirs;
            all.push_back(own);
            CHECK(mm::is(mm::decide(s.userB, id, mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms(title, all))), Decision::Approve), SO::Ok));
            // An unrelated photo and one in a live memory are not listed.
            const auto loose = del::readyPhoto(s.userA);
            const auto liveOwn = del::readyPhoto(s.userA);
            CHECK(mm::is(mm::create(s.userA, mm::terms("Live", {liveOwn})), SO::Created));
            CHECK(retainedIds(s.userA).empty());

            const auto cycle = base::cycleOf(base::erase(s.userB, id));
            const auto first = retainedPage(s.userA, 2);
            CHECK(first && first->outcome == MO::Ok && first->items.size() == 2u && first->nextCursor.has_value());
            if (!first || first->items.size() != 2u || !first->nextCursor) return;
            CHECK_EQ(first->items[0]["media_id"].asString(), theirs[2]);
            CHECK_EQ(first->items[1]["media_id"].asString(), theirs[1]);
            CHECK_EQ(first->items[0].size(), 4u);
            for (const char *field : {"media_id", "uploaded_at", "width", "height"}) CHECK(first->items[0].isMember(field));
            const auto raw = base::written(first->items[0]);
            CHECK(raw.find(title) == std::string::npos);
            CHECK(raw.find(id) == std::string::npos);
            const auto rest = retainedPage(s.userA, 2, first->nextCursor);
            CHECK(rest && rest->items.size() == 1u && !rest->nextCursor);
            if (rest && rest->items.size() == 1u) CHECK_EQ(rest->items[0]["media_id"].asString(), theirs[0]);
            const auto ids = retainedIds(s.userA);
            CHECK(!has(ids, loose) && !has(ids, liveOwn));

            // Only its uploader: the creator's deleted one is not retained, the partner's is not the creator's, and another space sees nothing.
            CHECK(retainedIds(s.userB).empty());
            CHECK(retainedIds(other.userA).empty());
            CHECK(retainedIds(other.userB).empty());
            CHECK(md::is(md::facts(other.userA, theirs[0]), MO::NotFound));
            auto outsider = std::make_shared<ts::Pending<M::DeletionPage>>();
            M::instance().listRetained(c::newUuid(), 10, std::nullopt, [outsider](M::DeletionPage page) { outsider->deliver(std::move(page)); });
            const auto none = outsider->wait();
            CHECK(none && none->outcome == MO::SpaceNotFound);

            // Its uploader may act on what is listed. Neither the binding nor the list gives anyone else a way in.
            CHECK(md::is(md::content(s.userA, theirs[0]), MO::Ok));
            CHECK(md::is(md::content(s.userB, theirs[0]), MO::NotFound));
            CHECK(md::is(del::erase(s.userB, theirs[0]), MO::NotFound));
            CHECK_EQ(del::bindings(theirs[0]), 1);
            const auto gone = del::cycleOf(del::erase(s.userA, theirs[0]));
            CHECK(!gone.empty());
            CHECK(!has(retainedIds(s.userA), theirs[0]));
            CHECK(md::is(del::undo(s.userA, theirs[0], gone), MO::Ok));
            CHECK(has(retainedIds(s.userA), theirs[0]));

            // An undo of the memory empties it.
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::Ok));
            CHECK(retainedIds(s.userA).empty());
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1c-retained", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::memorycascade
