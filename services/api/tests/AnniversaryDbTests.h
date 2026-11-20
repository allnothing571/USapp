#pragma once

// P2-2: real AnniversaryService on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test spaces); never resets.
#include <algorithm>
#include <functional>

#include "MessageDbTests.h"
#include "services/AnniversaryService.h"

namespace usapptest::anniversary {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using AS = usapp::AnniversaryService;
using AO = AS::Outcome;
using PendingStatus = ts::Pending<AS::Status>;

inline AS::Terms terms(const std::string &title, const std::string &date, const std::string &kind,
                       const std::string &note = "") {
    return AS::Terms{title, date, kind, note};
}
inline std::shared_ptr<PendingStatus> startCreate(const std::string &user, const AS::Terms &t, const std::string &key) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().create(user, key, t, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<AS::Status> create(const std::string &user, const AS::Terms &t,
                                        const std::string &key = c::newUuid()) {
    return startCreate(user, t, key)->wait();
}
inline std::optional<AS::Status> edit(const std::string &user, const std::string &id, int expected,
                                      const AS::Terms &t, const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().edit(user, id, key, expected, t, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<AS::Status> propose(const std::string &user, const std::string &id, int expected,
                                         const AS::Terms &t, const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().propose(user, id, key, expected, t, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::shared_ptr<PendingStatus> startDecide(const std::string &user, const std::string &id,
                                                  const std::string &proposal, AS::Decision decision,
                                                  const std::string &key) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().decide(user, id, proposal, key, decision, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<AS::Status> decide(const std::string &user, const std::string &id, const std::string &proposal,
                                        AS::Decision decision, const std::string &key = c::newUuid()) {
    return startDecide(user, id, proposal, decision, key)->wait();
}
inline std::optional<AS::Status> pin(const std::string &user, const std::optional<std::string> &id,
                                     const std::string &key = c::newUuid()) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().pin(user, key, id, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<AS::Status> get(const std::string &user, const std::string &id) {
    auto pending = std::make_shared<PendingStatus>();
    AS::instance().get(user, id, [pending](AS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<AS::ListResult> list(const std::string &user, int limit = 100,
                                          const std::optional<AS::Cursor> &cursor = std::nullopt) {
    auto pending = std::make_shared<ts::Pending<AS::ListResult>>();
    AS::instance().list(user, limit, cursor, [pending](AS::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
inline std::optional<AS::HomeResult> home(const std::string &user) {
    auto pending = std::make_shared<ts::Pending<AS::HomeResult>>();
    AS::instance().home(user, [pending](AS::HomeResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
inline std::vector<std::string> allPages(const std::string &user, int limit, int &pages) {
    std::vector<std::string> ids;
    std::optional<AS::Cursor> cursor;
    pages = 0;
    for (int i = 0; i < 10; ++i) {
        const auto r = list(user, limit, cursor);
        if (!r || r->outcome != AO::Ok) throw std::runtime_error("page failed");
        ++pages;
        for (const auto &a : r->anniversaries) ids.push_back(a.id);
        if (!r->nextCursor) return ids;
        cursor = AS::decodeCursor(*r->nextCursor);
        if (!cursor) throw std::runtime_error("bad next cursor");
    }
    throw std::runtime_error("too many pages");
}
inline bool is(const std::optional<AS::Status> &s, AO o) { return s.has_value() && s->outcome == o; }
inline std::string facts(const AS::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, s.mutation);
}
inline long long count(const c::Observer &o, const std::string &sql, const std::string &id) {
    return o.client->execSqlSync(sql, id)[0]["n"].as<long long>();
}
inline std::string idOf(const std::optional<AS::Status> &s) { return s ? s->data.id : std::string(); }

inline void runAnniversaryDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    using D = AS::Decision;

    TEST_CASE("P2-2: either member creates (also before the partner joins); both read; other spaces see nothing") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            ts::CommittedSpace alone;
            ts::commitSpace(pool, alone, false);
            CHECK(is(create(alone.userA, terms("一个人也能记", "2020-01-01", "anniversary")), AO::Created));

            const auto a = create(s.userA, terms("在一起", "2020-05-20", "anniversary", "第一次见面\n在公园"));
            CHECK(is(a, AO::Created));
            const std::string id = idOf(a);
            if (a) {
                CHECK_EQ(a->data.creatorId, s.userA);
                CHECK_EQ(a->data.version, 1);
                CHECK_EQ(a->data.terms.note, std::string("第一次见面\n在公园"));
                CHECK_EQ(a->mutation["action"].asString(), std::string("create"));
                CHECK_EQ(a->mutation["version"].asInt(), 1);
                CHECK_EQ(a->asOfDate, daily::day(observer));
                CHECK_FALSE(a->data.pending.has_value());
            }
            CHECK(is(create(s.userB, terms("考试", daily::day(observer, 30), "countdown")), AO::Created));
            const auto seen = get(s.userB, id);
            CHECK(is(seen, AO::Ok));
            if (seen) CHECK_EQ(seen->data.terms.title, std::string("在一起"));
            CHECK(is(get(other.userA, id), AO::NotFound));
            const auto otherList = list(other.userA);
            CHECK(otherList.has_value());
            if (otherList) CHECK(otherList->anniversaries.empty());
            CHECK(is(create(c::newUuid(), terms("x", "2020-01-01", "countdown")), AO::SpaceNotFound));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'anniversary.created' "
                                     "AND target_type = 'anniversary' AND target_id = $1::text", id), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text "
                                     "AND detail::text LIKE '%在一起%'", id), 0);  // audits never hold text
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id = $1::uuid",
                           s.couple), 0);
        } catch (const std::exception &) { c::expectThat(false, "p22-create", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: same-key replay, key reuse, and concurrent same-key creates write one item") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto t = terms("生日", "1998-02-28", "anniversary");
            const auto key = c::newUuid();
            const auto first = create(s.userA, t, key);
            CHECK(is(first, AO::Created));
            const auto again = create(s.userA, t, key);
            CHECK(is(again, AO::Replayed));
            if (first && again) {
                CHECK_EQ(facts(*again), facts(*first));
                CHECK_EQ(again->data.id, first->data.id);
            }
            CHECK(is(create(s.userA, terms("生日", "1998-03-01", "anniversary"), key), AO::KeyReused));
            CHECK(is(create(s.userB, t, key), AO::Created));  // keys are scoped per actor

            const auto racing = c::newUuid();
            const auto p = startCreate(s.userA, terms("并发", "2026-12-31", "countdown"), racing);
            const auto q = startCreate(s.userA, terms("并发", "2026-12-31", "countdown"), racing);
            const auto u = p->wait(), v = q->wait();
            CHECK((is(u, AO::Created) && is(v, AO::Replayed)) || (is(v, AO::Created) && is(u, AO::Replayed)));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE idempotency_key = $1",
                           racing), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM anniversaries a WHERE a.couple_id = $1::uuid "
                                     "AND NOT EXISTS (SELECT 1 FROM anniversary_revisions r WHERE r.anniversary_id = a.id)",
                           s.couple), 0);
        } catch (const std::exception &) { c::expectThat(false, "p22-replay", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: creator edits with expected_version; a committed edit replays; partner and creator roles") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto made = create(s.userA, terms("纪念", "2021-06-01", "anniversary"));
            const std::string id = idOf(made);
            const auto key = c::newUuid();
            const auto e = edit(s.userA, id, 1, terms("纪念日", "2021-06-02", "anniversary", "改了"), key);
            CHECK(is(e, AO::Created));
            if (e) {
                CHECK_EQ(e->data.version, 2);
                CHECK_EQ(e->data.terms.date, std::string("2021-06-02"));
                CHECK_EQ(e->mutation["action"].asString(), std::string("edit"));
                CHECK_EQ(e->mutation["version"].asInt(), 2);
            }
            // The same key after the version moved replays instead of conflicting.
            const auto replay = edit(s.userA, id, 1, terms("纪念日", "2021-06-02", "anniversary", "改了"), key);
            CHECK(is(replay, AO::Replayed));
            if (replay && e) CHECK_EQ(facts(*replay), facts(*e));
            CHECK(is(edit(s.userA, id, 1, terms("旧版本", "2021-06-02", "anniversary")), AO::VersionConflict));
            CHECK(is(edit(s.userA, id, 2, terms("纪念日", "2021-06-02", "anniversary", "改了")), AO::Validation));
            CHECK(is(edit(s.userB, id, 2, terms("对方直接改", "2021-06-02", "anniversary")), AO::Forbidden));
            CHECK(is(propose(s.userA, id, 2, terms("自己申请", "2021-06-02", "anniversary")), AO::Forbidden));
            CHECK(is(edit(s.userA, id, 2, terms("倒数", "2027-06-02", "countdown")), AO::Created));  // kind change
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM anniversary_revisions WHERE anniversary_id = $1::uuid",
                           id), 3);
        } catch (const std::exception &) { c::expectThat(false, "p22-edit", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: proposals: one pending, creator edit blocked, approve applies exact terms, reject/withdraw") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto made = create(s.userA, terms("第一次旅行", "2022-08-08", "anniversary"));
            const std::string id = idOf(made);
            const auto proposed = terms("第一次旅行（大理）", "2022-08-09", "anniversary", "日期记错了");
            CHECK(is(propose(s.userB, id, 1, terms("第一次旅行", "2022-08-08", "anniversary")), AO::Validation));
            CHECK(is(propose(s.userB, id, 2, proposed), AO::VersionConflict));
            const auto pkey = c::newUuid();
            const auto p = propose(s.userB, id, 1, proposed, pkey);
            CHECK(is(p, AO::Created));
            const std::string pid = p ? p->mutation["proposal_id"].asString() : "";
            if (p) {
                CHECK(p->data.pending.has_value());
                if (p->data.pending) {
                    CHECK_EQ(p->data.pending->id, pid);
                    CHECK_EQ(p->data.pending->baseVersion, 1);
                    CHECK(p->data.pending->terms == proposed);
                }
                CHECK_EQ(p->data.version, 1);  // nothing applied yet
            }
            const auto preplay = propose(s.userB, id, 1, proposed, pkey);
            CHECK(is(preplay, AO::Replayed));
            if (preplay && p) CHECK_EQ(facts(*preplay), facts(*p));
            CHECK(is(propose(s.userB, id, 1, terms("第二个申请", "2022-08-09", "anniversary")), AO::ProposalPending));
            CHECK(is(edit(s.userA, id, 1, terms("创建者想改", "2022-08-09", "anniversary")), AO::ProposalPending));
            // Decision actor matrix.
            CHECK(is(decide(s.userB, id, pid, D::Approve), AO::Forbidden));
            CHECK(is(decide(s.userB, id, pid, D::Reject), AO::Forbidden));
            CHECK(is(decide(s.userA, id, pid, D::Withdraw), AO::Forbidden));
            CHECK(is(decide(other.userA, id, pid, D::Approve), AO::NotFound));
            CHECK(is(decide(s.userA, id, c::newUuid(), D::Approve), AO::ProposalNotFound));
            const auto second = create(s.userA, terms("另一个", "2022-01-01", "anniversary"));
            CHECK(is(decide(s.userA, idOf(second), pid, D::Approve), AO::ProposalNotFound));  // proposal of another item

            const auto akey = c::newUuid();
            const auto approved = decide(s.userA, id, pid, D::Approve, akey);
            CHECK(is(approved, AO::Ok));
            if (approved) {
                CHECK_EQ(approved->data.version, 2);
                CHECK(approved->data.terms == proposed);
                CHECK_FALSE(approved->data.pending.has_value());
                CHECK(approved->data.lastDecision.has_value());
                if (approved->data.lastDecision) CHECK_EQ(approved->data.lastDecision->outcome, std::string("approved"));
                CHECK_EQ(approved->mutation["outcome"].asString(), std::string("approved"));
                CHECK_EQ(approved->mutation["version"].asInt(), 2);
                CHECK_EQ(approved->mutation["proposal_id"].asString(), pid);
            }
            const auto areplay = decide(s.userA, id, pid, D::Approve, akey);
            CHECK(is(areplay, AO::Replayed));
            if (areplay && approved) CHECK_EQ(facts(*areplay), facts(*approved));
            CHECK(is(decide(s.userA, id, pid, D::Reject), AO::ProposalDecided));
            CHECK(is(decide(s.userB, id, pid, D::Withdraw), AO::ProposalDecided));

            // Reject and withdraw change nothing else.
            const auto p2 = propose(s.userB, id, 2, terms("再改一次", "2022-08-09", "anniversary"));
            const std::string pid2 = p2 ? p2->mutation["proposal_id"].asString() : "";
            const auto rejected = decide(s.userA, id, pid2, D::Reject);
            CHECK(is(rejected, AO::Ok));
            if (rejected) {
                CHECK_EQ(rejected->data.version, 2);
                CHECK(rejected->mutation["version"].isNull());
            }
            const auto p3 = propose(s.userB, id, 2, terms("最后一次", "2022-08-09", "anniversary"));
            const std::string pid3 = p3 ? p3->mutation["proposal_id"].asString() : "";
            const auto withdrawn = decide(s.userB, id, pid3, D::Withdraw);
            CHECK(is(withdrawn, AO::Ok));
            if (withdrawn) {
                CHECK_EQ(withdrawn->data.version, 2);
                CHECK(withdrawn->data.lastDecision.has_value());
                if (withdrawn->data.lastDecision) CHECK_EQ(withdrawn->data.lastDecision->outcome, std::string("withdrawn"));
            }
            CHECK(is(edit(s.userA, id, 2, terms("创建者现在能改", "2022-08-09", "anniversary")), AO::Created));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1::text "
                                     "AND action IN ('anniversary.edit_proposed', 'anniversary.edit_approved', "
                                     "'anniversary.edit_rejected', 'anniversary.edit_withdrawn')", id), 6);
        } catch (const std::exception &) { c::expectThat(false, "p22-proposals", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: concurrent approve and withdraw: exactly one decision wins") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            for (int round = 0; round < 3; ++round) {
                const auto made = create(s.userA, terms("竞争", "2023-03-03", "anniversary"));
                const std::string id = idOf(made);
                const auto p = propose(s.userB, id, 1, terms("竞争改", "2023-03-04", "anniversary"));
                const std::string pid = p ? p->mutation["proposal_id"].asString() : "";
                const auto x = startDecide(s.userA, id, pid, D::Approve, c::newUuid());
                const auto y = startDecide(s.userB, id, pid, D::Withdraw, c::newUuid());
                const auto rx = x->wait(), ry = y->wait();
                CHECK((is(rx, AO::Ok) && is(ry, AO::ProposalDecided)) || (is(ry, AO::Ok) && is(rx, AO::ProposalDecided)));
                CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM anniversary_edit_decisions WHERE proposal_id = $1::uuid",
                               pid), 1);
                const auto now = get(s.userA, id);
                CHECK(is(now, AO::Ok));
                if (now) CHECK_EQ(now->data.version, is(rx, AO::Ok) ? 2 : 1);
            }
        } catch (const std::exception &) { c::expectThat(false, "p22-race", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: cross-space ids give 404 for read, edit, propose, decide and pin") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto made = create(s.userA, terms("私密", "2020-02-02", "anniversary"));
            const std::string id = idOf(made);
            const auto p = propose(s.userB, id, 1, terms("私密改", "2020-02-03", "anniversary"));
            const std::string pid = p ? p->mutation["proposal_id"].asString() : "";
            CHECK(is(get(other.userA, id), AO::NotFound));
            CHECK(is(edit(other.userA, id, 1, terms("越界", "2020-02-02", "anniversary")), AO::NotFound));
            CHECK(is(propose(other.userB, id, 1, terms("越界", "2020-02-02", "anniversary")), AO::NotFound));
            CHECK(is(decide(other.userA, id, pid, D::Approve), AO::NotFound));
            CHECK(is(pin(other.userA, id), AO::NotFound));
            CHECK(is(pin(s.userA, c::newUuid()), AO::NotFound));
            const auto still = get(s.userA, id);
            CHECK(is(still, AO::Ok));
            if (still) {
                CHECK_EQ(still->data.version, 1);
                CHECK(still->data.pending.has_value());
            }
        } catch (const std::exception &) { c::expectThat(false, "p22-cross-space", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: Home sources, per-user pins, and a pin replay never restores an older pin") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            auto h = home(s.userA);
            CHECK(h.has_value());
            if (h) {
                CHECK(h->outcome == AO::Ok);
                CHECK_EQ(h->source, std::string("none"));
                CHECK_FALSE(h->anniversary.has_value());
                CHECK_EQ(h->asOfDate, daily::day(observer));
            }
            // Only past countdowns: the most recently passed.
            const auto older = create(s.userA, terms("过去 5 天", daily::day(observer, -5), "countdown"));
            const auto recent = create(s.userB, terms("过去 2 天", daily::day(observer, -2), "countdown"));
            h = home(s.userA);
            CHECK(h.has_value());
            if (h) {
                CHECK_EQ(h->source, std::string("auto_past"));
                if (h->anniversary) CHECK_EQ(h->anniversary->id, idOf(recent));
            }
            // An upcoming item wins over past countdowns; the nearest one is chosen.
            const auto soon = create(s.userA, terms("三天后", daily::day(observer, 3), "countdown"));
            const auto later = create(s.userB, terms("五天后开始", daily::day(observer, 5), "anniversary"));
            for (const auto &user : {s.userA, s.userB}) {
                h = home(user);
                CHECK(h.has_value());
                if (h) {
                    CHECK_EQ(h->source, std::string("auto"));
                    if (h->anniversary) CHECK_EQ(h->anniversary->id, idOf(soon));
                }
            }
            // A pins an item for A's own Home; B is unaffected and never sees A's pin.
            const auto k1 = c::newUuid();
            const auto first = pin(s.userA, idOf(later), k1);
            CHECK(is(first, AO::Ok));
            if (first) {
                CHECK(first->isPin);
                CHECK_EQ(first->pin.seq, 1);
                CHECK(first->pin.anniversaryId == std::optional<std::string>(idOf(later)));
                CHECK_EQ(first->mutation["seq"].asInt(), 1);
            }
            h = home(s.userA);
            if (h) {
                CHECK_EQ(h->source, std::string("pinned"));
                if (h->anniversary) {
                    CHECK_EQ(h->anniversary->id, idOf(later));
                    CHECK(h->anniversary->pinnedByMe);
                }
            }
            h = home(s.userB);
            if (h) {
                CHECK_EQ(h->source, std::string("auto"));
                if (h->anniversary) CHECK_EQ(h->anniversary->id, idOf(soon));
            }
            const auto seenByB = get(s.userB, idOf(later));
            if (seenByB) CHECK_FALSE(seenByB->data.pinnedByMe);
            // A newer selection, then a replay of the older key: original facts, no new row, no restore.
            const auto second = pin(s.userA, idOf(older));
            CHECK(is(second, AO::Ok));
            const auto replay = pin(s.userA, idOf(later), k1);
            CHECK(is(replay, AO::Replayed));
            if (replay && first) {
                CHECK_EQ(facts(*replay), facts(*first));
                CHECK_EQ(replay->pin.seq, 2);
                CHECK(replay->pin.anniversaryId == std::optional<std::string>(idOf(older)));
            }
            CHECK(is(pin(s.userA, idOf(soon), k1), AO::KeyReused));
            h = home(s.userA);
            if (h && h->anniversary) CHECK_EQ(h->anniversary->id, idOf(older));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM anniversary_home_pins WHERE user_id = $1::uuid",
                           s.userA), 2);
            // Clearing returns to automatic selection.
            const auto cleared = pin(s.userA, std::nullopt);
            CHECK(is(cleared, AO::Ok));
            if (cleared) CHECK_FALSE(cleared->pin.anniversaryId.has_value());
            h = home(s.userA);
            if (h) CHECK_EQ(h->source, std::string("auto"));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'anniversary.home_pinned' "
                                     "AND actor_id = $1::uuid", s.userA), 3);
        } catch (const std::exception &) { c::expectThat(false, "p22-home", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: list pages in (created_at, id) order with tied timestamps; edits never move a row") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            std::vector<std::string> tied;
            for (int i = 0; i < 4; ++i) tied.push_back(c::newUuid());
            const std::string tiedAt = daily::day(observer, -1) + " 08:00:00.123456+08";
            auto tx = c::acquireTx(pool);
            for (size_t i = 0; i < tied.size(); ++i) {
                daily::checkedExec(tx, "INSERT INTO anniversaries (id, couple_id, creator_id, created_at) VALUES ('" +
                    tied[i] + "', '" + s.couple + "', '" + s.userA + "', TIMESTAMPTZ '" + tiedAt + "')");
                daily::checkedExec(tx, "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, "
                    "action, title, event_date, kind, idempotency_key, request_fingerprint, created_at) VALUES ('" +
                    tied[i] + "', '" + s.couple + "', 1, '" + s.userA + "', 'create', 'tied " + std::to_string(i) +
                    "', DATE '2020-01-01', 'anniversary', '" + task::taskKey() + "', '" + task::taskFingerprint() +
                    "', TIMESTAMPTZ '" + tiedAt + "')");
            }
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("tied fixture commit unconfirmed");
            const auto newer = create(s.userA, terms("最新", "2021-01-01", "anniversary"));
            auto sortedTied = tied;
            std::sort(sortedTied.begin(), sortedTied.end(), std::greater<std::string>());
            std::vector<std::string> all{idOf(newer)};
            all.insert(all.end(), sortedTied.begin(), sortedTied.end());
            int pages = 0;
            CHECK(allPages(s.userB, 2, pages) == all);
            CHECK_EQ(pages, 3);
            CHECK(is(edit(s.userA, sortedTied.back(), 1, terms("改过", "2019-01-01", "countdown")), AO::Created));
            CHECK(allPages(s.userA, 1, pages) == all);
            CHECK_EQ(pages, 5);
            const auto invalid = list(s.userB, 101);
            CHECK(invalid.has_value());
            if (invalid) CHECK(invalid->outcome == AO::Validation);
        } catch (const std::exception &) { c::expectThat(false, "p22-pages", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-2: direct SQL guards (actor, version, pending, proposer, decision actor, pin seq, space, append-only)") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto made = create(s.userA, terms("守卫", "2020-03-03", "anniversary"));
            const std::string id = idOf(made);
            const auto otherMade = create(other.userA, terms("别人的", "2020-03-03", "anniversary"));
            const std::string otherId = idOf(otherMade);
            const auto p = propose(s.userB, id, 1, terms("守卫改", "2020-03-04", "anniversary"));
            const std::string pid = p ? p->mutation["proposal_id"].asString() : "";
            auto revisionSql = [&](const std::string &actor, int version) {
                return "INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, "
                       "event_date, kind, idempotency_key, request_fingerprint) VALUES ('" + id + "', '" + s.couple +
                       "', " + std::to_string(version) + ", '" + actor + "', 'edit', 'x', DATE '2020-03-03', "
                       "'anniversary', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            auto proposalSql = [&](const std::string &proposer, int base) {
                return "INSERT INTO anniversary_edit_proposals (couple_id, anniversary_id, proposer_id, base_version, "
                       "title, event_date, kind, idempotency_key, request_fingerprint) VALUES ('" + s.couple + "', '" +
                       id + "', '" + proposer + "', " + std::to_string(base) + ", 'x', DATE '2020-03-03', "
                       "'anniversary', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            auto decisionSql = [&](const std::string &by, const std::string &outcome) {
                return "INSERT INTO anniversary_edit_decisions (proposal_id, couple_id, decided_by, outcome, "
                       "idempotency_key, request_fingerprint) VALUES ('" + pid + "', '" + s.couple + "', '" + by +
                       "', '" + outcome + "', '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            auto pinSql = [&](const std::string &seq, const std::string &target) {
                return "INSERT INTO anniversary_home_pins (user_id, seq, couple_id, anniversary_id, idempotency_key, "
                       "request_fingerprint) VALUES ('" + s.userA + "', " + seq + ", '" + s.couple + "', " + target +
                       ", '" + task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            for (const auto &[sql, state] : std::vector<std::pair<std::string, std::string>>{
                     {revisionSql(s.userB, 2), "UN002"},                    // partner revision
                     {revisionSql(s.userA, 3), "UN002"},                    // version gap
                     {revisionSql(s.userA, 2), "UN002"},                    // edit while a proposal is pending
                     {proposalSql(s.userA, 1), "UN003"},                    // creator proposing
                     {proposalSql(s.userB, 1), "UN003"},                    // second pending proposal
                     {decisionSql(s.userB, "approved"), "UN004"},           // partner approving
                     {decisionSql(s.userA, "withdrawn"), "UN004"},          // creator withdrawing
                     {pinSql("2", "NULL"), "UN006"},                        // pin seq gap
                     {pinSql("1", "'" + otherId + "'"), "23503"},           // another space's item
                     {"UPDATE anniversary_revisions SET title = 'changed' WHERE anniversary_id = '" + id + "'", "42501"},
                     {"DELETE FROM anniversary_edit_proposals WHERE id = '" + pid + "'", "42501"}}) {
                auto tx = c::acquireTx(pool);
                const auto r = wallet::exec(tx, wallet::expectFailureSql(sql));
                CHECK(!r.ok);
                CHECK_EQ(wallet::parseFailure(r.error).sqlState, state);
                c::rollbackTx(tx);
            }
            const auto still = get(s.userA, id);
            CHECK(is(still, AO::Ok));
            if (still) {
                CHECK_EQ(still->data.terms.title, std::string("守卫"));
                CHECK_EQ(still->data.version, 1);
            }
        } catch (const std::exception &) { c::expectThat(false, "p22-sql", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::anniversary
