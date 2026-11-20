#pragma once

// P2-5a: real PushService on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test users, spaces and sessions); never resets.
// Endpoints are random per run, so committed generations of earlier runs never collide.
#include <string>
#include <vector>

#include "PushUnitTests.h"
#include "TaskServiceDbTests.h"
#include "core/Crypto.h"
#include "services/PushService.h"

namespace usapptest::push {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using PS = usapp::PushService;
using PO = PS::Outcome;

constexpr const char *kSecret = "p25a-db-gated-marker-secret-0000000";

inline std::string vapid(const char *label = "current") { return usapp::crypto::sha256Hex(std::string("p25a-") + label); }

struct Device {
    PS::Registration reg;
};

inline Device device(const char *host = "https://updates.push.services.mozilla.com/wpush/v2/") {
    return Device{{host + usapp::crypto::randomHex(12), pushunit::p256dh(), pushunit::authSecret()}};
}

inline std::string commitSession(const c::ClientPtr &pool, const std::string &user) {
    const auto rows = pool->execSqlSync(
        "INSERT INTO sessions (token_hash, csrf_token_hash, user_id, is_authenticated, user_agent_family, expires_at) "
        "VALUES ($1, $2, $3::uuid, true, 'chrome', now() + interval '1 day') RETURNING id::text AS id",
        usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)), usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)),
        user);
    return rows[0]["id"].as<std::string>();
}
inline void expireSession(const c::ClientPtr &pool, const std::string &session) {
    pool->execSqlSync("UPDATE sessions SET expires_at = created_at + interval '1 microsecond' WHERE id = $1::uuid",
                      session);
}
inline void logoutSession(const c::ClientPtr &pool, const std::string &session) {
    pool->execSqlSync("UPDATE sessions SET revoked_at = now(), revoked_reason = 'logout' WHERE id = $1::uuid", session);
}

