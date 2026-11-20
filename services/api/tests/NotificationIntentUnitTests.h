// =====================================================================
// services/api/tests/NotificationIntentUnitTests.h
//
// P2-3b: pure parts of services/NotificationIntent (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <string>

#include "TestSupport.h"
#include "core/Crypto.h"
#include "services/NotificationIntent.h"

namespace usapptest::notificationintentunit {

namespace intent = usapp::intent;

inline void runNotificationIntentUnitTests() {
    TEST_CASE("P2-3b: notify_partner is optional, boolean only, and removed before legacy parsing") {
        std::string message;
        intent::Choice choice;
        Json::Value body(Json::objectValue);
        body["title"] = "x";
        CHECK(intent::takeChoice(body, choice, message));
        CHECK_FALSE(choice.provided);
        CHECK_FALSE(choice.requested());
        CHECK_EQ(body.size(), static_cast<Json::ArrayIndex>(1));

        body["notify_partner"] = true;
        CHECK(intent::takeChoice(body, choice, message));
        CHECK(choice.provided);
        CHECK(choice.requested());
        CHECK_FALSE(body.isMember("notify_partner"));  // the legacy body is exactly restored

        body["notify_partner"] = false;
        CHECK(intent::takeChoice(body, choice, message));
        CHECK(choice.provided);
        CHECK_FALSE(choice.requested());

        for (const Json::Value bad : {Json::Value("true"), Json::Value(1), Json::Value(Json::nullValue),
                                      Json::Value(Json::objectValue)}) {
            body["notify_partner"] = bad;
            CHECK_FALSE(intent::takeChoice(body, choice, message));
        }
    }

    TEST_CASE("P2-3b: omitted and false keep the literal legacy fingerprint; true is domain-separated") {
        const std::string legacy(64, 'a');
        intent::Choice omitted;
        intent::Choice off{true, false};
        intent::Choice on{true, true};
        CHECK_EQ(intent::fingerprint(legacy, omitted), legacy);
        CHECK_EQ(intent::fingerprint(legacy, off), legacy);
        // SHA-256("notify-partner:v1:true:" + legacy), fixed literal.
        CHECK_EQ(intent::fingerprint(legacy, on),
                 std::string("4638110e70553e083693f267a7dc17b4f0f7d120f77b0ae77455fed2886c574a"));
        CHECK(intent::fingerprint(legacy, on) != intent::fingerprint(std::string(64, 'b'), on));
    }

    TEST_CASE("P2-3b: notification facts and operation names") {
        auto f = intent::facts(true, true, "00000000-0000-4000-8000-000000000001");
        CHECK(f["requested"].asBool());
        CHECK(f["created"].asBool());
        CHECK_EQ(f["reason"].asString(), std::string("delivered_to_inbox"));
        f = intent::facts(true, false, "");
        CHECK_FALSE(f["created"].asBool());
        CHECK_EQ(f["reason"].asString(), std::string("no_partner"));
        CHECK(f["notification_id"].isNull());
        f = intent::facts(false, true, "");
        CHECK_EQ(f["reason"].asString(), std::string("not_requested"));
        f = intent::facts(false, false, "");
        CHECK_EQ(f["reason"].asString(), std::string("not_requested"));
        CHECK_EQ(std::string(intent::operationName(intent::Operation::TaskPublish)), std::string("task_publish"));
        CHECK_EQ(std::string(intent::operationName(intent::Operation::AnniversaryEdit)), std::string("anniversary_edit"));
        CHECK_EQ(std::string(intent::operationName(intent::Operation::MessageReply)), std::string("message_reply"));
    }
}

}  // namespace usapptest::notificationintentunit
