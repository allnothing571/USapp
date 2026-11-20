// =====================================================================
// services/api/tests/TaskCompletionUnitTests.h
//
// 阶段 1 M3-C1：TaskCompletionService 纯函数的轻量参数化测试（由 UnitTest.cc 的 main 调用）。
// 不访问数据库与网络。
//
// 明确限制：这里的余额上限算术测试与既有数据库 CHECK 测试**不能**证明真实服务路径上的
// WALLET_BALANCE_LIMIT 拒绝；真实服务的临界余额测试随 D33 推迟。
// =====================================================================
#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/TaskCompletionService.h"
#include "services/TaskService.h"

namespace usapptest::taskcompletionunit {

using usapp::TaskCompletionService;
using usapp::TaskService;

inline void runTaskCompletionUnitTests() {
    TEST_CASE("TaskCompletion: 完成指纹是带长度的规范串的 SHA-256") {
        const std::string id = "0f8fad5b-d9cb-469f-a165-70867728950e";
        CHECK_EQ(TaskCompletionService::canonicalCompleteString(id),
                 std::string("complete-task:v1:task_id=36:0f8fad5b-d9cb-469f-a165-70867728950e;"));
        const std::string fp = TaskCompletionService::completeFingerprint(id);
        CHECK_EQ(fp.size(), 64u);
        CHECK(fp.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK_NE(fp, TaskCompletionService::completeFingerprint("1f8fad5b-d9cb-469f-a165-70867728950e"));
    }

    TEST_CASE("TaskCompletion: 请求体只能为空或空对象") {
        struct Case {
            const char *raw;
            bool accepted;
        };
        const std::vector<Case> cases = {
            {"", true},          {"  \r\n\t", true}, {"{}", true},          {" { } ", true},
            {"{\n}", true},      {"{\"a\":1}", false}, {"null", false},     {"[]", false},
            {"{}{}", false},     {"x", false},       {"{\"\":0}", false},
        };
        for (const auto &c : cases) {
            const bool accepted = TaskCompletionService::isEmptyCompletionBody(c.raw);
            CHECK_EQ(accepted, c.accepted);
            if (accepted != c.accepted) usapptest::note(std::string("请求体判定错误: ") + c.raw);
        }
    }

    TEST_CASE("TaskCompletion: 余额上限预检查的边界（参数化；不代表真实服务路径）") {
        using C = TaskCompletionService;
        struct Case {
            std::int64_t balance;
            std::int64_t reward;
            std::int64_t cap;
            bool ok;
        };
        const std::vector<Case> cases = {
            {0, 999, C::kMaxBalanceFlowers, true},
            {999001, 999, C::kMaxBalanceFlowers, true},          // 恰好到上限
            {999002, 999, C::kMaxBalanceFlowers, false},         // 超过 1
            {999999, 1, C::kMaxBalanceFlowers, true},
            {999999, 2, C::kMaxBalanceFlowers, false},
            {1000000, 0, C::kMaxBalanceFlowers, true},
            {1000000, 1, C::kMaxBalanceFlowers, false},
            {999900000, 100000, C::kMaxBalanceSunshine, true},
            {999900001, 100000, C::kMaxBalanceSunshine, false},
            {-1, 1, C::kMaxBalanceFlowers, false},
            {0, -1, C::kMaxBalanceFlowers, false},
        };
        for (const auto &c : cases) {
            CHECK_EQ(TaskCompletionService::withinCap(c.balance, c.reward, c.cap), c.ok);
        }
        CHECK_EQ(TaskCompletionService::kMaxBalanceFlowers, std::int64_t(1000000));
        CHECK_EQ(TaskCompletionService::kMaxBalanceSunshine, std::int64_t(1000000000));
    }

    TEST_CASE("TaskCompletion: 结果到 HTTP 与错误码的映射") {
        using O = TaskCompletionService::Outcome;
        struct Case {
            O outcome;
            int status;
            const char *code;
        };
        const std::vector<Case> cases = {
            {O::Validation, 400, "VALIDATION_ERROR"},
            {O::SpaceNotFound, 404, "SPACE_NOT_FOUND"},
            {O::TaskNotFound, 404, "TASK_NOT_FOUND"},
            {O::NotAssignee, 403, "TASK_NOT_ASSIGNEE"},
            {O::AlreadyCompleted, 409, "TASK_ALREADY_COMPLETED"},
            {O::KeyReused, 409, "IDEMPOTENCY_KEY_REUSED"},
            {O::Expired, 409, "TASK_EXPIRED"},
            {O::BalanceLimit, 409, "WALLET_BALANCE_LIMIT"},
            {O::ResultUnknown, 503, "RESULT_UNKNOWN"},
            {O::Error, 500, "INTERNAL_ERROR"},
            {O::WrongMode, 409, "TASK_COMPLETION_MODE_INVALID"},
            {O::NotStarted, 409, "TASK_NOT_STARTED"},
            {O::Cancelled, 409, "TASK_CANCELLED"},
            {O::Superseded, 409, "TASK_SUPERSEDED"},
        };
        for (const auto &c : cases) {
            CHECK_EQ(TaskCompletionService::httpStatusFor(c.outcome), c.status);
            CHECK_EQ(std::string(usapp::errorCodeToString(TaskCompletionService::errorCodeFor(c.outcome))),
                     std::string(c.code));
        }
        CHECK_EQ(TaskCompletionService::httpStatusFor(O::Completed), 200);
        CHECK_EQ(TaskCompletionService::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(TaskCompletionService::httpStatusFor(O::Progressed), 200);
        CHECK_EQ(TaskCompletionService::httpStatusFor(O::Ignored), 200);
        CHECK(TaskCompletionService::kDefaultFault == TaskCompletionService::SettlementFault::None);
        CHECK_EQ(TaskService::failureDetail("task_complete", "fault_injection", "db_error"),
                 std::string("task_complete stage=fault_injection reason=db_error"));
    }

    TEST_CASE("TaskCompletion: 响应只有任务与结算事实，没有余额字段") {
        TaskCompletionService::Status status;
        status.outcome = TaskCompletionService::Outcome::Completed;
        status.task.id = "0f8fad5b-d9cb-469f-a165-70867728950e";
        status.task.title = "T";
        status.task.status = "completed";
        status.task.completedAt = "2026-10-14T01:02:03.123456Z";
        status.task.createdAt = "2026-10-13T01:02:03.123456Z";
        status.task.rewardFlowers = 5;
        status.settlement.operationId = "1f8fad5b-d9cb-469f-a165-70867728950e";
        status.settlement.flower = 5;
        status.settlement.sunshine = 0;
        const Json::Value body = TaskCompletionService::responseBody(status);
        CHECK_EQ(body.size(), 2u);
        CHECK(body.isMember("task") && body.isMember("settlement"));
        CHECK_EQ(body["settlement"].size(), 3u);
        CHECK(body["settlement"]["flower"].isInt());
        CHECK_EQ(body["settlement"]["sunshine"].asInt(), 0);
        CHECK_EQ(body["task"]["status"].asString(), std::string("completed"));
        for (const char *forbidden : {"wallet", "balance", "balances", "flower_balance"}) {
            CHECK_FALSE(body.isMember(forbidden));
            CHECK_FALSE(body["settlement"].isMember(forbidden));
        }
    }

    TEST_CASE("M4: progress responses distinguish increments, settlement and ignored excess") {
        TaskCompletionService::Status s;
        s.task.id = "task";
        s.progress = TaskCompletionService::Status::Progress{"event", 1, 2, "time", false};
        auto body = TaskCompletionService::progressResponseBody(s);
        CHECK_EQ(body["progress_count"].asInt(), 1);
        CHECK_FALSE(body["completed"].asBool());
        CHECK(body["settlement"].isNull());
        s.progress->count = 2;
        s.hasSettlement = true;
        s.settlement = {"operation", 2, 3};
        body = TaskCompletionService::progressResponseBody(s);
        CHECK(body["completed"].asBool());
        CHECK_EQ(body["settlement"]["flower"].asInt(), 2);
        CHECK_FALSE(body.isMember("wallet"));
        s.progress = TaskCompletionService::Status::Progress{"", 2, 2, "", true};
        body = TaskCompletionService::progressResponseBody(s);
        CHECK(body["ignored"].asBool());
        CHECK(body["event_id"].isNull());
        CHECK(body["recorded_at"].isNull());
    }

    TEST_CASE("TaskCompletion: 暴露的行映射复用 M3-B 列清单（D41 回归）") {
        const std::string columns = TaskService::taskColumnsSql();
        CHECK_CONTAINS(columns, "COALESCE(t.recurrence_id, t.id)::text AS id");
        CHECK_CONTAINS(columns, "AS completed_at");
        CHECK_CONTAINS(columns, "AS status");
        CHECK_NOT_CONTAINS(columns, "history_at");
        // history 视图仍单独添加 history_at（由 withHistory = true 的调用读取）。
        CHECK_CONTAINS(TaskService::listSql(TaskService::ListQuery::HistoryFirst), "AS history_at");
        CHECK_CONTAINS(TaskService::getSql(), columns);
    }
}

}  // namespace usapptest::taskcompletionunit
