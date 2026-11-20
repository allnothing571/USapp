#pragma once

// E1 user-data export through the real API binary (docs/user-data-export-design.md Section 8.1): its AuthFilter, Origin, CSRF,
// controller and headers, with the default process-local limits. Runs only inside the live step of the isolated verification,
// after the reminder live case, never in CTest. Tokens are per run and never printed.
#include <algorithm>
#include <cctype>

#include "ReminderLiveApiTest.h"

namespace usapptest::exportliveapi {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace live = usapptest::reminderliveapi;

inline std::string headerValue(const std::string &headers, const std::string &name) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto at = lower.find("\n" + key + ":");
    if (at == std::string::npos) return std::string();
    auto start = at + key.size() + 2;
    auto end = headers.find('\r', start);
    if (end == std::string::npos) end = headers.find('\n', start);
    std::string value = headers.substr(start, end - start);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    return value;
}

inline long long footprint(const c::Observer &o) {
    return c::scalar(o, "SELECT ((SELECT count(*) FROM audit_logs) + (SELECT count(*) FROM notifications) + (SELECT count(*) FROM notification_intents) + "
                        "(SELECT count(*) FROM wallet_operations) + (SELECT count(*) FROM wallet_ledger_entries) + (SELECT count(*) FROM achievement_activation))::bigint "
                        "AS n WHERE $1::text IS NOT NULL", std::string("x"));
}

inline void run(const c::ClientPtr &pool, const std::string &dsn, const std::string &base) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("E1 live: the real binary exports the session user's archive as an attachment, refuses selectors and limits per process") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const std::string marker = "E1LIVE-" + usapp::crypto::randomHex(6);
            pool->execSqlSync("INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint) "
                              "VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, repeat('c', 64))",
                              s.couple, s.userA, s.userB, marker, "e1live" + usapp::crypto::randomHex(8));
            const std::string csrfB = usapp::crypto::randomHex(32), csrfA = usapp::crypto::randomHex(32);
            const auto tokenB = live::commitToken(pool, s.userB, csrfB);
            const auto tokenA = live::commitToken(pool, s.userA, csrfA);
            const std::string url = base + "/api/v1/me/export";
            const auto headers = [](const std::string &csrf) {
                return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json"};
            };
            const std::string empty = "{}";
            const std::string loner = c::newUuid();
            ts::commitUser(pool, loner, "e1_live_loner");
            const std::string csrfL = usapp::crypto::randomHex(32);
            const auto lonerToken = live::commitToken(pool, loner, csrfL);
            // Login setup now emits its mandatory notice; capture the no-export-effects baseline after all login fixtures.
            const auto before = footprint(observer);

            CHECK_EQ(live::call(url, "", headers(csrfB), &empty).status, 401L);
            CHECK_EQ(live::call(url, tokenB, {"Content-Type: application/json"}, &empty).status, 403L);
            CHECK_EQ(live::call(url, tokenB, {"Origin: https://evil.example", "X-CSRF-Token: " + csrfB, "Content-Type: application/json"}, &empty).status, 403L);
            CHECK_EQ(live::call(url, tokenB, {"Origin: https://localhost:8443", "X-CSRF-Token: " + usapp::crypto::randomHex(32),
                                              "Content-Type: application/json"}, &empty).status, 403L);
            CHECK_EQ(live::get(url, tokenB).status == 200L, false);  // no GET route
            for (const std::string body : {std::string(R"({"user_id":")") + s.userA + "\"}", std::string(R"({"scope":"all"})"), std::string("[]"),
                                           std::string("not json")}) {
                const auto refused = live::call(url, tokenB, headers(csrfB), &body);
                CHECK_EQ(refused.status, 400L);
                CHECK(refused.body.find("VALIDATION_ERROR") != std::string::npos);
            }

            const auto ok = live::call(url, tokenB, headers(csrfB), &empty);
            CHECK_EQ(ok.status, 200L);
            CHECK(headerValue(ok.headers, "Content-Type").find("application/json") == 0);
            CHECK(headerValue(ok.headers, "Content-Disposition").find("attachment; filename=\"usapp-export-") == 0);
            CHECK(headerValue(ok.headers, "Cache-Control").find("no-store") != std::string::npos);
            CHECK_EQ(headerValue(ok.headers, "X-Content-Type-Options"), std::string("nosniff"));
            // The transmitted bytes: the declared length equals the body received, within the 16 MiB default cap.
            CHECK_EQ(headerValue(ok.headers, "Content-Length"), std::to_string(ok.body.size()));
            CHECK(ok.body.size() <= static_cast<size_t>(16 * 1024 * 1024));
            const auto doc = live::parsed(ok);
            CHECK_EQ(doc["export"]["subject_user_id"].asString(), s.userB);
            CHECK_EQ(doc["export"]["complete"].asBool(), true);
            CHECK(ok.body.find(marker) != std::string::npos);  // the partner's shared message, as the caller sees it
            CHECK(ok.body.find("e1live") == std::string::npos); // never its idempotency key

            // Process-local frequency: the same user again at once is refused with Retry-After; the partner is independent.
            const auto again = live::call(url, tokenB, headers(csrfB), &empty);
            CHECK_EQ(again.status, 429L);
            CHECK(again.body.find("RATE_LIMITED") != std::string::npos);
            CHECK(!headerValue(again.headers, "Retry-After").empty());
            const std::string absent;
            const auto partner = live::call(url, tokenA, headers(csrfA), &absent);
            CHECK_EQ(partner.status, 200L);
            CHECK_EQ(live::parsed(partner)["export"]["subject_user_id"].asString(), s.userA);

            // The two exports above may still be confirming their transaction ends (both slots busy): 429 until then, bounded.
            auto lonely = live::call(url, lonerToken, headers(csrfL), &empty);
            for (int tries = 0; lonely.status == 429L && tries < 100; ++tries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                lonely = live::call(url, lonerToken, headers(csrfL), &empty);
            }
            CHECK_EQ(lonely.status, 404L);
            CHECK(lonely.body.find("SPACE_NOT_FOUND") != std::string::npos);
            CHECK_EQ(footprint(observer), before);
        } catch (const std::exception &) {
            c::expectThat(false, "export-live-api", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::exportliveapi
