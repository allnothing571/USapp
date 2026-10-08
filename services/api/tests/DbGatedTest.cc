// =====================================================================
// services/api/tests/DbGatedTest.cc
//
// 目标：usapp_test_db_gated
// 数据库门控集成测试：
//   1. 门控机制：必须配置 USAPP_TEST_DSN，缺少时明确提示并退出 77 (SKIP)。
//   2. 生产防误触：DSN 包含 prod 且不含 test 时直接退出 1 (FAIL)。
//   3. 就绪校验：SELECT 1 探测与 0000..0005 迁移完整性校验，失败退出 1 (FAIL)。
//   4. 阶段 0 数据库状态测试：
//      - 预认证会话与正式会话生命周期（Session Fixation 防护、撤销）
//      - 空间创建与成员槽位约束（Slot 1/2、容量上限、单一空间）
//      - 邀请生成与原子消费（单次消费、重复消费拒绝）
//      - 调度器事务级 Advisory Lock 互斥与回滚自动释放
//      - make_interval 参数类型绑定与管理员会话 TTL 回归测试
//      - 管理后台列表分页参数绑定（int limit / long long offset）回归测试
//      - 真实 SpaceService 邀请码生成与接受 (slot / max_uses smallint 参数绑定与消费) 回归测试
//      - AdminFilter 真实管理员会话校验（无效 / 过期 / 撤销拒绝，有效放行，CSRF 绑定）回归测试
//   5. 阶段 1 M1：真实 ProfileService 昵称更新（只改调用者、同事务审计、非法输入不修改）
//   6. 阶段 1 M1：ProfileService 结束后释放请求回调（无 ctx -> TxScope -> onError_ -> ctx 环）
//   7. 阶段 1 M2-A：钱包流水账（WalletLedgerDbTests.h）：有效权限矩阵、触发器目录、
//      链式校验与篡改、上限、结构与成员一致性、延迟完整性（立即与提交时）、幂等、
//      usapp_api 权限结果、账户创建竞争。从不提交钱包行。
//
// 依赖：usapp_core、PostgreSQL 17 测试库。
// =====================================================================

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AdminFilterProbe.h"
#include "TestDatabaseGuard.h"
#include "TestSupport.h"
#include "WalletLedgerDbTests.h"
#include "WalletReadDbTests.h"
#include "core/Crypto.h"
#include "core/Errors.h"
#include "db/Db.h"
#include "http/AdminFilter.h"
#include "services/AdminAuthService.h"
#include "services/ProfileService.h"
#include "services/SpaceService.h"

