#pragma once

// Admin read-only task inspection (docs/admin-task-inspection.md), CTest target admin_task_db. The service directly, on the
// test database: states and history from real services (single and count completion with replays, a reaction, rejected,
// withdrawn and approved edits with a successor, a cancellation, a daily template and its stored period), an old expired
// row outside the public 30-day window, space states, a foreign task, keyset pages and cursor validation, the allowlist,
// status agreement with the public read, and an unchanged footprint.
#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "MediaDbTests.h"
#include "TaskReactionDbTests.h"
#include "services/AdminTaskService.h"

namespace usapptest::admintask {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace tc = usapptest::taskchange;
namespace tp = usapptest::taskprogress;
namespace dl = usapptest::daily;
namespace rx = usapptest::reaction;
namespace tk = usapptest::task;
using A = usapp::AdminTaskService;
using AO = A::Outcome;

inline std::optional<A::Result> listOf(const std::string &couple, const std::string &kind, int limit,
                                       const std::string &cursor = std::string()) {
    A::Cursor parsed;
    if (!cursor.empty() && !A::decodeCursor(cursor, couple, kind, parsed)) throw std::runtime_error("test cursor rejected");
    auto p = std::make_shared<ts::Pending<A::Result>>();
    A::list(couple, kind, limit, cursor.empty() ? nullptr : &parsed, [p](A::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::optional<A::Result> detailOf(const std::string &couple, const std::string &task) {
    auto p = std::make_shared<ts::Pending<A::Result>>();
    A::detail(couple, task, [p](A::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline bool ok(const std::optional<A::Result> &r) { return r && r->outcome == AO::Ok; }
inline bool is(const std::optional<A::Result> &r, AO o) { return r && r->outcome == o; }

inline std::string footprint(const c::Observer &o) {
    return o.client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM tasks), (SELECT count(*) FROM task_completions), "
        "(SELECT count(*) FROM task_progress_events), (SELECT count(*) FROM task_change_proposals), "
        "(SELECT count(*) FROM task_change_decisions), (SELECT count(*) FROM task_closures), "
        "(SELECT count(*) FROM task_reactions), (SELECT count(*) FROM wallet_operations), "
        "(SELECT count(*) FROM wallet_ledger_entries), (SELECT count(*) FROM audit_logs), "
        "(SELECT count(*) FROM notifications)) AS v")[0]["v"].as<std::string>();
}
inline void keys(const Json::Value &v, std::set<std::string> &out) {
    if (v.isObject()) for (const auto &k : v.getMemberNames()) { out.insert(k); keys(v[k], out); }
    else if (v.isArray()) for (const auto &x : v) keys(x, out);
}
inline std::vector<std::string> ids(const Json::Value &items) {
    std::vector<std::string> out;
    for (const auto &i : items) out.push_back(i["id"].asString());
    return out;
}
inline bool has(const std::vector<std::string> &v, const std::string &id) { return std::find(v.begin(), v.end(), id) != v.end(); }
inline std::string publicStatus(const std::string &user, const std::string &task) {
    const auto r = ts::runGet(user, task);
    return r && r->outcome == usapp::TaskService::Outcome::Ok ? r->task.status : std::string("?");
}
// Every page of one couple/filter with the given limit; stops on a failure or after 100 pages.
inline std::vector<std::string> walk(const std::string &couple, const std::string &kind, int limit, bool &failed) {
    std::vector<std::string> all;
    std::string cursor;
    failed = false;
    for (int page = 0; page < 100; ++page) {
        const auto r = listOf(couple, kind, limit, cursor);
        if (!ok(r)) { failed = true; return all; }
        if (static_cast<int>(r->body["items"].size()) > limit) failed = true;
        for (const auto &id : ids(r->body["items"])) all.push_back(id);
        if (r->body["next_cursor"].isNull()) return all;
        cursor = r->body["next_cursor"].asString();
    }
    failed = true;
    return all;
}

inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    using CA = tc::Change::Action;

    TEST_CASE("Admin tasks: states and full history from real services; old expired rows; status agrees; allowlist; no writes") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            // Single: completed by B, replayed with the same key (one completion), then a reaction by the creator.
            const auto single = c::publish(s.userA, "AT single", 5, 7);
            const auto completeKey = c::newUuid();
            CHECK(c::is(c::runComplete(s.userB, single, completeKey), c::Outcome::Completed));
            CHECK(c::is(c::runComplete(s.userB, single, completeKey), c::Outcome::Replayed));
            CHECK(rx::is(rx::start(s.userA, single, c::newUuid())->wait(), rx::O::Created));
            // Count: target 3, the last step replayed.
            const auto count = tp::publish(s.userA, 3, 2, 3);
            CHECK(tp::run(s.userB, count, c::newUuid()).has_value());
            CHECK(tp::run(s.userB, count, c::newUuid()).has_value());
            const auto lastKey = c::newUuid();
            const auto last = tp::run(s.userB, count, lastKey);
            CHECK((last && last->outcome == tp::Outcome::Completed));
            const auto lastReplay = tp::run(s.userB, count, lastKey);
            CHECK((lastReplay && lastReplay->outcome == tp::Outcome::Replayed));
            // Edits: rejected, withdrawn, then approved (successor).
            const auto edited = c::publish(s.userA, "AT edited", 4, 0);
            const auto p1 = tc::idOf(tc::propose(s.userA, edited, tc::editInput(tc::single("AT edit one", 6))));
            CHECK(tc::is(tc::decide(s.userB, edited, p1, CA::Reject), tc::CO::Decided));
            const auto p2 = tc::idOf(tc::propose(s.userA, edited, tc::editInput(tc::single("AT edit two", 7))));
            CHECK(tc::is(tc::decide(s.userA, edited, p2, CA::Withdraw), tc::CO::Decided));
            const auto p3 = tc::idOf(tc::propose(s.userA, edited, tc::editInput(tc::single("AT edit three", 8))));
            const auto approved = tc::decide(s.userB, edited, p3, CA::Approve);
            CHECK(tc::is(approved, tc::CO::Decided));
            const std::string successor = approved && approved->successor ? approved->successor->id : std::string();
            CHECK_FALSE(successor.empty());
            // Cancellation.
            const auto cancelled = c::publish(s.userA, "AT cancelled", 3, 0);
            const auto pc = tc::idOf(tc::propose(s.userA, cancelled, tc::cancelInput()));
            CHECK(tc::is(tc::decide(s.userB, cancelled, pc, CA::Approve), tc::CO::Decided));
            // Daily template, one step today stores today's period.
            const auto daily = dl::publish(s.userA, 2);
            const auto today = dl::day(observer);
            CHECK(dl::run(s.userB, daily, today, c::newUuid()).has_value());
            const auto period = dl::instance(observer, daily, today);
            // An old expired row: created 500 days ago, deadline 400 days ago (outside the public 30-day window).
            tk::TaskRow old;
            old.createdAtSql = "now() - interval '500 days'";
            old.deadline = tk::relativeDeadline("-400 days");
            const auto expired = tc::commitRow(pool, old, s);

            const auto before = footprint(observer);
            const auto all = listOf(s.couple, "", 100);
            CHECK(ok(all));
            if (!ok(all)) return;
            const auto listed = ids(all->body["items"]);
            CHECK_EQ(all->body["space"]["id"].asString(), s.couple);
            CHECK_EQ(all->body["space"]["status"].asString(), std::string("active"));
            CHECK(all->body["kind"].isNull());
            CHECK(all->body["next_cursor"].isNull());
            // Every stored row of the couple, exactly once, newest first by (created_at, id).
            const auto expectedRows = observer.client->execSqlSync(
                "SELECT id::text AS id FROM tasks WHERE couple_id = $1::uuid ORDER BY created_at DESC, id DESC", s.couple);
            std::vector<std::string> expectedIds;
            for (const auto &r : expectedRows) expectedIds.push_back(r["id"].as<std::string>());
            CHECK(listed == expectedIds);
            CHECK_EQ(listed.size(), 8u);  // single, count, edited, successor, cancelled, daily, period, expired
            CHECK_EQ(listed.back(), expired);
            // The public history window does not reach the old row; the admin list does.
            const auto history = ts::runList(s.userA, usapp::TaskService::View::History, std::nullopt, 100);
            bool publicHasOld = false;
            if (history) for (const auto &t : history->tasks) if (t.id == expired) publicHasOld = true;
            CHECK_FALSE(publicHasOld);

            std::map<std::string, Json::Value> byId;
            for (const auto &i : all->body["items"]) byId[i["id"].asString()] = i;
            CHECK_EQ(byId[single]["status"].asString(), std::string("completed"));
            CHECK_EQ(byId[count]["status"].asString(), std::string("completed"));
            CHECK_EQ(byId[count]["progress_count"].asInt(), 3);
            CHECK_EQ(byId[edited]["status"].asString(), std::string("superseded"));
            CHECK_EQ(byId[successor]["status"].asString(), std::string("open"));
            CHECK_EQ(byId[cancelled]["status"].asString(), std::string("cancelled"));
            CHECK_EQ(byId[expired]["status"].asString(), std::string("expired"));
            CHECK_EQ(byId[daily]["kind"].asString(), std::string("template"));
            CHECK_EQ(byId[daily]["status"].asString(), std::string("open"));
            CHECK_EQ(byId[period]["kind"].asString(), std::string("period"));
            CHECK_EQ(byId[period]["schedule_type"].asString(), std::string("daily"));
            CHECK_EQ(byId[period]["template_id"].asString(), daily);
            CHECK_EQ(byId[period]["period_date"].asString(), today);
            CHECK_EQ(byId[period]["progress_count"].asInt(), 1);
            CHECK_EQ(byId[single]["kind"].asString(), std::string("task"));
            CHECK(byId[single]["reward_flowers"].isString());
            CHECK_EQ(byId[single]["reward_flowers"].asString(), std::string("5"));
            CHECK_EQ(byId[single]["reward_sunshine"].asString(), std::string("7"));
            CHECK(byId[expired]["completed_at"].isNull());
            // Same status as the public read for every row it can still show.
            for (const auto &id : {single, count, edited, successor, cancelled}) CHECK_EQ(byId[id]["status"].asString(), publicStatus(s.userA, id));

            // Kind filters.
            const auto templates = listOf(s.couple, "template", 100);
            const auto periods = listOf(s.couple, "period", 100);
            const auto plain = listOf(s.couple, "task", 100);
            CHECK((ok(templates) && ids(templates->body["items"]) == std::vector<std::string>{daily}));
            CHECK((ok(periods) && ids(periods->body["items"]) == std::vector<std::string>{period}));
            CHECK((ok(plain) && plain->body["items"].size() == 6u && !has(ids(plain->body["items"]), daily)));
            if (ok(plain)) CHECK_EQ(plain->body["kind"].asString(), std::string("task"));

            // Single detail: one completion despite the replay, the reaction, no progress, no changes.
            const auto ds = detailOf(s.couple, single);
            CHECK(ok(ds));
            if (ok(ds)) {
                const auto &b = ds->body;
                CHECK_EQ(b["task"]["id"].asString(), single);
                CHECK_EQ(b["task"]["description"].isString(), true);
                CHECK_EQ(b["task"]["timezone"].asString(), std::string("Asia/Shanghai"));
                CHECK_EQ(b["completion"]["completed_by"].asString(), s.userB);
                CHECK_EQ(b["completion"]["reward_flowers"].asString(), std::string("5"));
                CHECK_EQ(b["completion"]["reward_sunshine"].asString(), std::string("7"));
                CHECK_EQ(c::completions(observer, single), 1);
                CHECK_EQ(b["progress"]["total"].asInt64(), 0);
                CHECK_EQ(b["reaction"]["author_id"].asString(), s.userA);
                CHECK_EQ(b["reaction"]["kind"].asString(), std::string("emoji"));
                CHECK_EQ(b["reaction"]["emoji_code"].asString(), std::string("heart"));
                CHECK_EQ(b["changes"]["owner_task_id"].asString(), single);
                CHECK_EQ(b["changes"]["total"].asInt64(), 0);
                CHECK(b["closure"].isNull());
                CHECK(b["replaces"].isNull());
            }
            // Count detail: three counted events newest first, only the last completes; one completion.
            const auto dc = detailOf(s.couple, count);
            CHECK(ok(dc));
            if (ok(dc)) {
                const auto &p = dc->body["progress"];
                CHECK_EQ(p["total"].asInt64(), 3);
                CHECK_EQ(p["items"].size(), 3u);
                CHECK_EQ(p["items"][0]["progress_count"].asInt(), 3);
                CHECK(p["items"][0]["completes_task"].asBool());
                CHECK_FALSE(p["items"][1]["completes_task"].asBool());
                CHECK_EQ(p["items"][2]["progress_count"].asInt(), 1);
                CHECK_EQ(p["items"][0]["user_id"].asString(), s.userB);
                CHECK_EQ(dc->body["completion"]["reward_flowers"].asString(), std::string("2"));
                CHECK(dc->body["reaction"].isNull());
            }
            // Edited detail: three proposals newest first with their decisions, the closure and its successor.
            const auto de = detailOf(s.couple, edited);
            CHECK(ok(de));
            if (ok(de)) {
                const auto &ch = de->body["changes"];
                CHECK_EQ(ch["total"].asInt64(), 3);
                CHECK_EQ(ch["items"].size(), 3u);
                CHECK_EQ(ch["items"][0]["id"].asString(), p3);
                CHECK_EQ(ch["items"][0]["decision"]["outcome"].asString(), std::string("approved"));
                CHECK_EQ(ch["items"][0]["decision"]["successor_task_id"].asString(), successor);
                CHECK_EQ(ch["items"][0]["terms"]["title"].asString(), std::string("AT edit three"));
                CHECK_EQ(ch["items"][0]["terms"]["reward_flowers"].asString(), std::string("8"));
                CHECK_EQ(ch["items"][1]["id"].asString(), p2);
                CHECK_EQ(ch["items"][1]["decision"]["outcome"].asString(), std::string("withdrawn"));
                CHECK_EQ(ch["items"][1]["decision"]["decided_by"].asString(), s.userA);
                CHECK(ch["items"][1]["decision"]["successor_task_id"].isNull());
                CHECK_EQ(ch["items"][2]["id"].asString(), p1);
                CHECK_EQ(ch["items"][2]["decision"]["outcome"].asString(), std::string("rejected"));
                CHECK_EQ(de->body["closure"]["reason"].asString(), std::string("superseded"));
                CHECK_EQ(de->body["closure"]["proposal_id"].asString(), p3);
                CHECK_EQ(de->body["closure"]["successor_task_id"].asString(), successor);
                CHECK(de->body["closure"]["applies_to_this_task"].asBool());
                CHECK_EQ(de->body["task"]["status"].asString(), std::string("superseded"));
            }
            const auto dsu = detailOf(s.couple, successor);
            CHECK(ok(dsu));
            if (ok(dsu)) {
                CHECK_EQ(dsu->body["replaces"]["task_id"].asString(), edited);
                CHECK_EQ(dsu->body["replaces"]["proposal_id"].asString(), p3);
                CHECK(dsu->body["closure"].isNull());
                CHECK_EQ(dsu->body["task"]["title"].asString(), std::string("AT edit three"));
            }
            const auto dx = detailOf(s.couple, cancelled);
            CHECK(ok(dx));
            if (ok(dx)) {
                CHECK_EQ(dx->body["closure"]["reason"].asString(), std::string("cancelled"));
                CHECK(dx->body["closure"]["successor_task_id"].isNull());
                CHECK_EQ(dx->body["changes"]["items"][0]["kind"].asString(), std::string("cancel"));
                CHECK(dx->body["changes"]["items"][0]["terms"].isNull());
            }
            // A stored period: its changes and closure live on the template, named as such.
            const auto dp = detailOf(s.couple, period);
            CHECK(ok(dp));
            if (ok(dp)) {
                CHECK_EQ(dp->body["changes"]["owner_task_id"].asString(), daily);
                CHECK_EQ(dp->body["task"]["kind"].asString(), std::string("period"));
                CHECK_EQ(dp->body["progress"]["total"].asInt64(), 1);
                CHECK(dp->body["completion"].isNull());
            }
            const auto dold = detailOf(s.couple, expired);
            CHECK((ok(dold) && dold->body["task"]["status"].asString() == "expired"));

            // Allowlist across every answer of this test: no keys, fingerprints or wallet operation ids.
            std::set<std::string> seen;
            for (const auto *r : {&all, &ds, &dc, &de, &dsu, &dx, &dp}) if (ok(*r)) keys((*r)->body, seen);
            const std::set<std::string> allowed = {
                "space", "id", "status", "kind", "items", "next_cursor", "read_at", "schedule_type", "template_id",
                "period_date", "title", "creator_id", "assignee_id", "reward_flowers", "reward_sunshine",
                "completion_mode", "target_count", "progress_count", "start_date", "deadline_at", "created_at",
                "completed_at", "task", "description", "timezone", "deadline_local_date", "deadline_local_time",
                "completion", "completed_by", "progress", "total", "user_id", "recorded_at", "completes_task",
                "reaction", "author_id", "emoji_code", "body", "changes", "owner_task_id", "proposer_id", "terms",
                "decision", "outcome", "decided_by", "decided_at", "successor_task_id", "closure", "reason",
                "closed_at", "proposal_id", "applies_to_this_task", "replaces", "task_id"};
            for (const auto &k : seen) c::expectThat(allowed.count(k) == 1u, "admin-task-allowlist", "unexpected field " + k);
            CHECK(footprint(observer) == before);
        } catch (const std::exception &e) {
            c::expectThat(false, "admin-task-history", usapptest::media::exceptionCategory(e));
        }
    }

    TEST_CASE("Admin tasks: unknown and empty spaces, foreign and unknown tasks, keyset pages, ties and cursor binding") {
        try {
            ts::CommittedSpace s, other, empty;
            ts::commitSpace(pool, s, true);
            ts::commitSpace(pool, other, true);
            ts::commitSpace(pool, empty, true);
            const auto unknown = c::newUuid();
            CHECK(is(listOf(unknown, "", 10), AO::SpaceNotFound));
            CHECK(is(detailOf(unknown, unknown), AO::SpaceNotFound));
            const auto none = listOf(empty.couple, "", 10);
            CHECK((ok(none) && none->body["items"].empty() && none->body["next_cursor"].isNull()));
            const auto foreign = c::publish(other.userA, "AT foreign", 1, 0);
            CHECK(is(detailOf(s.couple, foreign), AO::TaskNotFound));
            CHECK(is(detailOf(empty.couple, foreign), AO::TaskNotFound));
            CHECK(is(detailOf(s.couple, c::newUuid()), AO::TaskNotFound));
            CHECK(ok(detailOf(other.couple, foreign)));

            // Five service rows plus three rows sharing one created_at (the id breaks the tie).
            for (int i = 0; i < 5; ++i) c::publish(s.userA, "AT page " + std::to_string(i), 1, 0);
            for (int i = 0; i < 3; ++i) {
                tk::TaskRow tie;
                tie.createdAtSql = "TIMESTAMPTZ '2020-01-01 00:00:00.123456+00'";
                tie.deadline = tk::relativeDeadline("-1 day");
                tc::commitRow(pool, tie, s);
            }
            const auto expectedRows = observer.client->execSqlSync(
                "SELECT id::text AS id FROM tasks WHERE couple_id = $1::uuid ORDER BY created_at DESC, id DESC", s.couple);
            std::vector<std::string> expected;
            for (const auto &r : expectedRows) expected.push_back(r["id"].as<std::string>());
            CHECK_EQ(expected.size(), 8u);
            for (int limit : {1, 2, 3, 7, 8, 100}) {
                bool failed = false;
                const auto walked = walk(s.couple, "", limit, failed);
                c::expectThat(!failed && walked == expected, "admin-task-walk", "limit " + std::to_string(limit));
            }
            // Exactly at the boundary: a full last page has no cursor; one short has one more page of one row.
            const auto full = listOf(s.couple, "", 8);
            CHECK((ok(full) && full->body["items"].size() == 8u && full->body["next_cursor"].isNull()));
            const auto shy = listOf(s.couple, "", 7);
            CHECK((ok(shy) && shy->body["next_cursor"].isString()));
            if (ok(shy)) {
                const auto rest = listOf(s.couple, "", 7, shy->body["next_cursor"].asString());
                CHECK((ok(rest) && ids(rest->body["items"]) == std::vector<std::string>{expected.back()} &&
                       rest->body["next_cursor"].isNull()));
            }
            // No cross-page snapshot: a row created between pages is newer than the cursor and is not on later pages;
            // nothing repeats.
            const auto first = listOf(s.couple, "", 3);
            CHECK(ok(first));
            if (ok(first)) {
                const auto late = c::publish(s.userA, "AT between pages", 1, 0);
                const auto second = listOf(s.couple, "", 100, first->body["next_cursor"].asString());
                CHECK(ok(second));
                if (ok(second)) {
                    CHECK_FALSE(has(ids(second->body["items"]), late));
                    CHECK(ids(second->body["items"]) == std::vector<std::string>(expected.begin() + 3, expected.end()));
                }
            }

            // Cursor binding and shape.
            const auto c1 = listOf(s.couple, "", 2);
            CHECK(ok(c1));
            const std::string good = ok(c1) ? c1->body["next_cursor"].asString() : std::string();
            A::Cursor parsed;
            CHECK(A::decodeCursor(good, s.couple, "", parsed));
            CHECK_FALSE(A::decodeCursor(good, other.couple, "", parsed));   // another space
            CHECK_FALSE(A::decodeCursor(good, s.couple, "task", parsed));  // another filter
            const auto taskCursor = A::encodeCursor(s.couple, "task", parsed);
            CHECK(A::decodeCursor(taskCursor, s.couple, "task", parsed));
            CHECK_FALSE(A::decodeCursor(taskCursor, s.couple, "", parsed));
            CHECK_FALSE(A::decodeCursor(taskCursor, s.couple, "period", parsed));
            std::string upper = good;
            const auto lastDot = upper.rfind('.');
            for (std::size_t i = good.find('.', 7) + 1; i < lastDot; ++i)
                if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
            for (const std::string &bad : {std::string(), std::string("t1.all"), std::string("t1.all.0.x.y"),
                                           std::string("t2") + good.substr(2), good + "x", good.substr(0, good.size() - 1),
                                           std::string("t1.all.-5.") + good.substr(good.find('.', 7) + 1),
                                           std::string("t1.all.05.") + good.substr(good.find('.', 7) + 1),
                                           std::string("t1.all.99999999999999999999.") + good.substr(good.find('.', 7) + 1),
                                           upper, good + std::string(200, 'a')}) {
                if (bad == good) continue;  // a task id without hex letters cannot be upper-cased
                c::expectThat(!A::decodeCursor(bad, s.couple, "", parsed), "admin-task-cursor", "malformed cursor of length " +
                              std::to_string(bad.size()) + " accepted");
            }
            CHECK(A::validKind("") && A::validKind("task") && A::validKind("template") && A::validKind("period"));
            CHECK_FALSE(A::validKind("all"));
            CHECK_FALSE(A::validKind("Task"));
            CHECK_FALSE(A::validKind("one_off"));
        } catch (const std::exception &e) {
            c::expectThat(false, "admin-task-pages", usapptest::media::exceptionCategory(e));
        }
    }
}

}  // namespace usapptest::admintask
