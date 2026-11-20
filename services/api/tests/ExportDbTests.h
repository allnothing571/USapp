#pragma once

// E1 structured user-data export (docs/user-data-export-design.md). Three entries:
//   run        CTest target export_db: scope, isolation, deletion/withholding, snapshot boundary, lock/deadline/statement bounds, caps.
//   runLarge   harness only: a synthetic dataset sized against the default limits (timing evidence, kept out of CTest).
//   runClosure harness only, LAST: it commits a J3 recovery closure, which permanently closes this database's media/memory domain.
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AchievementDbTests.h"
#include "AlbumDbTests.h"
#include "MemoryEraseDbTests.h"
#include "MessageDbTests.h"
#include "ProductDbTests.h"
#include "ReminderDbTests.h"
#include "services/ExportService.h"

namespace usapptest::exportdb {
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace base = usapptest::memorydeletion;
namespace del = usapptest::mediadeletion;
namespace er = usapptest::memoryerase;
namespace al = usapptest::album;
namespace msg = usapptest::message;
namespace pr = usapptest::product;
namespace rem = usapptest::reminder;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using E = usapp::ExportService;
using EO = E::Outcome;
using Decision = usapp::MemoryService::Decision;

// The answer comes before the slot is released (the transaction's end is confirmed afterwards). By default, after an admitted
// export, wait (bounded, on the condition) until no slot is busy, so sequential cases start from a free gate. Pass settle=false
// where a case deliberately keeps the slot held.
inline std::optional<E::Result> exportOf(const std::string &user, int waitMs = 30000, bool settle = true) {
    auto p = std::make_shared<ts::Pending<E::Result>>();
    E::exportFor(user, [p](E::Result r) { p->deliver(std::move(r)); });
    auto r = p->wait(std::chrono::milliseconds(waitMs));
    if (settle && r && r->outcome != EO::Busy) {
        try {
            md::waitUntil([] { return E::busySlotsForTesting() == 0; }, 20000);
        } catch (const std::exception &) {
            c::expectThat(false, "export-settle", "the admitted export's slot was not released");
        }
    }
    return r;
}
inline Json::Value parse(const std::string &text) {
    Json::Value out;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &out, &errors)) throw std::runtime_error("export is not JSON");
    return out;
}
inline bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
inline bool ok(const std::optional<E::Result> &r) { return r && r->outcome == EO::Ok; }
inline std::string bodyOf(const std::string &user) {
    const auto r = exportOf(user);
    return ok(r) ? r->body : std::string();
}
// Hooks run on the database event loop and must never block it: hand the continuation to the test thread.
inline std::shared_ptr<ts::Pending<std::function<void()>>> pauseAfterLock() {
    auto release = std::make_shared<ts::Pending<std::function<void()>>>();
    E::setAfterLockHookForTesting([release](std::function<void()> resume) { release->deliver(std::move(resume)); });
    return release;
}
inline E::Limits openLimits() {
    E::Limits l;
    l.frequencySeconds = 0;
    return l;
}
// Every key anywhere in the document; none may name a secret or an internal column.
inline void keys(const Json::Value &v, std::vector<std::string> &out) {
    if (v.isObject()) {
        for (const auto &name : v.getMemberNames()) { out.push_back(name); keys(v[name], out); }
    } else if (v.isArray()) {
        for (const auto &item : v) keys(item, out);
    }
}
inline bool forbiddenKey(const std::string &k) {
    for (const char *bad : {"idempotency", "fingerprint", "request_key", "token", "csrf", "code_hash", "staging", "object_key", "final_key",
                            "endpoint", "p256dh", "password", "url", "email_canonical", "couple_id", "text_id", "lock_token", "operation_key"}) {
        if (k.find(bad) != std::string::npos) return true;
    }
    return k == "auth" || k == "request";
}
inline std::string footprint() {
    return md::obs().client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM audit_logs), (SELECT count(*) FROM notifications), (SELECT count(*) FROM notification_intents), "
        "(SELECT count(*) FROM wallet_operations), (SELECT count(*) FROM wallet_ledger_entries), (SELECT count(*) FROM achievement_activation), "
        "(SELECT count(*) FROM memory_deletions), (SELECT count(*) FROM memory_purges), (SELECT count(*) FROM media_deletions), "
        "(SELECT count(*) FROM media_cleanup_plans), (SELECT count(*) FROM deletion_recovery_attempts), (SELECT count(*) FROM avatar_purchases), "
        "(SELECT count(*) FROM avatar_loadout_events), (SELECT count(*) FROM reminder_occurrences), (SELECT count(*) FROM sessions)) AS v")[0]["v"]
        .as<std::string>();
}
inline std::string withoutStamp(Json::Value doc) {
    doc["export"].removeMember("generated_at");
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, doc);
}
inline long long waitingSharedOrExclusive() {
    return md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33026 "
                                         "AND objid = 0 AND NOT granted")[0]["n"].as<long long>();
}
// A separate maintenance connection that holds the recovery lock exclusively, as a closure does, until released.
struct Holder {
    drogon::orm::DbClientPtr client;
    std::shared_ptr<drogon::orm::Transaction> tx;
    explicit Holder(const std::string &sql = "SELECT pg_advisory_xact_lock(33026, 0)") {
        const char *info = std::getenv("USAPP_TEST_MAINT_CONNINFO");
        if (!info || !*info) throw std::runtime_error("this case needs USAPP_TEST_MAINT_CONNINFO");
        client = drogon::orm::DbClient::newPgClient(info, 1);
        for (int tries = 0;; ++tries) {
            try { tx = client->newTransaction(); break; } catch (const std::exception &) { if (tries > 50) throw; md::sleepMs(20); }
        }
        tx->execSqlSync(sql);
    }
    void rollback() {
        if (!tx) return;
        tx->rollback();
        tx.reset();
        md::waitUntil([] {
            return md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33026 "
                                                 "AND objid = 0 AND mode = 'ExclusiveLock' AND granted")[0]["n"].as<long long>() == 0;
        });
    }
    // Commit, and wait for its acknowledgement.
    void commit() {
        auto done = std::make_shared<ts::Pending<bool>>();
        tx->setCommitCallback([done](bool committed) { done->deliver(committed); });
        tx.reset();
        const auto committed = done->wait();
        if (!committed || !*committed) throw std::runtime_error("holder commit failed");
    }
};
struct LimitsGuard {
    explicit LimitsGuard(const E::Limits &l) { E::setLimitsForTesting(l); E::clearAdmissionsForTesting(); }
    ~LimitsGuard() {
        E::setAfterLockHookForTesting(nullptr);
        E::setBeforeEndHookForTesting(nullptr);
        E::setReleaseObserverForTesting(nullptr);
        E::setVerifyFaultsForTesting(0);
        E::setTokenObserverForTesting(nullptr);
        E::setBeginReplyLossForTesting(false);
        E::setGenerationReplyLossForTesting(false);
        E::setCapturedPidOverrideForTesting(0);
        // Let any export still cleaning up finish before the next case changes limits (bounded, condition-based). A destructor
        // must not throw: a slot still held here is recorded as a failed check instead.
        try {
            md::waitUntil([] { return E::busySlotsForTesting() == 0; }, 20000);
        } catch (const std::exception &) {
            c::expectThat(false, "export-guard", "an export slot was still held when the case ended");
        }
        E::resetLimitsForTesting();
        E::clearAdmissionsForTesting();
    }
};
inline std::string messageOf(const std::string &author, const std::string &text) {
    const auto made = msg::create(author, text);
    if (!made || !made->success()) throw std::runtime_error("message fixture");
    return made->data.id;
}
inline void bulkMessages(const ts::CommittedSpace &s, int count, int codePoints) {
    md::obs().client->execSqlSync(
        "INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint) "
        "SELECT $1::uuid, $2::uuid, $3::uuid, repeat('长', $5::integer), 'e1bulk' || md5(random()::text), repeat('d', 64) "
        "FROM generate_series(1, $4::integer)",
        s.couple, s.userA, s.userB, count, codePoints);
}


