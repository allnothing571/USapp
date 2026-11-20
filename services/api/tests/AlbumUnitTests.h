// =====================================================================
// services/api/tests/AlbumUnitTests.h
//
// P3-2a: AlbumService pure functions (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/AlbumService.h"

namespace usapptest::albumunit {

using usapp::AlbumService;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline const char *kP1 = "11111111-1111-4111-8111-11111111aaaa";
inline const char *kP2 = "22222222-2222-4222-8222-22222222bbbb";
inline const char *kP1Upper = "11111111-1111-4111-8111-11111111AAAA";

inline std::string repeated(const std::string &unit, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}
inline Json::Value idsBody(int count) {
    Json::Value body(Json::objectValue);
    body["media_ids"] = Json::Value(Json::arrayValue);
    for (int i = 0; i < count; ++i) {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "11111111-1111-4111-8111-%012d", i);
        body["media_ids"].append(buffer);
    }
    return body;
}

inline void runAlbumUnitTests() {
    TEST_CASE("Album: a create body is exactly a name of 1 to 40 code points, one line") {
        std::string name, m;
        CHECK(AlbumService::parseCreateRequest(json(R"({"name":"  旅行  "})"), name, m));
        CHECK_EQ(name, std::string("旅行"));
        Json::Value body(Json::objectValue);
        body["name"] = repeated("天", 40);
        CHECK(AlbumService::parseCreateRequest(body, name, m));
        CHECK_EQ(name, repeated("天", 40));
        body["name"] = repeated("天", 41);
        CHECK_FALSE(AlbumService::parseCreateRequest(body, name, m));
        for (const char *bad : {R"({"name":""})", R"({"name":"   "})", R"({"name":"a\nb"})", R"({"name":"a b"})", R"({"name":1})", R"({"name":null})",
                                R"({})", R"({"name":"a","cover":"b"})", R"([])"}) {
            CHECK_FALSE(AlbumService::parseCreateRequest(json(bad), name, m));
        }
    }

    TEST_CASE("Album: an add is 1 to 20 distinct ids, lowercased before anything hashes them, in request order") {
        std::vector<std::string> ids;
        std::string m;
        CHECK(AlbumService::parseAddRequest(json(std::string(R"({"media_ids":[")") + kP2 + R"(",")" + kP1Upper + R"("]})"), ids, m));
        CHECK(ids == std::vector<std::string>({kP2, kP1}));
        CHECK(AlbumService::parseAddRequest(idsBody(1), ids, m));
        CHECK(AlbumService::parseAddRequest(idsBody(20), ids, m));
        CHECK_EQ(ids.size(), 20u);
        CHECK_FALSE(AlbumService::parseAddRequest(idsBody(0), ids, m));
        CHECK_FALSE(AlbumService::parseAddRequest(idsBody(21), ids, m));
        // Duplicates only by case are duplicates.
        CHECK_FALSE(AlbumService::parseAddRequest(json(std::string(R"({"media_ids":[")") + kP1 + R"(",")" + kP1Upper + R"("]})"), ids, m));
        for (const char *bad : {R"({"media_ids":["not-a-uuid"]})", R"({"media_ids":[1]})", R"({"media_ids":"x"})", R"({})", R"({"media_ids":[],"x":1})", R"([])"}) {
            CHECK_FALSE(AlbumService::parseAddRequest(json(bad), ids, m));
        }
    }

    TEST_CASE("Album: a cover body is exactly one id, lowercased") {
        std::string id, m;
        CHECK(AlbumService::parseCoverRequest(json(std::string(R"({"media_id":")") + kP1Upper + R"("})"), id, m));
        CHECK_EQ(id, std::string(kP1));
        for (const char *bad : {R"({"media_id":"x"})", R"({"media_id":1})", R"({})", R"({"media_id":null})"}) {
            CHECK_FALSE(AlbumService::parseCoverRequest(json(bad), id, m));
        }
    }

    TEST_CASE("Album: fingerprints cover the album, the photos and their order, and are separated by operation") {
        const std::vector<std::string> ab = {kP1, kP2};
        const std::vector<std::string> ba = {kP2, kP1};
        CHECK_EQ(AlbumService::addFingerprint(kP1, ab), AlbumService::addFingerprint(kP1, ab));
        CHECK(AlbumService::addFingerprint(kP1, ab) != AlbumService::addFingerprint(kP1, ba));
        CHECK(AlbumService::addFingerprint(kP1, ab) != AlbumService::addFingerprint(kP2, ab));
        CHECK(AlbumService::removeFingerprint(kP1, kP2) != AlbumService::removeFingerprint(kP2, kP2));
        CHECK(AlbumService::removeFingerprint(kP1, kP2) != AlbumService::removeFingerprint(kP1, kP1));
        CHECK(AlbumService::removeFingerprint(kP1, kP2) != AlbumService::coverFingerprint(kP1, kP2));
        CHECK(AlbumService::createFingerprint("a") != AlbumService::createFingerprint("b"));
    }

    TEST_CASE("Album: cursors round-trip and nothing else decodes") {
        const AlbumService::Cursor cursor{"2026-05-21T01:02:03.000004Z", kP1};
        const auto back = AlbumService::decodeCursor(AlbumService::encodeCursor(cursor));
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->createdAt, cursor.createdAt);
            CHECK_EQ(back->id, cursor.id);
        }
        for (const std::string &bad : {std::string(""), std::string("x"), std::string("2026-05-21T01:02:03.000004Z"), std::string("_") + kP1,
                                       std::string("2026-13-21T01:02:03.000004Z_") + kP1, std::string("2026-05-21T01:02:03Z_") + kP1,
                                       std::string("2026-05-21T25:02:03.000004Z_") + kP1, std::string("2026-05-21T01:02:03.000004Z_nope"),
                                       std::string("2026-05-21 01:02:03.000004Z_") + kP1, repeated("x", 600)}) {
            CHECK_FALSE(AlbumService::decodeCursor(bad).has_value());
        }
        CHECK(AlbumService::decodePhotoCursor("7") == std::optional<int>(7));
        CHECK(AlbumService::decodePhotoCursor("2147483647") == std::optional<int>(2147483647));
        for (const char *bad : {"", "0", "-1", "+1", "07", "1.5", "x", " 1", "2147483648", "99999999999"}) {
            CHECK_FALSE(AlbumService::decodePhotoCursor(bad).has_value());
        }
    }

    TEST_CASE("Album: rename bodies are exact, and fingerprints cover the album, version, name and outcome") {
        int version = 0;
        std::string name, m;
        CHECK(AlbumService::parseRevisionRequest(json(R"({"expected_version":3,"name":"  新名  "})"), version, name, m));
        CHECK_EQ(version, 3);
        CHECK_EQ(name, std::string("新名"));
        for (const char *bad : {R"({"name":"a"})", R"({"expected_version":1})", R"({"expected_version":0,"name":"a"})", R"({"expected_version":-1,"name":"a"})",
                                R"({"expected_version":"1","name":"a"})", R"({"expected_version":1.5,"name":"a"})", R"({"expected_version":1,"name":""})",
                                R"({"expected_version":1,"name":"a\nb"})", R"({"expected_version":1,"name":"a","x":1})", R"([])"}) {
            CHECK_FALSE(AlbumService::parseRevisionRequest(json(bad), version, name, m));
        }
        Json::Value tooLong(Json::objectValue);
        tooLong["expected_version"] = 1;
        tooLong["name"] = repeated("天", 41);
        CHECK_FALSE(AlbumService::parseRevisionRequest(tooLong, version, name, m));
        tooLong["name"] = repeated("天", 40);
        CHECK(AlbumService::parseRevisionRequest(tooLong, version, name, m));

        AlbumService::Decision d = AlbumService::Decision::Reject;
        CHECK(AlbumService::parseDecisionRequest(json(R"({"outcome":"approve"})"), d, m));
        CHECK(d == AlbumService::Decision::Approve);
        CHECK(AlbumService::parseDecisionRequest(json(R"({"outcome":"withdraw"})"), d, m));
        CHECK(d == AlbumService::Decision::Withdraw);
        CHECK(AlbumService::parseDecisionRequest(json(R"({"outcome":"reject"})"), d, m));
        CHECK(d == AlbumService::Decision::Reject);
        for (const char *bad : {R"({"outcome":"approved"})", R"({"outcome":1})", R"({})", R"({"outcome":"approve","x":1})", R"([])"}) {
            CHECK_FALSE(AlbumService::parseDecisionRequest(json(bad), d, m));
        }

        CHECK(AlbumService::renameFingerprint(kP1, 1, "a") != AlbumService::renameFingerprint(kP2, 1, "a"));
        CHECK(AlbumService::renameFingerprint(kP1, 1, "a") != AlbumService::renameFingerprint(kP1, 2, "a"));
        CHECK(AlbumService::renameFingerprint(kP1, 1, "a") != AlbumService::renameFingerprint(kP1, 1, "b"));
        CHECK(AlbumService::renameFingerprint(kP1, 1, "a") != AlbumService::proposeFingerprint(kP1, 1, "a"));
        CHECK(AlbumService::decisionFingerprint(kP1, kP2, AlbumService::Decision::Approve) != AlbumService::decisionFingerprint(kP1, kP2, AlbumService::Decision::Reject));
        CHECK(AlbumService::decisionFingerprint(kP1, kP2, AlbumService::Decision::Approve) != AlbumService::decisionFingerprint(kP2, kP2, AlbumService::Decision::Approve));
        CHECK(AlbumService::decisionFingerprint(kP1, kP1, AlbumService::Decision::Approve) != AlbumService::decisionFingerprint(kP1, kP2, AlbumService::Decision::Approve));
    }

    TEST_CASE("Album: error codes and statuses") {
        using O = AlbumService::Outcome;
        using usapp::ErrorCode;
        CHECK(AlbumService::errorCodeFor(O::NotFound) == ErrorCode::AlbumNotFound);
        CHECK(AlbumService::errorCodeFor(O::PhotoUnavailable) == ErrorCode::AlbumPhotoUnavailable);
        CHECK(AlbumService::errorCodeFor(O::PhotoNotFound) == ErrorCode::AlbumPhotoNotFound);
        CHECK(AlbumService::errorCodeFor(O::PhotoForbidden) == ErrorCode::AlbumPhotoForbidden);
        CHECK(AlbumService::errorCodeFor(O::PhotoRemoved) == ErrorCode::AlbumPhotoRemoved);
        CHECK(AlbumService::errorCodeFor(O::EditForbidden) == ErrorCode::AlbumEditForbidden);
        CHECK(AlbumService::errorCodeFor(O::VersionConflict) == ErrorCode::AlbumVersionConflict);
        CHECK(AlbumService::errorCodeFor(O::ProposalPending) == ErrorCode::AlbumProposalPending);
        CHECK(AlbumService::errorCodeFor(O::ProposalNotFound) == ErrorCode::AlbumProposalNotFound);
        CHECK(AlbumService::errorCodeFor(O::ProposalDecided) == ErrorCode::AlbumProposalDecided);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumEditForbidden), 403);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumVersionConflict), 409);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumProposalPending), 409);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumProposalNotFound), 404);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumProposalDecided), 409);
        CHECK(AlbumService::errorCodeFor(O::KeyReused) == ErrorCode::IdempotencyKeyReused);
        CHECK_EQ(AlbumService::httpStatusFor(O::Created), 201);
        CHECK_EQ(AlbumService::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(AlbumService::httpStatusFor(O::Ok), 200);
        CHECK_EQ(std::string(usapp::errorCodeToString(ErrorCode::AlbumPhotoRemoved)), std::string("ALBUM_PHOTO_REMOVED"));
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumNotFound), 404);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumPhotoNotFound), 404);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumPhotoForbidden), 403);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumPhotoUnavailable), 409);
        CHECK_EQ(usapp::defaultStatusFor(ErrorCode::AlbumPhotoRemoved), 409);
    }
}

}  // namespace usapptest::albumunit
