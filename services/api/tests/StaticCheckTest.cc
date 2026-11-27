// =====================================================================
// services/api/tests/StaticCheckTest.cc
//
// 目标：usapp_test_static
// 静态合规性断言测试（纯文件与契约检查，不依赖数据库与网络）：
//   1. OpenAPI 契约合规：路径、方法、Cache-Control: no-store、
//      __Host- 前缀 cookie 与 X-CSRF-Token 头。
//   2. 数据库迁移 DDL 约束静态一致性：
//      - couple_members(user_id) UNIQUE 单一空间约束
//      - CHECK (slot IN (1, 2)) 与 UNIQUE (couple_id, slot) 槽位约束
//      - admin_users 与 users 彻底物理解耦（无外键关联）
//      - scheduler 事务级咨询锁规范
//   3. Drogon 控制器路由与参数绑定合规：
//      - 严格禁止 getParameter 读取路径参数
//      - 方法签名必须接收 std::string 路径参数（如 spaceId, inviteId, rawCode）
//      - 管理控制器必须引用 AdminFilter 守卫
//
// 依赖：C++17 <filesystem> 与 TestSupport.h
// =====================================================================

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "TestSupport.h"
#include "core/Crypto.h"

namespace {

std::string readFileContent(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] 无法打开文件: " << path.string() << "\n";
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::filesystem::path getRepoRoot() {
    const char *envRoot = std::getenv("USAPP_ROOT");
    if (envRoot && std::string(envRoot).length() > 0) {
        return std::filesystem::path(envRoot);
    }
    // 默认从 __FILE__ 向上推导：services/api/tests/StaticCheckTest.cc -> D:/JYJ/USapp
    std::filesystem::path thisFile(__FILE__);
    return thisFile.parent_path().parent_path().parent_path();
}

}  // namespace

