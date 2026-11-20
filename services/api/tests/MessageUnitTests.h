// =====================================================================
// services/api/tests/MessageUnitTests.h
//
// P2-1: MessageService pure functions (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/MessageService.h"

namespace usapptest::messageunit {

using usapp::MessageService;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline Json::Value bodyOf(const std::string &text) {
    Json::Value v(Json::objectValue);
    v["body"] = text;
    return v;
}

inline std::string repeat(const std::string &unit, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += unit;
    return out;
}

inline void runMessageUnitTests() {
    TEST_CASE("Message: body is exactly {body}; 1-500 code points; LF kept, CRLF normalised, trimmed") {
        std::string t;
        std::string m;
        CHECK(MessageService::parseMessageRequest(bodyOf("  今天面试加油\r\n不许紧张 ❤️ \n"), t, m));
        CHECK_EQ(t, std::string("今天面试加油\n不许紧张 ❤️"));
        CHECK(MessageService::parseMessageRequest(bodyOf(repeat("\xF0\x9F\x98\x80", 500)), t, m));  // 500 emoji
        CHECK_FALSE(MessageService::parseMessageRequest(bodyOf(repeat("\xF0\x9F\x98\x80", 501)), t, m));
        for (const char *bad : {R"({})", R"({"body":""})", R"({"body":"   "})", R"({"body":1})",
                                R"({"body":"x","author_id":"y"})", R"({"body":"a\u0007b"})", R"([1])"}) {
            CHECK_FALSE(MessageService::parseMessageRequest(json(bad), t, m));
        }
    }

    TEST_CASE("Message: a reply is one line of 1-100 code points") {
        std::string t;
        std::string m;
        CHECK(MessageService::parseReplyRequest(bodyOf(" 今天也辛苦啦。 "), t, m));
        CHECK_EQ(t, std::string("今天也辛苦啦。"));
        CHECK(MessageService::parseReplyRequest(bodyOf(repeat("好", 100)), t, m));
        CHECK_FALSE(MessageService::parseReplyRequest(bodyOf(repeat("好", 101)), t, m));
        CHECK_FALSE(MessageService::parseReplyRequest(bodyOf("第一行\n第二行"), t, m));
        CHECK_FALSE(MessageService::parseReplyRequest(bodyOf("\n"), t, m));
        CHECK_FALSE(MessageService::parseReplyRequest(json(R"({"body":"x","message_id":"y"})"), t, m));
    }

    TEST_CASE("Message: fingerprints bind the body, and the reply also binds the message") {
        const std::string a = "0f8fad5b-d9cb-469f-a165-70867728950e";
        const std::string b = "1f8fad5b-d9cb-469f-a165-70867728950e";
        const auto fp = MessageService::createFingerprint("hi");
        CHECK_EQ(fp.size(), 64u);
        CHECK_EQ(fp, MessageService::createFingerprint("hi"));
        CHECK_NE(fp, MessageService::createFingerprint("hi!"));
        CHECK_NE(MessageService::replyFingerprint(a, "ok"), MessageService::replyFingerprint(b, "ok"));
        CHECK_NE(MessageService::replyFingerprint(a, "ok"), MessageService::replyFingerprint(a, "ok!"));
        CHECK_NE(MessageService::replyFingerprint(a, "hi"), MessageService::createFingerprint("hi"));
    }

    TEST_CASE("Message: strict list cursor") {
        const MessageService::Cursor c{"2026-10-27T01:02:03.000004Z", "0f8fad5b-d9cb-469f-a165-70867728950e"};
        const auto raw = MessageService::encodeCursor(c);
        const auto back = MessageService::decodeCursor(raw);
        CHECK(back.has_value());
        if (back) CHECK_EQ(back->id, c.id);
        for (const std::string &bad : {std::string(), raw + "0", raw.substr(1),
                                      std::string("2026-10-27T01:02:03.000004Z_0F8FAD5B-D9CB-469F-A165-70867728950E")}) {
            CHECK_FALSE(MessageService::decodeCursor(bad).has_value());
        }
    }

    TEST_CASE("Message: error mapping, statuses and the reply shape") {
        using O = MessageService::Outcome;
        const std::vector<std::pair<O, std::pair<int, const char *>>> cases = {
            {O::Validation, {400, "VALIDATION_ERROR"}},
            {O::SpaceNotFound, {404, "SPACE_NOT_FOUND"}},
            {O::PartnerRequired, {409, "PARTNER_REQUIRED"}},
            {O::NotFound, {404, "MESSAGE_NOT_FOUND"}},
            {O::Forbidden, {403, "MESSAGE_REPLY_FORBIDDEN"}},
            {O::AlreadyReplied, {409, "MESSAGE_ALREADY_REPLIED"}},
            {O::KeyReused, {409, "IDEMPOTENCY_KEY_REUSED"}},
            {O::ResultUnknown, {503, "RESULT_UNKNOWN"}},
            {O::Error, {500, "INTERNAL_ERROR"}},
        };
        for (const auto &c : cases) {
            CHECK_EQ(MessageService::httpStatusFor(c.first), c.second.first);
            CHECK_EQ(std::string(usapp::errorCodeToString(MessageService::errorCodeFor(c.first))),
                     std::string(c.second.second));
        }
        CHECK_EQ(MessageService::httpStatusFor(O::Created), 201);
        CHECK_EQ(MessageService::httpStatusFor(O::Replayed), 200);
        MessageService::MessageData d;
        d.id = "m";
        CHECK(MessageService::messageJson(d)["reply"].isNull());
        d.hasReply = true;
        d.replyBody = "ok";
        CHECK_EQ(MessageService::messageJson(d)["reply"]["body"].asString(), std::string("ok"));
        MessageService::Status s;
        s.mutation["action"] = "reply";
        s.data = d;
        const auto body = MessageService::responseBody(s);
        CHECK_EQ(body["mutation"]["action"].asString(), std::string("reply"));
        CHECK_EQ(body["message"]["id"].asString(), std::string("m"));
    }
}

}  // namespace usapptest::messageunit