// The one backend holding the export's shared recovery lock right now (0 if none). Read from the observer connection.
inline int exportBackend() {
    const auto rows = md::obs().client->execSqlSync("SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND classid = 33026 AND objid = 0 "
                                                    "AND granted AND mode = 'ShareLock'");
    return rows.size() == 1u ? rows[0]["pid"].as<int>() : 0;
}
inline long long locksOf(int pid) {
    return md::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM pg_locks WHERE pid = $1::integer AND locktype = 'advisory' "
                                         "AND classid = 33026", pid)[0]["n"].as<long long>();
}
inline std::string stateOf(int pid) {
    const auto rows = md::obs().client->execSqlSync("SELECT COALESCE(state, '') AS s FROM pg_stat_activity WHERE pid = $1::integer", pid);
    return rows.empty() ? std::string("gone") : rows[0]["s"].as<std::string>();
}

// A separate application-role session, as another API connection would be: open a transaction and keep it open.
struct OtherSession {
    drogon::orm::DbClientPtr client;
    std::shared_ptr<drogon::orm::Transaction> tx;
    int pid = 0;
    OtherSession(const std::string &dsn, const std::string &applicationName) {
        client = drogon::orm::DbClient::newPgClient(dsn, 1);
        for (int tries = 0;; ++tries) {
            try { tx = client->newTransaction(); break; } catch (const std::exception &) { if (tries > 50) throw; md::sleepMs(20); }
        }
        tx->execSqlSync("SELECT set_config('application_name', $1, false)", applicationName);
        pid = tx->execSqlSync("SELECT pg_backend_pid() AS pid")[0]["pid"].as<int>();
    }
    void end() {
        if (!tx) return;
        tx->rollback();
        tx.reset();
        md::waitUntil([this] { return stateOf(pid) == "idle"; });
    }
};

