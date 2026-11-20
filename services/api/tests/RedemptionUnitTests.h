// =====================================================================
// services/api/tests/RedemptionUnitTests.h
//
// M8: RedemptionService pure functions (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/RedemptionService.h"

namespace usapptest::redemptionunit {

using usapp::RedemptionService;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline void runRedemptionUnitTests() {
    TEST_CASE("M9: transition identity includes action and proposal; nullable request operation") {
        using O=RedemptionService::Outcome;
        CHECK(RedemptionService::validTransition("confirm"));
        CHECK_FALSE(RedemptionService::validTransition("refund"));
        CHECK(RedemptionService::transitionFingerprint("r","cancel_approve","p1") != RedemptionService::transitionFingerprint("r","cancel_approve","p2"));
        CHECK(RedemptionService::transitionFingerprint("r","confirm","") != RedemptionService::transitionFingerprint("r","withdraw",""));
        CHECK_EQ(RedemptionService::httpStatusFor(O::Requested),201);
        CHECK_EQ(RedemptionService::httpStatusFor(O::Changed),200);
        CHECK(RedemptionService::errorCodeFor(O::BalanceLimit)==usapp::ErrorCode::WalletBalanceLimit);
        CHECK_EQ(RedemptionService::httpStatusFor(O::InvalidState),409);
        RedemptionService::RedemptionData request;
        request.status="pending_confirmation";
        CHECK(RedemptionService::redemptionJson(request)["operation_id"].isNull());
        CHECK(RedemptionService::redemptionJson(request)["pending_cancellation"].isNull());
    }

    TEST_CASE("Redemption: redeem body is exactly a positive integer expected_version") {
        int v = 0;
        std::string m;
        CHECK(RedemptionService::parseRedeemRequest(json(R"({"expected_version":4})"), v, m));
        CHECK_EQ(v, 4);
        for (const char *bad : {R"({})", R"({"expected_version":0})", R"({"expected_version":-1})",
                                R"({"expected_version":1.5})", R"({"expected_version":"1"})",
                                R"({"expected_version":1,"price_flowers":5})",
                                R"({"expected_version":1,"redeemer_id":"x"})", R"([1])", R"(null)"}) {
            CHECK_FALSE(RedemptionService::parseRedeemRequest(json(bad), v, m));
        }
    }

    TEST_CASE("Redemption: fingerprints bind product and version, or the redemption") {
        const std::string p = "0f8fad5b-d9cb-469f-a165-70867728950e";
        const std::string q = "1f8fad5b-d9cb-469f-a165-70867728950e";
        const auto fp = RedemptionService::redeemFingerprint(p, 1);
        CHECK_EQ(fp.size(), 64u);
        CHECK_EQ(fp, RedemptionService::redeemFingerprint(p, 1));
        CHECK_NE(fp, RedemptionService::redeemFingerprint(p, 2));
        CHECK_NE(fp, RedemptionService::redeemFingerprint(q, 1));
        CHECK_EQ(RedemptionService::fulfillFingerprint(p).size(), 64u);
        CHECK_NE(RedemptionService::fulfillFingerprint(p), RedemptionService::fulfillFingerprint(q));
        CHECK_NE(RedemptionService::fulfillFingerprint(p), fp);
    }

    TEST_CASE("Redemption: list cursor is a strict (created_at, id) pair") {
        const RedemptionService::Cursor c{"2026-10-26T01:02:03.000004Z", "0f8fad5b-d9cb-469f-a165-70867728950e"};
        const auto raw = RedemptionService::encodeCursor(c);
        const auto back = RedemptionService::decodeCursor(raw);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->createdAt, c.createdAt);
            CHECK_EQ(back->id, c.id);
        }
        for (const std::string &bad : {std::string(), raw + "0", raw.substr(1),
                                      std::string("2026-10-26T01:02:03.000004Z-0f8fad5b-d9cb-469f-a165-70867728950e"),
                                      std::string("2026-10-26T01:02:03.000004Z_0F8FAD5B-D9CB-469F-A165-70867728950E")}) {
            CHECK_FALSE(RedemptionService::decodeCursor(bad).has_value());
        }
    }

    TEST_CASE("Redemption: error mapping, statuses and response separation") {
        using O = RedemptionService::Outcome;
        const std::vector<std::pair<O, std::pair<int, const char *>>> cases = {
            {O::Validation, {400, "VALIDATION_ERROR"}},
            {O::SpaceNotFound, {404, "SPACE_NOT_FOUND"}},
            {O::ProductNotFound, {404, "PRODUCT_NOT_FOUND"}},
            {O::ProductArchived, {409, "PRODUCT_ARCHIVED"}},
            {O::VersionConflict, {409, "PRODUCT_VERSION_CONFLICT"}},
            {O::ModeUnsupported, {409, "REDEMPTION_MODE_UNSUPPORTED"}},
            {O::Forbidden, {403, "REDEMPTION_FORBIDDEN"}},
            {O::InsufficientBalance, {409, "INSUFFICIENT_BALANCE"}},
            {O::NotFound, {404, "REDEMPTION_NOT_FOUND"}},
            {O::AlreadyFulfilled, {409, "REDEMPTION_ALREADY_FULFILLED"}},
            {O::KeyReused, {409, "IDEMPOTENCY_KEY_REUSED"}},
            {O::ResultUnknown, {503, "RESULT_UNKNOWN"}},
            {O::Error, {500, "INTERNAL_ERROR"}},
        };
        for (const auto &c : cases) {
            CHECK_EQ(RedemptionService::httpStatusFor(c.first), c.second.first);
            CHECK_EQ(std::string(usapp::errorCodeToString(RedemptionService::errorCodeFor(c.first))),
                     std::string(c.second.second));
        }
        CHECK_EQ(RedemptionService::httpStatusFor(O::Redeemed), 201);
        CHECK_EQ(RedemptionService::httpStatusFor(O::Fulfilled), 200);
        CHECK_EQ(RedemptionService::httpStatusFor(O::Replayed), 200);
        RedemptionService::Status s;
        s.mutation["action"] = "redeem";
        s.redemption.status = "fulfilled";
        s.redemption.fulfilledAt = "2026-10-26T01:02:03.000004Z";
        const auto body = RedemptionService::responseBody(s);
        CHECK_EQ(body["mutation"]["action"].asString(), std::string("redeem"));
        CHECK_EQ(body["redemption"]["status"].asString(), std::string("fulfilled"));
        CHECK(body["redemption"]["fulfilled_by"].isNull());
        CHECK_FALSE(body["redemption"].isMember("balance"));
        RedemptionService::RedemptionData pending;
        CHECK(RedemptionService::redemptionJson(pending)["fulfilled_at"].isNull());
    }
}

}  // namespace usapptest::redemptionunit
