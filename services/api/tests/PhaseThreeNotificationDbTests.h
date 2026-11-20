#pragma once

// D20 (0043, docs/phase3-plan.md Section 30): opt-in partner notices for a direct memory creation and one album photo batch, through
// the real MemoryService and AlbumService on literal usapp_test with FakeObjectStore. Runs as its own CTest target
// (phase3_notification_db), outside db_gated. Shared-plan creation lives in its controller, so its notice is covered through the real
// API binary in ReminderLiveApiTest.h; the database guards and resolver are covered in database/tests/phase3_notification_test.sql.
#include <json/json.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "MemoryDeletionDbTests.h"
#include "services/NotificationService.h"

namespace usapptest::phase3notification {

namespace al = usapptest::album;
namespace del = usapptest::mediadeletion;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace mdel = usapptest::memorydeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using A = usapp::AlbumService;
using AO = A::Outcome;
using Choice = usapp::intent::Choice;

inline const c::Observer &obs() { return md::obs(); }
inline long long count(const std::string &sql, const std::string &a) { return c::scalar(obs(), sql, a); }

inline Choice omitted() { return Choice{}; }
inline Choice chosen(bool value) { return Choice{true, value}; }

inline std::optional<S::Status> createMemory(const std::string &user, const S::Terms &t, Choice choice, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().create(user, key, t, [p](S::Status s) { p->deliver(std::move(s)); }, choice);
    return p->wait();
}
inline std::optional<A::Status> addPhotos(const std::string &user, const std::string &album, const std::vector<std::string> &ids, Choice choice,
                                          const std::string &key = c::newUuid(), A::Fault fault = A::Fault::None) {
    auto p = std::make_shared<ts::Pending<A::Status>>();
    A::instance().addPhotos(user, album, key, ids, [p](A::Status s) { p->deliver(std::move(s)); }, fault, choice);
    return p->wait();
}
inline std::optional<usapp::NotificationService::Result> inbox(const std::string &user) {
    auto p = std::make_shared<ts::Pending<usapp::NotificationService::Result>>();
    usapp::NotificationService::list(user, 100, std::nullopt, false, [p](usapp::NotificationService::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
// The guard's refusal text, or empty when the statement went through. The deferred intent check (UN105) would only refuse at commit, so a
// USAPP_NOTIFICATION_SOURCE refusal shows the source itself was rejected.
inline std::string refusalOf(const std::function<void()> &statement) {
    try {
        statement();
        return std::string();
    } catch (const std::exception &e) {
        return e.what();
    }
}
inline bool sourceRefused(const std::string &message) { return message.find("USAPP_NOTIFICATION_SOURCE") != std::string::npos; }
inline std::string written(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

// The facts shape every create answer carries: exactly the four OptionalNotificationResult fields.
inline bool facts(const Json::Value &n, bool requested, bool created, const std::string &reason) {
    if (!n.isObject() || n.size() != 4u) return false;
    if (!n["requested"].isBool() || n["requested"].asBool() != requested) return false;
    if (!n["created"].isBool() || n["created"].asBool() != created) return false;
    if (n["reason"].asString() != reason) return false;
    return created ? n["notification_id"].isString() : n["notification_id"].isNull();
}

inline const char *kNoticesInCoupleSql = "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid";
inline const char *kNoticesOfMemorySql = "SELECT count(*)::bigint AS n FROM notifications WHERE memory_id = $1::uuid";
inline const char *kIntentsOfMemorySql = "SELECT count(*)::bigint AS n FROM notification_intents WHERE memory_id = $1::uuid";
inline const char *kNoticesOfAlbumSql =
    "SELECT count(*)::bigint AS n FROM notifications n JOIN album_photo_batches b ON b.id = n.album_batch_id WHERE b.album_id = $1::uuid";
inline const char *kIntentsOfAlbumSql =
    "SELECT count(*)::bigint AS n FROM notification_intents i JOIN album_photo_batches b ON b.id = i.album_batch_id WHERE b.album_id = $1::uuid";
inline const char *kRememberedSql =
    "SELECT count(*)::bigint AS n FROM notification_intents WHERE actor_id = $1::uuid AND preference_seq IS NOT NULL";
inline const char *kBatchesSql = "SELECT count(*)::bigint AS n FROM album_photo_batches WHERE album_id = $1::uuid";

inline void runPhaseThreeNotificationDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);

    TEST_CASE("D20 memory: omitted and false create no notice, true creates exactly one generic notice, nothing is remembered") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto off = createMemory(s.userA, mm::terms("D20-OMITTED-TITLE", {del::readyPhoto(s.userA)}, "2026-03-01", "D20-SECRET-WORDS"), omitted());
            CHECK(mm::is(off, SO::Created));
            if (!mm::is(off, SO::Created)) return;
            CHECK(off->hasNotification);
            CHECK(facts(off->notification, false, false, "not_requested"));
            CHECK_EQ(count(kIntentsOfMemorySql, mm::idOf(off)), 1);
            const auto no = createMemory(s.userA, mm::terms("D20-FALSE", {del::readyPhoto(s.userA)}), chosen(false));
            CHECK(mm::is(no, SO::Created));
            CHECK(no && facts(no->notification, false, false, "not_requested"));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 0);

            const std::string key = c::newUuid();
            const auto photo = del::readyPhoto(s.userA);
            const auto terms = mm::terms("D20-TRUE-TITLE", {photo}, "2026-03-02", "D20-TRUE-WORDS");
            const auto yes = createMemory(s.userA, terms, chosen(true), key);
            CHECK(mm::is(yes, SO::Created));
            if (!mm::is(yes, SO::Created)) return;
            const auto memory = mm::idOf(yes);
            CHECK(facts(yes->notification, true, true, "delivered_to_inbox"));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK_EQ(count(kNoticesOfMemorySql, memory), 1);
            const auto row = obs().client->execSqlSync(
                "SELECT id::text AS id, recipient_id::text AS r, actor_id::text AS a, event_type, target_kind, target_id::text AS t, "
                "task_id IS NULL AND anniversary_id IS NULL AND message_id IS NULL AND message_reply_id IS NULL AND shared_plan_id IS NULL "
                "AND album_batch_id IS NULL AS only_memory FROM notifications WHERE memory_id = $1::uuid", memory);
            CHECK_EQ(row.size(), 1u);
            if (row.size() == 1u) {
                CHECK_EQ(row[0]["id"].as<std::string>(), yes->notification["notification_id"].asString());
                CHECK_EQ(row[0]["r"].as<std::string>(), s.userB);
                CHECK_EQ(row[0]["a"].as<std::string>(), s.userA);
                CHECK_EQ(row[0]["event_type"].as<std::string>(), std::string("memory.created"));
                CHECK_EQ(row[0]["target_kind"].as<std::string>(), std::string("memory"));
                CHECK_EQ(row[0]["t"].as<std::string>(), memory);
                CHECK(row[0]["only_memory"].as<bool>());
            }
            // Explicit choices of the new operations are never remembered.
            CHECK_EQ(count(kRememberedSql, s.userA), 0);

            // Same key and choice: the original facts, no second notice. Omitted and false agree with each other, not with true.
            const auto again = createMemory(s.userA, terms, chosen(true), key);
            CHECK(mm::is(again, SO::Replayed));
            CHECK(again && written(again->notification) == written(yes->notification));
            CHECK(mm::is(createMemory(s.userA, terms, chosen(false), key), SO::KeyReused));
            CHECK(mm::is(createMemory(s.userA, terms, omitted(), key), SO::KeyReused));
            const auto offKey = c::newUuid();
            const auto offTerms = mm::terms("D20-OFF-KEY", {del::readyPhoto(s.userA)});
            CHECK(mm::is(createMemory(s.userA, offTerms, omitted(), offKey), SO::Created));
            const auto offAgain = createMemory(s.userA, offTerms, chosen(false), offKey);
            CHECK(mm::is(offAgain, SO::Replayed));
            CHECK(offAgain && facts(offAgain->notification, false, false, "not_requested"));
            CHECK(mm::is(createMemory(s.userA, offTerms, chosen(true), offKey), SO::KeyReused));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK_EQ(count(kIntentsOfMemorySql, memory), 1);

            // The recipient's inbox names only the event, the kind and the id: no title, description, photo or key.
            const auto listed = inbox(s.userB);
            CHECK(listed && listed->status == 200);
            if (listed && listed->status == 200) {
                const auto text = written(listed->body);
                for (const auto &secret : {std::string("D20-TRUE-TITLE"), std::string("D20-TRUE-WORDS"), photo, std::string("media/"), std::string("staging/")})
                    CHECK(text.find(secret) == std::string::npos);
                CHECK_EQ(listed->body["notifications"].size(), 1u);
                // Exactly the seven list fields every notice has; nothing D20-specific is added.
                const auto names = listed->body["notifications"][0].getMemberNames();
                CHECK((std::set<std::string>(names.begin(), names.end()) ==
                       std::set<std::string>{"id", "event_type", "actor_id", "target_kind", "target_id", "created_at", "read_at"}));
            }
            // The actor's own inbox gets nothing.
            const auto own = inbox(s.userA);
            CHECK(own && own->body["notifications"].size() == 0u);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("D20 memory: no partner is a successful write with no notice; another couple never receives one") {
        try {
            md::useStore();
            ts::CommittedSpace solo;
            ts::commitSpace(pool, solo, false);
            const auto made = createMemory(solo.userA, mm::terms("D20-SOLO", {del::readyPhoto(solo.userA)}), chosen(true));
            CHECK(mm::is(made, SO::Created));
            CHECK(made && facts(made->notification, true, false, "no_partner"));
            CHECK_EQ(count(kNoticesInCoupleSql, solo.couple), 0);
            CHECK_EQ(count(kRememberedSql, solo.userA), 0);

            const auto s = md::space();
            const auto other = md::space();
            CHECK(mm::is(createMemory(s.userA, mm::terms("D20-CROSS", {del::readyPhoto(s.userA)}), chosen(true)), SO::Created));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK_EQ(count(kNoticesInCoupleSql, other.couple), 0);
            for (const auto &stranger : {other.userA, other.userB}) {
                const auto listed = inbox(stranger);
                CHECK(listed && listed->status == 200 && listed->body["notifications"].size() == 0u);
            }
            // A notice cannot be pointed at another couple's memory: the composite key refuses it.
            const auto theirs = mm::idOf(createMemory(other.userA, mm::terms("D20-THEIRS", {del::readyPhoto(other.userA)}), omitted()));
            CHECK(sourceRefused(refusalOf([&] {
                pool->execSqlSync(
                    "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, memory_id) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'memory.created', 'memory', $4::uuid, $4::uuid)",
                    s.couple, s.userA, s.userB, theirs);
            })));
            CHECK_EQ(count(kNoticesOfMemorySql, theirs), 0);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("D20 memory: plan conversion, edits, requests, decisions, deletion and undo create no notice; opening uses current access") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto plan = pool->execSqlSync(
                "SELECT usapp_shared_plan_write($1::uuid, NULL::uuid, 'create', $2, $3::jsonb) #>> '{plan,id}' AS id", s.userA, c::newUuid(),
                std::string(R"({"body":{"title":"D20-PLAN","date":null,"time":null,"place":null,"note":null}})"))[0]["id"].as<std::string>();
            for (const char *from : {"want", "plan"})
                pool->execSqlSync("SELECT usapp_shared_plan_write($1::uuid, $2::uuid, 'advance', $3, $4::jsonb)", s.userA, plan, c::newUuid(),
                                  std::string(R"({"expected_status":")") + from + R"("})");
            const auto converted = mdel::forPlan(s.userB, plan, c::newUuid(), mm::terms("D20-CONVERTED", {md::agedReady(pool, s, s.userB)}));
            CHECK(mm::is(converted, SO::Created));
            CHECK(converted && !converted->hasNotification);
            CHECK_EQ(count(kIntentsOfMemorySql, mm::idOf(converted)), 0);
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 0);
            // The database refuses a notice for a plan-created memory even if a caller tried.
            CHECK(sourceRefused(refusalOf([&] {
                pool->execSqlSync(
                    "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, memory_id) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'memory.created', 'memory', $4::uuid, $4::uuid)",
                    s.couple, s.userB, s.userA, mm::idOf(converted));
            })));

            // A notified memory, then every later operation on it: still exactly one notice.
            const auto photo = del::readyPhoto(s.userA);
            const auto made = createMemory(s.userA, mm::terms("D20-LATER", {photo}), chosen(true));
            CHECK(mm::is(made, SO::Created));
            if (!mm::is(made, SO::Created)) return;
            const auto memory = mm::idOf(made);
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK(mm::is(mm::edit(s.userA, memory, 1, mm::terms("D20-EDITED", {photo})), SO::Created));
            const auto asked = mm::propose(s.userB, memory, 2, mm::terms("D20-ASKED", {photo}));
            const auto proposal = mm::proposalOf(asked);
            CHECK(!proposal.empty());
            if (!proposal.empty()) CHECK(mm::is(mm::decide(s.userA, memory, proposal, mm::Decision::Reject), SO::Ok));
            const auto gone = mdel::erase(s.userA, memory);
            CHECK(mm::is(gone, SO::Created));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK_EQ(count(kIntentsOfMemorySql, memory), 1);
            // The partner's notice stays, but opening it goes through today's authorization: the deleted memory is not found.
            CHECK(mm::is(mm::getMemory(s.userB, memory), SO::NotFound));
            const auto listed = inbox(s.userB);
            CHECK(listed && listed->body["notifications"].size() == 1u);
            if (listed && listed->body["notifications"].size() == 1u) {
                const auto text = written(listed->body);
                CHECK(text.find("D20-LATER") == std::string::npos);
                CHECK(text.find("D20-EDITED") == std::string::npos);
            }
            // Another couple cannot open it either; undo is silent too.
            const auto other = md::space();
            CHECK(mm::is(mm::getMemory(other.userA, memory), SO::NotFound));
            const auto cycle = mdel::cycleOf(gone);
            CHECK(!cycle.empty());
            if (!cycle.empty()) CHECK(mm::is(mdel::undo(s.userA, memory, cycle), SO::Ok));
            CHECK(mm::is(mm::getMemory(s.userB, memory), SO::Ok));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("D20 album: one notice per successful batch, never per photo; replay, changed choice, rejection and rollback add none") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto album = al::idOf(al::create(s.userA, "D20-ALBUM-NAME"));
            CHECK(!album.empty());
            if (album.empty()) return;
            const std::vector<std::string> three = {del::readyPhoto(s.userA), del::readyPhoto(s.userA), del::readyPhoto(s.userA)};
            const std::string key = c::newUuid();
            const auto added = addPhotos(s.userA, album, three, chosen(true), key);
            CHECK(al::is(added, AO::Created));
            if (!al::is(added, AO::Created)) return;
            CHECK(added->hasNotification);
            CHECK(facts(added->notification, true, true, "delivered_to_inbox"));
            CHECK_EQ(count(kNoticesOfAlbumSql, album), 1);
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            const auto row = obs().client->execSqlSync(
                "SELECT recipient_id::text AS r, event_type, target_kind, target_id::text AS t, memory_id IS NULL AND shared_plan_id IS NULL AS only_batch "
                "FROM notifications WHERE couple_id = $1::uuid", s.couple);
            CHECK_EQ(row.size(), 1u);
            if (row.size() == 1u) {
                CHECK_EQ(row[0]["r"].as<std::string>(), s.userB);
                CHECK_EQ(row[0]["event_type"].as<std::string>(), std::string("album.photos_added"));
                CHECK_EQ(row[0]["target_kind"].as<std::string>(), std::string("album"));
                CHECK_EQ(row[0]["t"].as<std::string>(), album);
                CHECK(row[0]["only_batch"].as<bool>());
            }
            const auto again = addPhotos(s.userA, album, three, chosen(true), key);
            CHECK(al::is(again, AO::Replayed));
            CHECK(again && written(again->notification) == written(added->notification));
            CHECK(al::is(addPhotos(s.userA, album, three, chosen(false), key), AO::KeyReused));
            CHECK(al::is(addPhotos(s.userA, album, three, omitted(), key), AO::KeyReused));
            CHECK_EQ(count(kNoticesOfAlbumSql, album), 1);
            CHECK_EQ(count(kIntentsOfAlbumSql, album), 1);

            // Default off: a second batch with the field omitted records its decision and notifies no one.
            const auto quiet = addPhotos(s.userA, album, {del::readyPhoto(s.userA)}, omitted());
            CHECK(al::is(quiet, AO::Created));
            CHECK(quiet && facts(quiet->notification, false, false, "not_requested"));
            CHECK_EQ(count(kNoticesOfAlbumSql, album), 1);
            CHECK_EQ(count(kIntentsOfAlbumSql, album), 2);

            // A rejected attachment saves nothing: no batch, intent or notice.
            const auto batches = count(kBatchesSql, album);
            const auto partners = del::readyPhoto(s.userB);
            CHECK(al::is(addPhotos(s.userA, album, {del::readyPhoto(s.userA), partners}, chosen(true)), AO::PhotoUnavailable));
            // A failure after binding rolls the whole batch back, including any decision.
            const auto rolled = del::readyPhoto(s.userA);
            CHECK(al::is(addPhotos(s.userA, album, {rolled}, chosen(true), c::newUuid(), A::Fault::FailAfterBinding), AO::Error));
            CHECK_EQ(count(kBatchesSql, album), batches);
            CHECK_EQ(count(kNoticesOfAlbumSql, album), 1);
            CHECK_EQ(count(kIntentsOfAlbumSql, album), 2);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid", rolled), 0);

            // Covers and removals are silent; explicit album choices are never remembered.
            CHECK(al::is(al::cover(s.userA, album, three[1]), AO::Created));
            CHECK(al::is(al::removePhoto(s.userA, album, three[2]), AO::Ok));
            CHECK_EQ(count(kNoticesInCoupleSql, s.couple), 1);
            CHECK_EQ(count(kRememberedSql, s.userA), 0);

            // Generic inbox entry; no partner and another couple.
            const auto listed = inbox(s.userB);
            CHECK(listed && listed->status == 200);
            if (listed && listed->status == 200) {
                const auto text = written(listed->body);
                CHECK(text.find("D20-ALBUM-NAME") == std::string::npos);
                for (const auto &id : three) CHECK(text.find(id) == std::string::npos);
            }
            ts::CommittedSpace solo;
            ts::commitSpace(pool, solo, false);
            const auto soloAlbum = al::idOf(al::create(solo.userA, "D20-SOLO-ALBUM"));
            const auto lonely = addPhotos(solo.userA, soloAlbum, {del::readyPhoto(solo.userA)}, chosen(true));
            CHECK(al::is(lonely, AO::Created));
            CHECK(lonely && facts(lonely->notification, true, false, "no_partner"));
            CHECK_EQ(count(kNoticesInCoupleSql, solo.couple), 0);
            // A notice cannot name a batch of another couple's album: the guard refuses it.
            CHECK(sourceRefused(refusalOf([&] {
                pool->execSqlSync(
                    "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, album_batch_id) "
                    "SELECT $1::uuid, $2::uuid, $3::uuid, 'album.photos_added', 'album', b.album_id, b.id FROM album_photo_batches b "
                    "WHERE b.album_id = $4::uuid LIMIT 1",
                    s.couple, s.userA, s.userB, soloAlbum);
            })));
            // The album page itself still decides access: another couple cannot read it.
            const auto other = md::space();
            CHECK(al::is(al::getAlbum(other.userA, album), AO::NotFound));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
}

}  // namespace usapptest::phase3notification
