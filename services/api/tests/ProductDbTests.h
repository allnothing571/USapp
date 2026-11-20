#pragma once

// M7: real ProductService on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test spaces); never resets.
#include <algorithm>
#include <functional>

#include "TaskChangeDbTests.h"
#include "services/ProductService.h"

namespace usapptest::product {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using PS = usapp::ProductService;
using PO = PS::Outcome;
using PendingProduct = ts::Pending<PS::Status>;

inline PS::Terms terms(const std::string &title, int price, const std::string &mode = "direct") {
    PS::Terms t;
    t.title = title;
    t.description = "";
    t.priceFlowers = price;
    t.redemptionMode = mode;
    return t;
}
inline std::shared_ptr<PendingProduct> startCreate(const std::string &user, const PS::Terms &t, const std::string &key) {
    auto pending = std::make_shared<PendingProduct>();
    PS::instance().create(user, key, t, [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<PS::Status> create(const std::string &user, const PS::Terms &t, const std::string &key = c::newUuid()) {
    return startCreate(user, t, key)->wait();
}
inline std::shared_ptr<PendingProduct> startEdit(const std::string &user, const std::string &id, int expected,
                                                 const PS::Terms &t, const std::string &key) {
    auto pending = std::make_shared<PendingProduct>();
    PS::instance().edit(user, id, key, expected, t, [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<PS::Status> edit(const std::string &user, const std::string &id, int expected, const PS::Terms &t,
                                      const std::string &key = c::newUuid()) {
    return startEdit(user, id, expected, t, key)->wait();
}
inline std::optional<PS::Status> archive(const std::string &user, const std::string &id, int expected,
                                         const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingProduct>();
    PS::instance().archive(user, id, key, expected, [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<PS::Status> get(const std::string &user, const std::string &id) {
    auto pending = std::make_shared<PendingProduct>();
    PS::instance().get(user, id, [pending](PS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<PS::ListResult> list(const std::string &user, int limit = 100,
                                          const std::optional<PS::Cursor> &cursor = std::nullopt) {
    auto pending = std::make_shared<ts::Pending<PS::ListResult>>();
    PS::instance().list(user, limit, cursor, [pending](PS::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
// Every page in order, following next_cursor (bounded).
inline std::vector<std::string> allPages(const std::string &user, int limit, int &pages) {
    std::vector<std::string> ids;
    std::optional<PS::Cursor> cursor;
    pages = 0;
    for (int i = 0; i < 10; ++i) {
        const auto r = list(user, limit, cursor);
        if (!r || r->outcome != PO::Ok) throw std::runtime_error("page failed");
        ++pages;
        for (const auto &p : r->products) ids.push_back(p.id);
        if (!r->nextCursor) return ids;
        cursor = PS::decodeCursor(*r->nextCursor);
        if (!cursor) throw std::runtime_error("bad next cursor");
    }
    throw std::runtime_error("too many pages");
}
inline bool is(const std::optional<PS::Status> &s, PO o) { return s.has_value() && s->outcome == o; }
inline std::string facts(const PS::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, s.mutation);
}
inline int listed(const std::optional<PS::ListResult> &r, const std::string &id) {
    int n = 0;
    if (r) for (const auto &p : r->products) n += p.id == id ? 1 : 0;
    return n;
}

inline void runProductDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);

    TEST_CASE("M7: creator/owner visibility, creator-only management, one-way archive, atomic audits, no wallet") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            ts::CommittedSpace alone;
            ts::commitSpace(pool, alone, false);
            CHECK(is(create(alone.userA, terms("M7 alone", 5)), PO::PartnerRequired));

            const auto made = create(s.userA, terms("M7 for B", 5));
            CHECK(is(made, PO::Created));
            const std::string id = made ? made->product.id : "";
            if (made) {
                CHECK_EQ(made->product.ownerId, s.userB);
                CHECK_EQ(made->product.creatorId, s.userA);
                CHECK_EQ(made->product.version, 1);
                CHECK_EQ(made->mutation["action"].asString(), std::string("create"));
            }
            const auto forA = create(s.userB, terms("M7 for A", 9, "request"));
            CHECK(is(forA, PO::Created));
            CHECK_EQ(listed(list(s.userA), id), 1);
            CHECK_EQ(listed(list(s.userB), id), 1);
            CHECK(is(get(s.userB, id), PO::Ok));
            CHECK(is(get(other.userA, id), PO::NotFound));
            CHECK_EQ(listed(list(other.userA), id), 0);

            CHECK(is(edit(s.userB, id, 1, terms("hijack", 1)), PO::Forbidden));
            CHECK(is(archive(s.userB, id, 1), PO::Forbidden));
            CHECK(is(edit(other.userA, id, 1, terms("other", 1)), PO::NotFound));
            const auto edited = edit(s.userA, id, 1, terms("M7 edited", 7));
            CHECK(is(edited, PO::Updated));
            if (edited) {
                CHECK_EQ(edited->product.version, 2);
                CHECK_EQ(edited->product.priceFlowers, 7);
            }
            CHECK(is(edit(s.userA, id, 1, terms("stale", 8)), PO::VersionConflict));
            const auto archived = archive(s.userA, id, 2);
            CHECK(is(archived, PO::Updated));
            if (archived) {
                CHECK_EQ(archived->product.status, std::string("archived"));
                CHECK_EQ(archived->product.priceFlowers, 7);  // archive keeps the current terms
            }
            CHECK(is(get(s.userB, id), PO::NotFound));
            CHECK_EQ(listed(list(s.userB), id), 0);
            const auto own = get(s.userA, id);
            CHECK(is(own, PO::Ok));
            if (own) CHECK_EQ(own->product.status, std::string("archived"));
            CHECK(is(edit(s.userA, id, 3, terms("after", 2)), PO::Archived));
            CHECK(is(archive(s.userA, id, 3), PO::Archived));
            CHECK(is(edit(s.userB, id, 3, terms("after", 2)), PO::NotFound));

            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM reward_product_revisions WHERE product_id = $1::uuid", id), 3);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'reward_product' "
                                         "AND target_id = $1::text", id), 3);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id = $1::uuid",
                               s.couple), 0);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM wallet_accounts WHERE couple_id = $1::uuid",
                               s.couple), 0);
        } catch (const std::exception &) { c::expectThat(false, "m7-visibility", "case failed (details withheld)"); }
    }

    TEST_CASE("M7: committed replay after later mutations, key reuse, concurrent same-version edits and creates") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto k1 = c::newUuid(), k2 = c::newUuid(), k3 = c::newUuid();
            const auto created = create(s.userA, terms("M7 replay", 5), k1);
            CHECK(is(created, PO::Created));
            const std::string id = created ? created->product.id : "";
            const auto edited = edit(s.userA, id, 1, terms("M7 replay v2", 6), k2);
            const auto archived = archive(s.userA, id, 2, k3);
            CHECK(is(edited, PO::Updated));
            CHECK(is(archived, PO::Updated));
            const auto r1 = create(s.userA, terms("M7 replay", 5), k1);
            const auto r2 = edit(s.userA, id, 1, terms("M7 replay v2", 6), k2);
            const auto r3 = archive(s.userA, id, 2, k3);
            CHECK(is(r1, PO::Replayed));
            CHECK(is(r2, PO::Replayed));
            CHECK(is(r3, PO::Replayed));
            if (r1 && created) {
                CHECK_EQ(facts(*r1), facts(*created));
                CHECK_EQ(r1->product.status, std::string("archived"));  // current state, truthfully newer
            }
            if (r2 && edited) CHECK_EQ(facts(*r2), facts(*edited));
            if (r3 && archived) CHECK_EQ(facts(*r3), facts(*archived));
            CHECK(is(edit(s.userA, id, 1, terms("M7 other", 9), k2), PO::KeyReused));
            CHECK(is(create(s.userA, terms("M7 different", 5), k1), PO::KeyReused));
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM reward_product_revisions WHERE product_id = $1::uuid", id), 3);

            const auto race = create(s.userA, terms("M7 race", 5));
            const std::string rid = race ? race->product.id : "";
            const auto a = startEdit(s.userA, rid, 1, terms("M7 race a", 6), c::newUuid());
            const auto b = startEdit(s.userA, rid, 1, terms("M7 race b", 7), c::newUuid());
            const auto x = a->wait(), y = b->wait();
            CHECK((is(x, PO::Updated) && is(y, PO::VersionConflict)) || (is(y, PO::Updated) && is(x, PO::VersionConflict)));
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM reward_product_revisions WHERE product_id = $1::uuid", rid), 2);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'reward_product' "
                                         "AND target_id = $1::text", rid), 2);

            const auto key = c::newUuid();
            const auto p = startCreate(s.userA, terms("M7 same key", 5), key);
            const auto q = startCreate(s.userA, terms("M7 same key", 5), key);
            const auto u = p->wait(), v = q->wait();
            CHECK((is(u, PO::Created) && is(v, PO::Replayed)) || (is(v, PO::Created) && is(u, PO::Replayed)));
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM reward_product_revisions WHERE actor_id = $1::uuid "
                                         "AND idempotency_key = $2", s.userA, key), 1);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM reward_products p WHERE p.creator_id = $1::uuid "
                                         "AND NOT EXISTS (SELECT 1 FROM reward_product_revisions r WHERE r.product_id = p.id)",
                               s.userA), 0);
        } catch (const std::exception &) { c::expectThat(false, "m7-replay", "case failed (details withheld)"); }
    }

    TEST_CASE("M7: cursor pages in (created_at, id) order with tied timestamps; visibility on every page") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            // Four products for B with one identical created_at, one of them archived; plus one newer.
            std::vector<std::string> tied;
            for (int i = 0; i < 4; ++i) tied.push_back(c::newUuid());
            // One identical instant, safely in the past (revisions must not precede their product).
            const std::string tiedAt = daily::day(observer, -1) + " 08:00:00.123456+08";
            auto tx = c::acquireTx(pool);
            for (size_t i = 0; i < tied.size(); ++i) {
                daily::checkedExec(tx, "INSERT INTO reward_products (id, couple_id, creator_id, owner_id, created_at) VALUES ('" +
                    tied[i] + "', '" + s.couple + "', '" + s.userA + "', '" + s.userB +
                    "', TIMESTAMPTZ '" + tiedAt + "')");
                daily::checkedExec(tx, "INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, "
                    "title, price_flowers, redemption_mode, status, idempotency_key, request_fingerprint) VALUES ('" + tied[i] +
                    "', '" + s.couple + "', 1, '" + s.userA + "', 'create', 'M7 tied " + std::to_string(i) +
                    "', 5, 'direct', 'active', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')");
            }
            daily::checkedExec(tx, "INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, "
                "title, price_flowers, redemption_mode, status, idempotency_key, request_fingerprint) VALUES ('" + tied[2] +
                "', '" + s.couple + "', 2, '" + s.userA + "', 'archive', 'M7 tied 2', 5, 'direct', 'archived', '" +
                task::taskKey() + "', '" + task::taskFingerprint() + "')");
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("tied fixture commit unconfirmed");
            const auto newer = create(s.userA, terms("M7 newer", 5));
            const std::string newest = newer ? newer->product.id : "";

            // Expected order: newest first, then the tied ones by id DESC.
            auto sortedTied = tied;
            std::sort(sortedTied.begin(), sortedTied.end(), std::greater<std::string>());
            std::vector<std::string> creatorExpected{newest};
            creatorExpected.insert(creatorExpected.end(), sortedTied.begin(), sortedTied.end());
            std::vector<std::string> ownerExpected;
            for (const auto &id : creatorExpected) if (id != tied[2]) ownerExpected.push_back(id);

            int pages = 0;
            CHECK(allPages(s.userA, 2, pages) == creatorExpected);  // creator: archived included
            CHECK_EQ(pages, 3);
            CHECK(allPages(s.userB, 1, pages) == ownerExpected);    // owner: archived never on any page
            CHECK_EQ(pages, 4);
            const auto first = list(s.userB, 4);
            CHECK(first.has_value());
            if (first) {
                CHECK_EQ(first->products.size(), 4u);
                CHECK_FALSE(first->nextCursor.has_value());  // exactly one full page: no extra cursor
            }
            const auto invalid = list(s.userB, 0);
            CHECK(invalid.has_value());
            if (invalid) CHECK(invalid->outcome == PO::Validation);
        } catch (const std::exception &) { c::expectThat(false, "m7-pages", "case failed (details withheld)"); }
    }

    TEST_CASE("M7: direct SQL guards (owner revision, version gap, mutation, product without revision, self-owned)") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto made = create(s.userA, terms("M7 guard", 5));
            const std::string id = made ? made->product.id : "";
            auto revision = [&](const std::string &actor, int version) {
                return "INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, "
                       "price_flowers, redemption_mode, status, idempotency_key, request_fingerprint) VALUES ('" + id +
                       "', '" + s.couple + "', " + std::to_string(version) + ", '" + actor +
                       "', 'edit', 'x', 5, 'direct', 'active', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            for (const auto &[sql, state] : std::vector<std::pair<std::string, std::string>>{
                     {revision(s.userB, 2), "UR002"},
                     {revision(s.userA, 3), "UR002"},
                     {"UPDATE reward_product_revisions SET price_flowers = 6 WHERE product_id = '" + id + "'", "42501"},
                     {"DELETE FROM reward_products WHERE id = '" + id + "'", "42501"},
                     {"INSERT INTO reward_products (couple_id, creator_id, owner_id) VALUES ('" + s.couple + "', '" +
                          s.userA + "', '" + s.userA + "')", "23514"}}) {
                auto tx = c::acquireTx(pool);
                const auto r = wallet::exec(tx, wallet::expectFailureSql(sql));
                CHECK(!r.ok);
                CHECK_EQ(wallet::parseFailure(r.error).sqlState, state);
                c::rollbackTx(tx);
            }
            auto tx = c::acquireTx(pool);
            const auto inserted = wallet::exec(tx, "INSERT INTO reward_products (couple_id, creator_id, owner_id) VALUES ('" +
                                                       s.couple + "', '" + s.userA + "', '" + s.userB + "')");
            CHECK(inserted.ok);
            const auto deferred = wallet::exec(tx, "SET CONSTRAINTS ALL IMMEDIATE");
            CHECK(!deferred.ok);
            CHECK(deferred.error.find("USAPP_PRODUCT_CREATE_REVISION") != std::string::npos);
            c::rollbackTx(tx);
        } catch (const std::exception &) { c::expectThat(false, "m7-sql", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::product
