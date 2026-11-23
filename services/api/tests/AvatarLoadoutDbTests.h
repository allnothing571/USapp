#pragma once

// Phase 4 B3 (0046_avatar_loadouts, docs/phase4-avatar-shop-design.md Section 7): saved loadouts and equip rules through the real
// AvatarLoadoutService and AvatarService on literal usapp_test. Paid items are bought through the real B2 purchase, funded by real
// task settlement. Fake registry rows only. Runs as its own CTest target (avatar_loadout_db), outside db_gated. The "no starter
// configuration" refusal is deterministic only on a fresh database: database/tests/avatar_loadout_test.sql covers it there.
#include <json/json.h>

#include <memory>
#include <optional>
#include <string>

#include "AvatarPurchaseDbTests.h"
#include "services/AvatarLoadoutService.h"

namespace usapptest::avatarloadout {

namespace av = usapptest::avatar;
namespace ap = usapptest::avatarpurchase;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using L = usapp::AvatarLoadoutService;
using LO = L::Outcome;
using S = usapp::AvatarService;

inline long long count(const std::string &sql, const std::string &a = "", const std::string &b = "") { return av::count(sql, a, b); }
inline long long events(const std::string &user) { return count("SELECT count(*)::bigint AS n FROM avatar_loadout_events WHERE user_id = $1::uuid", user); }
inline long long walletRows(const std::string &user) {
    return count("SELECT (SELECT count(*) FROM wallet_operations WHERE actor_user_id = $1::uuid) + "
                 "(SELECT count(*) FROM wallet_ledger_entries WHERE user_id = $1::uuid) AS n", user);
}

struct Slots {
    std::optional<std::string> hair, clothes, expression, hat, handheld, accessory;
};
inline L::Request request(int expected, const Slots &s) {
    L::Request r;
    r.expectedVersion = expected;
    r.slots = {s.hair, s.clothes, s.expression, s.hat, s.handheld, s.accessory};
    return r;
}
using Pending = ts::Pending<L::Result>;
inline std::shared_ptr<Pending> startSave(const std::string &user, const std::string &key, const L::Request &r, L::Fault f = L::Fault::None) {
    auto p = std::make_shared<Pending>();
    L::instance().save(user, key, r, [p](L::Result x) { p->deliver(std::move(x)); }, f);
    return p;
}
inline std::optional<L::Result> save(const std::string &user, const std::string &key, const L::Request &r, L::Fault f = L::Fault::None) {
    return startSave(user, key, r, f)->wait();
}
inline bool is(const std::optional<L::Result> &r, LO o) { return r.has_value() && r->outcome == o; }
inline Json::Value appearance(const std::string &user) {
    const auto r = av::readWith([&user](S::Callback cb) { S::instance().appearance(user, std::move(cb)); });
    if (!r || r->outcome != S::Outcome::Ok) throw std::runtime_error("appearance");
    return r->body["appearance"];
}
inline std::string slotId(const Json::Value &appearanceValue, const char *slot) {
    return appearanceValue["slots"][slot].isNull() ? std::string() : appearanceValue["slots"][slot]["id"].asString();
}

inline void runAvatarLoadoutDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    ap::Catalog cat;
    cat.admin = av::newAdmin(pool);

