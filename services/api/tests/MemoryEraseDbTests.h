#pragma once

// D6 M1d: text erasure through the real usapp_memory_erase and memory.purge job, on literal usapp_test, with FakeObjectStore for the photos. Runs inside the
// memory deletion target, after that suite has prepared its environment. The boundary case comes first. Only a maintenance connection can place an
// expired cycle on a memory with real history: base::cycleAged. The sentinel text is what the verification scans server logs for.
#include <json/json.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "IntentDiagnosticsDbTests.h"
#include "scheduler/jobs/MemoryPurgeJob.h"

namespace usapptest::memoryerase {

namespace base = usapptest::memorydeletion;
namespace req = usapptest::memorydeleterequest;
namespace casc = usapptest::memorycascade;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace del = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using M = usapp::MediaService;
using MO = M::Outcome;
using Decision = S::Decision;

inline std::shared_ptr<usapp::scheduler::MemoryPurgeJob> job;

inline const c::Observer &obs() { return base::obs(); }
inline std::string one(const std::string &sql, const std::string &a) { return obs().client->execSqlSync(sql, a)[0][static_cast<drogon::orm::Row::SizeType>(0)].as<std::string>(); }
inline std::string eraseNow(const std::string &cycle) { return one("SELECT usapp_memory_erase($1::uuid)", cycle); }
// What a refused statement said, for its fixed label. Empty: it was not refused.
inline std::string refusalOf(const std::function<void()> &action) {
    try {
        action();
        return std::string();
    } catch (const std::exception &failure) {
        return failure.what();
    }
}
inline bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
inline long long purges(const std::string &memory) { return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_purges WHERE memory_id = $1::uuid", memory); }
// Every text this memory uses, with its id, and everything else that must not change.
inline std::string textsOf(const std::string &memory) {
    return one("SELECT COALESCE(string_agg(x.id || ':' || x.title || ':' || x.description, '|' ORDER BY x.id), '') FROM memory_texts x WHERE x.id IN ("
               "SELECT r.text_id FROM memory_revisions r WHERE r.memory_id = $1::uuid UNION SELECT q.text_id FROM memory_edit_proposals q WHERE q.memory_id = $1::uuid)", memory);
}
inline std::string structureOf(const std::string &memory) {
    return one("SELECT concat_ws('#', "
               "(SELECT string_agg(r.version || ':' || r.actor_id || ':' || r.action || ':' || r.memory_date || ':' || r.text_id || ':' || COALESCE(r.idempotency_key, '') || ':' || r.created_at, '|' ORDER BY r.version) FROM memory_revisions r WHERE r.memory_id = $1::uuid), "
               "(SELECT string_agg(q.id || ':' || q.proposer_id || ':' || q.memory_date || ':' || q.text_id || ':' || q.idempotency_key, '|' ORDER BY q.id) FROM memory_edit_proposals q WHERE q.memory_id = $1::uuid), "
               "(SELECT string_agg(d.proposal_id || ':' || d.outcome || ':' || d.request_fingerprint, '|' ORDER BY d.proposal_id) FROM memory_edit_decisions d JOIN memory_edit_proposals q ON q.id = d.proposal_id WHERE q.memory_id = $1::uuid), "
               "(SELECT string_agg(ph.version || ':' || ph.position || ':' || ph.media_id, '|' ORDER BY ph.version, ph.position) FROM memory_revision_photos ph WHERE ph.memory_id = $1::uuid), "
               "(SELECT string_agg(b.media_id::text, '|' ORDER BY b.media_id) FROM media_bindings b WHERE b.target_id = $1::uuid), "
               "(SELECT count(*)::text FROM audit_logs a WHERE a.target_id = $1::text))", memory);
}
inline long long unerasedTexts(const std::string &memory) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_texts x WHERE (x.title <> '[erased]' OR x.description <> '') AND x.id IN ("
                            "SELECT r.text_id FROM memory_revisions r WHERE r.memory_id = $1::uuid UNION SELECT q.text_id FROM memory_edit_proposals q WHERE q.memory_id = $1::uuid)", memory);
}
inline long long unerasedFingerprints(const std::string &memory) {
    return c::scalar(obs(), "SELECT (SELECT count(*) FROM memory_revisions r WHERE r.memory_id = $1::uuid AND r.request_fingerprint IS NOT NULL AND r.request_fingerprint <> repeat('0', 64)) + "
                            "(SELECT count(*) FROM memory_edit_proposals q WHERE q.memory_id = $1::uuid AND q.request_fingerprint <> repeat('0', 64)) AS n", memory);
}
inline std::shared_ptr<ts::Pending<bool>> startErase(const std::string &cycle) {
    auto erasing = std::make_shared<ts::Pending<bool>>();
    casc::planConn->client->execSqlAsync(
        "SELECT usapp_memory_erase($1::uuid)", [erasing](const drogon::orm::Result &) { erasing->deliver(true); },
        [erasing](const drogon::orm::DrogonDbException &) { erasing->deliver(false); }, cycle);
    return erasing;
}

inline void runMemoryEraseDbTests() {
    job = std::make_shared<usapp::scheduler::MemoryPurgeJob>(false);
    usapp::scheduler::Scheduler::instance().registerJob(job);
    const std::string kTitle = "M1D-SENTINEL-TITLE";
    const std::string kWords = "M1D-SENTINEL-WORDS";
    const std::string kEdited = "M1D-SENTINEL-EDITED";
    const std::string kAsked = "M1D-SENTINEL-ASKED";
    const auto clean = [&](const std::string &raw) {
        return !has(raw, "M1D-SENTINEL") && !has(raw, "[erased]") && !has(raw, std::string(64, '0'));
    };

    TEST_CASE("D6 M1d: the boundary holds first: the application cannot write the protected columns, and only the function can") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto before = textsOf(id);
            const auto textId = one("SELECT text_id::text FROM memory_revisions WHERE memory_id = $1::uuid", id);
            // The application role, through the service's own pool.
            for (const auto &sql : {"UPDATE memory_texts SET title = 'x' WHERE id = '" + textId + "'",
                                    "UPDATE memory_texts SET description = '' WHERE id = '" + textId + "'",
                                    "UPDATE memory_revisions SET request_fingerprint = repeat('0', 64) WHERE memory_id = '" + id + "'",
                                    "UPDATE memory_edit_proposals SET request_fingerprint = repeat('0', 64) WHERE memory_id = '" + id + "'",
                                    "DELETE FROM memory_texts WHERE id = '" + textId + "'",
                                    "INSERT INTO memory_purges (deletion_id, memory_id, couple_id) VALUES (gen_random_uuid(), '" + id + "', '" + s.couple + "')",
                                    std::string("SET ROLE usapp_memory_eraser")}) {
                const auto refused = refusalOf([&]() { md::env.pool->execSqlSync(sql); });
                CHECK(has(refused, "permission denied"));
            }
            CHECK_EQ(textsOf(id), before);
            // It may execute the function, whose own rules then answer.
            const auto cycle = base::cycleOf(base::erase(s.userB, id));
            const auto window = refusalOf([&]() { md::env.pool->execSqlSync("SELECT usapp_memory_erase($1::uuid)", cycle); });
            CHECK(has(window, "USAPP_MEMORY_ERASE_WINDOW"));
            // A superuser meets the triggers: they are what enforces this, not only the grants.
            const auto super = refusalOf([&]() { base::maintClient()->execSqlSync("UPDATE memory_texts SET title = 'x' WHERE id = '" + textId + "'"); });
            CHECK(has(super, "USAPP_MEMORY_APPEND_ONLY"));
            const auto superRecord = refusalOf([&]() {
                base::maintClient()->execSqlSync("INSERT INTO memory_purges (deletion_id, memory_id, couple_id) VALUES ('" + cycle + "', '" + id + "', '" + s.couple + "')");
            });
            CHECK(has(superRecord, "USAPP_MEMORY_ERASE_WINDOW"));
            CHECK_EQ(textsOf(id), before);
            CHECK_EQ(purges(id), 0);
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1d-boundary", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: the function checks the exact cycle: absent, open, reversed, and a later cycle's own window") {
        try {
            md::useStore();
            const auto s = md::space();
            CHECK(has(refusalOf([&]() { eraseNow(c::newUuid()); }), "USAPP_MEMORY_ERASE_TARGET"));
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto before = textsOf(id);
            const auto first = base::cycleOf(base::erase(s.userB, id));
            CHECK(has(refusalOf([&]() { eraseNow(first); }), "USAPP_MEMORY_ERASE_WINDOW"));
            CHECK(mm::is(base::undo(s.userB, id, first), SO::Ok));
            CHECK(has(refusalOf([&]() { eraseNow(first); }), "USAPP_MEMORY_ERASE_REVERSED"));
            const auto second = base::cycleOf(base::erase(s.userB, id));
            CHECK(has(refusalOf([&]() { eraseNow(first); }), "USAPP_MEMORY_ERASE_REVERSED"));
            CHECK(has(refusalOf([&]() { eraseNow(second); }), "USAPP_MEMORY_ERASE_WINDOW"));
            CHECK_EQ(textsOf(id), before);
            CHECK_EQ(purges(id), 0);
            CHECK(mm::is(base::undo(s.userB, id, second), SO::Ok));
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1d-cycle", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: off by default the job does nothing; on, every text and fingerprint goes, the structure stays, every read and replay stays hidden") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto theirs = del::readyPhoto(s.userA);
            const auto createKey = c::newUuid();
            const auto createTerms = mm::terms(kTitle, {photo}, "2026-03-01", kWords);
            const auto id = mm::idOf(mm::create(s.userB, createTerms, createKey));
            const auto editKey = c::newUuid();
            const auto editTerms = mm::terms(kEdited, {photo}, "2026-03-02", kWords);
            CHECK(mm::is(mm::edit(s.userB, id, 1, editTerms, editKey), SO::Created));
            const auto askKey = c::newUuid();
            const auto askTerms = mm::terms(kAsked, {photo}, "2026-03-03", kWords);
            const auto rejected = mm::proposalOf(mm::propose(s.userA, id, 2, askTerms, askKey));
            const auto rejectKey = c::newUuid();
            CHECK(mm::is(mm::decide(s.userB, id, rejected, Decision::Reject, rejectKey), SO::Ok));
            const auto approved = mm::proposalOf(mm::propose(s.userA, id, 2, mm::terms(kAsked + "-2", {photo, theirs}, "2026-03-04", kWords)));
            CHECK(mm::is(mm::decide(s.userB, id, approved, Decision::Approve), SO::Ok));
            const auto requestKey = c::newUuid();
            const auto requestId = req::idOf(req::request(s.userA, id, requestKey));
            CHECK(mm::is(req::decide(s.userB, id, requestId, Decision::Reject), SO::Ok));
            // The photo's own deletion, whose recovery list must say nothing of the erased parent.
            const auto theirCycle = del::cycleOf(del::erase(s.userA, theirs));
            CHECK(md::is(del::undo(s.userA, theirs, theirCycle), MO::Ok));

            // From a plan too, whose own text is not the memory's.
            const auto plan = base::finishedPlan(s.userB);
            const auto planKey = c::newUuid();
            const auto planTerms = mm::terms(kTitle + "-P", {del::readyPhoto(s.userB)}, "2026-03-05", kWords);
            const auto fromPlan = mm::idOf(base::forPlan(s.userB, plan, planKey, planTerms));
            CHECK(!fromPlan.empty());
            const auto planBefore = base::planRead(plan);
            const auto eventsBefore = one("SELECT string_agg(e.id || ':' || e.request::text || ':' || COALESCE(e.body::text, ''), '|' ORDER BY e.id) FROM shared_plan_events e WHERE e.plan_id = $1::uuid", plan);
            CHECK(has(eventsBefore, "PLAN-OWN-TEXT"));

            const auto deleteKey = c::newUuid();
            const auto cycle = base::cycleAged(id, s.couple, s.userB, "169 hours", deleteKey);
            const auto planCycle = base::cycleAged(fromPlan, s.couple, s.userB, "169 hours");
            const auto structure = structureOf(id);
            const auto textsBefore = textsOf(id);
            CHECK(has(textsBefore, kTitle) && has(textsBefore, kEdited) && has(textsBefore, kAsked));
            const auto decisionFingerprint = one("SELECT request_fingerprint FROM memory_edit_decisions WHERE proposal_id = $1::uuid", rejected);

            // Off: a tick leaves it alone, though it has been due for an hour.
            md::tick();
            CHECK_EQ(purges(id), 0);
            CHECK_EQ(textsOf(id), textsBefore);

            job->setEnabled(true);
            md::tickUntil([&]() { return purges(id) == 1 && purges(fromPlan) == 1; });
            job->setEnabled(false);
            CHECK_EQ(unerasedTexts(id), 0);
            CHECK_EQ(unerasedFingerprints(id), 0);
            CHECK_EQ(unerasedTexts(fromPlan), 0);
            CHECK_EQ(unerasedFingerprints(fromPlan), 0);
            // Every id, key, date, link, photo, binding and audit row stands. A decision's fingerprint holds only ids.
            CHECK_EQ(structureOf(id), structure);
            CHECK_EQ(one("SELECT request_fingerprint FROM memory_edit_decisions WHERE proposal_id = $1::uuid", rejected), decisionFingerprint);
            // The plan's own text, byte for byte, and its link.
            CHECK_EQ(base::planRead(plan), planBefore);
            CHECK_EQ(one("SELECT string_agg(e.id || ':' || e.request::text || ':' || COALESCE(e.body::text, ''), '|' ORDER BY e.id) FROM shared_plan_events e WHERE e.plan_id = $1::uuid", plan), eventsBefore);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM shared_plan_memory_links WHERE plan_id = $1::uuid AND memory_id = $2::uuid", plan, fromPlan), 1);

            // Every read: hidden, with nothing of the text, the placeholder or the fingerprint.
            std::string seen;
            const auto look = [&](const std::optional<S::Status> &status) { if (status) seen += base::written(S::responseBody(*status)); };
            for (const auto &user : {s.userB, s.userA}) {
                const auto got = mm::getMemory(user, id);
                CHECK(mm::is(got, SO::NotFound));
                look(got);
                for (const auto &proposal : {rejected, approved}) {
                    const auto history = mm::getProposal(user, id, proposal);
                    CHECK(mm::is(history, SO::ProposalNotFound));
                    look(history);
                }
                CHECK(!base::memoryListed(user, id));
                CHECK(mm::is(req::read(user, id, requestId), SO::ProposalNotFound));
                const auto list = mm::listPage(user, 100);
                if (list) for (const auto &item : list->memories) seen += base::written(item);
            }
            CHECK(mm::is(mm::getMemory(other.userA, id), SO::NotFound));
            CHECK(!base::lists(s.userB, cycle));

            // Every old key: absent, whether or not the request is the same. A changed payload cannot be told from the same one any more.
            auto changed = createTerms;
            changed.title = "Changed";
            const std::vector<std::optional<S::Status>> replays = {
                mm::create(s.userB, createTerms, createKey), mm::create(s.userB, changed, createKey),
                mm::edit(s.userB, id, 1, editTerms, editKey), mm::edit(s.userB, id, 1, changed, editKey),
                mm::propose(s.userA, id, 2, askTerms, askKey), mm::propose(s.userA, id, 2, changed, askKey),
                mm::decide(s.userB, id, rejected, Decision::Reject, rejectKey),
                base::forPlan(s.userB, plan, planKey, planTerms)};
            for (const auto &replay : replays) {
                CHECK(mm::is(replay, SO::NotFound));
                look(replay);
            }
            // New writes: absent as well.
            CHECK(mm::is(mm::edit(s.userB, id, 3, changed), SO::NotFound));
            CHECK(mm::is(mm::propose(s.userA, id, 3, changed), SO::NotFound));
            CHECK(mm::is(req::request(s.userA, id), SO::NotFound));

            // Facts of its own cycle and request stay, and hold no text.
            const auto deleteReplay = base::erase(s.userB, id, deleteKey);
            CHECK(mm::is(deleteReplay, SO::Replayed));
            look(deleteReplay);
            const auto requestReplay = req::request(s.userA, id, requestKey);
            CHECK(mm::is(requestReplay, SO::Replayed));
            look(requestReplay);
            CHECK(mm::is(base::erase(s.userB, id), SO::AlreadyDeleted));

            // Closed, for the service and for the guard.
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::UndoClosed));
            CHECK(mm::is(base::undo(s.userB, id, cycle, c::newUuid(), S::Fault::IgnoreUndoPrecheck), SO::UndoClosed));
            CHECK_EQ(base::reversals(id), 0);
            CHECK(mm::is(base::undo(s.userB, fromPlan, planCycle), SO::UndoClosed));

            // The partner's photo stays theirs, and its lists say nothing of the parent.
            CHECK(casc::has(casc::retainedIds(s.userA), theirs));
            CHECK(md::is(md::content(s.userA, theirs), MO::Ok));
            CHECK(md::is(md::content(s.userB, theirs), MO::NotFound));
            const auto retained = casc::retainedPage(s.userA, 100);
            if (retained) for (const auto &item : retained->items) seen += base::written(item);
            const auto again = del::cycleOf(del::erase(s.userA, theirs));
            const auto recovery = del::undoablePage(s.userA, 100);
            CHECK(recovery.has_value());
            if (recovery) {
                bool named = false;
                for (const auto &item : recovery->items) {
                    seen += base::written(item);
                    if (item["deletion_id"].asString() == again) {
                        named = true;
                        CHECK_EQ(item["context"]["kind"].asString(), std::string("unavailable"));
                        CHECK(item["context"]["label"].isNull());
                    }
                }
                CHECK(named);
            }
            CHECK(!seen.empty());
            CHECK(clean(seen));

            // Repeat: the function and the job change nothing.
            const auto erasedTexts = textsOf(id);
            CHECK_EQ(eraseNow(cycle), std::string("already"));
            job->setEnabled(true);
            md::tick();
            job->setEnabled(false);
            CHECK_EQ(textsOf(id), erasedTexts);
            CHECK_EQ(purges(id), 1);
            CHECK_EQ(structureOf(id), structure);
            (void)store;
        } catch (const std::exception &failure) {
            job->setEnabled(false);
            c::expectThat(false, "m1d-erase", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: erasure is atomic: a rolled back call leaves everything, and a refused one writes nothing") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms(kEdited, {del::readyPhoto(s.userB)}, "2026-03-02", kWords)), SO::Created));
            const auto cycle = base::cycleAged(id, s.couple, s.userB, "169 hours");
            const auto before = textsOf(id);
            auto tx = c::acquireTx(md::env.pool);
            CHECK(c::exec(tx, "SELECT usapp_memory_erase($1::uuid)", cycle).ok);
            c::rollbackTx(tx);
            CHECK_EQ(textsOf(id), before);
            CHECK_EQ(purges(id), 0);
            CHECK(unerasedFingerprints(id) > 0);
            CHECK_EQ(eraseNow(cycle), std::string("erased"));
            CHECK_EQ(unerasedTexts(id), 0);
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1d-atomic", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: a text another unerased memory uses is refused, whole; the job leaves it and still erases the rest") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto a = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto b = mm::idOf(mm::create(s.userB, mm::terms(kEdited, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto bystander = mm::idOf(mm::create(s.userB, mm::terms(kAsked, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            // No code path shares text, so only a maintenance connection can.
            base::asMaintenance("INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint) "
                                "SELECT '" + b + "', '" + s.couple + "', 2, '" + s.userB + "', 'edit', DATE '2026-03-02', r.text_id, '" + c::newUuid() + "', repeat('a', 64) "
                                "FROM memory_revisions r WHERE r.memory_id = '" + a + "';");
            const auto cycleA = base::cycleAged(a, s.couple, s.userB, "169 hours");
            const auto cycleBystander = base::cycleAged(bystander, s.couple, s.userB, "169 hours");
            const auto beforeA = textsOf(a);
            const auto beforeB = textsOf(b);
            // The other memory is not even deleted.
            CHECK(has(refusalOf([&]() { eraseNow(cycleA); }), "USAPP_MEMORY_ERASE_SHARED_TEXT"));
            CHECK_EQ(textsOf(a), beforeA);
            CHECK_EQ(textsOf(b), beforeB);
            CHECK_EQ(purges(a), 0);
            CHECK(unerasedFingerprints(a) > 0);
            job->setEnabled(true);
            md::tickUntil([&]() { return purges(bystander) == 1; });
            job->setEnabled(false);
            CHECK_EQ(purges(a), 0);
            CHECK_EQ(textsOf(a), beforeA);
            CHECK(mm::is(mm::getMemory(s.userB, b), SO::Ok));
            CHECK_EQ(unerasedTexts(bystander), 0);
            (void)cycleBystander;
        } catch (const std::exception &failure) {
            job->setEnabled(false);
            c::expectThat(false, "m1d-shared", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: what the enablement check reports is exactly what the first tick erases, in its order, and blocked text is reported apart") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto a = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto b = mm::idOf(mm::create(s.userB, mm::terms(kEdited, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto open = mm::idOf(mm::create(s.userB, mm::terms(kAsked, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
            const auto cycleA = base::cycleAged(a, s.couple, s.userB, "171 hours");
            const auto cycleB = base::cycleAged(b, s.couple, s.userB, "170 hours");
            const auto openCycle = base::cycleOf(base::erase(s.userB, open));
            const auto dueIds = [&]() {
                std::vector<std::string> ids;
                for (const auto &row : obs().client->execSqlSync("SELECT deletion_id::text AS id FROM usapp_memory_purge_due() WHERE NOT blocked_shared_text ORDER BY purge_after, deletion_id")) ids.push_back(row["id"].as<std::string>());
                return ids;
            };
            const auto beforeTick = dueIds();
            CHECK(std::find(beforeTick.begin(), beforeTick.end(), cycleA) != beforeTick.end());
            CHECK(std::find(beforeTick.begin(), beforeTick.end(), cycleB) != beforeTick.end());
            CHECK(std::find(beforeTick.begin(), beforeTick.end(), openCycle) == beforeTick.end());
            // No cascade on these two, and the report says so. Only what a report may hold.
            CHECK_EQ(c::scalar(obs(), "SELECT linked_photos AS n FROM usapp_memory_purge_due() WHERE deletion_id = $1::uuid", cycleA), 0);
            const auto columns = obs().client->execSqlSync("SELECT * FROM usapp_memory_purge_due() LIMIT 1");
            CHECK_EQ(columns.columns(), 6u);
            // Off, whatever the report says.
            md::tick();
            CHECK(dueIds() == beforeTick);
            // One real tick: exactly the first batch of the report, and not one other.
            job->setEnabled(true);
            md::tick();
            job->setEnabled(false);
            const std::size_t batch = std::min<std::size_t>(beforeTick.size(), usapp::scheduler::MemoryPurgeJob::kBatch);
            for (std::size_t i = 0; i < beforeTick.size(); ++i) {
                const auto purged = c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_purges WHERE deletion_id = $1::uuid", beforeTick[i]);
                CHECK_EQ(purged, i < batch ? 1 : 0);
            }
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM memory_purges WHERE deletion_id = $1::uuid", openCycle), 0);
            CHECK(mm::is(base::undo(s.userB, open, openCycle), SO::Ok));
        } catch (const std::exception &failure) {
            job->setEnabled(false);
            c::expectThat(false, "m1d-enablement", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("D6 M1d: undo and erasure serialize on the memory lock, in both orders") {
        try {
            md::useStore();
            const auto s = md::space();
            {   // The undo first, inside the window: restored, and the erasure refused.
                const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
                const auto cycle = base::cycleOf(base::erase(s.userB, id));
                const auto before = textsOf(id);
                auto hold = c::acquireTx(base::holderConn->client);
                CHECK(c::exec(hold, mm::kLockMemory, id).ok);
                auto undoing = std::make_shared<mm::PendingS>();
                S::instance().undoDeletion(s.userB, id, cycle, c::newUuid(), [undoing](S::Status status) { undoing->deliver(std::move(status)); });
                casc::waitBlockedSessions(1);
                auto erasing = startErase(cycle);
                casc::waitBlockedSessions(2);
                CHECK(c::commitTx(hold).value_or(false));
                CHECK(mm::is(undoing->wait(), SO::Ok));
                const auto erased = erasing->wait();
                CHECK(erased.has_value() && !*erased);
                CHECK_EQ(textsOf(id), before);
                CHECK_EQ(purges(id), 0);
                CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
            }
            {   // The erasure first, past the deadline: the undo, told to pass its own reads, finds it gone.
                const auto id = mm::idOf(mm::create(s.userB, mm::terms(kTitle, {del::readyPhoto(s.userB)}, "2026-03-01", kWords)));
                const auto cycle = base::cycleAged(id, s.couple, s.userB, "169 hours");
                auto hold = c::acquireTx(base::holderConn->client);
                CHECK(c::exec(hold, mm::kLockMemory, id).ok);
                auto erasing = startErase(cycle);
                casc::waitBlockedSessions(1);
                auto undoing = std::make_shared<mm::PendingS>();
                S::instance().undoDeletion(s.userB, id, cycle, c::newUuid(), [undoing](S::Status status) { undoing->deliver(std::move(status)); }, S::Fault::IgnoreUndoPrecheck);
                casc::waitBlockedSessions(2);
                CHECK(c::commitTx(hold).value_or(false));
                const auto erased = erasing->wait();
                CHECK(erased.has_value() && *erased);
                CHECK(mm::is(undoing->wait(), SO::UndoClosed));
                CHECK_EQ(base::reversals(id), 0);
                CHECK_EQ(purges(id), 1);
                CHECK_EQ(unerasedTexts(id), 0);
                CHECK(mm::is(mm::getMemory(s.userB, id), SO::NotFound));
            }
        } catch (const std::exception &failure) {
            c::expectThat(false, "m1d-lock-orders", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::memoryerase
