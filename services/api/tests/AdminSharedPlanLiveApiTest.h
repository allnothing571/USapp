#pragma once

// Admin read-only shared-plan inspection through the real API binary on both listeners (docs/admin-shared-plan-inspection.md):
// no session, an ordinary user's cookie (with a forged surface header), a missing or wrong admin surface header, then valid
// admin reads, request validation (malformed, unknown and foreign ids, limits, cursors, filters), and the public plan read
// left caller-scoped. Runs only inside the live step of the isolated verification; tokens never printed.
#include "AdminWalletLiveApiTest.h"
#include "AdminSharedPlanDbTests.h"

namespace usapptest::adminsharedplanliveapi {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace live = usapptest::reminderliveapi;

inline void run(const c::ClientPtr &pool, const std::string &dsn, const std::string &base) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("Admin shared plans live: AdminFilter on both listeners, decimal strings, id/limit/cursor/filter validation, public reads unchanged") {
        try {
            const char *adminBase = std::getenv("USAPP_TEST_ADMIN_API_URL");
            CHECK(adminBase != nullptr);
            if (!adminBase) return;
            ts::CommittedSpace s, other;
            ts::commitSpace(pool, s, true);
            ts::commitSpace(pool, other, true);
            const auto task=usapptest::adminsharedplan::create(pool,s.userA,"Admin plan live");
            const auto second=usapptest::adminsharedplan::create(pool,s.userA,"Admin plan live two");
            const auto foreign=usapptest::adminsharedplan::create(pool,other.userA,"FOREIGN-PLAN-TEXT");
            const std::string adminId = pool->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) VALUES ($1, 'pbkdf2-sha256$1000$AAAA$AAAA', 'active') RETURNING id::text AS id",
                "t_sp_" + usapp::crypto::randomHex(5))[0]["id"].as<std::string>();
            const std::string adminToken = usapp::crypto::randomHex(32);
            pool->execSqlSync("INSERT INTO admin_sessions (token_hash, csrf_token_hash, admin_user_id, expires_at) "
                              "VALUES ($1, $2, $3::uuid, now() + interval '1 hour')",
                              usapp::AdminAuthService::tokenHash(adminToken), usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)), adminId);
            const std::string userToken = live::commitToken(pool, s.userB);
            const std::string outsider = live::commitToken(pool, other.userB);
            const std::vector<std::string> surface = {"X-Usapp-Admin-Surface: usapp-admin"};
            const std::vector<std::string> wrongSurface = {"X-Usapp-Admin-Surface: usapp-web"};
            const auto admin = [&](const std::string &listener, const std::string &path, const std::vector<std::string> &headers) {
                auto h = headers;
                h.push_back("Cookie: __Host-usapp-admin=" + adminToken);
                return live::call(listener + path, "", h, nullptr);
            };
            const std::string listPath = "/api/admin/v1/couples/" + s.couple + "/shared-plans";
            const std::string detailPath = listPath + "/" + task;
            const auto writes=[&](){return usapptest::adminsharedplan::footprint(observer);};
            const auto before = writes();

            for (const std::string listener : {base, std::string(adminBase)}) {
                for (const auto &path : {listPath, detailPath}) {
                    CHECK_EQ(live::call(listener + path, "", surface, nullptr).status, 401L);          // no session
                    CHECK_EQ(live::call(listener + path, userToken, surface, nullptr).status, 401L);   // a user cookie, forged surface
                    CHECK_EQ(live::call(listener + path, userToken, {}, nullptr).status, 403L);        // a user cookie, no surface
                    CHECK_EQ(admin(listener, path, {}).status, 403L);                                 // no admin surface header
                    CHECK_EQ(admin(listener, path, wrongSurface).status, 403L);                       // a wrong surface value
                    const auto okRead = admin(listener, path, surface);                              // a valid admin read
                    CHECK_EQ(okRead.status, 200L);
                    CHECK(usapptest::exportliveapi::headerValue(okRead.headers, "Cache-Control").find("no-store") != std::string::npos);
                    CHECK(okRead.body.find("idempotency") == std::string::npos);
                    CHECK(okRead.body.find("fingerprint") == std::string::npos);
                    CHECK(okRead.body.find("operation_id") == std::string::npos);
                }
                const auto list = live::parsed(admin(listener, listPath, surface));
                CHECK_EQ(list["space"]["id"].asString(), s.couple);
                CHECK_EQ(list["items"].size(), 2u);
                CHECK_EQ(list["items"][0]["id"].asString(), second);
                const auto detail = live::parsed(admin(listener, detailPath, surface));
                CHECK_EQ(detail["plan"]["id"].asString(), task);
                CHECK(detail["plan"]["body_version"].isInt());
                CHECK_EQ(detail["plan"]["body"]["title"].asString(),std::string("Admin plan live"));
                CHECK_EQ(detail["plan"]["status"].asString(), std::string("want"));
            }
            // Validation and subject states on the admin listener.
            const std::string a(adminBase);
            const std::string upper = [&] { std::string u = s.couple; for (auto &ch : u) if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A'); return u; }();
            CHECK_EQ(admin(a, "/api/admin/v1/couples/" + upper + "/shared-plans", surface).status, 200L);           // canonicalised
            CHECK_EQ(admin(a, "/api/admin/v1/couples/not-a-uuid/shared-plans", surface).status, 400L);
            CHECK_EQ(admin(a, "/api/admin/v1/couples/not-a-uuid/shared-plans/" + task, surface).status, 400L);
            CHECK_EQ(admin(a, listPath + "/not-a-uuid", surface).status, 400L);
            const auto unknownSpace = admin(a, "/api/admin/v1/couples/" + c::newUuid() + "/shared-plans", surface);
            CHECK_EQ(unknownSpace.status, 404L);
            CHECK_EQ(live::parsed(unknownSpace)["error"]["code"].asString(), std::string("SPACE_NOT_FOUND"));
            const auto foreignRead = admin(a, listPath + "/" + foreign, surface);
            CHECK_EQ(foreignRead.status, 404L);
            CHECK_EQ(live::parsed(foreignRead)["error"]["code"].asString(), std::string("NOT_FOUND"));
            CHECK(foreignRead.body.find("FOREIGN-PLAN-TEXT") == std::string::npos);
            CHECK_EQ(admin(a, listPath + "/" + c::newUuid(), surface).status, 404L);
            CHECK_EQ(admin(a, "/api/admin/v1/couples/" + c::newUuid() + "/shared-plans/" + task, surface).status, 404L);
            CHECK_EQ(admin(a, listPath + "?limit=0", surface).status, 400L);
            CHECK_EQ(admin(a, listPath + "?limit=101", surface).status, 400L);
            CHECK_EQ(admin(a, listPath + "?limit=abc", surface).status, 400L);
            CHECK_EQ(admin(a, listPath + "?cursor=nonsense", surface).status, 400L);
            const auto firstPage = live::parsed(admin(a, listPath + "?limit=1", surface));
            const std::string cursor = firstPage["next_cursor"].asString();
            CHECK_FALSE(cursor.empty());
            CHECK_EQ(admin(a, listPath + "?limit=1&cursor=" + cursor, surface).status, 200L);
            CHECK_EQ(admin(a, "/api/admin/v1/couples/" + other.couple + "/shared-plans?cursor=" + cursor, surface).status, 400L);  // another space
            // The public plan read stays caller-scoped: a member of another space cannot read this task.
            CHECK_EQ(live::call(base + "/api/v1/shared-plans/" + task, outsider, {}, nullptr).status, 404L);
            CHECK_EQ(live::call(base + "/api/v1/shared-plans/" + task, userToken, {}, nullptr).status, 200L);
            CHECK_EQ(writes(), before);  // the admin reads wrote nothing
        } catch (const std::exception &) {
            c::expectThat(false, "admin-shared-plan-live", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::adminsharedplanliveapi