inline std::shared_ptr<ts::Pending<E::Release>> observeRelease() {
    auto released = std::make_shared<ts::Pending<E::Release>>();
    E::setReleaseObserverForTesting([released](const E::Release &r) { released->deliver(r); });
    return released;
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    E::init(dsn, 2);  // the dedicated export sessions, as main() does

    TEST_CASE("E1: scope, isolation, own ledger disclosure, expired history, allowlist, determinism and no side effects") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            // Tasks both ways; a completion each, so both members have ledgers.
            const auto toB = c::publish(s.userA, "E1-TASK-FOR-B", 2, 3);
            CHECK(c::is(c::runComplete(s.userB, toB, c::newUuid()), c::Outcome::Completed));
            const auto toA = c::publish(s.userB, "E1-TASK-FOR-A", 5, 0);
            CHECK(c::is(c::runComplete(s.userA, toA, c::newUuid()), c::Outcome::Completed));
            // Q4: a task expired 40 days ago, beyond the list's 30-day window.
            const auto old = rem::publishNoDeadline(s.userA);
            base::asMaintenance("UPDATE tasks SET created_at = clock_timestamp() - interval '60 days', "
                                "deadline_local_date = (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date - 40, "
                                "deadline_at = (((clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date - 40) + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' "
                                "WHERE id = '" + old + "';");
            // Private data of both: reminders (own only), avatar rows (own only).
            const auto remindB = rem::publishOneOff(s.userA, md::obs(), 3);
            const auto remindA = rem::publishOneOff(s.userB, md::obs(), 3);
            CHECK(rem::is(rem::setTask(s.userB, remindB, rem::before(30)), rem::RO::Created));
            CHECK(rem::is(rem::setTask(s.userA, remindA, rem::before(45)), rem::RO::Created));
            const auto ownBuy = c::newUuid(), partnerBuy = c::newUuid(), partnerLoadout = c::newUuid();
            base::asMaintenance(
                "INSERT INTO avatar_purchases (id, couple_id, user_id, item_id, item_version, name, category, asset_key, price_sunshine, operation_id, "
                "idempotency_key, request_fingerprint) VALUES ('" + ownBuy + "', '" + s.couple + "', '" + s.userB + "', gen_random_uuid(), 1, 'E1OWNBUY', "
                "'hat', 'hat.e1own.v1', 5, gen_random_uuid(), 'e1ownbuykey000000', repeat('e', 64)), ('" + partnerBuy + "', '" + s.couple + "', '" +
                s.userA + "', gen_random_uuid(), 1, 'E1PARTNERBUY', 'hat', 'hat.e1partner.v1', 5, gen_random_uuid(), 'e1partnerbuykey00', repeat('e', 64)); "
                "INSERT INTO avatar_loadout_events (id, user_id, couple_id, version, hair_item_id, clothes_item_id, expression_item_id, requested, "
                "idempotency_key, request_fingerprint) VALUES ('" + partnerLoadout + "', '" + s.userA + "', '" + s.couple + "', 1, gen_random_uuid(), "
                "gen_random_uuid(), gen_random_uuid(), '{}'::jsonb, 'e1partnerloadkey0', repeat('e', 64));");
            // Shared history: a message and its reply; a product the partner archived (D14: hidden from its owner); one still active.
            const auto message = messageOf(s.userA, "E1-MESSAGE-FROM-A");
            CHECK(msg::is(msg::reply(s.userB, message, "E1-REPLY-FROM-B"), msg::MO::Created));
            const auto archived = pr::create(s.userA, pr::terms("E1-ARCHIVED-PRODUCT", 3));
            CHECK(archived && archived->success());
            CHECK(pr::is(pr::archive(s.userA, archived->product.id, 1), pr::PO::Updated));
            const auto active = pr::create(s.userA, pr::terms("E1-ACTIVE-PRODUCT", 4));
            CHECK(active && active->success());
            messageOf(other.userA, "E1-OTHER-COUPLE");

            const auto before = footprint();
            const auto first = exportOf(s.userB);
            const auto second = exportOf(s.userB);
            CHECK_EQ(footprint(), before);  // read only: nothing written anywhere
            CHECK(ok(first) && ok(second));
            if (!ok(first) || !ok(second)) return;
            const auto doc = parse(first->body);
            CHECK_EQ(withoutStamp(doc), withoutStamp(parse(second->body)));  // stable order and content
            CHECK(first->filename.rfind("usapp-export-", 0) == 0);
            CHECK_EQ(doc["export"]["subject_user_id"].asString(), s.userB);
            CHECK_EQ(doc["export"]["schema_version"].asInt(), 1);
            CHECK_EQ(doc["export"]["photos_included"].asBool(), false);
            CHECK_EQ(doc["account"]["id"].asString(), s.userB);
            CHECK_EQ(doc["space"]["members"].size(), 2u);
            const auto &body = first->body;
            // Shared records, including the partner's words.
            for (const char *shared : {"E1-TASK-FOR-B", "E1-TASK-FOR-A", "E1-MESSAGE-FROM-A", "E1-REPLY-FROM-B", "E1-ACTIVE-PRODUCT"}) CHECK(has(body, shared));
            // D14: the owner does not see an archived product, nor its revisions.
            CHECK_FALSE(has(body, "E1-ARCHIVED-PRODUCT"));
            CHECK(has(bodyOf(s.userA), "E1-ARCHIVED-PRODUCT"));  // its creator does
            // Q4: the old expired task with its stored deadline.
            bool sawOld = false;
            for (const auto &task : doc["tasks"]) if (task["id"].asString() == old) sawOld = !task["deadline_at"].isNull();
            CHECK(sawOld);
            // Private data: own only.
            CHECK_EQ(doc["reminder_rules"].size(), 1u);
            CHECK_EQ(doc["reminder_rules"][0]["task_id"].asString(), remindB);
            CHECK(has(body, "E1OWNBUY"));
            CHECK_FALSE(has(body, "E1PARTNERBUY"));
            CHECK_FALSE(has(body, partnerLoadout));
            CHECK_EQ(doc["avatar_loadouts"].size(), 0u);
            CHECK_FALSE(has(body, "E1-OTHER-COUPLE"));
            CHECK_FALSE(has(body, other.userA));
            // The ledger: a deliberate new disclosure of the caller's own entries, from the same snapshot as the balances.
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value());
            CHECK_EQ(doc["wallet"]["balances"]["flower"].asInt64(), wallet ? static_cast<Json::Int64>(wallet->flower) : -1);
            CHECK_EQ(doc["wallet"]["balances"]["sunshine"].asInt64(), wallet ? static_cast<Json::Int64>(wallet->sunshine) : -1);
            long long ledgerFlower = 0, lastFlower = 0;
            for (const auto &e : doc["wallet"]["ledger"]) {
                if (e["asset_type"].asString() == "flower") { ledgerFlower += e["amount"].asInt64(); lastFlower = e["balance_after"].asInt64(); }
            }
            CHECK_EQ(ledgerFlower, doc["wallet"]["balances"]["flower"].asInt64());
            CHECK_EQ(lastFlower, doc["wallet"]["balances"]["flower"].asInt64());
            CHECK_EQ(static_cast<long long>(doc["wallet"]["ledger"].size()),
                     c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE user_id = $1::uuid", s.userB));
            const auto partnerEntries = md::obs().client->execSqlSync("SELECT id::text AS id FROM wallet_ledger_entries WHERE user_id = $1::uuid", s.userA);
            CHECK(!partnerEntries.empty());
            for (const auto &row : partnerEntries) CHECK_FALSE(has(body, row["id"].as<std::string>()));
            // Achievements: derived from the caller's own completions, equal to the achievement read.
            const auto badges = usapptest::achievement::read(s.userB);
            CHECK(badges && doc["achievements"]["total_completed"] == badges->body["total_completed"]);
            CHECK(badges && doc["achievements"]["badges"] == badges->body["badges"]);
            CHECK_EQ(doc["achievements"]["derived"].asBool(), true);
            // The allowlist: no secret-like key anywhere, and no stored key or fingerprint value.
            std::vector<std::string> names;
            keys(doc, names);
            for (const auto &k : names) CHECK_FALSE(forbiddenKey(k));
            for (const auto &row : md::obs().client->execSqlSync(
                     "SELECT idempotency_key AS k FROM messages WHERE couple_id = $1::uuid UNION ALL SELECT create_idempotency_key FROM tasks WHERE couple_id = $1::uuid "
                     "UNION ALL SELECT request_fingerprint FROM messages WHERE couple_id = $1::uuid", s.couple)) {
                CHECK_FALSE(has(body, row["k"].as<std::string>()));
            }
            CHECK_FALSE(has(body, "repeat("));
            // No space: the caller only, inside generation.
            const auto lonely = c::newUuid();
            ts::commitUser(pool, lonely, "e1_lonely");
            const auto none = exportOf(lonely);
            CHECK(none && none->outcome == EO::SpaceNotFound && none->body.empty());
        } catch (const std::exception &e) { c::expectThat(false, "export-scope", md::exceptionCategory(e)); }
    }

    TEST_CASE("E1: deleted, undoable, erased memories; deleted and proposal photos; album items; plan links and O12") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            const auto mine = del::readyPhoto(s.userB);
            const auto theirs = del::readyPhoto(s.userA);
            // A live memory of B with B's photo; A's approved proposal adds A's photo, which A then deletes.
            const auto live = mm::idOf(mm::create(s.userB, mm::terms("E1-LIVE-MEMORY", {mine})));
            const auto approved = mm::proposalOf(mm::propose(s.userA, live, 1, mm::terms("E1-LIVE-EDITED", {mine, theirs})));
            CHECK(mm::is(mm::decide(s.userB, live, approved, Decision::Approve), mm::SO::Ok));
            // A rejected proposal naming A's private upload, and a pending one naming another.
            const auto rejectedPhoto = del::readyPhoto(s.userA);
            const auto rejected = mm::proposalOf(mm::propose(s.userA, live, 2, mm::terms("E1-REJECTED-ASK", {mine, rejectedPhoto})));
            CHECK(mm::is(mm::decide(s.userB, live, rejected, Decision::Reject), mm::SO::Ok));
            const auto pendingPhoto = del::readyPhoto(s.userA);
            const auto pending = mm::proposalOf(mm::propose(s.userA, live, 2, mm::terms("E1-PENDING-ASK", {mine, pendingPhoto})));
            CHECK(!pending.empty());
            const auto deletedPhoto = del::cycleOf(del::erase(s.userA, theirs));
            CHECK(!deletedPhoto.empty());
            // Deleted memories: one undoable, one purged and erased.
            const auto undoable = mm::idOf(mm::create(s.userB, mm::terms("E1-UNDOABLE-MEMORY", {del::readyPhoto(s.userB)})));
            const auto undoCycle = base::cycleOf(base::erase(s.userB, undoable));
            CHECK(!undoCycle.empty());
            const auto erased = mm::idOf(mm::create(s.userB, mm::terms("E1-ERASED-MEMORY", {del::readyPhoto(s.userB)})));
            const auto aged = base::cycleAged(erased, s.couple, s.userB, "200 hours");
            CHECK_EQ(er::eraseNow(aged), std::string("erased"));
            // A plan converted to a memory that is then deleted: the plan text stays (O12), the link is withheld.
            const auto plan = base::finishedPlan(s.userA);
            const auto fromPlan = mm::idOf(base::forPlan(s.userA, plan, c::newUuid(), mm::terms("E1-PLAN-MEMORY", {del::readyPhoto(s.userA)})));
            CHECK(!base::cycleOf(base::erase(s.userA, fromPlan)).empty());
            // An album: B's item stays; A's item is deleted and omitted.
            const auto album = al::idOf(al::create(s.userB, "E1-ALBUM"));
            const auto albumMine = del::readyPhoto(s.userB), albumTheirs = del::readyPhoto(s.userA);
            const auto addedMine = al::add(s.userB, album, {albumMine});
            const auto addedTheirs = al::add(s.userA, album, {albumTheirs});
            CHECK(addedMine && addedMine->success() && addedTheirs && addedTheirs->success());
            CHECK(!del::cycleOf(del::erase(s.userA, albumTheirs)).empty());

            const auto exported = exportOf(s.userB);
            CHECK(ok(exported));
            if (!ok(exported)) return;
            const auto &body = exported->body;
            const auto doc = parse(body);
            CHECK_EQ(doc["memories"]["state"].asString(), std::string("available"));
            for (const auto &gone : {undoable, erased, fromPlan, theirs, rejectedPhoto, albumTheirs}) CHECK_FALSE(has(body, gone));
            for (const char *text : {"E1-UNDOABLE-MEMORY", "E1-ERASED-MEMORY", "[erased]", "E1-PLAN-MEMORY"}) CHECK_FALSE(has(body, text));
            for (const char *text : {"E1-LIVE-MEMORY", "E1-LIVE-EDITED", "E1-REJECTED-ASK", "E1-PENDING-ASK", "PLAN-OWN-TEXT"}) CHECK(has(body, text));
            CHECK(has(body, live) && has(body, mine) && has(body, pendingPhoto) && has(body, albumMine));
            // Every photo slot is either a visible reference or a withheld marker without any id.
            int withheld = 0;
            for (const char *section : {"memory_revisions", "memory_proposals"}) {
                for (const auto &record : doc[section]) {
                    for (const auto &slot : record["photos"]) {
                        if (slot["photo"].isNull()) {
                            ++withheld;
                            CHECK_EQ(slot["withheld"].asString(), std::string("unavailable"));
                            CHECK_EQ(slot.getMemberNames().size(), 3u);
                        }
                    }
                }
            }
            CHECK_EQ(withheld, 3);  // the deleted photo in revision 2 and in the approved proposal, and the rejected private upload
            CHECK_EQ(doc["album_items"].size(), 1u);
            bool sawLink = false;
            for (const auto &p : doc["shared_plans"]) {
                if (p["id"].asString() != plan) continue;
                sawLink = p["memory_link"]["memory_id"].isNull() && p["memory_link"]["withheld"].asString() == "unavailable";
                CHECK_EQ(p["body"]["title"].asString(), std::string("PLAN-OWN-TEXT"));
            }
            CHECK(sawLink);
            // The uploader's own export does show the rejected upload's reference; the partner's never did.
            CHECK(has(bodyOf(s.userA), rejectedPhoto));
            // Undo restores the undoable memory in a later export.
            CHECK(mm::is(base::undo(s.userB, undoable, undoCycle), mm::SO::Ok));
            CHECK(has(bodyOf(s.userB), "E1-UNDOABLE-MEMORY"));
        } catch (const std::exception &e) { c::expectThat(false, "export-deletion", md::exceptionCategory(e)); }
    }

    TEST_CASE("E1: snapshot after the lock; lock timeout, end-to-end deadline and statement timeout fail closed and release the gate") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            const auto doomed = mm::idOf(mm::create(s.userB, mm::terms("E1-BOUNDARY-MEMORY", {del::readyPhoto(s.userB)})));
            // A deletion committed while the export holds the lock, before generation starts, is excluded: the snapshot follows the lock.
            auto paused = pauseAfterLock();
            auto boundaryAnswer = std::make_shared<ts::Pending<E::Result>>();
            E::exportFor(s.userB, [boundaryAnswer](E::Result r) { boundaryAnswer->deliver(std::move(r)); });
            const auto boundaryResume = paused->wait();
            CHECK(boundaryResume.has_value());
            E::setAfterLockHookForTesting(nullptr);
            CHECK(!base::cycleOf(base::erase(s.userB, doomed)).empty());  // commits while the export holds the lock, before generation
            if (boundaryResume) (*boundaryResume)();
            const auto boundary = boundaryAnswer->wait();
            CHECK(ok(boundary) && !has(boundary->body, "E1-BOUNDARY-MEMORY"));
            // A deletion after an export finished changes only the next one.
            const auto later = mm::idOf(mm::create(s.userB, mm::terms("E1-LATER-MEMORY", {del::readyPhoto(s.userB)})));
            const auto beforeDelete = exportOf(s.userB);
            CHECK(ok(beforeDelete) && has(beforeDelete->body, "E1-LATER-MEMORY"));
            CHECK(!base::cycleOf(base::erase(s.userB, later)).empty());
            CHECK(ok(beforeDelete) && has(beforeDelete->body, "E1-LATER-MEMORY"));  // a returned file is not changed by later deletions
            const auto afterDelete = exportOf(s.userB);
            CHECK(ok(afterDelete) && !has(afterDelete->body, "E1-LATER-MEMORY"));

            // Bounded lock wait.
            {
                auto l = openLimits();
                l.lockTimeoutMs = 300;
                E::setLimitsForTesting(l);
                Holder holder;
                const auto started = std::chrono::steady_clock::now();
                const auto waited = exportOf(s.userB);
                CHECK(waited && waited->outcome == EO::Unavailable && waited->reason == "lock_timeout" && waited->body.empty());
                CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
                holder.rollback();
            }
            // The end-to-end deadline answers first; the gate stays held until the database work ends, then is released.
            {
                auto l = openLimits();
                l.lockTimeoutMs = 20000;
                l.deadlineMs = 400;
                E::setLimitsForTesting(l);
                Holder holder;
                const auto late = exportOf(s.userB, 30000, false);
                CHECK(late && late->outcome == EO::Unavailable && late->reason == "deadline" && late->body.empty());
                const auto busy = exportOf(s.userB);
                CHECK(busy && busy->outcome == EO::Busy && busy->reason == "concurrent");
                CHECK_EQ(waitingSharedOrExclusive(), 1);
                holder.rollback();
                E::setLimitsForTesting(openLimits());
                md::waitUntil([&] { const auto r = exportOf(s.userB); return r && r->outcome == EO::Ok; }, 8000);
            }
            // Generation timeout: the lock was taken (the hook ran), then the generation statement hit statement_timeout.
            {
                bulkMessages(s, 3000, 50);
                auto l = openLimits();
                l.statementTimeoutMs = 5;
                E::setLimitsForTesting(l);
                auto locked = std::make_shared<std::atomic<bool>>(false);
                E::setAfterLockHookForTesting([locked](std::function<void()> resume) { locked->store(true); resume(); });
                const auto slow = exportOf(s.userB);
                E::setAfterLockHookForTesting(nullptr);
                CHECK(locked->load());
                CHECK(slow && slow->outcome == EO::Unavailable && slow->reason == "statement_timeout" && slow->body.empty());
                E::setLimitsForTesting(openLimits());
                CHECK(ok(exportOf(s.userB)));  // the slot was released
            }
        } catch (const std::exception &e) { c::expectThat(false, "export-bounds", md::exceptionCategory(e)); }
    }

    TEST_CASE("E1: admission and frequency; row, input-byte and final-size caps fail closed before aggregation; exact final bytes") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            c::publish(s.userA, "E1-CAP-1", 1, 0);
            c::publish(s.userA, "E1-CAP-2", 1, 0);
            messageOf(s.userA, "E1-CAP-MESSAGE");
            const auto full = exportOf(s.userB);
            CHECK(ok(full));
            if (!ok(full)) return;
            const auto size = static_cast<long long>(full->body.size());
            const auto expectTooLarge = [&](const E::Limits &l, const std::string &reason) {
                E::setLimitsForTesting(l);
                const auto r = exportOf(s.userB);
                CHECK(r && r->outcome == EO::TooLarge && r->reason == reason && r->body.empty() && r->filename.empty());
            };
            auto l = openLimits();
            l.maxSectionRows = 1;
            expectTooLarge(l, "section_rows");
            l = openLimits();
            l.maxTotalRows = 2;
            expectTooLarge(l, "total_rows");
            l = openLimits();
            l.maxInputBytes = 200;
            expectTooLarge(l, "input_bytes");
            l = openLimits();
            l.maxOutputBytes = size - 1;
            expectTooLarge(l, "output_bytes");
            l = openLimits();
            l.maxOutputBytes = size;
            E::setLimitsForTesting(l);
            const auto exact = exportOf(s.userB);
            CHECK(ok(exact) && static_cast<long long>(exact->body.size()) == size);
            // The gate. Each section CTE is scanned twice: once by the pre-aggregation counts (always) and once by its array
            // aggregation inside the document. Refused (section cap 1): every aggregation scan is "never executed". Accepted
            // (positive control): every scan of the same CTE ran. Plan text only, no row data.
            const auto planOf = [&](int sectionCap) {
                const auto plan = md::obs().client->execSqlSync("EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) " + E::generationSql(), s.userB,
                                                                sectionCap, 60000LL, 15LL * 1024 * 1024);
                std::vector<std::string> lines;
                for (const auto &row : plan) lines.push_back(row[static_cast<drogon::orm::Row::SizeType>(0)].as<std::string>());
                return lines;
            };
            const auto refused = planOf(1), accepted = planOf(20000);
            // A scan of exactly this CTE: the name followed by a space (alias or timing) or the end of the line.
            const auto scans = [](const std::string &line, const std::string &scan) {
                const auto at = line.find(scan);
                return at != std::string::npos && (at + scan.size() == line.size() || line[at + scan.size()] == ' ');
            };
            for (const char *cte : {"s_tasks", "s_messages", "s_ledger", "s_plans", "s_album_items", "s_memory_revisions"}) {
                const std::string scan = std::string("CTE Scan on ") + cte;
                int refusedRan = 0, refusedSkipped = 0, acceptedSkipped = 0, acceptedScans = 0;
                for (const auto &line : refused) {
                    if (!scans(line, scan)) continue;
                    if (has(line, "never executed")) ++refusedSkipped; else ++refusedRan;
                }
                for (const auto &line : accepted) {
                    if (!scans(line, scan)) continue;
                    ++acceptedScans;
                    if (has(line, "never executed")) ++acceptedSkipped;
                }
                std::printf("E1_GATE %s refused_ran=%d refused_skipped=%d accepted_scans=%d accepted_skipped=%d\n", cte, refusedRan, refusedSkipped,
                            acceptedScans, acceptedSkipped);
                CHECK_EQ(refusedRan, 1);      // the counts scan
                CHECK(refusedSkipped >= 1);   // the aggregation scan(s), never run on refusal
                CHECK(acceptedScans >= 2);
                CHECK_EQ(acceptedSkipped, 0); // positive control: the same scans do run when accepted
            }
            // Process-local admission.
            l = openLimits();
            l.frequencySeconds = 60;
            E::setLimitsForTesting(l);
            E::clearAdmissionsForTesting();
            CHECK(ok(exportOf(s.userB)));
            const auto often = exportOf(s.userB);
            CHECK(often && often->outcome == EO::Busy && often->reason == "frequency" && often->retryAfterSeconds >= 1 && often->retryAfterSeconds <= 61);
            l = openLimits();
            l.maxGlobal = 1;
            E::setLimitsForTesting(l);
            auto release = pauseAfterLock();
            auto held = std::make_shared<ts::Pending<E::Result>>();
            E::exportFor(s.userA, [held](E::Result r) { held->deliver(std::move(r)); });
            const auto resume = release->wait();
            CHECK(resume.has_value());
            const auto crowded = exportOf(s.userB);
            CHECK(crowded && crowded->outcome == EO::Busy && crowded->reason == "global");
            E::setAfterLockHookForTesting(nullptr);
            if (resume) (*resume)();
            const auto first = held->wait();
            CHECK(first && first->outcome == EO::Ok);
            md::waitUntil([] { return E::busySlotsForTesting() == 0; }, 20000);
            CHECK(ok(exportOf(s.userB)));
        } catch (const std::exception &e) { c::expectThat(false, "export-limits", md::exceptionCategory(e)); }
    }

    TEST_CASE("E1: the slot is held from end request until the server-confirmed end, on success, deadline, error and an uncertain BEGIN") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            auto l = openLimits();
            l.maxGlobal = 1;
            E::setLimitsForTesting(l);

            // Success. (1) End requested: the answer is out, ROLLBACK not yet sent, so the transaction is genuinely still open.
            auto released = observeRelease();
            auto proceed = std::make_shared<ts::Pending<std::function<void()>>>();
            E::setBeforeEndHookForTesting([proceed](std::function<void()> go) { proceed->deliver(std::move(go)); });
            const auto first = exportOf(s.userB, 30000, false);
            CHECK(ok(first));
            const auto go = proceed->wait();
            CHECK(go.has_value());
            E::setBeforeEndHookForTesting(nullptr);
            const int pid = exportBackend();
            CHECK(pid != 0);
            CHECK_EQ(locksOf(pid), 1);
            CHECK_EQ(stateOf(pid), std::string("idle in transaction"));
            CHECK_EQ(E::busySlotsForTesting(), 1);
            auto refused = exportOf(s.userA);
            CHECK(refused && refused->outcome == EO::Busy && refused->reason == "global");
            // (2) Rollback answered but its end not yet confirmed: verification fails (as an unreachable server would); the slot stays held.
            E::setVerifyFaultsForTesting(1000);
            if (go) (*go)();
            md::waitUntil([pid] { return locksOf(pid) == 0; });
            CHECK_FALSE(released->readyWithin(std::chrono::milliseconds(300)));
            CHECK_EQ(E::busySlotsForTesting(), 1);
            refused = exportOf(s.userA);
            CHECK(refused && refused->outcome == EO::Busy && refused->reason == "global");
            // (3) Ended: the next verification succeeds; exactly one release, on the same session, which is idle and lock-free.
            E::setVerifyFaultsForTesting(0);
            const auto done = released->wait(std::chrono::milliseconds(10000));
            CHECK(done && done->cleanup == "ended" && done->pid == pid && done->verifyAttempts >= 2);
            CHECK_EQ(stateOf(pid), std::string("idle"));
            CHECK_EQ(locksOf(pid), 0);
            CHECK_EQ(released->calls(), 1);
            CHECK(ok(exportOf(s.userA)));  // admitted after the confirmed release

            // Deadline: the caller is answered at the deadline while the lock wait continues; the slot is held until the end.
            {
                auto dl = l;
                dl.deadlineMs = 300;
                dl.lockTimeoutMs = 20000;
                E::setLimitsForTesting(dl);
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                auto late = observeRelease();
                Holder holder;
                const auto answered = exportOf(s.userB, 30000, false);
                CHECK(answered && answered->outcome == EO::Unavailable && answered->reason == "deadline");
                CHECK_EQ(E::busySlotsForTesting(), 1);
                const auto crowded = exportOf(s.userA);
                CHECK(crowded && crowded->outcome == EO::Busy && crowded->reason == "global");
                CHECK_FALSE(late->readyWithin(std::chrono::milliseconds(200)));
                holder.rollback();
                const auto cleaned = late->wait(std::chrono::milliseconds(10000));
                CHECK(cleaned && cleaned->cleanup == "ended");
                CHECK_EQ(late->calls(), 1);
                E::setLimitsForTesting(l);
                CHECK(ok(exportOf(s.userA)));
            }

            // The deadline queues ordered ROLLBACK and token verification even if an early or final reply never arrives.
            for (const bool loseBegin : {false, true}) {
                auto dl = l;
                dl.deadlineMs = 300;
                E::setLimitsForTesting(dl);
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                auto ended = observeRelease();
                if (loseBegin) E::setBeginReplyLossForTesting(true, true);
                else E::setGenerationReplyLossForTesting(true);
                const auto missing = exportOf(s.userB, 10000, false);
                CHECK(missing && missing->outcome == EO::Unavailable && missing->reason == "deadline" && missing->body.empty());
                const auto cleaned = ended->wait(std::chrono::milliseconds(5000));
                CHECK(cleaned && cleaned->cleanup == "ended" && locksOf(cleaned->pid) == 0);
                if (loseBegin) CHECK(cleaned && cleaned->pid == 0);
                CHECK_EQ(ended->calls(), 1);
                CHECK_EQ(E::busySlotsForTesting(), 0);
                E::setLimitsForTesting(l);
                CHECK(ok(exportOf(s.userA)));
            }

            // Database error: generation hits statement_timeout; the aborted transaction is ended by the same ROLLBACK and confirmed.
            {
                bulkMessages(s, 3000, 50);
                auto slow = l;
                slow.statementTimeoutMs = 5;
                E::setLimitsForTesting(slow);
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                auto ended = observeRelease();
                const auto failed = exportOf(s.userB);
                CHECK(failed && failed->outcome == EO::Unavailable && failed->reason == "statement_timeout");
                const auto cleaned = ended->wait(std::chrono::milliseconds(10000));
                CHECK(cleaned && cleaned->cleanup == "ended" && locksOf(cleaned->pid) == 0);
                E::setLimitsForTesting(l);
            }


            // BEGIN applied, its reply lost before any pid was captured, and the original transaction still open elsewhere (a
            // session carrying this export's token, as the original would be after a half-open connection): the slot is NOT
            // released, nothing is terminated, and it is released only once that session's transaction has ended.
            {
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                auto token = std::make_shared<ts::Pending<std::string>>();
                E::setTokenObserverForTesting([token](const std::string &t) { token->deliver(t); });
                auto proceed = std::make_shared<ts::Pending<std::function<void()>>>();
                E::setBeforeEndHookForTesting([proceed](std::function<void()> go) { proceed->deliver(std::move(go)); });
                E::setBeginReplyLossForTesting(true);
                auto held = observeRelease();
                const auto uncertain = exportOf(s.userB, 30000, false);
                CHECK(uncertain && uncertain->outcome == EO::Unavailable && uncertain->reason == "session_uncertain" && uncertain->body.empty());
                const auto tag = token->wait();
                CHECK(tag.has_value());
                const auto go = proceed->wait();
                CHECK(go.has_value());
                E::setBeforeEndHookForTesting(nullptr);
                OtherSession original(dsn, tag.value_or("missing"));
                if (go) (*go)();
                md::waitUntil([] { const auto st = E::slotStatesForTesting(); return !st.empty() && st[0] == "unconfirmed"; });
                CHECK_FALSE(held->readyWithin(std::chrono::milliseconds(300)));
                CHECK_EQ(E::busySlotsForTesting(), 1);
                const auto blocked = exportOf(s.userA);
                CHECK(blocked && blocked->outcome == EO::Busy && blocked->reason == "global");
                CHECK_EQ(stateOf(original.pid), std::string("idle in transaction"));  // not terminated
                original.end();
                const auto cleaned = held->wait(std::chrono::milliseconds(10000));
                CHECK(cleaned && cleaned->pid == 0 && cleaned->cleanup == "ended");
                CHECK_EQ(stateOf(original.pid), std::string("idle"));  // still alive: never terminated
                CHECK_EQ(held->calls(), 1);
                CHECK(ok(exportOf(s.userA)));
            }

            // The captured pid points at an unrelated same-role session with an open transaction: no decision uses it. The export
            // is released on its own token, and the unrelated session is neither waited for nor terminated.
            {
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                OtherSession bystander(dsn, "usapp-unrelated-" + c::newUuid().substr(0, 8));
                E::setCapturedPidOverrideForTesting(bystander.pid);
                auto released = observeRelease();
                CHECK(ok(exportOf(s.userB, 30000, false)));
                const auto cleaned = released->wait(std::chrono::milliseconds(10000));
                CHECK(cleaned && cleaned->pid == bystander.pid && cleaned->cleanup == "ended");
                CHECK_EQ(stateOf(bystander.pid), std::string("idle in transaction"));
                E::setCapturedPidOverrideForTesting(0);
                bystander.end();
            }

        } catch (const std::exception &e) { c::expectThat(false, "export-lifecycle", md::exceptionCategory(e)); }
    }
}

