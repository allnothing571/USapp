// =====================================================================
// services/api/tests/MemoryUnitTests.h
//
// P3-1b: MemoryService pure functions (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <string>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/MemoryService.h"
#include "services/TaskService.h"

namespace usapptest::memoryunit {

using usapp::MemoryService;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline const char *kP1 = "11111111-1111-4111-8111-111111111111";
inline const char *kP2 = "22222222-2222-4222-8222-222222222222";

inline std::string repeated(const std::string &unit, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

inline Json::Value validBody() {
    Json::Value body(Json::objectValue);
    body["date"] = "2026-05-20";
    body["title"] = "t";
    body["media_ids"] = Json::Value(Json::arrayValue);
    body["media_ids"].append(kP1);
    return body;
}

inline void runMemoryUnitTests() {
    TEST_CASE("Memory: create bodies are exact, terms are normalised, and photo order is kept") {
        MemoryService::Terms t;
        std::string m;
        Json::Value body = validBody();
        body["title"] = "  海边 ";
        body["description"] = " 第一行\r\n第二行\r\n ";
        body["media_ids"] = Json::Value(Json::arrayValue);
        body["media_ids"].append(kP2);
        body["media_ids"].append("11111111-1111-4111-8111-111111111111");
        CHECK(MemoryService::parseCreateRequest(body, t, m));
        CHECK_EQ(t.title, std::string("海边"));
        CHECK_EQ(t.description, std::string("第一行\n第二行"));
        CHECK_EQ(t.date, std::string("2026-05-20"));
        CHECK_EQ(t.mediaIds.size(), 2u);
        CHECK_EQ(t.mediaIds[0], std::string(kP2));
        for (const char *bad : {
                 R"({})", R"([1])",
                 R"({"date":"2026-05-20","title":"t"})",
                 R"({"date":"2026-05-20","title":"t","media_ids":[]})",
                 R"({"date":"2026-05-20","title":"t","media_ids":"x"})",
                 R"({"date":"2026-05-20","title":"t","media_ids":["x"]})",
                 R"({"date":"2026-05-20","title":"t","media_ids":[1]})",
                 R"({"date":"2026-5-20","title":"t","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"1899-12-31","title":"t","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2023-02-29","title":"t","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":20260520,"title":"t","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"a\nb","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"t","description":1,"media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"t","description":"x\u0007","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"t","creator_id":"x","media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"t","expected_version":1,"media_ids":["11111111-1111-4111-8111-111111111111"]})",
                 R"({"date":"2026-05-20","title":"t","media_ids":["11111111-1111-4111-8111-111111111111","11111111-1111-4111-8111-111111111111"]})"}) {
            CHECK_FALSE(MemoryService::parseCreateRequest(json(bad), t, m));
        }
    }

    TEST_CASE("Memory: the reviewed limits hold at their edges: 60, 2000 and nine") {
        MemoryService::Terms t;
        std::string m;
        Json::Value body = validBody();
        body["title"] = repeated("\xE5\xA4\xA9", 60);
        CHECK(MemoryService::parseCreateRequest(body, t, m));
        body["title"] = repeated("\xE5\xA4\xA9", 61);
        CHECK_FALSE(MemoryService::parseCreateRequest(body, t, m));
        body = validBody();
        body["description"] = repeated("\xE5\xA4\xA9", 2000);
        CHECK(MemoryService::parseCreateRequest(body, t, m));
        body["description"] = repeated("\xF0\x9F\x8C\x8A", 2000);  // Four bytes each: still within the raw limit.
        CHECK(MemoryService::parseCreateRequest(body, t, m));
        body["description"] = repeated("\xE5\xA4\xA9", 2001);
        CHECK_FALSE(MemoryService::parseCreateRequest(body, t, m));
        body = validBody();
        body["media_ids"] = Json::Value(Json::arrayValue);
        for (int i = 1; i <= 9; ++i) body["media_ids"].append("1111111" + std::to_string(i) + "-1111-4111-8111-111111111111");
        CHECK(MemoryService::parseCreateRequest(body, t, m));
        body["media_ids"].append("11111110-1111-4111-8111-111111111111");
        CHECK_FALSE(MemoryService::parseCreateRequest(body, t, m));
        // A task's own limits are not moved by the shared helper's new parameter.
        std::string out;
        CHECK(usapp::TaskService::normalizeTitle(repeated("x", 41), out) == usapp::TaskService::TextError::TooLong);
        CHECK(usapp::TaskService::normalizeDescription(repeated("x", 501), out) == usapp::TaskService::TextError::TooLong);
    }

    TEST_CASE("Memory: revision and decision bodies are exact") {
        MemoryService::Terms t;
        std::string m;
        int version = 0;
        Json::Value body = validBody();
        body["expected_version"] = 3;
        CHECK(MemoryService::parseRevisionRequest(body, version, t, m));
        CHECK_EQ(version, 3);
        for (const char *bad : {"0", "-1", "1.5", "\"1\"", "true", "null", "99999999999"}) {
            body["expected_version"] = json(std::string("[") + bad + "]")[0];
            CHECK_FALSE(MemoryService::parseRevisionRequest(body, version, t, m));
        }
        CHECK_FALSE(MemoryService::parseRevisionRequest(validBody(), version, t, m));
        MemoryService::Decision d = MemoryService::Decision::Reject;
        CHECK(MemoryService::parseDecisionRequest(json(R"({"outcome":"approve"})"), d, m));
        CHECK(d == MemoryService::Decision::Approve);
        CHECK(MemoryService::parseDecisionRequest(json(R"({"outcome":"withdraw"})"), d, m));
        CHECK(d == MemoryService::Decision::Withdraw);
        for (const char *bad : {R"({})", R"({"outcome":"approved"})", R"({"outcome":1})", R"({"outcome":"reject","x":1})"}) {
            CHECK_FALSE(MemoryService::parseDecisionRequest(json(bad), d, m));
        }
    }

    TEST_CASE("Memory: fingerprints cover every term and photo order, and are separated by operation") {
        MemoryService::Terms a{"2026-05-20", "t", "d", {kP1, kP2}};
        const auto base = MemoryService::createFingerprint(a);
        CHECK_EQ(base.size(), 64u);
        CHECK_EQ(MemoryService::createFingerprint(a), base);
        MemoryService::Terms b = a;
        b.mediaIds = {kP2, kP1};
        CHECK(MemoryService::createFingerprint(b) != base);
        b = a; b.date = "2026-05-21";
        CHECK(MemoryService::createFingerprint(b) != base);
        b = a; b.title = "u";
        CHECK(MemoryService::createFingerprint(b) != base);
        b = a; b.description = "e";
        CHECK(MemoryService::createFingerprint(b) != base);
        // A field boundary cannot be moved: title "ab" + "" differs from "a" + "b".
        MemoryService::Terms left{"2026-05-20", "ab", "", {kP1}};
        MemoryService::Terms right{"2026-05-20", "a", "b", {kP1}};
        CHECK(MemoryService::createFingerprint(left) != MemoryService::createFingerprint(right));
        CHECK(MemoryService::editFingerprint(kP1, 1, a) != MemoryService::proposeFingerprint(kP1, 1, a));
        CHECK(MemoryService::editFingerprint(kP1, 1, a) != MemoryService::editFingerprint(kP1, 2, a));
        CHECK(MemoryService::editFingerprint(kP1, 1, a) != MemoryService::editFingerprint(kP2, 1, a));
        CHECK(MemoryService::decisionFingerprint(kP1, kP2, MemoryService::Decision::Approve) !=
              MemoryService::decisionFingerprint(kP1, kP2, MemoryService::Decision::Reject));
    }

    TEST_CASE("Memory: the cursor round-trips and nothing else decodes") {
        const MemoryService::Cursor cursor{"2026-05-20", "2026-05-21T01:02:03.000004Z", kP1};
        const auto raw = MemoryService::encodeCursor(cursor);
        const auto back = MemoryService::decodeCursor(raw);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->date, cursor.date);
            CHECK_EQ(back->createdAt, cursor.createdAt);
            CHECK_EQ(back->id, cursor.id);
        }
        const std::string id(kP1);
        for (const std::string &bad : {
                 std::string(), std::string("x"), std::string("a_b_c"),
                 "2026-5-20_2026-05-21T01:02:03.000004Z_" + id,
                 "2026-02-30_2026-05-21T01:02:03.000004Z_" + id,
                 "2026-05-20_2026-05-21 01:02:03.000004Z_" + id,
                 "2026-05-20_2026-05-21T01:02:03Z_" + id,
                 "2026-05-20_2026-05-21T25:02:03.000004Z_" + id,
                 "2026-05-20_2026-13-21T01:02:03.000004Z_" + id,
                 "2026-05-20_2026-05-21T01:02:03.00000xZ_" + id,
                 std::string("2026-05-20_2026-05-21T01:02:03.000004Z_not-a-uuid"),
                 "2026-05-20_2026-05-21T01:02:03.000004Z_" + id + "'; DROP TABLE memories",
                 std::string(600, 'x')}) {
            CHECK_FALSE(MemoryService::decodeCursor(bad).has_value());
        }
    }

    TEST_CASE("Memory: error codes and statuses") {
        using O = MemoryService::Outcome;
        using usapp::ErrorCode;
        CHECK(MemoryService::errorCodeFor(O::NotFound) == ErrorCode::MemoryNotFound);
        CHECK(MemoryService::errorCodeFor(O::PhotoUnavailable) == ErrorCode::MemoryPhotoUnavailable);
        CHECK(MemoryService::errorCodeFor(O::KeyReused) == ErrorCode::IdempotencyKeyReused);
        CHECK_EQ(MemoryService::httpStatusFor(O::Created), 201);
        CHECK_EQ(MemoryService::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(std::string(usapp::errorCodeToString(ErrorCode::MemoryPhotoUnavailable)), std::string("MEMORY_PHOTO_UNAVAILABLE"));
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::MemoryNotFound), 404);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::MemoryEditForbidden), 403);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::MemoryProposalDecided), 409);
    }
}

}  // namespace usapptest::memoryunit
