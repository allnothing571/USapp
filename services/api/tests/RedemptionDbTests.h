#pragma once

// M8: real RedemptionService on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test spaces, funded through real task
// completions); never resets. Sunshine is never touched by redemption.
#include "ProductDbTests.h"
#include "TaskChangeDbTests.h"
#include "services/RedemptionService.h"

namespace usapptest::redemption {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace pr = usapptest::product;
namespace tc = usapptest::taskchange;
using RS = usapp::RedemptionService;
using RO = RS::Outcome;
using PO = usapp::ProductService::Outcome;
using PendingRedemption = ts::Pending<RS::Status>;

inline std::shared_ptr<PendingRedemption> startRedeem(const std::string &user, const std::string &product, int version,
                                                      const std::string &key) {
    auto pending = std::make_shared<PendingRedemption>();
    RS::instance().redeem(user, product, key, version, [pending](RS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<RS::Status> redeem(const std::string &user, const std::string &product, int version,
                                        const std::string &key = c::newUuid()) {
    return startRedeem(user, product, version, key)->wait();
}
inline std::optional<RS::Status> fulfill(const std::string &user, const std::string &redemption,
                                         const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingRedemption>();
    RS::instance().fulfill(user, redemption, key, [pending](RS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<RS::Status> get(const std::string &user, const std::string &id) {
    auto pending = std::make_shared<PendingRedemption>();
    RS::instance().get(user, id, [pending](RS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<RS::ListResult> list(const std::string &user, int limit = 100,
                                          const std::optional<RS::Cursor> &cursor = std::nullopt) {
    auto pending = std::make_shared<ts::Pending<RS::ListResult>>();
    RS::instance().list(user, limit, cursor, [pending](RS::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
inline std::vector<std::string> allPages(const std::string &user, int limit, int &pages) {
    std::vector<std::string> ids;
    std::optional<RS::Cursor> cursor;
    pages = 0;
    for (int i = 0; i < 10; ++i) {
        const auto r = list(user, limit, cursor);
        if (!r || r->outcome != RO::Ok) throw std::runtime_error("page failed");
        ++pages;
        for (const auto &d : r->redemptions) ids.push_back(d.id);
        if (!r->nextCursor) return ids;
        cursor = RS::decodeCursor(*r->nextCursor);
        if (!cursor) throw std::runtime_error("bad next cursor");
    }
    throw std::runtime_error("too many pages");
}
inline bool is(const std::optional<RS::Status> &s, RO o) { return s.has_value() && s->outcome == o; }
inline std::string facts(const RS::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, s.mutation);
}
// Funds the recipient through a real single-task completion (creator publishes, recipient completes).
inline void fund(const ts::CommittedSpace &s, int flowers) {
    const auto task = c::publish(s.userA, "M8 fund", flowers, 0);
    if (!c::is(c::runComplete(s.userB, task, c::newUuid()), c::Outcome::Completed)) {
        throw std::runtime_error("fixture funding failed");
    }
}
inline long long flowers(const std::string &user) {
    const auto w = c::walletOf(user);
    if (!w) throw std::runtime_error("wallet read failed");
    return w->flower;
}
inline long long count(const c::Observer &o, const std::string &sql, const std::string &id) {
    return o.client->execSqlSync(sql, id)[0]["n"].as<long long>();
}
// Redemption side effects of one redeemer: operations, entries, rows, audits, sunshine entries.
struct Effects {
    long long ops = 0, entries = 0, rows = 0, audits = 0, sunshine = 0;
    bool operator==(const Effects &o) const {
        return ops == o.ops && entries == o.entries && rows == o.rows && audits == o.audits && sunshine == o.sunshine;
    }
};
inline Effects effects(const c::Observer &o, const std::string &user) {
    Effects e;
    e.ops = count(o, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE actor_user_id = $1::uuid "
                     "AND operation_type = 'REWARD_REDEMPTION'", user);
    e.entries = count(o, "SELECT count(*)::bigint AS n FROM wallet_ledger_entries e JOIN wallet_operations w "
                         "ON w.id = e.operation_id WHERE e.user_id = $1::uuid AND w.operation_type = 'REWARD_REDEMPTION'",
                      user);
    e.rows = count(o, "SELECT count(*)::bigint AS n FROM reward_redemptions WHERE redeemer_id = $1::uuid", user);
    e.audits = count(o, "SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid "
                        "AND action = 'reward_redemption.redeemed'", user);
    e.sunshine = count(o, "SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE user_id = $1::uuid "
                          "AND asset_type = 'sunshine'", user);
    return e;
}
// Dedicated holder of the per-product advisory lock shared with M7 edit/archive.
inline c::Holder lockProduct(const std::string &dsn, const std::string &productId) {
    c::Holder h;
    h.conn = c::Observer::connect(dsn);
    h.tx = c::acquireTx(h.conn->client);
    if (!h.tx) throw std::runtime_error("holder acquisition failed");
    const auto pid = c::exec(h.tx, "SELECT pg_backend_pid() AS pid");
    if (!pid.ok || !pid.result) throw std::runtime_error("holder pid failed");
    h.pid = (*pid.result)[0]["pid"].as<int>();
    const auto lock = c::exec(h.tx, "SELECT pg_advisory_xact_lock(33007::integer, hashtext($1::text))", productId);
    if (!lock.ok) throw std::runtime_error("holder lock failed");
    return h;
}

inline void runRedemptionDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);

    TEST_CASE("M8: owner redeems once (debit, snapshot, audit); creator alone fulfils; no sunshine or refunds") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            fund(s, 10);
            CHECK_EQ(flowers(s.userB), 10);
            const auto made = pr::create(s.userA, pr::terms("M8 movie", 4));
            const auto request = pr::create(s.userA, pr::terms("M8 request", 1, "request"));
            const std::string pid = made ? made->product.id : "";
            const std::string rid = request ? request->product.id : "";
            const auto before = effects(observer, s.userB);

            CHECK(is(redeem(s.userA, pid, 1), RO::Forbidden));         // creator cannot redeem
            CHECK(is(redeem(other.userB, pid, 1), RO::ProductNotFound));  // other space
            CHECK(is(redeem(s.userB, rid, 2), RO::VersionConflict));   // M9 accepts request mode; stale versions still fail
            CHECK(is(redeem(s.userB, pid, 2), RO::VersionConflict));
            CHECK(effects(observer, s.userB) == before);

            const auto key = c::newUuid();
            const auto done = redeem(s.userB, pid, 1, key);
            CHECK(is(done, RO::Redeemed));
            const std::string id = done ? done->redemption.id : "";
            if (done) {
                CHECK_EQ(done->redemption.status, std::string("pending_fulfillment"));
                CHECK_EQ(done->redemption.priceFlowers, 4);
                CHECK_EQ(done->redemption.creatorId, s.userA);
                CHECK_EQ(done->mutation["action"].asString(), std::string("redeem"));
                CHECK_EQ(done->mutation["debited_flowers"].asInt(), 4);
                CHECK_FALSE(done->mutation.isMember("balance"));
            }
            CHECK_EQ(flowers(s.userB), 6);
            auto after = before;
            after.ops += 1;
            after.entries += 1;
            after.rows += 1;
            after.audits += 1;
            CHECK(effects(observer, s.userB) == after);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM wallet_ledger_entries e JOIN reward_redemptions d "
                                     "ON d.operation_id = e.operation_id WHERE d.id = $1::uuid AND e.amount = -4", id), 1);

            // Replay is exactly-once (no second debit); a different body under the key is rejected.
            const auto replay = redeem(s.userB, pid, 1, key);
            CHECK(is(replay, RO::Replayed));
            if (replay && done) CHECK_EQ(facts(*replay), facts(*done));
            CHECK(is(redeem(s.userB, rid, 1, key), RO::KeyReused));
            CHECK_EQ(flowers(s.userB), 6);

            // Fulfilment: only the creator, once; no wallet change for either party.
            const long long creatorFlowers = flowers(s.userA);
            CHECK(is(fulfill(s.userB, id), RO::Forbidden));
            CHECK(is(fulfill(other.userA, id), RO::NotFound));
            CHECK(is(get(other.userA, id), RO::NotFound));
            const auto fkey = c::newUuid();
            const auto fulfilled = fulfill(s.userA, id, fkey);
            CHECK(is(fulfilled, RO::Fulfilled));
            if (fulfilled) {
                CHECK_EQ(fulfilled->redemption.status, std::string("fulfilled"));
                CHECK_EQ(fulfilled->redemption.fulfilledBy, s.userA);
            }
            const auto again = fulfill(s.userA, id, fkey);
            CHECK(is(again, RO::Replayed));
            if (again && fulfilled) CHECK_EQ(facts(*again), facts(*fulfilled));
            CHECK(is(fulfill(s.userA, id), RO::AlreadyFulfilled));
            CHECK_EQ(flowers(s.userB), 6);
            CHECK_EQ(flowers(s.userA), creatorFlowers);
            CHECK(effects(observer, s.userB) == after);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = "
                                     "'reward_redemption.fulfilled' AND target_id = $1::text", id), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id = $1::uuid "
                                     "AND operation_type = 'REDEMPTION_REFUND'", s.couple), 0);
            const auto seen = get(s.userB, id);
            CHECK(is(seen, RO::Ok));
            if (seen) CHECK_EQ(seen->redemption.status, std::string("fulfilled"));
        } catch (const std::exception &) { c::expectThat(false, "m8-redeem", "case failed (details withheld)"); }
    }

    TEST_CASE("M8: snapshots stay fixed after edit/archive; queued edit/archive make a stale redeem fail without a debit") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            fund(s, 20);
            const auto made = pr::create(s.userA, pr::terms("M8 snap v1", 3));
            const std::string pid = made ? made->product.id : "";
            const auto key = c::newUuid();
            const auto done = redeem(s.userB, pid, 1, key);
            CHECK(is(done, RO::Redeemed));
            const std::string id = done ? done->redemption.id : "";
            CHECK(pr::is(pr::edit(s.userA, pid, 1, pr::terms("M8 snap v2", 9)), PO::Updated));
            const auto snap = get(s.userA, id);
            CHECK(is(snap, RO::Ok));
            if (snap) {
                CHECK_EQ(snap->redemption.title, std::string("M8 snap v1"));
                CHECK_EQ(snap->redemption.priceFlowers, 3);
                CHECK_EQ(snap->redemption.productVersion, 1);
            }
            CHECK(is(redeem(s.userB, pid, 1), RO::VersionConflict));  // the displayed v1 terms are stale now

            // Edit queued ahead of the redeem on the shared product lock: the redeem sees v3.
            {
                auto holder = lockProduct(dsn, pid);
                const auto edit = pr::startEdit(s.userA, pid, 2, pr::terms("M8 snap v3", 5), c::newUuid());
                CHECK(tc::waiters(observer, holder.pid, 1));
                const auto stale = startRedeem(s.userB, pid, 2, c::newUuid());
                CHECK(tc::waiters(observer, holder.pid, 2));
                const auto before = effects(observer, s.userB);
                c::endHolder(observer, holder, "m8-edit-race");
                CHECK(pr::is(edit->wait(), PO::Updated));
                CHECK(is(stale->wait(), RO::VersionConflict));
                CHECK(effects(observer, s.userB) == before);
            }
            // Archive queued ahead: the redeem is rejected as archived.
            {
                auto holder = lockProduct(dsn, pid);
                auto archive = std::make_shared<pr::PendingProduct>();
                usapp::ProductService::instance().archive(s.userA, pid, c::newUuid(), 3,
                    [archive](usapp::ProductService::Status st) { archive->deliver(std::move(st)); });
                CHECK(tc::waiters(observer, holder.pid, 1));
                const auto late = startRedeem(s.userB, pid, 3, c::newUuid());
                CHECK(tc::waiters(observer, holder.pid, 2));
                const auto before = effects(observer, s.userB);
                c::endHolder(observer, holder, "m8-archive-race");
                CHECK(pr::is(archive->wait(), PO::Updated));
                CHECK(is(late->wait(), RO::ProductArchived));
                CHECK(effects(observer, s.userB) == before);
            }
            // The committed redemption still replays (old facts, unchanged snapshot) after archive.
            const auto replay = redeem(s.userB, pid, 1, key);
            CHECK(is(replay, RO::Replayed));
            if (replay && done) {
                CHECK_EQ(facts(*replay), facts(*done));
                CHECK_EQ(replay->redemption.title, std::string("M8 snap v1"));
            }
            CHECK_EQ(flowers(s.userB), 17);
        } catch (const std::exception &) { c::expectThat(false, "m8-snapshot", "case failed (details withheld)"); }
    }

    TEST_CASE("M8: concurrent redemptions compete for the balance; same-key duplicates debit once") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            fund(s, 10);
            const auto p1 = pr::create(s.userA, pr::terms("M8 race one", 6));
            const auto p2 = pr::create(s.userA, pr::terms("M8 race two", 6));
            const std::string a = p1 ? p1->product.id : "", b = p2 ? p2->product.id : "";
            // Different products (different product locks): only the account row lock serialises them.
            const auto x = startRedeem(s.userB, a, 1, c::newUuid());
            const auto y = startRedeem(s.userB, b, 1, c::newUuid());
            const auto rx = x->wait(), ry = y->wait();
            CHECK((is(rx, RO::Redeemed) && is(ry, RO::InsufficientBalance)) ||
                  (is(ry, RO::Redeemed) && is(rx, RO::InsufficientBalance)));
            CHECK_EQ(flowers(s.userB), 4);
            const auto e = effects(observer, s.userB);
            CHECK_EQ(e.ops, 1);
            CHECK_EQ(e.entries, 1);
            CHECK_EQ(e.rows, 1);
            CHECK_EQ(e.audits, 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE user_id = $1::uuid "
                                     "AND asset_type = 'flower' AND balance_after < 0", s.userB), 0);

