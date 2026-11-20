#pragma once

// Phase 4 B2 (0045_avatar_purchases, docs/phase4-avatar-shop-design.md Section 6): sunshine purchases and owned items through the
// real AvatarPurchaseService on literal usapp_test. Buyers are funded by real task settlement (TASK_REWARD), never by forged
// ledger rows. Fake registry rows only. Runs as its own CTest target (avatar_purchase_db), outside db_gated. Deferred-link
// violations with forged rows are in database/tests/avatar_purchase_test.sql; the HTTP routes are in ReminderLiveApiTest.h.
#include <json/json.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "AvatarCatalogDbTests.h"
#include "TaskCompletionDbTests.h"
#include "services/AvatarPurchaseService.h"

namespace usapptest::avatarpurchase {

namespace av = usapptest::avatar;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace w = usapptest::wallet;
using P = usapp::AvatarPurchaseService;
using PO = P::Outcome;

inline const c::Observer &obs() { return md::obs(); }
inline long long count(const std::string &sql, const std::string &a = "", const std::string &b = "") { return av::count(sql, a, b); }

// Real settlement: the creator publishes, the assignee completes; the assignee receives the rewards.
inline void fund(const ts::CommittedSpace &s, int sunshine, int flowers = 0) {
    const auto task = c::publish(s.userA, "B2 fund " + usapp::crypto::randomHex(3), flowers, sunshine);
    const auto done = c::runComplete(s.userB, task, c::newUuid());
    if (!c::is(done, c::Outcome::Completed)) throw std::runtime_error("funding failed");
}
inline c::Latest sunshineOf(const std::string &user) { return c::latestEntry(obs(), user, "sunshine"); }
inline c::Latest flowersOf(const std::string &user) { return c::latestEntry(obs(), user, "flower"); }
inline long long purchaseOps(const std::string &user) {
    return count("SELECT count(*)::bigint AS n FROM wallet_operations WHERE actor_user_id = $1::uuid AND operation_type = 'AVATAR_PURCHASE'", user);
}
inline long long purchases(const std::string &user) { return count("SELECT count(*)::bigint AS n FROM avatar_purchases WHERE user_id = $1::uuid", user); }

inline P::Request request(const std::string &item, int version) {
    P::Request r;
    r.itemId = item;
    r.expectedVersion = version;
    return r;
}
using Pending = ts::Pending<P::Result>;
inline std::shared_ptr<Pending> startBuy(const std::string &user, const std::string &key, const P::Request &r, P::Fault f = P::Fault::None) {
    auto p = std::make_shared<Pending>();
    P::instance().purchase(user, key, r, [p](P::Result x) { p->deliver(std::move(x)); }, f);
    return p;
}
inline std::optional<P::Result> buy(const std::string &user, const std::string &key, const P::Request &r, P::Fault f = P::Fault::None) {
    return startBuy(user, key, r, f)->wait();
}
inline std::optional<P::Result> owned(const std::string &user, int limit, const std::string &cursor = "") {
    auto p = std::make_shared<Pending>();
    P::instance().owned(user, limit, cursor, [p](P::Result x) { p->deliver(std::move(x)); });
    return p->wait();
}
inline bool is(const std::optional<P::Result> &r, PO o) { return r.has_value() && r->outcome == o; }

struct Catalog {
    std::string admin;
    std::string run = usapp::crypto::randomHex(4);
    int n = 0;
    // A listed paid item at `price`, version 1.
    std::string paid(const std::string &category, int price) {
        const auto asset = av::addAsset(category + ".b2_" + run + "_" + std::to_string(++n), 1);
        const auto made = av::create(admin, av::key(), av::item(category, false, asset, "B2-" + std::to_string(n), price, true));
        if (!av::is(made, usapp::AvatarService::Outcome::Created)) throw std::runtime_error("item");
        return av::idOf(made);
    }
    std::string starterItem(const std::string &category) {
        const auto asset = av::addAsset(category + ".b2s_" + run + "_" + std::to_string(++n), 1);
        return av::idOf(av::create(admin, av::key(), av::item(category, true, asset, "B2S-" + std::to_string(n))));
    }
    std::string assetOf(const std::string &item) {
        return obs().client->execSqlSync("SELECT asset_key FROM avatar_item_revisions WHERE item_id = $1::uuid ORDER BY version DESC LIMIT 1",
                                         item)[0]["asset_key"].as<std::string>();
    }
    bool revise(const std::string &item, int expected, int price, bool listed) {
        return av::is(av::revise(admin, av::key(), item, av::revision(expected, assetOf(item), "B2-R" + std::to_string(++n), price, listed)),
                      usapp::AvatarService::Outcome::Created);
    }
};

inline void runAvatarPurchaseDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    Catalog cat;
    cat.admin = av::newAdmin(pool);