// Harness only: connection loss after the recovery lock. How long Drogon's single connection takes to notice a server-terminated
// session and reconnect varies (about 1 s in run usapp-e1-09bfa86258's targeted pass, over 20 s in its CTest pass), so this case
// cannot fit export_db's 30 s budget and runs on its own with longer bounded waits. The assertions are unchanged.
inline void runConnectionLoss(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    E::init(dsn, 2);
    TEST_CASE("E1 connection loss: the slot is released only after the replaced session confirms the original is gone") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            auto l = openLimits();
            l.maxGlobal = 1;
            E::setLimitsForTesting(l);
            {
                md::waitUntil([] { return E::busySlotsForTesting() == 0; });
                auto replaced = observeRelease();
                auto paused = pauseAfterLock();
                auto answer = std::make_shared<ts::Pending<E::Result>>();
                const auto started = std::chrono::steady_clock::now();
                E::exportFor(s.userB, [answer](E::Result r) { answer->deliver(std::move(r)); });
                const auto resume = paused->wait();
                CHECK(resume.has_value());
                E::setAfterLockHookForTesting(nullptr);
                const int victim = exportBackend();
                CHECK(victim != 0);
                base::maintClient()->execSqlSync("SELECT pg_terminate_backend($1::integer)", victim);
                md::waitUntil([victim] { return stateOf(victim) == "gone"; });
                const auto terminated = std::chrono::steady_clock::now();
                if (resume) (*resume)();
                const auto lost = answer->wait(std::chrono::milliseconds(60000));
                const auto answeredAt = std::chrono::steady_clock::now();
                CHECK(lost && lost->outcome == EO::Unavailable && lost->body.empty());
                // While unanswered or uncleaned, the slot stays held: another export is refused.
                const auto cleaned = replaced->wait(std::chrono::milliseconds(60000));
                const auto releasedAt = std::chrono::steady_clock::now();
                const auto ms = [&](std::chrono::steady_clock::time_point t) {
                    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t - terminated).count());
                };
                std::printf("E1_LOSS reason=%s answer_ms=%lld release_ms=%lld cleanup=%s attempts=%d total_ms=%lld\n",
                            lost ? lost->reason.c_str() : "none", ms(answeredAt), ms(releasedAt), cleaned ? cleaned->cleanup.c_str() : "none",
                            cleaned ? cleaned->verifyAttempts : -1,
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(releasedAt - started).count()));
                CHECK(cleaned && cleaned->pid == victim && cleaned->cleanup == "session_replaced");
                CHECK_EQ(stateOf(victim), std::string("gone"));
                CHECK_EQ(replaced->calls(), 1);
                CHECK_EQ(E::busySlotsForTesting(), 0);
                CHECK(ok(exportOf(s.userA, 60000)));  // the reconnected session serves the next export
            }
        } catch (const std::exception &e) { c::expectThat(false, "export-connection-loss", md::exceptionCategory(e)); }
    }
}

