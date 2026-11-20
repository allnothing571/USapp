#pragma once

// P2-1: real MessageService on literal usapp_test, after the migration/database guard.
// Isolated committed fixtures (@m3-fixture.test spaces); never resets.
#include <algorithm>
#include <functional>

#include "ProductDbTests.h"
#include "services/MessageService.h"

namespace usapptest::message {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using MS = usapp::MessageService;
using MO = MS::Outcome;
using PendingMessage = ts::Pending<MS::Status>;

inline std::shared_ptr<PendingMessage> startCreate(const std::string &user, const std::string &text,
                                                   const std::string &key) {
    auto pending = std::make_shared<PendingMessage>();
    MS::instance().create(user, key, text, [pending](MS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<MS::Status> create(const std::string &user, const std::string &text,
                                        const std::string &key = c::newUuid()) {
    return startCreate(user, text, key)->wait();
}
inline std::shared_ptr<PendingMessage> startReply(const std::string &user, const std::string &id,
                                                  const std::string &text, const std::string &key) {
    auto pending = std::make_shared<PendingMessage>();
    MS::instance().reply(user, id, key, text, [pending](MS::Status s) { pending->deliver(std::move(s)); });
    return pending;
}
inline std::optional<MS::Status> reply(const std::string &user, const std::string &id, const std::string &text,
                                       const std::string &key = c::newUuid()) {
    return startReply(user, id, text, key)->wait();
}
inline std::optional<MS::Status> get(const std::string &user, const std::string &id) {
    auto pending = std::make_shared<PendingMessage>();
    MS::instance().get(user, id, [pending](MS::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}
inline std::optional<MS::ListResult> list(const std::string &user, bool partnerOnly = false, int limit = 100,
                                          const std::optional<MS::Cursor> &cursor = std::nullopt) {
    auto pending = std::make_shared<ts::Pending<MS::ListResult>>();
    MS::instance().list(user, partnerOnly, limit, cursor, [pending](MS::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}
inline std::vector<std::string> allPages(const std::string &user, bool partnerOnly, int limit, int &pages) {
    std::vector<std::string> ids;
    std::optional<MS::Cursor> cursor;
    pages = 0;
    for (int i = 0; i < 10; ++i) {
        const auto r = list(user, partnerOnly, limit, cursor);
        if (!r || r->outcome != MO::Ok) throw std::runtime_error("page failed");
        ++pages;
        for (const auto &m : r->messages) ids.push_back(m.id);
        if (!r->nextCursor) return ids;
        cursor = MS::decodeCursor(*r->nextCursor);
        if (!cursor) throw std::runtime_error("bad next cursor");
    }
    throw std::runtime_error("too many pages");
}
inline bool is(const std::optional<MS::Status> &s, MO o) { return s.has_value() && s->outcome == o; }
inline std::string facts(const MS::Status &s) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, s.mutation);
}
inline long long count(const c::Observer &o, const std::string &sql, const std::string &id) {
    return o.client->execSqlSync(sql, id)[0]["n"].as<long long>();
}
inline int listed(const std::optional<MS::ListResult> &r, const std::string &id) {
    int n = 0;
    if (r) for (const auto &m : r->messages) n += m.id == id ? 1 : 0;
    return n;
}

inline void runMessageDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);

    TEST_CASE("P2-1: either member writes to the partner; both read; other spaces see nothing; audited") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            ts::CommittedSpace alone;
            ts::commitSpace(pool, alone, false);
            CHECK(is(create(alone.userA, "无人可收"), MO::PartnerRequired));

            const auto a = create(s.userA, "今天面试加油\n不许紧张");
            CHECK(is(a, MO::Created));
            const std::string id = a ? a->data.id : "";
            if (a) {
                CHECK_EQ(a->data.authorId, s.userA);
                CHECK_EQ(a->data.recipientId, s.userB);
                CHECK_FALSE(a->data.hasReply);
                CHECK_EQ(a->mutation["action"].asString(), std::string("create"));
                CHECK_EQ(a->mutation["body"].asString(), std::string("今天面试加油\n不许紧张"));
            }
            const auto b = create(s.userB, "收到啦");
            CHECK(is(b, MO::Created));
            if (b) CHECK_EQ(b->data.recipientId, s.userA);
            CHECK_EQ(listed(list(s.userA), id), 1);
            CHECK_EQ(listed(list(s.userB), id), 1);
            CHECK(is(get(s.userB, id), MO::Ok));
            CHECK(is(get(other.userA, id), MO::NotFound));
            CHECK_EQ(listed(list(other.userA), id), 0);
            CHECK(is(reply(other.userB, id, "越界"), MO::NotFound));
            // from=partner: only messages written to the caller.
            const auto toB = list(s.userB, true);
            CHECK(toB.has_value());
            if (toB) {
                CHECK_EQ(listed(toB, id), 1);
                for (const auto &m : toB->messages) CHECK_EQ(m.authorId, s.userA);
            }
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'message.created' "
                                     "AND target_type = 'message' AND target_id = $1::text", id), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id = $1::uuid",
                           s.couple), 0);
        } catch (const std::exception &) { c::expectThat(false, "p21-visibility", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-1: same-key replay, key reuse, and concurrent same-key creates write one message") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto key = c::newUuid();
            const auto first = create(s.userA, "一张小纸条", key);
            CHECK(is(first, MO::Created));
            const auto again = create(s.userA, "一张小纸条", key);
            CHECK(is(again, MO::Replayed));
            if (first && again) {
                CHECK_EQ(facts(*again), facts(*first));
                CHECK_EQ(again->data.id, first->data.id);
            }
            CHECK(is(create(s.userA, "另一张", key), MO::KeyReused));
            CHECK(is(create(s.userB, "一张小纸条", key), MO::Created));  // keys are scoped per author

            const auto racing = c::newUuid();
            const auto p = startCreate(s.userA, "并发", racing);
            const auto q = startCreate(s.userA, "并发", racing);
            const auto u = p->wait(), v = q->wait();
            CHECK((is(u, MO::Created) && is(v, MO::Replayed)) || (is(v, MO::Created) && is(u, MO::Replayed)));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM messages WHERE idempotency_key = $1", racing), 1);
        } catch (const std::exception &) { c::expectThat(false, "p21-replay", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-1: only the recipient replies, once; replay; concurrent replies give exactly one") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto made = create(s.userA, "晚上一起散步吗");
            const std::string id = made ? made->data.id : "";
            CHECK(is(reply(s.userA, id, "自己回自己"), MO::Forbidden));
            const auto key = c::newUuid();
            const auto r = reply(s.userB, id, "好呀", key);
            CHECK(is(r, MO::Created));
            if (r) {
                CHECK(r->data.hasReply);
                CHECK_EQ(r->data.replyAuthorId, s.userB);
                CHECK_EQ(r->data.replyBody, std::string("好呀"));
                CHECK_EQ(r->mutation["action"].asString(), std::string("reply"));
            }
            const auto replay = reply(s.userB, id, "好呀", key);
            CHECK(is(replay, MO::Replayed));
            if (replay && r) CHECK_EQ(facts(*replay), facts(*r));
            CHECK(is(reply(s.userB, id, "不同的内容", key), MO::KeyReused));
            CHECK(is(reply(s.userB, id, "再回一次"), MO::AlreadyReplied));
            const auto seen = get(s.userA, id);
            CHECK(is(seen, MO::Ok));
            if (seen) CHECK_EQ(seen->data.replyBody, std::string("好呀"));

            const auto second = create(s.userA, "并发回复");
            const std::string sid = second ? second->data.id : "";
            const auto x = startReply(s.userB, sid, "第一句", c::newUuid());
            const auto y = startReply(s.userB, sid, "第二句", c::newUuid());
            const auto rx = x->wait(), ry = y->wait();
            CHECK((is(rx, MO::Created) && is(ry, MO::AlreadyReplied)) ||
                  (is(ry, MO::Created) && is(rx, MO::AlreadyReplied)));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM message_replies WHERE message_id = $1::uuid", sid), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'message.replied' "
                                     "AND target_id = $1::text", sid), 1);
        } catch (const std::exception &) { c::expectThat(false, "p21-reply", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-1: cursor pages in (created_at, id) order with tied timestamps; partner filter on every page") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            // Four messages with one identical past created_at: two from A, two from B.
            std::vector<std::string> tied;
            for (int i = 0; i < 4; ++i) tied.push_back(c::newUuid());
            const std::string tiedAt = daily::day(observer, -1) + " 08:00:00.123456+08";
            auto tx = c::acquireTx(pool);
            for (size_t i = 0; i < tied.size(); ++i) {
                const bool fromA = i % 2 == 0;
                daily::checkedExec(tx, "INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, "
                    "request_fingerprint, created_at) VALUES ('" + tied[i] + "', '" + s.couple + "', '" +
                    (fromA ? s.userA : s.userB) + "', '" + (fromA ? s.userB : s.userA) + "', 'tied " + std::to_string(i) +
                    "', '" + task::taskKey() + "', '" + task::taskFingerprint() + "', TIMESTAMPTZ '" + tiedAt + "')");
            }
            const auto committed = c::commitTx(tx);
            if (!committed || !*committed) throw std::runtime_error("tied fixture commit unconfirmed");
            const auto newer = create(s.userA, "最新");
            const std::string newest = newer ? newer->data.id : "";

            auto sortedTied = tied;
            std::sort(sortedTied.begin(), sortedTied.end(), std::greater<std::string>());
            std::vector<std::string> all{newest};
            all.insert(all.end(), sortedTied.begin(), sortedTied.end());
            std::vector<std::string> toB;
            for (const auto &id : all) {
                if (id == newest || id == tied[0] || id == tied[2]) toB.push_back(id);
            }
            int pages = 0;
            CHECK(allPages(s.userA, false, 2, pages) == all);
            CHECK_EQ(pages, 3);
            CHECK(allPages(s.userB, true, 1, pages) == toB);
            CHECK_EQ(pages, 3);
            const auto exact = list(s.userB, false, 5);
            CHECK(exact.has_value());
            if (exact) {
                CHECK_EQ(exact->messages.size(), 5u);
                CHECK_FALSE(exact->nextCursor.has_value());
            }
            const auto invalid = list(s.userB, false, 101);
            CHECK(invalid.has_value());
            if (invalid) CHECK(invalid->outcome == MO::Validation);
        } catch (const std::exception &) { c::expectThat(false, "p21-pages", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-1: direct SQL guards (reply actor, couple, future time, self-message, append-only, one reply)") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto made = create(s.userA, "守卫测试");
            const std::string id = made ? made->data.id : "";
            CHECK(is(reply(s.userB, id, "已回复"), MO::Created));
            const auto otherMade = create(other.userA, "别人的留言");
            const std::string otherId = otherMade ? otherMade->data.id : "";
            auto replySql = [&](const std::string &message, const std::string &couple, const std::string &author) {
                return "INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, "
                       "request_fingerprint) VALUES ('" + message + "', '" + couple + "', '" + author + "', 'x', '" +
                       task::taskKey() + "', '" + task::taskFingerprint() + "')";
            };
            auto messageSql = [&](const std::string &author, const std::string &recipient, const std::string &when) {
                return "INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, "
                       "request_fingerprint, created_at) VALUES ('" + s.couple + "', '" + author + "', '" + recipient +
                       "', 'x', '" + task::taskKey() + "', '" + task::taskFingerprint() + "', " + when + ")";
            };
            const auto unreplied = create(s.userB, "未回复");
            const std::string open = unreplied ? unreplied->data.id : "";
            for (const auto &[sql, state] : std::vector<std::pair<std::string, std::string>>{
                     {replySql(open, s.couple, s.userB), "UB002"},          // the author replying
                     {replySql(otherId, s.couple, s.userB), "UB002"},       // message of another space
                     {replySql(id, s.couple, s.userB), "23505"},            // second reply
                     {messageSql(s.userA, s.userA, "clock_timestamp()"), "23514"},
                     {messageSql(s.userA, other.userB, "clock_timestamp()"), "23503"},  // non-member recipient
                     {messageSql(s.userA, s.userB, "clock_timestamp() + interval '1 hour'"), "UB002"},
                     {"UPDATE messages SET body = 'changed' WHERE id = '" + id + "'", "42501"},
                     {"DELETE FROM message_replies WHERE message_id = '" + id + "'", "42501"}}) {
                auto tx = c::acquireTx(pool);
                const auto r = wallet::exec(tx, wallet::expectFailureSql(sql));
                CHECK(!r.ok);
                CHECK_EQ(wallet::parseFailure(r.error).sqlState, state);
                c::rollbackTx(tx);
            }
            const auto still = get(s.userA, id);
            CHECK(is(still, MO::Ok));
            if (still) CHECK_EQ(still->data.body, std::string("守卫测试"));
        } catch (const std::exception &) { c::expectThat(false, "p21-sql", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::message