    TEST_CASE("Avatar B2: an exact sunshine debit, flowers untouched, one audit row, ownership and an admin count") {
        try {
            const auto s = md::space();
            fund(s, 500, 3);
            const auto flowersBefore = flowersOf(s.userB);
            const auto item = cat.paid("hat", 120);
            const auto k = av::key();
            const auto made = buy(s.userB, k, request(item, 1));
            CHECK(is(made, PO::Created));
            if (!is(made, PO::Created)) return;
            const auto &p = made->body["purchase"];
            CHECK(!made->body["replayed"].asBool());
            CHECK_EQ(p["item_id"].asString(), item);
            CHECK_EQ(p["item_version"].asInt(), 1);
            CHECK_EQ(p["price_sunshine"].asInt(), 120);
            CHECK_EQ(p["debited_sunshine"].asInt(), 120);
            CHECK_EQ(p["sunshine_balance_after"].asInt(), 380);
            CHECK_EQ(made->body["current"]["sunshine_balance"].asInt(), 380);
            CHECK(made->body["current"]["item"]["listed"].asBool());
            CHECK_EQ(sunshineOf(s.userB).balance, 380);
            CHECK(flowersOf(s.userB) == flowersBefore);
            const auto op = p["operation_id"].asString();
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE operation_id = $1::uuid", op), 1);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE operation_id = $1::uuid AND asset_type = 'sunshine' "
                           "AND amount = -120 AND user_id = $2::uuid", op, s.userB), 1);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.purchased' AND target_id = $1", p["id"].asString()), 1);
            CHECK_EQ(purchases(s.userB), 1);
            // The partner owns nothing and was charged nothing.
            CHECK_EQ(purchases(s.userA), 0);
            CHECK_EQ(purchaseOps(s.userA), 0);
            const auto admin = av::readWith([&item](usapp::AvatarService::Callback cb) { usapp::AvatarService::instance().adminItem(item, std::move(cb)); });
            CHECK(admin && admin->body["item"]["owner_count"].asInt() == 1);
            const auto starterAdmin = av::readWith([&](usapp::AvatarService::Callback cb) {
                usapp::AvatarService::instance().adminItem(cat.starterItem("hair"), std::move(cb));
            });
            CHECK(starterAdmin && starterAdmin->body["item"]["owner_count"].isNull());
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: refusals write nothing: insufficient, stale version, starter, unlisted, unknown, owned, outside a couple") {
        try {
            const auto s = md::space();
            fund(s, 200, 1);
            const auto rich = cat.paid("clothes", 300);
            const auto cheap = cat.paid("clothes", 50);
            const auto starter = cat.starterItem("expression");
            const auto hidden = cat.paid("accessory", 10);
            CHECK(cat.revise(hidden, 1, 10, false));
            const auto before = sunshineOf(s.userB);
            const auto flowers = flowersOf(s.userB);
            CHECK(is(buy(s.userB, av::key(), request(rich, 1)), PO::Insufficient));
            CHECK(is(buy(s.userB, av::key(), request(cheap, 2)), PO::VersionConflict));
            CHECK(is(buy(s.userB, av::key(), request(starter, 1)), PO::Unavailable));
            CHECK(is(buy(s.userB, av::key(), request(hidden, 2)), PO::Unavailable));
            CHECK(is(buy(s.userB, av::key(), request(c::newUuid(), 1)), PO::NotFound));
            CHECK(sunshineOf(s.userB) == before);
            CHECK(flowersOf(s.userB) == flowers);
            CHECK_EQ(purchases(s.userB), 0);
            CHECK_EQ(purchaseOps(s.userB), 0);
            // A user with no sunshine at all: the refusal also rolls back the account row it would have created.
            CHECK(is(buy(s.userA, av::key(), request(cheap, 1)), PO::Insufficient));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM wallet_accounts WHERE user_id = $1::uuid AND asset_type = 'sunshine'", s.userA), 0);
            // Owned already, under another key.
            CHECK(is(buy(s.userB, av::key(), request(cheap, 1)), PO::Created));
            const auto after = sunshineOf(s.userB);
            CHECK(is(buy(s.userB, av::key(), request(cheap, 1)), PO::AlreadyOwned));
            CHECK(sunshineOf(s.userB) == after);
            CHECK_EQ(purchases(s.userB), 1);
            // Outside a couple, as the wallet.
            const std::string loner = c::newUuid();
            ts::commitUser(pool, loner, "b2_loner");
            CHECK(is(buy(loner, av::key(), request(cheap, 1)), PO::SpaceNotFound));
            CHECK(is(owned(loner, 10), PO::SpaceNotFound));
            // Request shape.
            P::Request out;
            std::string message;
            Json::Value body;
            Json::Reader().parse(R"({"item_id":"x","expected_version":1})", body);
            CHECK(!P::parse(body, out, message));
            Json::Reader().parse(R"({"item_id":")" + cheap + R"(","expected_version":0})", body);
            CHECK(!P::parse(body, out, message));
            Json::Reader().parse(R"({"item_id":")" + cheap + R"(","expected_version":1,"price":1})", body);
            CHECK(!P::parse(body, out, message));
            Json::Reader().parse(R"({"item_id":")" + cheap + R"(","expected_version":true})", body);
            CHECK(!P::parse(body, out, message));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: a replay returns the original facts after a reprice and an unlist; another request under the key is reuse") {
        try {
            const auto s = md::space();
            fund(s, 400);
            const auto item = cat.paid("handheld", 120);
            const auto other = cat.paid("handheld", 30);
            const auto k = av::key();
            const auto made = buy(s.userB, k, request(item, 1));
            CHECK(is(made, PO::Created));
            if (!is(made, PO::Created)) return;
            CHECK(cat.revise(item, 1, 200, true));
            CHECK(cat.revise(item, 2, 200, false));
            fund(s, 50);
            const auto again = buy(s.userB, k, request(item, 1));
            CHECK(is(again, PO::Replayed));
            if (!again) return;
            CHECK(again->body["replayed"].asBool());
            CHECK_EQ(av::written(again->body["purchase"]), av::written(made->body["purchase"]));
            CHECK_EQ(again->body["purchase"]["price_sunshine"].asInt(), 120);
            CHECK_EQ(again->body["purchase"]["sunshine_balance_after"].asInt(), 280);
            // Today's state is separate and says so: repriced, unlisted, a new balance.
            CHECK_EQ(again->body["current"]["sunshine_balance"].asInt(), 330);
            CHECK_EQ(again->body["current"]["item"]["price_sunshine"].asInt(), 200);
            CHECK(!again->body["current"]["item"]["listed"].asBool());
            CHECK(is(buy(s.userB, k, request(item, 2)), PO::KeyReused));
            CHECK(is(buy(s.userB, k, request(other, 1)), PO::KeyReused));
            CHECK_EQ(purchases(s.userB), 1);
            CHECK_EQ(sunshineOf(s.userB).balance, 330);
            // The owned list keeps the unlisted item, with its original facts and today's view.
            const auto list = owned(s.userB, 10);
            CHECK(is(list, PO::Ok));
            CHECK(list && list->body["items"].size() == 1u && list->body["items"][0]["purchase"]["id"].asString() == made->body["purchase"]["id"].asString());
            CHECK(list && !list->body["items"][0]["current_item"]["listed"].asBool());
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: concurrency: same key, same item under two keys, and two items competing for limited funds") {
        try {
            const auto s = md::space();
            fund(s, 150);
            const auto a = cat.paid("hair", 100);
            const auto b = cat.paid("hair", 100);
            // The same key twice at once: one purchase, one replay, one debit.
            const auto k = av::key();
            auto first = startBuy(s.userB, k, request(a, 1));
            auto second = startBuy(s.userB, k, request(a, 1));
            const auto r1 = first->wait();
            const auto r2 = second->wait();
            CHECK(r1 && r2);
            if (!r1 || !r2) return;
            CHECK((r1->outcome == PO::Created && r2->outcome == PO::Replayed) || (r1->outcome == PO::Replayed && r2->outcome == PO::Created));
            CHECK_EQ(purchases(s.userB), 1);
            CHECK_EQ(sunshineOf(s.userB).balance, 50);

            const auto t = md::space();
            fund(t, 150);
            // The same item under two keys: exactly one charge.
            auto x = startBuy(t.userB, av::key(), request(a, 1));
            auto y = startBuy(t.userB, av::key(), request(a, 1));
            const auto rx = x->wait();
            const auto ry = y->wait();
            CHECK(rx && ry);
            if (!rx || !ry) return;
            CHECK((rx->outcome == PO::Created) != (ry->outcome == PO::Created));
            CHECK(rx->outcome == PO::AlreadyOwned || ry->outcome == PO::AlreadyOwned);
            CHECK_EQ(sunshineOf(t.userB).balance, 50);
            // Two different items, funds for one: one purchase, one INSUFFICIENT_BALANCE, never negative.
            fund(t, 50);
            auto m = startBuy(t.userB, av::key(), request(b, 1));
            auto n = startBuy(t.userB, av::key(), request(cat.paid("hat", 100), 1));
            const auto rm = m->wait();
            const auto rn = n->wait();
            CHECK(rm && rn);
            if (!rm || !rn) return;
            CHECK((rm->outcome == PO::Created) != (rn->outcome == PO::Created));
            CHECK(rm->outcome == PO::Insufficient || rn->outcome == PO::Insufficient);
            CHECK_EQ(sunshineOf(t.userB).balance, 0);
            CHECK_EQ(purchases(t.userB), 2);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: a catalog revision holding the item lock decides first; the waiting purchase then sees a conflict") {
        c::Holder holder;
        try {
            const auto s = md::space();
            fund(s, 300);
            const auto item = cat.paid("accessory", 80);
            // An admin revision, uncommitted, holding the item lock.
            holder.conn = c::Observer::connect(dsn);
            holder.tx = c::acquireTx(holder.conn->client);
            const auto pid = c::exec(holder.tx, "SELECT pg_backend_pid() AS pid");
            CHECK(pid.ok);
            holder.pid = (*pid.result)[0]["pid"].as<int>();
            const auto revised = c::exec(holder.tx,
                "SELECT usapp_avatar_item_revise($1::uuid, $2, repeat('c', 64), $3::uuid, 1, 'B2-LOCKED', $4, 90, true)::text AS r",
                cat.admin, av::key(), item, cat.assetOf(item));
            CHECK(revised.ok && (*revised.result)[0]["r"].as<std::string>().find("created") != std::string::npos);
            auto waiting = startBuy(s.userB, av::key(), request(item, 1));
            CHECK(c::observeBlockedBy(obs(), holder.pid) != 0);
            CHECK(c::commitTx(holder.tx) == std::optional<bool>(true));
            const auto result = waiting->wait();
            CHECK(is(result, PO::VersionConflict));
            CHECK_EQ(sunshineOf(s.userB).balance, 300);
            CHECK_EQ(purchases(s.userB), 0);
            // With the new version it goes through at the new price.
            const auto ok = buy(s.userB, av::key(), request(item, 2));
            CHECK(is(ok, PO::Created));
            CHECK(ok && ok->body["purchase"]["price_sunshine"].asInt() == 90);
            c::endHolder(obs(), holder, "b2-revision-holder");
        } catch (const std::exception &) {
            c::endHolder(obs(), holder, "b2-revision-holder");
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: an uncertain commit is retried with the original key and body and writes exactly once") {
        try {
            const auto s = md::space();
            fund(s, 100);
            const auto item = cat.paid("clothes", 40);
            const auto k = av::key();
            CHECK(is(buy(s.userB, k, request(item, 1), P::Fault::FailBeforeCommit), PO::ResultUnknown));
            CHECK_EQ(purchases(s.userB), 0);
            CHECK_EQ(purchaseOps(s.userB), 0);
            CHECK_EQ(sunshineOf(s.userB).balance, 100);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid AND action = 'avatar.purchased'", s.userB), 0);
            const auto made = buy(s.userB, k, request(item, 1));
            CHECK(is(made, PO::Created));
            const auto again = buy(s.userB, k, request(item, 1));
            CHECK(is(again, PO::Replayed));
            CHECK(made && again && av::written(made->body["purchase"]) == av::written(again->body["purchase"]));
            CHECK_EQ(purchases(s.userB), 1);
            CHECK_EQ(sunshineOf(s.userB).balance, 60);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: the owned list is the caller's own, paginated, and excludes starters") {
        try {
            const auto s = md::space();
            fund(s, 1000);
            std::vector<std::string> bought;
            for (const char *category : {"hat", "handheld", "accessory"}) {
                const auto item = cat.paid(category, 10);
                CHECK(is(buy(s.userB, av::key(), request(item, 1)), PO::Created));
                bought.push_back(item);
            }
            const auto partner = owned(s.userA, 10);
            CHECK(is(partner, PO::Ok) && partner->body["items"].size() == 0u);
            const auto page1 = owned(s.userB, 2);
            CHECK(is(page1, PO::Ok));
            if (!is(page1, PO::Ok)) return;
            CHECK_EQ(page1->body["items"].size(), 2u);
            CHECK(page1->body["next_cursor"].isString());
            const auto page2 = owned(s.userB, 2, page1->body["next_cursor"].asString());
            CHECK(is(page2, PO::Ok));
            if (!is(page2, PO::Ok)) return;
            CHECK_EQ(page2->body["items"].size(), 1u);
            CHECK(page2->body["next_cursor"].isNull());
            std::set<std::string> seen;
            for (const auto *page : {&page1->body, &page2->body}) {
                for (const auto &e : (*page)["items"]) seen.insert(e["purchase"]["item_id"].asString());
            }
            CHECK((seen == std::set<std::string>(bought.begin(), bought.end())));
            // Newest first.
            CHECK_EQ(page1->body["items"][0]["purchase"]["item_id"].asString(), bought[2]);
            // Nothing about the partner or a balance.
            const auto text = av::written(page1->body);
            CHECK(text.find(s.userA) == std::string::npos);
            CHECK(text.find("sunshine_balance\"") == std::string::npos);
            CHECK(is(owned(s.userB, 0), PO::Validation));
            CHECK(is(owned(s.userB, 101), PO::Validation));
            CHECK(is(owned(s.userB, 2, "nonsense"), PO::Validation));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B2: purchases are append-only and never updated or deleted") {
        try {
            const auto s = md::space();
            fund(s, 50);
            const auto item = cat.paid("hat", 5);
            const auto made = buy(s.userB, av::key(), request(item, 1));
            CHECK(is(made, PO::Created));
            if (!made) return;
            const auto id = made->body["purchase"]["id"].asString();
            for (const char *sql : {"UPDATE avatar_purchases SET price_sunshine = 1 WHERE id = $1::uuid", "DELETE FROM avatar_purchases WHERE id = $1::uuid"}) {
                CHECK(av::refusedWith(av::refusal([&] { pool->execSqlSync(sql, id); }), "permission denied"));
            }
            CHECK(av::refusedWith(av::refusal([&] { av::asMaintenance("DELETE FROM avatar_purchases WHERE id = '" + id + "'"); }), "USAPP_AVATAR_APPEND_ONLY"));
            CHECK_EQ(purchases(s.userB), 1);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
}

}  // namespace usapptest::avatarpurchase