// Harness only: a synthetic dataset near the default input cap succeeds within the default timeouts; just past it is refused.
inline void runLarge(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    E::init(dsn, 2);  // the dedicated export sessions, as main() does
    TEST_CASE("E1 large: synthetic data at the default limits") {
        LimitsGuard limits(openLimits());
        try {
            const auto defaults = E::Limits{};
            const auto s = md::space();
            bulkMessages(s, 8500, 500);  // 1500-byte bodies; about 14.7 MB of row JSON
            const auto started = std::chrono::steady_clock::now();
            auto l = openLimits();
            E::setLimitsForTesting(l);
            const auto big = exportOf(s.userB, 40000);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            CHECK(ok(big));
            if (big) std::printf("E1_LARGE ok=%d rows=%lld input_bytes=%lld output_bytes=%zu ms=%lld\n", ok(big) ? 1 : 0, big->rows, big->inputBytes,
                                 big->body.size(), static_cast<long long>(ms));
            CHECK(big && static_cast<long long>(big->body.size()) <= defaults.maxOutputBytes);
            CHECK(big && big->inputBytes <= defaults.maxInputBytes);
            CHECK(ms < defaults.deadlineMs);
            bulkMessages(s, 1000, 500);
            const auto refused = exportOf(s.userB, 40000);
            if (refused) std::printf("E1_LARGE_REFUSED reason=%s rows=%lld input_bytes=%lld\n", refused->reason.c_str(), refused->rows, refused->inputBytes);
            CHECK(refused && refused->outcome == EO::TooLarge && refused->reason == "input_bytes" && refused->body.empty());
            // The default per-section row cap, measured before aggregation.
            const auto rowsSpace = md::space();
            bulkMessages(rowsSpace, defaults.maxSectionRows + 1, 1);
            const auto tooMany = exportOf(rowsSpace.userB, 40000);
            if (tooMany) std::printf("E1_LARGE_ROWS reason=%s rows=%lld\n", tooMany->reason.c_str(), tooMany->rows);
            CHECK(tooMany && tooMany->outcome == EO::TooLarge && tooMany->reason == "section_rows");
        } catch (const std::exception &e) { c::expectThat(false, "export-large", md::exceptionCategory(e)); }
    }
}

