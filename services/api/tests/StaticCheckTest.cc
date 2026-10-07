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

    return usapptest::finish();
}
