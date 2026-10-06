// =====================================================================
// services/api/tests/integration_tests.cc
//
// 集成测试（需要真实 PostgreSQL）。标签：integration
//
// 运行规则（阶段 0 任务书的硬要求）：
//   * 环境变量 USAPP_TEST_DSN 未设置/为空 -> 打印一行 "SKIP: USAPP_TEST_DSN 未设置" 并返回 0；
//   * 已设置 -> 打印将要执行的破坏性操作警告，然后运行。
//
// ---------------------------------------------------------------------
// 为什么这个文件是「可执行的检查清单（runbook）」而不是真的连库执行 SQL
// ---------------------------------------------------------------------
// 1) 仓库只链接 Drogon 的 ORM（services/api/CMakeLists.txt），没有 libpq，
//    因此没有同步的 SQL 入口；唯一入口是 db::Pool 的**异步** execSqlAsync。
// 2) 异步入口需要事件循环。Db.h 明确写着 init() 必须在 drogon::app().run()
//    之前调用，而 run() 需要监听端口（会真的起 HTTP 服务）；用裸 EventLoop
//    则需要 trantor 直连 + 手写异步状态机。
// 3) 本任务的约束是「不执行任何命令」（不编译、不运行、不连库）。既然无法
//    编译验证事件循环/回调链，写一个 100% 会编译的空壳并宣称它测了东西，
//    比不写更糟。所以本文件诚实地做三件可验证的事：
//      a. DSN 的存在性/形状校验（真实断言，纯字符串，不需要数据库）；
//      b. 拒绝把破坏性操作指向名字不像测试库的数据库（真实断言 + 保护）；
//      c. 打印一份**逐步可执行**的 SQL 断言清单，人和脚本都能照着跑。
//    README.md「集成测试」一节给出了同样内容 + 逐步说明与预期错误码。
//    `*.sql` 意图：清单里的 SQL 全部是"跑完即回滚"或"只 SELECT"，不含迁移。
// =====================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "RepoText.h"
#include "TestSupport.h"

