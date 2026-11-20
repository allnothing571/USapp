#pragma once

// Admin read-only wallet inspection (docs/admin-wallet-ledger.md), CTest target admin_wallet_db. The service directly, on the
// test database: exact balances from real postings (task rewards, a redemption debit and refund, an avatar purchase), the
// subject states, keyset pages and cursor validation, isolation, the field allowlist, exact large and negative values, and
// an unchanged footprint.
#include <json/json.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "AvatarPurchaseDbTests.h"
#include "RedemptionWorkflowDbTests.h"
#include "services/AdminWalletService.h"

namespace usapptest::adminwallet {
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace av = usapptest::avatar;
namespace ap = usapptest::avatarpurchase;
namespace rd = usapptest::redemption;
namespace rw = usapptest::redemptionworkflow;
namespace base = usapptest::memorydeletion;
using W = usapp::AdminWalletService;
using WO = W::Outcome;

inline std::optional<W::Result> summaryOf(const std::string &user) {
    auto p = std::make_shared<ts::Pending<W::Result>>();
    W::summary(user, [p](W::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::optional<W::Result> pageOf(const std::string &user, const std::string &asset, int limit, std::int64_t cursor = 0) {
    auto p = std::make_shared<ts::Pending<W::Result>>();
    W::ledger(user, asset, limit, cursor, [p](W::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline bool ok(const std::optional<W::Result> &r) { return r && r->outcome == WO::Ok; }
inline std::string footprint() {
    return md::obs().client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM wallet_operations), (SELECT count(*) FROM wallet_ledger_entries), "
        "(SELECT count(*) FROM wallet_accounts), (SELECT count(*) FROM audit_logs), (SELECT count(*) FROM notifications), "
        "(SELECT count(*) FROM avatar_purchases), (SELECT count(*) FROM reward_redemption_events)) AS v")[0]["v"].as<std::string>();
}
inline Json::Value balanceOf(const W::Result &r, const std::string &asset) {
    for (const auto &b : r.body["balances"]) if (b["asset"].asString() == asset) return b;
    return Json::Value();
}
inline void keys(const Json::Value &v, std::set<std::string> &out) {
    if (v.isObject()) for (const auto &k : v.getMemberNames()) { out.insert(k); keys(v[k], out); }
    else if (v.isArray()) for (const auto &x : v) keys(x, out);
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    ap::Catalog catalog;
    catalog.admin = av::newAdmin(pool);

    TEST_CASE("Admin wallet: exact balances and history from real postings; subject states; isolation and allowlist; no writes") {
        try {
            const auto s = md::space();
            // Real postings for userB: two task rewards (both assets), a direct redemption debit and its mutual-cancel refund,
            // and an avatar purchase. userA (the partner) also earns, to prove isolation.
            ap::fund(s, 500, 10);                                   // flower +10, sunshine +500
            const auto product = rw::create(s, "direct");          // price 4 flowers
            const auto paid = rd::redeem(s.userB, product, 1);
            CHECK(paid && paid->success());
            const auto redemption = paid ? paid->redemption.id : std::string();
            const auto proposal = rw::propose(s.userB, redemption);
            CHECK(rd::is(rw::act(s.userA, redemption, "cancel_approve", proposal), rd::RO::Changed));
            const auto item = catalog.paid("hat", 120);
            CHECK(ap::is(ap::buy(s.userB, av::key(), ap::request(item, 1)), ap::PO::Created));
            const auto partnerTask = c::publish(s.userB, "AW partner reward", 7, 3);
            CHECK(c::is(c::runComplete(s.userA, partnerTask, c::newUuid()), c::Outcome::Completed));

            const auto before = footprint();
            const auto sum = summaryOf(s.userB);
            CHECK(ok(sum));
            if (!ok(sum)) return;
            const auto wallet = c::walletOf(s.userB);
            CHECK(wallet.has_value());
            CHECK_EQ(sum->body["user"]["id"].asString(), s.userB);
            CHECK_EQ(sum->body["space"]["couple_id"].asString(), s.couple);
            CHECK_EQ(sum->body["balances"].size(), 2u);
            CHECK_EQ(sum->body["balances"][0]["asset"].asString(), std::string("flower"));
            CHECK_EQ(balanceOf(*sum, "flower")["balance"].asString(), std::to_string(wallet ? wallet->flower : -1));
            CHECK_EQ(balanceOf(*sum, "sunshine")["balance"].asString(), std::to_string(wallet ? wallet->sunshine : -1));
            CHECK_EQ(balanceOf(*sum, "flower")["balance"].asString(), std::string("10"));    // +10 -4 +4
            CHECK_EQ(balanceOf(*sum, "sunshine")["balance"].asString(), std::string("380")); // +500 -120
            CHECK(balanceOf(*sum, "flower")["balance"].isString());  // decimal strings, never JSON numbers
            CHECK(balanceOf(*sum, "flower")["account_exists"].asBool());

            const auto flowers = pageOf(s.userB, "flower", 50);
            CHECK(ok(flowers));
            if (!ok(flowers)) return;
            const auto &fi = flowers->body["items"];
            CHECK_EQ(fi.size(), 3u);
            // Newest first by the per-account sequence: refund, debit, reward.
            CHECK_EQ(fi[0]["seq"].asString(), std::string("3"));
            CHECK_EQ(fi[0]["amount"].asString(), std::string("4"));
            CHECK_EQ(fi[0]["operation"]["type"].asString(), std::string("REDEMPTION_REFUND"));
            CHECK_EQ(fi[1]["amount"].asString(), std::string("-4"));
            CHECK_EQ(fi[1]["operation"]["type"].asString(), std::string("REWARD_REDEMPTION"));
            CHECK_EQ(fi[1]["operation"]["source_type"].asString(), std::string("reward_redemption"));
            CHECK_EQ(fi[1]["operation"]["source_id"].asString(), redemption);
            CHECK_EQ(fi[2]["operation"]["type"].asString(), std::string("TASK_REWARD"));
            CHECK_EQ(fi[0]["balance_after"].asString(), flowers->body["balance"].asString());
            CHECK_EQ(flowers->body["latest_seq"].asString(), std::string("3"));
            CHECK(flowers->body["next_cursor"].isNull());
            const auto sunshine = pageOf(s.userB, "sunshine", 50);
            CHECK(ok(sunshine) && sunshine->body["items"].size() == 2u);
            if (ok(sunshine)) {
                CHECK_EQ(sunshine->body["items"][0]["amount"].asString(), std::string("-120"));
                CHECK_EQ(sunshine->body["items"][0]["operation"]["type"].asString(), std::string("AVATAR_PURCHASE"));
                CHECK_EQ(sunshine->body["items"][0]["balance_after"].asString(), std::string("380"));
            }
            // Isolation: no entry of the partner, and no field outside the allowlist.
            const auto partnerOps = md::obs().client->execSqlSync(
                "SELECT e.operation_id::text AS id FROM wallet_ledger_entries e WHERE e.user_id = $1::uuid", s.userA);
            CHECK(!partnerOps.empty());
            for (const auto *page : {&*flowers, &*sunshine}) {
                for (const auto &e : page->body["items"]) {
                    for (const auto &row : partnerOps) {
                        if (row["id"].as<std::string>() == e["operation"]["id"].asString()) {
                            // A shared operation is possible only if it also posted to userB; it never does here.
                            CHECK(false);
                        }
                    }
                }
            }
            std::set<std::string> names;
            keys(sum->body, names);
            keys(flowers->body, names);
            const std::set<std::string> allowed = {"user", "id", "status", "space", "couple_id", "slot", "balances", "asset", "account_exists",
                                                   "balance", "latest_seq", "read_at", "user_id", "items", "seq", "amount", "balance_after",
                                                   "recorded_at", "operation", "type", "source_type", "source_id", "next_cursor"};
            for (const auto &k : names) CHECK(allowed.count(k) == 1u);
            CHECK_EQ(footprint(), before);  // the reads wrote nothing

            // Subject states: no such user; a user without a space; a member without postings.
            const auto missing = summaryOf(c::newUuid());
            CHECK(missing && missing->outcome == WO::NotFound);
            const auto missingPage = pageOf(c::newUuid(), "flower", 10);
            CHECK(missingPage && missingPage->outcome == WO::NotFound);
            const auto loner = c::newUuid();
            ts::commitUser(pool, loner, "aw_loner");
            const auto lonely = summaryOf(loner);
            CHECK(ok(lonely));
            if (ok(lonely)) {
                CHECK(lonely->body["space"].isNull());
                for (const auto &b : lonely->body["balances"]) {
                    CHECK_FALSE(b["account_exists"].asBool());
                    CHECK_EQ(b["balance"].asString(), std::string("0"));
                    CHECK_EQ(b["latest_seq"].asString(), std::string("0"));
                }
            }
            const auto lonelyPage = pageOf(loner, "sunshine", 10);
            CHECK(ok(lonelyPage) && lonelyPage->body["items"].empty() && lonelyPage->body["next_cursor"].isNull());
            const auto fresh = md::space();
            const auto quiet = summaryOf(fresh.userA);
            CHECK(ok(quiet) && !quiet->body["space"].isNull() && balanceOf(*quiet, "flower")["balance"].asString() == "0");
        } catch (const std::exception &e) { c::expectThat(false, "admin-wallet-postings", md::exceptionCategory(e)); }
    }

    TEST_CASE("Admin wallet: keyset pages by seq, cursor boundaries and validation, exact large and negative values") {
        try {
            const auto s = md::space();
            for (int i = 1; i <= 5; ++i) {
                const auto task = c::publish(s.userA, "AW page " + std::to_string(i), i, 0);
                CHECK(c::is(c::runComplete(s.userB, task, c::newUuid()), c::Outcome::Completed));
            }
            std::vector<std::string> seen;
            std::int64_t cursor = 0;
            int pages = 0;
            for (;;) {
                const auto p = pageOf(s.userB, "flower", 2, cursor);
                CHECK(ok(p));
                if (!ok(p)) break;
                ++pages;
                CHECK(p->body["items"].size() <= 2u);
                for (const auto &e : p->body["items"]) seen.push_back(e["seq"].asString());
                if (p->body["next_cursor"].isNull()) break;
                std::int64_t next = 0;
                CHECK(W::decodeCursor(p->body["next_cursor"].asString(), s.userB, "flower", next));
                cursor = next;
                if (pages > 5) break;
            }
            CHECK_EQ(pages, 3);
            CHECK(seen == std::vector<std::string>({"5", "4", "3", "2", "1"}));
            // Boundaries: below the first entry is empty; above the newest starts from the newest.
            const auto below = pageOf(s.userB, "flower", 2, 1);
            CHECK(ok(below) && below->body["items"].empty() && below->body["next_cursor"].isNull());
            const auto above = pageOf(s.userB, "flower", 2, 1000);
            CHECK(ok(above) && above->body["items"][0]["seq"].asString() == "5");
            // Cursors bind the user and the asset.
            std::int64_t out = 0;
            const auto good = W::encodeCursor(s.userB, "flower", 3);
            CHECK(W::decodeCursor(good, s.userB, "flower", out) && out == 3);
            CHECK_FALSE(W::decodeCursor(good, s.userA, "flower", out));
            CHECK_FALSE(W::decodeCursor(good, s.userB, "sunshine", out));
            for (const std::string bad : {std::string("w1.flower.0.") + s.userB, std::string("w1.flower.03.") + s.userB,
                                          std::string("w1.flower.-3.") + s.userB, std::string("w1.flower.9223372036854775808.") + s.userB,
                                          std::string("w1.flower.12345678901234567890.") + s.userB, std::string("w2.flower.3.") + s.userB,
                                          std::string("w1.flower.3"), std::string("w1.flower..") + s.userB, std::string(200, 'w')}) {
                CHECK_FALSE(W::decodeCursor(bad, s.userB, "flower", out));
            }
            CHECK(W::decodeCursor(std::string("w1.flower.9223372036854775807.") + s.userB, s.userB, "flower", out));

            // Exact values at the schema's real bounds and a negative amount, in a chain-consistent synthetic account written by
            // maintenance (CHECK constraints still apply; the harness's ledger-chain post-check must still hold): 10 000 entries of
            // +100000 reach the sunshine maximum 1 000 000 000, then -100000. Each entry has its own operation and source id. A seq
            // beyond 2^53 is not reachable through real postings; it is covered by decodeCursor above and the admin client tests.
            ts::CommittedSpace big;
            ts::commitSpace(pool, big, true);
            base::asMaintenance(
                "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES ('" + big.userB + "', 'sunshine', '" + big.couple + "'); "
                "CREATE TEMP TABLE aw_ops ON COMMIT DROP AS SELECT g AS seq, gen_random_uuid() AS id FROM generate_series(1, 10001) g; "
                "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id) "
                "SELECT o.id, '" + big.couple + "', '" + big.userB + "', 'TASK_REWARD', 'task', o.id::text FROM aw_ops o; "
                "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after) "
                "SELECT o.id, '" + big.couple + "', '" + big.userB + "', 'sunshine', o.seq, "
                "CASE WHEN o.seq <= 10000 THEN 100000 ELSE -100000 END, "
                "CASE WHEN o.seq <= 10000 THEN o.seq * 100000 ELSE 999900000 END FROM aw_ops o;");
            const auto bigSum = summaryOf(big.userB);
            CHECK(ok(bigSum));
            if (ok(bigSum)) {
                CHECK_EQ(balanceOf(*bigSum, "sunshine")["balance"].asString(), std::string("999900000"));
                CHECK_EQ(balanceOf(*bigSum, "sunshine")["latest_seq"].asString(), std::string("10001"));
                CHECK_FALSE(balanceOf(*bigSum, "flower")["account_exists"].asBool());
            }
            const auto bigPage = pageOf(big.userB, "sunshine", 1);
            CHECK(ok(bigPage));
            if (ok(bigPage)) {
                CHECK_EQ(bigPage->body["items"][0]["seq"].asString(), std::string("10001"));
                CHECK_EQ(bigPage->body["items"][0]["amount"].asString(), std::string("-100000"));
                CHECK_EQ(bigPage->body["items"][0]["balance_after"].asString(), std::string("999900000"));
                CHECK_EQ(bigPage->body["next_cursor"].asString(), W::encodeCursor(big.userB, "sunshine", 10001));
                std::int64_t next = 0;
                CHECK(W::decodeCursor(bigPage->body["next_cursor"].asString(), big.userB, "sunshine", next));
                const auto second = pageOf(big.userB, "sunshine", 1, next);
                CHECK(ok(second) && second->body["items"][0]["seq"].asString() == "10000" &&
                      second->body["items"][0]["amount"].asString() == "100000" &&
                      second->body["items"][0]["balance_after"].asString() == "1000000000");
                // The last page (limit 100 from cursor 101) ends at seq 1 with no further cursor.
                const auto tail = pageOf(big.userB, "sunshine", 100, 101);
                CHECK(ok(tail) && tail->body["items"].size() == 100u && tail->body["items"][99]["seq"].asString() == "1" &&
                      tail->body["next_cursor"].isNull());
            }
        } catch (const std::exception &e) { c::expectThat(false, "admin-wallet-pages", md::exceptionCategory(e)); }
    }
}

}  // namespace usapptest::adminwallet