    TEST_CASE("Avatar B3: with no starter configuration a required-slot fallback fails closed (fresh database only)") {
        try {
            if (av::starterVersion() != 0) {
                std::printf("AVATAR_LOADOUT_DEFAULTS_CASE skipped: a configuration already exists\n");
            } else {
                const auto s = md::space();
                CHECK(is(save(s.userB, av::key(), request(0, {})), LO::DefaultsUnavailable));
                CHECK_EQ(events(s.userB), 0);
                std::printf("AVATAR_LOADOUT_DEFAULTS_CASE covered\n");
            }
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    // Shared fixtures: starters, one configuration, paid items bought by userB. A failure here fails the suite, never the process.
    std::string hair1, hair2, clothes1, smile, calm, hat, scarf, paidHair, balloon;
    int config = 0;
    ts::CommittedSpace s;
    const auto configure = [&](const std::string &hair, const std::string &expression) {
        const auto r = av::starter(cat.admin, av::key(), av::starterVersion(), hair, clothes1, expression);
        if (!av::is(r, S::Outcome::Created)) throw std::runtime_error("configure");
        return r->body["starter"]["version"].asInt();
    };
    try {
        hair1 = cat.starterItem("hair");
        hair2 = cat.starterItem("hair");
        clothes1 = cat.starterItem("clothes");
        smile = cat.starterItem("expression");
        calm = cat.starterItem("expression");
        config = configure(hair1, smile);
        s = md::space();
        ap::fund(s, 1000, 2);
        hat = cat.paid("hat", 30);
        scarf = cat.paid("accessory", 20);
        paidHair = cat.paid("hair", 40);
        balloon = cat.paid("handheld", 10);  // never bought
        for (const auto &item : {hat, scarf, paidHair}) {
            if (!ap::is(ap::buy(s.userB, av::key(), ap::request(item, 1)), ap::PO::Created)) throw std::runtime_error("buy");
        }
        if (!cat.revise(scarf, 1, 20, false)) throw std::runtime_error("unlist");
    } catch (const std::exception &) {
        CHECK(false);
        return;
    }

    TEST_CASE("Avatar B3: no saved loadout, then a save resolving required defaults, keeping an unlisted owned item, touching no wallet") {
        try {
            const auto before = appearance(s.userB);
            CHECK_EQ(before["source"].asString(), std::string("starter_defaults"));
            CHECK_EQ(before["loadout_version"].asInt(), 0);
            const auto wallet = walletRows(s.userB);
            const auto balance = ap::sunshineOf(s.userB);
            const auto k = av::key();
            auto first = request(0, {std::nullopt, clothes1, std::nullopt, hat, std::nullopt, scarf});
            first.characterGender = "female";
            first.hairHidden = true;
            const auto made = save(s.userB, k, first);
            CHECK(is(made, LO::Created));
            if (!is(made, LO::Created)) return;
            const auto &l = made->body["loadout"];
            CHECK(!made->body["replayed"].asBool());
            CHECK_EQ(l["version"].asInt(), 1);
            CHECK_EQ(l["character_gender"].asString(), std::string("female"));
            CHECK(l["hair_hidden"].asBool());
            CHECK_EQ(l["slots"]["hair"].asString(), hair1);
            CHECK_EQ(l["slots"]["expression"].asString(), smile);
            CHECK_EQ(l["slots"]["clothes"].asString(), clothes1);
            CHECK_EQ(l["slots"]["hat"].asString(), hat);
            CHECK_EQ(l["slots"]["accessory"].asString(), scarf);
            CHECK(l["slots"]["handheld"].isNull());
            CHECK(l["requested"]["hair"].isNull() && l["requested"]["expression"].isNull());
            CHECK_EQ(l["starter_version"].asInt(), config);
            const auto now = made->body["current"]["appearance"];
            CHECK_EQ(now["source"].asString(), std::string("saved_loadout"));
            CHECK_EQ(now["loadout_version"].asInt(), 1);
            CHECK_EQ(now["character_gender"].asString(), std::string("female"));
            CHECK(now["hair_hidden"].asBool());
            const auto replay = save(s.userB, k, first);
            CHECK(is(replay, LO::Replayed));
            first.characterGender = "male";
            CHECK(is(save(s.userB, k, first), LO::KeyReused));
            CHECK_EQ(slotId(now, "accessory"), scarf);
            CHECK_EQ(walletRows(s.userB), wallet);
            CHECK(ap::sunshineOf(s.userB) == balance);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.loadout_saved' AND target_id = $1", l["id"].asString()), 1);
            // Clearing an optional slot leaves it empty; naming every required slot needs no configuration.
            const auto cleared = save(s.userB, av::key(), request(1, {paidHair, clothes1, calm, std::nullopt, std::nullopt, scarf}));
            CHECK(is(cleared, LO::Created));
            CHECK(cleared && cleared->body["loadout"]["slots"]["hat"].isNull() && cleared->body["loadout"]["starter_version"].isNull());
            const auto after = appearance(s.userB);
            CHECK_EQ(after["loadout_version"].asInt(), 2);
            CHECK_EQ(after["character_gender"].asString(), std::string("female"));
            CHECK(after["hair_hidden"].asBool());
            CHECK(after["slots"]["hat"].isNull());
            CHECK_EQ(slotId(after, "hair"), paidHair);
            CHECK(after["starter_version"].isNull());
            // The partner's appearance is untouched.
            CHECK_EQ(appearance(s.userA)["source"].asString(), std::string("starter_defaults"));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B3: refusals save nothing: wrong category, unknown item, unbought, the partner's purchase, stale version, no couple") {
        try {
            const auto before = events(s.userB);
            const int current = static_cast<int>(before);
            CHECK(is(save(s.userB, av::key(), request(current, {hair1, clothes1, smile, clothes1, std::nullopt, std::nullopt})), LO::SlotInvalid));
            CHECK(is(save(s.userB, av::key(), request(current, {hair1, clothes1, smile, c::newUuid(), std::nullopt, std::nullopt})), LO::NotFound));
            CHECK(is(save(s.userB, av::key(), request(current, {hair1, clothes1, smile, std::nullopt, balloon, std::nullopt})), LO::NotOwned));
            CHECK(is(save(s.userB, av::key(), request(current + 5, {hair1, clothes1, smile})), LO::VersionConflict));
            CHECK_EQ(events(s.userB), before);
            // The partner cannot wear what userB bought.
            CHECK(is(save(s.userA, av::key(), request(0, {paidHair, clothes1, smile})), LO::NotOwned));
            CHECK(is(save(s.userA, av::key(), request(0, {hair1, clothes1, smile, hat})), LO::NotOwned));
            CHECK_EQ(events(s.userA), 0);
            const std::string loner = c::newUuid();
            ts::commitUser(pool, loner, "b3_loner");
            CHECK(is(save(loner, av::key(), request(0, {hair1, clothes1, smile})), LO::SpaceNotFound));
            // Request shape.
            L::Request out;
            std::string message;
            const auto rejects = [&](const std::string &text) {
                Json::Value body;
                Json::Reader().parse(text, body);
                return !L::parse(body, out, message);
            };
            const std::string six = R"("hair":null,"clothes":null,"expression":null,"hat":null,"handheld":null)";
            CHECK(rejects(R"({"expected_version":0,"slots":{)" + six + "}}"));
            CHECK(rejects(R"({"expected_version":0,"slots":{)" + six + R"(,"accessory":null,"wings":null}})"));
            CHECK(rejects(R"({"expected_version":0,"slots":{)" + six + R"(,"accessory":"x"}})"));
            CHECK(rejects(R"({"expected_version":-1,"slots":{)" + six + R"(,"accessory":null}})"));
            CHECK(rejects(R"({"expected_version":0,"slots":[],"user_id":"x"})"));
            CHECK(rejects(R"({"expected_version":0,"slots":{)" + six + R"(,"accessory":null},"user_id":")" + s.userA + R"("})"));
            CHECK(!rejects(R"({"expected_version":0,"slots":{)" + six + R"(,"accessory":null}})"));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B3: replays keep their captured defaults after a default change; an uncertain save resolves again") {
        try {
            const auto t = md::space();
            const auto k = av::key();
            const auto first = save(t.userB, k, request(0, {}));
            CHECK(is(first, LO::Created));
            if (!is(first, LO::Created)) return;
            CHECK_EQ(first->body["loadout"]["slots"]["hair"].asString(), hair1);
            config = configure(hair2, calm);
            // The saved loadout and its replay are unchanged by the new default.
            CHECK_EQ(slotId(appearance(t.userB), "hair"), hair1);
            const auto again = save(t.userB, k, request(0, {}));
            CHECK(is(again, LO::Replayed));
            CHECK(again && av::written(again->body["loadout"]) == av::written(first->body["loadout"]));
            CHECK_EQ(events(t.userB), 1);
            // Another request under the key is reuse; the identical request is still a replay.
            CHECK(is(save(t.userB, k, request(0, {hair2})), LO::KeyReused));
            CHECK(is(save(t.userB, k, request(1, {})), LO::KeyReused));
            // A new save with null required slots takes today's default.
            const auto next = save(t.userB, av::key(), request(1, {}));
            CHECK(is(next, LO::Created));
            CHECK(next && next->body["loadout"]["slots"]["hair"].asString() == hair2 && next->body["loadout"]["starter_version"].asInt() == config);
            // Uncertain: nothing was captured, so the retry resolves against the configuration current when it commits.
            const auto u = av::key();
            CHECK(is(save(t.userB, u, request(2, {}), L::Fault::FailBeforeCommit), LO::ResultUnknown));
            CHECK_EQ(events(t.userB), 2);
            config = configure(hair1, smile);
            const auto retried = save(t.userB, u, request(2, {}));
            CHECK(is(retried, LO::Created));
            CHECK(retried && retried->body["loadout"]["slots"]["hair"].asString() == hair1 && retried->body["loadout"]["starter_version"].asInt() == config);
            CHECK(is(save(t.userB, u, request(2, {})), LO::Replayed));
            CHECK_EQ(events(t.userB), 3);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.loadout_saved' AND actor_id = $1::uuid", t.userB), 3);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B3: concurrency: duplicate same-key requests, and two saves on the same version") {
        try {
            const auto t = md::space();
            const auto k = av::key();
            auto a = startSave(t.userB, k, request(0, {hair1, clothes1, smile}));
            auto b = startSave(t.userB, k, request(0, {hair1, clothes1, smile}));
            const auto ra = a->wait();
            const auto rb = b->wait();
            CHECK(ra && rb);
            if (!ra || !rb) return;
            CHECK((ra->outcome == LO::Created && rb->outcome == LO::Replayed) || (ra->outcome == LO::Replayed && rb->outcome == LO::Created));
            CHECK_EQ(events(t.userB), 1);
            auto x = startSave(t.userB, av::key(), request(1, {hair1, clothes1, calm}));
            auto y = startSave(t.userB, av::key(), request(1, {hair2, clothes1, smile}));
            const auto rx = x->wait();
            const auto ry = y->wait();
            CHECK(rx && ry);
            if (!rx || !ry) return;
            CHECK((rx->outcome == LO::Created) != (ry->outcome == LO::Created));
            CHECK(rx->outcome == LO::VersionConflict || ry->outcome == LO::VersionConflict);
            CHECK_EQ(events(t.userB), 2);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B3: a fallback save waits for a starter write in progress and resolves the new default; a full save does not wait") {
        c::Holder holder;
        try {
            const auto t = md::space();
            holder.conn = c::Observer::connect(dsn);
            holder.tx = c::acquireTx(holder.conn->client);
            const auto pid = c::exec(holder.tx, "SELECT pg_backend_pid() AS pid");
            CHECK(pid.ok);
            holder.pid = (*pid.result)[0]["pid"].as<int>();
            const auto next = av::starterVersion() + 1;
            const auto set = c::exec(holder.tx,
                "SELECT usapp_avatar_starter_set($1::uuid, $2, repeat('e', 64), $3::integer, $4::uuid, $5::uuid, $6::uuid)::text AS r",
                cat.admin, av::key(), std::to_string(next - 1), hair2, clothes1, calm);
            CHECK(set.ok && (*set.result)[0]["r"].as<std::string>().find("created") != std::string::npos);
            // A save naming every required slot needs no configuration and goes through while the starter write is open.
            CHECK(is(save(t.userA, av::key(), request(0, {hair1, clothes1, smile})), LO::Created));
            auto waiting = startSave(t.userB, av::key(), request(0, {}));
            CHECK(c::observeBlockedBy(av::obs(), holder.pid) != 0);
            CHECK(c::commitTx(holder.tx) == std::optional<bool>(true));
            const auto result = waiting->wait();
            CHECK(is(result, LO::Created));
            CHECK(result && result->body["loadout"]["slots"]["hair"].asString() == hair2 && result->body["loadout"]["starter_version"].asInt() == next);
            config = next;
            c::endHolder(av::obs(), holder, "b3-starter-holder");
        } catch (const std::exception &) {
            c::endHolder(av::obs(), holder, "b3-starter-holder");
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B3: the database refuses forged loadouts and never updates or deletes one") {
        try {
            const auto t = md::space();
            CHECK(is(save(t.userB, av::key(), request(0, {hair1, clothes1, smile})), LO::Created));
            const auto forge = [&](const std::string &user, int version, const std::string &hair, const std::string &requestedHair,
                                   const std::string &starter, bool rollBack = false) {
                Json::Value requested(Json::objectValue);
                requested["hair"] = requestedHair.empty() ? Json::Value(Json::nullValue) : Json::Value(requestedHair);
                requested["clothes"] = clothes1;
                requested["expression"] = smile;
                for (const char *slot : {"hat", "handheld", "accessory"}) requested[slot] = Json::nullValue;
                const auto requestedText = av::written(requested);
                const char *insert =
                    "INSERT INTO avatar_loadout_events (user_id, couple_id, version, hair_item_id, clothes_item_id, expression_item_id, requested, "
                    "starter_version, idempotency_key, request_fingerprint) "
                    "VALUES ($1::uuid, $2::uuid, $3::integer, $4::uuid, $5::uuid, $6::uuid, $7::jsonb, NULLIF($8, '')::integer, $9, repeat('a', 64))";
                if (rollBack) {
                    // Accepted or refused, inside a transaction that is rolled back: nothing unaudited is left behind.
                    auto conn = c::Observer::connect(dsn);
                    auto tx = c::acquireTx(conn.client);
                    if (!tx) return std::string("no transaction");
                    const auto done = c::exec(tx, insert, user, t.couple, std::to_string(version), hair, clothes1, smile, requestedText, starter, av::key());
                    c::rollbackTx(tx);
                    return done.ok ? std::string() : (done.error.empty() ? std::string("refused") : done.error);
                }
                return av::refusal([&] {
                    pool->execSqlSync(insert, user, t.couple, std::to_string(version), hair, clothes1, smile, requestedText, starter, av::key());
                });
            };
            // A starter hair that is not the current configuration's default, whichever that is now.
            const auto latest = av::starterVersion();
            const auto configured = av::obs().client->execSqlSync("SELECT hair_item_id::text AS h FROM avatar_starter_configs WHERE version = $1::integer",
                                                                  std::to_string(latest))[0]["h"].as<std::string>();
            const auto otherHair = configured == hair1 ? hair2 : hair1;
            // Another user's purchase, a version gap, a default that is not the recorded configuration's.
            CHECK(av::refusedWith(forge(t.userB, 2, paidHair, paidHair, ""), "USAPP_AVATAR_LOADOUT_ITEM"));
            CHECK(av::refusedWith(forge(t.userB, 4, hair1, hair1, ""), "USAPP_AVATAR_LOADOUT_VERSION"));
            CHECK(av::refusedWith(forge(t.userB, 2, otherHair, "", std::to_string(latest)), "USAPP_AVATAR_LOADOUT_DEFAULT"));
            CHECK(av::refusedWith(forge(t.userB, 2, configured, "", ""), "USAPP_AVATAR_LOADOUT_DEFAULT"));
            // The control: the same forged row with the configuration's own default is accepted by the guard (and rolled back).
            CHECK(forge(t.userB, 2, configured, "", std::to_string(latest), true).empty());
            CHECK_EQ(events(t.userB), 1);
            for (const char *sql : {"UPDATE avatar_loadout_events SET hat_item_id = NULL WHERE user_id = $1::uuid",
                                    "DELETE FROM avatar_loadout_events WHERE user_id = $1::uuid"}) {
                CHECK(av::refusedWith(av::refusal([&] { pool->execSqlSync(sql, t.userB); }), "permission denied"));
            }
            CHECK(av::refusedWith(av::refusal([&] { av::asMaintenance("DELETE FROM avatar_loadout_events WHERE user_id = '" + t.userB + "'"); }),
                                  "USAPP_AVATAR_APPEND_ONLY"));
            CHECK_EQ(events(t.userB), 1);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
}

}  // namespace usapptest::avatarloadout
