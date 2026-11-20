#pragma once

#include "AlbumDbTests.h"

namespace usapptest::sharedplan {

inline void run(const taskcompletion::ClientPtr &pool) {
    constexpr auto sql =
        "SELECT usapp_shared_plan_write($1::uuid, NULLIF($2,'')::uuid, $3, $4, $5::jsonb)::text AS facts";
    constexpr auto original = R"({"body":{"title":"Original","date":null,"time":null,"place":null,"note":null}})";
    constexpr auto proposed = R"({"expected_version":1,"body":{"title":"Revised","date":null,"time":null,"place":null,"note":null}})";
    auto parse = [](const drogon::orm::Result &rows) {
        Json::Value result;
        Json::Reader reader;
        if (rows.size() == 1) reader.parse(rows[0]["facts"].as<std::string>(), result);
        return result;
    };
    auto start = [&](const std::string &actor, const std::string &id, const std::string &op, const std::string &body) {
        auto pending = std::make_shared<taskservice::Pending<Json::Value>>();
        pool->execSqlAsync(sql,
            [pending, parse](const drogon::orm::Result &rows) { pending->deliver(parse(rows)); },
            [pending](const drogon::orm::DrogonDbException &) { pending->deliver(Json::Value{}); },
            actor, id, op, album::newKey(), body);
        return pending;
    };
    TEST_CASE("P3-3a: competing requests produce one pending request; approval and advance commute") {
        try {
            const auto space = media::space();
            const auto created = start(space.userA, "", "create", original)->wait();
            CHECK(created && (*created)["plan"]["id"].isString());
            if (!created || !(*created)["plan"]["id"].isString()) return;
            const auto id = (*created)["plan"]["id"].asString();
            auto left = start(space.userB, id, "propose", proposed);
            auto right = start(space.userB, id, "propose", proposed);
            const auto first = left->wait();
            const auto second = right->wait();
            CHECK(first && second);
            if (!first || !second) return;
            const bool firstWon = (*first)["plan"].isObject();
            CHECK(firstWon != (*second)["plan"].isObject());
            CHECK_EQ((firstWon ? *second : *first)["error"].asString(), std::string("conflict"));
            const auto pending = (firstWon ? *first : *second)["plan"]["pending_proposal"]["id"];
            CHECK(pending.isString());
            if (!pending.isString()) return;
            auto approve = start(space.userA, id, "decide",
                "{\"proposal_id\":\"" + pending.asString() + "\",\"outcome\":\"approved\"}");
            auto advance = start(space.userB, id, "advance", R"({"expected_status":"want"})");
            const auto approved = approve->wait();
            const auto advanced = advance->wait();
            CHECK(approved && (*approved)["plan"].isObject());
            CHECK(advanced && (*advanced)["plan"].isObject());
            const auto rows = pool->execSqlSync("SELECT usapp_shared_plan_read($1::uuid)::text AS facts", id);
            const auto current = parse(rows);
            CHECK_EQ(current["body"]["title"].asString(), std::string("Revised"));
            CHECK_EQ(current["body_version"].asInt(), 2);
            CHECK_EQ(current["status"].asString(), std::string("plan"));
            CHECK_EQ(current["status_version"].asInt(), 2);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
    TEST_CASE("P3-3b: a failed conversion rolls back the memory, binding and link; retry creates them once") {
        try {
            media::useStore();
            const auto space = media::space();
            const auto created = start(space.userA, "", "create", original)->wait();
            CHECK(created && (*created)["plan"]["id"].isString());
            if (!created || !(*created)["plan"]["id"].isString()) return;
            const auto plan = (*created)["plan"]["id"].asString();
            const auto photo = media::agedReady(pool, space, space.userB);
            auto terms = memory::terms("Plan memory", {photo});
            const auto key = album::newKey();
            auto convert = [&](const std::string &actor, usapp::MemoryService::Fault fault = usapp::MemoryService::Fault::None) {
                auto pending = std::make_shared<memory::PendingS>();
                usapp::MemoryService::instance().createForPlan(actor, plan, key, terms,
                    [pending](usapp::MemoryService::Status status) { pending->deliver(std::move(status)); }, fault);
                return pending->wait();
            };
            using Outcome = usapp::MemoryService::Outcome;
            CHECK(memory::is(convert(space.userB), Outcome::PlanConflict));
            const auto planned = start(space.userA, plan, "advance", R"({"expected_status":"want"})")->wait();
            const auto finished = start(space.userB, plan, "advance", R"({"expected_status":"plan"})")->wait();
            CHECK(planned && (*planned)["plan"]["status"].asString() == "plan");
            CHECK(finished && (*finished)["plan"]["status"].asString() == "finished");
            CHECK(memory::is(convert(space.userB, usapp::MemoryService::Fault::FailPlanLink), Outcome::Error));
            CHECK_EQ(taskcompletion::scalar(media::obs(),
                "SELECT count(*)::bigint AS n FROM shared_plan_memory_links WHERE plan_id = $1::uuid", plan), 0);
            CHECK_EQ(taskcompletion::scalar(media::obs(),
                "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid", photo), 0);
            CHECK_EQ(taskcompletion::scalar(media::obs(),
                "SELECT count(*)::bigint AS n FROM memories WHERE couple_id = $1::uuid", space.couple), 0);
            const auto success = convert(space.userB);
            const auto replay = convert(space.userB);
            CHECK(memory::is(success, Outcome::Created));
            CHECK(memory::is(replay, Outcome::Replayed));
            CHECK_EQ(memory::idOf(success), memory::idOf(replay));
            CHECK_EQ(taskcompletion::scalar(media::obs(),
                "SELECT count(*)::bigint AS n FROM shared_plan_memory_links WHERE plan_id = $1::uuid "
                "AND memory_id = $2::uuid", plan, memory::idOf(success)), 1);
            CHECK_EQ(taskcompletion::scalar(media::obs(),
                "SELECT count(*)::bigint AS n FROM media_bindings WHERE media_id = $1::uuid "
                "AND target_kind = 'memory' AND target_id = $2::uuid", photo, memory::idOf(success)), 1);
            terms.title = "Changed terms";
            CHECK(memory::is(convert(space.userB), Outcome::KeyReused));
            CHECK(memory::is(convert(space.userA), Outcome::PlanConflict));

            const auto second = start(space.userB, "", "create", original)->wait();
            CHECK(second && (*second)["plan"]["id"].isString());
            if (!second || !(*second)["plan"]["id"].isString()) return;
            const auto nextPlan = (*second)["plan"]["id"].asString();
            (void)start(space.userB, nextPlan, "advance", R"({"expected_status":"want"})")->wait();
            (void)start(space.userA, nextPlan, "advance", R"({"expected_status":"plan"})")->wait();
            const auto memoryId = memory::idOf(success);
            const auto attachKey = album::newKey();
            auto attach = [&](const std::string &actor, const std::string &target) {
                auto pending = std::make_shared<taskservice::Pending<Json::Value>>();
                pool->execSqlAsync(
                    "SELECT usapp_plan_memory_attach($1::uuid,$2::uuid,$3,$4::uuid)::text AS facts",
                    [pending, parse](const drogon::orm::Result &rows) { pending->deliver(parse(rows)); },
                    [pending](const drogon::orm::DrogonDbException &) { pending->deliver(Json::Value{}); },
                    actor, nextPlan, attachKey, target);
                return pending;
            };
            const auto outsider = media::space();
            const auto wrongSpace = attach(outsider.userA, memoryId)->wait();
            CHECK(wrongSpace && (*wrongSpace)["error"].asString() == "not_found");
            auto first = attach(space.userA, memoryId);
            auto other = attach(space.userB, memoryId);
            const auto left = first->wait();
            const auto right = other->wait();
            CHECK(left && right);
            if (!left || !right) return;
            const bool firstWon = (*left)["memory_id"].isString();
            CHECK(firstWon != (*right)["memory_id"].isString());
            CHECK_EQ((firstWon ? *right : *left)["error"].asString(), std::string("conflict"));
            const auto &winner = firstWon ? space.userA : space.userB;
            const auto existingReplay = attach(winner, memoryId)->wait();
            CHECK(existingReplay && (*existingReplay)["replayed"].asBool());
            const auto changed = attach(winner, taskcompletion::newUuid())->wait();
            CHECK(changed && (*changed)["error"].asString() == "key_reused");
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
}
}  // namespace usapptest::sharedplan