int main() {
    const auto repoRoot = getRepoRoot();
    std::printf("[ INFO ] 使用代码仓根目录: %s\n", repoRoot.string().c_str());

    // -----------------------------------------------------------------
    // 1. OpenAPI 契约规范检查
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: contracts/openapi.yaml 契约合规") {
        const auto oasPath = repoRoot / "contracts" / "openapi.yaml";
        const std::string content = readFileContent(oasPath);
        CHECK_FALSE(content.empty());

        // 核心路径存在性
        CHECK_CONTAINS(content, "/api/v1/spaces:");
        CHECK_CONTAINS(content, "/api/v1/spaces/current:");
        CHECK_CONTAINS(content, "/api/v1/spaces/{spaceId}:");
        CHECK_CONTAINS(content, "/api/v1/spaces/current/invites:");
        CHECK_CONTAINS(content, "/api/v1/spaces/current/invites/{inviteId}:");
        CHECK_CONTAINS(content, "/api/v1/invites/{code}/accept:");
        // 阶段 1 M1：PATCH /api/v1/me（增量操作，请求体只允许 display_name）
        CHECK_CONTAINS(content, "operationId: updateMe");
        CHECK_CONTAINS(content, "UpdateMeRequest:");

        // 安全响应头与 Cookie 约定
        CHECK_CONTAINS(content, "Cache-Control:");
        CHECK_CONTAINS(content, "no-store");
        CHECK_CONTAINS(content, "__Host-");
        CHECK_CONTAINS(content, "X-CSRF-Token");
    }

    // -----------------------------------------------------------------
    // 2. 数据库迁移 DDL 约束静态一致性
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: 数据库迁移 0000-0005 关键不变量检查") {
        const auto migDir = repoRoot / "database" / "migrations";

        // 0002_identity.sql: 邮箱唯一与规范化约束
        const std::string idSql = readFileContent(migDir / "0002_identity.sql");
        CHECK_FALSE(idSql.empty());
        CHECK_CONTAINS(idSql, "email_canonical");
        CHECK_CONTAINS(idSql, "token_hash");

        // 0003_couple_binding.sql: 槽位取值域与容量
        const std::string coupleSql = readFileContent(migDir / "0003_couple_binding.sql");
        CHECK_FALSE(coupleSql.empty());
        CHECK_CONTAINS(coupleSql, "slot IN (1, 2)");
        CHECK_CONTAINS(coupleSql, "UNIQUE (couple_id, slot)");
        CHECK_CONTAINS(coupleSql, "UNIQUE (user_id)");

        // 0004_scheduler.sql: 调度锁与时间戳
        const std::string schedSql = readFileContent(migDir / "0004_scheduler.sql");
        CHECK_FALSE(schedSql.empty());
        CHECK_CONTAINS(schedSql, "scheduler_ticks");

        // 0005_admin_audit.sql: 管理员独立表无外键
        const std::string adminSql = readFileContent(migDir / "0005_admin_audit.sql");
        CHECK_FALSE(adminSql.empty());
        CHECK_CONTAINS(adminSql, "CREATE TABLE admin_users");
        CHECK_NOT_CONTAINS(adminSql, "REFERENCES users");  // 禁止引用普通用户
    }

    // -----------------------------------------------------------------
    // 2b. 阶段 1 M2-A：0006_wallet_ledger 与 init-roles 钱包权限收窄（静态文本断言）
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: 0006_wallet_ledger 只追加、链式校验、权限收窄且不写入数据") {
        usapptest::noteStaticAssertion("（文本存在性检查，不是运行时验证；运行时见 db_gated 钱包用例）");
        const auto migDir = repoRoot / "database" / "migrations";
        const std::string walletSql = readFileContent(migDir / "0006_wallet_ledger.sql");
        CHECK_FALSE(walletSql.empty());
        CHECK_CONTAINS(walletSql, "CREATE TABLE wallet_accounts");
        CHECK_CONTAINS(walletSql, "CREATE TABLE wallet_operations");
        CHECK_CONTAINS(walletSql, "CREATE TABLE wallet_ledger_entries");
        CHECK_CONTAINS(walletSql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_accounts");
        CHECK_CONTAINS(walletSql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_operations");
        CHECK_CONTAINS(walletSql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_ledger_entries");
        CHECK_CONTAINS(walletSql, "CREATE TRIGGER wallet_ledger_entries_chain");
        CHECK_CONTAINS(walletSql, "REFERENCING NEW TABLE AS wallet_inserted_rows");
        CHECK_CONTAINS(walletSql, "DEFERRABLE INITIALLY DEFERRED");
        CHECK_CONTAINS(walletSql, "GRANT SELECT, INSERT ON wallet_accounts, wallet_operations, wallet_ledger_entries TO usapp_api;");
        CHECK_CONTAINS(walletSql, "GRANT UPDATE (lock_token) ON wallet_accounts TO usapp_api;");
        CHECK_CONTAINS(walletSql, "amount BETWEEN -999 AND 999");
        CHECK_CONTAINS(walletSql, "amount BETWEEN -100000 AND 100000");
        CHECK_CONTAINS(walletSql, "balance_after BETWEEN 0 AND 1000000)");
        CHECK_CONTAINS(walletSql, "balance_after BETWEEN 0 AND 1000000000)");
        // 账户不保存可变余额或序号；迁移不写入任何钱包数据；不授予 DELETE / TRUNCATE。
        CHECK_NOT_CONTAINS(walletSql, "last_seq");
        CHECK_NOT_CONTAINS(walletSql, "INSERT INTO wallet");
        CHECK_NOT_CONTAINS(walletSql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(walletSql, "GRANT ALL");
        CHECK_NOT_CONTAINS(walletSql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(walletSql, "DISABLE TRIGGER");

        const std::string initRoles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_FALSE(initRoles.empty());
        // 钱包收窄段必须位于 ALL TABLES 授权之后，否则重新执行 init-roles 会重新放宽。
        CHECK_ORDER(initRoles, "GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO usapp_api;",
                    "DO $wallet_grants$");
        CHECK_CONTAINS(initRoles, "GRANT UPDATE (lock_token) ON public.wallet_accounts TO usapp_api");
    }

    // -----------------------------------------------------------------
    // 2c. 目标库守卫的位置（静态文本断言）：守卫必须先于任何迁移门控、测试用例与写入。
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: db_gated 与 wallet_ledger_test.sql 在任何写入之前检查 current_database()") {
        usapptest::noteStaticAssertion("（文本顺序检查，不是运行时验证；判定逻辑由 unit 中的纯函数用例覆盖）");
        const std::string dbGated =
            readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_FALSE(dbGated.empty());
        const std::string guard = "SELECT current_database()::text AS name";
        CHECK_CONTAINS(dbGated, guard);
        CHECK_CONTAINS(dbGated, "usapptest::isExpectedTestDatabase(currentDatabase)");
        CHECK_ORDER(dbGated, guard, "SELECT version FROM schema_migrations");
        CHECK_ORDER(dbGated, guard, "TEST_CASE(");
        CHECK_ORDER(dbGated, guard, "INSERT INTO");
        CHECK_ORDER(dbGated, guard, "runWalletLedgerDbTests(");
        // 守卫块内的失败路径返回 1，而不是 Skipped（77）。
        const auto guardPos = dbGated.find(guard);
        const auto guardEnd = dbGated.find("目标库守卫通过", guardPos);
        CHECK(guardPos != std::string::npos && guardEnd != std::string::npos);
        if (guardPos != std::string::npos && guardEnd != std::string::npos) {
            const std::string block = dbGated.substr(guardPos, guardEnd - guardPos);
            CHECK_CONTAINS(block, "return 1;");
            CHECK_NOT_CONTAINS(block, "skip(");
            CHECK_NOT_CONTAINS(block, "77");
            CHECK_NOT_CONTAINS(block, "dsn");
        }

        const std::string walletScript =
            readFileContent(repoRoot / "database" / "tests" / "wallet_ledger_test.sql");
        CHECK_FALSE(walletScript.empty());
        const std::string sqlGuard = "IF current_database() IS DISTINCT FROM 'usapp_test' THEN";
        CHECK_CONTAINS(walletScript, sqlGuard);
        CHECK_ORDER(walletScript, "\\set ON_ERROR_STOP on", "BEGIN;");
        CHECK_ORDER(walletScript, "BEGIN;", sqlGuard);
        CHECK_ORDER(walletScript, sqlGuard, "INSERT INTO");
        CHECK_ORDER(walletScript, sqlGuard, "UPDATE wallet_");
        CHECK_ORDER(walletScript, sqlGuard, "DELETE FROM");
        CHECK_ORDER(walletScript, sqlGuard, "TRUNCATE wallet_");
        // 脚本以 ROLLBACK 结束，且从不 COMMIT。
        const auto lastRollback = walletScript.rfind("ROLLBACK;");
        CHECK(lastRollback != std::string::npos);
        if (lastRollback != std::string::npos) {
            CHECK(walletScript.find_first_not_of(" \t\r\n", lastRollback + 9) == std::string::npos);
        }
        CHECK_NOT_CONTAINS(walletScript, "COMMIT;");
    }

    // -----------------------------------------------------------------
    // 3. Drogon 控制器路由与参数绑定静态检查
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: 控制器路径参数绑定与安全过滤") {
        const auto ctrlDir = repoRoot / "services" / "api" / "src" / "controllers";

        // SpaceController
        const std::string spaceCtrl = readFileContent(ctrlDir / "SpaceController.cc");
        CHECK_FALSE(spaceCtrl.empty());
        CHECK_CONTAINS(spaceCtrl, "/api/v1/spaces/{spaceId}");
        CHECK_CONTAINS(spaceCtrl, "std::string spaceId");
        CHECK_NOT_CONTAINS(spaceCtrl, "getParameter(\"spaceId\")");

        // InviteController
        const std::string invCtrl = readFileContent(ctrlDir / "InviteController.cc");
        CHECK_FALSE(invCtrl.empty());
        CHECK_CONTAINS(invCtrl, "/api/v1/spaces/current/invites/{inviteId}");
        CHECK_CONTAINS(invCtrl, "/api/v1/invites/{code}/accept");
        CHECK_CONTAINS(invCtrl, "std::string inviteId");
        CHECK_CONTAINS(invCtrl, "std::string rawCode");
        CHECK_NOT_CONTAINS(invCtrl, "getParameter(\"inviteId\")");
        CHECK_NOT_CONTAINS(invCtrl, "getParameter(\"code\")");

        // AdminController: 守卫过滤器
        const std::string adminCtrl = readFileContent(ctrlDir / "AdminController.cc");
        CHECK_FALSE(adminCtrl.empty());
        CHECK_CONTAINS(adminCtrl, "AdminFilter::classTypeName()");

        // SessionController：PATCH /api/v1/me 必须挂 AuthFilter（Origin + CSRF + 会话）
        const std::string sessionCtrl = readFileContent(ctrlDir / "SessionController.cc");
        CHECK_FALSE(sessionCtrl.empty());
        CHECK_CONTAINS(sessionCtrl,
                       "ADD_METHOD_TO(SessionController::updateMe, \"/api/v1/me\", drogon::Patch,\n"
                       "                  AuthFilter::classTypeName());");
    }

    TEST_CASE("StaticCheck: the journal service suites run exactly once in CTest, in journal_services_db, and still in a direct full run") {
        usapptest::noteStaticAssertion("Text checks only; the runtime case lists are compared in docs/phase3-plan.md Section 26.");
        const std::string cmake = readFileContent(repoRoot / "services" / "api" / "tests" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "ENVIRONMENT \"USAPP_TEST_SEPARATE_MEDIA=1;USAPP_TEST_SEPARATE_JOURNAL=1;USAPP_TEST_SEPARATE_REMINDERS=1\"");
        CHECK_CONTAINS(cmake, "add_test(NAME journal_services_db COMMAND usapp_test_db_gated)");
        CHECK_CONTAINS(cmake, "ENVIRONMENT \"USAPP_TEST_ONLY_JOURNAL_SERVICES=1\"");
        const std::string gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        const auto mode = gated.find("if (std::getenv(\"USAPP_TEST_ONLY_JOURNAL_SERVICES\")) {");
        const auto skip = gated.find("if (!std::getenv(\"USAPP_TEST_SEPARATE_JOURNAL\")) {");
        CHECK(mode != std::string::npos && skip != std::string::npos);
        if (mode != std::string::npos && skip != std::string::npos) {
            // Both places hold the same three suites in the same order.
            const std::string calls = "        usapptest::journalmedia::run(client, dsn);\n        usapptest::journalmemory::run(client, dsn);\n        usapptest::journalpurge::run(client, dsn);\n";
            std::string normalized = gated;
            normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
            const auto m = normalized.find("if (std::getenv(\"USAPP_TEST_ONLY_JOURNAL_SERVICES\")) {");
            const auto k = normalized.find("if (!std::getenv(\"USAPP_TEST_SEPARATE_JOURNAL\")) {");
            CHECK(normalized.compare(normalized.find('\n', m) + 1, calls.size(), calls) == 0);
            CHECK(normalized.compare(normalized.find('\n', k) + 1, calls.size(), calls) == 0);
            // The focused mode runs with the full run's diagnostic sink already on.
            CHECK_ORDER(normalized, "setSourceFailureSink(", "USAPP_TEST_ONLY_JOURNAL_SERVICES");
        }
    }

    TEST_CASE("StaticCheck: the upload capability reads the same predicate as uploads_enabled and never the store") {
        usapptest::noteStaticAssertion("Text checks only; behaviour is in media_capability_db and the live API run.");
        const auto src = repoRoot / "services" / "api" / "src";
        const std::string service = readFileContent(src / "services" / "MediaService.cc");
        const auto begin = service.find("void MediaService::capabilities(");
        const auto end = service.find("void MediaService::listUnattached(", begin);
        CHECK(begin != std::string::npos && end != std::string::npos);
        if (begin != std::string::npos && end != std::string::npos) {
            const std::string body = service.substr(begin, end - begin);
            CHECK_CONTAINS(body, "MediaService::instance().uploadsEnabled()");
            for (const char *absent : {"current()", "store", "INSERT", "UPDATE ", "FOR SHARE", "FOR UPDATE"}) CHECK(body.find(absent) == std::string::npos);
        }
        for (const char *controllerFile : {"MemoryController.cc", "AlbumController.cc"}) {
            CHECK_CONTAINS(readFileContent(src / "controllers" / controllerFile), "MediaService::instance().uploadsEnabled()");
        }
        const std::string controller = readFileContent(src / "controllers" / "MediaController.cc");
        CHECK_CONTAINS(controller, "ADD_METHOD_TO(MediaController::capabilities, \"/api/v1/media/capabilities\", drogon::Get, AuthFilter::classTypeName());");
        CHECK_ORDER(controller, "\"/api/v1/media/capabilities\"", "\"/api/v1/media/{mediaId}\"");
        CHECK_CONTAINS(readFileContent(repoRoot / "contracts" / "openapi.yaml"), "operationId: getMediaCapabilities");
        CHECK_CONTAINS(readFileContent(repoRoot / "services" / "api" / "tests" / "CMakeLists.txt"), "ENVIRONMENT \"USAPP_TEST_ONLY_MEDIA_CAPABILITY=1\"");
    }

    TEST_CASE("StaticCheck: one definition of a first-home photo serves the album check and the unattached list; memory states the same") {
        usapptest::noteStaticAssertion("Text checks only; behaviour is in unattached_db and the live API run.");
        const auto src = repoRoot / "services" / "api" / "src";
        const std::string eligibility = readFileContent(src / "services" / "MediaEligibility.h");
        for (const char *part : {"p.scope = 'everything'", "FROM media_bindings b WHERE b.media_id = ", "usapp_media_pending_reference(", "NOT usapp_media_deleted_now("}) {
            CHECK_CONTAINS(eligibility, part);
        }
        const std::string album = readFileContent(src / "services" / "AlbumService.cc");
        CHECK_CONTAINS(album, "\"WHERE \" USAPP_MEDIA_UNHOMED(\"o\");");
        const std::string media = readFileContent(src / "services" / "MediaService.cc");
        CHECK_CONTAINS(media, "AND o.couple_id = c.couple_id AND \" USAPP_MEDIA_UNHOMED(\"o\")");
        CHECK_CONTAINS(media, "JOIN media_upload_outcomes t ON t.media_id = o.id AND t.outcome = 'ready' ");
        // The memory check's unbound branch keeps the same four conditions.
        const std::string memory = readFileContent(src / "services" / "MemoryService.cc");
        const auto begin = memory.find("const char *kPhotoCheckSql =");
        const auto end = memory.find(";", memory.find("usapp_media_pending_reference(o.id) IS NULL", begin));
        CHECK(begin != std::string::npos && end != std::string::npos);
        if (begin != std::string::npos && end != std::string::npos) {
            const std::string check = memory.substr(begin, end - begin);
            for (const char *part : {"p.scope = 'everything'", "b.media_id IS NULL", "usapp_media_pending_reference(o.id) IS NULL", "NOT usapp_media_deleted_now(o.id)",
                                     "t.outcome = 'ready'", "o.uploader_id = $4::uuid"}) {
                CHECK_CONTAINS(check, part);
            }
        }
        const std::string controller = readFileContent(src / "controllers" / "MediaController.cc");
        CHECK_CONTAINS(controller, "ADD_METHOD_TO(MediaController::listUnattached, \"/api/v1/media/unattached\", drogon::Get, AuthFilter::classTypeName());");
        CHECK_ORDER(controller, "\"/api/v1/media/unattached\"", "\"/api/v1/media/{mediaId}\"");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: listUnattachedMedia");
        const std::string cmake = readFileContent(repoRoot / "services" / "api" / "tests" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "ENVIRONMENT \"USAPP_TEST_ONLY_UNATTACHED=1\"");
    }

    TEST_CASE("StaticCheck: album rename-request history is an authenticated read of summaries, in its own CTest target") {
        usapptest::noteStaticAssertion("Text checks only; behaviour is in album_history_db and the live API run.");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: listAlbumProposals");
        CHECK_CONTAINS(contract, "AlbumProposalListResponse:");
        const std::string controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "AlbumController.cc");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(AlbumController::listProposals, \"/api/v1/albums/{albumId}/proposals\", drogon::Get, AuthFilter::classTypeName());");
        const std::string service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AlbumService.cc");
        const auto begin = service.find("#define USAPP_PROPOSAL_LIST_SQL");
        const auto end = service.find("#undef USAPP_PROPOSAL_LIST_SQL");
        CHECK(begin != std::string::npos && end != std::string::npos && begin < end);
        if (begin != std::string::npos && end != std::string::npos && begin < end) {
            const std::string sql = service.substr(begin, end - begin);
            CHECK_CONTAINS(sql, "JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid");
            CHECK_CONTAINS(sql, "ORDER BY p.created_at DESC, p.id DESC");
            for (const char *absent : {"album_texts", "idempotency_key", "request_fingerprint", "FOR SHARE", "FOR UPDATE", "INSERT", "UPDATE "}) {
                CHECK(sql.find(absent) == std::string::npos);
            }
        }
        const std::string cmake = readFileContent(repoRoot / "services" / "api" / "tests" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "add_test(NAME album_history_db COMMAND usapp_test_db_gated)");
        CHECK_CONTAINS(cmake, "ENVIRONMENT \"USAPP_TEST_ONLY_ALBUM_HISTORY=1\"");
    }

    TEST_CASE("StaticCheck: memory edit-request history is an authenticated read of summaries that hides deleted memories") {
        usapptest::noteStaticAssertion("Text checks only; behaviour is in memory_deletion_db and the live API run.");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: listMemoryProposals");
        CHECK_CONTAINS(contract, "MemoryProposalListResponse:");
        const std::string controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MemoryController.cc");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(MemoryController::listProposals, \"/api/v1/memories/{memoryId}/proposals\", drogon::Get, AuthFilter::classTypeName());");
        const std::string service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        const auto begin = service.find("#define USAPP_PROPOSAL_LIST_SQL");
        const auto end = service.find("#undef USAPP_PROPOSAL_LIST_SQL");
        CHECK(begin != std::string::npos && end != std::string::npos && begin < end);
        if (begin != std::string::npos && end != std::string::npos && begin < end) {
            const std::string sql = service.substr(begin, end - begin);
            CHECK_CONTAINS(sql, "AND NOT usapp_memory_deleted_now(m.id)");
            CHECK_CONTAINS(sql, "ORDER BY p.created_at DESC, p.id DESC");
            // Summaries: no text table, photo table, key or fingerprint, and nothing that writes or locks.
            for (const char *absent : {"memory_texts", "_photos", "idempotency_key", "request_fingerprint", "FOR SHARE", "FOR UPDATE", "INSERT", "UPDATE "}) {
                CHECK(sql.find(absent) == std::string::npos);
            }
        }
    }

    TEST_CASE("StaticCheck: M2-B read route, contract and startup ordering") {
        usapptest::noteStaticAssertion("Text checks only; live startup and routing require owner acceptance.");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        // 0.4.0（M3-B）与 0.5.0（M3-C1）只新增任务操作；钱包读取保持不变。
        CHECK_CONTAINS(contract, "version: 0.67.0");
        CHECK_CONTAINS(contract, "operationId: getWallet");
        CHECK_CONTAINS(contract, "WalletResponse:");
        const std::string controller = readFileContent(
            repoRoot / "services" / "api" / "src" / "controllers" / "WalletController.cc");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(WalletController::getWallet, \"/api/v1/wallet\", drogon::Get,\n"
                       "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller, "ctrl::requireUser");
        CHECK_NOT_CONTAINS(controller, "getParameter");
        CHECK_NOT_CONTAINS(controller, "drogon::Post");
        const std::string cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/WalletController.cc");
        const std::string entry = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        const auto mainPos = entry.find("int main()");
        CHECK(mainPos != std::string::npos);
        if (mainPos != std::string::npos) {
            const auto mainBody = entry.substr(mainPos);
            CHECK_ORDER(mainBody, "waitForDatabase(15)", "checkWalletPrivilegesForStartup(");
            CHECK_ORDER(mainBody, "checkWalletPrivilegesForStartup(", "registerFilter(");
            CHECK_ORDER(mainBody, "checkWalletPrivilegesForStartup(", "addListener(");
            CHECK_ORDER(mainBody, "checkWalletPrivilegesForStartup(", "startScheduler()");
            CHECK_ORDER(mainBody, "checkWalletPrivilegesForStartup(", "drogon::app().run()");
        }
    }

    // -----------------------------------------------------------------
    // 4. 阶段 1 M3-A：0007_tasks_one_off、init-roles 第 8 节、0006 冻结、范围边界（静态文本断言）
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: M3-A 0007 tasks migration, grants, frozen 0006 and scope boundaries") {
        usapptest::noteStaticAssertion("（文本/校验和检查，不是运行时验证；运行时见 db_gated 任务用例）");
        const auto migDir = repoRoot / "database" / "migrations";

        // 0006 已在 usapp_test 与主库应用，字节必须保持不变（与 apply.ps1 的 SHA-256 一致）。
        const std::string walletMigration = readFileContent(migDir / "0006_wallet_ledger.sql");
        CHECK_FALSE(walletMigration.empty());
        CHECK_EQ(usapp::crypto::sha256Hex(walletMigration),
                 std::string("2633e17ea6ff6e664832df84023d1fab8aa9513b4cfe48786c4ca093bc7d543d"));

        const std::string taskSql = readFileContent(migDir / "0007_tasks_one_off.sql");
        CHECK_FALSE(taskSql.empty());
        CHECK_CONTAINS(taskSql, "CREATE TABLE tasks");
        CHECK_CONTAINS(taskSql, "CREATE TABLE task_completions");
        CHECK_CONTAINS(taskSql, "CONSTRAINT tasks_reward_positive CHECK (reward_flowers > 0 OR reward_sunshine > 0)");
        CHECK_CONTAINS(taskSql, "CHECK (reward_flowers BETWEEN 0 AND 999)");
        CHECK_CONTAINS(taskSql, "CHECK (reward_sunshine BETWEEN 0 AND 100000)");
        CHECK_CONTAINS(taskSql, "CONSTRAINT tasks_timezone_supported CHECK (timezone = 'Asia/Shanghai')");
        CHECK_CONTAINS(taskSql, "CONSTRAINT tasks_create_key_unique UNIQUE (creator_id, create_idempotency_key)");
        CHECK_CONTAINS(taskSql, "CONSTRAINT task_completions_operation_unique UNIQUE (operation_id)");
        CHECK_CONTAINS(taskSql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON tasks");
        CHECK_CONTAINS(taskSql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON task_completions");
        CHECK_CONTAINS(taskSql, "CREATE TRIGGER task_completions_guard");
        CHECK_CONTAINS(taskSql, "CREATE FUNCTION usapp_task_settlement_validate(p_operation_id uuid)");
        CHECK_CONTAINS(taskSql, "CREATE CONSTRAINT TRIGGER task_completions_settlement_valid");
        CHECK_CONTAINS(taskSql, "CREATE CONSTRAINT TRIGGER wallet_operations_task_settlement_valid");
        CHECK_CONTAINS(taskSql, "WHEN (NEW.source_type = 'task')");
        CHECK_CONTAINS(taskSql, "CREATE CONSTRAINT TRIGGER wallet_ledger_entries_task_settlement_valid");
        CHECK_CONTAINS(taskSql, "GRANT SELECT, INSERT ON tasks, task_completions TO usapp_api;");
        // 不设发布时截止时间必须在未来的数据库规则（重放先于时间校验，设计第 4.2 节）。
        CHECK_NOT_CONTAINS(taskSql, "deadline_at > now()");
        CHECK_NOT_CONTAINS(taskSql, "deadline_at > clock_timestamp()");
        // 不写入数据、不放宽钱包表、不绕过保护。
        CHECK_NOT_CONTAINS(taskSql, "INSERT INTO");
        CHECK_NOT_CONTAINS(taskSql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(taskSql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(taskSql, "GRANT ALL");
        CHECK_NOT_CONTAINS(taskSql, "ON wallet_accounts TO");
        CHECK_NOT_CONTAINS(taskSql, "DROP ");
        CHECK_NOT_CONTAINS(taskSql, "ALTER TABLE wallet_");
        CHECK_NOT_CONTAINS(taskSql, "DISABLE TRIGGER");
        CHECK_NOT_CONTAINS(taskSql, "SECURITY DEFINER");

        const std::string initRoles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_FALSE(initRoles.empty());
        CHECK_ORDER(initRoles, "GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO usapp_api;",
                    "DO $task_grants$");
        CHECK_ORDER(initRoles, "DO $wallet_grants$", "DO $task_grants$");
        CHECK_CONTAINS(initRoles, "ARRAY['tasks', 'task_completions']");
        CHECK_CONTAINS(initRoles, "EXECUTE format('GRANT SELECT, INSERT ON public.%I TO usapp_api', task_table);");

        const std::string dbGated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(dbGated, "\"0007_tasks_one_off\"");
        CHECK_ORDER(dbGated, "SELECT current_database()::text AS name", "runTaskLedgerDbTests(");

        const std::string script = readFileContent(repoRoot / "database" / "tests" / "task_ledger_test.sql");
        CHECK_FALSE(script.empty());
        const std::string sqlGuard = "IF current_database() IS DISTINCT FROM 'usapp_test' THEN";
        CHECK_ORDER(script, "\\set ON_ERROR_STOP on", "BEGIN;");
        CHECK_ORDER(script, "BEGIN;", sqlGuard);
        CHECK_ORDER(script, sqlGuard, "INSERT INTO");
        CHECK_NOT_CONTAINS(script, "COMMIT;");
        CHECK_NOT_CONTAINS(script, "DISABLE TRIGGER");
        const auto lastRollback = script.rfind("ROLLBACK;");
        CHECK(lastRollback != std::string::npos);
        if (lastRollback != std::string::npos) {
            CHECK(script.find_first_not_of(" \t\r\n", lastRollback + 9) == std::string::npos);
        }

        // M3-A 时的范围边界（不接入启动、不新增任务接口）已由 M3-B 取代，见下一个用例。
        // 0007 已在 usapp_test 应用，字节必须保持不变。
        CHECK_EQ(usapp::crypto::sha256Hex(taskSql),
                 std::string("a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161"));
    }

    // -----------------------------------------------------------------
    // 5. 阶段 1 M3-B：任务发布与读取接口、契约 0.4.0、启动检查顺序与范围边界（静态文本断言）
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: M3-B task routes, contract 0.4.0+, startup ordering and scope boundaries") {
        usapptest::noteStaticAssertion("（文本检查，不是运行时验证；运行时见 db_gated 与负责人验收）");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "version: 0.67.0");  // M8 additive direct redemption
        CHECK_CONTAINS(contract, "  /api/v1/tasks:");
        CHECK_CONTAINS(contract, "  /api/v1/tasks/{taskId}:");
        CHECK_CONTAINS(contract, "operationId: createTask");
        CHECK_CONTAINS(contract, "operationId: listTasks");
        CHECK_CONTAINS(contract, "operationId: getTask");
        CHECK_CONTAINS(contract, "CreateTaskRequest:");
        CHECK_CONTAINS(contract, "TaskListResponse:");
        CHECK_CONTAINS(contract, "name: Idempotency-Key");
        for (const char *code : {"- TASK_NOT_FOUND", "- PARTNER_REQUIRED", "- IDEMPOTENCY_KEY_REUSED",
                                 "- RESULT_UNKNOWN"}) {
            CHECK_CONTAINS(contract, code);
        }
        // M3-B 时“契约不得声明完成接口”的边界已由 M3-C1 取代（见第 6 个用例）。
        // 既有操作保留。
        for (const char *op : {"operationId: getMe", "operationId: updateMe", "operationId: getWallet",
                               "operationId: createSpace", "operationId: acceptInvite"}) {
            CHECK_CONTAINS(contract, op);
        }

        const auto ctrlDir = repoRoot / "services" / "api" / "src" / "controllers";
        const std::string controller = readFileContent(ctrlDir / "TaskController.cc");
        CHECK_FALSE(controller.empty());
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(TaskController::createTask, \"/api/v1/tasks\", drogon::Post,\n"
                       "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(TaskController::listTasks, \"/api/v1/tasks\", drogon::Get,\n"
                       "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(TaskController::getTask, \"/api/v1/tasks/{taskId}\", drogon::Get,\n"
                       "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller, "std::string taskId");
        CHECK_CONTAINS(controller, "ctrl::requireUser");
        CHECK_CONTAINS(controller, "TaskService::validIdempotencyKey");
        CHECK_CONTAINS(controller, "TaskService::parseCreateRequest");
        CHECK_NOT_CONTAINS(controller, "getParameter(\"taskId\")");
        CHECK_NOT_CONTAINS(controller, "getParameter(\"user");
        CHECK_NOT_CONTAINS(controller, "drogon::Put");
        CHECK_NOT_CONTAINS(controller, "drogon::Delete");
        CHECK_NOT_CONTAINS(controller, "drogon::Patch");
        const std::string cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/TaskController.cc");

        const std::string service =
            readFileContent(repoRoot / "services" / "api" / "src" / "services" / "TaskService.cc");
        CHECK_FALSE(service.empty());
        // 先抢占、再分类；截止时间只在新插入分支检查；审计同事务；提交确认后才成功。
        CHECK_CONTAINS(service, "ON CONFLICT (creator_id, create_idempotency_key) DO NOTHING");
        CHECK_CONTAINS(service, "deadline_at > clock_timestamp()) AS deadline_in_future");
        CHECK_CONTAINS(service, "FOR SHARE OF m");
        CHECK_ORDER(service, "void createStepClassifyExisting(", "void createStepClaim(");
        CHECK_ORDER(service, "ctx->commitRequested = true;", "ctx->scope->commit(");
        CHECK_CONTAINS(service, "std::weak_ptr<CreateCtx> weakCtx = ctx;");
        CHECK_CONTAINS(service, "'task.created'");
        // 只发布与读取：不写钱包表、不修改或删除任务、没有完成记录写入。
        CHECK_NOT_CONTAINS(service, "INSERT INTO wallet_");
        CHECK_NOT_CONTAINS(service, "INSERT INTO task_completions");
        CHECK_NOT_CONTAINS(service, "UPDATE tasks");
        CHECK_NOT_CONTAINS(service, "DELETE FROM");
        CHECK_NOT_CONTAINS(service, "OFFSET");
        // 应用自有的错误详情：不转发 ORM 异常文本，也不使用事务回调收到的消息。
        CHECK_NOT_CONTAINS(service, "describePgError");
        CHECK_NOT_CONTAINS(service, "e.base()");
        CHECK_NOT_CONTAINS(service, ".what()");
        CHECK_CONTAINS(service, "[weakCtx](const std::string &) {");
        CHECK_NOT_CONTAINS(controller, ".what()");

        const std::string entry = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        const auto mainPos = entry.find("int main()");
        CHECK(mainPos != std::string::npos);
        if (mainPos != std::string::npos) {
            const auto mainBody = entry.substr(mainPos);
            CHECK_ORDER(mainBody, "checkWalletPrivilegesForStartup(", "checkTaskPrivilegesForStartup(");
            CHECK_ORDER(mainBody, "checkTaskPrivilegesForStartup(", "registerFilter(");
            CHECK_ORDER(mainBody, "checkTaskPrivilegesForStartup(", "addListener(");
            CHECK_ORDER(mainBody, "checkTaskPrivilegesForStartup(", "startScheduler()");
            CHECK_ORDER(mainBody, "checkTaskPrivilegesForStartup(", "drogon::app().run()");
        }

        const std::string dbGated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_ORDER(dbGated, "SELECT current_database()::text AS name", "runTaskServiceDbTests(");
        const std::string dbTests =
            readFileContent(repoRoot / "services" / "api" / "tests" / "TaskServiceDbTests.h");
        CHECK_CONTAINS(dbTests, "@m3-fixture.test");
        CHECK_NOT_CONTAINS(dbTests, "DELETE FROM");
        CHECK_NOT_CONTAINS(dbTests, "DISABLE TRIGGER");
        CHECK_NOT_CONTAINS(dbTests, "DROP ");

        const auto webDir = repoRoot / "apps" / "web" / "src";
        const std::string webApi = readFileContent(webDir / "api" / "tasks.ts");
        CHECK_CONTAINS(webApi, "'Idempotency-Key'");
        CHECK_CONTAINS(webApi, "from './client'");
        const std::string webStore = readFileContent(webDir / "stores" / "tasks.ts");
        CHECK_FALSE(webStore.empty());
        CHECK_NOT_CONTAINS(webStore, "localStorage");
        CHECK_NOT_CONTAINS(webStore, "sessionStorage");
        CHECK_NOT_CONTAINS(webStore, "indexedDB");
        const std::string webSession = readFileContent(webDir / "stores" / "session.ts");
        CHECK_CONTAINS(webSession, "useTasksStore().reset()");
    }

    // -----------------------------------------------------------------
    // 6. 阶段 1 M3-C1：完成接口、契约 0.5.0 与 D32 隔离（只检查有意义的边界）
    // -----------------------------------------------------------------
    TEST_CASE("StaticCheck: M3-C1 completion route, contract 0.5.0 and D32 isolation") {
        usapptest::noteStaticAssertion("（文本检查，不是运行时验证；运行时见 db_gated R1-R9）");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "version: 0.67.0");
        CHECK_CONTAINS(contract, "  /api/v1/tasks/{taskId}/completion:");
        CHECK_CONTAINS(contract, "operationId: completeTask");
        CHECK_CONTAINS(contract, "CompleteTaskResponse:");
        CHECK_CONTAINS(contract, "TaskSettlement:");
        for (const char *code : {"- TASK_NOT_ASSIGNEE", "- TASK_ALREADY_COMPLETED", "- TASK_EXPIRED",
                                 "- WALLET_BALANCE_LIMIT"}) {
            CHECK_CONTAINS(contract, code);
        }
        for (const char *op : {"operationId: createTask", "operationId: listTasks", "operationId: getTask",
                               "operationId: getWallet", "operationId: updateMe", "operationId: acceptInvite"}) {
            CHECK_CONTAINS(contract, op);
        }

        const std::string controller =
            readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "TaskController.cc");
        CHECK_CONTAINS(controller,
                       "ADD_METHOD_TO(TaskController::completeTask, \"/api/v1/tasks/{taskId}/completion\", "
                       "drogon::Post,\n                  AuthFilter::classTypeName());");
        // 控制器的完成调用只传 4 个参数（不带 D32 故障参数）；控制器源码不引用故障枚举。
        CHECK_CONTAINS(controller,
                       "TaskCompletionService::instance().completeTask(\n            userId, taskId, key, [req, callback]");
        CHECK_NOT_CONTAINS(controller, "SettlementFault");

        const std::string header =
            readFileContent(repoRoot / "services" / "api" / "src" / "services" / "TaskCompletionService.h");
        CHECK_CONTAINS(header, "enum class SettlementFault { None, FailAfterFirstEntry };");
        CHECK_CONTAINS(header, "SettlementFault fault = kDefaultFault);");
        CHECK_CONTAINS(header, "static constexpr SettlementFault kDefaultFault = SettlementFault::None;");

        const std::string dbGated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_ORDER(dbGated, "SELECT current_database()::text AS name", "runTaskCompletionDbTests(");
    }

    TEST_CASE("StaticCheck: M4 progress route, migration and database guard") {
        const auto migration = readFileContent(repoRoot / "database" / "migrations" / "0008_task_counting.sql");
        CHECK_CONTAINS(migration, "CREATE TABLE task_progress_events");
        CHECK_CONTAINS(migration, "CREATE TRIGGER task_completions_count_guard");
        CHECK_CONTAINS(migration, "CREATE CONSTRAINT TRIGGER task_progress_events_count_valid");
        CHECK_CONTAINS(migration, "GRANT SELECT, INSERT ON task_progress_events TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: progressTask");
        CHECK_CONTAINS(contract, "ProgressTaskResponse:");
        CHECK_CONTAINS(contract, "TASK_COMPLETION_MODE_INVALID");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "TaskController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(TaskController::progressTask, \"/api/v1/tasks/{taskId}/progress\", drogon::Post,\n"
            "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller, "progressTask(\n            userId, taskId, key, [req, callback]");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0008_task_counting\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runTaskProgressDbTests(");
    }

    TEST_CASE("StaticCheck: M5 read-only daily model and guarded service boundary") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0009_daily_tasks.sql");
        CHECK_CONTAINS(sql, "CREATE VIEW task_read_model WITH (security_invoker = true)");
        CHECK_CONTAINS(sql, "tasks_daily_period_unique UNIQUE (recurrence_id, period_date)");
        CHECK_CONTAINS(sql, "GRANT SELECT ON task_read_model TO usapp_api;");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "TaskController.cc");
        CHECK_CONTAINS(controller, "progressDailyTask(\n                userId, taskId, key, (*json)[\"period_date\"].asString(), [req, callback]");
        CHECK_NOT_CONTAINS(controller, "SettlementFault");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0009_daily_tasks\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runDailyTaskDbTests(");
    }

    TEST_CASE("StaticCheck: M5-B weekly migration keeps the read model SELECT-only and is gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0010_weekly_tasks.sql");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE VIEW task_read_model WITH (security_invoker = true)");
        CHECK_CONTAINS(sql, "GRANT SELECT ON task_read_model TO usapp_api;");
        CHECK_CONTAINS(sql, "OR (span = 7 AND EXTRACT(ISODOW FROM NEW.period_date) <> 1)");
        CHECK_NOT_CONTAINS(sql, "CREATE TABLE");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0010_weekly_tasks\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runWeeklyTaskDbTests(");
    }

    TEST_CASE("StaticCheck: M5-C custom range migration keeps the read model SELECT-only and is gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0011_custom_range_tasks.sql");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE VIEW task_read_model WITH (security_invoker = true)");
        CHECK_CONTAINS(sql, "GRANT SELECT ON task_read_model TO usapp_api;");
        CHECK_CONTAINS(sql, "AND start_local_date <= deadline_local_date))");
        CHECK_CONTAINS(sql, "BEFORE INSERT ON task_progress_events");
        CHECK_NOT_CONTAINS(sql, "CREATE TABLE");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "- TASK_NOT_STARTED");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0011_custom_range_tasks\"");
        CHECK_CONTAINS(gated, "\"0012_custom_action_guard\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runCustomRangeDbTests(");
        const auto guard = readFileContent(repoRoot / "database" / "migrations" / "0012_custom_action_guard.sql");
        CHECK_CONTAINS(guard, "IF t.schedule_type NOT IN ('one_off', 'custom') THEN");
    }

    TEST_CASE("StaticCheck: M6 change tables are append-only, gated, and routes are authenticated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0013_task_changes.sql");
        for (const char *table : {"task_change_proposals", "task_change_decisions", "task_closures"}) {
            CHECK_CONTAINS(sql, std::string("BEFORE UPDATE OR DELETE OR TRUNCATE ON ") + table);
        }
        CHECK_CONTAINS(sql, "GRANT SELECT, INSERT ON task_change_proposals, task_change_decisions, task_closures TO usapp_api;");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "ALTER TABLE tasks");
        CHECK_CONTAINS(sql, "GRANT SELECT ON task_read_model TO usapp_api;");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.task_change_proposals, public.task_change_decisions, public.task_closures");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: proposeTaskChange");
        CHECK_CONTAINS(contract, "operationId: decideTaskChange");
        CHECK_CONTAINS(contract, "- TASK_HAS_PROGRESS");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "TaskController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(TaskController::proposeChange, \"/api/v1/tasks/{taskId}/changes\", drogon::Post,\n"
            "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(TaskController::decideChange, \"/api/v1/tasks/{taskId}/changes/{proposalId}/decision\",\n"
            "                  drogon::Post, AuthFilter::classTypeName());");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0013_task_changes\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runTaskChangeDbTests(");
    }

    TEST_CASE("StaticCheck: M7 reward products are append-only, price-bounded, wallet-free and gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0014_reward_products.sql");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_products");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_product_revisions");
        CHECK_CONTAINS(sql, "CHECK (price_flowers BETWEEN 1 AND 999)");
        CHECK_CONTAINS(sql, "CHECK (creator_id <> owner_id)");
        CHECK_CONTAINS(sql, "GRANT SELECT, INSERT ON reward_products, reward_product_revisions TO usapp_api;");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "wallet_");  // M7 writes no wallet data
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.reward_products, public.reward_product_revisions TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *op : {"operationId: createRewardProduct", "operationId: listRewardProducts",
                               "operationId: getRewardProduct", "operationId: editRewardProduct",
                               "operationId: archiveRewardProduct", "- PRODUCT_VERSION_CONFLICT"}) {
            CHECK_CONTAINS(contract, op);
        }
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "RewardController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(RewardController::createProduct, \"/api/v1/rewards/products\", drogon::Post,\n"
            "                  AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(RewardController::archiveProduct, \"/api/v1/rewards/products/{productId}/archive\", drogon::Post,\n"
            "                  AuthFilter::classTypeName());");
        CHECK_NOT_CONTAINS(controller, "Wallet");
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/RewardController.cc");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0014_reward_products\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runProductDbTests(");
    }

    TEST_CASE("StaticCheck: M8 redemptions are append-only, direct-only, debit-linked and gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0015_reward_redemptions.sql");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemptions");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemption_fulfillments");
        CHECK_CONTAINS(sql, "CHECK (redemption_mode = 'direct')");
        CHECK_CONTAINS(sql, "PERFORM pg_advisory_xact_lock(33007::integer, hashtext(NEW.product_id::text));");
        CHECK_CONTAINS(sql, "AFTER INSERT ON wallet_ledger_entries\n    DEFERRABLE INITIALLY DEFERRED");
        CHECK_CONTAINS(sql, "GRANT SELECT, INSERT ON reward_redemptions, reward_redemption_fulfillments TO usapp_api;");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "sunshine");  // M8 debits flowers only
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles,
            "GRANT SELECT, INSERT ON public.reward_redemptions, public.reward_redemption_fulfillments TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *op : {"operationId: redeemRewardProduct", "operationId: listRedemptions",
                               "operationId: getRedemption", "operationId: fulfillRedemption",
                               "- INSUFFICIENT_BALANCE", "- REDEMPTION_MODE_UNSUPPORTED"}) {
            CHECK_CONTAINS(contract, op);
        }
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "RewardController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(RewardController::redeemProduct, \"/api/v1/rewards/products/{productId}/redemptions\", drogon::Post,\n"
            "                  AuthFilter::classTypeName());");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0015_reward_redemptions\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runRedemptionDbTests(");
    }

    TEST_CASE("StaticCheck: M9 authenticated transitions and guarded workflow") {
        const auto sql=readFileContent(repoRoot / "database/migrations/0016_redemption_workflow.sql");
        CHECK_CONTAINS(sql,"BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemption_events");
        CHECK_CONTAINS(sql,"security_invoker=true");
        CHECK_CONTAINS(sql,"CREATE CONSTRAINT TRIGGER reward_redemption_events_ledger_valid");
        const auto contract=readFileContent(repoRoot / "contracts/openapi.yaml");
        CHECK_CONTAINS(contract,"operationId: transitionRedemption");
        CHECK_CONTAINS(contract,"REDEMPTION_STATE_CONFLICT");
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/RewardController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(RewardController::transitionRedemption, \"/api/v1/rewards/redemptions/{redemptionId}/transitions\",\n"
            "                  drogon::Post, AuthFilter::classTypeName());");
    }


    TEST_CASE("StaticCheck: P2-1 messages are append-only, membership-bound and gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0017_messages.sql");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON messages");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON message_replies");
        CHECK_CONTAINS(sql, "CONSTRAINT message_replies_pkey PRIMARY KEY (message_id)");
        CHECK_CONTAINS(sql, "REFERENCES couple_members (couple_id, user_id)");
        CHECK_CONTAINS(sql, "GRANT SELECT, INSERT ON messages, message_replies TO usapp_api;");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.messages, public.message_replies TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *op : {"operationId: createMessage", "operationId: listMessages", "operationId: getMessage",
                               "operationId: replyMessage", "- MESSAGE_ALREADY_REPLIED"}) {
            CHECK_CONTAINS(contract, op);
        }
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MessageController.cc");
        CHECK_CONTAINS(controller,
            "ADD_METHOD_TO(MessageController::createMessage, \"/api/v1/messages\", drogon::Post, AuthFilter::classTypeName());");
        CHECK_CONTAINS(controller, "\"/api/v1/messages/{messageId}/reply\", drogon::Post,\n                  AuthFilter::classTypeName());");
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/MessageController.cc");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0017_messages\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runMessageDbTests(");
    }

    TEST_CASE("StaticCheck: P2-2 anniversaries are append-only, space-bound and gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0018_anniversaries.sql");
        for (const char *table : {"anniversaries", "anniversary_revisions", "anniversary_edit_proposals",
                                  "anniversary_edit_decisions", "anniversary_home_pins"}) {
            CHECK_CONTAINS(sql, std::string("BEFORE UPDATE OR DELETE OR TRUNCATE ON ") + table + "\n");
        }
        CHECK_CONTAINS(sql, "CONSTRAINT anniversary_home_pins_key_unique UNIQUE (user_id, idempotency_key)");
        CHECK_CONTAINS(sql, "REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT");
        CHECK_CONTAINS(sql, "CONSTRAINT anniversary_edit_decisions_pkey PRIMARY KEY (proposal_id)");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "public.anniversary_edit_decisions, public.anniversary_home_pins TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *op : {"operationId: createAnniversary", "operationId: listAnniversaries",
                               "operationId: getAnniversary", "operationId: editAnniversary",
                               "operationId: proposeAnniversaryEdit", "operationId: decideAnniversaryProposal",
                               "operationId: getHomeAnniversary", "operationId: setHomeAnniversaryPin",
                               "- ANNIVERSARY_PROPOSAL_STALE"}) {
            CHECK_CONTAINS(contract, op);
        }
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/AnniversaryController.cc");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0018_anniversaries\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runAnniversaryDbTests(");
    }

    TEST_CASE("StaticCheck: P2-3b intents are append-only, linked to optional notices and gated") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0020_notification_choices.sql");
        CHECK_CONTAINS(sql, "BEFORE UPDATE OR DELETE OR TRUNCATE ON notification_intents\n");
        CHECK_CONTAINS(sql, "CREATE CONSTRAINT TRIGGER notifications_optional_intent AFTER INSERT ON notifications\n"
                            "    DEFERRABLE INITIALLY DEFERRED");
        CHECK_CONTAINS(sql, "REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT");
        CHECK_CONTAINS(sql, "GRANT SELECT, INSERT ON notification_intents TO usapp_api;");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.notification_intents TO usapp_api;");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: getNotificationChoices", "TaskPublishResponse:",
                                   "OptionalNotificationResult:", "AnniversaryEditRequest:",
                                   // D20 (0.41.0) appends its three events after the P2 list, which stays in place and in order.
                                   "message.replied, reminder.task, reminder.anniversary, memory.created, shared_plan.created, album.photos_added, security.wallet_adjusted, security.redemption_cancelled, security.session_created]"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0020_notification_choices\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runNotificationChoiceDbTests(");
    }

    TEST_CASE("P2-4: immutable task reactions and explicit period contract") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0021_task_reactions.sql");
        CHECK_CONTAINS(sql, "REFERENCES task_completions(task_id)");
        CHECK_CONTAINS(sql, "UQ001"); CHECK_CONTAINS(sql, "UQ002");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "operationId: reactToTask");
        CHECK_CONTAINS(contract, "TaskReactionResponse:");
    }

    TEST_CASE("StaticCheck: P2-5a push subscriptions are append-only, gated and endpoint-free in lists") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0022_push_subscriptions.sql");
        for (const char *table : {"push_subscriptions", "push_subscription_revocations", "push_registration_commands"}) {
            CHECK_CONTAINS(sql, std::string("CREATE TRIGGER ") + table + "_append_only BEFORE UPDATE OR DELETE OR TRUNCATE");
        }
        CHECK_CONTAINS(sql, "pg_advisory_xact_lock(33013::integer, hashtext(NEW.user_id::text))");
        CHECK_CONTAINS(sql, "pg_advisory_xact_lock(33014::integer, hashtext(NEW.endpoint_hash))");
        CHECK_CONTAINS(sql, "IF active_count >= 10 THEN");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "push_deliveries");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "public.push_registration_commands TO usapp_api;");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "PushService.cc");
        // Lock order: session/member FOR SHARE, then the user lock, then the endpoint lock.
        CHECK_ORDER(service, "FOR SHARE OF s\";", "FOR SHARE OF m\";");
        CHECK_CONTAINS(service, "pg_advisory_xact_lock(33013::integer, hashtext($1::uuid::text))");
        CHECK_CONTAINS(service, "pg_advisory_xact_lock(33014::integer, hashtext($1::text))");
        CHECK_NOT_CONTAINS(service, "LOG_INFO");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "PushController.cc");
        CHECK_CONTAINS(controller, "push::workerConfigFromEnv().has_value()");
        CHECK_CONTAINS(controller, "devices.append(PushService::generationJson(g, false));");
        CHECK_CONTAINS(controller, "history.append(PushService::generationJson(g, false));");
        CHECK_CONTAINS(service, "\"push_list\", \"query\", \"cap_invariant\"");
        CHECK_CONTAINS(controller, "\"/api/v1/push/subscriptions/{generationId}/revoke\"");
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/PushController.cc");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: getPushConfig", "operationId: listPushSubscriptions",
                                   "operationId: registerPushSubscription", "operationId: revokePushSubscription",
                                   "- PUSH_DEVICE_LIMIT", "PushRegistrationResponse:"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0022_push_subscriptions\"");
        CHECK_ORDER(gated, "SELECT current_database()::text AS name", "runPushDbTests(");
    }

    TEST_CASE("StaticCheck: P2-5b dispatch is append-only, owner-controlled and commits before sending") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0023_push_dispatch.sql");
        for (const char *table : {"push_dispatch_control", "push_deliveries", "push_attempts",
                                  "push_attempt_observations", "push_attempt_expiries", "push_delivery_outcomes"}) {
            CHECK_CONTAINS(sql, std::string("CREATE TRIGGER ") + table + "_append_only BEFORE UPDATE OR DELETE OR TRUNCATE");
        }
        CHECK_CONTAINS(sql, "(reason = 'gone' AND revoked_by IS NULL)");
        CHECK_CONTAINS(sql, "IF live >= 4 THEN");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "public.push_attempt_observations, public.push_attempt_expiries, public.push_delivery_outcomes\n            TO usapp_api;");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "PushDispatch.cc");
        // The only control write of the application is the automatic pause.
        CHECK_CONTAINS(service, "SELECT 'pause', 'dispatcher', $1");
        CHECK_NOT_CONTAINS(service, "'enable'");
        CHECK_NOT_CONTAINS(service, "audit_logs");
        CHECK_NOT_CONTAINS(service, "wallet_");
        CHECK_NOT_CONTAINS(service, "HttpClient");
        const auto job = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "PushDispatchJob.cc");
        CHECK_CONTAINS(job, "result.afterCommit = [current, claims = std::move(report.claims)]()");
        CHECK_NOT_CONTAINS(job, "[&");
        const auto main = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        CHECK_CONTAINS(main, "std::make_shared<usapp::push::Dispatcher>(std::move(pushPolicy), std::move(pushTransport))");
        CHECK_CONTAINS(main, "if (auto worker = usapp::push::workerConfigFromEnv())");
        CHECK_NOT_CONTAINS(main, "push_dispatch_control");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "enum: [user, superseded, replaced, reowned, gone, null]");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0023_push_dispatch\"");
    }

    TEST_CASE("StaticCheck: P2-5b repairs fence the exact attempt, stale pauses and gone versus registration") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0024_push_dispatch_repairs.sql");
        CHECK_CONTAINS(sql, "NEW.control_revision := latest;");
        CHECK_CONTAINS(sql, "'lease_lost_skipped'");
        CHECK_ORDER(sql, "IF NEW.reason = 'gone' AND FOUND THEN", "pg_advisory_xact_lock(33014::integer, hashtext(g.endpoint_hash))");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "PushDispatch.cc");
        CHECK_CONTAINS(service, "AND a.attempt_no = $5::integer");
        CHECK_CONTAINS(service, "AND a.control_revision = (SELECT max(r.revision) FROM push_dispatch_control r)");
        CHECK_ORDER(service, "recordStep(ctx, kPauseLockSql, \"pause_lock\"", "recordStep(ctx, kPauseSql, \"pause\"");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0024_push_dispatch_repairs\"");
    }

    TEST_CASE("StaticCheck: P3-1b binds in the revision's transaction, locks memory before media, and leaves the worker alone") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0030_memories.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE FUNCTION usapp_media_plan_guard");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE FUNCTION usapp_media_binding_guard");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        // Bindings before photos, in one transaction; the proposal path never binds.
        CHECK_ORDER(service, "kRevisionInsertSql, \"insert_revision\"", "kBindSql, \"bind\"");
        CHECK_ORDER(service, "kBindSql, \"bind\"", "kRevisionPhotosSql, \"insert_photos\"");
        CHECK_ORDER(service, "kApproveRevisionSql, \"insert_revision\"", "kApproveBindSql, \"bind\"");
        CHECK_ORDER(service, "kApproveBindSql, \"bind\"", "kApprovePhotosSql, \"insert_photos\"");
        // Media completion and cleanup never take the memory lock, which is what makes the order one-way.
        const auto media = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        CHECK_NOT_CONTAINS(media, "33020");
        // No mandatory notice. D20 (docs/phase3-plan.md Section 30) supersedes "left for review": a direct create may write one
        // opt-in intent, and the worker knows memory only as a closed target kind.
        CHECK_NOT_CONTAINS(service, "NotificationWriter");
        CHECK_NOT_CONTAINS(service, "object_key");
        const auto worker = readFileContent(repoRoot / "services" / "push-worker" / "src" / "deliver.ts");
        CHECK_CONTAINS(worker, "new Set(['task', 'reward', 'anniversary', 'message', 'memory', 'shared_plan', 'album', 'account'])");
        CHECK_NOT_CONTAINS(worker, "memory_id");
    }

    TEST_CASE("StaticCheck: P3-2a locks album before media, never touches memory locks or the store, and leaves the worker alone") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0031_albums.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "pg_advisory_xact_lock(33020");
        CHECK_NOT_CONTAINS(sql, "CREATE OR REPLACE FUNCTION usapp_media_plan_guard");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE FUNCTION usapp_media_binding_guard");
        CHECK_ORDER(sql, "PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.target_id::text));", "PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));");
        const auto item = sql.substr(sql.find("CREATE FUNCTION usapp_album_item_guard"));
        CHECK_ORDER(item, "pg_advisory_xact_lock(33021::integer", "pg_advisory_xact_lock(33018::integer");
        CHECK_CONTAINS(sql, "WHEN (NEW.target_kind = 'album')");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AlbumService.cc");
        // Album before media; batch, bindings, then items; one transaction.
        CHECK_ORDER(service, "kAlbumLockSql, \"lock\"", "classify(ctx, [ctx]() {\n                          if (ctx->op == Op::Add) addChecks(ctx);");
        CHECK_ORDER(service, "kBatchInsertSql, \"insert_batch\"", "kBindSql, \"bind\"");
        CHECK_ORDER(service, "kBindSql, \"bind\"", "kItemsInsertSql, \"insert_items\"");
        CHECK_NOT_CONTAINS(service, "33020");
        // No album path signs, stores or fetches bytes, and no response carries a key.
        CHECK_NOT_CONTAINS(service, "ObjectStore");
        CHECK_NOT_CONTAINS(service, "signRead");
        CHECK_NOT_CONTAINS(service, "final_key");
        CHECK_NOT_CONTAINS(service, "staging_key");
        CHECK_NOT_CONTAINS(service, "NotificationWriter");
        // Completion and cleanup never take the album lock.
        const auto media = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        CHECK_NOT_CONTAINS(media, "33021");
        const auto memory = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        CHECK_NOT_CONTAINS(memory, "33021");
        CHECK_CONTAINS(media, "usapp_media_shared_now(o.id) AS shared");
        // D20 supersedes "no album notice": one opt-in batch notice; the worker knows album only as a closed target kind.
        const auto worker = readFileContent(repoRoot / "services" / "push-worker" / "src" / "deliver.ts");
        CHECK_NOT_CONTAINS(worker, "album_batch");
        CHECK_NOT_CONTAINS(worker, "album_id");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: createAlbum", "operationId: listAlbums", "operationId: getAlbum", "operationId: listAlbumPhotos",
                                   "operationId: addAlbumPhotos", "operationId: removeAlbumPhoto", "operationId: setAlbumCover", "- ALBUM_NOT_FOUND",
                                   "- ALBUM_PHOTO_UNAVAILABLE", "- ALBUM_PHOTO_NOT_FOUND", "- ALBUM_PHOTO_FORBIDDEN", "- ALBUM_PHOTO_REMOVED"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0031_albums\"");
    }

    TEST_CASE("StaticCheck: P3-2b takes only the album lock, approves after deciding, keeps its stale guard and has no public stale code") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0032_album_rename.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "pg_advisory_xact_lock(33018");
        CHECK_NOT_CONTAINS(sql, "pg_advisory_xact_lock(33020");
        CHECK_NOT_CONTAINS(sql, "usapp_media_binding_guard");
        CHECK_NOT_CONTAINS(sql, "usapp_media_shared_now");
        CHECK_CONTAINS(sql, "CREATE OR REPLACE FUNCTION usapp_album_revision_guard");
        CHECK_CONTAINS(sql, "USAPP_ALBUM_DECISION_STALE");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AlbumService.cc");
        CHECK_ORDER(service, "kDecisionInsertSql, \"insert_decision\"", "kApproveRevisionSql, \"insert_revision\"");
        CHECK_NOT_CONTAINS(service, "NotificationWriter");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: renameAlbum", "operationId: proposeAlbumRename", "operationId: getAlbumProposal", "operationId: decideAlbumProposal",
                                   "- ALBUM_EDIT_FORBIDDEN", "- ALBUM_VERSION_CONFLICT", "- ALBUM_PROPOSAL_PENDING", "- ALBUM_PROPOSAL_NOT_FOUND", "- ALBUM_PROPOSAL_DECIDED"}) {
            CHECK_CONTAINS(contract, needle);
        }
        CHECK_NOT_CONTAINS(contract, "ALBUM_PROPOSAL_STALE");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0032_album_rename\"");
    }

    TEST_CASE("StaticCheck: P3-1a has no real store, keeps READY's key server-side, and sends each copy once") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0029_media.sql");
        CHECK_CONTAINS(sql, "USAPP_MEDIA_OUTCOME_CLEANUP");
        CHECK_CONTAINS(sql, "USAPP_MEDIA_ATTEMPT_CLEANUP");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "bytea");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        // Copy, then the fence, then inspection of the final key; one copy call in the whole service.
        CHECK_ORDER(service, "ctx->store->copy(ctx->stagingKey, ctx->finalKey", "fenceThenInspect(ctx);");
        CHECK_EQ(service.find("store->copy(", service.find("store->copy(") + 1), std::string::npos);
        CHECK_NOT_CONTAINS(service, "signRead(row[\"staging_key\"]");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MediaController.cc");
        CHECK_NOT_CONTAINS(controller, "final_key");
        CHECK_NOT_CONTAINS(controller, "staging");
        // The store is configured only behind the opt-in; the fake never reaches the production binary.
        const auto main = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        CHECK_ORDER(main, "auto ossConfig = usapp::media::ossConfigFromEnv(mediaProblem)", "usapp::MediaService::instance().configure(mediaPolicy");
        CHECK_NOT_CONTAINS(main, "FakeObjectStore");
        CHECK_CONTAINS(main, "std::make_shared<usapp::scheduler::MediaCleanupJob>()");
        // Verification cannot be configured away, and test-only settings have no route from the environment.
        const auto adapter = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "OssObjectStore.cc");
        CHECK_CONTAINS(adapter, "CURLOPT_SSL_VERIFYPEER, 1L");
        CHECK_CONTAINS(adapter, "CURLOPT_SSL_VERIFYHOST, 2L");
        CHECK_NOT_CONTAINS(adapter, "VERIFYPEER, 0");
        CHECK_NOT_CONTAINS(adapter, "VERIFYHOST, 0");
        const auto fromEnv = adapter.substr(adapter.find("std::optional<OssConfig> ossConfigFromEnv"), 900);
        CHECK_NOT_CONTAINS(fromEnv, "testCaFile");
        CHECK_NOT_CONTAINS(fromEnv, "testConnectTo");
        CHECK_CONTAINS(fromEnv, "env::getBool(\"MEDIA_ENABLED\", false)");
        // O8: the rule sits where the URL is signed.
        CHECK_ORDER(service, "variant != \"thumb\" && variant != \"view\" && rows[0][\"uploader_id\"]", "store->signRead(rows[0][\"final_key\"]");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: createMediaUpload", "operationId: completeMediaUpload", "operationId: getMedia",
                                   "operationId: getMediaContent", "- MEDIA_DISABLED", "- MEDIA_STORE_UNAVAILABLE"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0029_media\"");
    }

    TEST_CASE("StaticCheck: D6 stays additive, signs under the lock, and never calls a store from a deletion") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0035_media_deletion.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(sql, "pg_advisory_xact_lock(33020");
        CHECK_CONTAINS(sql, "reason = 'user_deleted' AND deletion_id IS NOT NULL");
        CHECK_CONTAINS(sql, "purge_after = deleted_at + interval '168 hours'");
        // Only the separate reason gets past the bound and reserved refusals.
        const auto plan = sql.substr(sql.find("CREATE OR REPLACE FUNCTION usapp_media_plan_guard"));
        CHECK_ORDER(plan, "IF NEW.reason = 'user_deleted' THEN", "USAPP_MEDIA_PLAN_BOUND");
        const auto cover = sql.substr(sql.find("CREATE OR REPLACE FUNCTION usapp_album_cover_guard"));
        CHECK_ORDER(cover, "pg_advisory_xact_lock(33021::integer", "pg_advisory_xact_lock(33018::integer");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        CHECK_ORDER(service, "kMediaLockSql, \"content_lock\"", "store->signRead(rows[0][\"final_key\"]");
        CHECK_ORDER(service, "kKeyLockSql, \"deletion_key_lock\"", "\"deletion_classify\"");
        CHECK_ORDER(service, "\"deletion_classify\"", "kMediaLockSql, \"deletion_lock\"");
        CHECK_CONTAINS(service, "removed == StoreStatus::Ok || removed == StoreStatus::NotFound");
        const auto start = service.find("void deleteStart(");
        CHECK_NOT_CONTAINS(service.substr(start, service.find("std::string lowerId(") - start), "store");
        const auto album = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AlbumService.cc");
        CHECK_CONTAINS(album, "usapp_media_deleted_now(");
        const auto memory = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        CHECK_CONTAINS(memory, "usapp_media_deleted_now(");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: deleteMedia", "operationId: undoMediaDeletion", "- MEDIA_ALREADY_DELETED", "- MEDIA_UNDO_CLOSED", "- MEDIA_DELETION_NOT_ACTIVE"}) {
            CHECK_CONTAINS(contract, needle);
        }
        CHECK_CONTAINS(contract, "operationId: listUndoableMediaDeletions");
        // The list reads no key and signs nothing, and production never passes the undo fault.
        const auto list = service.substr(service.find("#define USAPP_UNDOABLE_SQL"), service.find("#undef USAPP_UNDOABLE_SQL") - service.find("#define USAPP_UNDOABLE_SQL"));
        CHECK_NOT_CONTAINS(list, "final_key");
        CHECK_NOT_CONTAINS(list, "staging_key");
        for (const char *excluded : {"memory_proposal_photos", "memory_edit_proposals", "pending_reference", "declared_type", "width", "signRead"}) CHECK_NOT_CONTAINS(list, excluded);
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MediaController.cc");
        CHECK_NOT_CONTAINS(controller, "IgnoreUndoPrecheck");
        CHECK_ORDER(controller, "\"/api/v1/media/deletions\", drogon::Get", "\"/api/v1/media/{mediaId}\", drogon::Get");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0035_media_deletion\"");
    }

    TEST_CASE("StaticCheck: 0041: exactly eight guards assign exactly eight times, with no tolerance; Push defers at each stage") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0041_timestamp_guards.sql");
        size_t drops = 0;
        for (size_t at = sql.find("DROP DEFAULT"); at != std::string::npos; at = sql.find("DROP DEFAULT", at + 1)) ++drops;
        CHECK_EQ(drops, 8u);
        for (const char *column : {"messages ALTER COLUMN created_at", "message_replies ALTER COLUMN created_at", "reward_redemptions ALTER COLUMN created_at",
                                   "reward_redemption_events ALTER COLUMN created_at", "reward_redemption_fulfillments ALTER COLUMN fulfilled_at",
                                   "notifications ALTER COLUMN created_at", "notification_intents ALTER COLUMN created_at", "push_deliveries ALTER COLUMN created_at"}) {
            CHECK_CONTAINS(sql, column);
        }
        size_t replaced = 0;
        for (size_t at = sql.find("CREATE OR REPLACE FUNCTION"); at != std::string::npos; at = sql.find("CREATE OR REPLACE FUNCTION", at + 1)) ++replaced;
        CHECK_EQ(replaced, 8u);
        // The only interval is the TTL rule the delivery guard always had. Times are never adjusted by arithmetic.
        size_t intervals = 0;
        for (size_t at = sql.find("interval"); at != std::string::npos; at = sql.find("interval", at + 1)) ++intervals;
        CHECK_EQ(intervals, 1u);
        CHECK_CONTAINS(sql, "notice_at <= clock_timestamp() - interval '24 hours'");
        CHECK_NOT_CONTAINS(sql, "SET search_path");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "UPDATE ");
        // No lock class but the guards' existing ones.
        for (size_t at = sql.find("pg_advisory_xact_lock("); at != std::string::npos; at = sql.find("pg_advisory_xact_lock(", at + 1)) {
            const auto cls = sql.substr(at + 22, 5);
            CHECK(cls == "33007" || cls == "33008" || cls == "33012");
        }
        // Eligibility and TTL come before the delivery's own time.
        const auto delivery = sql.substr(sql.find("FUNCTION usapp_push_delivery_guard"));
        CHECK_ORDER(delivery, "notice_at < activated OR notice_at < ctl.cutover", "NEW.created_at := GREATEST(clock_timestamp(), notice_at)");
        // A decision follows its proposal; an event's time is read after its lock.
        const auto event = sql.substr(sql.find("FUNCTION usapp_redemption_event_guard"));
        CHECK_ORDER(event, "pg_advisory_xact_lock(33008", "NEW.created_at := GREATEST(clock_timestamp(), s.created_at");
        CHECK_CONTAINS(event, "e.id=NEW.proposal_id");

        const auto dispatch = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "PushDispatch.cc");
        // Each within its own statement. kExpiredSql, earlier in the file, has its own "<= clock_timestamp() -".
        const auto fanoutStart = dispatch.find("const char *kFanoutSql");
        const auto fanoutSql = dispatch.substr(fanoutStart, dispatch.find("const char *kPreClaimInactiveSql") - fanoutStart);
        CHECK_CONTAINS(fanoutSql, "\"AND n.created_at <= clock_timestamp() \"");
        const auto claimStart = dispatch.find("const char *kClaimSql");
        const auto claimSql = dispatch.substr(claimStart, dispatch.find("const char *kPreSendSql") - claimStart);
        CHECK_CONTAINS(claimSql, "\"AND n.created_at <= clock_timestamp() AND \"");
        CHECK_CONTAINS(dispatch, "(n.created_at > src.at_time) AS notice_ahead");
        CHECK_CONTAINS(dispatch, "boundedTtlSeconds(row[\"remaining_ttl\"]");
        // The worker's limit is not moved to fit.
        const auto worker = readFileContent(repoRoot / "services" / "push-worker" / "src" / "deliver.ts");
        CHECK_CONTAINS(worker, "(ttl as number) > 86400");
    }

    TEST_CASE("StaticCheck: D6 M1d: an unsafe eraser role is refused everywhere, and nothing alters it; the job and the report share one definition") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0040_memory_eraser_safety.sql");
        const auto apply = readFileContent(repoRoot / "database" / "apply.ps1");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        for (const char *label : {"'superuser'", "'login'", "'createrole'", "'createdb'", "'replication'", "'bypassrls'", "'inherit'", "'has_members'", "'has_memberships'", "'role_settings'", "'default_privileges'"}) {
            CHECK_CONTAINS(sql, label);
            CHECK_CONTAINS(apply, label);
            CHECK_CONTAINS(roles, label);
        }
        for (const auto *text : {&sql, &apply, &roles}) {
            CHECK_NOT_CONTAINS(*text, "ALTER ROLE usapp_memory_eraser");
            CHECK_NOT_CONTAINS(*text, "DROP ROLE usapp_memory_eraser");
            CHECK_NOT_CONTAINS(*text, "GRANT usapp_memory_eraser");
        }
        // The check comes before the migrations, and before InitRoles grants anything.
        CHECK_ORDER(apply, "USAPP_MEMORY_ERASER_UNSAFE", "$pendingCount = 0");
        CHECK_ORDER(roles, "memory_eraser_precheck", "GRANT CONNECT ON DATABASE");
        CHECK_ORDER(sql, "CREATE OR REPLACE FUNCTION public.usapp_memory_erase", "memory_eraser_safety_check");
        const auto erase = sql.substr(sql.find("CREATE OR REPLACE FUNCTION public.usapp_memory_erase"));
        CHECK_ORDER(erase, "USAPP_MEMORY_ERASER_UNSAFE", "pg_advisory_xact_lock");
        const auto job = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "MemoryPurgeJob.cc");
        CHECK_CONTAINS(job, "usapp_memory_purge_due()");
        const auto check = readFileContent(repoRoot / "database" / "memory_purge_enablement_check.sql");
        CHECK_CONTAINS(check, "usapp_memory_purge_due()");
        CHECK_CONTAINS(check, "BEGIN READ ONLY");
        CHECK_NOT_CONTAINS(check, "usapp_memory_erase(");
        CHECK_NOT_CONTAINS(check, "title");
        const auto compose = readFileContent(repoRoot / "deploy" / "docker-compose.yml");
        CHECK_CONTAINS(compose, "MEMORY_PURGE_ENABLED: ${MEMORY_PURGE_ENABLED:-false}");
        const auto policy = readFileContent(repoRoot / "services" / "api" / "src" / "db" / "TaskPrivilegePolicy.cc");
        CHECK_CONTAINS(policy, "usapp_memory_eraser_violations()");
    }

    TEST_CASE("StaticCheck: D6 M1d: one narrow definer function, a fixed path, no PUBLIC; off by default; nothing else may write the text") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0039_memory_text_erasure.sql");
        CHECK_CONTAINS(sql, "SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
        CHECK_CONTAINS(sql, "REVOKE ALL ON FUNCTION public.usapp_memory_erase(uuid) FROM PUBLIC;");
        CHECK_CONTAINS(sql, "ALTER FUNCTION public.usapp_memory_erase(uuid) OWNER TO usapp_memory_eraser;");
        CHECK_CONTAINS(sql, "NOSUPERUSER");
        CHECK_NOT_CONTAINS(sql, "public.shared_plan_events");
        CHECK_NOT_CONTAINS(sql, "UPDATE public.audit_logs");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE ON");
        // Keep new definer functions scoped to their own tables and operations.
        for (const auto &entry : std::filesystem::directory_iterator(repoRoot / "database" / "migrations")) {
            const auto name = entry.path().filename();
            if (name == "0039_memory_text_erasure.sql" || name == "0019_notifications.sql") continue;
            const auto text = readFileContent(entry.path());
            if (name == "0040_memory_eraser_safety.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_ORDER(text, "CREATE OR REPLACE FUNCTION public.usapp_memory_erase", "SECURITY DEFINER");
                continue;
            }
            if (name == "0042_deletion_recovery.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_ORDER(text, "CREATE FUNCTION public.usapp_recovery_closed()", "SECURITY DEFINER");
                CHECK_CONTAINS(text, "SET search_path = pg_catalog, pg_temp");
                continue;
            }
            if (name == "0054_shared_room.sql") {
                const std::string definer = "SECURITY DEFINER";
                const auto readDefiner = text.find(definer);
                const auto saveDefiner = text.find(definer, readDefiner + definer.size());
                const auto shareDefiner = text.find(definer, saveDefiner + definer.size());
                CHECK(readDefiner != std::string::npos);
                CHECK(saveDefiner != std::string::npos);
                CHECK(shareDefiner != std::string::npos);
                CHECK_EQ(shareDefiner, text.rfind(definer));
                CHECK_CONTAINS(text, "usapp_shared_room_read(p_user uuid)\nRETURNS jsonb LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "usapp_shared_room_save(p_user uuid, p_version integer, p_layout jsonb)\nRETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "usapp_shared_room_share(p_user uuid, p_shared boolean)\nRETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "public.usapp_shared_room_share(uuid, boolean) FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "memory_erase");
                continue;
            }
            if (name == "0055_shared_garden.sql") {
                const std::string definer = "SECURITY DEFINER";
                const auto first = text.find(definer);
                const auto second = text.find(definer, first + definer.size());
                CHECK(first != std::string::npos);
                CHECK(second != std::string::npos);
                CHECK_EQ(second, text.rfind(definer));
                CHECK_CONTAINS(text, "LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "ON CONFLICT (couple_id, watering_date, user_id) DO NOTHING");
                CHECK_CONTAINS(text, "FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                continue;
            }
            if (name == "0056_shared_garden_recent.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_CONTAINS(text, "LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "public.usapp_shared_garden_recent(uuid) FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                CHECK_NOT_CONTAINS(text, "INSERT INTO");
                continue;
            }
            if (name == "0057_shared_garden_month.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_CONTAINS(text, "LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "public.usapp_shared_garden_month(uuid,text) FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "INSERT INTO");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                continue;
            }
            if (name == "0058_room_furniture.sql") {
                CHECK_CONTAINS(text, "REVOKE ALL ON TABLE public.room_furniture_catalog, public.room_furniture_purchases");
                CHECK_CONTAINS(text, "CREATE CONSTRAINT TRIGGER room_furniture_debit_guard");
                CHECK_CONTAINS(text, "CREATE CONSTRAINT TRIGGER room_furniture_ledger_guard");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                continue;
            }
            if (name == "0059_room_character_positions.sql") {
                CHECK_CONTAINS(text, "REVOKE ALL ON public.avatar_character_choices, public.room_standings, public.room_position_swaps FROM PUBLIC, usapp_api, usapp_push;");
                CHECK_CONTAINS(text, "RETURNS void LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "RETURNS jsonb LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, public, pg_temp");
                CHECK_CONTAINS(text, "FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                continue;
            }
            if (name == "0060_avatar_bald_style.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_CONTAINS(text, "RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean)");
                CHECK_CONTAINS(text, "FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                continue;
            }
            if (name == "0064_shared_room_free_layout.sql") {
                CHECK_EQ(text.find("SECURITY DEFINER"), text.rfind("SECURITY DEFINER"));
                CHECK_CONTAINS(text, "usapp_shared_room_save(p_user uuid, p_version integer, p_layout jsonb)");
                CHECK_CONTAINS(text, "RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp");
                CHECK_CONTAINS(text, "FROM PUBLIC, usapp_push;");
                CHECK_NOT_CONTAINS(text, "memory_texts");
                CHECK_NOT_CONTAINS(text, "wallet_");
                continue;
            }
            CHECK_NOT_CONTAINS(text, "SECURITY DEFINER");
        }
        const auto appConfig = readFileContent(repoRoot / "services" / "api" / "src" / "config" / "AppConfig.cc");
        CHECK_CONTAINS(appConfig, "env::getBool(\"MEMORY_PURGE_ENABLED\", false)");
        const auto compose = readFileContent(repoRoot / "deploy" / "docker-compose.yml");
        CHECK_CONTAINS(compose, "MEMORY_PURGE_ENABLED: ${MEMORY_PURGE_ENABLED:-false}");
        const auto job = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "MemoryPurgeJob.h");
        CHECK_CONTAINS(job, "kJobKey = \"memory.purge\"");
        const auto main = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        CHECK_CONTAINS(main, "MemoryPurgeJob>(cfg.memoryPurgeEnabled)");
        const auto jobSource = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "MemoryPurgeJob.cc");
        CHECK_CONTAINS(jobSource, "usapp_memory_erase(");
        CHECK_NOT_CONTAINS(jobSource, "UPDATE memory_texts");
        // The service never writes these tables, or calls the function.
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        CHECK_NOT_CONTAINS(service, "UPDATE memory_texts");
        CHECK_NOT_CONTAINS(service, "usapp_memory_erase");
    }

    TEST_CASE("StaticCheck: D6 J2c2: with a journal, every cleanup removal waits for its intent; the fault seam is test-only") {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(repoRoot / "services" / "api" / "src")) {
            if (!entry.is_regular_file() || entry.path().filename() == "MediaService.cc" || entry.path().filename() == "MediaService.h") continue;
            CHECK_NOT_CONTAINS(readFileContent(entry.path()), "setCleanupFaultForTest");
        }
        const auto cc = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        // The resumed attempt and the job branch to the journal before their unjournaled removal.
        CHECK_ORDER(cc, "journaledLateRemoval(ctx->journal, ctx->store, ctx->finalKey", "ctx->store->remove(ctx->finalKey");
        CHECK_ORDER(cc, "runJournaledWork(journal, store, work, 0", "runWork(store, work, 0, [store, pageSize]()");
        const auto block = cc.substr(cc.find("// J2c2: journaled object cleanup"), cc.find("// Pure functions") - cc.find("// J2c2: journaled object cleanup"));
        // Removal only from inside an Effect, after the facts and every key's disposition were read again.
        CHECK_ORDER(block, "kDispositionsSql,", "removeEach(self->store");
        CHECK_ORDER(block, "kKeyFactsSql,", "self->store->remove(key");
        // Never acknowledged as aborted: a refusal cannot prove an earlier attempt removed nothing.
        CHECK_NOT_CONTAINS(block, "EffectState::ProvenNotApplied");
        // No transaction or lock in this path: pooled single statements only.
        CHECK_NOT_CONTAINS(block, "TxScope");
        CHECK_NOT_CONTAINS(block, "pg_advisory");
        const auto journal = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "DeletionJournal.h");
        CHECK_CONTAINS(journal, "operation.action == Action::Cleanup && subject.kind != Kind::Media");
    }

    TEST_CASE("StaticCheck: the intent diagnostic is off in production: only the test binary sets its sink") {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(repoRoot / "services" / "api" / "src")) {
            if (!entry.is_regular_file() || entry.path().filename() == "NotificationIntent.cc" || entry.path().filename() == "NotificationIntent.h") continue;
            CHECK_NOT_CONTAINS(readFileContent(entry.path()), "setSourceFailureSink");
        }
        // No id, key or text in what it can print.
        const auto intent = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "NotificationIntent.cc");
        const auto describe = intent.substr(intent.find("std::string describeSourceFailure("));
        const auto end = describe.find("\n}\n");
        for (const char *field : {"coupleId", "actorId", "recipientId", "messageId", "probedAt", "what()"}) CHECK_NOT_CONTAINS(describe.substr(0, end), field);
    }

    TEST_CASE("StaticCheck: D6 M1c backfills nothing and erases no text; photo locks come before the cycle; linked cycles stay out of the standalone list") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0038_memory_photo_cascade.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "UPDATE memory_texts");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO media_cleanup_plans");
        // The migration writes no row of its own: every INSERT is inside a function body.
        CHECK_ORDER(sql, "CREATE FUNCTION usapp_memory_cascade_photos", "INSERT INTO media_deletions");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        const auto rowsAt = service.find("void writeCycleRows(");
        const auto writeAt = service.find("void writeCycle(");
        const auto endAt = service.find("void insertDeletion(", writeAt);
        CHECK(rowsAt != std::string::npos && writeAt != std::string::npos && endAt != std::string::npos && rowsAt < writeAt && writeAt < endAt);
        if (rowsAt != std::string::npos && writeAt != std::string::npos && endAt != std::string::npos && rowsAt < writeAt && writeAt < endAt) {
            const auto rows = service.substr(rowsAt, writeAt - rowsAt);
            const auto write = service.substr(writeAt, endAt - writeAt);
            CHECK_ORDER(write, "kCascadeLockSql, \"cascade_lock\"", "writeCycleRows(ctx");
            CHECK_ORDER(write, "kCascadeLockSql, \"cascade_lock\"", "kJournalCandidatesSql");
            const auto branchAt = write.find("kJournalCandidatesSql");
            CHECK(branchAt != std::string::npos);
            if (branchAt != std::string::npos) {
                CHECK_ORDER(write.substr(branchAt), "journalPhotosMatch", "writeCycleRows(ctx");
            }
            CHECK_ORDER(rows, "kDeletionInsertSql, \"insert_deletion\"", "kCascadeSql, \"cascade\"");
            CHECK_ORDER(rows, "kDeletionInsertSql, \"insert_deletion\"", "kJournalCascadeSql");
            CHECK_ORDER(rows, "kJournalCascadeSql", "kJournalLinkSql");
            for (const char *external : {"ensureIntent(", "lookupOperation(", "->settle("}) {
                CHECK_NOT_CONTAINS(write, external); CHECK_NOT_CONTAINS(rows, external);
            }
        }
        CHECK_NOT_CONTAINS(readFileContent(repoRoot / "services" / "api" / "src" / "main.cc"), "configureDeletionJournal");
        const auto media = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MediaService.cc");
        const auto undoableAt = media.find("#define USAPP_UNDOABLE_SQL");
        const auto undoable = media.substr(undoableAt, media.find("#undef USAPP_UNDOABLE_SQL") - undoableAt);
        CHECK_CONTAINS(undoable, "memory_deletion_photo_cycles");
        const auto retained = media.substr(media.find("#define USAPP_RETAINED_SQL"), 1200);
        CHECK_CONTAINS(retained, "o.uploader_id = c.user_id");
        CHECK_NOT_CONTAINS(retained, "tx.title");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MemoryController.cc");
        CHECK_NOT_CONTAINS(controller, "FailAfterCascade");
        const auto mediaController = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MediaController.cc");
        CHECK_ORDER(mediaController, "\"/api/v1/media/retained\", drogon::Get", "\"/api/v1/media/{mediaId}\", drogon::Get");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: listRetainedMedia", "- MEDIA_DELETION_LINKED"}) CHECK_CONTAINS(contract, needle);
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0038_memory_photo_cascade\"");
    }

    TEST_CASE("StaticCheck: D6 M1b adds requests only: it erases and purges nothing, and an approval's decision comes before its cycle") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0037_memory_delete_requests.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "UPDATE memory_texts");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO media_cleanup_plans");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO media_deletions");
        CHECK_CONTAINS(sql, "DEFERRABLE INITIALLY DEFERRED");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        const auto target = service.substr(service.find("void requestTarget("));
        CHECK_ORDER(target, "kMemoryLockSql, \"lock\"", "classifyDeleteRequest(ctx, [ctx]() { requestChecks(ctx); })");
        const auto decided = service.substr(service.find("void deleteDecisionTarget("));
        CHECK_ORDER(decided, "kMemoryLockSql, \"lock\"", "classifyDeleteRequest(ctx, [ctx]() { deleteDecisionChecks(ctx); })");
        // A request's facts never read a title, a description or a photo.
        const auto facts = service.substr(service.find("#define USAPP_DELETE_REQUEST_JSON"), 700);
        CHECK_NOT_CONTAINS(facts, "title");
        CHECK_NOT_CONTAINS(facts, "description");
        CHECK_NOT_CONTAINS(facts, "photos");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MemoryController.cc");
        CHECK_NOT_CONTAINS(controller, "FailDeleteApproval");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: requestMemoryDeletion", "operationId: getMemoryDeleteRequest", "operationId: decideMemoryDeleteRequest", "pending_delete_request:"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0037_memory_delete_requests\"");
    }

    TEST_CASE("StaticCheck: D6 M1a stays reversible: it erases and purges nothing, and takes no new lock class") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0036_memory_deletion.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        CHECK_NOT_CONTAINS(sql, "GRANT DELETE");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "UPDATE memory_texts");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO media_cleanup_plans");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO media_deletions");
        CHECK_CONTAINS(sql, "purge_after = deleted_at + interval '168 hours'");
        const auto binding = sql.substr(sql.find("CREATE OR REPLACE FUNCTION usapp_media_binding_guard"));
        CHECK_ORDER(binding, "pg_advisory_xact_lock(33020::integer", "pg_advisory_xact_lock(33018::integer");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        // Classified under the lock, before any state is read; the answer is cycle facts and never reads the memory.
        const auto target = service.substr(service.find("void deletionTarget("));
        CHECK_ORDER(target, "kMemoryLockSql, \"lock\"", "classifyDeletion(ctx, [ctx]() {");
        const auto facts = service.substr(service.find("const char *kDeletionFactsSql"), 500);
        CHECK_NOT_CONTAINS(facts, "title");
        CHECK_NOT_CONTAINS(facts, "photos");
        // Every read of a memory is filtered.
        for (const char *read : {"const char *kGetSql", "const char *kCurrentInTxSql"}) {
            CHECK_CONTAINS(service.substr(service.find(read), 300), "usapp_memory_deleted_now(m.id)");
        }
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "MemoryController.cc");
        CHECK_NOT_CONTAINS(controller, "IgnoreUndoPrecheck");
        CHECK_ORDER(controller, "\"/api/v1/memories/deletions\", drogon::Get", "\"/api/v1/memories/{memoryId}\", drogon::Get");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: deleteMemory", "operationId: undoMemoryDeletion", "operationId: listMemoryDeletions", "- MEMORY_ALREADY_DELETED",
                                   "- MEMORY_UNDO_CLOSED", "- MEMORY_DELETION_NOT_ACTIVE", "\"memory_unavailable\""}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0036_memory_deletion\"");
    }

    TEST_CASE("StaticCheck: generation status comes from both flags; nothing still says a reminder is never generated") {
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "ReminderController.cc");
        CHECK_CONTAINS(controller, "ReminderService::generationEnabled(cfg.remindersEnabled, cfg.schedulerEnabled)");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "required: [reminders, next_cursor, generation_enabled]");
        CHECK_NOT_CONTAINS(contract, "generated in this version");
    }

    TEST_CASE("StaticCheck: P2-6 R5 fences stale reminder pushes additively, with no worker or privilege change") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0028_reminder_push_stale.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT ");
        CHECK_NOT_CONTAINS(sql, "DELETE FROM");
        CHECK_NOT_CONTAINS(sql, "UPDATE ");
        CHECK_NOT_CONTAINS(sql, "pg_advisory");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "PushDispatch.cc");
        CHECK_CONTAINS(service, "usapp_reminder_occurrence_valid(n.id, src.at_time)");
        CHECK_CONTAINS(service, "preflight_missing_0028");
        // The new skip is decided after every existing fence.
        CHECK_ORDER(service, "if (!f.generationActive) return PreSend::Inactive;", "if (f.sourceStale) return PreSend::SourceStale;");
        CHECK_ORDER(service, "if (f.sourceStale) return PreSend::SourceStale;", "if (f.windowUnderASecond) return PreSend::Ttl;");
        // The worker stays stateless and cannot report a source fact.
        for (const auto &file : {repoRoot / "services" / "push-worker" / "src" / "deliver.ts",
                                 repoRoot / "services" / "api" / "src" / "services" / "PushWorkerTransport.cc"}) {
            const auto text = readFileContent(file);
            CHECK_NOT_CONTAINS(text, "source_stale");
            CHECK_NOT_CONTAINS(text, "stale_at");
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0028_reminder_push_stale\"");
    }

    TEST_CASE("StaticCheck: P2-6 R1 stores rules only: no generator, no scheduler registration") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0025_reminders.sql");
        for (const char *table : {"reminder_rules", "reminder_occurrences"}) {
            CHECK_CONTAINS(sql, std::string("CREATE TRIGGER ") + table + "_append_only BEFORE UPDATE OR DELETE OR TRUNCATE");
        }
        CHECK_CONTAINS(sql, "CONSTRAINT notifications_reminder_family");
        CHECK_NOT_CONTAINS(sql, "GRANT UPDATE");
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "public.reminder_rules, public.reminder_occurrences TO usapp_api;");
        const auto main = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        // R2: registered before push.dispatch, off unless configured, and never with the tests' scan clock.
        CHECK_ORDER(main, "scheduler::ReminderJob>(", "scheduler::PushDispatchJob>(");
        CHECK_CONTAINS(main, "reminderPolicy.enabled = cfg.remindersEnabled;");
        CHECK_NOT_CONTAINS(main, "scanAtForTest");
        const auto appConfig = readFileContent(repoRoot / "services" / "api" / "src" / "config" / "AppConfig.cc");
        CHECK_CONTAINS(appConfig, "env::getBool(\"REMINDERS_ENABLED\", false)");
        const auto compose = readFileContent(repoRoot / "deploy" / "docker-compose.yml");
        CHECK_CONTAINS(compose, "REMINDERS_ENABLED: ${REMINDERS_ENABLED:-false}");
        const auto jobHeader = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "ReminderJob.h");
        CHECK_CONTAINS(jobHeader, "kJobKey = \"reminder.generate\"");
        const auto jobSource = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "jobs" / "ReminderJob.cc");
        const auto generation = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "ReminderGeneration.cc");
        // In-app only: no Push reference, no post-commit work and no network in the job.
        for (const auto &text : {jobHeader, jobSource, generation}) {
            CHECK_NOT_CONTAINS(text, "PushDispatch");
            CHECK_NOT_CONTAINS(text, "push_");
            CHECK_NOT_CONTAINS(text, "afterCommit");
            CHECK_NOT_CONTAINS(text, "HttpClient");
        }
        CHECK_ORDER(generation, "kLockSql, \"lock\"", "kInsertSql, \"insert\"");
        // R4: additive, with no privilege change, and the generator honours the one usability definition.
        const auto successorRules = readFileContent(repoRoot / "database" / "migrations" / "0027_reminder_successor_rules.sql");
        CHECK_NOT_CONTAINS(successorRules, "GRANT ");
        CHECK_NOT_CONTAINS(successorRules, "UPDATE reminder_rules");
        CHECK_NOT_CONTAINS(successorRules, "DELETE FROM");
        CHECK_CONTAINS(generation, "usapp_reminder_rule_needs_reset");
        const auto changeService = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "TaskChangeService.cc");
        // The copy runs from the closure's success callback, so a failure in either rolls back both.
        CHECK_ORDER(changeService, "kClosureInsertSql,", "copyReminderRule(ctx);");
        // A job chain continues on a database event-loop thread, which must never wait for a connection.
        const auto scheduler = readFileContent(repoRoot / "services" / "api" / "src" / "scheduler" / "Scheduler.cc");
        CHECK_CONTAINS(scheduler, "db::TxScope::beginAsync(");
        CHECK_NOT_CONTAINS(scheduler, "db::TxScope::begin(");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "ReminderService.cc");
        // The API writes rules only.
        CHECK_NOT_CONTAINS(service, "INSERT INTO reminder_occurrences");
        CHECK_NOT_CONTAINS(service, "INSERT INTO notifications");
        CHECK_ORDER(service, "pg_advisory_xact_lock(33004::integer", "pg_advisory_xact_lock(33017::integer");
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "src/controllers/ReminderController.cc");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: listReminderRules", "operationId: setTaskReminder",
                                   "operationId: setAnniversaryReminder", "- REMINDER_NOT_ASSIGNEE",
                                   "- REMINDER_TARGET_CLOSED", "- REMINDER_TIME_INVALID", "- REMINDER_TIME_PASSED",
                                   "actor_id: { type: [string, 'null'], format: uuid"}) {
            CHECK_CONTAINS(contract, needle);
        }
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0025_reminders\"");
        CHECK_CONTAINS(gated, "\"0026_reminder_next_fire\"");
        CHECK_CONTAINS(gated, "\"0027_reminder_successor_rules\"");
        const auto repair = readFileContent(repoRoot / "database" / "migrations" / "0026_reminder_next_fire.sql");
        CHECK_NOT_CONTAINS(repair, "CREATE TABLE");
        CHECK_NOT_CONTAINS(repair, "GRANT ");
    }

    TEST_CASE("StaticCheck: P2-5c keeps the worker stateless, private and off by default") {
        const auto compose = readFileContent(repoRoot / "deploy" / "docker-compose.yml");
        const size_t start = compose.find("  push-worker:\n");
        CHECK(start != std::string::npos);
        // Only the worker's own service block: up to the next service.
        const size_t end = compose.find("\n  test-runner:", start);
        const auto worker = compose.substr(start, end == std::string::npos ? std::string::npos : end - start);
        CHECK_CONTAINS(worker, "profiles: [\"push\"]");
        CHECK_NOT_CONTAINS(worker, "ports:");
        CHECK_NOT_CONTAINS(worker, "POSTGRES");
        CHECK_NOT_CONTAINS(worker, "postgres");
        CHECK_CONTAINS(compose, "PUSH_ENABLED: ${PUSH_ENABLED:-false}");
        CHECK_CONTAINS(compose, "PUSH_SUBSCRIBE_ENABLED: ${PUSH_SUBSCRIBE_ENABLED:-false}");
        const auto dockerfile = readFileContent(repoRoot / "deploy" / "Dockerfile.push-worker");
        CHECK_CONTAINS(dockerfile, "CMD [\"node\", \"dist/src/index.js\"]");
        CHECK_CONTAINS(dockerfile, "THIRD_PARTY_NOTICES.md");
        const auto package = readFileContent(repoRoot / "services" / "push-worker" / "package.json");
        CHECK_CONTAINS(package, "\"web-push\": \"3.6.7\"");
        for (const char *dependency : {"\"pg\"", "\"postgres\"", "\"bullmq\"", "\"redis\""}) CHECK_NOT_CONTAINS(package, dependency);
        const auto deliver = readFileContent(repoRoot / "services" / "push-worker" / "src" / "deliver.ts");
        CHECK_CONTAINS(deliver, "webPush.generateRequestDetails(");
        CHECK_NOT_CONTAINS(deliver, "sendNotification");
        const auto transport = readFileContent(repoRoot / "services" / "push-worker" / "src" / "transport.ts");
        CHECK_CONTAINS(transport, "new https.Agent({ keepAlive: false, maxSockets: 1 })");
        CHECK_CONTAINS(transport, "rejectUnauthorized: true");
        CHECK_NOT_CONTAINS(transport, "proxyEnv:");
        const auto index = readFileContent(repoRoot / "services" / "push-worker" / "src" / "index.ts");
        CHECK_CONTAINS(index, "transport: createProviderTransport(),");
        CHECK_CONTAINS(index, "assertNoEnvProxy(process.env, process.execArgv);");
    }

    TEST_CASE("StaticCheck: reminder suites retain their focused order and run separately in CTest") {
        const auto cmake = readFileContent(repoRoot / "services" / "api" / "tests" / "CMakeLists.txt");
        CHECK_CONTAINS(cmake, "add_test(NAME reminder_services_db COMMAND usapp_test_db_gated)");
        CHECK_CONTAINS(cmake, "ENVIRONMENT \"USAPP_TEST_ONLY_REMINDER_JOB=1\"");
        auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        gated.erase(std::remove(gated.begin(), gated.end(), '\r'), gated.end());
        const std::string calls = "        usapptest::reminder::runReminderDbTests(client, dsn);\n"
                                  "        usapptest::reminderjob::runReminderJobDbTests(client, dsn);\n"
                                  "        usapptest::remindersuccessor::runReminderSuccessorDbTests(client, dsn);\n"
                                  "        usapptest::remindersource::runReminderSourceDbTests();\n";
        CHECK_CONTAINS(gated, "if (std::getenv(\"USAPP_TEST_ONLY_REMINDER_JOB\")) {\n" + calls);
        CHECK_CONTAINS(gated, "if (!std::getenv(\"USAPP_TEST_SEPARATE_REMINDERS\")) {\n" + calls);
    }

    TEST_CASE("StaticCheck: D20 notifications are additive, default off, never remembered and never from plan conversion") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0043_phase3_notifications.sql");
        CHECK_NOT_CONTAINS(sql, "GRANT ");
        CHECK_NOT_CONTAINS(sql, "DELETE FROM");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO");
        CHECK_CONTAINS(sql, "l.mode = 'create'");
        const auto intent = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "NotificationIntent.cc");
        CHECK_CONTAINS(intent, "remember = c->choice.provided && hasRecipient && remembersChoice(c->operation)");
        CHECK_CONTAINS(intent, "!c->recipientId.empty() && remembersChoice(c->operation)");
        // Plan conversion (createForPlan) never records a creation intent; only a direct create does.
        const auto memory = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "MemoryService.cc");
        CHECK_CONTAINS(memory, "if (ctx->op != Op::Create || !ctx->planId.empty()) {");
        const auto plans = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "SharedPlanController.cc");
        CHECK_CONTAINS(plans, "if (operation == \"create\") {");
        CHECK_NOT_CONTAINS(plans, "intent::Operation::MemoryCreate");
        const auto deliver = readFileContent(repoRoot / "services" / "push-worker" / "src" / "deliver.ts");
        CHECK_CONTAINS(deliver, "'memory', 'shared_plan', 'album'");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0043_phase3_notifications\"");
    }

    TEST_CASE("StaticCheck: Phase 4 B1 avatar catalog is additive, admin-only for writes and wallet-free") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0044_avatar_catalog.sql");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "wallet_operations");
        CHECK_NOT_CONTAINS(sql, "wallet_ledger_entries");
        CHECK_NOT_CONTAINS(sql, "GRANT SELECT, INSERT ON avatar_assets");
        CHECK_CONTAINS(sql, "GRANT SELECT ON avatar_assets TO usapp_api;");
        CHECK_CONTAINS(sql, "CONSTRAINT avatar_items_family_unique UNIQUE (family)");
        // Each admin write classifies its key before reading any mutable catalog state.
        for (const char *fn : {"usapp_avatar_item_create", "usapp_avatar_item_revise", "usapp_avatar_starter_set"}) {
            const auto start = sql.find(std::string("CREATE FUNCTION ") + fn);
            CHECK(start != std::string::npos);
            if (start == std::string::npos) continue;
            const auto body = sql.substr(start, sql.find("$$;", start) - start);
            CHECK_ORDER(body, "usapp_avatar_admin_key(p_admin, p_key)", "INSERT INTO");
            CHECK_CONTAINS(body, "INSERT INTO audit_logs");
        }
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT ON public.avatar_assets TO usapp_api;");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "AvatarController.cc");
        CHECK_CONTAINS(controller, "\"/api/v1/avatar/catalog\", drogon::Get, AuthFilter::classTypeName()");
        CHECK_CONTAINS(controller, "\"/api/v1/avatar\", drogon::Get, AuthFilter::classTypeName()");
        // No user route writes: every Post is an admin route behind AdminFilter.
        size_t at = 0;
        int posts = 0;
        while ((at = controller.find("drogon::Post", at)) != std::string::npos) {
            const auto line = controller.substr(controller.rfind('\n', at), controller.find('\n', at) - controller.rfind('\n', at));
            CHECK_CONTAINS(line, "/api/admin/v1/avatar/");
            CHECK_CONTAINS(line, "AdminFilter::classTypeName()");
            ++posts;
            ++at;
        }
        CHECK_EQ(posts, 3);
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AvatarService.cc");
        CHECK_NOT_CONTAINS(service, "wallet");
        CHECK_NOT_CONTAINS(service, "http://");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0044_avatar_catalog\"");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: getAvatarCatalog", "operationId: getMyAvatar", "operationId: adminCreateAvatarItem",
                                   "operationId: adminReviseAvatarItem", "operationId: adminSetAvatarStarter", "- AVATAR_ITEM_NOT_FOUND",
                                   "- AVATAR_VERSION_CONFLICT", "- AVATAR_ASSET_INVALID", "- AVATAR_STARTER_INVALID"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }

    TEST_CASE("StaticCheck: Phase 4 B2 purchases classify the key first, keep the lock order, spend sunshine only and add one posting type") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0045_avatar_purchases.sql");
        CHECK_CONTAINS(sql, "CHECK (operation_type IN ('TASK_REWARD', 'REWARD_REDEMPTION', 'REDEMPTION_REFUND', 'AVATAR_PURCHASE'))");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "'flower'");
        CHECK_CONTAINS(sql, "CONSTRAINT avatar_purchases_owner_unique UNIQUE (user_id, item_id)");
        const auto start = sql.find("CREATE FUNCTION usapp_avatar_purchase(");
        CHECK(start != std::string::npos);
        if (start != std::string::npos) {
            const auto body = sql.substr(start, sql.find("$$;", start) - start);
            // Key lock, then classification, then membership, the item lock, catalog checks, ownership, the account row, writes.
            CHECK_ORDER(body, "pg_advisory_xact_lock(33030", "WHERE user_id = p_user AND idempotency_key = p_key");
            CHECK_ORDER(body, "WHERE user_id = p_user AND idempotency_key = p_key", "FROM couple_members m");
            CHECK_ORDER(body, "FROM couple_members m", "pg_advisory_xact_lock(33027");
            CHECK_ORDER(body, "pg_advisory_xact_lock(33027", "FROM avatar_item_revisions");
            CHECK_ORDER(body, "FROM avatar_item_revisions", "WHERE user_id = p_user AND item_id = p_item");
            CHECK_ORDER(body, "WHERE user_id = p_user AND item_id = p_item", "asset_type = 'sunshine' FOR UPDATE");
            CHECK_ORDER(body, "asset_type = 'sunshine' FOR UPDATE", "INSERT INTO wallet_operations");
            CHECK_ORDER(body, "INSERT INTO audit_logs", "wallet_ledger_entries_avatar_purchase_valid, wallet_operations_require_entries IMMEDIATE");
            CHECK_ORDER(body, "wallet_operations_require_entries IMMEDIATE", "wallet_operations_require_entries DEFERRED");
            CHECK_NOT_CONTAINS(body, "SET CONSTRAINTS ALL");
        }
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.avatar_purchases TO usapp_api;");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "AvatarPurchaseController.cc");
        CHECK_CONTAINS(controller, "\"/api/v1/avatar/purchases\", drogon::Post, AuthFilter::classTypeName()");
        CHECK_CONTAINS(controller, "\"/api/v1/avatar/owned\", drogon::Get, AuthFilter::classTypeName()");
        CHECK_NOT_CONTAINS(controller, "AdminFilter");
        // The B1 catalog controller still has no user write; the B1 service still never names the wallet.
        const auto catalog = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "AvatarController.cc");
        CHECK_NOT_CONTAINS(catalog, "/api/v1/avatar/purchases");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AvatarPurchaseService.cc");
        CHECK_CONTAINS(service, "tx->rollback([finish, r]() { finish(r); });");
        CHECK_NOT_CONTAINS(service, "equip");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0045_avatar_purchases\"");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: purchaseAvatarItem", "operationId: listOwnedAvatarItems", "- AVATAR_ALREADY_OWNED",
                                   "- AVATAR_ITEM_UNAVAILABLE", "AvatarPurchaseResponse:", "owner_count:"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }

    TEST_CASE("StaticCheck: Phase 4 B3 loadouts lock per user, classify the key first, resolve defaults once and never touch the wallet") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0046_avatar_loadouts.sql");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "wallet_");
        CHECK_NOT_CONTAINS(sql, "33027");
        CHECK_NOT_CONTAINS(sql, "INSERT INTO avatar_purchases");
        CHECK_CONTAINS(sql, "CONSTRAINT avatar_loadout_events_version_unique UNIQUE (user_id, version)");
        const auto start = sql.find("CREATE FUNCTION usapp_avatar_loadout_save(");
        CHECK(start != std::string::npos);
        if (start != std::string::npos) {
            const auto body = sql.substr(start, sql.find("$$;", start) - start);
            CHECK_ORDER(body, "pg_advisory_xact_lock(33031", "WHERE user_id = p_user AND idempotency_key = p_key");
            CHECK_ORDER(body, "WHERE user_id = p_user AND idempotency_key = p_key", "FROM couple_members m");
            CHECK_ORDER(body, "FROM couple_members m", "COALESCE(max(version), 0)");
            CHECK_ORDER(body, "COALESCE(max(version), 0)", "pg_advisory_xact_lock_shared(33028");
            CHECK_ORDER(body, "FROM avatar_starter_configs ORDER BY version DESC LIMIT 1", "usapp_avatar_equippable(p_user, item, slot)");
            CHECK_ORDER(body, "INSERT INTO avatar_loadout_events", "INSERT INTO audit_logs");
        }
        const auto roles = readFileContent(repoRoot / "database" / "init-roles.sql");
        CHECK_CONTAINS(roles, "GRANT SELECT, INSERT ON public.avatar_loadout_events TO usapp_api;");
        const auto controller = readFileContent(repoRoot / "services" / "api" / "src" / "controllers" / "AvatarLoadoutController.cc");
        CHECK_CONTAINS(controller, "\"/api/v1/avatar/loadout\", drogon::Post, AuthFilter::classTypeName()");
        CHECK_NOT_CONTAINS(controller, "user_id");
        const auto service = readFileContent(repoRoot / "services" / "api" / "src" / "services" / "AvatarLoadoutService.cc");
        CHECK_CONTAINS(service, "\"avatar:loadout:v1:\" + std::to_string(request.expectedVersion) + \":\" + requestedJson(request)");
        CHECK_NOT_CONTAINS(service, "wallet");
        CHECK_NOT_CONTAINS(service, "AvatarPurchaseService");
        const auto gated = readFileContent(repoRoot / "services" / "api" / "tests" / "DbGatedTest.cc");
        CHECK_CONTAINS(gated, "\"0046_avatar_loadouts\"");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: saveAvatarLoadout", "AvatarLoadoutRequest:", "enum: [starter_defaults, saved_loadout]",
                                   "- AVATAR_SLOT_INVALID", "- AVATAR_NOT_OWNED", "- AVATAR_DEFAULTS_UNAVAILABLE"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }

    TEST_CASE("Achievements: read-only session scope, explicit immutable activation and five rules") {
        const auto sql = readFileContent(repoRoot / "database" / "migrations" / "0047_achievements.sql");
        CHECK_CONTAINS(sql, "CREATE TABLE achievement_activation");
        CHECK_CONTAINS(sql, "GRANT SELECT ON achievement_activation TO usapp_api");
        CHECK_NOT_CONTAINS(sql, "SECURITY DEFINER");
        CHECK_NOT_CONTAINS(sql, "wallet_operations");
        CHECK_CONTAINS(sql, "t >= cutover");
        CHECK_CONTAINS(sql, "AT TIME ZONE 'Asia/Shanghai'");
        const auto service = readFileContent(repoRoot / "services/api/src/services/AchievementService.cc");
        CHECK_CONTAINS(service, "c.completed_by = $1::uuid");
        CHECK_CONTAINS(service, "c.couple_id = m.couple_id");
        CHECK_NOT_CONTAINS(service, "INSERT");
        const auto controller = readFileContent(repoRoot / "services/api/src/controllers/AchievementController.cc");
        CHECK_CONTAINS(controller, "AuthFilter::classTypeName()");
        CHECK_NOT_CONTAINS(controller, "getParameter");
    }
    TEST_CASE("StaticCheck: E1 export is a session-scoped read-only POST with the recovery lock before one generation statement") {
        const auto controller = readFileContent(repoRoot / "services/api/src/controllers/ExportController.cc");
        CHECK_CONTAINS(controller, "\"/api/v1/me/export\", drogon::Post, AuthFilter::classTypeName()");
        CHECK_CONTAINS(controller, "!body.empty()");
        CHECK_NOT_CONTAINS(controller, "getHeader");  // no Idempotency-Key or any other header selector
        CHECK_NOT_CONTAINS(controller, "getParameter");
        const auto service = readFileContent(repoRoot / "services/api/src/services/ExportService.cc");
        CHECK_ORDER(service, "BEGIN ISOLATION LEVEL READ COMMITTED, READ ONLY", "set_config('lock_timeout'");
        CHECK_ORDER(service, "set_config('lock_timeout'", "SELECT pg_advisory_xact_lock_shared(33026, 0)");
        // Inside begin(): the lock statement's success callback is the only caller of generation.
        CHECK_ORDER(service, "SELECT pg_advisory_xact_lock_shared(33026, 0)", "else generate(ctx);");
        // The slot is released only by release(), called only from the end verification; the ROLLBACK precedes it.
        CHECK_ORDER(service, "\"ROLLBACK\", [ctx](const drogon::orm::Result &) { checkEnded(ctx); }", "ctx->release(mine ? \"ended\" : \"session_replaced\");");
        CHECK_NOT_CONTAINS(service, "TxScope");
        // No backend is ever terminated, and the dedicated clients are never destroyed or replaced after init.
        CHECK_NOT_CONTAINS(service, "pg_terminate_backend");
        CHECK_CONTAINS(service, "std::vector<std::shared_ptr<Slot>> *gSlots = new std::vector<std::shared_ptr<Slot>>();");
        for (const char *teardown : {"delete gSlots", "gSlots->clear", "gSlots->erase", "gSlots->pop_back", "client.reset", "client = nullptr"}) {
            CHECK_NOT_CONTAINS(service, teardown);
        }
        // Identity is the per-export token set before BEGIN; release only from the token-based verification.
        CHECK_ORDER(service, "SELECT set_config('application_name', $1, false) AS tag", "\"BEGIN ISOLATION LEVEL READ COMMITTED, READ ONLY\"");
        CHECK_CONTAINS(service, "gated AS MATERIALIZED (SELECT CASE WHEN (SELECT status FROM verdict) = 'ok' THEN (");
        CHECK_CONTAINS(service, "LIMIT $2::integer + 1");
        for (const char *forbidden : {"INSERT INTO", "UPDATE ", "DELETE FROM", "idempotency_key", "request_fingerprint", "staging_key", "final_key",
                                      "email_canonical", "token_hash", "csrf", "commit("}) {
            CHECK_NOT_CONTAINS(service, forbidden);
        }
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: createUserDataExport", "UserDataExport:", "- EXPORT_TOO_LARGE", "- EXPORT_UNAVAILABLE",
                                   "ExportPhotoSlot:", "withheld: { type: string, const: unavailable }"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }
    TEST_CASE("StaticCheck: admin wallet inspection is AdminFilter-only, read-only, and leaves the caller-only wallet read alone") {
        const auto controller = readFileContent(repoRoot / "services/api/src/controllers/AdminWalletController.cc");
        CHECK_CONTAINS(controller, "\"/api/admin/v1/users/{userId}/wallet\", drogon::Get, AdminFilter::classTypeName()");
        CHECK_CONTAINS(controller, "\"/api/admin/v1/users/{userId}/wallet/ledger\", drogon::Get, AdminFilter::classTypeName()");
        CHECK_CONTAINS(controller, "adminctx::userId(req)");
        CHECK_NOT_CONTAINS(controller, "drogon::Post");
        CHECK_NOT_CONTAINS(controller, "AuthFilter::");
        const auto service = readFileContent(repoRoot / "services/api/src/services/AdminWalletService.cc");
        for (const char *forbidden : {"INSERT INTO", "UPDATE ", "DELETE FROM", "idempotency_key", "request_fingerprint", "lock_token", "actor_user_id"}) {
            CHECK_NOT_CONTAINS(service, forbidden);
        }
        // Every ledger read is filtered by the selected user and asset, ordered by the per-account sequence.
        CHECK_CONTAINS(service, "WHERE e.user_id = $1::uuid AND e.asset_type = $2 AND ($3::bigint = 0 OR e.seq < $3::bigint)");
        CHECK_CONTAINS(service, "ORDER BY e.seq DESC LIMIT $4::integer + 1");
        // The public caller-only wallet read takes no user selector.
        const auto wallet = readFileContent(repoRoot / "services/api/src/controllers/WalletController.cc");
        CHECK_NOT_CONTAINS(wallet, "getParameter");
        CHECK_NOT_CONTAINS(wallet, "{userId}");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: adminGetUserWallet", "operationId: adminListUserLedger", "AdminDecimal:", "AdminLedgerPage:"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }
    TEST_CASE("StaticCheck: admin shared-plan reads project authoritative facts without memory references or writes") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminSharedPlanController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"adminctx::userId");CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminSharedPlanService.cc");
        CHECK_CONTAINS(service,"usapp_shared_plan_read(p.id)");CHECK_CONTAINS(service,"AS MATERIALIZED");CHECK_CONTAINS(service,"p.couple_id=$1::uuid");CHECK_CONTAINS(service,"e.couple_id=p.couple_id");CHECK_CONTAINS(service,"ORDER BY e.seq DESC LIMIT 50");
        CHECK_NOT_CONTAINS(service,"e.request");CHECK_NOT_CONTAINS(service,"shared_plan_memory_links");CHECK_NOT_CONTAINS(service,"FROM memories");CHECK_NOT_CONTAINS(service,"INSERT INTO");CHECK_NOT_CONTAINS(service,"UPDATE ");CHECK_NOT_CONTAINS(service,"DELETE FROM");
    }

    TEST_CASE("StaticCheck: admin notification inspection is bounded, recipient-scoped and never sends or marks read") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminNotificationController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"adminctx::userId");CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminNotificationService.cc");
        CHECK_CONTAINS(service,"n.recipient_id=$1::uuid");CHECK_CONTAINS(service,"z.recipient_id=n.recipient_id AND z.couple_id=n.couple_id");
        CHECK_CONTAINS(service,"LIMIT 5");CHECK_CONTAINS(service,"ORDER BY p.attempt_no DESC LIMIT 1");
        CHECK_NOT_CONTAINS(service,"push_subscriptions");CHECK_NOT_CONTAINS(service,"INSERT INTO");CHECK_NOT_CONTAINS(service,"UPDATE ");CHECK_NOT_CONTAINS(service,"DELETE FROM");
        CHECK_NOT_CONTAINS(service,"markRead(");CHECK_NOT_CONTAINS(service,".send(");CHECK_NOT_CONTAINS(service,"n.target_id");
    }

    TEST_CASE("StaticCheck: admin user-session inspection cannot mutate or expose session secrets") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminUserSessionController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");
        CHECK_CONTAINS(controller,"adminctx::userId");
        CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminUserSessionService.cc");
        CHECK_CONTAINS(service,"s.user_id=$1::uuid");
        CHECK_CONTAINS(service,"s.expires_at<=statement_timestamp()");
        CHECK_NOT_CONTAINS(service,"s.token_hash");
        CHECK_NOT_CONTAINS(service,"s.csrf_token_hash");
        CHECK_NOT_CONTAINS(service,"UPDATE ");
        CHECK_NOT_CONTAINS(service,"DELETE FROM");
        CHECK_NOT_CONTAINS(service,"INSERT INTO");
    }

    TEST_CASE("StaticCheck: admin redemption inspection is read-only and scoped to the selected couple") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminRedemptionController.cc");
        CHECK_CONTAINS(controller, "AdminFilter::classTypeName()");
        CHECK_CONTAINS(controller, "adminctx::userId");
        CHECK_NOT_CONTAINS(controller, "drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminRedemptionService.cc");
        CHECK_CONTAINS(service, "reward_redemption_state");
        CHECK_CONTAINS(service, "t.couple_id = $1::uuid");
        CHECK_CONTAINS(service, "e.couple_id=t.couple_id");
        CHECK_NOT_CONTAINS(service, "INSERT INTO");
        CHECK_NOT_CONTAINS(service, "UPDATE ");
        CHECK_NOT_CONTAINS(service, "DELETE FROM");
        CHECK_NOT_CONTAINS(service, "t.idempotency_key");
        CHECK_NOT_CONTAINS(service, "t.operation_id");
    }

    TEST_CASE("StaticCheck: admin task inspection is AdminFilter-only, read-only, space-filtered, and shares the public task state") {
        const auto controller = readFileContent(repoRoot / "services/api/src/controllers/AdminTaskController.cc");
        CHECK_CONTAINS(controller, "\"/api/admin/v1/couples/{coupleId}/tasks\", drogon::Get, AdminFilter::classTypeName()");
        CHECK_CONTAINS(controller, "\"/api/admin/v1/couples/{coupleId}/tasks/{taskId}\", drogon::Get,");
        CHECK_CONTAINS(controller, "adminctx::userId(req)");
        CHECK_NOT_CONTAINS(controller, "drogon::Post");
        CHECK_NOT_CONTAINS(controller, "AuthFilter::");
        const auto service = readFileContent(repoRoot / "services/api/src/services/AdminTaskService.cc");
        for (const char *forbidden : {"INSERT INTO", "UPDATE ", "DELETE FROM", "idempotency_key", "request_fingerprint", "create_fingerprint",
                                      "'operation_id'", "task_read_model", "interval '30 days'"}) {
            CHECK_NOT_CONTAINS(service, forbidden);
        }
        // Every list and detail read is bound to the selected couple; the detail to the selected task within it.
        CHECK_CONTAINS(service, "WHERE t.couple_id = $1::uuid AND \" USAPP_ADMIN_TASK_KIND_FILTER");
        CHECK_CONTAINS(service, "WITH tk AS (SELECT t.* FROM tasks t WHERE t.id = $2::uuid AND t.couple_id = $1::uuid)");
        CHECK_CONTAINS(service, "ORDER BY t.created_at DESC, t.id DESC LIMIT $5::integer + 1");
        // One status expression for public and admin reads.
        CHECK_CONTAINS(service, "'status', \" USAPP_TASK_STATUS_CASE");
        const auto task = readFileContent(repoRoot / "services/api/src/services/TaskService.cc");
        CHECK_CONTAINS(task, "USAPP_TASK_STATUS_CASE \" AS status, \"");
        CHECK_CONTAINS(task, "#define USAPP_ROW_CLOSURE USAPP_TASK_ROW_CLOSURE");
        CHECK_CONTAINS(task, "c.completed_at > now() - interval '30 days'");  // the public history window is unchanged
        const auto state = readFileContent(repoRoot / "services/api/src/services/TaskStateSql.h");
        CHECK_CONTAINS(state, "\"     WHEN t.deadline_at IS NOT NULL AND t.deadline_at <= now() THEN 'expired' \"");
        const auto contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        for (const char *needle : {"operationId: adminListCoupleTasks", "operationId: adminGetCoupleTask", "AdminTaskPage:", "AdminTaskDetail:"}) {
            CHECK_CONTAINS(contract, needle);
        }
    }
    TEST_CASE("StaticCheck: admin memory inspection is read-only, parent-scoped and withholds deleted photo ids across snapshots") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminMemoryController.cc");
        CHECK_CONTAINS(controller,"adminctx::userId(req)");CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminMemoryService.cc");
        CHECK_CONTAINS(service,"NOT usapp_memory_deleted_now(id)");CHECK_CONTAINS(service,"ph.couple_id=m.couple_id");CHECK_CONTAINS(service,"'position',ph.position,'available',false");
        for(const char *word:{"INSERT INTO","UPDATE ","DELETE FROM","idempotency_key","request_fingerprint","signedUrl"}) CHECK_NOT_CONTAINS(service,word);
    }
    TEST_CASE("StaticCheck: admin anniversary reads require independent admin auth and exclude private operational fields") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminAnniversaryController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"adminctx::userId(req)");CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminAnniversaryService.cc");
        CHECK_CONTAINS(service,"anniversaries WHERE couple_id=$1::uuid AND id=$2::uuid");CHECK_CONTAINS(service,"p.couple_id=a.couple_id");CHECK_CONTAINS(service,"d.couple_id=p.couple_id");
        for(const char *word:{"INSERT INTO","UPDATE ","DELETE FROM","idempotency_key","request_fingerprint","anniversary_home_pins","usapp_recovery_closed"})CHECK_NOT_CONTAINS(service,word);
    }
    TEST_CASE("StaticCheck: admin product reads are scoped and cannot change catalog, redemption or balances") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminProductController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"adminctx::userId(req)");CHECK_NOT_CONTAINS(controller,"drogon::Post");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminProductService.cc");
        CHECK_CONTAINS(service,"reward_products WHERE couple_id=$1::uuid AND id=$2::uuid");CHECK_CONTAINS(service,"x.couple_id=p.couple_id");CHECK_CONTAINS(service,"r.price_flowers::text");CHECK_CONTAINS(service,"ORDER BY x.version DESC LIMIT 1");
        for(const char *word:{"INSERT INTO","UPDATE ","DELETE FROM","idempotency_key","request_fingerprint","wallet_operations","usapp_recovery_closed"})CHECK_NOT_CONTAINS(service,word);
    }
    TEST_CASE("StaticCheck: admin correction uses true admin actor, key-first SQL, legacy bounded entries and confirmed commit") {
        const auto sql=readFileContent(repoRoot / "database/migrations/0049_admin_wallet_adjustments.sql");
        CHECK_ORDER(sql,"WHERE admin_id=p_admin AND idempotency_key=p_key","SELECT m.couple_id INTO space");
        CHECK_CONTAINS(sql,"actor_user_id IS NULL");CHECK_CONTAINS(sql,"sign(left_over)*least(abs(left_over),999)");CHECK_CONTAINS(sql,"FOR UPDATE");
        CHECK_CONTAINS(sql,"wallet_operations_require_entries IMMEDIATE");CHECK_CONTAINS(sql,"audit_admin_adjustment_immutable");
        CHECK_NOT_CONTAINS(sql,"SECURITY DEFINER");CHECK_NOT_CONTAINS(sql,"DROP CONSTRAINT wallet_ledger_entries_amount_bound");
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminWalletAdjustmentController.cc");CHECK_CONTAINS(controller,"adminctx::userId(req)");CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"validIdempotencyKey");
        const auto service=readFileContent(repoRoot / "services/api/src/services/AdminWalletAdjustmentService.cc");CHECK_CONTAINS(service,"tx->commit(");CHECK_CONTAINS(service,"DiscardConfirmedCommitReply");
    }
    TEST_CASE("StaticCheck: admin cancellation preserves member history and uses reviewed state plus original refund") {
        const auto sql=readFileContent(repoRoot / "database/migrations/0051_admin_redemption_cancellations.sql");
        const auto command=sql.substr(sql.find("CREATE FUNCTION usapp_admin_redemption_cancel("));
        CHECK_ORDER(command,"WHERE admin_id=p_admin AND idempotency_key=p_key","SELECT * INTO r FROM reward_redemption_state");
        CHECK_CONTAINS(sql,"pg_advisory_xact_lock(33008");CHECK_CONTAINS(sql,"amount:=r.price_flowers");CHECK_CONTAINS(sql,"actor_admin_id");
        CHECK_NOT_CONTAINS(sql,"INSERT INTO reward_redemption_events");CHECK_NOT_CONTAINS(sql,"SECURITY DEFINER");CHECK_CONTAINS(sql,"admin.redemption.cancelled");
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminRedemptionCancellationController.cc");
        CHECK_CONTAINS(controller,"AdminFilter::classTypeName()");CHECK_CONTAINS(controller,"adminctx::userId(req)");CHECK_CONTAINS(controller,"validIdempotencyKey");
    }
    TEST_CASE("StaticCheck: personal security notices are mandatory, source-validated and contain no credential or private reason payload") {
        const auto sql=readFileContent(repoRoot / "database/migrations/0052_personal_security_notifications.sql");
        CHECK_CONTAINS(sql,"CREATE TRIGGER sessions_login_capture");CHECK_CONTAINS(sql,"CREATE CONSTRAINT TRIGGER sessions_login_notice_required");
        CHECK_CONTAINS(sql,"security.wallet_adjusted");CHECK_CONTAINS(sql,"security.redemption_cancelled");CHECK_CONTAINS(sql,"security.session_created");
        CHECK_NOT_CONTAINS(sql,"SECURITY DEFINER");CHECK_NOT_CONTAINS(sql,"INSERT INTO notification_intents");
        const auto service=readFileContent(repoRoot / "services/api/src/services/NotificationService.cc");CHECK_CONTAINS(service,"n.recipient_id=$1::uuid");CHECK_CONTAINS(service,"n.login_event_id IS NOT NULL");
    }
    TEST_CASE("StaticCheck: memory automatic decision time follows semantic checks and preserves explicit strict times") {
        const auto sql=readFileContent(repoRoot / "database/migrations/0053_memory_decision_timestamp.sql");
        CHECK_CONTAINS(sql,"ALTER TABLE memory_edit_decisions ALTER COLUMN decided_at DROP DEFAULT");
        CHECK_ORDER(sql,"pg_advisory_xact_lock(33020::integer", "NEW.decided_at := GREATEST(clock_timestamp(), p.created_at)");
        CHECK_ORDER(sql,"USAPP_MEMORY_DELETED", "NEW.decided_at := GREATEST(clock_timestamp(), p.created_at)");
        CHECK_ORDER(sql,"USAPP_MEMORY_DECISION_STALE", "NEW.decided_at := GREATEST(clock_timestamp(), p.created_at)");
        CHECK_CONTAINS(sql,"ELSIF NEW.decided_at > clock_timestamp() OR NEW.decided_at < p.created_at THEN");
        CHECK_CONTAINS(sql,"USAPP_MEMORY_DECISION_ACTOR");CHECK_CONTAINS(sql,"USAPP_MEMORY_DECISION_TARGET");
        CHECK_NOT_CONTAINS(sql,"SECURITY DEFINER");CHECK_NOT_CONTAINS(sql,"UPDATE memory_edit_decisions");
    }
    TEST_CASE("StaticCheck: admin operational snapshot is authenticated and uses an explicit read-only allowlist") {
        const auto controller=readFileContent(repoRoot / "services/api/src/controllers/AdminController.cc");
        const auto start=controller.find("void systemStatus(");
        const auto snapshot=controller.substr(start,controller.find("void bootstrap(",start)-start);
        CHECK_CONTAINS(snapshot,"adminctx::userId(req).empty()");
        CHECK_CONTAINS(snapshot,"FROM schema_migrations");CHECK_CONTAINS(snapshot,"uploadsEnabled()");
        CHECK_NOT_CONTAINS(snapshot,"postgresPassword");CHECK_NOT_CONTAINS(snapshot,"smtpPassword");
        CHECK_NOT_CONTAINS(snapshot,"getenv(");CHECK_NOT_CONTAINS(snapshot,"INSERT ");CHECK_NOT_CONTAINS(snapshot,"UPDATE ");
    }
    TEST_CASE("StaticCheck: deletion startup refusal precedes storage, listeners and scheduler without readiness overrides") {
        const auto main=readFileContent(repoRoot / "services/api/src/main.cc");
        const auto gate=main.substr(main.find("// J3c refusal gate:"));
        CHECK_ORDER(gate,"deletionStartup.allowed()","MediaService::instance().configure");
        CHECK_ORDER(gate,"deletionStartup.allowed()","addListener(");
        CHECK_ORDER(gate,"deletionStartup.allowed()","if (!startScheduler())");
        const auto check=readFileContent(repoRoot / "services/api/src/services/DeletionStartupCheck.cc");
        CHECK_CONTAINS(check,"public.usapp_recovery_closed()");CHECK_CONTAINS(check,"State::ProviderUnavailable");
        CHECK_NOT_CONTAINS(check,"getenv(");CHECK_NOT_CONTAINS(check,"INSERT ");CHECK_NOT_CONTAINS(check,"UPDATE ");
    }
    return usapptest::finish();
}
