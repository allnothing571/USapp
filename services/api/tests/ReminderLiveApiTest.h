#pragma once

// P2-6 reminder availability: GET /api/v1/reminders from the real API binary, started by the isolated verification with
// one REMINDERS_ENABLED and SCHEDULER_ENABLED combination, through its real AuthFilter with a real session cookie. Runs
// only when USAPP_TEST_REMINDER_API_URL is set, against literal usapp_test, and is not part of the CTest run. The token
// is per run and never printed.
#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "ReminderDbTests.h"
#include "core/Crypto.h"
#include "services/AdminAuthService.h"

namespace usapptest::reminderliveapi {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace r = usapptest::reminder;

struct Reply {
    long status = 0;
    std::string body;
    std::string headers;
};
inline size_t collect(char *data, size_t size, size_t count, void *out) {
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}
inline Reply call(const std::string &url, const std::string &token, const std::vector<std::string> &headers,
                  const std::string *postBody, const char *method = nullptr) {
    Reply reply;
    CURL *curl = curl_easy_init();
    if (!curl) throw std::runtime_error("no curl handle");
    curl_slist *list = nullptr;
    if (!token.empty()) list = curl_slist_append(list, ("Cookie: __Host-usapp-session=" + token).c_str());
    for (const auto &header : headers) list = curl_slist_append(list, header.c_str());
    if (postBody) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
    if (method) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &reply.headers);
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return reply;
}
inline Reply get(const std::string &url, const std::string &token = "") { return call(url, token, {}, nullptr); }
inline Json::Value parsed(const Reply &reply) {
    Json::Value out;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    reader->parse(reply.body.data(), reply.body.data() + reply.body.size(), &out, &errors);
    return out;
}
// A formal session for this user. Only its hash is stored, as the service does.
inline std::string commitToken(const c::ClientPtr &pool, const std::string &user, const std::string &csrf = usapp::crypto::randomHex(32)) {
    const std::string token = usapp::crypto::randomHex(32);
    pool->execSqlSync(
        "INSERT INTO sessions (token_hash, csrf_token_hash, user_id, is_authenticated, user_agent_family, expires_at) "
        "VALUES ($1, $2, $3::uuid, true, 'chrome', now() + interval '1 hour')",
        usapp::crypto::sha256Hex(token), usapp::crypto::sha256Hex(csrf), user);
    return token;
}