// Harness only, LAST: both lock orders against a real closure, which then stays committed.
inline void runClosure(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    E::init(dsn, 2);  // the dedicated export sessions, as main() does
    TEST_CASE("E1 closure: an export holding the lock delays a closure; a closure holding it delays the export, which then sees it") {
        LimitsGuard limits(openLimits());
        try {
            md::useStore();
            const auto s = md::space();
            const auto memory = mm::idOf(mm::create(s.userB, mm::terms("E1-CLOSURE-MEMORY", {del::readyPhoto(s.userB)})));
            const auto plan = base::finishedPlan(s.userA);
            CHECK(!mm::idOf(base::forPlan(s.userA, plan, c::newUuid(), mm::terms("E1-CLOSURE-PLAN-MEMORY", {del::readyPhoto(s.userA)}))).empty());
            CHECK_FALSE(md::obs().client->execSqlSync("SELECT usapp_recovery_closed() AS v")[0]["v"].as<bool>());

            // Order 1: the export holds the shared lock; a closure waits for it and the export sees the open domain.
            auto release = pauseAfterLock();
            auto held = std::make_shared<ts::Pending<E::Result>>();
            E::exportFor(s.userB, [held](E::Result r) { held->deliver(std::move(r)); });
            const auto resume = release->wait();
            CHECK(resume.has_value());
            const char *info = std::getenv("USAPP_TEST_MAINT_CONNINFO");
            auto closer = drogon::orm::DbClient::newPgClient(info ? info : "", 1);
            md::sleepMs(200);
            auto closed = std::make_shared<ts::Pending<bool>>();
            closer->execSqlAsync("SELECT usapp_recovery_close(gen_random_uuid(), gen_random_uuid(), gen_random_uuid())",
                                 [closed](const drogon::orm::Result &) { closed->deliver(true); },
                                 [closed](const drogon::orm::DrogonDbException &) { closed->deliver(false); });
            md::waitUntil([] { return waitingSharedOrExclusive() == 1; });
            CHECK_FALSE(closed->readyWithin(std::chrono::milliseconds(300)));  // the closure waits for the export's transaction
            E::setAfterLockHookForTesting(nullptr);
            if (resume) (*resume)();
            const auto first = held->wait();
            CHECK(ok(first));
            md::waitUntil([] { return E::busySlotsForTesting() == 0; }, 20000);
            if (ok(first)) {
                const auto doc = parse(first->body);
                CHECK_EQ(doc["memories"]["state"].asString(), std::string("available"));
                CHECK(has(first->body, "E1-CLOSURE-MEMORY"));
            }
            const auto closedOk = closed->wait();
            CHECK(closedOk && *closedOk);
            CHECK(md::obs().client->execSqlSync("SELECT usapp_recovery_closed() AS v")[0]["v"].as<bool>());

            // Closed before generation: memory and album sections unavailable, every plan link withheld, nothing of the domain leaks.
            const auto after = exportOf(s.userB);
            CHECK(ok(after));
            if (ok(after)) {
                const auto doc = parse(after->body);
                CHECK_EQ(doc["memories"]["state"].asString(), std::string("unavailable"));
                CHECK_EQ(doc["albums"]["state"].asString(), std::string("unavailable"));
                CHECK_EQ(doc["memory_revisions"].size(), 0u);
                CHECK_EQ(doc["album_items"].size(), 0u);
                CHECK_FALSE(has(after->body, memory));
                CHECK_FALSE(has(after->body, "E1-CLOSURE-MEMORY"));
                for (const auto &p : doc["shared_plans"]) {
                    CHECK(p["memory_link"]["memory_id"].isNull());
                    CHECK_EQ(p["memory_link"]["withheld"].asString(), std::string("unavailable"));
                }
                CHECK(has(after->body, "PLAN-OWN-TEXT"));  // plans are not under the hold
            }

            // Order 2: a closure holds the lock exclusively; the export waits, then generates after it commits.
            Holder closure("SELECT usapp_recovery_close(gen_random_uuid(), gen_random_uuid(), gen_random_uuid())");
            auto waiting = std::make_shared<ts::Pending<E::Result>>();
            E::exportFor(s.userB, [waiting](E::Result r) { waiting->deliver(std::move(r)); });
            md::waitUntil([] { return waitingSharedOrExclusive() == 1; });
            CHECK_FALSE(waiting->readyWithin(std::chrono::milliseconds(300)));
            closure.commit();
            const auto second = waiting->wait();
            md::waitUntil([] { return E::busySlotsForTesting() == 0; }, 20000);
            CHECK(ok(second));
            if (ok(second)) CHECK_EQ(parse(second->body)["memories"]["state"].asString(), std::string("unavailable"));
            CHECK_EQ(c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM deletion_recovery_attempts WHERE $1::text IS NOT NULL", std::string("x")), 2);
        } catch (const std::exception &e) { c::expectThat(false, "export-closure", md::exceptionCategory(e)); }
    }
}

}  // namespace usapptest::exportdb
