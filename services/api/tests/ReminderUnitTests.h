#pragma once

// P2-6 R1: pure request parsing, fingerprints and error mapping (no database).
#include <json/json.h>

#include <string>

#include "TestSupport.h"
#include "core/Errors.h"
#include "services/MediaService.h"
#include "services/ReminderService.h"

namespace usapptest::reminderunit {

using RS = usapp::ReminderService;

inline Json::Value parseJson(const std::string &text) {
    Json::Value out;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &out, &errors);
    return out;
}
inline bool task(const std::string &text, RS::Terms &terms) {
    std::string message;
    return RS::parseTaskRequest(parseJson(text), terms, message);
}
inline bool anniversary(const std::string &text, RS::Terms &terms) {
    std::string message;
    return RS::parseAnniversaryRequest(parseJson(text), terms, message);
}

inline void runReminderUnitTests() {
    TEST_CASE("P3-1a: an upload request is exact, and a decision comes only from inspected facts") {
        using M = usapp::MediaService;
        std::string type, message;
        int64_t size = 0;
        CHECK(M::parseUploadRequest(parseJson(R"({"content_type":"image/png","size_bytes":20000000})"), type, size, message));
        CHECK_EQ(size, 20000000);
        for (const char *bad : {R"({"content_type":"image/png"})", R"({"size_bytes":1})", R"({"content_type":"image/svg+xml","size_bytes":1})",
                                R"({"content_type":"image/heic","size_bytes":1})", R"({"content_type":"image/png","size_bytes":0})",
                                R"({"content_type":"image/png","size_bytes":20000001})", R"({"content_type":"image/png","size_bytes":"1"})",
                                R"({"content_type":"image/png","size_bytes":1,"file_name":"a.png"})", R"({"content_type":"image/png","size_bytes":1,"key":"x"})"}) {
            CHECK_FALSE(M::parseUploadRequest(parseJson(bad), type, size, message));
        }
        CHECK_EQ(M::rejectionFor(10, 10, "image/png", "image/png", 4, 4), std::string());
        CHECK_EQ(M::rejectionFor(11, 10, "image/png", "image/png", 4, 4), std::string("size_mismatch"));
        CHECK_EQ(M::rejectionFor(10, 10, "image/gif", "image/png", 4, 4), std::string("not_image"));
        CHECK_EQ(M::rejectionFor(10, 10, "image/png", "image/jpeg", 4, 4), std::string("format_mismatch"));
        CHECK_EQ(M::rejectionFor(10, 10, "image/png", "image/png", 30001, 4), std::string("dimensions"));
        CHECK_EQ(M::rejectionFor(10, 10, "image/png", "image/png", 30000, 30000), std::string("dimensions"));
        CHECK(M::fingerprint("image/png", 1) != M::fingerprint("image/png", 2));
        CHECK(M::fingerprint("image/png", 1) != M::fingerprint("image/jpeg", 1));
        // A store outage is not one of these: it never reaches a decision.
        CHECK_EQ(usapp::errorCodeToString(M::errorCodeFor(M::Outcome::StoreUnavailable)), std::string("MEDIA_STORE_UNAVAILABLE"));
        CHECK_EQ(usapp::defaultStatusFor(M::errorCodeFor(M::Outcome::StoreUnavailable)), 503);
        CHECK_EQ(usapp::defaultStatusFor(M::errorCodeFor(M::Outcome::GrantExpired)), 409);
    }

    TEST_CASE("P2-6: generation status needs both flags, and every list page states it") {
        CHECK(RS::generationEnabled(true, true));
        CHECK_FALSE(RS::generationEnabled(true, false));
        CHECK_FALSE(RS::generationEnabled(false, true));
        CHECK_FALSE(RS::generationEnabled(false, false));
        RS::ListResult page;
        page.reminders.push_back(parseJson(R"({"target_id":"t1"})"));
        page.nextCursor = "c1";
        for (const bool enabled : {true, false}) {
            const auto body = RS::listBody(page, enabled);
            CHECK(body["generation_enabled"].isBool());
            CHECK_EQ(body["generation_enabled"].asBool(), enabled);
            CHECK_EQ(body["reminders"].size(), 1u);
            CHECK_EQ(body["next_cursor"].asString(), std::string("c1"));
            CHECK_EQ(body.size(), 3u);
        }
        const auto last = RS::listBody(RS::ListResult{}, false);
        CHECK(last["reminders"].isArray());
        CHECK(last["next_cursor"].isNull());
        CHECK(last["generation_enabled"].isBool());
    }

    TEST_CASE("P2-6: a task rule request has exactly one mode's fields, and disabling exactly one field") {
        RS::Terms t;
        CHECK(task(R"({"enabled":false})", t));
        CHECK_FALSE(t.enabled);
        CHECK(t.mode.empty());
        // A disabled rule carries no schedule at all.
        CHECK_FALSE(task(R"({"enabled":false,"mode":"before_deadline"})", t));
        CHECK_FALSE(task(R"({"enabled":false,"before_minutes":30})", t));
        CHECK_FALSE(task(R"({"enabled":false,"at_time":"22:00"})", t));

        CHECK(task(R"({"enabled":true,"mode":"before_deadline","before_minutes":30})", t));
        CHECK_EQ(*t.beforeMinutes, 30);
        CHECK(task(R"({"enabled":true,"mode":"before_deadline","before_minutes":1})", t));
        CHECK(task(R"({"enabled":true,"mode":"before_deadline","before_minutes":10080})", t));
        for (const char *bad : {R"({"enabled":true,"mode":"before_deadline","before_minutes":0})",
                                R"({"enabled":true,"mode":"before_deadline","before_minutes":10081})",
                                R"({"enabled":true,"mode":"before_deadline","before_minutes":30.0})",
                                R"({"enabled":true,"mode":"before_deadline","before_minutes":"30"})",
                                R"({"enabled":true,"mode":"before_deadline"})",
                                R"({"enabled":true,"mode":"before_deadline","before_minutes":30,"at_time":"22:00"})",
                                R"({"enabled":true,"mode":"local_time","at_time":"22:00"})",
                                R"({"enabled":true,"mode":"local_time","days_before":8,"at_time":"22:00"})",
                                R"({"enabled":true,"mode":"local_time","days_before":-1,"at_time":"22:00"})",
                                R"({"enabled":true,"mode":"local_time","days_before":0,"at_time":"24:00"})",
                                R"({"enabled":true,"mode":"local_time","days_before":0,"at_time":"22:00:00"})",
                                R"({"enabled":true,"mode":"local_time","days_before":0,"at_time":"2200"})",
                                R"({"enabled":true,"mode":"absolute","at_local_date":"2026-02-30","at_time":"09:00"})",
                                R"({"enabled":true,"mode":"absolute","at_local_date":"2026-02-03"})",
                                R"({"enabled":true,"mode":"anniversary_offsets","days_before":[1]})",
                                R"({"enabled":true})", R"({"enabled":"true"})", R"({"mode":"local_time"})", R"([])"}) {
            CHECK_FALSE(task(bad, t));
        }
        CHECK(task(R"({"enabled":true,"mode":"local_time","days_before":0,"at_time":"22:00"})", t));
        CHECK_EQ(t.atTime, std::string("22:00"));
        CHECK(task(R"({"enabled":true,"mode":"absolute","at_local_date":"2099-02-03","at_time":"09:00"})", t));
        CHECK_EQ(t.atLocalDate, std::string("2099-02-03"));
    }

    TEST_CASE("P2-6: anniversary offsets are a closed set, sorted and distinct, with a default time") {
        RS::Terms t;
        CHECK(anniversary(R"({"enabled":false})", t));
        CHECK_FALSE(anniversary(R"({"enabled":false,"days_before":[1]})", t));
        CHECK(anniversary(R"({"enabled":true,"days_before":[7,0,3]})", t));
        CHECK_EQ(t.atTime, std::string("09:00"));
        CHECK_EQ(RS::offsetsLiteral(t.offsets), std::string("{0,3,7}"));
        CHECK_EQ(t.mode, std::string("anniversary_offsets"));
        CHECK(anniversary(R"({"enabled":true,"days_before":[30],"at_time":"08:30"})", t));
        CHECK_EQ(t.atTime, std::string("08:30"));
        for (const char *bad : {R"({"enabled":true,"days_before":[]})", R"({"enabled":true,"days_before":[2]})",
                                R"({"enabled":true,"days_before":[1,1]})", R"({"enabled":true,"days_before":[0,1,3,7]})",
                                R"({"enabled":true,"days_before":[1.0]})", R"({"enabled":true,"days_before":1})",
                                R"({"enabled":true})", R"({"enabled":true,"days_before":[1],"at_time":"9:00"})",
                                R"({"enabled":true,"days_before":[1],"mode":"local_time"})"}) {
            CHECK_FALSE(anniversary(bad, t));
        }
    }

    TEST_CASE("P2-6: a fingerprint covers the target and the normalized terms") {
        RS::Terms a, b;
        CHECK(anniversary(R"({"enabled":true,"days_before":[7,1]})", a));
        CHECK(anniversary(R"({"enabled":true,"days_before":[1,7],"at_time":"09:00"})", b));
        const auto kind = RS::TargetKind::Anniversary;
        const std::string id(36, 'a');
        // Spelling differences of one choice are the same request.
        CHECK_EQ(RS::fingerprint(kind, id, a), RS::fingerprint(kind, id, b));
        CHECK(RS::fingerprint(kind, id, a) != RS::fingerprint(kind, std::string(36, 'b'), a));
        CHECK(RS::fingerprint(kind, id, a) != RS::fingerprint(RS::TargetKind::Task, id, a));
        b.atTime = "09:01";
        CHECK(RS::fingerprint(kind, id, a) != RS::fingerprint(kind, id, b));
        RS::Terms off;
        CHECK(RS::fingerprint(kind, id, a) != RS::fingerprint(kind, id, off));
    }

    TEST_CASE("P2-6: outcomes map to the contract's codes and statuses") {
        using O = RS::Outcome;
        const auto code = [](O o) { return std::string(usapp::errorCodeToString(RS::errorCodeFor(o))); };
        CHECK_EQ(code(O::NotAssignee), std::string("REMINDER_NOT_ASSIGNEE"));
        CHECK_EQ(code(O::TargetClosed), std::string("REMINDER_TARGET_CLOSED"));
        CHECK_EQ(code(O::TimeInvalid), std::string("REMINDER_TIME_INVALID"));
        CHECK_EQ(code(O::TimePassed), std::string("REMINDER_TIME_PASSED"));
        CHECK_EQ(code(O::KeyReused), std::string("IDEMPOTENCY_KEY_REUSED"));
        CHECK_EQ(RS::httpStatusFor(O::Created), 201);
        CHECK_EQ(RS::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(RS::httpStatusFor(O::NotAssignee), 403);
        CHECK_EQ(RS::httpStatusFor(O::TimeInvalid), 400);
        CHECK_EQ(RS::httpStatusFor(O::TimePassed), 409);
        CHECK_EQ(RS::httpStatusFor(O::TargetClosed), 409);
        CHECK_EQ(RS::httpStatusFor(O::ResultUnknown), 503);
    }
}

}  // namespace usapptest::reminderunit
