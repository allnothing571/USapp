#pragma once

// Admin read-only wallet inspection through the real API binary on both listeners (docs/admin-wallet-ledger.md Section 1):
// no session, an ordinary user's cookie, a missing admin surface header, then valid admin reads, request validation, and the
// caller-only /api/v1/wallet left unchanged. Runs only inside the live step of the isolated verification; tokens never printed.
#include "ExportLiveApiTest.h"
#include "MemoryDeletionDbTests.h"
#include "services/AdminAuthService.h"

namespace usapptest::adminwalletliveapi {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace live = usapptest::reminderliveapi;

inline void run(const c::ClientPtr &pool, const std::string &dsn, const std::string &base) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("Admin wallet live: AdminFilter on both listeners, exact decimal strings, validation, and the public wallet unchanged") {
        try {
            const char *adminBase = std::getenv("USAPP_TEST_ADMIN_API_URL");
            const char *maintInfo = std::getenv("USAPP_TEST_MAINT_CONNINFO");
            CHECK(adminBase != nullptr && maintInfo != nullptr);
            if (!adminBase || !maintInfo) return;
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            // One synthetic posting for userB, written as maintenance would (constraints apply).
            const auto op = c::newUuid();
            usapptest::memorydeletion::asMaintenance(
                "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES ('" + s.userB + "', 'flower', '" + s.couple + "'); "
                "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id) VALUES ('" + op + "', '" +
                s.couple + "', '" + s.userA + "', 'TASK_REWARD', 'task', '" + op + "'); "
                "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after) VALUES "
                "('" + op + "', '" + s.couple + "', '" + s.userB + "', 'flower', 1, 777, 777);");
            const std::string adminId = pool->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) VALUES ($1, 'pbkdf2-sha256$1000$AAAA$AAAA', 'active') RETURNING id::text AS id",
                "t_aw_" + usapp::crypto::randomHex(5))[0]["id"].as<std::string>();
            const std::string adminToken = usapp::crypto::randomHex(32);
            pool->execSqlSync("INSERT INTO admin_sessions (token_hash, csrf_token_hash, admin_user_id, expires_at) "
                              "VALUES ($1, $2, $3::uuid, now() + interval '1 hour')",
                              usapp::AdminAuthService::tokenHash(adminToken), usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)), adminId);
            const std::string userToken = live::commitToken(pool, s.userB);
            const std::vector<std::string> surface = {"X-Usapp-Admin-Surface: usapp-admin"};
            const auto admin = [&](const std::string &listener, const std::string &path, const std::vector<std::string> &headers) {
                auto h = headers;
                h.push_back("Cookie: __Host-usapp-admin=" + adminToken);
                return live::call(listener + path, "", h, nullptr);
            };
            const std::string walletPath = "/api/admin/v1/users/" + s.userB + "/wallet";
            const std::string ledgerPath = walletPath + "/ledger?asset=flower&limit=10";
            const auto writes = [&]() {
                return c::scalar(observer, "SELECT ((SELECT count(*) FROM wallet_operations) + (SELECT count(*) FROM wallet_ledger_entries) + "
                                           "(SELECT count(*) FROM wallet_accounts) + (SELECT count(*) FROM audit_logs))::bigint AS n "
                                           "WHERE $1::text IS NOT NULL", std::string("x"));
            };
            const auto before = writes();

            for (const std::string listener : {base, std::string(adminBase)}) {
                for (const auto &path : {walletPath, ledgerPath, std::string("/api/admin/v1/system/status")}) {
                    CHECK_EQ(live::call(listener + path, "", surface, nullptr).status, 401L);           // no session
                    CHECK_EQ(live::call(listener + path, userToken, surface, nullptr).status, 401L);    // an ordinary user's cookie
                    CHECK_EQ(admin(listener, path, {}).status, 403L);                                  // no admin surface header
                    const auto okRead = admin(listener, path, surface);                               // a valid admin read
                    CHECK_EQ(okRead.status, 200L);
                    CHECK(usapptest::exportliveapi::headerValue(okRead.headers, "Cache-Control").find("no-store") != std::string::npos);
                }
                const auto statusResponse = admin(listener, "/api/admin/v1/system/status", surface);
                const auto status = live::parsed(statusResponse);
                CHECK_EQ(status.getMemberNames().size(),std::size_t(3));
                CHECK(status["read_at"].isString());
                CHECK_EQ(status["database"].getMemberNames().size(),std::size_t(2));
                const auto migration = pool->execSqlSync("SELECT count(*)::integer AS n,max(version) AS v FROM schema_migrations");
                CHECK_EQ(status["database"]["migration_count"].asInt(),migration[0]["n"].as<int>());
                CHECK_EQ(status["database"]["latest_migration"].asString(),migration[0]["v"].as<std::string>());
                const auto process=status["process"];
                CHECK_EQ(process.getMemberNames().size(),std::size_t(6));
                for(const auto *field:{"scheduler_configured","reminders_configured","memory_purge_configured","uploads_enabled"}) CHECK(process[field].isBool());
                CHECK_FALSE(process["scheduler_configured"].asBool());
                CHECK_FALSE(process["reminders_configured"].asBool());
                CHECK_FALSE(process["memory_purge_configured"].asBool());
                CHECK_FALSE(process["uploads_enabled"].asBool());  // Real-provider configuration is off in this disposable HTTP fixture.
                CHECK_EQ(process["mail_mode"].asString(),std::string("log"));
                CHECK_EQ(process["registration_mode"].asString(),std::string("open"));
                CHECK(statusResponse.body.find(adminToken)==std::string::npos);
                CHECK(statusResponse.body.find(userToken)==std::string::npos);
                CHECK(statusResponse.body.find(adminId)==std::string::npos);
                const auto summary = live::parsed(admin(listener, walletPath, surface));
                CHECK_EQ(summary["user"]["id"].asString(), s.userB);
                CHECK(summary["balances"][0]["balance"].isString());
                CHECK_EQ(summary["balances"][0]["balance"].asString(), std::string("777"));
                const auto page = admin(listener, ledgerPath, surface);
                CHECK(page.body.find("\"amount\":\"777\"") != std::string::npos);
                CHECK(page.body.find("idempotency") == std::string::npos);
                CHECK(page.body.find("fingerprint") == std::string::npos);
            }
            // Validation and subject states on the admin listener.
            const std::string a(adminBase);
            CHECK_EQ(admin(a, "/api/admin/v1/users/" + c::newUuid() + "/wallet", surface).status, 404L);
            CHECK_EQ(admin(a, "/api/admin/v1/users/not-a-uuid/wallet", surface).status, 400L);
            CHECK_EQ(admin(a, walletPath + "/ledger", surface).status, 400L);                  // asset required
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=gold", surface).status, 400L);
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&limit=0", surface).status, 400L);
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&limit=101", surface).status, 400L);
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&cursor=nonsense", surface).status, 400L);
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&cursor=w1.sunshine.5." + s.userB, surface).status, 400L);  // other asset
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&cursor=w1.flower.5." + s.userA, surface).status, 400L);    // other user
            CHECK_EQ(admin(a, walletPath + "/ledger?asset=flower&cursor=w1.flower.1." + s.userB, surface).status, 200L);
            // The caller-only public read is unchanged: own balances only, and a user selector is ignored.
            const auto own = live::call(base + "/api/v1/wallet?user_id=" + s.userA, userToken, {}, nullptr);
            CHECK_EQ(own.status, 200L);
            CHECK_EQ(live::parsed(own)["wallet"]["flower"].asInt64(), 777);
            const auto partnerToken = live::commitToken(pool, s.userA);
            const auto partner = live::call(base + "/api/v1/wallet?user_id=" + s.userB, partnerToken, {}, nullptr);
            CHECK_EQ(live::parsed(partner)["wallet"]["flower"].asInt64(), 0);
            CHECK_EQ(writes(), before);  // the admin reads wrote no wallet or audit row
        } catch (const std::exception &) {
            c::expectThat(false, "admin-wallet-live", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::adminwalletliveapi