            // Same key, same body, concurrently: one redemption, the other replays it.
            fund(s, 10);
            const auto key = c::newUuid();
            const auto u = startRedeem(s.userB, a, 1, key);
            const auto v = startRedeem(s.userB, a, 1, key);
            const auto ru = u->wait(), rv = v->wait();
            CHECK((is(ru, RO::Redeemed) && is(rv, RO::Replayed)) || (is(rv, RO::Redeemed) && is(ru, RO::Replayed)));
            if (ru && rv) CHECK_EQ(ru->redemption.id, rv->redemption.id);
            CHECK_EQ(flowers(s.userB), 8);
            CHECK_EQ(effects(observer, s.userB).rows, 2);
            CHECK(is(redeem(s.userB, b, 1), RO::Redeemed));
            CHECK(is(redeem(s.userB, b, 1), RO::InsufficientBalance));
            CHECK_EQ(flowers(s.userB), 2);
        } catch (const std::exception &) { c::expectThat(false, "m8-race", "case failed (details withheld)"); }
    }

    TEST_CASE("M8: both parties page the history in (created_at, id) order; other spaces see nothing") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            fund(s, 10);
            const auto made = pr::create(s.userA, pr::terms("M8 pages", 1));
            const std::string pid = made ? made->product.id : "";
            std::vector<std::string> ids;
            for (int i = 0; i < 3; ++i) {
                const auto r = redeem(s.userB, pid, 1);
                if (!is(r, RO::Redeemed)) throw std::runtime_error("redeem failed");
                ids.insert(ids.begin(), r->redemption.id);  // newest first
            }
            int pages = 0;
            CHECK(allPages(s.userB, 1, pages) == ids);
            CHECK_EQ(pages, 3);
            CHECK(allPages(s.userA, 2, pages) == ids);
            CHECK_EQ(pages, 2);
            const auto none = list(other.userA);
            CHECK(none.has_value());
            if (none) CHECK(none->redemptions.empty());
            const auto invalid = list(s.userB, 101);
            CHECK(invalid.has_value());
            if (invalid) CHECK(invalid->outcome == RO::Validation);
        } catch (const std::exception &) { c::expectThat(false, "m8-pages", "case failed (details withheld)"); }
    }

    TEST_CASE("M8: direct SQL guards (actor, stale snapshot, fulfiller, debit link, append-only) and atomic rollback") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            fund(s, 10);
            const auto made = pr::create(s.userA, pr::terms("M8 guard", 2));
            const std::string pid = made ? made->product.id : "";
            CHECK(pr::is(pr::edit(s.userA, pid, 1, pr::terms("M8 guard v2", 2)), PO::Updated));
            const auto done = redeem(s.userB, pid, 2);
            const std::string id = done ? done->redemption.id : "";
            const auto before = effects(observer, s.userB);
            auto row = [&](const std::string &redeemer, int version, const std::string &title) {
                const std::string rid = c::newUuid();
                const std::string op = c::newUuid();
                return "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, "
                       "source_id) VALUES ('" + op + "', '" + s.couple + "', '" + redeemer +
                       "', 'REWARD_REDEMPTION', 'reward_redemption', '" + rid + "'); "
                       "INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, "
                       "redeemer_id, title, price_flowers, redemption_mode, operation_id, idempotency_key, "
                       "request_fingerprint) VALUES ('" + rid + "', '" + s.couple + "', '" + pid + "', " +
                       std::to_string(version) + ", '" + s.userA + "', '" + redeemer + "', '" + title +
                       "', 2, 'direct', '" + op + "', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            for (const auto &[sql, state] : std::vector<std::pair<std::string, std::string>>{
                     {row(s.userA, 2, "M8 guard v2"), "UM002"},  // redeemer is not the owner
                     {row(s.userB, 1, "M8 guard"), "UM002"},     // stale version
                     {row(s.userB, 2, "tampered"), "UM002"},     // snapshot differs from the revision
                     {"INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, "
                      "idempotency_key, request_fingerprint) VALUES ('" + id + "', '" + s.couple + "', '" + s.userB +
                      "', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')", "UM004"},
                     {"UPDATE reward_redemptions SET price_flowers = 1 WHERE id = '" + id + "'", "42501"},
                     {"DELETE FROM reward_redemptions WHERE id = '" + id + "'", "42501"}}) {
                auto tx = c::acquireTx(pool);
                const auto r = wallet::exec(tx, wallet::expectFailureSql(sql));
                CHECK(!r.ok);
                CHECK_EQ(wallet::parseFailure(r.error).sqlState, state);
                c::rollbackTx(tx);
            }
            // A redemption operation with a debit but no redemption row fails its deferred link check.
            {
                auto tx = c::acquireTx(pool);
                const std::string op = c::newUuid();
                CHECK(wallet::exec(tx, "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, "
                                       "source_type, source_id) VALUES ('" + op + "', '" + s.couple + "', '" + s.userB +
                                       "', 'REWARD_REDEMPTION', 'reward_redemption', '" + c::newUuid() + "')").ok);
                const auto deferred = wallet::exec(tx, "SET CONSTRAINTS ALL IMMEDIATE");
                CHECK(!deferred.ok);
                CHECK(deferred.error.find("USAPP_REDEMPTION_LINK") != std::string::npos);
                c::rollbackTx(tx);
            }
            // Debit, redemption and audit commit together or not at all: a failing audit rolls everything back.
            {
                auto tx = c::acquireTx(pool);
                const std::string rid = c::newUuid(), op = c::newUuid();
                const auto latest = c::latestEntry(observer, s.userB, "flower");
                const bool staged =
                    wallet::exec(tx, "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, "
                                     "source_type, source_id) VALUES ('" + op + "', '" + s.couple + "', '" + s.userB +
                                     "', 'REWARD_REDEMPTION', 'reward_redemption', '" + rid + "')").ok &&
                    wallet::exec(tx, "INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, "
                                     "creator_id, redeemer_id, title, price_flowers, redemption_mode, operation_id, "
                                     "idempotency_key, request_fingerprint) VALUES ('" + rid + "', '" + s.couple +
                                     "', '" + pid + "', 2, '" + s.userA + "', '" + s.userB + "', 'M8 guard v2', 2, "
                                     "'direct', '" + op + "', '" + task::taskKey() + "', '" + task::taskFingerprint() +
                                     "')").ok &&
                    wallet::exec(tx, "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, "
                                     "seq, amount, balance_after) VALUES ('" + op + "', '" + s.couple + "', '" + s.userB +
                                     "', 'flower', " + std::to_string(latest.seq + 1) + ", -2, " +
                                     std::to_string(latest.balance - 2) + ")").ok;
                CHECK(staged);
                const auto audit = wallet::exec(tx, wallet::expectFailureSql(
                    "INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id) VALUES ('user', '" +
                    s.userB + "', ' ', 'reward_redemption', '" + rid + "')"));
                CHECK(!audit.ok);
                c::rollbackTx(tx);
                CHECK(effects(observer, s.userB) == before);
                CHECK(c::latestEntry(observer, s.userB, "flower") == latest);
            }
        } catch (const std::exception &) { c::expectThat(false, "m8-sql", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::redemption