inline std::shared_ptr<ts::Pending<PS::Status>> startRegister(const std::string &user, const std::string &session,
                                                              const std::string &key, const PS::Registration &reg,
                                                              const std::string &keyId = vapid()) {
    auto pending = std::make_shared<ts::Pending<PS::Status>>();
    PS::instance().registerSubscription(user, session, key, reg, keyId, kSecret,
                                        [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<PS::Status> reg(const std::string &user, const std::string &session, const std::string &key,
                                     const PS::Registration &r, const std::string &keyId = vapid()) {
    return startRegister(user, session, key, r, keyId)->wait();
}
inline std::optional<PS::Status> revoke(const std::string &user, const std::string &session, const std::string &id) {
    auto pending = std::make_shared<ts::Pending<PS::Status>>();
    PS::instance().revoke(user, session, id, vapid(), [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<PS::ListResult> list(const std::string &user, const std::string &session) {
    auto pending = std::make_shared<ts::Pending<PS::ListResult>>();
    PS::instance().list(user, session, vapid(), [pending](PS::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
inline bool is(const std::optional<PS::Status> &s, PO o) { return s.has_value() && s->outcome == o; }
inline std::string gen(const std::optional<PS::Status> &s) { return s ? s->generation.id : std::string(); }
inline std::string cmdGen(const std::optional<PS::Status> &s) {
    return s ? s->command["generation_id"].asString() : std::string();
}
inline std::string cmdResult(const std::optional<PS::Status> &s) {
    return s ? s->command["result"].asString() : std::string();
}
inline long long n(const c::Observer &o, const std::string &sql, const std::string &a, const std::string &b = "") {
    return c::scalar(o, sql, a, b);
}
inline long long generations(const c::Observer &o, const std::string &user) {
    return n(o, "SELECT count(*)::bigint AS n FROM push_subscriptions WHERE user_id = $1::uuid", user);
}
inline long long commands(const c::Observer &o, const std::string &user) {
    return n(o, "SELECT count(*)::bigint AS n FROM push_registration_commands WHERE user_id = $1::uuid", user);
}
inline long long counted(const std::optional<PS::ListResult> &r) {
    return r ? static_cast<long long>(r->devices.size()) : -1;
}
inline std::string stateOf(const std::optional<PS::ListResult> &r, const std::string &id) {
    if (r) for (const auto &g : r->devices) if (g.id == id) return g.state;
    if (r) for (const auto &g : r->history) if (g.id == id) return g.state;
    return "missing";
}
inline bool inDevices(const std::optional<PS::ListResult> &r, const std::string &id) {
    if (r) for (const auto &g : r->devices) if (g.id == id) return true;
    return false;
}
inline std::string revokedReason(const c::Observer &o, const std::string &id) {
    const auto rows = o.client->execSqlSync(
        "SELECT reason || '|' || coalesce(caused_by::text, '') || '|' || revoked_by::text AS r "
        "FROM push_subscription_revocations WHERE subscription_id = $1::uuid", id);
    return rows.size() == 1u ? rows[0]["r"].as<std::string>() : std::string("none");
}
inline long long activeOnEndpoint(const c::Observer &o, const std::string &endpoint) {
    return n(o,
             "SELECT count(*)::bigint AS n FROM push_subscriptions g WHERE g.endpoint_hash = $1 AND NOT EXISTS "
             "(SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id)",
             PS::endpointHash(endpoint));
}

inline void runPushDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);

    TEST_CASE("P2-5a: K1/G1 -> K2 already_active -> revoke G1 -> K3/G3 -> retry K2 changes nothing") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA);
            const auto d = device();
            const auto k1 = c::newUuid(), k2 = c::newUuid(), k3 = c::newUuid();

            const auto r1 = reg(s.userA, sa, k1, d.reg);
            CHECK(is(r1, PO::Created));
            const std::string g1 = gen(r1);
            CHECK_EQ(cmdResult(r1), std::string("created"));
            CHECK_EQ(cmdGen(r1), g1);
            if (r1) {
                CHECK_EQ(r1->generation.state, std::string("active"));
                CHECK(r1->generation.currentSession);
                CHECK_EQ(r1->generation.endpointHash, PS::endpointHash(d.reg.endpoint));
                CHECK_EQ(r1->generation.vapidKeyId, vapid());
                CHECK_EQ(r1->marker, PS::markerFor(kSecret, g1));
                const auto out = PS::responseBody(*r1);
                CHECK_FALSE(out["generation"].isMember("endpoint"));
                CHECK(out.toStyledString().find(d.reg.endpoint) == std::string::npos);
                CHECK(out.toStyledString().find(d.reg.p256dh) == std::string::npos);
            }

            const auto r2 = reg(s.userA, sa, k2, d.reg);
            CHECK(is(r2, PO::Ok));
            CHECK_EQ(cmdResult(r2), std::string("already_active"));
            CHECK_EQ(cmdGen(r2), g1);
            CHECK_EQ(generations(observer, s.userA), 1);
            CHECK_EQ(commands(observer, s.userA), 2);

            const auto v1 = revoke(s.userA, sa, g1);
            CHECK(is(v1, PO::Ok));
            if (v1) {
                CHECK_EQ(v1->revocation["reason"].asString(), std::string("user"));
                CHECK_EQ(v1->generation.state, std::string("revoked"));
            }
            const auto v1again = revoke(s.userA, sa, g1);
            CHECK(is(v1again, PO::Replayed));
            if (v1 && v1again) {
                CHECK_EQ(v1again->revocation["revoked_at"].asString(), v1->revocation["revoked_at"].asString());
            }

            const auto r3 = reg(s.userA, sa, k3, d.reg);
            CHECK(is(r3, PO::Created));
            const std::string g3 = gen(r3);
            CHECK(g3 != g1);

            // Retry K2: the original already_active facts, G1's current state; nothing else moves.
            const auto r2again = reg(s.userA, sa, k2, d.reg);
            CHECK(is(r2again, PO::Replayed));
            CHECK_EQ(cmdResult(r2again), std::string("already_active"));
            CHECK_EQ(cmdGen(r2again), g1);
            if (r2 && r2again) {
                CHECK_EQ(r2again->command["recorded_at"].asString(), r2->command["recorded_at"].asString());
                CHECK_EQ(r2again->generation.state, std::string("revoked"));
                CHECK(r2again->marker.empty());
            }
            CHECK_EQ(revokedReason(observer, g3), std::string("none"));
            CHECK_EQ(revokedReason(observer, g1), std::string("user||") + s.userA);
            CHECK_EQ(generations(observer, s.userA), 2);
            CHECK_EQ(commands(observer, s.userA), 3);
            // Retry K1 replays created/G1 (revoked), retry K3 replays G3 (active).
            CHECK_EQ(reg(s.userA, sa, k1, d.reg)->generation.state, std::string("revoked"));
            const auto r3again = reg(s.userA, sa, k3, d.reg);
            CHECK(is(r3again, PO::Replayed));
            if (r3again) CHECK_EQ(r3again->marker, PS::markerFor(kSecret, g3));
            const auto listed = list(s.userA, sa);
            CHECK_EQ(stateOf(listed, g1), std::string("revoked"));
            CHECK_EQ(stateOf(listed, g3), std::string("active"));
            CHECK_EQ(counted(listed), 1);
            CHECK_EQ(n(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid AND action = "
                                 "'push.subscription_registered' AND strpos(detail::text, $2) = 0",
                       s.userA, d.reg.endpoint), 3);
            CHECK_EQ(n(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid AND action = "
                                 "'push.subscription_revoked'", s.userA), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-sequence", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: same-key races replay one command; a reused key with another payload is rejected") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA);
            const auto sa2 = commitSession(pool, s.userA);
            const auto d = device("https://fcm.googleapis.com/fcm/send/");
            const auto key = c::newUuid();
            auto a = startRegister(s.userA, sa, key, d.reg), b = startRegister(s.userA, sa, key, d.reg);
            const auto x = a->wait(), y = b->wait();
            CHECK(((is(x, PO::Created) && is(y, PO::Replayed)) || (is(y, PO::Created) && is(x, PO::Replayed))));
            CHECK_EQ(cmdGen(x), cmdGen(y));
            CHECK_EQ(generations(observer, s.userA), 1);
            CHECK_EQ(commands(observer, s.userA), 1);
            PS::Registration other = d.reg;
            other.auth = pushunit::authSecret();
            CHECK(is(reg(s.userA, sa, key, other), PO::KeyReused));
            CHECK(is(reg(s.userA, sa2, key, d.reg), PO::KeyReused));  // another session differs
            CHECK(is(reg(s.userA, sa, key, d.reg, vapid("rotated")), PO::KeyReused));
            CHECK_EQ(generations(observer, s.userA), 1);
            CHECK_EQ(commands(observer, s.userA), 1);
            if (x) CHECK_EQ(x->generation.provider, std::string("fcm"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-same-key", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: device cap; concurrent distinct endpoints at cap-1; replacement at the cap") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA);
            std::vector<Device> devices;
            for (int i = 0; i < PS::kMaxActiveDevices - 1; ++i) {
                devices.push_back(device());
                CHECK(is(reg(s.userA, sa, c::newUuid(), devices.back().reg), PO::Created));
            }
            CHECK_EQ(counted(list(s.userA, sa)), PS::kMaxActiveDevices - 1);
            const auto e1 = device(), e2 = device("https://web.push.apple.com/");
            auto p = startRegister(s.userA, sa, c::newUuid(), e1.reg), q = startRegister(s.userA, sa, c::newUuid(), e2.reg);
            const auto p1 = p->wait(), q1 = q->wait();
            CHECK(((is(p1, PO::Created) && is(q1, PO::DeviceLimit)) || (is(q1, PO::Created) && is(p1, PO::DeviceLimit))));
            CHECK_EQ(counted(list(s.userA, sa)), PS::kMaxActiveDevices);
            CHECK_EQ(generations(observer, s.userA), PS::kMaxActiveDevices);
            CHECK_EQ(commands(observer, s.userA), PS::kMaxActiveDevices);  // a rejected key is not accepted
            CHECK(is(reg(s.userA, sa, c::newUuid(), device().reg), PO::DeviceLimit));

            // At the cap: same endpoint, new keys -> replaced; count stays at the cap.
            const std::string old = [&]() {
                const auto r = reg(s.userA, sa, c::newUuid(), devices[0].reg);  // already_active probe
                return cmdGen(r);
            }();
            PS::Registration rekeyed = devices[0].reg;
            rekeyed.p256dh = pushunit::p256dhOtherThan(rekeyed.p256dh);
            const auto replaced = reg(s.userA, sa, c::newUuid(), rekeyed);
            CHECK(is(replaced, PO::Created));
            CHECK_EQ(revokedReason(observer, old), "replaced|" + gen(replaced) + "|" + s.userA);
            CHECK_EQ(counted(list(s.userA, sa)), PS::kMaxActiveDevices);
            // At the cap: a new session of the same user on the same endpoint -> superseded.
            const auto sa2 = commitSession(pool, s.userA);
            const auto superseded = reg(s.userA, sa2, c::newUuid(), rekeyed);
            CHECK(is(superseded, PO::Created));
            CHECK_EQ(revokedReason(observer, gen(replaced)), "superseded|" + gen(superseded) + "|" + s.userA);
            CHECK_EQ(counted(list(s.userA, sa)), PS::kMaxActiveDevices);
            // A new VAPID key id on the same endpoint and session -> replaced.
            const auto rotated = reg(s.userA, sa2, c::newUuid(), rekeyed, vapid("rotated"));
            CHECK(is(rotated, PO::Created));
            CHECK_EQ(revokedReason(observer, gen(superseded)), "replaced|" + gen(rotated) + "|" + s.userA);
            // Active under the key id that call was configured with; inactive under the current key.
            if (rotated) CHECK_EQ(rotated->generation.state, std::string("active"));
            CHECK_EQ(stateOf(list(s.userA, sa2), gen(rotated)), std::string("inactive"));
            CHECK_EQ(counted(list(s.userA, sa)), PS::kMaxActiveDevices);
            // Revoking one frees a slot.
            CHECK(is(revoke(s.userA, sa, gen(rotated)), PO::Ok));
            CHECK(is(reg(s.userA, sa, c::newUuid(), device().reg), PO::Created));
        } catch (const std::exception &) {
            c::expectThat(false, "push-cap", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: re-own at the caller's cap is rejected and harmless; below the cap it re-owns") {
        try {
            ts::CommittedSpace owner, full;
            ts::commitSpace(pool, owner, true);
            ts::commitSpace(pool, full, true);
            const auto so = commitSession(pool, owner.userA);
            const auto sf = commitSession(pool, full.userA);
            const auto shared = device("https://db5p.notify.windows.com/w/?token=");
            const auto first = reg(owner.userA, so, c::newUuid(), shared.reg);
            CHECK(is(first, PO::Created));
            for (int i = 0; i < PS::kMaxActiveDevices; ++i) {
                CHECK(is(reg(full.userA, sf, c::newUuid(), device().reg), PO::Created));
            }
            const auto before = commands(observer, full.userA);
            CHECK(is(reg(full.userA, sf, c::newUuid(), shared.reg), PO::DeviceLimit));
            CHECK_EQ(revokedReason(observer, gen(first)), std::string("none"));
            CHECK_EQ(stateOf(list(owner.userA, so), gen(first)), std::string("active"));
            CHECK_EQ(commands(observer, full.userA), before);

            // The partner (another user, below the cap) re-owns the shared browser endpoint.
            const auto sb = commitSession(pool, owner.userB);
            const auto reowned = reg(owner.userB, sb, c::newUuid(), shared.reg);
            CHECK(is(reowned, PO::Created));
            CHECK_EQ(revokedReason(observer, gen(first)), "reowned|" + gen(reowned) + "|" + owner.userB);
            CHECK_EQ(stateOf(list(owner.userA, so), gen(first)), std::string("revoked"));
            CHECK_EQ(activeOnEndpoint(observer, shared.reg.endpoint), 1);
            // Cross-user revoke: another user's generation is not found, and nothing changes.
            CHECK(is(revoke(owner.userA, so, gen(reowned)), PO::NotFound));
            CHECK(is(revoke(full.userA, sf, gen(reowned)), PO::NotFound));
            CHECK_EQ(revokedReason(observer, gen(reowned)), std::string("none"));
            CHECK_EQ(stateOf(list(full.userA, sf), gen(reowned)), std::string("missing"));
            // Revoke of an already re-owned generation returns that original revocation.
            const auto replay = revoke(owner.userA, so, gen(first));
            CHECK(is(replay, PO::Replayed));
            if (replay) CHECK_EQ(replay->revocation["reason"].asString(), std::string("reowned"));
        } catch (const std::exception &) {
            c::expectThat(false, "push-reown", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: concurrent re-own of one endpoint leaves exactly one active generation") {
        try {
            ts::CommittedSpace a, b;
            ts::commitSpace(pool, a, true);
            ts::commitSpace(pool, b, true);
            const auto sa = commitSession(pool, a.userA), sb = commitSession(pool, b.userA);
            const auto d = device();
            auto x = startRegister(a.userA, sa, c::newUuid(), d.reg), y = startRegister(b.userA, sb, c::newUuid(), d.reg);
            const auto x1 = x->wait(), y1 = y->wait();
            CHECK(is(x1, PO::Created));
            CHECK(is(y1, PO::Created));
            CHECK_EQ(activeOnEndpoint(observer, d.reg.endpoint), 1);
            CHECK_EQ(n(observer, "SELECT count(*)::bigint AS n FROM push_subscription_revocations r JOIN "
                                 "push_subscriptions g ON g.id = r.subscription_id WHERE g.endpoint_hash = $1 "
                                 "AND r.reason = 'reowned'", PS::endpointHash(d.reg.endpoint)), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "push-reown-race", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: logout and expiry deactivate without writes; ended sessions cannot register") {
        try {
            ts::CommittedSpace s, lonely;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA), sx = commitSession(pool, s.userA);
            const auto d1 = device(), d2 = device();
            const auto g1 = reg(s.userA, sa, c::newUuid(), d1.reg), g2 = reg(s.userA, sx, c::newUuid(), d2.reg);
            CHECK(is(g1, PO::Created));
            CHECK(is(g2, PO::Created));
            const auto sNew = commitSession(pool, s.userA);
            const auto writesBefore =
                n(observer, "SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE user_id = $1::uuid",
                  s.userA);
            logoutSession(pool, sa);
            expireSession(pool, sx);
            const auto after = list(s.userA, sNew);
            CHECK_EQ(stateOf(after, gen(g1)), std::string("inactive"));
            CHECK_EQ(stateOf(after, gen(g2)), std::string("inactive"));
            CHECK_EQ(counted(after), 0);
            CHECK_EQ(n(observer, "SELECT count(*)::bigint AS n FROM push_subscription_revocations WHERE user_id = "
                                 "$1::uuid", s.userA), writesBefore);
            CHECK(is(reg(s.userA, sa, c::newUuid(), device().reg), PO::SessionInvalid));
            CHECK(is(reg(s.userA, sx, c::newUuid(), device().reg), PO::SessionInvalid));
            CHECK(is(revoke(s.userA, sx, gen(g2)), PO::SessionInvalid));
            // A fresh session on the same endpoint and keys supersedes the inactive generation.
            const auto again = reg(s.userA, sNew, c::newUuid(), d1.reg);
            CHECK(is(again, PO::Created));
            CHECK_EQ(revokedReason(observer, gen(g1)), "superseded|" + gen(again) + "|" + s.userA);
            CHECK_EQ(counted(list(s.userA, sNew)), 1);
            // No space: not found, nothing written.
            ts::commitUser(pool, lonely.userA, "push_lonely");
            const auto sl = commitSession(pool, lonely.userA);
            CHECK(is(reg(lonely.userA, sl, c::newUuid(), device().reg), PO::SpaceNotFound));
            CHECK_EQ(generations(observer, lonely.userA), 0);
            // Another user's session cannot be used.
            CHECK(is(reg(s.userB, sNew, c::newUuid(), device().reg), PO::SessionInvalid));
        } catch (const std::exception &) {
            c::expectThat(false, "push-session", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a review R2: every slot-occupying device stays listed and revocable behind long history") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA);
            // Two older counted generations: one under the current key, one under an old key.
            const auto kept = reg(s.userA, sa, c::newUuid(), device().reg);
            const auto oldKey = reg(s.userA, sa, c::newUuid(), device().reg, vapid("old"));
            CHECK(is(kept, PO::Created));
            CHECK(is(oldKey, PO::Created));
            // More than kHistoryLimit newer generations, each created and then revoked.
            for (int i = 0; i < PS::kHistoryLimit + 3; ++i) {
                const auto newer = reg(s.userA, sa, c::newUuid(), device().reg);
                CHECK(is(newer, PO::Created));
                CHECK(is(revoke(s.userA, sa, gen(newer)), PO::Ok));
            }
            const auto listed = list(s.userA, sa);
            CHECK(listed.has_value());
            if (listed) {
                CHECK_EQ(listed->devices.size(), 2u);
                CHECK_EQ(listed->history.size(), static_cast<size_t>(PS::kHistoryLimit));
                for (const auto &g : listed->history) CHECK_EQ(g.state, std::string("revoked"));
            }
            CHECK(inDevices(listed, gen(kept)));
            CHECK(inDevices(listed, gen(oldKey)));
            CHECK_EQ(stateOf(listed, gen(kept)), std::string("active"));
            CHECK_EQ(stateOf(listed, gen(oldKey)), std::string("inactive"));  // occupies a slot, old key
            CHECK_EQ(counted(listed), 2);
            // Both are revocable from what the list exposes.
            CHECK(is(revoke(s.userA, sa, gen(oldKey)), PO::Ok));
            CHECK(is(revoke(s.userA, sa, gen(kept)), PO::Ok));
            const auto after = list(s.userA, sa);
            CHECK_EQ(counted(after), 0);
            CHECK_FALSE(inDevices(after, gen(kept)));
            // An ended session's unrevoked generation leaves the device bucket (it frees its slot).
            const auto sx = commitSession(pool, s.userA);
            const auto ended = reg(s.userA, sx, c::newUuid(), device().reg);
            CHECK(is(ended, PO::Created));
            CHECK(inDevices(list(s.userA, sa), gen(ended)));
            logoutSession(pool, sx);
            CHECK_FALSE(inDevices(list(s.userA, sa), gen(ended)));
        } catch (const std::exception &) {
            c::expectThat(false, "push-list-coverage", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-5a: linkage is atomic and the tables are append-only for usapp_api") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto sa = commitSession(pool, s.userA);
            const auto d = device();
            const auto first = reg(s.userA, sa, c::newUuid(), d.reg);
            PS::Registration rekeyed = d.reg;
            rekeyed.auth = pushunit::authSecret();
            const auto second = reg(s.userA, sa, c::newUuid(), rekeyed);
            CHECK(is(second, PO::Created));
            // Each generation has exactly one creating command; the predecessor's revocation names it.
            CHECK_EQ(n(observer, "SELECT count(*)::bigint AS n FROM push_subscriptions g WHERE g.user_id = $1::uuid AND "
                                 "(SELECT count(*) FROM push_registration_commands c WHERE c.generation_id = g.id AND "
                                 "c.result = 'created') <> 1", s.userA), 0);
            CHECK_EQ(revokedReason(observer, gen(first)), "replaced|" + gen(second) + "|" + s.userA);
            // A generation without its creating command fails the deferred link and leaves nothing.
            // The link is made immediate inside the statement: in pipeline mode Drogon does not
            // surface a commit-time error of an implicit transaction (observed in the first run).
            bool rejected = false;
            try {
                const std::string endpoint = device().reg.endpoint;
                pool->execSqlSync(
                    "DO $orphan$ BEGIN SET CONSTRAINTS ALL IMMEDIATE; INSERT INTO push_subscriptions (user_id, "
                    "couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id) VALUES ('" + s.userA +
                    "', '" + s.couple + "', '" + sa + "', '" + endpoint + "', encode(sha256(convert_to('" + endpoint +
                    "', 'UTF8')), 'hex'), '" + pushunit::p256dh() + "', '" + pushunit::authSecret() + "', '" + vapid() +
                    "'); END $orphan$");
            } catch (const std::exception &) {
                rejected = true;
            }
            CHECK(rejected);
            CHECK_EQ(generations(observer, s.userA), 2);
            for (const char *sql : {"UPDATE push_subscriptions SET auth = auth WHERE user_id = $1::uuid",
                                    "DELETE FROM push_registration_commands WHERE user_id = $1::uuid",
                                    "DELETE FROM push_subscription_revocations WHERE user_id = $1::uuid"}) {
                bool denied = false;
                try {
                    pool->execSqlSync(sql, s.userA);
                } catch (const std::exception &) {
                    denied = true;
                }
                CHECK(denied);
            }
            CHECK_EQ(generations(observer, s.userA), 2);
            CHECK_EQ(commands(observer, s.userA), 2);
        } catch (const std::exception &) {
            c::expectThat(false, "push-linkage", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::push