int main() {
    using namespace usapp;

    // -----------------------------------------------------------------
    // 0. 数据库环境门控校验
    // -----------------------------------------------------------------
    const char *dsnEnv = std::getenv("USAPP_TEST_DSN");
    if (!dsnEnv || std::string(dsnEnv).empty()) {
        return usapptest::skip(
            "未检测到 USAPP_TEST_DSN 环境变量，跳过数据库集成测试。"
            "如需运行数据库测试，请设置测试数据库连接串，例如：\n"
            "  $env:USAPP_TEST_DSN=\"postgresql://usapp_api:secret@127.0.0.1:5432/usapp_test\"",
            77);
    }

    std::string dsn(dsnEnv);
    // 安全防护：禁止以任何形式连向生产库
    if (dsn.find("prod") != std::string::npos && dsn.find("test") == std::string::npos) {
        std::cerr << "[SECURITY ALERT] USAPP_TEST_DSN 包含 'prod' 关键字，拒绝针对疑似生产库运行测试！\n";
        return 1;
    }

    // 初始化连接池并设置有限超时（防止同步查询无时间限制阻塞）
    constexpr double kDbTimeoutSeconds = 5.0;
    try {
        db::Pool::instance().init(dsn, 2);
        db::Pool::instance().client()->setTimeout(kDbTimeoutSeconds);
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] 数据库连接池初始化失败: " << e.what() << "\n";
        return 1;
    }

    // 基础连通性探测
    try {
        db::Pool::instance().client()->execSqlSync("SELECT 1");
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] 数据库探测失败 (SELECT 1 执行异常): " << e.what() << "\n";
        return 1;
    }

    // 目标库守卫：必须在任何夹具写入与测试用例（含既有用例）之前完成。
    // 依据服务器返回的 current_database()，要求与 usapp_test 完全相等；不依赖连接串子串。
    // 不符或查询失败一律以失败退出（返回 1，不是 77/Skipped）。只输出安全的库名与原因。
    {
        std::string currentDatabase;
        try {
            const auto rows = db::Pool::instance().client()->execSqlSync(
                "SELECT current_database()::text AS name");
            if (rows.size() != 1u || rows[0]["name"].isNull()) {
                std::cerr << "[SECURITY ALERT] 无法确定当前数据库名，拒绝运行数据库测试。\n";
                return 1;
            }
            currentDatabase = rows[0]["name"].as<std::string>();
        } catch (const std::exception &) {
            std::cerr << "[SECURITY ALERT] 查询 current_database() 失败，拒绝运行数据库测试。\n";
            return 1;
        }
        if (!usapptest::isExpectedTestDatabase(currentDatabase)) {
            std::cerr << "[SECURITY ALERT] 当前数据库为 '" << usapptest::safeDatabaseName(currentDatabase)
                      << "'，不是 '" << usapptest::kExpectedTestDatabase
                      << "'，拒绝运行数据库测试（未写入任何数据）。\n";
            return 1;
        }
        std::printf("[ INFO ] 目标库守卫通过: current_database() = %s\n", usapptest::kExpectedTestDatabase);
    }

    // 迁移完整性门控：阶段 0 的 6 个迁移与阶段 1 M2-A 的 0006 必须全部已应用（精确匹配完整 BaseName）
    const std::vector<std::string> expectedMigrations = {
        "0000_schema_migrations",
        "0001_common",
        "0002_identity",
        "0003_couple_binding",
        "0004_scheduler",
        "0005_admin_audit",
        "0006_wallet_ledger"
    };

    try {
        auto res = db::Pool::instance().client()->execSqlSync(
            "SELECT version FROM schema_migrations");
        std::unordered_set<std::string> appliedVersions;
        for (const auto &row : res) {
            appliedVersions.insert(row["version"].as<std::string>());
        }

        std::vector<std::string> missing;
        for (const auto &expected : expectedMigrations) {
            if (appliedVersions.find(expected) == appliedVersions.end()) {
                missing.push_back(expected);
            }
        }

        if (!missing.empty()) {
            std::cerr << "[ERROR] 测试数据库缺失必要迁移 (必须包含完整 BaseName)：\n";
            for (const auto &m : missing) {
                std::cerr << "  - 缺失: " << m << "\n";
            }
            std::cerr << "请先执行数据库迁移！\n";
            return 1;
        }
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] 检查 schema_migrations 异常 (迁移表可能尚未创建): " << e.what() << "\n";
        return 1;
    }

    auto client = db::Pool::instance().client();

    // -----------------------------------------------------------------
    // 1. 预认证会话与正式会话生命周期
    // -----------------------------------------------------------------
    TEST_CASE("Database: 预认证会话创建、加载、认证切换与撤销") {
        const std::string rawToken = crypto::randomHex(32);
        const std::string rawCsrf = crypto::randomHex(32);
        const std::string tokenHash = crypto::sha256Hex(rawToken);
        const std::string csrfHash = crypto::sha256Hex(rawCsrf);

        std::string preauthSessionId;
        std::string testUserId;

        try {
            // A. 插入预认证会话
            auto insertPre = client->execSqlSync(
                "INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, expires_at) "
                "VALUES ($1, $2, false, NOW() + INTERVAL '1 hour') "
                "RETURNING id::text",
                tokenHash, csrfHash);
            CHECK_EQ(insertPre.size(), 1u);
            preauthSessionId = insertPre[0]["id"].as<std::string>();
            CHECK_FALSE(preauthSessionId.empty());

            // 查出该会话，确认未认证且 user_id 为空
            auto q1 = client->execSqlSync(
                "SELECT id::text, user_id, is_authenticated, revoked_at FROM sessions WHERE token_hash = $1",
                tokenHash);
            CHECK_EQ(q1.size(), 1u);
            CHECK_FALSE(q1[0]["is_authenticated"].as<bool>());
            CHECK(q1[0]["user_id"].isNull());
            CHECK(q1[0]["revoked_at"].isNull());

            // B. 模拟用户登录：创建测试用户并切换为正式会话
            auto insertUser = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('dbtest_user1@example.com', 'dbtest_user1@example.com', 'Tester 1') "
                "RETURNING id::text");
            CHECK_EQ(insertUser.size(), 1u);
            testUserId = insertUser[0]["id"].as<std::string>();

            // 撤销旧会话并生成新会话 (防会话固定)
            client->execSqlSync("UPDATE sessions SET revoked_at = NOW() WHERE id = $1::uuid", preauthSessionId);

            const std::string authRawToken = crypto::randomHex(32);
            const std::string authCsrf = crypto::randomHex(32);
            auto insertAuth = client->execSqlSync(
                "INSERT INTO sessions (user_id, token_hash, csrf_token_hash, is_authenticated, expires_at) "
                "VALUES ($1::uuid, $2, $3, true, NOW() + INTERVAL '7 days') "
                "RETURNING id::text",
                testUserId, crypto::sha256Hex(authRawToken), crypto::sha256Hex(authCsrf));
            CHECK_EQ(insertAuth.size(), 1u);
            std::string authSessionId = insertAuth[0]["id"].as<std::string>();

            // 验证旧会话已撤销
            auto qOld = client->execSqlSync("SELECT revoked_at FROM sessions WHERE id = $1::uuid", preauthSessionId);
            CHECK_EQ(qOld.size(), 1u);
            CHECK_FALSE(qOld[0]["revoked_at"].isNull());

            // 验证新会话为认证状态
            auto qNew = client->execSqlSync(
                "SELECT is_authenticated, user_id::text FROM sessions WHERE id = $1::uuid", authSessionId);
            CHECK_EQ(qNew.size(), 1u);
            CHECK(qNew[0]["is_authenticated"].as<bool>());
            CHECK_EQ(qNew[0]["user_id"].as<std::string>(), testUserId);

            // 清理
            client->execSqlSync("DELETE FROM sessions WHERE id IN ($1::uuid, $2::uuid)", preauthSessionId, authSessionId);
            client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", testUserId);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            // 确保测试数据清理
            if (!preauthSessionId.empty()) {
                client->execSqlSync("DELETE FROM sessions WHERE id = $1::uuid", preauthSessionId);
            }
            if (!testUserId.empty()) {
                client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", testUserId);
            }
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 2. 空间创建与成员槽位容量 (slot 1 & 2) 约束
    // -----------------------------------------------------------------
    TEST_CASE("Database: 空间成员槽位 (1, 2) 取值域、容量上限与单一空间约束") {
        std::string u1, u2, u3, space1, space2;
        try {
            // 创建 3 个测试用户
            auto r1 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('slot_u1@example.com', 'slot_u1@example.com', 'U1') RETURNING id::text");
            auto r2 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('slot_u2@example.com', 'slot_u2@example.com', 'U2') RETURNING id::text");
            auto r3 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('slot_u3@example.com', 'slot_u3@example.com', 'U3') RETURNING id::text");
            u1 = r1[0]["id"].as<std::string>();
            u2 = r2[0]["id"].as<std::string>();
            u3 = r3[0]["id"].as<std::string>();

            // 创建空间 1
            auto s1 = client->execSqlSync(
                "INSERT INTO couples (name, created_by_user_id) VALUES ('Space 1', $1::uuid) RETURNING id::text", u1);
            space1 = s1[0]["id"].as<std::string>();

            // u1 占 slot=1
            client->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)", u1, space1);

            // u2 占 slot=2
            client->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 2)", u2, space1);

            // 验证 slot=3 必须被 CHECK (slot IN (1, 2)) 拒绝
            bool slot3Rejected = false;
            try {
                client->execSqlSync(
                    "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 3)", u3, space1);
            } catch (const std::exception &) {
                slot3Rejected = true;
            }
            CHECK(slot3Rejected);

            // 验证重复 slot=1 必须被 UNIQUE (couple_id, slot) 拒绝
            bool duplicateSlotRejected = false;
            try {
                client->execSqlSync(
                    "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)", u3, space1);
            } catch (const std::exception &) {
                duplicateSlotRejected = true;
            }
            CHECK(duplicateSlotRejected);

            // 创建空间 2，验证 u1 不能加入第二个空间 (couple_members.user_id UNIQUE 约束)
            auto s2 = client->execSqlSync(
                "INSERT INTO couples (name, created_by_user_id) VALUES ('Space 2', $1::uuid) RETURNING id::text", u3);
            space2 = s2[0]["id"].as<std::string>();

            bool multiSpaceRejected = false;
            try {
                client->execSqlSync(
                    "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)", u1, space2);
            } catch (const std::exception &) {
                multiSpaceRejected = true;
            }
            CHECK(multiSpaceRejected);

            // 清理
            client->execSqlSync("DELETE FROM couple_members WHERE couple_id IN ($1::uuid, $2::uuid)", space1, space2);
            client->execSqlSync("DELETE FROM couples WHERE id IN ($1::uuid, $2::uuid)", space1, space2);
            client->execSqlSync("DELETE FROM users WHERE id IN ($1::uuid, $2::uuid, $3::uuid)", u1, u2, u3);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            if (!space1.empty()) client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", space1);
            if (!space2.empty()) client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", space2);
            if (!space1.empty()) client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", space1);
            if (!space2.empty()) client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", space2);
            if (!u1.empty()) client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", u1);
            if (!u2.empty()) client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", u2);
            if (!u3.empty()) client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", u3);
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 3. 邀请生成与原子消费
    // -----------------------------------------------------------------
    TEST_CASE("Database: 邀请单次使用与二次消费拒绝") {
        std::string inviterId, inviteeId, spaceId, inviteId;
        std::shared_ptr<drogon::orm::Transaction> consumeTx;
        try {
            auto r1 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('inv_host@example.com', 'inv_host@example.com', 'Host') RETURNING id::text");
            auto r2 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ('inv_guest@example.com', 'inv_guest@example.com', 'Guest') RETURNING id::text");
            inviterId = r1[0]["id"].as<std::string>();
            inviteeId = r2[0]["id"].as<std::string>();

            auto s = client->execSqlSync(
                "INSERT INTO couples (name, created_by_user_id) VALUES ('InviteSpace', $1::uuid) RETURNING id::text", inviterId);
            spaceId = s[0]["id"].as<std::string>();

            client->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)", inviterId, spaceId);

            // 生成测试邀请（前缀规则：规范化邀请码的前 4 个字符）
            const std::string rawCode = SpaceService::generateInviteCode();
            const std::string normalized = SpaceService::normalizeInviteCode(rawCode);
            CHECK_FALSE(normalized.empty());
            const std::string codeHash = crypto::sha256Hex(normalized);
            const std::string codePrefix = normalized.substr(0, 4);

            auto inv = client->execSqlSync(
                "INSERT INTO space_invites (couple_id, created_by_user_id, code_hash, code_prefix, expires_at, max_uses, used_count) "
                "VALUES ($1::uuid, $2::uuid, $3, $4, NOW() + INTERVAL '7 days', 1, 0) RETURNING id::text",
                spaceId, inviterId, codeHash, codePrefix);
            CHECK_EQ(inv.size(), 1u);
            inviteId = inv[0]["id"].as<std::string>();
            CHECK_FALSE(inviteId.empty());

            // 首次消费：通过真实 Drogon Transaction 在同一连接、同一事务中执行原子更新 used_count 与插入 slot=2
            consumeTx = client->newTransaction();
            consumeTx->setTimeout(kDbTimeoutSeconds);
            auto consumeRes = consumeTx->execSqlSync(
                "UPDATE space_invites SET used_count = used_count + 1, consumed_at = NOW(), consumed_by_user_id = $1::uuid "
                "WHERE id = $2::uuid AND used_count < max_uses AND revoked_at IS NULL "
                "RETURNING used_count",
                inviteeId, inviteId);
            CHECK_EQ(consumeRes.size(), 1u);
            CHECK_EQ(consumeRes[0]["used_count"].as<int>(), 1);

            consumeTx->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 2)",
                inviteeId, spaceId);

            // 提交事务：设置提交回调并释放引用以触发真实 COMMIT
            // 回调按值持有 shared_ptr<promise>，不捕获局部引用或事务自身以防止悬空引用与引用环
            auto commitPromise = std::make_shared<std::promise<bool>>();
            auto commitFuture = commitPromise->get_future();
            consumeTx->setCommitCallback([commitPromise](bool ok) {
                try {
                    commitPromise->set_value(ok);
                } catch (...) {
                    // 防御 promise 重复 set_value
                }
            });
            consumeTx.reset();

            // 等待提交结果（超时仅表示提交未在时限内确认，不代表回滚完成）
            bool commitOk = false;
            if (commitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
                commitOk = commitFuture.get();
            }
            CHECK(commitOk);
            if (!commitOk) {
                throw std::runtime_error("首次消费事务提交未在时限内确认或提交失败");
            }

            // 成功提交后通过连接池验证持久化结果
            auto qInv = client->execSqlSync(
                "SELECT used_count, consumed_by_user_id::text FROM space_invites WHERE id = $1::uuid",
                inviteId);
            CHECK_EQ(qInv.size(), 1u);
            CHECK_EQ(qInv[0]["used_count"].as<int>(), 1);
            CHECK_EQ(qInv[0]["consumed_by_user_id"].as<std::string>(), inviteeId);

            auto qMember = client->execSqlSync(
                "SELECT slot FROM couple_members WHERE user_id = $1::uuid AND couple_id = $2::uuid",
                inviteeId, spaceId);
            CHECK_EQ(qMember.size(), 1u);
            CHECK_EQ(qMember[0]["slot"].as<int>(), 2);

            // 二次消费：由于 used_count >= max_uses，更新应影响 0 行
            auto secondConsume = client->execSqlSync(
                "UPDATE space_invites SET used_count = used_count + 1, consumed_at = NOW() "
                "WHERE id = $1::uuid AND used_count < max_uses AND revoked_at IS NULL "
                "RETURNING used_count",
                inviteId);
            CHECK_EQ(secondConsume.size(), 0u);

            // 清理
            client->execSqlSync("DELETE FROM space_invites WHERE id = $1::uuid", inviteId);
            client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", spaceId);
            client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", spaceId);
            client->execSqlSync("DELETE FROM users WHERE id IN ($1::uuid, $2::uuid)", inviterId, inviteeId);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            if (consumeTx) {
                try { consumeTx->rollback(); } catch (...) {}
                consumeTx.reset();
            }
            if (!inviteId.empty()) client->execSqlSync("DELETE FROM space_invites WHERE id = $1::uuid", inviteId);
            if (!spaceId.empty()) client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", spaceId);
            if (!spaceId.empty()) client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", spaceId);
            if (!inviterId.empty()) client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", inviterId);
            if (!inviteeId.empty()) client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", inviteeId);
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 4. 调度器事务级 Advisory Lock 互斥与回滚释放
    // -----------------------------------------------------------------
    TEST_CASE("Database: pg_try_advisory_xact_lock 事务级锁互斥与回滚释放") {
        constexpr int64_t kTestLockKey = 7492001;
        std::shared_ptr<drogon::orm::Transaction> txA;
        std::shared_ptr<drogon::orm::Transaction> txB;

        try {
            // 分别开启两个真实 Drogon Transaction，各绑定独立连接，并设置有限超时防止查询无限阻塞
            txA = client->newTransaction();
            txA->setTimeout(kDbTimeoutSeconds);
            txB = client->newTransaction();
            txB->setTimeout(kDbTimeoutSeconds);

            // 用 pg_backend_pid() 断言两个事务确实使用不同连接
            auto pidA_res = txA->execSqlSync("SELECT pg_backend_pid() AS pid");
            CHECK_EQ(pidA_res.size(), 1u);
            int64_t pidA = pidA_res[0]["pid"].as<int64_t>();

            auto pidB_res = txB->execSqlSync("SELECT pg_backend_pid() AS pid");
            CHECK_EQ(pidB_res.size(), 1u);
            int64_t pidB = pidB_res[0]["pid"].as<int64_t>();

            CHECK_NE(pidA, pidB);

            // A 获取同一个事务级 advisory lock，断言 true
            auto lockA = txA->execSqlSync("SELECT pg_try_advisory_xact_lock($1) AS locked", kTestLockKey);
            CHECK_EQ(lockA.size(), 1u);
            CHECK(lockA[0]["locked"].as<bool>());

            // A 持锁期间，B 尝试获取同一个锁，断言竞争失败 (false)
            auto lockB1 = txB->execSqlSync("SELECT pg_try_advisory_xact_lock($1) AS locked", kTestLockKey);
            CHECK_EQ(lockB1.size(), 1u);
            CHECK_FALSE(lockB1[0]["locked"].as<bool>());

            // A 请求回滚并释放引用（注：Drogon 的 rollback() 无回调，命令排入连接队列异步下发）
            txA->rollback();
            txA.reset();

            // A 请求回滚后，B 在有明确截止时间的短间隔重试中最终获取成功，断言 true（超时明确失败）
            bool bAcquired = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto lockB2 = txB->execSqlSync("SELECT pg_try_advisory_xact_lock($1) AS locked", kTestLockKey);
                if (!lockB2.empty() && lockB2[0]["locked"].as<bool>()) {
                    bAcquired = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CHECK(bAcquired);

            // 清理 B 的事务，释放锁
            txB->rollback();
            txB.reset();
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            if (txA) {
                try { txA->rollback(); } catch (...) {}
                txA.reset();
            }
            if (txB) {
                try { txB->rollback(); } catch (...) {}
                txB.reset();
            }
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 5. make_interval 参数类型绑定与管理员会话 TTL 回归测试
    // -----------------------------------------------------------------
    TEST_CASE("Database: make_interval 参数类型绑定与管理员会话 TTL 回归测试") {
        std::string testAdminId;
        std::string testSessionId;

        try {
            // A. 基础参数绑定验证：传入 C++ int 秒数，断言时间差匹配，验证 $1::integer 解决 Drogon 二进制 int 绑定问题
            const int testSecs = 600;
            auto intervalRes = client->execSqlSync(
                "SELECT EXTRACT(EPOCH FROM (now() + make_interval(secs => $1::integer) - now()))::integer AS diff_secs",
                testSecs);
            CHECK_EQ(intervalRes.size(), 1u);
            if (intervalRes.size() != 1u) {
                throw std::runtime_error("intervalRes 结果行数不符合预期 (应为 1 行)");
            }
            CHECK_EQ(intervalRes[0]["diff_secs"].as<int>(), testSecs);

            const int testTtlSecs = AdminAuthService::kAdminSessionTtlSeconds;
            auto ttlRes = client->execSqlSync(
                "SELECT EXTRACT(EPOCH FROM (now() + make_interval(secs => $1::integer) - now()))::integer AS diff_secs",
                testTtlSecs);
            CHECK_EQ(ttlRes.size(), 1u);
            if (ttlRes.size() != 1u) {
                throw std::runtime_error("ttlRes 结果行数不符合预期 (应为 1 行)");
            }
            CHECK_EQ(ttlRes[0]["diff_secs"].as<int>(), testTtlSecs);

            // B. 真实 AdminAuthService::login 流程与数据库 session 记录验证
            const std::string adminUsername = "t_adm_" + crypto::randomHex(4);
            const std::string testPassword = "AdminTestPassword123!";
            const std::string passwordHash = crypto::pbkdf2Hash(testPassword, 10000);

            // 插入测试管理员
            auto insertAdminRes = client->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) "
                "VALUES ($1, $2, 'active') "
                "RETURNING id::text AS id",
                adminUsername,
                passwordHash);
            CHECK_EQ(insertAdminRes.size(), 1u);
            if (insertAdminRes.size() != 1u) {
                throw std::runtime_error("insertAdminRes 结果行数不符合预期 (应为 1 行)");
            }
            testAdminId = insertAdminRes[0]["id"].as<std::string>();
            CHECK(!testAdminId.empty());
            if (testAdminId.empty()) {
                throw std::runtime_error("插入管理员返回的 ID 为空");
            }

            // 异步调用 AdminAuthService::login，使用 shared_ptr<promise> 捕获，防止超时时局部对象销毁引发悬空引用
            auto loginPromise = std::make_shared<std::promise<AdminAuthService::LoginResult>>();
            auto loginFuture = loginPromise->get_future();

            AdminAuthService::instance().login(
                adminUsername,
                testPassword,
                [loginPromise](AdminAuthService::LoginResult res) {
                    try {
                        loginPromise->set_value(std::move(res));
                    } catch (...) {}
                });

            const auto waitStatus = loginFuture.wait_for(std::chrono::seconds(5));
            CHECK(waitStatus == std::future_status::ready);
            if (waitStatus != std::future_status::ready) {
                throw std::runtime_error("AdminAuthService::login 异步调用超时 (5s)");
            }
            AdminAuthService::LoginResult loginResult = loginFuture.get();

            CHECK(loginResult.outcome == AdminAuthService::LoginOutcome::Success);
            CHECK(!loginResult.sessionId.empty());
            if (loginResult.outcome != AdminAuthService::LoginOutcome::Success ||
                loginResult.sessionId.empty()) {
                throw std::runtime_error("AdminAuthService::login 未成功或 sessionId 为空: " +
                                         loginResult.error);
            }
            testSessionId = loginResult.sessionId;

            CHECK_EQ(loginResult.adminUserId, testAdminId);
            CHECK(!loginResult.rawSessionToken.empty());
            CHECK(!loginResult.rawCsrfToken.empty());

            // 查询 admin_sessions 验证数据库落库与 TTL 准确性
            auto sessionQuery = client->execSqlSync(
                "SELECT id::text AS id, admin_user_id::text AS admin_user_id, "
                "       token_hash, csrf_token_hash, "
                "       EXTRACT(EPOCH FROM (expires_at - created_at))::integer AS ttl_secs "
                "FROM admin_sessions WHERE id = $1::uuid",
                testSessionId);
            CHECK_EQ(sessionQuery.size(), 1u);
            if (sessionQuery.size() != 1u) {
                throw std::runtime_error("sessionQuery 结果行数不符合预期 (应为 1 行)");
            }
            CHECK_EQ(sessionQuery[0]["admin_user_id"].as<std::string>(), testAdminId);

            // 散列校验
            const std::string expectedTokenHash = crypto::sha256Hex(loginResult.rawSessionToken);
            const std::string expectedCsrfHash = crypto::sha256Hex(loginResult.rawCsrfToken);
            CHECK_EQ(sessionQuery[0]["token_hash"].as<std::string>(), expectedTokenHash);
            CHECK_EQ(sessionQuery[0]["csrf_token_hash"].as<std::string>(), expectedCsrfHash);

            // TTL 校验：必须与 kAdminSessionTtlSeconds (43200 秒) 精确匹配
            const int actualTtlSecs = sessionQuery[0]["ttl_secs"].as<int>();
            CHECK_EQ(actualTtlSecs, AdminAuthService::kAdminSessionTtlSeconds);

            // 清理数据（按依赖逆序）：audit_logs -> admin_sessions -> admin_users
            client->execSqlSync(
                "DELETE FROM audit_logs WHERE actor_type = 'admin' AND actor_id = $1::uuid",
                testAdminId);
            if (!testSessionId.empty()) {
                client->execSqlSync("DELETE FROM admin_sessions WHERE id = $1::uuid", testSessionId);
                testSessionId.clear();
            }
            if (!testAdminId.empty()) {
                client->execSqlSync("DELETE FROM admin_users WHERE id = $1::uuid", testAdminId);
                testAdminId.clear();
            }
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            if (!testAdminId.empty()) {
                try {
                    client->execSqlSync(
                        "DELETE FROM audit_logs WHERE actor_type = 'admin' AND actor_id = $1::uuid",
                        testAdminId);
                } catch (...) {}
            }
            if (!testSessionId.empty()) {
                try { client->execSqlSync("DELETE FROM admin_sessions WHERE id = $1::uuid", testSessionId); } catch (...) {}
            }
            if (!testAdminId.empty()) {
                try { client->execSqlSync("DELETE FROM admin_users WHERE id = $1::uuid", testAdminId); } catch (...) {}
            }
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 6. 管理后台列表分页参数绑定（int limit / long long offset）回归测试
    // -----------------------------------------------------------------
    TEST_CASE("Database: 管理后台列表分页参数类型绑定 (LIMIT integer OFFSET bigint)") {
        int64_t auditId1 = 0;
        int64_t auditId2 = 0;
        int64_t auditId3 = 0;
        std::string testUserId1;
        std::string testUserId2;

        try {
            // A. 审计日志列表分页验证（具有确定的有序测试数据）
            const std::string auditTag = "page_test_" + crypto::randomHex(4);
            const std::string action1 = auditTag + "_1";
            const std::string action2 = auditTag + "_2";
            const std::string action3 = auditTag + "_3";

            // 依次插入 3 条确定的测试审计日志（bigserial 自增保证 auditId1 < auditId2 < auditId3）
            auto ins1 = client->execSqlSync(
                "INSERT INTO audit_logs (actor_type, action) VALUES ('system', $1) RETURNING id",
                action1);
            if (ins1.size() != 1u) throw std::runtime_error("插入测试审计日志 1 失败");
            auditId1 = ins1[0]["id"].as<int64_t>();

            auto ins2 = client->execSqlSync(
                "INSERT INTO audit_logs (actor_type, action) VALUES ('system', $1) RETURNING id",
                action2);
            if (ins2.size() != 1u) throw std::runtime_error("插入测试审计日志 2 失败");
            auditId2 = ins2[0]["id"].as<int64_t>();

            auto ins3 = client->execSqlSync(
                "INSERT INTO audit_logs (actor_type, action) VALUES ('system', $1) RETURNING id",
                action3);
            if (ins3.size() != 1u) throw std::runtime_error("插入测试审计日志 3 失败");
            auditId3 = ins3[0]["id"].as<int64_t>();

            CHECK(auditId1 < auditId2);
            CHECK(auditId2 < auditId3);

            // 真实 Drogon 绑定：C++ int 作为 limit，C++ long long 作为 offset
            const int auditLimit = 2;
            const long long auditOffset0 = 0LL;
            const long long auditOffset1 = 1LL;
            const long long auditOffset2 = 2LL;

            // 第一页：LIMIT 2 OFFSET 0（按 id 倒序应返回 auditId3 和 auditId2）
            auto auditPage0 = client->execSqlSync(
                "SELECT id, actor_type, action, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM audit_logs "
                " ORDER BY id DESC "
                " LIMIT $1::integer OFFSET $2::bigint",
                auditLimit, auditOffset0);
            CHECK(auditPage0.size() >= 2u);
            if (auditPage0.size() < 2u) {
                throw std::runtime_error("auditPage0 结果行数不足 2 行");
            }
            CHECK_EQ(auditPage0[0]["id"].as<int64_t>(), auditId3);
            CHECK_EQ(auditPage0[0]["action"].as<std::string>(), action3);
            CHECK_EQ(auditPage0[1]["id"].as<int64_t>(), auditId2);
            CHECK_EQ(auditPage0[1]["action"].as<std::string>(), action2);

            // 非零偏移页：LIMIT 2 OFFSET 1（应返回 auditId2 和 auditId1）
            auto auditPage1 = client->execSqlSync(
                "SELECT id, actor_type, action, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM audit_logs "
                " ORDER BY id DESC "
                " LIMIT $1::integer OFFSET $2::bigint",
                auditLimit, auditOffset1);
            CHECK(auditPage1.size() >= 2u);
            if (auditPage1.size() < 2u) {
                throw std::runtime_error("auditPage1 结果行数不足 2 行");
            }
            CHECK_EQ(auditPage1[0]["id"].as<int64_t>(), auditId2);
            CHECK_EQ(auditPage1[0]["action"].as<std::string>(), action2);
            CHECK_EQ(auditPage1[1]["id"].as<int64_t>(), auditId1);
            CHECK_EQ(auditPage1[1]["action"].as<std::string>(), action1);

            // 非零偏移页：LIMIT 2 OFFSET 2（应返回 auditId1 作为首行）
            auto auditPage2 = client->execSqlSync(
                "SELECT id, actor_type, action, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM audit_logs "
                " ORDER BY id DESC "
                " LIMIT $1::integer OFFSET $2::bigint",
                auditLimit, auditOffset2);
            CHECK(auditPage2.size() >= 1u);
            if (auditPage2.empty()) {
                throw std::runtime_error("auditPage2 结果为空");
            }
            CHECK_EQ(auditPage2[0]["id"].as<int64_t>(), auditId1);
            CHECK_EQ(auditPage2[0]["action"].as<std::string>(), action1);

            // B. 用户列表分页验证（测试 admin_user_overview 视图及其带搜索词的分页查询）
            const std::string userTag = crypto::randomHex(4);
            const std::string email1 = "u1_" + userTag + "@example.com";
            const std::string email2 = "u2_" + userTag + "@example.com";

            auto userIns1 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name, status) "
                "VALUES ($1, $2, 'PageTestUser1', 'active') RETURNING id::text AS id",
                email1, email1);
            if (userIns1.size() != 1u) throw std::runtime_error("插入测试用户 1 失败");
            testUserId1 = userIns1[0]["id"].as<std::string>();

            auto userIns2 = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name, status) "
                "VALUES ($1, $2, 'PageTestUser2', 'active') RETURNING id::text AS id",
                email2, email2);
            if (userIns2.size() != 1u) throw std::runtime_error("插入测试用户 2 失败");
            testUserId2 = userIns2[0]["id"].as<std::string>();

            const std::string queryFilter = userTag;
            const int userLimit = 1;
            const long long userOffset0 = 0LL;
            const long long userOffset1 = 1LL;
            const long long userOffset2 = 2LL;

            // 第一页：LIMIT 1 OFFSET 0
            auto userPage0 = client->execSqlSync(
                "SELECT user_id::text AS id, email, display_name, status, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM admin_user_overview "
                " WHERE $1 = '' OR email ILIKE '%' || $1 || '%' OR display_name ILIKE '%' || $1 || '%' "
                " ORDER BY created_at DESC, user_id "
                " LIMIT $2::integer OFFSET $3::bigint",
                queryFilter, userLimit, userOffset0);
            CHECK_EQ(userPage0.size(), 1u);
            if (userPage0.size() != 1u) throw std::runtime_error("userPage0 行数不为 1");
            const std::string firstPageUserId = userPage0[0]["id"].as<std::string>();

            // 第二页：LIMIT 1 OFFSET 1
            auto userPage1 = client->execSqlSync(
                "SELECT user_id::text AS id, email, display_name, status, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM admin_user_overview "
                " WHERE $1 = '' OR email ILIKE '%' || $1 || '%' OR display_name ILIKE '%' || $1 || '%' "
                " ORDER BY created_at DESC, user_id "
                " LIMIT $2::integer OFFSET $3::bigint",
                queryFilter, userLimit, userOffset1);
            CHECK_EQ(userPage1.size(), 1u);
            if (userPage1.size() != 1u) throw std::runtime_error("userPage1 行数不为 1");
            const std::string secondPageUserId = userPage1[0]["id"].as<std::string>();
            CHECK_NE(firstPageUserId, secondPageUserId);

            // 第三页：LIMIT 1 OFFSET 2（命中 2 条测试数据后的偏移，返回 0 行）
            auto userPage2 = client->execSqlSync(
                "SELECT user_id::text AS id, email, display_name, status, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM admin_user_overview "
                " WHERE $1 = '' OR email ILIKE '%' || $1 || '%' OR display_name ILIKE '%' || $1 || '%' "
                " ORDER BY created_at DESC, user_id "
                " LIMIT $2::integer OFFSET $3::bigint",
                queryFilter, userLimit, userOffset2);
            CHECK_EQ(userPage2.size(), 0u);

            // C. 情侣空间列表分页写法执行验证（测试 admin_couple_overview 视图）
            const int coupleLimit = 2;
            const long long coupleOffset0 = 0LL;
            const long long coupleOffset1 = 1LL;
            auto couplePage0 = client->execSqlSync(
                "SELECT couple_id::text AS id, name, member_count, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM admin_couple_overview "
                " ORDER BY created_at DESC, couple_id "
                " LIMIT $1::integer OFFSET $2::bigint",
                coupleLimit, coupleOffset0);
            // 允许空列表，但数据库执行必须成功无异常
            auto couplePage1 = client->execSqlSync(
                "SELECT couple_id::text AS id, name, member_count, "
                "       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
                "  FROM admin_couple_overview "
                " ORDER BY created_at DESC, couple_id "
                " LIMIT $1::integer OFFSET $2::bigint",
                coupleLimit, coupleOffset1);

            // 清理测试数据：按逆依赖顺序清理
            if (!testUserId1.empty() || !testUserId2.empty()) {
                client->execSqlSync(
                    "DELETE FROM users WHERE id IN ($1::uuid, $2::uuid)",
                    testUserId1, testUserId2);
                testUserId1.clear();
                testUserId2.clear();
            }
            if (auditId1 > 0 || auditId2 > 0 || auditId3 > 0) {
                client->execSqlSync(
                    "DELETE FROM audit_logs WHERE id IN ($1, $2, $3)",
                    auditId1, auditId2, auditId3);
                auditId1 = auditId2 = auditId3 = 0;
            }
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            if (!testUserId1.empty() || !testUserId2.empty()) {
                try {
                    client->execSqlSync(
                        "DELETE FROM users WHERE id IN ($1::uuid, $2::uuid)",
                        testUserId1, testUserId2);
                } catch (...) {}
            }
            if (auditId1 > 0 || auditId2 > 0 || auditId3 > 0) {
                try {
                    client->execSqlSync(
                        "DELETE FROM audit_logs WHERE id IN ($1, $2, $3)",
                        auditId1, auditId2, auditId3);
                } catch (...) {}
            }
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 7. 真实 SpaceService 邀请码生成与接受 (slot / max_uses smallint 参数绑定与消费) 回归测试
    // -----------------------------------------------------------------
    TEST_CASE("Database: SpaceService 邀请码生成与接受 (参数类型绑定与消费状态校验)") {
        std::string userId;
        std::string user2Id;
        std::string spaceId;
        std::string inviteId;

        try {
            // A. 创建测试用户 1 与空间，并将其加入槽位 1
            const std::string userEmail = "inv_creator_" + crypto::randomHex(4) + "@example.com";
            auto uRes = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ($1, $2, 'InvCreator') RETURNING id::text AS id",
                userEmail, userEmail);
            if (uRes.size() != 1u) throw std::runtime_error("插入测试邀请创建者失败");
            userId = uRes[0]["id"].as<std::string>();

            auto sRes = client->execSqlSync(
                "INSERT INTO couples (name, created_by_user_id) "
                "VALUES ('InviteTestSpace', $1::uuid) RETURNING id::text AS id",
                userId);
            if (sRes.size() != 1u) throw std::runtime_error("插入测试空间失败");
            spaceId = sRes[0]["id"].as<std::string>();

            client->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)",
                userId, spaceId);

            // B. 异步调用真实 SpaceService::createInvite（验证 $4::integer 参数绑定到 smallint）
            struct CreateInviteResult {
                SpaceService::Status status;
                std::string rawCode;
                std::string inviteId;
                std::string expiresAt;
            };
            auto promise = std::make_shared<std::promise<CreateInviteResult>>();
            auto future = promise->get_future();

            // 按值捕获 promise，杜绝超时局部变量析构悬空引用
            SpaceService::instance().createInvite(
                userId,
                [promise](SpaceService::Status status,
                          const std::string &rawCode,
                          const std::string &inviteId,
                          const std::string &expiresAt) {
                    try {
                        promise->set_value(CreateInviteResult{status, rawCode, inviteId, expiresAt});
                    } catch (...) {}
                });

            const auto waitStatus = future.wait_for(std::chrono::seconds(5));
            CHECK(waitStatus == std::future_status::ready);
            if (waitStatus != std::future_status::ready) {
                throw std::runtime_error("SpaceService::createInvite 异步调用超时 (5s)");
            }
            CreateInviteResult res = future.get();

            // C. 验证结果状态与非空（不打印明文邀请码）
            CHECK(res.status.ok());
            if (!res.status.ok()) {
                throw std::runtime_error("SpaceService::createInvite 失败: " + res.status.detail);
            }
            CHECK(!res.rawCode.empty());
            CHECK(!res.inviteId.empty());
            inviteId = res.inviteId;

            // D. 校验数据库中的邀请记录：max_uses=1、code_prefix 与规范化码前 4 字符一致
            auto invQuery = client->execSqlSync(
                "SELECT id::text AS id, couple_id::text AS couple_id, code_hash, code_prefix, "
                "       max_uses, used_count "
                "FROM space_invites WHERE id = $1::uuid",
                inviteId);
            CHECK_EQ(invQuery.size(), 1u);
            if (invQuery.size() != 1u) {
                throw std::runtime_error("未查到刚生成的邀请记录");
            }
            CHECK_EQ(invQuery[0]["max_uses"].as<int>(), 1);

            const std::string dbPrefix = invQuery[0]["code_prefix"].as<std::string>();
            const std::string expectedNormalized = SpaceService::normalizeInviteCode(res.rawCode);
            CHECK(!expectedNormalized.empty());
            const std::string expectedPrefix = expectedNormalized.substr(0, 4);
            CHECK_EQ(dbPrefix, expectedPrefix);

            const std::string expectedHash = crypto::sha256Hex(expectedNormalized);
            CHECK_EQ(invQuery[0]["code_hash"].as<std::string>(), expectedHash);

            // E. 创建测试用户 2 并通过真实 SpaceService::acceptInvite 接受邀请（验证 $3::integer 参数绑定到 smallint）
            const std::string user2Email = "inv_acceptor_" + crypto::randomHex(4) + "@example.com";
            auto u2Res = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ($1, $2, 'InvAcceptor') RETURNING id::text AS id",
                user2Email, user2Email);
            if (u2Res.size() != 1u) throw std::runtime_error("插入测试邀请接受者失败");
            user2Id = u2Res[0]["id"].as<std::string>();

            struct AcceptInviteResult {
                SpaceService::Status status;
                SpaceInfo space;
            };
            auto acceptPromise = std::make_shared<std::promise<AcceptInviteResult>>();
            auto acceptFuture = acceptPromise->get_future();

            SpaceService::instance().acceptInvite(
                user2Id,
                res.rawCode,
                [acceptPromise](SpaceService::Status status, SpaceInfo space) {
                    try {
                        acceptPromise->set_value(AcceptInviteResult{status, std::move(space)});
                    } catch (...) {}
                });

            const auto acceptWait = acceptFuture.wait_for(std::chrono::seconds(5));
            CHECK(acceptWait == std::future_status::ready);
            if (acceptWait != std::future_status::ready) {
                throw std::runtime_error("SpaceService::acceptInvite 异步调用超时 (5s)");
            }
            AcceptInviteResult acceptRes = acceptFuture.get();

            CHECK(acceptRes.status.ok());
            if (!acceptRes.status.ok()) {
                throw std::runtime_error("SpaceService::acceptInvite 失败: " + acceptRes.status.detail);
            }
            CHECK_EQ(acceptRes.space.spaceId, spaceId);
            CHECK_EQ(acceptRes.space.members.size(), 2u);

            // 校验数据库成员：第二个用户属于同一空间、slot=2，成员总数为 2
            auto memQuery = client->execSqlSync(
                "SELECT user_id::text AS user_id, slot FROM couple_members WHERE couple_id = $1::uuid ORDER BY slot",
                spaceId);
            CHECK_EQ(memQuery.size(), 2u);
            if (memQuery.size() != 2u) {
                throw std::runtime_error("空间成员总数不为 2");
            }
            CHECK_EQ(memQuery[0]["user_id"].as<std::string>(), userId);
            CHECK_EQ(memQuery[0]["slot"].as<int>(), 1);
            CHECK_EQ(memQuery[1]["user_id"].as<std::string>(), user2Id);
            CHECK_EQ(memQuery[1]["slot"].as<int>(), 2);

            // 校验数据库邀请记录：used_count=1、consumed_at 非空、consumed_by_user_id 为第二个用户
            auto invConsumedQuery = client->execSqlSync(
                "SELECT used_count, (consumed_at IS NOT NULL) AS has_consumed_at, "
                "       consumed_by_user_id::text AS consumed_by "
                "FROM space_invites WHERE id = $1::uuid",
                inviteId);
            CHECK_EQ(invConsumedQuery.size(), 1u);
            if (invConsumedQuery.size() != 1u) {
                throw std::runtime_error("未查到已消费的邀请记录");
            }
            CHECK_EQ(invConsumedQuery[0]["used_count"].as<int>(), 1);
            CHECK(invConsumedQuery[0]["has_consumed_at"].as<bool>());
            CHECK_EQ(invConsumedQuery[0]["consumed_by"].as<std::string>(), user2Id);

            // F. 再次接受同一邀请必须被拒绝，成员数和消费次数不增加
            auto repeatPromise = std::make_shared<std::promise<AcceptInviteResult>>();
            auto repeatFuture = repeatPromise->get_future();

            SpaceService::instance().acceptInvite(
                user2Id,
                res.rawCode,
                [repeatPromise](SpaceService::Status status, SpaceInfo space) {
                    try {
                        repeatPromise->set_value(AcceptInviteResult{status, std::move(space)});
                    } catch (...) {}
                });

            const auto repeatWait = repeatFuture.wait_for(std::chrono::seconds(5));
            CHECK(repeatWait == std::future_status::ready);
            if (repeatWait != std::future_status::ready) {
                throw std::runtime_error("重复 acceptInvite 异步调用超时 (5s)");
            }
            AcceptInviteResult repeatRes = repeatFuture.get();
            CHECK_FALSE(repeatRes.status.ok());
            CHECK(repeatRes.status.outcome ==
                  SpaceService::Outcome::InviteInvalidOrExpired);

            // 验证成员数与消费次数仍保持原样未递增
            auto memCountAfter = client->execSqlSync(
                "SELECT count(*)::integer AS cnt FROM couple_members WHERE couple_id = $1::uuid",
                spaceId);
            CHECK_EQ(memCountAfter.size(), 1u);
            if (memCountAfter.size() != 1u) throw std::runtime_error("查询成员数失败");
            CHECK_EQ(memCountAfter[0]["cnt"].as<int>(), 2);

            auto invCountAfter = client->execSqlSync(
                "SELECT used_count FROM space_invites WHERE id = $1::uuid",
                inviteId);
            CHECK_EQ(invCountAfter.size(), 1u);
            if (invCountAfter.size() != 1u) throw std::runtime_error("查询邀请消费次数失败");
            CHECK_EQ(invCountAfter[0]["used_count"].as<int>(), 1);

            // 清理数据（按逆依赖顺序清理）：audit_logs -> space_invites -> couple_members -> couples -> users
            client->execSqlSync(
                "DELETE FROM audit_logs WHERE (target_type = 'space_invite' AND target_id = $1) "
                "   OR (target_type = 'couple' AND target_id = $2)",
                inviteId, spaceId);
            client->execSqlSync("DELETE FROM space_invites WHERE id = $1::uuid", inviteId);
            inviteId.clear();
            client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", spaceId);
            client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", spaceId);
            spaceId.clear();
            client->execSqlSync("DELETE FROM users WHERE id IN ($1::uuid, $2::uuid)", userId, user2Id);
            userId.clear();
            user2Id.clear();
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            try {
                if (!inviteId.empty() || !spaceId.empty()) {
                    client->execSqlSync(
                        "DELETE FROM audit_logs WHERE (target_type = 'space_invite' AND target_id = $1) "
                        "   OR (target_type = 'couple' AND target_id = $2)",
                        inviteId, spaceId);
                }
                if (!inviteId.empty()) {
                    client->execSqlSync("DELETE FROM space_invites WHERE id = $1::uuid", inviteId);
                }
                if (!spaceId.empty()) {
                    client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", spaceId);
                    client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", spaceId);
                }
                if (!userId.empty() || !user2Id.empty()) {
                    client->execSqlSync("DELETE FROM users WHERE id IN ($1::uuid, $2::uuid)",
                                        userId.empty() ? "00000000-0000-0000-0000-000000000000" : userId,
                                        user2Id.empty() ? "00000000-0000-0000-0000-000000000000" : user2Id);
                }
            } catch (const std::exception &cleanErr) {
                std::cerr << "清理时发生异常: " << cleanErr.what() << "\n";
            }
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 8. AdminFilter 真实管理员会话校验（真实 doFilter + 真实 loadByRawToken）
    //
    // 使用本用例独立创建的测试管理员与会话行，不复用、不修改任何既有管理员。
    // 会话行直接写入 admin_sessions（库里只有令牌 SHA-256），由过滤器经
    // AdminAuthService::loadByRawToken 真实查库判定，而不是模拟一个布尔值。
    // 本用例不调用 login / revokeSession，因此不会产生异步审计日志。
    // -----------------------------------------------------------------
    TEST_CASE("Database: AdminFilter 拒绝无效 / 过期 / 撤销会话，放行有效会话并保持 CSRF 绑定") {
        namespace af = usapptest::adminfilter;
        const std::string peerCidr = af::probePeerCidr();
        CHECK_FALSE(peerCidr.empty());
        // 只把测试请求的精确默认对端地址加入白名单；作用域结束即恢复原配置。
        af::ScopedAdminConfig adminConfig(std::vector<std::string>{peerCidr});

        std::string adminId;
        std::vector<std::string> sessionIds;  // 本用例创建的全部 admin_sessions.id

        // 按标识符、按依赖顺序清理：admin_sessions -> admin_users。
        // 每条失败都打印并使返回值为 false，由调用方计为断言失败；不吞异常、
        // 不在失败时宣称清理成功。
        bool cleanupDone = false;
        auto cleanup = [&client, &adminId, &sessionIds, &cleanupDone]() -> bool {
            if (cleanupDone) {
                return true;
            }
            cleanupDone = true;
            bool ok = true;
            for (const auto &id : sessionIds) {
                try {
                    const auto r = client->execSqlSync(
                        "DELETE FROM admin_sessions WHERE id = $1::uuid", id);
                    if (r.affectedRows() != 1) {
                        ok = false;
                        std::cerr << "[CLEANUP FAIL] admin_sessions id=" << id
                                  << " 删除行数=" << r.affectedRows() << "（预期 1）\n";
                    }
                } catch (const std::exception &e) {
                    ok = false;
                    std::cerr << "[CLEANUP FAIL] admin_sessions id=" << id << ": " << e.what() << "\n";
                }
            }
            if (!adminId.empty()) {
                try {
                    const auto r = client->execSqlSync(
                        "DELETE FROM admin_users WHERE id = $1::uuid", adminId);
                    if (r.affectedRows() != 1) {
                        ok = false;
                        std::cerr << "[CLEANUP FAIL] admin_users id=" << adminId
                                  << " 删除行数=" << r.affectedRows() << "（预期 1）\n";
                    }
                } catch (const std::exception &e) {
                    ok = false;
                    std::cerr << "[CLEANUP FAIL] admin_users id=" << adminId << ": " << e.what() << "\n";
                }
            }
            return ok;
        };

        try {
            // A. 独立测试管理员（口令散列只是占位，本用例不登录）
            const std::string adminUsername = "t_flt_" + crypto::randomHex(4);
            auto adminRes = client->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) "
                "VALUES ($1, $2, 'active') RETURNING id::text AS id",
                adminUsername,
                crypto::pbkdf2Hash(crypto::randomHex(16), 1000));
            if (adminRes.size() != 1u) {
                throw std::runtime_error("插入测试管理员失败");
            }
            adminId = adminRes[0]["id"].as<std::string>();

            // B. 三条独立会话：有效 / 已过期 / 已撤销；另有一枚从未入库的未知令牌。
            const std::string validToken = crypto::randomHex(32);
            const std::string validCsrf = crypto::randomHex(32);
            const std::string expiredToken = crypto::randomHex(32);
            const std::string expiredCsrf = crypto::randomHex(32);
            const std::string revokedToken = crypto::randomHex(32);
            const std::string revokedCsrf = crypto::randomHex(32);
            const std::string unknownToken = crypto::randomHex(32);
            const std::string unknownCsrf = crypto::randomHex(32);

            auto insertSession = [&client, &adminId, &sessionIds](const char *sql,
                                                                   const std::string &rawToken,
                                                                   const std::string &rawCsrf) {
                auto r = client->execSqlSync(sql,
                                             AdminAuthService::tokenHash(rawToken),
                                             crypto::sha256Hex(rawCsrf),
                                             adminId);
                if (r.size() != 1u) {
                    throw std::runtime_error("插入测试管理员会话失败");
                }
                const std::string id = r[0]["id"].as<std::string>();
                sessionIds.push_back(id);
                return id;
            };

            const std::string validSessionId = insertSession(
                "INSERT INTO admin_sessions (token_hash, csrf_token_hash, admin_user_id, expires_at) "
                "VALUES ($1, $2, $3::uuid, now() + interval '1 hour') RETURNING id::text AS id",
                validToken, validCsrf);
            const std::string expiredSessionId = insertSession(
                "INSERT INTO admin_sessions "
                "(token_hash, csrf_token_hash, admin_user_id, created_at, last_seen_at, expires_at) "
                "VALUES ($1, $2, $3::uuid, now() - interval '2 hours', now() - interval '2 hours', "
                "        now() - interval '1 hour') RETURNING id::text AS id",
                expiredToken, expiredCsrf);
            const std::string revokedSessionId = insertSession(
                "INSERT INTO admin_sessions "
                "(token_hash, csrf_token_hash, admin_user_id, expires_at, revoked_at, revoked_reason) "
                "VALUES ($1, $2, $3::uuid, now() + interval '1 hour', now(), 'admin_filter_test') "
                "RETURNING id::text AS id",
                revokedToken, revokedCsrf);

            // 夹具自检：确认三条会话在库里的状态正是用例所假设的状态。
            auto stateRes = client->execSqlSync(
                "SELECT id::text AS id, (expires_at <= now()) AS expired, "
                "       (revoked_at IS NOT NULL) AS revoked "
                "FROM admin_sessions WHERE id IN ($1::uuid, $2::uuid, $3::uuid)",
                validSessionId, expiredSessionId, revokedSessionId);
            CHECK_EQ(stateRes.size(), 3u);
            for (const auto &row : stateRes) {
                const std::string id = row["id"].as<std::string>();
                const bool expired = row["expired"].as<bool>();
                const bool revoked = row["revoked"].as<bool>();
                if (id == validSessionId) {
                    CHECK_FALSE(expired);
                    CHECK_FALSE(revoked);
                } else if (id == expiredSessionId) {
                    CHECK(expired);
                    CHECK_FALSE(revoked);
                } else if (id == revokedSessionId) {
                    CHECK_FALSE(expired);
                    CHECK(revoked);
                } else {
                    CHECK(false);
                }
            }

            // C. 无效 / 过期 / 撤销：三个列表与注销一律 401；引导接口仍按未登录放行。
            struct BadSession {
                std::string label;
                std::string token;
                std::string csrf;  // 该会话"自己的" CSRF：证明正确 CSRF 也救不回失效会话
            };
            const std::vector<BadSession> badSessions = {
                {"未知令牌", unknownToken, unknownCsrf},
                {"已过期会话", expiredToken, expiredCsrf},
                {"已撤销会话", revokedToken, revokedCsrf},
            };
            for (const auto &bad : badSessions) {
                for (const auto &path : af::protectedListPaths()) {
                    auto req = af::adminRequest(drogon::Get, path);
                    af::setSessionCookie(req, bad.token);
                    CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                         bad.label + " " + af::describe(drogon::Get, path));
                    CHECK_FALSE(adminctx::isAuthenticated(req));
                }
                {
                    auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                    af::addWriteHeaders(req);
                    af::setCsrfToken(req, bad.csrf);
                    af::setSessionCookie(req, bad.token);
                    CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                         bad.label + " DELETE sessions");
                }
                {
                    auto req = af::adminRequest(drogon::Get, af::kBootstrapPath);
                    af::setSessionCookie(req, bad.token);
                    CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), bad.label + " GET session");
                    CHECK_FALSE(adminctx::isAuthenticated(req));
                    CHECK(adminctx::sessionId(req).empty());
                }
            }

            // D. 有效会话：三个列表放行，并附加真实会话身份。
            for (const auto &path : af::protectedListPaths()) {
                auto req = af::adminRequest(drogon::Get, path);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req),
                                      "有效会话 " + af::describe(drogon::Get, path));
                CHECK(adminctx::isAuthenticated(req));
                CHECK_EQ(adminctx::sessionId(req), validSessionId);
                CHECK_EQ(adminctx::userId(req), adminId);
                CHECK_EQ(adminctx::username(req), adminUsername);
            }
            {
                auto req = af::adminRequest(drogon::Get, af::kBootstrapPath);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "有效会话 GET session");
                CHECK(adminctx::isAuthenticated(req));
                CHECK_EQ(adminctx::sessionId(req), validSessionId);
            }

            // E. 有效会话的写请求（注销）仍受会话绑定 CSRF 与来源校验保护。
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setCsrfToken(req, validCsrf);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_CONTINUED(af::runAdminFilter(req), "有效会话 + 正确 CSRF 的注销");
                CHECK_EQ(adminctx::sessionId(req), validSessionId);
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setCsrfToken(req, crypto::randomHex(32));
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID",
                                     "有效会话 + 错误 CSRF 的注销");
            }
            {
                // 另一条会话的 CSRF 不能用于本会话（会话绑定）。
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setCsrfToken(req, revokedCsrf);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID",
                                     "有效会话 + 其他会话 CSRF 的注销");
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID",
                                     "有效会话 + 缺 CSRF 的注销");
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::setRequestedBy(req, adminctx::kRequestedByValue);
                af::setCsrfToken(req, validCsrf);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "ORIGIN_NOT_ALLOWED",
                                     "有效会话 + 缺 Origin 的注销");
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::setOrigin(req, af::kTestAdminOrigin);
                af::setCsrfToken(req, validCsrf);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 403, "CSRF_TOKEN_INVALID",
                                     "有效会话 + 缺 X-Requested-By 的注销");
            }

            // F. 同一枚令牌在会话被撤销（等价于注销完成）后立即失效。
            auto revokeRes = client->execSqlSync(
                "UPDATE admin_sessions SET revoked_at = now(), revoked_reason = 'admin_filter_test_logout' "
                "WHERE id = $1::uuid AND revoked_at IS NULL RETURNING id::text AS id",
                validSessionId);
            CHECK_EQ(revokeRes.size(), 1u);
            for (const auto &path : af::protectedListPaths()) {
                auto req = af::adminRequest(drogon::Get, path);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     "撤销后 " + af::describe(drogon::Get, path));
                CHECK_FALSE(adminctx::isAuthenticated(req));
            }
            {
                auto req = af::adminRequest(drogon::Delete, af::kSessionsPath);
                af::addWriteHeaders(req);
                af::setCsrfToken(req, validCsrf);
                af::setSessionCookie(req, validToken);
                CHECK_ADMIN_REJECTED(af::runAdminFilter(req), 401, "UNAUTHENTICATED",
                                     "撤销后 DELETE sessions");
            }

            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 9. 阶段 1 M1：真实 ProfileService 昵称更新
    //
    // 独立夹具：调用者 A（在一个独立测试空间的 slot 1）与旁观者 B（不在任何空间）。
    // 验证：只更新 A；B 不变；同事务写入一条审计且 detail 不含昵称值；
    // 非法输入与不存在的用户都不产生任何修改或审计。
    // 清理按标识符、按依赖顺序：audit_logs -> couple_members -> couples -> users，
    // 任一步失败都计为断言失败，不吞异常。
    // -----------------------------------------------------------------
    TEST_CASE("Database: ProfileService 只更新调用者昵称，审计同事务写入，非法输入不修改") {
        std::string userA;
        std::string userB;
        std::string spaceId;
        const std::string ghostUser = "00000000-0000-4000-8000-" + crypto::randomHex(6);

        bool cleanupDone = false;
        auto cleanup = [&client, &userA, &userB, &spaceId, &ghostUser, &cleanupDone]() -> bool {
            if (cleanupDone) {
                return true;
            }
            cleanupDone = true;
            bool ok = true;
            auto run = [&ok](const char *label, const std::function<void()> &step) {
                try {
                    step();
                } catch (const std::exception &e) {
                    ok = false;
                    std::cerr << "[CLEANUP FAIL] " << label << ": " << e.what() << "\n";
                }
            };
            const std::string none = "00000000-0000-0000-0000-000000000000";
            run("audit_logs", [&]() {
                client->execSqlSync(
                    "DELETE FROM audit_logs WHERE action = 'user.display_name_updated' "
                    "AND target_type = 'user' AND target_id IN ($1, $2, $3)",
                    userA.empty() ? none : userA, userB.empty() ? none : userB, ghostUser);
            });
            if (!spaceId.empty()) {
                run("couple_members", [&]() {
                    client->execSqlSync("DELETE FROM couple_members WHERE couple_id = $1::uuid", spaceId);
                });
                run("couples", [&]() {
                    const auto r = client->execSqlSync("DELETE FROM couples WHERE id = $1::uuid", spaceId);
                    if (r.affectedRows() != 1) {
                        throw std::runtime_error("删除行数=" + std::to_string(r.affectedRows()) + "（预期 1）");
                    }
                });
            }
            for (const std::string *id : {&userA, &userB}) {
                if (id->empty()) {
                    continue;
                }
                run("users", [&]() {
                    const auto r = client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", *id);
                    if (r.affectedRows() != 1) {
                        throw std::runtime_error("id=" + *id + " 删除行数=" +
                                                 std::to_string(r.affectedRows()) + "（预期 1）");
                    }
                });
            }
            return ok;
        };

        // 调用真实服务并有界等待；promise 按值捕获，超时后不再 get()。
        auto runUpdate = [](const std::string &userId, const std::string &rawName) {
            auto promise = std::make_shared<std::promise<ProfileService::Status>>();
            auto future = promise->get_future();
            ProfileService::instance().updateDisplayName(
                userId, rawName, [promise](ProfileService::Status status) {
                    try {
                        promise->set_value(std::move(status));
                    } catch (...) {}
                });
            if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                throw std::runtime_error("ProfileService::updateDisplayName 异步调用超时 (5s)");
            }
            return future.get();
        };

        auto displayNameOf = [&client](const std::string &id) {
            const auto r = client->execSqlSync("SELECT display_name FROM users WHERE id = $1::uuid", id);
            if (r.size() != 1u) {
                throw std::runtime_error("未查到测试用户");
            }
            return r[0]["display_name"].as<std::string>();
        };

        auto auditCountFor = [&client](const std::string &id) {
            const auto r = client->execSqlSync(
                "SELECT count(*)::integer AS cnt FROM audit_logs "
                "WHERE action = 'user.display_name_updated' AND target_type = 'user' AND target_id = $1",
                id);
            return r[0]["cnt"].as<int>();
        };

        try {
            // A. 夹具
            const std::string emailA = "prof_a_" + crypto::randomHex(4) + "@example.com";
            auto aRes = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ($1, $2, 'ProfA_old') RETURNING id::text AS id",
                emailA, emailA);
            if (aRes.size() != 1u) throw std::runtime_error("插入测试用户 A 失败");
            userA = aRes[0]["id"].as<std::string>();

            const std::string emailB = "prof_b_" + crypto::randomHex(4) + "@example.com";
            auto bRes = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ($1, $2, 'ProfB_keep') RETURNING id::text AS id",
                emailB, emailB);
            if (bRes.size() != 1u) throw std::runtime_error("插入测试用户 B 失败");
            userB = bRes[0]["id"].as<std::string>();

            auto sRes = client->execSqlSync(
                "INSERT INTO couples (name, created_by_user_id) "
                "VALUES ('ProfileTestSpace', $1::uuid) RETURNING id::text AS id",
                userA);
            if (sRes.size() != 1u) throw std::runtime_error("插入测试空间失败");
            spaceId = sRes[0]["id"].as<std::string>();
            client->execSqlSync(
                "INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)",
                userA, spaceId);

            // B. 合法更新：首尾空白被裁掉，中文 + emoji 原样保存。
            const std::string newName = "\xE6\x96\xB0\xE6\x98\xB5\xE7\xA7\xB0\xF0\x9F\x98\x80";  // 新昵称😀
            const ProfileService::Status okStatus = runUpdate(userA, "  " + newName + "  ");
            CHECK(okStatus.ok());
            if (!okStatus.ok()) {
                throw std::runtime_error("合法昵称更新失败: " + okStatus.detail);
            }
            CHECK_EQ(displayNameOf(userA), newName);
            CHECK_EQ(displayNameOf(userB), std::string("ProfB_keep"));

            // C. 审计：恰好一条；actor / couple / target 正确；detail 只有 field，且不含昵称值。
            auto audit = client->execSqlSync(
                "SELECT actor_type, actor_id::text AS actor_id, couple_id::text AS couple_id, "
                "       detail->>'field' AS field, "
                "       (SELECT count(*)::integer FROM jsonb_object_keys(detail)) AS key_count, "
                "       (position($2 in detail::text) > 0) AS leaks_name "
                "FROM audit_logs "
                "WHERE action = 'user.display_name_updated' AND target_type = 'user' AND target_id = $1",
                userA, newName);
            CHECK_EQ(audit.size(), 1u);
            if (audit.size() != 1u) throw std::runtime_error("昵称审计行数不为 1");
            CHECK_EQ(audit[0]["actor_type"].as<std::string>(), std::string("user"));
            CHECK_EQ(audit[0]["actor_id"].as<std::string>(), userA);
            CHECK_FALSE(audit[0]["couple_id"].isNull());
            if (!audit[0]["couple_id"].isNull()) {
                CHECK_EQ(audit[0]["couple_id"].as<std::string>(), spaceId);
            }
            CHECK_EQ(audit[0]["field"].as<std::string>(), std::string("display_name"));
            CHECK_EQ(audit[0]["key_count"].as<int>(), 1);
            CHECK_FALSE(audit[0]["leaks_name"].as<bool>());
            CHECK_EQ(auditCountFor(userB), 0);

            // D. 非法输入：拒绝且不修改、不写审计。
            struct BadInput {
                std::string label;
                std::string raw;
                ProfileService::DisplayNameError expected;
            };
            const std::vector<BadInput> badInputs = {
                {"换行", "bad\nname", ProfileService::DisplayNameError::ControlCharacter},
                {"纯空白", "   ", ProfileService::DisplayNameError::Empty},
                {"21 个码点", std::string(21, 'x'), ProfileService::DisplayNameError::TooLong},
                {"非法 UTF-8", "bad\xFF", ProfileService::DisplayNameError::InvalidUtf8},
            };
            for (const auto &bad : badInputs) {
                const ProfileService::Status status = runUpdate(userA, bad.raw);
                CHECK(status.outcome == ProfileService::Outcome::Validation);
                CHECK(status.validation == bad.expected);
                if (status.outcome != ProfileService::Outcome::Validation) {
                    std::cerr << "非法输入未被拒绝: " << bad.label << "\n";
                }
            }
            CHECK_EQ(displayNameOf(userA), newName);
            CHECK_EQ(displayNameOf(userB), std::string("ProfB_keep"));
            CHECK_EQ(auditCountFor(userA), 1);

            // E. 不存在的用户：NotFound，不写审计，也不影响其他用户。
            const ProfileService::Status ghost = runUpdate(ghostUser, "Ghost");
            CHECK(ghost.outcome == ProfileService::Outcome::NotFound);
            CHECK_EQ(auditCountFor(ghostUser), 0);
            CHECK_EQ(displayNameOf(userA), newName);
            CHECK_EQ(displayNameOf(userB), std::string("ProfB_keep"));

            // F. 重复昵称允许：B 改成与 A 相同的昵称。
            const ProfileService::Status dup = runUpdate(userB, newName);
            CHECK(dup.ok());
            CHECK_EQ(displayNameOf(userB), newName);
            CHECK_EQ(auditCountFor(userB), 1);

            // B 不在任何空间：审计 couple_id 为 NULL。
            auto auditB = client->execSqlSync(
                "SELECT (couple_id IS NULL) AS no_couple FROM audit_logs "
                "WHERE action = 'user.display_name_updated' AND target_type = 'user' AND target_id = $1",
                userB);
            CHECK_EQ(auditB.size(), 1u);
            if (auditB.size() == 1u) {
                CHECK(auditB[0]["no_couple"].as<bool>());
            }

            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 10. 阶段 1 M1：ProfileService 生命周期回归
    //
    // 回归对象：UpdateCtx 持有 TxScope，TxScope 把 begin() 的错误回调保存为
    // onError_；若该回调强捕获 ctx，成功提交或 NotFound 回滚之后
    // ctx -> scope -> onError_ -> ctx 成环，请求回调永不释放。
    //
    // 做法：结果回调按值捕获一个哨兵 shared_ptr，调用方随即放弃自己的引用，
    // 此后哨兵只由服务内部保存的回调拥有。拿到结果后在有界时间内等待哨兵释放
    // （不要求回调触发的瞬间就析构：Drogon 在回调返回后才释放其持有的闭包）。
    // 同时断言结果回调恰好被调用一次。
    // SQL 失败路径（非法 uuid 让 $1::uuid 报错）经由弱捕获的错误回调结束，
    // 一并覆盖；事务创建失败与提交失败无法在测试库中可靠触发，仅做了代码审查。
    // -----------------------------------------------------------------
    TEST_CASE("Database: ProfileService 成功 / NotFound / SQL 失败结束后释放回调持有的哨兵") {
        std::string userId;

        bool cleanupDone = false;
        auto cleanup = [&client, &userId, &cleanupDone]() -> bool {
            if (cleanupDone || userId.empty()) {
                cleanupDone = true;
                return true;
            }
            cleanupDone = true;
            bool ok = true;
            try {
                client->execSqlSync(
                    "DELETE FROM audit_logs WHERE action = 'user.display_name_updated' "
                    "AND target_type = 'user' AND target_id = $1",
                    userId);
            } catch (const std::exception &e) {
                ok = false;
                std::cerr << "[CLEANUP FAIL] audit_logs: " << e.what() << "\n";
            }
            try {
                const auto r = client->execSqlSync("DELETE FROM users WHERE id = $1::uuid", userId);
                if (r.affectedRows() != 1) {
                    ok = false;
                    std::cerr << "[CLEANUP FAIL] users id=" << userId << " 删除行数=" << r.affectedRows()
                              << "（预期 1）\n";
                }
            } catch (const std::exception &e) {
                ok = false;
                std::cerr << "[CLEANUP FAIL] users: " << e.what() << "\n";
            }
            return ok;
        };

        struct LifetimeResult {
            bool ready = false;
            ProfileService::Status status;
            int calls = 0;
            bool released = false;
        };

        auto runWithSentinel = [](const std::string &uid, const std::string &name) {
            auto promise = std::make_shared<std::promise<ProfileService::Status>>();
            auto future = promise->get_future();
            auto calls = std::make_shared<std::atomic<int>>(0);
            auto sentinel = std::make_shared<int>(42);
            const std::weak_ptr<int> watch = sentinel;

            ProfileService::instance().updateDisplayName(
                uid, name, [promise, calls, sentinel](ProfileService::Status status) {
                    calls->fetch_add(1);
                    try {
                        promise->set_value(std::move(status));
                    } catch (...) {}
                });
            sentinel.reset();  // 此后哨兵只由服务保存的结果回调拥有

            LifetimeResult out;
            if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                return out;  // 超时：不再 get()
            }
            out.ready = true;
            out.status = future.get();

            // 有界等待异步拆除。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!watch.expired() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out.released = watch.expired();
            out.calls = calls->load();
            return out;
        };

        try {
            const std::string email = "prof_life_" + crypto::randomHex(4) + "@example.com";
            auto uRes = client->execSqlSync(
                "INSERT INTO users (email_canonical, email_display, display_name) "
                "VALUES ($1, $2, 'Life_old') RETURNING id::text AS id",
                email, email);
            if (uRes.size() != 1u) throw std::runtime_error("插入测试用户失败");
            userId = uRes[0]["id"].as<std::string>();

            // A. 成功提交
            const LifetimeResult ok = runWithSentinel(userId, "Lifetime1");
            CHECK(ok.ready);
            CHECK(ok.status.ok());
            CHECK_EQ(ok.calls, 1);
            CHECK(ok.released);
            if (!ok.released) {
                std::cerr << "成功路径结束后哨兵未在 3s 内释放：疑似 ctx 所有权环\n";
            }

            // B. NotFound 回滚（库中不存在的合法 uuid）
            const std::string ghost = "00000000-0000-4000-8000-" + crypto::randomHex(6);
            const LifetimeResult notFound = runWithSentinel(ghost, "Ghost");
            CHECK(notFound.ready);
            CHECK(notFound.status.outcome == ProfileService::Outcome::NotFound);
            CHECK_EQ(notFound.calls, 1);
            CHECK(notFound.released);
            if (!notFound.released) {
                std::cerr << "NotFound 路径结束后哨兵未在 3s 内释放：疑似 ctx 所有权环\n";
            }

            // C. SQL 失败（非法 uuid）：经 TxScope::fail -> 弱捕获的错误回调结束。
            const LifetimeResult sqlError = runWithSentinel("not-a-uuid", "Broken");
            CHECK(sqlError.ready);
            CHECK(sqlError.status.outcome == ProfileService::Outcome::Error);
            CHECK_EQ(sqlError.calls, 1);
            CHECK(sqlError.released);

            // 业务结果不受影响：成功路径确实写入，且只有一条审计。
            auto row = client->execSqlSync("SELECT display_name FROM users WHERE id = $1::uuid", userId);
            CHECK_EQ(row.size(), 1u);
            if (row.size() == 1u) {
                CHECK_EQ(row[0]["display_name"].as<std::string>(), std::string("Lifetime1"));
            }
            auto audits = client->execSqlSync(
                "SELECT count(*)::integer AS cnt FROM audit_logs "
                "WHERE action = 'user.display_name_updated' AND target_type = 'user' "
                "  AND target_id IN ($1, $2, 'not-a-uuid')",
                userId, ghost);
            CHECK_EQ(audits[0]["cnt"].as<int>(), 1);

            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
        } catch (const std::exception &e) {
            std::cerr << "用例异常: " << e.what() << "\n";
            const bool cleanedUp = cleanup();
            CHECK(cleanedUp);
            CHECK(false);
        }
    }

    // -----------------------------------------------------------------
    // 11. 阶段 1 M2-A：钱包流水账（见 WalletLedgerDbTests.h）
    //     工作事务使用本进程的连接池；观察者是独立的单连接客户端。
    // -----------------------------------------------------------------
    usapptest::wallet::runWalletLedgerDbTests(client, dsn);

    // 12. M2-B: real read service and its SQL, with no committed wallet data.
    usapptest::wallet::runWalletReadDbTests(client, dsn);

    return usapptest::finish();
}