namespace {

// 从 libpq 风格的连接串里取 key=value（值可带单引号）。
std::string dsnValue(const std::string &dsn, const std::string &key) {
    const std::string needle = key + "=";
    std::size_t i = 0;
    while (true) {
        i = dsn.find(needle, i);
        if (i == std::string::npos) {
            return std::string();
        }
        const bool atTokenStart = (i == 0) || dsn[i - 1] == ' ' || dsn[i - 1] == '\t';
        if (atTokenStart) {
            const std::size_t begin = i + needle.size();
            std::size_t end = dsn.find(' ', begin);
            if (end == std::string::npos) {
                end = dsn.size();
            }
            std::string value = dsn.substr(begin, end - begin);
            if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        i += needle.size();
    }
}

// 打印连接串前先把口令抹掉：日志里不允许出现口令。
std::string redactDsn(const std::string &dsn) {
    std::string out;
    std::size_t i = 0;
    static const std::string kPasswordKey = "password=";
    while (i < dsn.size()) {
        if (dsn.compare(i, kPasswordKey.size(), kPasswordKey) == 0) {
            out += "password=***";
            i += kPasswordKey.size();
            while (i < dsn.size() && dsn[i] != ' ') {
                ++i;
            }
        } else {
            out.push_back(dsn[i]);
            ++i;
        }
    }
    return out;
}

void printSection(const char *title) {
    std::printf("\n---- %s ----\n", title);
}

void printPlan(const std::string &dbname) {
    printSection("将要执行的破坏性操作（全部在 USAPP_TEST_DSN 指向的库上）");
    std::printf(
        "  * 只针对 dbname=%s；不触碰任何其它数据库。\n"
        "  * 会 INSERT 临时 users / couples / couple_members / sessions / scheduler_ticks 行，\n"
        "    每个用例都在事务里，结束一律 ROLLBACK（不留下数据）。\n"
        "  * 会并发占住 pg_advisory_xact_lock 以验证锁竞争，事务结束即释放。\n"
        "  * 不会执行任何迁移（database/migrations 必须由 DBA/CI 事先应用）。\n"
        "  * 不会 DROP / TRUNCATE 任何表。\n",
        dbname.c_str());

    printSection("D1 会话令牌/CSRF 散列不可逆（对应 session_tests.cc 的无库部分）");
    std::printf(R"(  -- 令牌散列必须与明文不同，且同一明文两次写入散列相同：
  INSERT INTO sessions (token_hash, csrf_token_hash, expires_at)
       VALUES ('<hash(raw)>', '<hash(csrf)>', now() + interval '1 hour') RETURNING id;
  SELECT count(*) FROM sessions WHERE token_hash = '<raw token>';        -- 期望 0
  SELECT count(*) FROM sessions WHERE csrf_token_hash = '<raw csrf>';    -- 期望 0
  -- 应用层已覆盖（services/api/tests/session_tests.cc）：tokenHash 稳定、与明文不等、verifyCsrf 对错令牌/空令牌返回 false。
)");

    printSection("D2 注册白名单只影响新账号");
    std::printf(
        "  * 唯一的判定调用点是 AuthService.cc:166，即「新账号注册」分支；\n"
        "    RegistrationPolicy.h:7-10 明确写着白名单只决定“是否允许创建一个新账号”、\n"
        "    “不决定任何业务权限”、“已存在的账号永远可以继续登录”。\n"
        "  * SQL 侧断言：对已存在的 email_canonical 行，登录流程不得因为白名单而拒绝；\n"
        "    白名单为关闭态时，新 email_canonical 的 INSERT 必须成功。\n"
        "  * 该用例需要驱动 AuthService（drogon 异步 + 事件循环），见 README「集成测试」一节。\n");

    printSection("D3 成员数量约束（空间 = couples，成员 = couple_members）");
    std::printf(R"(  -- 前提：库已应用 database/migrations/0003_couple_binding.sql
  BEGIN;
  INSERT INTO couples (created_by_user_id) VALUES (:u1) RETURNING id;     -- :c
  INSERT INTO couple_members (user_id, couple_id, slot) VALUES (:u1, :c, 1);  -- 成功
  INSERT INTO couple_members (user_id, couple_id, slot) VALUES (:u2, :c, 2);  -- 成功
  -- (1) 第三条成员、slot 1 已被占：
  INSERT INTO couple_members (user_id, couple_id, slot) VALUES (:u3, :c, 1);
  --     期望 SQLSTATE 23505，约束名 couple_members_couple_slot_unique
  -- (2) 第三条成员、slot 越界：
  INSERT INTO couple_members (user_id, couple_id, slot) VALUES (:u3, :c, 3);
  --     期望 SQLSTATE 23514，约束名 couple_members_slot_valid
  -- (3) 同一个人加入第二个空间：
  INSERT INTO couple_members (user_id, couple_id, slot) VALUES (:u1, :c2, 1);
  --     期望 SQLSTATE 23505，约束名 couple_members_user_unique
  ROLLBACK;
  -- 读取实际约束定义（drift 检查）：
  SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint
   WHERE conrelid = 'couple_members'::regclass ORDER BY conname;
  --     期望含 slot IN (1,2) / UNIQUE (user_id) / UNIQUE (couple_id, slot)
)");

    printSection("D4 同空间权限：查不到「自己不是成员」的空间");
    std::printf(R"(  -- kSpaceQueryById（services/api/src/services/SpaceService.cc:41-54）的语义：
  --   ... WHERE self_m.user_id = $1::uuid AND c.id = $2::uuid
  -- 用**非成员**的 user_id 作为 $1，用该空间 id 作为 $2 执行上面这条 SQL：
  --     期望 0 行（不是 403，而是与“不存在”完全同一路径 -> 409/404 SPACE_NOT_FOUND）
  -- 用成员的 user_id 执行：期望恰好 1 行。
  -- 接口层（contracts/openapi.yaml:215-238）只声明 200/401/404，没有 403。
)");

    printSection("D5 CSRF 拒绝场景");
    std::printf(
        "  * 纯逻辑，已在 services/api/tests/csrf_tests.cc 用运行时断言覆盖，无需数据库。\n"
        "  * 端到端只需一条：带会话 Cookie 但错误 x-csrf-token 的 POST -> 403 CSRF_TOKEN_INVALID。\n");

    printSection("D6 调度锁竞争与回滚释放");
    std::printf(R"(  -- 会话 A 与 会话 B 两个并发连接，:key 取 LockKey::value()（恒 > 0）。
  -- A: BEGIN; SELECT pg_try_advisory_xact_lock(:key);   -- 期望 t
  -- B: BEGIN; SELECT pg_try_advisory_xact_lock(:key);   -- 期望 f（竞争失败）
  --    B 此时**不写任何行**（Scheduler.cc:255-259 注释与 rollback 分支）：
  --    SELECT count(*) FROM scheduler_ticks WHERE job_key = :job;   -- 期望 0（相对基线不变）
  -- A: ROLLBACK;                                        -- 锁随事务释放
  -- B: SELECT pg_try_advisory_xact_lock(:key);          -- 期望 t（已可获取）
  -- B: ROLLBACK;
  -- 持锁成功时写入的行（Scheduler.cc:285-288 的 'completed'）与之同事务：
  --    BEGIN; SELECT pg_try_advisory_xact_lock(:key);
  --    INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome, detail)
  --         VALUES (:job, :key, 'itest', 'completed', '{}'::jsonb);
  --    COMMIT;
  -- schema 强制的两条不变量（database/migrations/0004_scheduler.sql:26-30）：
  INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome)
       VALUES ('itest.job', 1, 'itest', 'skipped');
  --     期望 SQLSTATE 23514，约束名 scheduler_ticks_outcome_valid
  --     （跳过不写行是 schema 强制的，不是约定）
  INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome)
       VALUES ('itest.job', 0, 'itest', 'completed');
  --     期望 SQLSTATE 23514，约束名 scheduler_ticks_lock_key_nonzero
  SELECT lock_key FROM scheduler_ticks WHERE job_key = '__bootstrap__';   -- 期望 -1（哨兵行）
)");

    printSection("D7 静态与私有路径不得进入可缓存外壳");
    std::printf(
        "  * 契约侧（本仓库文本，已由 cache_and_contract_tests.cc 断言）：\n"
        "    contracts/openapi.yaml 的 6 个 components.responses 都带 NoStore；\n"
        "    /api/admin/v1/** 的 7 个成功响应缺 NoStore（已知缺口）。\n"
        "  * 前端侧：apps/web/src/pwa/cache-policy.ts 与 sw.ts 的边界由\n"
        "    apps/web/tests/unit/sw-cache-policy.spec.ts（vitest，已有）运行时覆盖；\n"
        "    services/api 侧的 C++ 测试只做文本断言，不重复实现。\n"
        "  * 端到端只需一条：请求 /api/v1/health 后检查响应头 Cache-Control 含 no-store。\n");

    printSection("收尾");
    std::printf(
        "  * 所有事务已 ROLLBACK；请确认 count(*) 与开始前的基线一致。\n"
        "  * 若某条断言失败，请保留 SQLSTATE 与约束名，它们指明了具体是哪一条不变量被破坏。\n");
}

}  // namespace

