// =====================================================================
// services/api/tests/TaskChangeUnitTests.h
//
// M6: TaskChangeService pure functions and the additive Task JSON fields
// (called from UnitTest.cc main). No database or network.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/TaskChangeService.h"
#include "services/TaskService.h"

namespace usapptest::taskchangeunit {

using usapp::TaskChangeService;
using usapp::TaskService;

inline Json::Value parsed(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline void runTaskChangeUnitTests() {
    TEST_CASE("TaskChange: propose request shape (edit needs full terms, cancel takes none)") {
        TaskChangeService::ProposeInput in;
        std::string message;
        CHECK(TaskChangeService::parseProposeRequest(parsed(R"({"kind":"cancel"})"), in, message));
        CHECK_EQ(in.kind, std::string("cancel"));
        CHECK_FALSE(TaskChangeService::parseProposeRequest(parsed(R"({"kind":"cancel","terms":{}})"), in, message));
        CHECK_FALSE(TaskChangeService::parseProposeRequest(parsed(R"({"kind":"edit"})"), in, message));
        CHECK_FALSE(TaskChangeService::parseProposeRequest(parsed(R"({"kind":"delete"})"), in, message));
        CHECK_FALSE(TaskChangeService::parseProposeRequest(parsed(R"({"kind":"cancel","x":1})"), in, message));
        CHECK_FALSE(TaskChangeService::parseProposeRequest(
            parsed(R"({"kind":"edit","terms":{"title":"","reward_flowers":1,"reward_sunshine":0}})"), in, message));
        CHECK(TaskChangeService::parseProposeRequest(
            parsed(R"({"kind":"edit","terms":{"title":" New ","reward_flowers":3,"reward_sunshine":0}})"), in, message));
        CHECK_EQ(in.kind, std::string("edit"));
        CHECK_EQ(in.terms.title, std::string("New"));
        CHECK_EQ(in.terms.rewardFlowers, 3);
        CHECK_EQ(in.terms.scheduleType, std::string("one_off"));
    }

    TEST_CASE("TaskChange: decision request accepts exactly one action") {
        TaskChangeService::Action action = TaskChangeService::Action::Reject;
        std::string message;
        CHECK(TaskChangeService::parseDecisionRequest(parsed(R"({"action":"approve"})"), action, message));
        CHECK(action == TaskChangeService::Action::Approve);
        CHECK(TaskChangeService::parseDecisionRequest(parsed(R"({"action":"withdraw"})"), action, message));
        CHECK(action == TaskChangeService::Action::Withdraw);
        CHECK_FALSE(TaskChangeService::parseDecisionRequest(parsed(R"({"action":"approve","x":1})"), action, message));
        CHECK_FALSE(TaskChangeService::parseDecisionRequest(parsed(R"({"action":"ok"})"), action, message));
        CHECK_FALSE(TaskChangeService::parseDecisionRequest(parsed(R"({})"), action, message));
    }

    TEST_CASE("TaskChange: fingerprints bind task, kind, terms, proposal and action") {
        const std::string a = "0f8fad5b-d9cb-469f-a165-70867728950e";
        const std::string b = "1f8fad5b-d9cb-469f-a165-70867728950e";
        TaskChangeService::ProposeInput cancel;
        cancel.kind = "cancel";
        TaskChangeService::ProposeInput edit;
        edit.kind = "edit";
        edit.terms.title = "New";
        edit.terms.rewardFlowers = 3;
        auto edit2 = edit;
        edit2.terms.rewardFlowers = 4;
        const auto fp = TaskChangeService::proposeFingerprint(a, cancel);
        CHECK_EQ(fp.size(), 64u);
        CHECK_EQ(fp, TaskChangeService::proposeFingerprint(a, cancel));
        CHECK_NE(fp, TaskChangeService::proposeFingerprint(b, cancel));
        CHECK_NE(fp, TaskChangeService::proposeFingerprint(a, edit));
        CHECK_NE(TaskChangeService::proposeFingerprint(a, edit), TaskChangeService::proposeFingerprint(a, edit2));
        const auto approve = TaskChangeService::decisionFingerprint(a, TaskChangeService::Action::Approve);
        CHECK_EQ(approve.size(), 64u);
        CHECK_NE(approve, TaskChangeService::decisionFingerprint(a, TaskChangeService::Action::Reject));
        CHECK_NE(approve, TaskChangeService::decisionFingerprint(b, TaskChangeService::Action::Approve));
    }

    TEST_CASE("TaskChange: error mapping and HTTP status") {
        using O = TaskChangeService::Outcome;
        struct Case {
            O outcome;
            int status;
            const char *code;
        };
        const std::vector<Case> cases = {
            {O::Validation, 400, "VALIDATION_ERROR"},
            {O::SpaceNotFound, 404, "SPACE_NOT_FOUND"},
            {O::TaskNotFound, 404, "TASK_NOT_FOUND"},
            {O::ChangeNotFound, 404, "TASK_CHANGE_NOT_FOUND"},
            {O::Forbidden, 403, "TASK_CHANGE_FORBIDDEN"},
            {O::Pending, 409, "TASK_CHANGE_PENDING"},
            {O::HasProgress, 409, "TASK_HAS_PROGRESS"},
            {O::AlreadyDecided, 409, "TASK_CHANGE_DECIDED"},
            {O::AlreadyCompleted, 409, "TASK_ALREADY_COMPLETED"},
            {O::Expired, 409, "TASK_EXPIRED"},
            {O::Cancelled, 409, "TASK_CANCELLED"},
            {O::Superseded, 409, "TASK_SUPERSEDED"},
            {O::KeyReused, 409, "IDEMPOTENCY_KEY_REUSED"},
            {O::ResultUnknown, 503, "RESULT_UNKNOWN"},
            {O::Error, 500, "INTERNAL_ERROR"},
        };
        for (const auto &c : cases) {
            CHECK_EQ(TaskChangeService::httpStatusFor(c.outcome), c.status);
            CHECK_EQ(std::string(usapp::errorCodeToString(TaskChangeService::errorCodeFor(c.outcome))),
                     std::string(c.code));
        }
        CHECK_EQ(TaskChangeService::httpStatusFor(O::Proposed), 201);
        CHECK_EQ(TaskChangeService::httpStatusFor(O::Decided), 200);
        CHECK_EQ(TaskChangeService::httpStatusFor(O::Replayed), 200);
    }

    TEST_CASE("TaskChange: blocked reason follows the approved rules") {
        TaskService::TaskData t;
        t.status = "open";
        CHECK_EQ(TaskService::changeBlockedReason(t, "edit"), std::string());
        CHECK_EQ(TaskService::changeBlockedReason(t, "cancel"), std::string());
        t.completionMode = "count";
        t.progressCount = 1;
        CHECK_EQ(TaskService::changeBlockedReason(t, "edit"), std::string("progress"));
        CHECK_EQ(TaskService::changeBlockedReason(t, "cancel"), std::string());  // cancel keeps progress
        t.status = "completed";
        CHECK_EQ(TaskService::changeBlockedReason(t, "cancel"), std::string("completed"));
        t.scheduleType = "weekly";  // a completed current period does not block template cancellation
        CHECK_EQ(TaskService::changeBlockedReason(t, "cancel"), std::string());
        CHECK_EQ(TaskService::changeBlockedReason(t, "edit"), std::string("completed"));
        t.status = "superseded";
        CHECK_EQ(TaskService::changeBlockedReason(t, "cancel"), std::string("closed"));
    }

    TEST_CASE("TaskChange: Task JSON adds closure, lineage and pending change only when present") {
        TaskService::TaskData t;
        t.id = "0f8fad5b-d9cb-469f-a165-70867728950e";
        t.status = "open";
        const auto plain = TaskService::taskJson(t);
        CHECK_FALSE(plain.isMember("closure"));
        CHECK_FALSE(plain.isMember("replaces"));
        CHECK_FALSE(plain.isMember("pending_change"));

        t.status = "superseded";
        t.closureJson = R"({"reason":"superseded","closed_at":"2026-10-25T01:02:03.000004Z",)"
                        R"("replaced_by":"1f8fad5b-d9cb-469f-a165-70867728950e"})";
        const auto closed = TaskService::taskJson(t);
        CHECK_EQ(closed["closure"]["reason"].asString(), std::string("superseded"));
        CHECK_EQ(closed["closure"]["replaced_by"].asString(), std::string("1f8fad5b-d9cb-469f-a165-70867728950e"));

        TaskService::TaskData s;
        s.status = "open";
        s.replaces = t.id;
        s.completionMode = "count";
        s.progressCount = 2;
        s.pendingChangeJson = R"({"id":"2f8fad5b-d9cb-469f-a165-70867728950e","kind":"edit",)"
                              R"("proposer_id":"3f8fad5b-d9cb-469f-a165-70867728950e","created_at":"x",)"
                              R"("title":"T","description":"","reward_flowers":1,"reward_sunshine":0,)"
                              R"("completion_mode":"count","target_count":3,"deadline_local_date":null,)"
                              R"("deadline_local_time":null,"start_date":null})";
        const auto pending = TaskService::taskJson(s);
        CHECK_EQ(pending["replaces"].asString(), t.id);
        CHECK_EQ(pending["pending_change"]["kind"].asString(), std::string("edit"));
        CHECK_EQ(pending["pending_change"]["terms"]["target_count"].asInt(), 3);
        CHECK(pending["pending_change"]["terms"]["deadline"].isNull());
        CHECK_EQ(pending["pending_change"]["blocked_reason"].asString(), std::string("progress"));
    }

    TEST_CASE("TaskChange: response separates immutable facts from current state") {
        TaskChangeService::Status status;
        status.outcome = TaskChangeService::Outcome::Decided;
        status.change["id"] = "2f8fad5b-d9cb-469f-a165-70867728950e";
        status.task.status = "cancelled";
        const auto body = TaskChangeService::responseBody(status);
        CHECK(body.isMember("change"));
        CHECK_EQ(body["task"]["status"].asString(), std::string("cancelled"));
        CHECK(body["successor"].isNull());
    }
}

}  // namespace usapptest::taskchangeunit