inline void run(const c::ClientPtr &pool, const std::string &dsn, const std::string &base, bool expected) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("P2-6: the real API states generation status on every authenticated page, and nowhere else") {
        try {
            // Bounded: the harness's own deadline stands over this.
            const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (get(base + "/api/v1/health").status != 200) {
                if (std::chrono::steady_clock::now() >= ready) throw std::runtime_error("API not ready");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto first = r::publishOneOff(s.userA, observer, 5);
            const auto second = r::publishOneOff(s.userA, observer, 6);
            CHECK(r::is(r::setTask(s.userB, first, r::before(30)), r::RO::Created));
            CHECK(r::is(r::setTask(s.userB, second, r::before(30)), r::RO::Created));
            const auto token = commitToken(pool, s.userB);
            const std::string url = base + "/api/v1/reminders";

            // Anonymous and invalid requests learn nothing about the configuration.
            for (const auto &refused : {get(url), get(url, usapp::crypto::randomHex(32))}) {
                CHECK_EQ(refused.status, 401L);
                CHECK(refused.body.find("generation_enabled") == std::string::npos);
            }
            const auto badLimit = get(url + "?limit=0", token);
            CHECK_EQ(badLimit.status, 400L);
            CHECK(badLimit.body.find("generation_enabled") == std::string::npos);

            // Both pages state it, and pagination is unchanged.
            const auto pageOne = get(url + "?limit=1", token);
            CHECK_EQ(pageOne.status, 200L);
            CHECK(pageOne.headers.find("no-store") != std::string::npos);
            const auto one = parsed(pageOne);
            CHECK(one["generation_enabled"].isBool());
            CHECK_EQ(one["generation_enabled"].asBool(), expected);
            CHECK_EQ(one["reminders"].size(), 1u);
            CHECK(one["next_cursor"].isString());
            // A computed instant stays in the response either way: the field is what qualifies it.
            CHECK(one["reminders"][0]["next_fire_at"].isString());
            CHECK(!one["reminders"][0].isMember("generation_enabled"));
            const auto two = parsed(get(url + "?limit=1&cursor=" + one["next_cursor"].asString(), token));
            CHECK(two["generation_enabled"].isBool());
            CHECK_EQ(two["generation_enabled"].asBool(), expected);
            CHECK_EQ(two["reminders"].size(), 1u);
            CHECK(two["next_cursor"].isNull());
            CHECK(two["reminders"][0]["target_id"].asString() != one["reminders"][0]["target_id"].asString());

            // Another member's session: the same status, and none of this member's rules.
            const auto partner = parsed(get(url, commitToken(pool, s.userA)));
            CHECK_EQ(partner["generation_enabled"].asBool(), expected);
            CHECK_EQ(partner["reminders"].size(), 0u);
            // Reading wrote nothing: still one revision per target.
            CHECK_EQ(r::rules(observer, first), 1);

            // P3-1a: no real process has a store. Media routes exist, refuse safely, and expose nothing.
            const std::string csrf = usapp::crypto::randomHex(32);
            const auto mediaToken = commitToken(pool, s.userB, csrf);
            {
                const auto room = base + "/api/v1/shared-room";
                const auto share = room + "/avatar-sharing";
                const auto memberA = commitToken(pool, s.userA);
                const auto headers = std::vector<std::string>{"Origin: https://localhost:8443",
                    "X-CSRF-Token: " + csrf, "Content-Type: application/json"};
                const std::string draft = R"({"expected_version":0,"layout":[{"item":"lamp","x":1,"y":0}]})";
                const std::string yes = R"({"share_avatar":true})";
                const std::string no = R"({"share_avatar":false})";
                CHECK_EQ(get(room).status, 401L);
                CHECK_EQ(parsed(get(room, memberA))["version"].asInt(), 0);
                CHECK(parsed(get(room, memberA))["partner_appearance"].isNull());
                CHECK_EQ(call(room, mediaToken, {}, &draft, "PUT").status, 403L);
                CHECK_EQ(call(room, mediaToken, headers, &draft, "PUT").status, 200L);
                const auto firstLayout = parsed(get(room, memberA));
                CHECK_EQ(firstLayout["version"].asInt(), 1);
                CHECK_EQ(firstLayout["layout"][0]["item"].asString(), "lamp");
                const auto conflict = call(room, mediaToken, headers, &draft, "PUT");
                CHECK_EQ(conflict.status, 409L);
                CHECK_EQ(parsed(conflict)["error"]["code"].asString(), "SHARED_ROOM_VERSION_CONFLICT");
                CHECK_EQ(call(share, mediaToken, headers, &yes, "PUT").status, 200L);
                const auto exposed = parsed(get(room, memberA));
                CHECK(exposed["partner_appearance"].isObject());
                CHECK_FALSE(exposed.isMember("partner_id"));
                CHECK_EQ(call(share, mediaToken, headers, &no, "PUT").status, 200L);
                CHECK(parsed(get(room, memberA))["partner_appearance"].isNull());
                ts::CommittedSpace other;
                ts::commitSpace(pool, other, true);
                const auto otherRoom = parsed(get(room, commitToken(pool, other.userA)));
                CHECK_EQ(otherRoom["version"].asInt(), 0);
                CHECK(otherRoom["layout"].empty());
                CHECK(otherRoom["partner_appearance"].isNull());

                const auto garden = base + "/api/v1/shared-garden";
                const auto water = garden + "/watering";
                CHECK_EQ(get(garden).status, 401L);
                const auto initial = parsed(get(garden, memberA));
                const auto today = initial["watering_date"].asString();
                CHECK_EQ(today.size(), 10u);
                CHECK_EQ(initial["total_waterings"].asInt(), 0);
                CHECK_EQ(initial["recent_waterings"].size(), 7u);
                CHECK_EQ(initial["recent_waterings"][0]["watering_date"].asString(), today);
                CHECK_FALSE(initial["recent_waterings"][0]["watered"].asBool());
                const std::string command = R"({"watering_date":")" + today + R"("})";
                const std::string stale = R"({"watering_date":"2000-01-01"})";
                CHECK_EQ(call(water, mediaToken, {}, &command, "PUT").status, 403L);
                const auto wrongDay = call(water, mediaToken, headers, &stale, "PUT");
                CHECK_EQ(wrongDay.status, 409L);
                CHECK_EQ(parsed(wrongDay)["error"]["code"].asString(), "SHARED_GARDEN_DATE_CHANGED");
                auto first = std::async(std::launch::async, [&]() { return call(water, mediaToken, headers, &command, "PUT"); });
                const auto second = call(water, mediaToken, headers, &command, "PUT");
                CHECK_EQ(first.get().status, 200L);
                CHECK_EQ(second.status, 200L);
                CHECK_EQ(parsed(second)["watering_date"].asString(), today);
                const auto partnerGarden = parsed(get(garden, memberA));
                CHECK_EQ(partnerGarden["total_waterings"].asInt(), 1);
                CHECK_FALSE(partnerGarden["watered_today"].asBool());
                CHECK(partnerGarden["partner_watered_today"].asBool());
                CHECK_FALSE(partnerGarden["recent_waterings"][0]["watered"].asBool());
                CHECK(partnerGarden["recent_waterings"][0]["partner_watered"].asBool());
                const auto gardenHistory = garden + "/history";
                CHECK_EQ(get(gardenHistory).status, 401L);
                CHECK_EQ(get(gardenHistory + "?month=2000-13", memberA).status, 400L);
                CHECK_EQ(get(gardenHistory + "?month=9999-01", memberA).status, 400L);
                const auto month = parsed(get(gardenHistory, memberA));
                CHECK_EQ(month["month"].asString(), today.substr(0, 7));
                CHECK_EQ(month["days"].size(), static_cast<unsigned>(std::stoi(today.substr(8))));
                auto lastDay = month["days"][month["days"].size() - 1];
                CHECK_EQ(lastDay["watering_date"].asString(), today);
                CHECK(lastDay["partner_watered"].asBool());
                CHECK_FALSE(lastDay["watered"].asBool());
                const auto leapMonth = parsed(get(gardenHistory + "?month=2000-02", memberA));
                CHECK_EQ(leapMonth["days"].size(), 29u);
                const auto gardenCsrf = usapp::crypto::randomHex(32);
                const auto gardenMember = commitToken(pool, s.userA, gardenCsrf);
                CHECK_EQ(call(water, gardenMember,
                              {"Origin: https://localhost:8443", "X-CSRF-Token: " + gardenCsrf, "Content-Type: application/json"},
                              &command, "PUT").status, 200L);
                const auto shared = parsed(get(garden, mediaToken));
                CHECK_EQ(shared["total_waterings"].asInt(), 2);
                CHECK(shared["watered_today"].asBool());
                CHECK(shared["partner_watered_today"].asBool());
                CHECK_EQ(parsed(get(garden, commitToken(pool, other.userA)))["total_waterings"].asInt(), 0);
            }
            const std::string uploads = base + "/api/v1/media/uploads";
            const std::string body = R"({"content_type":"image/png","size_bytes":1000})";
            const std::string unknown = base + "/api/v1/media/" + c::newUuid();
            const std::vector<std::string> writeHeaders = {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf,
                                                           "Content-Type: application/json", "Idempotency-Key: " + c::newUuid()};
            const std::string empty;
            CHECK_EQ(call(uploads, mediaToken, {"Content-Type: application/json"}, &body).status, 403L);
            CHECK_EQ(call(uploads, "", writeHeaders, &body).status, 401L);
            const auto disabled = call(uploads, mediaToken, writeHeaders, &body);
            CHECK_EQ(disabled.status, 503L);
            CHECK(disabled.body.find("MEDIA_DISABLED") != std::string::npos);
            CHECK_EQ(call(uploads + "/" + c::newUuid() + "/complete", mediaToken, writeHeaders, &empty).status, 503L);
            CHECK_EQ(get(unknown, mediaToken).status, 404L);
            CHECK_EQ(get(unknown).status, 401L);
            CHECK_EQ(get(unknown + "/content", mediaToken).status, 503L);
            CHECK_EQ(get(unknown + "/content?variant=raw", mediaToken).status, 400L);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM media_objects WHERE uploader_id = $1::uuid", s.userB), 0);

            // D6: deletion and undo through the real filter and controller. Neither needs a store, so a real process serves them.
            {
                const auto writeWithKey = [&](const std::string &key) {
                    return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Idempotency-Key: " + key};
                };
                const std::string absentDeletions = unknown + "/deletions";
                CHECK_EQ(call(absentDeletions, "", writeWithKey(c::newUuid()), &empty).status, 401L);
                CHECK_EQ(call(absentDeletions, mediaToken, {"Origin: https://localhost:8443"}, &empty).status, 403L);
                CHECK_EQ(call(absentDeletions, mediaToken, {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf}, &empty).status, 400L);
                const auto absent = call(absentDeletions, mediaToken, writeWithKey(c::newUuid()), &empty);
                CHECK_EQ(absent.status, 404L);
                CHECK(absent.body.find("MEDIA_NOT_FOUND") != std::string::npos);
                // A READY photo of this member's, committed directly: no real process has a store to make one.
                const std::string photo = c::newUuid();
                pool->execSqlSync(
                    "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '1 day', $4, repeat('a', 64), clock_timestamp() - interval '1 day 00:10:00')",
                    photo, s.couple, s.userB, c::newUuid());
                pool->execSqlSync(
                    "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
                    "VALUES ($1::uuid, 1, $2::uuid, 'media/' || $2 || '/' || $1 || '/' || gen_random_uuid() || '/original', clock_timestamp() - interval '1 day')",
                    photo, s.couple);
                pool->execSqlSync(
                    "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
                    "VALUES ($1::uuid, $2::uuid, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '23 hours')",
                    photo, s.couple);
                const std::string photoUrl = base + "/api/v1/media/" + photo;
                CHECK_EQ(get(photoUrl, mediaToken).status, 200L);
                const std::string csrfA = usapp::crypto::randomHex(32);
                const auto partnerToken = commitToken(pool, s.userA, csrfA);
                const auto partnerHeaders = std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrfA, "Idempotency-Key: " + c::newUuid()};
                CHECK_EQ(call(photoUrl + "/deletions", partnerToken, partnerHeaders, &empty).status, 404L);
                const std::string deleteKey = c::newUuid();
                const auto deleted = call(photoUrl + "/deletions", mediaToken, writeWithKey(deleteKey), &empty);
                CHECK_EQ(deleted.status, 201L);
                CHECK(deleted.headers.find("no-store") != std::string::npos);
                const auto cycle = parsed(deleted)["deletion"];
                CHECK_EQ(cycle["state"].asString(), std::string("deleted"));
                CHECK(deleted.body.find("media/") == std::string::npos);
                const auto again = call(photoUrl + "/deletions", mediaToken, writeWithKey(deleteKey), &empty);
                CHECK_EQ(again.status, 200L);
                CHECK_EQ(parsed(again)["deletion"]["purge_after"].asString(), cycle["purge_after"].asString());
                const auto twice = call(photoUrl + "/deletions", mediaToken, writeWithKey(c::newUuid()), &empty);
                CHECK_EQ(twice.status, 409L);
                CHECK(twice.body.find("MEDIA_ALREADY_DELETED") != std::string::npos);
                CHECK_EQ(get(photoUrl, mediaToken).status, 404L);
                const std::string undoUrl = photoUrl + "/deletions/" + cycle["deletion_id"].asString() + "/undo";
                CHECK_EQ(call(undoUrl, partnerToken, partnerHeaders, &empty).status, 404L);
                const auto undone = call(undoUrl, mediaToken, writeWithKey(c::newUuid()), &empty);
                CHECK_EQ(undone.status, 200L);
                CHECK_EQ(parsed(undone)["deletion"]["state"].asString(), std::string("undone"));
                CHECK_EQ(get(photoUrl, mediaToken).status, 200L);
                const auto stale = call(undoUrl, mediaToken, writeWithKey(c::newUuid()), &empty);
                CHECK_EQ(stale.status, 409L);
                CHECK(stale.body.find("MEDIA_DELETION_NOT_ACTIVE") != std::string::npos);

                // The undoable list: its literal path is not taken for a media id. A new session of the same account finds the deletion.
                const std::string listUrl = base + "/api/v1/media/deletions";
                CHECK_EQ(get(listUrl).status, 401L);
                CHECK_EQ(parsed(get(listUrl, mediaToken))["deletions"].size(), 0u);
                const auto again2 = call(photoUrl + "/deletions", mediaToken, writeWithKey(c::newUuid()), &empty);
                CHECK_EQ(again2.status, 201L);
                const auto openCycle = parsed(again2)["deletion"]["deletion_id"].asString();
                CHECK(openCycle != cycle["deletion_id"].asString());
                const auto freshToken = commitToken(pool, s.userB);
                const auto found = get(listUrl + "?limit=1", freshToken);
                CHECK_EQ(found.status, 200L);
                CHECK(found.headers.find("no-store") != std::string::npos);
                const auto foundBody = parsed(found);
                CHECK_EQ(foundBody["deletions"].size(), 1u);
                CHECK(foundBody["next_cursor"].isNull());
                CHECK_EQ(foundBody["deletions"][0]["deletion_id"].asString(), openCycle);
                CHECK_EQ(foundBody["deletions"][0]["media_id"].asString(), photo);
                CHECK_EQ(foundBody["deletions"][0].size(), 6u);
                CHECK(foundBody["deletions"][0]["uploaded_at"].isString());
                // Unbound, so generic: no parent is named, on the real route.
                const auto liveContext = foundBody["deletions"][0]["context"];
                CHECK_EQ(liveContext["kind"].asString(), std::string("none"));
                CHECK(liveContext["relation"].isNull());
                CHECK(liveContext["target_id"].isNull());
                CHECK(liveContext["label"].isNull());
                CHECK(liveContext["position"].isNull());
                CHECK(found.body.find("media/") == std::string::npos);
                CHECK(found.body.find("staging") == std::string::npos);
                // The partner's session lists nothing; bad paging refuses.
                CHECK_EQ(parsed(get(listUrl, partnerToken))["deletions"].size(), 0u);
                CHECK_EQ(get(listUrl + "?limit=0", freshToken).status, 400L);
                CHECK_EQ(get(listUrl + "?cursor=nonsense", freshToken).status, 400L);
                // What the list named, the new session can undo with its own token.
                const std::string freshCsrf = usapp::crypto::randomHex(32);
                const auto undoToken = commitToken(pool, s.userB, freshCsrf);
                const auto undoneNow = call(photoUrl + "/deletions/" + openCycle + "/undo", undoToken,
                                            {"Origin: https://localhost:8443", "X-CSRF-Token: " + freshCsrf, "Idempotency-Key: " + c::newUuid()}, &empty);
                CHECK_EQ(undoneNow.status, 200L);
                CHECK_EQ(parsed(get(listUrl, freshToken))["deletions"].size(), 0u);
            }

            // P3-1b: memory routes, through the real filter and controller. No store is needed for any of these answers.
            const std::string memories = base + "/api/v1/memories";
            const std::string memoryBody = R"({"date":"2026-03-01","title":"t","media_ids":[")" + c::newUuid() + R"("]})";
            const std::string missing = memories + "/" + c::newUuid();
            CHECK_EQ(get(memories).status, 401L);
            CHECK_EQ(call(memories, "", writeHeaders, &memoryBody).status, 401L);
            CHECK_EQ(call(memories, mediaToken, {"Content-Type: application/json"}, &memoryBody).status, 403L);
            const std::vector<std::string> noKey = {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json"};
            CHECK_EQ(call(memories, mediaToken, noKey, &memoryBody).status, 400L);
            const auto unavailable = call(memories, mediaToken, writeHeaders, &memoryBody);
            CHECK_EQ(unavailable.status, 409L);
            CHECK(unavailable.body.find("MEMORY_PHOTO_UNAVAILABLE") != std::string::npos);
            const auto listed = get(memories, mediaToken);
            CHECK_EQ(listed.status, 200L);
            CHECK(listed.body.find("\"next_cursor\"") != std::string::npos);
            CHECK(listed.body.find("\"uploads_enabled\":false") != std::string::npos);
            CHECK_EQ(get(memories + "?cursor=x", mediaToken).status, 400L);
            const auto absent = get(missing, mediaToken);
            CHECK_EQ(absent.status, 404L);
            CHECK(absent.body.find("MEMORY_NOT_FOUND") != std::string::npos);
            CHECK_EQ(get(memories + "/not-a-uuid", mediaToken).status, 404L);
            CHECK_EQ(get(missing + "/proposals/" + c::newUuid(), mediaToken).status, 404L);
            const std::string decision = R"({"outcome":"approve"})";
            const auto noMemory = call(missing + "/proposals/" + c::newUuid() + "/decision", mediaToken, writeHeaders, &decision);
            CHECK_EQ(noMemory.status, 404L);
            CHECK(noMemory.body.find("MEMORY_NOT_FOUND") != std::string::npos);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM memories WHERE creator_id = $1::uuid", s.userB), 0);

            // D6 M1a: memory deletion, its recovery list and the plan detail, through the real filter, routes and controllers. Neither needs a store.
            {
                const auto deleteHeaders = [&](const std::string &key) {
                    return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Idempotency-Key: " + key};
                };
                const std::string absentDeletions = missing + "/deletions";
                CHECK_EQ(call(absentDeletions, "", deleteHeaders(c::newUuid()), &empty).status, 401L);
                CHECK_EQ(call(absentDeletions, mediaToken, {"Origin: https://localhost:8443"}, &empty).status, 403L);
                CHECK_EQ(call(absentDeletions, mediaToken, {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf}, &empty).status, 400L);
                CHECK(call(absentDeletions, mediaToken, deleteHeaders(c::newUuid()), &empty).body.find("MEMORY_NOT_FOUND") != std::string::npos);
                // Its literal path is not taken for a memory id.
                const std::string recovery = memories + "/deletions";
                CHECK_EQ(get(recovery).status, 401L);
                const auto emptyRecovery = get(recovery, mediaToken);
                CHECK_EQ(emptyRecovery.status, 200L);
                CHECK(emptyRecovery.headers.find("no-store") != std::string::npos);
                CHECK_EQ(parsed(emptyRecovery)["deletions"].size(), 0u);
                CHECK_EQ(get(recovery + "?limit=0", mediaToken).status, 400L);
                CHECK_EQ(get(recovery + "?cursor=nonsense", mediaToken).status, 400L);

                // A memory by this member, linked from a finished plan. Committed directly: no real process has a store to make its photo.
                const std::string photo = c::newUuid();
                const std::string memory = c::newUuid();
                pool->execSqlSync(
                    "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '1 day', $4, repeat('a', 64), clock_timestamp() - interval '1 day 00:10:00')",
                    photo, s.couple, s.userB, c::newUuid());
                pool->execSqlSync(
                    "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
                    "VALUES ($1::uuid, 1, $2::uuid, 'media/' || $2 || '/' || $1 || '/' || gen_random_uuid() || '/original', clock_timestamp() - interval '1 day')",
                    photo, s.couple);
                pool->execSqlSync(
                    "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
                    "VALUES ($1::uuid, $2::uuid, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '23 hours')",
                    photo, s.couple);
                pool->execSqlSync(
                    "DO $live$ DECLARE t uuid; BEGIN "
                    "INSERT INTO memories (id, couple_id, creator_id) VALUES ('" + memory + "', '" + s.couple + "', '" + s.userB + "'); "
                    "INSERT INTO memory_texts (couple_id, title, description) VALUES ('" + s.couple + "', 'LIVE-M1A-TITLE', 'LIVE-M1A-WORDS') RETURNING id INTO t; "
                    "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint) "
                    "VALUES ('" + memory + "', '" + s.couple + "', 1, '" + s.userB + "', 'create', DATE '2026-01-01', t, '" + c::newUuid() + "', repeat('a', 64)); "
                    "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) VALUES ('" + photo + "', '" + s.couple + "', 'memory', '" + memory + "', '" + s.userB + "'); "
                    "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) VALUES ('" + memory + "', 1, 1, '" + photo + "', '" + s.couple + "'); "
                    "END $live$");
                const auto plan = pool->execSqlSync(
                    "SELECT usapp_shared_plan_write($1::uuid, NULL::uuid, 'create', $2, $3::jsonb) #>> '{plan,id}' AS id", s.userB, c::newUuid(),
                    std::string(R"({"body":{"title":"LIVE-PLAN-TITLE","date":null,"time":null,"place":null,"note":"LIVE-PLAN-NOTE"}})"))[0]["id"].as<std::string>();
                for (const char *from : {"want", "plan"}) {
                    pool->execSqlSync("SELECT usapp_shared_plan_write($1::uuid, $2::uuid, 'advance', $3, $4::jsonb)", s.userB, plan, c::newUuid(),
                                      std::string(R"({"expected_status":")") + from + R"("})");
                }
                pool->execSqlSync("SELECT usapp_plan_memory_attach($1::uuid, $2::uuid, $3, $4::uuid)", s.userB, plan, c::newUuid(), memory);
                const std::string planUrl = base + "/api/v1/shared-plans/" + plan;
                const auto planBefore = parsed(get(planUrl, mediaToken));
                CHECK_EQ(planBefore["memory_id"].asString(), memory);
                CHECK(!planBefore["memory_unavailable"].asBool());
                CHECK(planBefore["memory_unavailable"].isBool());

                const std::string memoryUrl = memories + "/" + memory;
                const std::string csrfPartner = usapp::crypto::randomHex(32);
                const auto partnerSession = commitToken(pool, s.userA, csrfPartner);
                const std::vector<std::string> partnerHeaders = {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrfPartner, "Idempotency-Key: " + c::newUuid()};
                const auto forbidden = call(memoryUrl + "/deletions", partnerSession, partnerHeaders, &empty);
                CHECK_EQ(forbidden.status, 403L);
                CHECK(forbidden.body.find("MEMORY_EDIT_FORBIDDEN") != std::string::npos);

                // Edit-request history: the GET beside the POST on the same path, through the real filter and controller.
                const std::string historyUrl = memoryUrl + "/proposals";
                CHECK_EQ(get(historyUrl).status, 401L);
                for (const auto &session : {mediaToken, partnerSession}) {
                    const auto history = get(historyUrl, session);
                    CHECK_EQ(history.status, 200L);
                    CHECK(history.headers.find("no-store") != std::string::npos);
                    const auto listedHistory = parsed(history);
                    CHECK(listedHistory["proposals"].isArray());
                    CHECK_EQ(listedHistory["proposals"].size(), 0u);
                    CHECK(listedHistory["next_cursor"].isNull());
                    CHECK_EQ(listedHistory["current_version"].asInt(), 1);
                    CHECK(history.body.find("LIVE-M1A") == std::string::npos);
                }
                CHECK_EQ(get(historyUrl + "?limit=0", mediaToken).status, 400L);
                CHECK_EQ(get(historyUrl + "?limit=101", mediaToken).status, 400L);
                CHECK_EQ(get(historyUrl + "?cursor=nonsense", mediaToken).status, 400L);
                const auto historyAbsent = get(missing + "/proposals", mediaToken);
                CHECK_EQ(historyAbsent.status, 404L);
                CHECK(historyAbsent.body.find("MEMORY_NOT_FOUND") != std::string::npos);
                CHECK(get(memories + "/not-a-uuid/proposals", mediaToken).body.find("MEMORY_NOT_FOUND") != std::string::npos);
                ts::CommittedSpace stranger;
                ts::commitSpace(pool, stranger, true);
                const auto strangerHistory = get(historyUrl, commitToken(pool, stranger.userA));
                CHECK_EQ(strangerHistory.status, 404L);
                CHECK(strangerHistory.body.find("MEMORY_NOT_FOUND") != std::string::npos);

                const std::string deleteKey = c::newUuid();
                const auto deleted = call(memoryUrl + "/deletions", mediaToken, deleteHeaders(deleteKey), &empty);
                CHECK_EQ(deleted.status, 201L);
                CHECK(deleted.headers.find("no-store") != std::string::npos);
                CHECK(deleted.body.find("LIVE-M1A") == std::string::npos);
                CHECK(deleted.body.find(photo) == std::string::npos);
                const auto cycle = parsed(deleted)["deletion"];
                CHECK_EQ(cycle["state"].asString(), std::string("deleted"));
                CHECK_EQ(cycle.size(), 7u);
                const auto cycleId = cycle["deletion_id"].asString();
                CHECK_EQ(call(memoryUrl + "/deletions", mediaToken, deleteHeaders(deleteKey), &empty).status, 200L);
                const auto again = call(memoryUrl + "/deletions", mediaToken, deleteHeaders(c::newUuid()), &empty);
                CHECK_EQ(again.status, 409L);
                CHECK(again.body.find("MEMORY_ALREADY_DELETED") != std::string::npos);

                // Hidden from both on every route; the plan says so, keeps its own text, and no longer names the memory.
                for (const auto &session : {mediaToken, partnerSession}) {
                    const auto hidden = get(memoryUrl, session);
                    CHECK_EQ(hidden.status, 404L);
                    const auto hiddenHistory = get(historyUrl, session);
                    CHECK_EQ(hiddenHistory.status, 404L);
                    CHECK(hiddenHistory.body.find("MEMORY_NOT_FOUND") != std::string::npos);
                    CHECK(hidden.body.find("LIVE-M1A") == std::string::npos);
                    CHECK(get(memories, session).body.find(memory) == std::string::npos);
                    const auto planNow = get(planUrl, session);
                    CHECK_EQ(planNow.status, 200L);
                    CHECK(planNow.body.find(memory) == std::string::npos);
                    CHECK(planNow.body.find("LIVE-M1A") == std::string::npos);
                    const auto planBody = parsed(planNow);
                    CHECK(planBody["memory_id"].isNull());
                    CHECK(planBody["memory_unavailable"].asBool());
                    Json::Value mine = planBody, before = planBefore;
                    mine.removeMember("memory_id");
                    mine.removeMember("memory_unavailable");
                    before.removeMember("memory_id");
                    before.removeMember("memory_unavailable");
                    if (session == mediaToken) CHECK(mine == before);
                }

                const auto listedNow = get(recovery + "?limit=1", mediaToken);
                CHECK_EQ(listedNow.status, 200L);
                const auto listedBody = parsed(listedNow);
                CHECK_EQ(listedBody["deletions"].size(), 1u);
                CHECK_EQ(listedBody["deletions"][0]["deletion_id"].asString(), cycleId);
                CHECK_EQ(listedBody["deletions"][0]["title"].asString(), std::string("LIVE-M1A-TITLE"));
                CHECK_EQ(listedBody["deletions"][0].size(), 5u);
                CHECK(listedNow.body.find("LIVE-M1A-WORDS") == std::string::npos);
                CHECK_EQ(parsed(get(recovery, partnerSession))["deletions"].size(), 0u);

                const std::string undoUrl = memoryUrl + "/deletions/" + cycleId + "/undo";
                std::vector<std::string> partnerUndo = partnerHeaders;
                partnerUndo.back() = "Idempotency-Key: " + c::newUuid();
                CHECK_EQ(call(undoUrl, partnerSession, partnerUndo, &empty).status, 404L);
                const std::string undoKey = c::newUuid();
                const auto undone = call(undoUrl, mediaToken, deleteHeaders(undoKey), &empty);
                CHECK_EQ(undone.status, 200L);
                CHECK_EQ(parsed(undone)["deletion"]["state"].asString(), std::string("undone"));
                CHECK(undone.body.find("LIVE-M1A") == std::string::npos);
                CHECK_EQ(call(undoUrl, mediaToken, deleteHeaders(undoKey), &empty).status, 200L);
                const auto stale = call(undoUrl, mediaToken, deleteHeaders(c::newUuid()), &empty);
                CHECK_EQ(stale.status, 409L);
                CHECK(stale.body.find("MEMORY_DELETION_NOT_ACTIVE") != std::string::npos);
                CHECK_EQ(get(memoryUrl, partnerSession).status, 200L);
                CHECK_EQ(get(historyUrl, partnerSession).status, 200L);
                const auto planRestored = parsed(get(planUrl, mediaToken));
                CHECK_EQ(planRestored["memory_id"].asString(), memory);
                CHECK(!planRestored["memory_unavailable"].asBool());
                CHECK(planRestored == planBefore);
                CHECK_EQ(parsed(get(recovery, mediaToken))["deletions"].size(), 0u);

                // D6 M1c: the retained list's literal path, and the linked cycle's refusals, on the real routes.
                const std::string retainedUrl = base + "/api/v1/media/retained";
                CHECK_EQ(get(retainedUrl).status, 401L);
                const auto noneRetained = get(retainedUrl, mediaToken);
                CHECK_EQ(noneRetained.status, 200L);
                CHECK(noneRetained.headers.find("no-store") != std::string::npos);
                CHECK_EQ(parsed(noneRetained)["photos"].size(), 0u);
                CHECK(parsed(noneRetained)["next_cursor"].isNull());
                CHECK_EQ(get(retainedUrl + "?limit=0", mediaToken).status, 400L);
                CHECK_EQ(get(retainedUrl + "?cursor=nonsense", mediaToken).status, 400L);
                const auto again3 = call(memoryUrl + "/deletions", mediaToken, deleteHeaders(c::newUuid()), &empty);
                CHECK_EQ(again3.status, 201L);
                const auto linkedMemoryCycle = parsed(again3)["deletion"]["deletion_id"].asString();
                const std::string livePhotoUrl = base + "/api/v1/media/" + photo;
                CHECK_EQ(get(livePhotoUrl, mediaToken).status, 404L);
                const auto linkedCycle = pool->execSqlSync(
                    "SELECT media_deletion_id::text AS id FROM memory_deletion_photo_cycles WHERE memory_deletion_id = $1::uuid", linkedMemoryCycle)[0]["id"].as<std::string>();
                const auto standalone = call(livePhotoUrl + "/deletions/" + linkedCycle + "/undo", mediaToken, deleteHeaders(c::newUuid()), &empty);
                CHECK_EQ(standalone.status, 409L);
                CHECK(standalone.body.find("MEDIA_DELETION_LINKED") != std::string::npos);
                CHECK_EQ(call(memoryUrl + "/deletions/" + linkedMemoryCycle + "/undo", mediaToken, deleteHeaders(c::newUuid()), &empty).status, 200L);
                CHECK_EQ(get(livePhotoUrl, mediaToken).status, 200L);

                // D6 M1b: delete requests, through the real filter, routes and controller.
                const std::string requestsUrl = memoryUrl + "/delete-requests";
                const auto partnerWrite = [&](const std::string &key) {
                    return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrfPartner, "Content-Type: application/json", "Idempotency-Key: " + key};
                };
                const auto creatorWrite = [&](const std::string &key) {
                    return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json", "Idempotency-Key: " + key};
                };
                CHECK_EQ(call(requestsUrl, "", partnerWrite(c::newUuid()), &empty).status, 401L);
                CHECK_EQ(call(requestsUrl, partnerSession, {"Origin: https://localhost:8443"}, &empty).status, 403L);
                CHECK_EQ(call(requestsUrl, partnerSession, {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrfPartner}, &empty).status, 400L);
                CHECK(call(requestsUrl, mediaToken, creatorWrite(c::newUuid()), &empty).body.find("MEMORY_EDIT_FORBIDDEN") != std::string::npos);
                CHECK(call(missing + "/delete-requests", partnerSession, partnerWrite(c::newUuid()), &empty).body.find("MEMORY_NOT_FOUND") != std::string::npos);
                const std::string requestKey = c::newUuid();
                const auto requested = call(requestsUrl, partnerSession, partnerWrite(requestKey), &empty);
                CHECK_EQ(requested.status, 201L);
                CHECK(requested.headers.find("no-store") != std::string::npos);
                CHECK(requested.body.find("LIVE-M1A") == std::string::npos);
                CHECK(requested.body.find(photo) == std::string::npos);
                const auto requestFacts = parsed(requested)["request"];
                CHECK_EQ(requestFacts["status"].asString(), std::string("pending"));
                CHECK_EQ(requestFacts.size(), 7u);
                const auto requestId = requestFacts["request_id"].asString();
                CHECK_EQ(call(requestsUrl, partnerSession, partnerWrite(requestKey), &empty).status, 200L);
                const auto second = call(requestsUrl, partnerSession, partnerWrite(c::newUuid()), &empty);
                CHECK_EQ(second.status, 409L);
                CHECK(second.body.find("MEMORY_PROPOSAL_PENDING") != std::string::npos);
                const std::string requestUrl = requestsUrl + "/" + requestId;
                CHECK_EQ(parsed(get(requestUrl, mediaToken))["request"]["status"].asString(), std::string("pending"));
                CHECK_EQ(parsed(get(memoryUrl, mediaToken))["memory"]["pending_delete_request"]["id"].asString(), requestId);
                const std::string approveBody = R"({"outcome":"approve"})";
                const std::string decisionUrl = requestUrl + "/decision";
                const std::string badOutcome = R"({"outcome":"maybe"})";
                CHECK_EQ(call(decisionUrl, mediaToken, creatorWrite(c::newUuid()), &badOutcome).status, 400L);
                CHECK_EQ(call(decisionUrl, partnerSession, partnerWrite(c::newUuid()), &approveBody).status, 403L);
                const std::string approveKey = c::newUuid();
                const auto approved = call(decisionUrl, mediaToken, creatorWrite(approveKey), &approveBody);
                CHECK_EQ(approved.status, 200L);
                CHECK(approved.body.find("LIVE-M1A") == std::string::npos);
                const auto approvedBody = parsed(approved);
                CHECK_EQ(approvedBody["request"]["status"].asString(), std::string("approved"));
                CHECK_EQ(approvedBody["deletion"]["state"].asString(), std::string("deleted"));
                CHECK_EQ(approvedBody["deletion"]["cycle_no"].asInt(), 3);
                CHECK_EQ(get(memoryUrl, partnerSession).status, 404L);
                CHECK_EQ(get(requestUrl, partnerSession).status, 404L);
                CHECK(parsed(get(planUrl, mediaToken))["memory_unavailable"].asBool());
                // The partner's replay says what became of the request, without the cycle, and only that.
                const auto partnerReplay = call(requestsUrl, partnerSession, partnerWrite(requestKey), &empty);
                CHECK_EQ(partnerReplay.status, 200L);
                CHECK_EQ(parsed(partnerReplay)["request"]["status"].asString(), std::string("approved"));
                CHECK(!parsed(partnerReplay).isMember("deletion"));
                const auto cycleTwo = approvedBody["deletion"]["deletion_id"].asString();
                CHECK_EQ(call(memoryUrl + "/deletions/" + cycleTwo + "/undo", mediaToken, deleteHeaders(c::newUuid()), &empty).status, 200L);
                const auto afterUndo = parsed(get(requestUrl, partnerSession));
                CHECK_EQ(afterUndo["request"]["status"].asString(), std::string("approved"));
                const auto approveReplay = parsed(call(decisionUrl, mediaToken, creatorWrite(approveKey), &approveBody));
                CHECK_EQ(approveReplay["deletion"]["state"].asString(), std::string("undone"));
                CHECK_EQ(get(memoryUrl, partnerSession).status, 200L);
            }

            // P3-2a: album routes, through the real filter and controller. An empty album needs no store.
            const std::string albums = base + "/api/v1/albums";
            const std::string nameBody = R"({"name":"旅行"})";
            const auto albumHeaders = [&](const std::string &key) {
                return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json", "Idempotency-Key: " + key};
            };
            CHECK_EQ(get(albums).status, 401L);
            CHECK_EQ(call(albums, "", albumHeaders(c::newUuid()), &nameBody).status, 401L);
            CHECK_EQ(call(albums, mediaToken, {"Content-Type: application/json"}, &nameBody).status, 403L);
            CHECK_EQ(call(albums, mediaToken, noKey, &nameBody).status, 400L);
            const std::string longName = R"({"name":")" + std::string(41, 'x') + R"("})";
            CHECK_EQ(call(albums, mediaToken, albumHeaders(c::newUuid()), &longName).status, 400L);
            const std::string albumKey = c::newUuid();
            const auto madeAlbum = call(albums, mediaToken, albumHeaders(albumKey), &nameBody);
            CHECK_EQ(madeAlbum.status, 201L);
            CHECK(madeAlbum.headers.find("no-store") != std::string::npos);
            const auto albumId = parsed(madeAlbum)["album"]["id"].asString();
            CHECK(parsed(madeAlbum)["album"]["cover"].isNull());
            CHECK_EQ(parsed(madeAlbum)["album"]["photo_count"].asInt(), 0);
            CHECK_EQ(call(albums, mediaToken, albumHeaders(albumKey), &nameBody).status, 200L);
            const auto reused = call(albums, mediaToken, albumHeaders(albumKey), &longName);
            CHECK_EQ(reused.status, 400L);
            const std::string otherName = R"({"name":"别的"})";
            const auto keyReused = call(albums, mediaToken, albumHeaders(albumKey), &otherName);
            CHECK_EQ(keyReused.status, 409L);
            CHECK(keyReused.body.find("IDEMPOTENCY_KEY_REUSED") != std::string::npos);
            const auto albumList = get(albums, mediaToken);
            CHECK_EQ(albumList.status, 200L);
            CHECK_EQ(parsed(albumList)["albums"].size(), 1u);
            CHECK(albumList.body.find("\"uploads_enabled\":false") != std::string::npos);
            CHECK_EQ(get(albums + "?cursor=x", mediaToken).status, 400L);
            CHECK_EQ(get(albums + "?limit=0", mediaToken).status, 400L);
            const std::string thisAlbum = albums + "/" + albumId;
            CHECK_EQ(get(thisAlbum, mediaToken).status, 200L);
            // The partner reads it; someone else's session gets the same answer as an absent id.
            CHECK_EQ(get(thisAlbum, commitToken(pool, s.userA)).status, 200L);
            ts::CommittedSpace stranger;
            ts::commitSpace(pool, stranger, true);
            const auto foreign = get(thisAlbum, commitToken(pool, stranger.userA));
            const auto absentAlbum = get(albums + "/" + c::newUuid(), mediaToken);
            CHECK_EQ(foreign.status, 404L);
            CHECK_EQ(absentAlbum.status, 404L);
            CHECK(foreign.body.find("ALBUM_NOT_FOUND") != std::string::npos);
            CHECK(absentAlbum.body.find("ALBUM_NOT_FOUND") != std::string::npos);
            CHECK_EQ(get(albums + "/not-a-uuid", mediaToken).status, 404L);
            const auto noPhotos = get(thisAlbum + "/photos", mediaToken);
            CHECK_EQ(noPhotos.status, 200L);
            CHECK_EQ(parsed(noPhotos)["photos"].size(), 0u);
            CHECK(parsed(noPhotos)["next_cursor"].isNull());
            CHECK_EQ(get(thisAlbum + "/photos?cursor=0", mediaToken).status, 400L);
            const std::string addBody = R"({"media_ids":[")" + c::newUuid() + R"("]})";
            const auto addRefused = call(thisAlbum + "/photos", mediaToken, albumHeaders(c::newUuid()), &addBody);
            CHECK_EQ(addRefused.status, 409L);
            CHECK(addRefused.body.find("ALBUM_PHOTO_UNAVAILABLE") != std::string::npos);
            const auto removeRefused = call(thisAlbum + "/photos/" + c::newUuid() + "/removal", mediaToken, albumHeaders(c::newUuid()), &empty);
            CHECK_EQ(removeRefused.status, 404L);
            CHECK(removeRefused.body.find("ALBUM_PHOTO_NOT_FOUND") != std::string::npos);
            CHECK_EQ(call(thisAlbum + "/photos/not-a-uuid/removal", mediaToken, albumHeaders(c::newUuid()), &empty).status, 404L);
            const std::string coverBody = R"({"media_id":")" + c::newUuid() + R"("})";
            const auto coverRefused = call(thisAlbum + "/cover", mediaToken, albumHeaders(c::newUuid()), &coverBody);
            CHECK_EQ(coverRefused.status, 404L);
            CHECK(coverRefused.body.find("ALBUM_PHOTO_NOT_FOUND") != std::string::npos);
            const std::string badCover = R"({"media_id":"x"})";
            CHECK_EQ(call(thisAlbum + "/cover", mediaToken, albumHeaders(c::newUuid()), &badCover).status, 400L);
            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM albums WHERE creator_id = $1::uuid", s.userB), 1);

            // P3-2b: rename and requests end to end, through the real filter. userB created this album; userA is the partner.
            const std::string partnerCsrf = usapp::crypto::randomHex(32);
            const auto partnerToken = commitToken(pool, s.userA, partnerCsrf);
            const auto partnerHeaders = [&](const std::string &key) {
                return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + partnerCsrf, "Content-Type: application/json", "Idempotency-Key: " + key};
            };
            const std::string revisions = thisAlbum + "/revisions";
            const std::string proposals = thisAlbum + "/proposals";
            const auto named = [](int version, const std::string &name) { return R"({"expected_version":)" + std::to_string(version) + R"(,"name":")" + name + R"("})"; };
            const std::string renameOne = named(1, "改名");
            CHECK_EQ(call(revisions, "", albumHeaders(c::newUuid()), &renameOne).status, 401L);
            CHECK_EQ(call(revisions, mediaToken, {"Content-Type: application/json"}, &renameOne).status, 403L);
            CHECK_EQ(call(revisions, mediaToken, noKey, &renameOne).status, 400L);
            CHECK_EQ(call(revisions, mediaToken, albumHeaders(c::newUuid()), &nameBody).status, 400L);
            const auto partnerRename = call(revisions, partnerToken, partnerHeaders(c::newUuid()), &renameOne);
            CHECK_EQ(partnerRename.status, 403L);
            CHECK(partnerRename.body.find("ALBUM_EDIT_FORBIDDEN") != std::string::npos);
            const std::string renameKey = c::newUuid();
            const auto renamedAlbum = call(revisions, mediaToken, albumHeaders(renameKey), &renameOne);
            CHECK_EQ(renamedAlbum.status, 201L);
            CHECK(renamedAlbum.headers.find("no-store") != std::string::npos);
            CHECK_EQ(parsed(renamedAlbum)["album"]["name"].asString(), std::string("改名"));
            CHECK_EQ(parsed(renamedAlbum)["album"]["version"].asInt(), 2);
            CHECK_EQ(call(revisions, mediaToken, albumHeaders(renameKey), &renameOne).status, 200L);
            const auto staleRename = call(revisions, mediaToken, albumHeaders(c::newUuid()), &renameOne);
            CHECK_EQ(staleRename.status, 409L);
            CHECK(staleRename.body.find("ALBUM_VERSION_CONFLICT") != std::string::npos);
            const std::string proposeBody = named(2, "申请");
            const auto creatorRequest = call(proposals, mediaToken, albumHeaders(c::newUuid()), &proposeBody);
            CHECK_EQ(creatorRequest.status, 403L);
            const auto requested = call(proposals, partnerToken, partnerHeaders(c::newUuid()), &proposeBody);
            CHECK_EQ(requested.status, 201L);
            const auto requestId = parsed(requested)["mutation"]["proposal_id"].asString();
            CHECK_EQ(parsed(requested)["album"]["pending_proposal"]["id"].asString(), requestId);
            const std::string renameTwo = named(2, "直接");
            const auto whilePending = call(revisions, mediaToken, albumHeaders(c::newUuid()), &renameTwo);
            CHECK_EQ(whilePending.status, 409L);
            CHECK(whilePending.body.find("ALBUM_PROPOSAL_PENDING") != std::string::npos);
            const auto review = get(proposals + "/" + requestId, mediaToken);
            CHECK_EQ(review.status, 200L);
            CHECK_EQ(parsed(review)["proposal"]["original_name"].asString(), std::string("改名"));
            CHECK_EQ(parsed(review)["proposal"]["proposed_name"].asString(), std::string("申请"));
            CHECK_EQ(get(proposals + "/not-a-uuid", mediaToken).status, 404L);
            const auto foreignReview = get(proposals + "/" + requestId, commitToken(pool, stranger.userA));
            CHECK_EQ(foreignReview.status, 404L);
            CHECK(foreignReview.body.find("ALBUM_PROPOSAL_NOT_FOUND") != std::string::npos);
            const std::string decisionUrl = proposals + "/" + requestId + "/decision";
            const std::string approveBody = R"({"outcome":"approve"})";
            const std::string badOutcome = R"({"outcome":"maybe"})";
            CHECK_EQ(call(decisionUrl, mediaToken, albumHeaders(c::newUuid()), &badOutcome).status, 400L);
            const auto partnerApprove = call(decisionUrl, partnerToken, partnerHeaders(c::newUuid()), &approveBody);
            CHECK_EQ(partnerApprove.status, 403L);
            const auto absentRequest = call(proposals + "/" + c::newUuid() + "/decision", mediaToken, albumHeaders(c::newUuid()), &approveBody);
            CHECK_EQ(absentRequest.status, 404L);
            CHECK(absentRequest.body.find("ALBUM_PROPOSAL_NOT_FOUND") != std::string::npos);
            const std::string approveKey = c::newUuid();
            const auto approvedRequest = call(decisionUrl, mediaToken, albumHeaders(approveKey), &approveBody);
            CHECK_EQ(approvedRequest.status, 200L);
            CHECK_EQ(parsed(approvedRequest)["album"]["name"].asString(), std::string("申请"));
            CHECK_EQ(parsed(approvedRequest)["album"]["version"].asInt(), 3);
            CHECK_EQ(call(decisionUrl, mediaToken, albumHeaders(approveKey), &approveBody).status, 200L);
            const auto decidedAgain = call(decisionUrl, mediaToken, albumHeaders(c::newUuid()), &approveBody);
            CHECK_EQ(decidedAgain.status, 409L);
            CHECK(decidedAgain.body.find("ALBUM_PROPOSAL_DECIDED") != std::string::npos);
            // The partner reads the new name.
            CHECK_EQ(parsed(get(thisAlbum, partnerToken))["album"]["name"].asString(), std::string("申请"));
            // Rename-request history: the GET beside the POST on the same path. Summaries only; the names stay in the detail read.
            CHECK_EQ(get(proposals).status, 401L);
            for (const auto &session : {mediaToken, partnerToken}) {
                const auto history = get(proposals, session);
                CHECK_EQ(history.status, 200L);
                CHECK(history.headers.find("no-store") != std::string::npos);
                const auto listedHistory = parsed(history);
                CHECK_EQ(listedHistory["proposals"].size(), 1u);
                CHECK_EQ(listedHistory["proposals"][0]["id"].asString(), requestId);
                CHECK_EQ(listedHistory["proposals"][0]["status"].asString(), std::string("approved"));
                CHECK_EQ(listedHistory["proposals"][0]["approved_version"].asInt(), 3);
                CHECK_EQ(listedHistory["proposals"][0].size(), 9u);
                CHECK_EQ(listedHistory["current_version"].asInt(), 3);
                CHECK(listedHistory["next_cursor"].isNull());
                CHECK(history.body.find("申请") == std::string::npos);
                CHECK(history.body.find("改名") == std::string::npos);
            }
            CHECK_EQ(get(proposals + "?limit=0", mediaToken).status, 400L);
            CHECK_EQ(get(proposals + "?limit=101", mediaToken).status, 400L);
            CHECK_EQ(get(proposals + "?cursor=nonsense", mediaToken).status, 400L);
            const auto foreignHistory = get(proposals, commitToken(pool, stranger.userA));
            CHECK_EQ(foreignHistory.status, 404L);
            CHECK(foreignHistory.body.find("ALBUM_NOT_FOUND") != std::string::npos);
            CHECK(get(albums + "/" + c::newUuid() + "/proposals", mediaToken).body.find("ALBUM_NOT_FOUND") != std::string::npos);
            CHECK(get(albums + "/not-a-uuid/proposals", mediaToken).body.find("ALBUM_NOT_FOUND") != std::string::npos);

            // Unattached uploads: one READY photo of this member with no home, committed directly (no real process has a store).
            {
                const std::string free = c::newUuid();
                pool->execSqlSync(
                    "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, idempotency_key, request_fingerprint, created_at) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, 'staging/' || $2 || '/' || $1 || '/upload', 'image/png', 1000, clock_timestamp() - interval '1 hour', $4, repeat('a', 64), clock_timestamp() - interval '70 minutes')",
                    free, s.couple, s.userB, c::newUuid());
                pool->execSqlSync(
                    "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key, created_at) "
                    "VALUES ($1::uuid, 1, $2::uuid, 'media/' || $2 || '/' || $1 || '/' || gen_random_uuid() || '/original', clock_timestamp() - interval '1 hour')",
                    free, s.couple);
                pool->execSqlSync(
                    "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, attempt_no, real_format, width, height, stored_size, etag, decided_at) "
                    "VALUES ($1::uuid, $2::uuid, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp() - interval '50 minutes')",
                    free, s.couple);
                const std::string unattachedUrl = base + "/api/v1/media/unattached";
                CHECK_EQ(get(unattachedUrl).status, 401L);
                const auto mine = get(unattachedUrl, mediaToken);
                CHECK_EQ(mine.status, 200L);
                CHECK(mine.headers.find("no-store") != std::string::npos);
                CHECK(mine.body.find(free) != std::string::npos);
                CHECK(mine.body.find("staging/") == std::string::npos);
                CHECK(mine.body.find("media/") == std::string::npos);
                CHECK(parsed(mine)["photos"].isArray());
                CHECK(parsed(mine).isMember("next_cursor"));
                // The partner and another space never see it; a bad page request is refused.
                CHECK(get(unattachedUrl, partnerToken).body.find(free) == std::string::npos);
                CHECK(get(unattachedUrl, commitToken(pool, stranger.userA)).body.find(free) == std::string::npos);
                CHECK_EQ(get(unattachedUrl + "?limit=0", mediaToken).status, 400L);
                CHECK_EQ(get(unattachedUrl + "?cursor=nonsense", mediaToken).status, 400L);
                // Its literal path is not taken for a media id.
                CHECK(get(unattachedUrl, mediaToken).body.find("MEDIA_NOT_FOUND") == std::string::npos);
            }

            // Upload capability: no store is configured in this process, so it agrees with the timeline's uploads_enabled:false.
            {
                const std::string capabilityUrl = base + "/api/v1/media/capabilities";
                CHECK_EQ(get(capabilityUrl).status, 401L);
                for (const auto &session : {mediaToken, partnerToken}) {
                    const auto capability = get(capabilityUrl, session);
                    CHECK_EQ(capability.status, 200L);
                    CHECK(capability.headers.find("no-store") != std::string::npos);
                    const auto body = parsed(capability);
                    CHECK(body.isObject() && body.size() == 1u);
                    CHECK(body["uploads_enabled"].isBool() && !body["uploads_enabled"].asBool());
                }
                CHECK(get(base + "/api/v1/memories", mediaToken).body.find("\"uploads_enabled\":false") != std::string::npos);
                const std::string loner = c::newUuid();
                ts::commitUser(pool, loner, "cap_live");
                const auto noSpace = get(capabilityUrl, commitToken(pool, loner));
                CHECK_EQ(noSpace.status, 404L);
                CHECK(noSpace.body.find("SPACE_NOT_FOUND") != std::string::npos);
                CHECK(get(capabilityUrl, mediaToken).body.find("MEDIA_NOT_FOUND") == std::string::npos);
            }

            // D20: shared-plan creation's optional notify_partner through the real filter and controller (memories and albums are covered in
            // phase3_notification_db). Default off; true creates one generic notice; the same key never makes another.
            {
                const std::string plans = base + "/api/v1/shared-plans";
                const auto jsonWrite = [&](const std::string &key) {
                    return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json", "Idempotency-Key: " + key};
                };
                const std::string planBody = R"({"title":"D20-LIVE-PLAN-TITLE","date":null,"time":null,"place":null,"note":"D20-LIVE-PLAN-NOTE"})";
                const auto withChoice = [&](const std::string &choice) {
                    return std::string(R"({"body":)") + planBody + (choice.empty() ? std::string() : R"(,"notify_partner":)" + choice) + "}";
                };
                const auto noticesFor = [&](const std::string &plan) {
                    return c::scalar(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE shared_plan_id = $1::uuid", plan);
                };
                const auto omittedBody = withChoice("");
                const auto falseBody = withChoice("false");
                const auto trueBody = withChoice("true");
                const std::string offKey = c::newUuid();
                const auto off = call(plans, mediaToken, jsonWrite(offKey), &omittedBody);
                CHECK_EQ(off.status, 201L);
                const auto offFacts = parsed(off)["notification"];
                CHECK(offFacts.isObject() && !offFacts["requested"].asBool() && !offFacts["created"].asBool());
                CHECK_EQ(offFacts["reason"].asString(), std::string("not_requested"));
                const auto offPlan = parsed(off)["plan"]["id"].asString();
                CHECK_EQ(noticesFor(offPlan), 0);
                // Omitted and false are one request; true under the same key is key reuse.
                CHECK_EQ(call(plans, mediaToken, jsonWrite(offKey), &falseBody).status, 200L);
                const auto reused = call(plans, mediaToken, jsonWrite(offKey), &trueBody);
                CHECK_EQ(reused.status, 409L);
                CHECK(reused.body.find("IDEMPOTENCY_KEY_REUSED") != std::string::npos);
                CHECK_EQ(noticesFor(offPlan), 0);

                const std::string onKey = c::newUuid();
                const auto on = call(plans, mediaToken, jsonWrite(onKey), &trueBody);
                CHECK_EQ(on.status, 201L);
                const auto onFacts = parsed(on)["notification"];
                CHECK(onFacts["requested"].asBool() && onFacts["created"].asBool());
                CHECK_EQ(onFacts["reason"].asString(), std::string("delivered_to_inbox"));
                CHECK(onFacts["notification_id"].isString());
                const auto onPlan = parsed(on)["plan"]["id"].asString();
                CHECK_EQ(noticesFor(onPlan), 1);
                // A lost response: the retry with the same key and choice reads the original decision and adds nothing.
                const auto retried = call(plans, mediaToken, jsonWrite(onKey), &trueBody);
                CHECK_EQ(retried.status, 200L);
                CHECK_EQ(parsed(retried)["notification"]["notification_id"].asString(), onFacts["notification_id"].asString());
                CHECK_EQ(call(plans, mediaToken, jsonWrite(onKey), &omittedBody).status, 409L);
                CHECK_EQ(call(plans, mediaToken, jsonWrite(onKey), &falseBody).status, 409L);
                CHECK_EQ(noticesFor(onPlan), 1);
                // The partner's inbox names the plan and nothing of it; the actor's own inbox and another couple's get nothing new.
                const auto partnerInbox = get(base + "/api/v1/notifications", partnerToken);
                CHECK_EQ(partnerInbox.status, 200L);
                CHECK(partnerInbox.body.find("D20-LIVE-PLAN") == std::string::npos);
                CHECK(partnerInbox.body.find("\"shared_plan.created\"") != std::string::npos);
                CHECK(partnerInbox.body.find(onPlan) != std::string::npos);
                CHECK(get(base + "/api/v1/notifications", mediaToken).body.find(onPlan) == std::string::npos);
                ts::CommittedSpace outsider;
                ts::commitSpace(pool, outsider, true);
                CHECK(get(base + "/api/v1/notifications", commitToken(pool, outsider.userA)).body.find(onPlan) == std::string::npos);
                CHECK_EQ(get(plans + "/" + onPlan, commitToken(pool, outsider.userA)).status, 404L);

                // A non-boolean choice is refused before anything is saved, on all three D20 writes; other plan writes refuse the field.
                const auto badPlan = withChoice("\"yes\"");
                CHECK_EQ(call(plans, mediaToken, jsonWrite(c::newUuid()), &badPlan).status, 400L);
                const std::string badMemory = R"({"date":"2026-03-01","title":"t","media_ids":[")" + c::newUuid() + R"("],"notify_partner":1})";
                CHECK_EQ(call(base + "/api/v1/memories", mediaToken, jsonWrite(c::newUuid()), &badMemory).status, 400L);
                const std::string badAlbum = R"({"media_ids":[")" + c::newUuid() + R"("],"notify_partner":"true"})";
                CHECK_EQ(call(base + "/api/v1/albums/" + c::newUuid() + "/photos", mediaToken, jsonWrite(c::newUuid()), &badAlbum).status, 400L);
                const std::string editWithChoice = std::string(R"({"expected_version":1,"body":)") + planBody + R"(,"notify_partner":true})";
                const auto edited = call(plans + "/" + onPlan + "/revisions", mediaToken, jsonWrite(c::newUuid()), &editWithChoice);
                CHECK(edited.status != 200L && edited.status != 201L);
                CHECK_EQ(noticesFor(onPlan), 1);

                // No partner: a successful creation with no notice.
                ts::CommittedSpace solo;
                ts::commitSpace(pool, solo, false);
                const std::string soloCsrf = usapp::crypto::randomHex(32);
                const auto soloToken = commitToken(pool, solo.userA, soloCsrf);
                const auto lonely = call(plans, soloToken,
                                         {"Origin: https://localhost:8443", "X-CSRF-Token: " + soloCsrf, "Content-Type: application/json", "Idempotency-Key: " + c::newUuid()},
                                         &trueBody);
                CHECK_EQ(lonely.status, 201L);
                CHECK_EQ(parsed(lonely)["notification"]["reason"].asString(), std::string("no_partner"));
                CHECK(!parsed(lonely)["notification"]["created"].asBool());
                CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid AND event_type IN('memory.created','shared_plan.created','album.photos_added')", solo.couple), 0);
            }

            // Phase 4 B1 avatar catalog, through the real filters on both listeners. Admin routes are registered on both; the
            // admin surface header (injected only by the admin site), peer CIDR, exact admin Origin, X-Requested-By and the admin
            // session's CSRF token decide. A user session never reaches catalog administration.
            {
                const char *adminBase = std::getenv("USAPP_TEST_ADMIN_API_URL");
                const char *maintInfo = std::getenv("USAPP_TEST_MAINT_CONNINFO");
                CHECK(adminBase != nullptr && maintInfo != nullptr);
                if (adminBase && maintInfo) {
                    const std::string adminOrigin = "https://localhost:8444";
                    // A fake registry asset, added by the maintenance role as the reviewed-art step would.
                    const std::string family = "hat.live_" + usapp::crypto::randomHex(4);
                    auto maint = drogon::orm::DbClient::newPgClient(maintInfo, 1);
                    maint->setTimeout(5.0);
                    for (int tries = 0;; ++tries) {
                        try {
                            maint->execSqlSync("SELECT 1");
                            break;
                        } catch (const std::exception &) {
                            if (tries >= 100) throw;
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                    }
                    maint->execSqlSync("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) "
                                       "VALUES ($1 || '.v1', 'hat', $1, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true)", family);
                    // An admin with one session, stored as login would store it.
                    const std::string adminId = pool->execSqlSync(
                        "INSERT INTO admin_users (username, password_hash, status) VALUES ($1, 'pbkdf2-sha256$1000$AAAA$AAAA', 'active') RETURNING id::text AS id",
                        "t_live_" + usapp::crypto::randomHex(5))[0]["id"].as<std::string>();
                    const std::string adminToken = usapp::crypto::randomHex(32);
                    const std::string adminCsrf = usapp::crypto::randomHex(32);
                    pool->execSqlSync("INSERT INTO admin_sessions (token_hash, csrf_token_hash, admin_user_id, expires_at) "
                                      "VALUES ($1, $2, $3::uuid, now() + interval '1 hour')",
                                      usapp::AdminAuthService::tokenHash(adminToken), usapp::crypto::sha256Hex(adminCsrf), adminId);
                    const auto adminCall = [&](const std::string &listener, const std::string &path, const std::vector<std::string> &extra,
                                               const std::string *body, bool withCookie) {
                        std::vector<std::string> headers = extra;
                        if (withCookie) headers.push_back("Cookie: __Host-usapp-admin=" + adminToken);
                        return call(listener + path, "", headers, body);
                    };
                    const std::vector<std::string> surface = {"X-Usapp-Admin-Surface: usapp-admin"};
                    const auto adminWriteHeaders = [&](const std::string &key) {
                        return std::vector<std::string>{"X-Usapp-Admin-Surface: usapp-admin", "Origin: " + adminOrigin, "X-Requested-By: usapp-admin",
                                                        "X-CSRF-Token: " + adminCsrf, "Content-Type: application/json", "Idempotency-Key: " + key};
                    };
                    const std::string createBody = R"({"category":"hat","starter":false,"name":"LIVE-HAT","asset_key":")" + family +
                                                   R"(.v1","price_sunshine":42,"listed":true})";
                    const std::string items = "/api/admin/v1/avatar/items";
                    const auto itemsBefore = c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_items WHERE family = $1", family);

                    // Refusals, on both listeners: no admin session, a user session, no surface header, a foreign Origin, no
                    // X-Requested-By, a wrong CSRF token, and an invalid key.
                    for (const std::string listener : {base, std::string(adminBase)}) {
                        CHECK_EQ(adminCall(listener, items, surface, nullptr, false).status, 401L);
                        CHECK_EQ(call(listener + items, mediaToken, surface, nullptr).status, 401L);
                        CHECK_EQ(adminCall(listener, items, {}, nullptr, true).status, 403L);
                        auto userWrite = adminWriteHeaders(c::newUuid());
                        CHECK_EQ(call(listener + items, mediaToken, userWrite, &createBody).status, 401L);
                        auto foreign = adminWriteHeaders(c::newUuid());
                        foreign[1] = "Origin: https://evil.example";
                        CHECK_EQ(adminCall(listener, items, foreign, &createBody, true).status, 403L);
                        auto noMarker = adminWriteHeaders(c::newUuid());
                        noMarker.erase(noMarker.begin() + 2);
                        CHECK_EQ(adminCall(listener, items, noMarker, &createBody, true).status, 403L);
                        auto badCsrf = adminWriteHeaders(c::newUuid());
                        badCsrf[3] = "X-CSRF-Token: " + usapp::crypto::randomHex(32);
                        CHECK_EQ(adminCall(listener, items, badCsrf, &createBody, true).status, 403L);
                        auto noKey = adminWriteHeaders(c::newUuid());
                        noKey.pop_back();
                        CHECK_EQ(adminCall(listener, items, noKey, &createBody, true).status, 400L);
                    }
                    CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_items WHERE family = $1", family), itemsBefore);

                    // The admin listener with a session: create, replay, changed payload.
                    const std::string key = c::newUuid();
                    const auto made = adminCall(adminBase, items, adminWriteHeaders(key), &createBody, true);
                    CHECK_EQ(made.status, 201L);
                    const auto itemId = parsed(made)["item"]["id"].asString();
                    CHECK_EQ(parsed(made)["item"]["price_sunshine"].asInt(), 42);
                    CHECK_EQ(adminCall(adminBase, items, adminWriteHeaders(key), &createBody, true).status, 200L);
                    const std::string changed = R"({"category":"hat","starter":false,"name":"LIVE-HAT","asset_key":")" + family +
                                                R"(.v1","price_sunshine":43,"listed":true})";
                    const auto reused = adminCall(adminBase, items, adminWriteHeaders(key), &changed, true);
                    CHECK_EQ(reused.status, 409L);
                    CHECK(reused.body.find("IDEMPOTENCY_KEY_REUSED") != std::string::npos);
                    CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_type = 'admin' AND actor_id = $1::uuid "
                                                 "AND action = 'avatar.item_created'", adminId), 1);
                    // Version conflict and unlisting through the revision route.
                    const std::string revisions = items + "/" + itemId + "/revisions";
                    const std::string stale = R"({"expected_version":2,"name":"LIVE-HAT","asset_key":")" + family +
                                              R"(.v1","price_sunshine":42,"listed":false})";
                    const auto conflict = adminCall(adminBase, revisions, adminWriteHeaders(c::newUuid()), &stale, true);
                    CHECK_EQ(conflict.status, 409L);
                    CHECK(conflict.body.find("AVATAR_VERSION_CONFLICT") != std::string::npos);
                    const auto badAsset = R"({"expected_version":1,"name":"LIVE-HAT","asset_key":"https://evil/x.v1","price_sunshine":42,"listed":false})";
                    const std::string badAssetBody = badAsset;
                    CHECK_EQ(adminCall(adminBase, revisions, adminWriteHeaders(c::newUuid()), &badAssetBody, true).status, 400L);
                    CHECK_EQ(adminCall(adminBase, items + "/" + c::newUuid(), surface, nullptr, true).status, 404L);

                    // Achievements: the session supplies identity; reads cannot mint currency or select a partner.
                    const std::string achievements = base + "/api/v1/achievements";
                    CHECK_EQ(get(achievements).status, 401L);
                    const auto badges = get(achievements, mediaToken);
                    CHECK_EQ(badges.status, 200L);
                    CHECK(badges.headers.find("no-store") != std::string::npos);
                    CHECK_EQ(parsed(badges)["badges"].size(), 5u);
                    CHECK_EQ(parsed(badges)["timezone"].asString(), std::string("Asia/Shanghai"));
                    const auto selectedOther = get(achievements + "?user_id=" + s.userA, mediaToken);
                    CHECK_EQ(parsed(selectedOther), parsed(badges));
                    CHECK_EQ(call(achievements, "", {"Cookie: __Host-usapp-admin=" + adminToken}, nullptr).status, 401L);
                    const auto lonelyAchievementUser = c::newUuid();
                    ts::commitUser(pool, lonelyAchievementUser, "achievement_http_lonely");
                    const auto lonelyAchievementToken = commitToken(pool, lonelyAchievementUser, csrf);
                    const auto noAchievementSpace = get(achievements, lonelyAchievementToken);
                    CHECK_EQ(noAchievementSpace.status, 404L);
                    CHECK(noAchievementSpace.body.find("SPACE_NOT_FOUND") != std::string::npos);
                    // Users read the global catalog and their own appearance; nothing else, and no writes.
                    const std::string catalogUrl = base + "/api/v1/avatar/catalog";
                    CHECK_EQ(get(catalogUrl).status, 401L);
                    const auto catalog = get(catalogUrl, mediaToken);
                    CHECK_EQ(catalog.status, 200L);
                    CHECK(catalog.headers.find("no-store") != std::string::npos);
                    CHECK(catalog.body.find(itemId) != std::string::npos);
                    CHECK(get(catalogUrl, partnerToken).body.find(itemId) != std::string::npos);
                    ts::CommittedSpace elsewhere;
                    ts::commitSpace(pool, elsewhere, true);
                    CHECK(get(catalogUrl, commitToken(pool, elsewhere.userA)).body.find(itemId) != std::string::npos);
                    CHECK(catalog.body.find("sha") == std::string::npos);
                    CHECK(catalog.body.find("http") == std::string::npos);
                    const std::string unlist = R"({"expected_version":1,"name":"LIVE-HAT","asset_key":")" + family +
                                               R"(.v1","price_sunshine":42,"listed":false})";
                    CHECK_EQ(adminCall(adminBase, revisions, adminWriteHeaders(c::newUuid()), &unlist, true).status, 201L);
                    CHECK(get(catalogUrl, mediaToken).body.find(itemId) == std::string::npos);
                    CHECK(adminCall(adminBase, items, surface, nullptr, true).body.find(itemId) != std::string::npos);
                    const auto mine = get(base + "/api/v1/avatar", mediaToken);
                    CHECK_EQ(mine.status, 200L);
                    CHECK_EQ(parsed(mine)["appearance"]["source"].asString(), std::string("starter_defaults"));
                    CHECK(mine.body.find("owned") == std::string::npos);
                    CHECK_EQ(get(base + "/api/v1/avatar").status, 401L);
                    CHECK_EQ(get(base + "/api/v1/avatar/" + s.userA, mediaToken).status, 404L);
                    // The admin session cookie is not a user session.
                    CHECK_EQ(call(base + "/api/v1/avatar", "", {"Cookie: __Host-usapp-admin=" + adminToken}, nullptr).status, 401L);
                    for (const std::string path : {"/api/v1/avatar/catalog", "/api/v1/avatar"}) {
                        const auto attempt = call(base + path, mediaToken, {"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf,
                                                                            "Content-Type: application/json", "Idempotency-Key: " + c::newUuid()}, &createBody);
                        CHECK(attempt.status != 200L && attempt.status != 201L);
                    }
                    CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_item_revisions WHERE item_id = $1::uuid", itemId), 2);
                    // Catalog administration of this item touched no wallet row (B2 purchases elsewhere in the database are legitimate).
                    CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE source_id = $1 OR source_id IN "
                                                 "(SELECT id::text FROM avatar_purchases WHERE item_id = $1::uuid)", itemId), 0);

                    // Phase 4 B2: purchases and owned items through the real filter and controller. userB is funded by a real
                    // task settlement over HTTP (userA publishes, userB completes), never by a forged ledger row.
                    {
                        const auto newItem = [&](const std::string &category, int price) {
                            const std::string fam = category + ".live2_" + usapp::crypto::randomHex(4);
                            maint->execSqlSync("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top, accessory_position) "
                                               "VALUES ($1 || '.v1', $2, $1, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), "
                                               "CASE WHEN $2 = 'hat' THEN true END, CASE WHEN $2 = 'accessory' THEN 'neck' END)", fam, category);
                            const std::string body = R"({"category":")" + category + R"(","starter":false,"name":"LIVE-B2","asset_key":")" + fam +
                                                     R"(.v1","price_sunshine":)" + std::to_string(price) + R"(,"listed":true})";
                            const auto made = adminCall(adminBase, items, adminWriteHeaders(c::newUuid()), &body, true);
                            CHECK_EQ(made.status, 201L);
                            return parsed(made)["item"]["id"].asString();
                        };
                        const std::string walletUrl = base + "/api/v1/wallet";
                        const auto before = parsed(get(walletUrl, mediaToken))["wallet"];
                        const std::string taskBody = R"({"title":"LIVE-B2-FUND","reward_flowers":0,"reward_sunshine":100})";
                        const auto task = call(base + "/api/v1/tasks", partnerToken, partnerHeaders(c::newUuid()), &taskBody);
                        CHECK_EQ(task.status, 201L);
                        const std::string emptyBody = "{}";
                        const auto userHeaders = [&](const std::string &key) {
                            return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json",
                                                            "Idempotency-Key: " + key};
                        };
                        CHECK_EQ(call(base + "/api/v1/tasks/" + parsed(task)["task"]["id"].asString() + "/completion", mediaToken,
                                      userHeaders(c::newUuid()), &emptyBody).status, 200L);
                        const auto funded = parsed(get(walletUrl, mediaToken))["wallet"];
                        CHECK_EQ(funded["sunshine"].asInt64(), before["sunshine"].asInt64() + 100);

                        const std::string purchases = base + "/api/v1/avatar/purchases";
                        const std::string hat = newItem("hat", 25);
                        const std::string buyBody = R"({"item_id":")" + hat + R"(","expected_version":1})";
                        // Boundary: session, Origin, CSRF, key and body shape, before anything is written.
                        CHECK_EQ(call(purchases, "", userHeaders(c::newUuid()), &buyBody).status, 401L);
                        CHECK_EQ(call(purchases, mediaToken, {"Content-Type: application/json", "Idempotency-Key: " + c::newUuid()}, &buyBody).status, 403L);
                        auto noKey = userHeaders(c::newUuid());
                        noKey.pop_back();
                        CHECK_EQ(call(purchases, mediaToken, noKey, &buyBody).status, 400L);
                        const std::string extraField = R"({"item_id":")" + hat + R"(","expected_version":1,"price_sunshine":1})";
                        CHECK_EQ(call(purchases, mediaToken, userHeaders(c::newUuid()), &extraField).status, 400L);
                        CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_purchases WHERE item_id = $1::uuid", hat), 0);

                        const std::string buyKey = c::newUuid();
                        const auto bought = call(purchases, mediaToken, userHeaders(buyKey), &buyBody);
                        CHECK_EQ(bought.status, 201L);
                        CHECK(bought.headers.find("no-store") != std::string::npos);
                        const auto facts = parsed(bought)["purchase"];
                        CHECK_EQ(facts["price_sunshine"].asInt(), 25);
                        CHECK_EQ(facts["debited_sunshine"].asInt(), 25);
                        CHECK_EQ(facts["sunshine_balance_after"].asInt64(), funded["sunshine"].asInt64() - 25);
                        const auto after = parsed(get(walletUrl, mediaToken))["wallet"];
                        CHECK_EQ(after["sunshine"].asInt64(), funded["sunshine"].asInt64() - 25);
                        CHECK_EQ(after["flower"].asInt64(), funded["flower"].asInt64());
                        // Lost response: the same key and body replay the original facts; anything else under the key is reuse.
                        const auto replay = call(purchases, mediaToken, userHeaders(buyKey), &buyBody);
                        CHECK_EQ(replay.status, 200L);
                        CHECK(parsed(replay)["replayed"].asBool());
                        Json::StreamWriterBuilder flat;
                        flat["indentation"] = "";
                        CHECK_EQ(Json::writeString(flat, parsed(replay)["purchase"]), Json::writeString(flat, facts));
                        const std::string otherBody = R"({"item_id":")" + hat + R"(","expected_version":2})";
                        CHECK(call(purchases, mediaToken, userHeaders(buyKey), &otherBody).body.find("IDEMPOTENCY_KEY_REUSED") != std::string::npos);
                        const auto owned = call(purchases, mediaToken, userHeaders(c::newUuid()), &buyBody);
                        CHECK_EQ(owned.status, 409L);
                        CHECK(owned.body.find("AVATAR_ALREADY_OWNED") != std::string::npos);
                        // The B1 item above is unlisted; a stale version and a price beyond the balance write nothing.
                        const std::string unlistedBody = R"({"item_id":")" + itemId + R"(","expected_version":2})";
                        CHECK(call(purchases, mediaToken, userHeaders(c::newUuid()), &unlistedBody).body.find("AVATAR_ITEM_UNAVAILABLE") != std::string::npos);
                        const std::string cap = newItem("handheld", 5);
                        const std::string staleBody = R"({"item_id":")" + cap + R"(","expected_version":2})";
                        CHECK(call(purchases, mediaToken, userHeaders(c::newUuid()), &staleBody).body.find("AVATAR_VERSION_CONFLICT") != std::string::npos);
                        const std::string dear = newItem("accessory", 9999);
                        const std::string dearBody = R"({"item_id":")" + dear + R"(","expected_version":1})";
                        const auto poor = call(purchases, mediaToken, userHeaders(c::newUuid()), &dearBody);
                        CHECK_EQ(poor.status, 409L);
                        CHECK(poor.body.find("INSUFFICIENT_BALANCE") != std::string::npos);
                        CHECK_EQ(parsed(get(walletUrl, mediaToken))["wallet"]["sunshine"].asInt64(), after["sunshine"].asInt64());

                        // Owned items: the caller's only; the partner, another couple and no couple see nothing of it.
                        const std::string ownedUrl = base + "/api/v1/avatar/owned";
                        CHECK_EQ(get(ownedUrl).status, 401L);
                        const auto mine = get(ownedUrl, mediaToken);
                        CHECK_EQ(mine.status, 200L);
                        CHECK(mine.headers.find("no-store") != std::string::npos);
                        CHECK(mine.body.find(facts["id"].asString()) != std::string::npos);
                        CHECK(mine.body.find("sunshine_balance\"") == std::string::npos);
                        CHECK(get(ownedUrl, partnerToken).body.find(hat) == std::string::npos);
                        CHECK_EQ(parsed(get(ownedUrl, partnerToken))["items"].size(), 0u);
                        CHECK(get(ownedUrl, commitToken(pool, elsewhere.userA)).body.find(hat) == std::string::npos);
                        const std::string nobody = c::newUuid();
                        ts::commitUser(pool, nobody, "b2_live_loner");
                        const auto loner = commitToken(pool, nobody);
                        CHECK_EQ(get(ownedUrl, loner).status, 404L);
                        CHECK_EQ(get(ownedUrl + "?limit=0", mediaToken).status, 400L);
                        CHECK_EQ(get(ownedUrl + "?limit=abc", mediaToken).status, 400L);
                        CHECK_EQ(get(ownedUrl + "?cursor=nonsense", mediaToken).status, 400L);
                        CHECK(parsed(get(ownedUrl + "?limit=1", mediaToken))["items"].size() == 1u);
                        // The admin sees a count, never who.
                        const auto adminView = adminCall(adminBase, items + "/" + hat, surface, nullptr, true);
                        CHECK_EQ(parsed(adminView)["item"]["owner_count"].asInt(), 1);
                        CHECK(adminView.body.find(s.userB) == std::string::npos);
                        CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM wallet_operations WHERE operation_type = 'AVATAR_PURCHASE' "
                                                     "AND actor_user_id = $1::uuid", s.userB), 1);

                        // Phase 4 B3: saving the caller's loadout through the real filter and controller. The caller comes from the
                        // session only; equipping writes no wallet row; the partner cannot wear what userB bought.
                        {
                            const std::string avatarUrl = base + "/api/v1/avatar";
                            const std::string loadoutUrl = base + "/api/v1/avatar/loadout";
                            const auto start = parsed(get(avatarUrl, mediaToken))["appearance"];
                            CHECK_EQ(start["source"].asString(), std::string("starter_defaults"));
                            CHECK_EQ(start["loadout_version"].asInt(), 0);
                            const auto loadoutBody = [&](int expected, const std::string &hatSlot, const std::string &extra = "") {
                                return std::string(R"({"expected_version":)") + std::to_string(expected) +
                                       R"(,"slots":{"hair":null,"clothes":null,"expression":null,"hat":)" + hatSlot +
                                       R"(,"handheld":null,"accessory":null})" + extra + "}";
                            };
                            const std::string withHat = loadoutBody(0, "\"" + hat + "\"");
                            CHECK_EQ(call(loadoutUrl, "", userHeaders(c::newUuid()), &withHat).status, 401L);
                            CHECK_EQ(call(loadoutUrl, mediaToken, {"Content-Type: application/json", "Idempotency-Key: " + c::newUuid()}, &withHat).status, 403L);
                            auto noKey = userHeaders(c::newUuid());
                            noKey.pop_back();
                            CHECK_EQ(call(loadoutUrl, mediaToken, noKey, &withHat).status, 400L);
                            const std::string forged = loadoutBody(0, "null", R"(,"user_id":")" + s.userA + R"(")");
                            CHECK_EQ(call(loadoutUrl, mediaToken, userHeaders(c::newUuid()), &forged).status, 400L);
                            const std::string wrongSlot = std::string(R"({"expected_version":0,"slots":{"hair":")") + hat +
                                                          R"(","clothes":null,"expression":null,"hat":null,"handheld":null,"accessory":null}})";
                            CHECK(call(loadoutUrl, mediaToken, userHeaders(c::newUuid()), &wrongSlot).body.find("AVATAR_SLOT_INVALID") != std::string::npos);
                            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_loadout_events WHERE user_id = $1::uuid", s.userB), 0);

                            const auto walletBefore = parsed(get(walletUrl, mediaToken))["wallet"];
                            const std::string saveKey = c::newUuid();
                            const auto saved = call(loadoutUrl, mediaToken, userHeaders(saveKey), &withHat);
                            CHECK_EQ(saved.status, 201L);
                            CHECK(saved.headers.find("no-store") != std::string::npos);
                            const auto facts = parsed(saved)["loadout"];
                            CHECK_EQ(facts["version"].asInt(), 1);
                            CHECK_EQ(facts["slots"]["hat"].asString(), hat);
                            CHECK_EQ(facts["slots"]["hair"].asString(), start["slots"]["hair"]["id"].asString());
                            CHECK(facts["requested"]["hair"].isNull());
                            CHECK_EQ(parsed(saved)["current"]["appearance"]["source"].asString(), std::string("saved_loadout"));
                            const auto walletAfter = parsed(get(walletUrl, mediaToken))["wallet"];
                            CHECK_EQ(walletAfter["sunshine"].asInt64(), walletBefore["sunshine"].asInt64());
                            CHECK_EQ(walletAfter["flower"].asInt64(), walletBefore["flower"].asInt64());
                            // Replay, key reuse, stale version.
                            const auto replay = call(loadoutUrl, mediaToken, userHeaders(saveKey), &withHat);
                            CHECK_EQ(replay.status, 200L);
                            Json::StreamWriterBuilder flat;
                            flat["indentation"] = "";
                            CHECK_EQ(Json::writeString(flat, parsed(replay)["loadout"]), Json::writeString(flat, facts));
                            const std::string noHat = loadoutBody(0, "null");
                            CHECK(call(loadoutUrl, mediaToken, userHeaders(saveKey), &noHat).body.find("IDEMPOTENCY_KEY_REUSED") != std::string::npos);
                            CHECK(call(loadoutUrl, mediaToken, userHeaders(c::newUuid()), &noHat).body.find("AVATAR_VERSION_CONFLICT") != std::string::npos);
                            const auto mine = parsed(get(avatarUrl, mediaToken))["appearance"];
                            CHECK_EQ(mine["loadout_version"].asInt(), 1);
                            CHECK_EQ(mine["slots"]["hat"]["id"].asString(), hat);
                            // Isolation: the partner cannot wear it and still has no saved loadout; no couple, no save.
                            const auto partnerTry = call(loadoutUrl, partnerToken, partnerHeaders(c::newUuid()), &withHat);
                            CHECK_EQ(partnerTry.status, 409L);
                            CHECK(partnerTry.body.find("AVATAR_NOT_OWNED") != std::string::npos);
                            CHECK_EQ(parsed(get(avatarUrl, partnerToken))["appearance"]["loadout_version"].asInt(), 0);
                            CHECK(get(avatarUrl, partnerToken).body.find(hat) == std::string::npos);
                            const std::string lonerCsrf = usapp::crypto::randomHex(32);
                            const auto lonerToken = commitToken(pool, nobody, lonerCsrf);
                            const std::string lonerBody = loadoutBody(0, "null");
                            const auto lonely = call(loadoutUrl, lonerToken, {"Origin: https://localhost:8443", "X-CSRF-Token: " + lonerCsrf,
                                                                              "Content-Type: application/json", "Idempotency-Key: " + c::newUuid()}, &lonerBody);
                            CHECK_EQ(lonely.status, 404L);
                            CHECK(lonely.body.find("SPACE_NOT_FOUND") != std::string::npos);
                            CHECK_EQ(get(avatarUrl, lonerToken).status, 200L);
                            CHECK_EQ(c::scalar(observer, "SELECT count(*)::bigint AS n FROM avatar_loadout_events WHERE user_id = $1::uuid", s.userB), 1);
                        }
                    }
                }
            }
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-live-api", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::reminderliveapi
