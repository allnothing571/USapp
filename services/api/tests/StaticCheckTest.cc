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

    TEST_CASE("StaticCheck: M2-B read route, contract and startup ordering") {
        usapptest::noteStaticAssertion("Text checks only; live startup and routing require owner acceptance.");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "version: 0.3.0");
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

        // M3-A 范围边界：不接入启动、不改契约版本、不新增任务接口。
        const std::string entry = readFileContent(repoRoot / "services" / "api" / "src" / "main.cc");
        CHECK_NOT_CONTAINS(entry, "TaskPrivilegePolicy");
        const std::string contract = readFileContent(repoRoot / "contracts" / "openapi.yaml");
        CHECK_CONTAINS(contract, "version: 0.3.0");
        CHECK_NOT_CONTAINS(contract, "/api/v1/tasks");
    }

    return usapptest::finish();
}
