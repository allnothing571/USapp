// =====================================================================
// services/api/tests/ProductUnitTests.h
//
// M7: ProductService pure functions (called from UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/ProductService.h"

namespace usapptest::productunit {

using usapp::ProductService;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline void runProductUnitTests() {
    TEST_CASE("Product: create request uses task text rules, price 1-999 flowers and a stored mode") {
        ProductService::Terms t;
        std::string m;
        CHECK(ProductService::parseCreateRequest(
            json(R"({"title":"  Movie night ","description":"a\r\nb ","price_flowers":1,"redemption_mode":"direct"})"), t, m));
        CHECK_EQ(t.title, std::string("Movie night"));
        CHECK_EQ(t.description, std::string("a\nb"));
        CHECK_EQ(t.priceFlowers, 1);
        CHECK(ProductService::parseCreateRequest(
            json(R"({"title":"x","price_flowers":999,"redemption_mode":"request"})"), t, m));
        CHECK_EQ(t.description, std::string());
        for (const char *bad : {
                 R"({"title":"x","price_flowers":0,"redemption_mode":"direct"})",
                 R"({"title":"x","price_flowers":1000,"redemption_mode":"direct"})",
                 R"({"title":"x","price_flowers":1.5,"redemption_mode":"direct"})",
                 R"({"title":"x","price_flowers":"5","redemption_mode":"direct"})",
                 R"({"title":"x","price_flowers":5})",
                 R"({"title":"x","price_flowers":5,"redemption_mode":"auto"})",
                 R"({"title":"","price_flowers":5,"redemption_mode":"direct"})",
                 R"({"title":"a\nb","price_flowers":5,"redemption_mode":"direct"})",
                 R"({"title":"x","price_flowers":5,"redemption_mode":"direct","price_sunshine":1})",
                 R"({"title":"x","price_flowers":5,"redemption_mode":"direct","owner_id":"y"})",
             }) {
            CHECK_FALSE(ProductService::parseCreateRequest(json(bad), t, m));
        }
        CHECK_FALSE(ProductService::parseCreateRequest(
            json(R"({"title":")" + std::string(41, 'x') + R"(","price_flowers":5,"redemption_mode":"direct"})"), t, m));
    }

    TEST_CASE("Product: edit needs expected_version; archive takes only expected_version") {
        ProductService::Terms t;
        int v = 0;
        std::string m;
        CHECK(ProductService::parseEditRequest(
            json(R"({"expected_version":2,"title":"x","price_flowers":5,"redemption_mode":"direct"})"), t, v, m));
        CHECK_EQ(v, 2);
        CHECK_FALSE(ProductService::parseEditRequest(
            json(R"({"title":"x","price_flowers":5,"redemption_mode":"direct"})"), t, v, m));
        CHECK_FALSE(ProductService::parseEditRequest(
            json(R"({"expected_version":0,"title":"x","price_flowers":5,"redemption_mode":"direct"})"), t, v, m));
        CHECK(ProductService::parseArchiveRequest(json(R"({"expected_version":3})"), v, m));
        CHECK_EQ(v, 3);
        CHECK_FALSE(ProductService::parseArchiveRequest(json(R"({"expected_version":3,"title":"x"})"), v, m));
        CHECK_FALSE(ProductService::parseArchiveRequest(json(R"({})"), v, m));
    }

    TEST_CASE("Product: fingerprints bind action, product, expected version and terms") {
        ProductService::Terms a;
        a.title = "x";
        a.priceFlowers = 5;
        a.redemptionMode = "direct";
        auto b = a;
        b.priceFlowers = 6;
        using A = ProductService::Action;
        const std::string p = "0f8fad5b-d9cb-469f-a165-70867728950e";
        const auto fp = ProductService::fingerprint(A::Edit, p, 1, a);
        CHECK_EQ(fp.size(), 64u);
        CHECK_EQ(fp, ProductService::fingerprint(A::Edit, p, 1, a));
        CHECK_NE(fp, ProductService::fingerprint(A::Edit, p, 2, a));
        CHECK_NE(fp, ProductService::fingerprint(A::Edit, p, 1, b));
        CHECK_NE(fp, ProductService::fingerprint(A::Create, "", 0, a));
        CHECK_NE(ProductService::fingerprint(A::Archive, p, 1, a), ProductService::fingerprint(A::Archive, p, 2, a));
        CHECK_EQ(ProductService::fingerprint(A::Archive, p, 1, a), ProductService::fingerprint(A::Archive, p, 1, b));
    }

    TEST_CASE("Product: list cursor is a strict (created_at, id) pair; invalid input is rejected") {
        const ProductService::Cursor c{"2026-10-26T01:02:03.000004Z", "0f8fad5b-d9cb-469f-a165-70867728950e"};
        const auto raw = ProductService::encodeCursor(c);
        const auto back = ProductService::decodeCursor(raw);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->createdAt, c.createdAt);
            CHECK_EQ(back->id, c.id);
        }
        for (const std::string &bad : {std::string(), std::string("x"), raw + "0", raw.substr(1),
                                      std::string("2026-10-26T01:02:03.000004Z-0f8fad5b-d9cb-469f-a165-70867728950e"),
                                      std::string("2026-13-26T01:02:03.000004Z_0f8fad5b-d9cb-469f-a165-70867728950e"),
                                      std::string("2026-10-26T01:02:03.000004Z_0F8FAD5B-D9CB-469F-A165-70867728950E"),
                                      std::string("2026-10-26 01:02:03.000004Z_0f8fad5b-d9cb-469f-a165-70867728950e")}) {
            CHECK_FALSE(ProductService::decodeCursor(bad).has_value());
        }
    }

    TEST_CASE("Product: error mapping, statuses and response separation") {
        using O = ProductService::Outcome;
        const std::vector<std::pair<O, std::pair<int, const char *>>> cases = {
            {O::Validation, {400, "VALIDATION_ERROR"}},
            {O::SpaceNotFound, {404, "SPACE_NOT_FOUND"}},
            {O::PartnerRequired, {409, "PARTNER_REQUIRED"}},
            {O::NotFound, {404, "PRODUCT_NOT_FOUND"}},
            {O::Forbidden, {403, "PRODUCT_FORBIDDEN"}},
            {O::Archived, {409, "PRODUCT_ARCHIVED"}},
            {O::VersionConflict, {409, "PRODUCT_VERSION_CONFLICT"}},
            {O::KeyReused, {409, "IDEMPOTENCY_KEY_REUSED"}},
            {O::ResultUnknown, {503, "RESULT_UNKNOWN"}},
            {O::Error, {500, "INTERNAL_ERROR"}},
        };
        for (const auto &c : cases) {
            CHECK_EQ(ProductService::httpStatusFor(c.first), c.second.first);
            CHECK_EQ(std::string(usapp::errorCodeToString(ProductService::errorCodeFor(c.first))),
                     std::string(c.second.second));
        }
        CHECK_EQ(ProductService::httpStatusFor(O::Created), 201);
        CHECK_EQ(ProductService::httpStatusFor(O::Updated), 200);
        CHECK_EQ(ProductService::httpStatusFor(O::Replayed), 200);
        ProductService::Status s;
        s.mutation["action"] = "edit";
        s.product.status = "archived";
        const auto body = ProductService::responseBody(s);
        CHECK_EQ(body["mutation"]["action"].asString(), std::string("edit"));
        CHECK_EQ(body["product"]["status"].asString(), std::string("archived"));
        CHECK_FALSE(body["product"].isMember("balance"));
    }
}

}  // namespace usapptest::productunit
