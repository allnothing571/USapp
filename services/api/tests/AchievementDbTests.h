#pragma once
#include "AvatarCatalogDbTests.h"
#include "TaskCompletionDbTests.h"
#include "TaskProgressDbTests.h"
#include "services/AchievementService.h"
namespace usapptest::achievement {
namespace av = usapptest::avatar;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::AchievementService;
inline std::optional<S::Result> read(const std::string &user) {
    auto p = std::make_shared<ts::Pending<S::Result>>();
    S::read(user, [p](S::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline std::string footprint() {
    return md::obs().client->execSqlSync("SELECT concat_ws('|', (SELECT count(*) FROM task_completions), "
        "(SELECT count(*) FROM wallet_operations), (SELECT count(*) FROM wallet_ledger_entries), "
        "(SELECT count(*) FROM audit_logs), (SELECT count(*) FROM achievement_activation)) AS v")[0]["v"].as<std::string>();
}
inline void runAchievementDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("Achievements: activation boundary, real completion versus progress, identity isolation and read-only replay") {
        try {
            const auto s = md::space();
            const auto before = read(s.userB);
            CHECK(before && before->outcome == S::Outcome::Ok);
            CHECK(before && before->body["total_completed"].asInt() == 0);
            CHECK(before && before->body["badges"].size() == 5u);
            const bool active = before && before->body["enabled"].asBool();
            // One real task completes before this suite's activation call. On a fresh DB it cannot count afterwards.
            auto task = c::publish(s.userA, "achievement before activation", 1, 0);
            CHECK(c::is(c::runComplete(s.userB, task, c::newUuid()), c::Outcome::Completed));
            av::asMaintenance("SELECT usapp_achievement_activate()");
            const auto cutover = read(s.userB);
            CHECK(cutover && cutover->body["enabled"].asBool());
            CHECK(cutover && cutover->body["total_completed"].asInt() == (active ? 1 : 0));
            task = c::publish(s.userA, "achievement final completion", 1, 0);
            const auto pending = read(s.userB);
            CHECK(pending && pending->body["total_completed"].asInt() == (active ? 1 : 0));
            CHECK(c::is(c::runComplete(s.userB, task, c::newUuid()), c::Outcome::Completed));
            const auto counted = usapptest::taskprogress::publish(s.userA, 2, 1, 0);
            CHECK(c::is(usapptest::taskprogress::run(s.userB, counted, c::newUuid()), c::Outcome::Progressed));
            const auto partial = read(s.userB);
            CHECK(partial && partial->body["total_completed"].asInt() == (active ? 2 : 1));
            const auto finalKey = c::newUuid();
            CHECK(c::is(usapptest::taskprogress::run(s.userB, counted, finalKey), c::Outcome::Completed));
            CHECK(c::is(usapptest::taskprogress::run(s.userB, counted, finalKey), c::Outcome::Replayed));
            const auto rows = footprint();
            const auto a = read(s.userB), replay = read(s.userB), partner = read(s.userA);
            CHECK(a && replay && a->body == replay->body);
            CHECK(a && a->body["total_completed"].asInt() == (active ? 3 : 2));
            CHECK(a && a->body["badges"][0]["earned_at"].isString());
            CHECK(a && a->body["timezone"].asString() == "Asia/Shanghai");
            CHECK(partner && partner->body["total_completed"].asInt() == 0);
            CHECK_EQ(footprint(), rows);
            const auto other = md::space(); const auto isolated = read(other.userB);
            CHECK(isolated && isolated->body["total_completed"].asInt() == 0);
            // All reads above do not post, audit or create badge events; other space creation precedes this snapshot.
            const auto stable = footprint(); const auto last = read(s.userB);
            CHECK(last && last->outcome == S::Outcome::Ok);
            CHECK_EQ(footprint(), stable);
            const auto lonely = c::newUuid(); ts::commitUser(pool, lonely, "achievement_lonely");
            const auto noSpace = read(lonely);
            CHECK(noSpace && noSpace->outcome == S::Outcome::SpaceNotFound);
        } catch (const std::exception &e) { c::expectThat(false, "achievement-service", md::exceptionCategory(e)); }
    }
}
}