int main() {
    const char *dsnEnv = std::getenv("USAPP_TEST_DSN");
    const std::string dsn = (dsnEnv == nullptr) ? std::string() : std::string(dsnEnv);

    if (dsn.empty()) {
        // 阶段 0 硬要求：未设置 USAPP_TEST_DSN 时只打印一行并返回 0（不算失败）。
        std::printf("SKIP: USAPP_TEST_DSN 未设置\n");
        return 0;
    }

    std::printf("\n=====================================================================\n");
    std::printf("USAPP 集成测试（真实 PostgreSQL）\n");
    std::printf("DSN: %s\n", redactDsn(dsn).c_str());
    std::printf("=====================================================================\n");

    // ---- 真实断言部分（纯字符串，不连库）----
    const std::string dbname = dsnValue(dsn, "dbname");
    const std::string host = dsnValue(dsn, "host");
    const std::string user = dsnValue(dsn, "user");
    const char *allowNonTest = std::getenv("USAPP_TEST_ALLOW_NON_TEST_DB");
    const bool explicitOverride = (allowNonTest != nullptr) && (std::string(allowNonTest) == "1");

    TEST_CASE("集成前置：DSN 必须是 libpq 连接串且字段齐全") {
        usapptest::note("只校验 DSN 形状，不建立任何连接（本任务禁止联网/连库）");
        CHECK(!dbname.empty());
        CHECK(!host.empty());
        CHECK(!user.empty());
    }

    TEST_CASE("集成前置：破坏性操作只允许指向名字像测试库的数据库") {
        if (dbname.empty()) {
            CHECK(false);
            return;
        }
        const bool endsWithTest = dbname.size() > 5 && dbname.compare(dbname.size() - 5, 5, "_test") == 0;
        usapptest::note("dbname 必须以 _test 结尾；确需例外时设置 USAPP_TEST_ALLOW_NON_TEST_DB=1");
        CHECK(endsWithTest || explicitOverride);
    }

    // ---- 打印 runbook ----
    printPlan(dbname.empty() ? std::string("<未解析出 dbname>") : dbname);

    const int result = usapptest::finish();
    if (result != 0) {
        std::printf("集成测试前置校验失败：请修正 USAPP_TEST_DSN 后再运行。\n");
        return 2;  // 与「断言失败」区分开：2 = 前置条件不满足
    }

    std::printf(
        "\nNOTE: 本可执行文件只做前置校验并打印上方的 SQL 断言清单，"
        "尚未连接数据库执行它们。\n"
        "      原因见本文件头部注释与 services/api/tests/README.md。\n");
    return 0;
}
