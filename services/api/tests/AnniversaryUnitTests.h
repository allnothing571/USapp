// =====================================================================
// services/api/tests/AnniversaryUnitTests.h
//
// P2-2: AnniversaryCalendar and AnniversaryService pure functions (called from
// UnitTest.cc main). No database.
// =====================================================================
#pragma once

#include <json/json.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/AnniversaryCalendar.h"
#include "services/AnniversaryService.h"

namespace usapptest::anniversaryunit {

using usapp::AnniversaryCalendar;
using usapp::AnniversaryService;
using Cal = AnniversaryCalendar;

inline Json::Value json(const std::string &text) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &v, &errors);
    return v;
}

inline Cal::Civil d(const char *text) {
    return *Cal::parse(text);
}

inline Cal::Candidate candidate(const char *id, Cal::Kind kind, const char *date, const char *createdAt,
                                bool pinned = false) {
    return Cal::Candidate{id, kind, d(date), createdAt, pinned};
}

inline void runAnniversaryUnitTests() {
    using K = Cal::Kind;

    TEST_CASE("Anniversary calendar: strict civil dates and the 1900-2199 input range") {
        CHECK(Cal::parse("2024-02-29").has_value());
        CHECK_FALSE(Cal::parse("2023-02-29").has_value());
        CHECK_FALSE(Cal::parse("2100-02-29").has_value());  // not a leap year
        CHECK(Cal::parse("2000-02-29").has_value());
        for (const char *bad : {"2024-2-29", "2024-13-01", "2024-00-10", "2024-04-31", "20240101", " 2024-01-01",
                                "2024-01-01T00:00", "abcd-ef-gh", "", "0000-01-01"}) {
            CHECK_FALSE(Cal::parse(bad).has_value());
        }
        CHECK(Cal::inInputRange(d("1900-01-01")));
        CHECK(Cal::inInputRange(d("2199-12-31")));
        CHECK_FALSE(Cal::inInputRange(d("1899-12-31")));
        CHECK_FALSE(Cal::inInputRange(d("2200-01-01")));
        CHECK_EQ(Cal::format(d("0999-03-04")), std::string("0999-03-04"));
        CHECK_EQ(Cal::toDays(d("1970-01-01")), static_cast<int64_t>(0));
        CHECK_EQ(Cal::toDays(d("2000-03-01")) - Cal::toDays(d("2000-02-28")), static_cast<int64_t>(2));
        CHECK_EQ(Cal::toDays(d("2100-03-01")) - Cal::toDays(d("2100-02-28")), static_cast<int64_t>(1));
        for (const char *t : {"1900-01-01", "1969-12-31", "2024-02-29", "2199-12-31", "2400-02-29"}) {
            CHECK(Cal::fromDays(Cal::toDays(d(t))) == d(t));
        }
    }

    TEST_CASE("Anniversary calendar: elapsed days and the next yearly occurrence around D = T") {
        const auto today = d("2026-10-29");
        auto c = Cal::compute(K::Anniversary, d("2026-10-29"), today);  // D = T
        CHECK_EQ(c.daysSince, static_cast<int64_t>(0));
        CHECK(c.next == today);
        CHECK_EQ(c.years, 0);
        CHECK_EQ(c.nextDaysUntil, static_cast<int64_t>(0));
        c = Cal::compute(K::Anniversary, d("2026-10-28"), today);  // D = T - 1
        CHECK_EQ(c.daysSince, static_cast<int64_t>(1));
        CHECK(c.next == d("2027-10-28"));
        CHECK_EQ(c.years, 1);
        CHECK_EQ(c.nextDaysUntil, static_cast<int64_t>(364));
        c = Cal::compute(K::Anniversary, d("2026-10-30"), today);  // D = T + 1: not started yet
        CHECK_EQ(c.daysSince, static_cast<int64_t>(-1));
        CHECK(c.next == d("2026-10-30"));
        CHECK_EQ(c.years, 0);
        CHECK_EQ(c.nextDaysUntil, static_cast<int64_t>(1));
        c = Cal::compute(K::Anniversary, d("2020-10-29"), today);  // 6th anniversary today
        CHECK(c.next == today);
        CHECK_EQ(c.years, 6);
        CHECK_EQ(c.nextDaysUntil, static_cast<int64_t>(0));
        CHECK_EQ(c.daysSince, Cal::toDays(today) - Cal::toDays(d("2020-10-29")));
        c = Cal::compute(K::Anniversary, d("2020-12-31"), today);
        CHECK(c.next == d("2026-12-31"));
        CHECK_EQ(c.years, 6);
        c = Cal::compute(K::Anniversary, d("2020-01-01"), today);
        CHECK(c.next == d("2027-01-01"));
        CHECK_EQ(c.years, 7);
    }

    TEST_CASE("Anniversary calendar: Feb 29 falls on Feb 28 in non-leap years, always from the original") {
        const auto leap = d("2024-02-29");
        CHECK(Cal::addYears(leap, 1) == d("2025-02-28"));
        CHECK(Cal::addYears(leap, 4) == d("2028-02-29"));
        CHECK(Cal::addYears(d("2096-02-29"), 4) == d("2100-02-28"));  // century, not leap
        CHECK(Cal::addYears(d("1996-02-29"), 4) == d("2000-02-29"));  // 400-year leap
        auto c = Cal::compute(K::Anniversary, leap, d("2025-02-28"));
        CHECK(c.next == d("2025-02-28"));
        CHECK_EQ(c.years, 1);
        CHECK_EQ(c.nextDaysUntil, static_cast<int64_t>(0));
        c = Cal::compute(K::Anniversary, leap, d("2025-03-01"));
        CHECK(c.next == d("2026-02-28"));
        c = Cal::compute(K::Anniversary, leap, d("2027-03-01"));
        CHECK(c.next == d("2028-02-29"));  // back to Feb 29, not drifted to the 28th
        CHECK_EQ(c.years, 4);
        c = Cal::compute(K::Anniversary, d("2096-02-29"), d("2100-01-01"));
        CHECK(c.next == d("2100-02-28"));
    }

    TEST_CASE("Anniversary calendar: range edges; derived occurrences may pass 2199") {
        auto c = Cal::compute(K::Anniversary, d("1900-01-01"), d("2026-10-29"));
        CHECK(c.next == d("2027-01-01"));
        CHECK_EQ(c.years, 127);
        c = Cal::compute(K::Anniversary, d("2199-12-31"), d("2200-01-01"));
        CHECK(c.next == d("2200-12-31"));  // beyond the input bound, not truncated or rejected
        CHECK_EQ(c.years, 1);
        CHECK_EQ(c.daysSince, static_cast<int64_t>(1));
        c = Cal::compute(K::Anniversary, d("2196-02-29"), d("2200-03-01"));
        CHECK(c.next == d("2201-02-28"));
        CHECK_EQ(Cal::format(c.next), std::string("2201-02-28"));
    }

    TEST_CASE("Anniversary calendar: countdowns do not recur") {
        const auto today = d("2026-10-29");
        CHECK_EQ(Cal::compute(K::Countdown, d("2026-11-01"), today).daysUntil, static_cast<int64_t>(3));
        CHECK_EQ(Cal::compute(K::Countdown, today, today).daysUntil, static_cast<int64_t>(0));
        CHECK_EQ(Cal::compute(K::Countdown, d("2026-10-19"), today).daysUntil, static_cast<int64_t>(-10));
    }

    TEST_CASE("Anniversary calendar: the result depends only on the supplied Beijing date") {
        // 2026-10-29T16:30Z is already 2026-10-30 in Beijing; the database supplies the Beijing
        // date, and the same item reads differently from the UTC date.
        const auto item = d("2025-10-30");
        const auto beijing = Cal::compute(K::Anniversary, item, d("2026-10-30"));
        const auto utc = Cal::compute(K::Anniversary, item, d("2026-10-29"));
        CHECK_EQ(beijing.nextDaysUntil, static_cast<int64_t>(0));
        CHECK_EQ(beijing.years, 1);
        CHECK_EQ(utc.nextDaysUntil, static_cast<int64_t>(1));
    }

    TEST_CASE("Anniversary selection: pinned > auto > auto_past > none, with deterministic tie-breaks") {
        const auto today = d("2026-10-29");
        CHECK(Cal::select({}, today).source == Cal::Source::None);
        CHECK_FALSE(Cal::select({}, today).index.has_value());
        std::vector<Cal::Candidate> items = {
            candidate("00000000-0000-4000-8000-000000000001", K::Anniversary, "2020-11-05", "2026-01-01T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000002", K::Countdown, "2026-11-01", "2026-01-02T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000003", K::Countdown, "2026-10-01", "2026-01-03T00:00:00.000000Z"),
        };
        auto s = Cal::select(items, today);
        CHECK(s.source == Cal::Source::Auto);
        CHECK_EQ(*s.index, static_cast<size_t>(1));  // 3 days beats 7 days; the past countdown is skipped
        items[2].pinned = true;
        s = Cal::select(items, today);
        CHECK(s.source == Cal::Source::Pinned);
        CHECK_EQ(*s.index, static_cast<size_t>(2));  // a pin wins even for a past countdown
        // Equal distance: earlier created_at, then smaller id.
        std::vector<Cal::Candidate> ties = {
            candidate("00000000-0000-4000-8000-00000000000b", K::Countdown, "2026-11-01", "2026-01-02T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-00000000000a", K::Anniversary, "2000-11-01", "2026-01-02T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000009", K::Countdown, "2026-11-01", "2026-01-03T00:00:00.000000Z"),
        };
        s = Cal::select(ties, today);
        CHECK(s.source == Cal::Source::Auto);
        CHECK_EQ(*s.index, static_cast<size_t>(1));
        // An anniversary today (distance 0) beats a countdown tomorrow.
        std::vector<Cal::Candidate> zero = {
            candidate("00000000-0000-4000-8000-000000000001", K::Countdown, "2026-10-30", "2026-01-01T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000002", K::Anniversary, "2019-10-29", "2026-02-01T00:00:00.000000Z"),
        };
        CHECK_EQ(*Cal::select(zero, today).index, static_cast<size_t>(1));
        // Only past countdowns: the most recently passed, then created_at, then id.
        std::vector<Cal::Candidate> past = {
            candidate("00000000-0000-4000-8000-000000000001", K::Countdown, "2026-09-01", "2026-01-01T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000003", K::Countdown, "2026-10-20", "2026-01-02T00:00:00.000000Z"),
            candidate("00000000-0000-4000-8000-000000000002", K::Countdown, "2026-10-20", "2026-01-02T00:00:00.000000Z"),
        };
        s = Cal::select(past, today);
        CHECK(s.source == Cal::Source::AutoPast);
        CHECK_EQ(*s.index, static_cast<size_t>(2));
        CHECK_EQ(std::string(Cal::sourceName(Cal::Source::AutoPast)), std::string("auto_past"));
    }

    TEST_CASE("Anniversary: create and revision bodies are exact; terms are normalised") {
        AnniversaryService::Terms t;
        std::string m;
        CHECK(AnniversaryService::parseCreateRequest(
            json(R"({"title":"  在一起 ","date":"2020-05-20","kind":"anniversary","note":" 第一次见面\r\n "})"), t, m));
        CHECK_EQ(t.title, std::string("在一起"));
        CHECK_EQ(t.date, std::string("2020-05-20"));
        CHECK_EQ(t.note, std::string("第一次见面"));
        CHECK(AnniversaryService::parseCreateRequest(json(R"({"title":"考试","date":"2026-12-20","kind":"countdown"})"),
                                                     t, m));
        CHECK_EQ(t.note, std::string());
        for (const char *bad : {
                 R"({})", R"({"title":"a","date":"2020-05-20"})", R"({"title":"","date":"2020-05-20","kind":"countdown"})",
                 R"({"title":"a\nb","date":"2020-05-20","kind":"countdown"})",
                 R"({"title":"a","date":"2020-5-20","kind":"countdown"})",
                 R"({"title":"a","date":"1899-12-31","kind":"countdown"})",
                 R"({"title":"a","date":"2200-01-01","kind":"countdown"})",
                 R"({"title":"a","date":"2023-02-29","kind":"countdown"})",
                 R"({"title":"a","date":"2020-05-20","kind":"birthday"})",
                 R"({"title":"a","date":"2020-05-20","kind":"countdown","note":1})",
                 R"({"title":"a","date":"2020-05-20","kind":"countdown","note":"x\u0007"})",
                 R"({"title":"a","date":"2020-05-20","kind":"countdown","creator_id":"x"})",
                 R"({"title":"a","date":"2020-05-20","kind":"countdown","expected_version":1})", R"([1])"}) {
            CHECK_FALSE(AnniversaryService::parseCreateRequest(json(bad), t, m));
        }
        std::string title41;
        for (int i = 0; i < 41; ++i) title41 += "\xE5\xA4\xA9";  // 41 x CJK
        Json::Value longTitle = json(R"({"date":"2020-05-20","kind":"countdown"})");
        longTitle["title"] = title41;
        CHECK_FALSE(AnniversaryService::parseCreateRequest(longTitle, t, m));
        longTitle["title"] = title41.substr(3);
        CHECK(AnniversaryService::parseCreateRequest(longTitle, t, m));
        Json::Value longNote = json(R"({"title":"a","date":"2020-05-20","kind":"countdown"})");
        longNote["note"] = std::string(501, 'x');
        CHECK_FALSE(AnniversaryService::parseCreateRequest(longNote, t, m));
        longNote["note"] = std::string(500, 'x');
        CHECK(AnniversaryService::parseCreateRequest(longNote, t, m));

        int v = 0;
        CHECK(AnniversaryService::parseRevisionRequest(
            json(R"({"expected_version":3,"title":"a","date":"2020-05-20","kind":"anniversary"})"), v, t, m));
        CHECK_EQ(v, 3);
        for (const char *bad : {R"({"title":"a","date":"2020-05-20","kind":"anniversary"})",
                                R"({"expected_version":0,"title":"a","date":"2020-05-20","kind":"anniversary"})",
                                R"({"expected_version":1.5,"title":"a","date":"2020-05-20","kind":"anniversary"})",
                                R"({"expected_version":"1","title":"a","date":"2020-05-20","kind":"anniversary"})",
                                R"({"expected_version":1,"title":"a","date":"2020-05-20","kind":"anniversary","x":1})"}) {
            CHECK_FALSE(AnniversaryService::parseRevisionRequest(json(bad), v, t, m));
        }
    }

    TEST_CASE("Anniversary: decision and pin bodies") {
        AnniversaryService::Decision decision = AnniversaryService::Decision::Approve;
        std::string m;
        CHECK(AnniversaryService::parseDecisionRequest(json(R"({"outcome":"withdraw"})"), decision, m));
        CHECK(decision == AnniversaryService::Decision::Withdraw);
        CHECK(AnniversaryService::parseDecisionRequest(json(R"({"outcome":"reject"})"), decision, m));
        CHECK(decision == AnniversaryService::Decision::Reject);
        for (const char *bad : {R"({})", R"({"outcome":"approved"})", R"({"outcome":"approve","reason":"x"})"}) {
            CHECK_FALSE(AnniversaryService::parseDecisionRequest(json(bad), decision, m));
        }
        std::optional<std::string> id;
        CHECK(AnniversaryService::parsePinRequest(json(R"({"anniversary_id":null})"), id, m));
        CHECK_FALSE(id.has_value());
        CHECK(AnniversaryService::parsePinRequest(
            json(R"({"anniversary_id":"ABCDEF01-2345-4678-89AB-CDEF01234567"})"), id, m));
        CHECK_EQ(*id, std::string("abcdef01-2345-4678-89ab-cdef01234567"));
        for (const char *bad : {R"({})", R"({"anniversary_id":"x"})", R"({"anniversary_id":1})",
                                R"({"anniversary_id":null,"user_id":"x"})"}) {
            CHECK_FALSE(AnniversaryService::parsePinRequest(json(bad), id, m));
        }
    }

    TEST_CASE("Anniversary: fingerprints separate actions, targets, versions and terms") {
        AnniversaryService::Terms t{"a", "2020-05-20", "anniversary", ""};
        AnniversaryService::Terms other = t;
        other.note = "x";
        const std::string id = "00000000-0000-4000-8000-000000000001";
        const std::string id2 = "00000000-0000-4000-8000-000000000002";
        std::vector<std::string> all = {
            AnniversaryService::createFingerprint(t),
            AnniversaryService::createFingerprint(other),
            AnniversaryService::editFingerprint(id, 1, t),
            AnniversaryService::editFingerprint(id, 2, t),
            AnniversaryService::editFingerprint(id2, 1, t),
            AnniversaryService::proposeFingerprint(id, 1, t),
            AnniversaryService::decisionFingerprint(id, id2, AnniversaryService::Decision::Approve),
            AnniversaryService::decisionFingerprint(id, id2, AnniversaryService::Decision::Reject),
            AnniversaryService::decisionFingerprint(id2, id, AnniversaryService::Decision::Approve),
            AnniversaryService::pinFingerprint(std::nullopt),
            AnniversaryService::pinFingerprint(id),
            AnniversaryService::pinFingerprint(id2),
        };
        for (size_t i = 0; i < all.size(); ++i) {
            CHECK_EQ(all[i].size(), static_cast<size_t>(64));
            for (size_t j = i + 1; j < all.size(); ++j) CHECK(all[i] != all[j]);
        }
        CHECK_EQ(AnniversaryService::createFingerprint(t), AnniversaryService::createFingerprint(t));
    }

    TEST_CASE("Anniversary: computed JSON, cursor and error mapping") {
        AnniversaryService::Terms t{"a", "2024-02-29", "anniversary", ""};
        auto c = AnniversaryService::computedJson(t, "2025-03-01");
        CHECK_EQ(c["days_since"].asInt64(), static_cast<Json::Int64>(366));
        CHECK_EQ(c["next"]["date"].asString(), std::string("2026-02-28"));
        CHECK_EQ(c["next"]["years"].asInt(), 2);
        CHECK(c["days_until"].isNull());
        t.kind = "countdown";
        c = AnniversaryService::computedJson(t, "2024-03-01");
        CHECK_EQ(c["days_until"].asInt64(), static_cast<Json::Int64>(-1));
        CHECK(c["next"].isNull());
        CHECK(c["days_since"].isNull());

        const AnniversaryService::Cursor cursor{"2026-10-29T01:02:03.456789Z", "00000000-0000-4000-8000-000000000001"};
        const auto encoded = AnniversaryService::encodeCursor(cursor);
        const auto decoded = AnniversaryService::decodeCursor(encoded);
        CHECK(decoded.has_value());
        CHECK_EQ(decoded->id, cursor.id);
        CHECK_FALSE(AnniversaryService::decodeCursor(encoded.substr(1)).has_value());
        CHECK_FALSE(AnniversaryService::decodeCursor("2026-10-29T01:02:03.456789Z_not-a-uuid").has_value());

        using O = AnniversaryService::Outcome;
        CHECK_EQ(AnniversaryService::httpStatusFor(O::Created), 201);
        CHECK_EQ(AnniversaryService::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(AnniversaryService::httpStatusFor(O::NotFound), 404);
        CHECK_EQ(AnniversaryService::httpStatusFor(O::ProposalNotFound), 404);
        CHECK_EQ(AnniversaryService::httpStatusFor(O::Forbidden), 403);
        for (O o : {O::VersionConflict, O::ProposalPending, O::ProposalDecided, O::ProposalStale, O::KeyReused}) {
            CHECK_EQ(AnniversaryService::httpStatusFor(o), 409);
        }
        CHECK_EQ(AnniversaryService::httpStatusFor(O::ResultUnknown), 503);
        CHECK_EQ(std::string(usapp::errorCodeToString(AnniversaryService::errorCodeFor(O::ProposalStale))),
                 std::string("ANNIVERSARY_PROPOSAL_STALE"));
        CHECK_EQ(std::string(usapp::errorCodeToString(AnniversaryService::errorCodeFor(O::Forbidden))),
                 std::string("ANNIVERSARY_EDIT_FORBIDDEN"));
    }
}

}  // namespace usapptest::anniversaryunit
