// =====================================================================
// services/api/tests/TaskServiceDbTests.h
//
// 阶段 1 M3-B：真实 TaskService 的发布与读取（由 DbGatedTest.cc 的 main 调用，
// 位于 current_database() = 'usapp_test' 守卫之后）。
//
// 夹具（负责人 2026-10-09 批准的按次隔离已提交夹具）：
//   * 每次运行使用随机 uuid 与保留域名 @m3-fixture.test 的用户、空间与任务；
//     断言只针对本次生成的标识，从不断言全局计数或空表。
//   * 已提交的只有任务及其非钱包父行（users / couples / couple_members）与 task.created 审计。
//     任务表只追加，这些行永久保留（设计第 6.4 节）；本文件不删除、不绕过保护、不重置数据库。
//   * 需要完成记录的历史视图用例只在回滚事务中执行生产查询文本（TaskService::listSql），
//     从不提交钱包结算；没有临界余额夹具。
//
// 有界等待：服务调用、锁等待观察、截止时间等待与 teardown 都有上限。
// “持有发布者”使用独立的单连接客户端，不占用工作连接池（池只有两条连接）。
// =====================================================================
#pragma once

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "TaskLedgerDbTests.h"
#include "TestSupport.h"
#include "WalletLedgerDbTests.h"
#include "WalletLedgerProbe.h"
#include "db/TaskStartupCheck.h"
#include "services/TaskService.h"

namespace usapptest::taskservice {

using usapp::TaskService;
using wallet::acquireTx;
using wallet::CaseContext;
using wallet::ClientPtr;
using wallet::commitTx;
using wallet::exec;
using wallet::ExecOutcome;
using wallet::expectThat;
using wallet::kExecTimeout;
using wallet::Millis;
using wallet::newUuid;
using wallet::Observer;
using wallet::rollbackTx;
using wallet::startExec;
using wallet::teardownEnded;
using wallet::TxPtr;
using wallet::uuidLiteral;

constexpr const char *kFixtureDomain = "@m3-fixture.test";

// ---------------------------------------------------------------- 有界等待的服务调用
template <typename T>
class Pending {
  public:
    Pending() : future_(promise_.get_future().share()) {}

    void deliver(T value) {
        std::lock_guard<std::mutex> guard(mutex_);
        ++calls_;
        if (delivered_) {
            return;
        }
        delivered_ = true;
        promise_.set_value(std::move(value));
    }

    std::optional<T> wait(Millis timeout = kExecTimeout) {
        if (future_.wait_for(timeout) != std::future_status::ready) {
            return std::nullopt;
        }
        return future_.get();
    }

    bool readyWithin(Millis timeout) { return future_.wait_for(timeout) == std::future_status::ready; }

    int calls() {
        std::lock_guard<std::mutex> guard(mutex_);
        return calls_;
    }

  private:
    std::mutex mutex_;
    bool delivered_ = false;
    int calls_ = 0;
    std::promise<T> promise_;
    std::shared_future<T> future_;
};

using PendingStatus = Pending<TaskService::Status>;
using PendingList = Pending<TaskService::ListResult>;

inline std::shared_ptr<PendingStatus> startCreate(const std::string &userId,
                                                  const TaskService::CreateInput &input,
                                                  const std::string &key) {
    auto pending = std::make_shared<PendingStatus>();
    TaskService::instance().createTask(userId, input, key,
                                       [pending](TaskService::Status status) { pending->deliver(std::move(status)); });
    return pending;
}

inline std::optional<TaskService::Status> runCreate(const std::string &userId,
                                                    const TaskService::CreateInput &input,
                                                    const std::string &key) {
    return startCreate(userId, input, key)->wait();
}

inline std::optional<TaskService::ListResult> runList(const std::string &userId,
                                                      TaskService::View view,
                                                      const std::optional<TaskService::Cursor> &cursor,
                                                      int limit) {
    auto pending = std::make_shared<PendingList>();
    TaskService::instance().listTasks(userId, view, cursor, limit,
                                      [pending](TaskService::ListResult r) { pending->deliver(std::move(r)); });
    return pending->wait();
}

inline std::optional<TaskService::Status> runGet(const std::string &userId, const std::string &taskId) {
    auto pending = std::make_shared<PendingStatus>();
    TaskService::instance().getTask(userId, taskId,
                                    [pending](TaskService::Status s) { pending->deliver(std::move(s)); });
    return pending->wait();
}

inline bool outcomeIs(const std::optional<TaskService::Status> &status, TaskService::Outcome outcome) {
    return status.has_value() && status->outcome == outcome;
}

// ---------------------------------------------------------------- 已提交夹具
// 一个空间：A 在 slot 1，B（可选）在 slot 2。逐条自动提交（池上 execSqlSync，有 5 秒超时）。
struct CommittedSpace {
    std::string userA = newUuid();
    std::string userB = newUuid();
    std::string couple = newUuid();
};

inline void commitUser(const ClientPtr &pool, const std::string &id, const std::string &label) {
    const std::string email = "task_m3b_" + label + "_" + usapp::crypto::randomHex(6) + kFixtureDomain;
    pool->execSqlSync("INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $2, $2)", id, email);
}

inline void commitSpace(const ClientPtr &pool, const CommittedSpace &s, bool withPartner) {
    commitUser(pool, s.userA, "a");
    if (withPartner) {
        commitUser(pool, s.userB, "b");
    }
    pool->execSqlSync("INSERT INTO couples (id, name, created_by_user_id) VALUES ($1::uuid, 'm3b-fixture', $2::uuid)",
                      s.couple, s.userA);
    pool->execSqlSync("INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)", s.userA,
                      s.couple);
    if (withPartner) {
        pool->execSqlSync("INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 2)",
                          s.userB, s.couple);
    }
}

// Asia/Shanghai 本地日期偏移 days 天（服务器时钟）。
inline std::string localDate(const ClientPtr &pool, int days) {
    const auto rows = pool->execSqlSync(
        "SELECT to_char((now() AT TIME ZONE 'Asia/Shanghai')::date + $1::integer, 'YYYY-MM-DD') AS d",
        std::to_string(days));
    return rows[0]["d"].as<std::string>();
}

inline TaskService::CreateInput inputFrom(const Json::Value &body) {
    TaskService::CreateInput input;
    std::string message;
    if (!TaskService::parseCreateRequest(body, input, message)) {
        throw std::runtime_error("测试请求体未通过校验");
    }
    return input;
}

inline TaskService::CreateInput simpleInput(const std::string &title,
                                            int flowers,
                                            int sunshine,
                                            const std::string &localDateValue = "",
                                            const std::string &localTime = "") {
    Json::Value body(Json::objectValue);
    body["title"] = title;
    body["reward_flowers"] = flowers;
    body["reward_sunshine"] = sunshine;
    if (!localDateValue.empty()) {
        body["deadline"]["local_date"] = localDateValue;
        if (!localTime.empty()) {
            body["deadline"]["local_time"] = localTime;
        }
    }
    return inputFrom(body);
}

inline long long committedTasksForKey(const Observer &observer, const std::string &creator, const std::string &key) {
    const auto rows = observer.client->execSqlSync(
        "SELECT count(*)::bigint AS n FROM tasks WHERE creator_id = $1::uuid AND create_idempotency_key = $2",
        creator, key);
    return rows[0]["n"].as<long long>();
}

inline long long committedTasksByCreator(const Observer &observer, const std::string &creator) {
    const auto rows = observer.client->execSqlSync(
        "SELECT count(*)::bigint AS n FROM tasks WHERE creator_id = $1::uuid", creator);
    return rows[0]["n"].as<long long>();
}

inline long long createdAuditsByActor(const Observer &observer, const std::string &actor) {
    const auto rows = observer.client->execSqlSync(
        "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'task.created' AND actor_id = $1::uuid", actor);
    return rows[0]["n"].as<long long>();
}

inline long long auditsForTask(const Observer &observer, const std::string &taskId) {
    const auto rows = observer.client->execSqlSync(
        "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_type = 'task' AND target_id = $1", taskId);
    return rows[0]["n"].as<long long>();
}

// 直接插入一条任务（字面量 SQL；只用于已提交的列表夹具与回滚用例）。
// dateSql / timeSql / createdAtSql 是 SQL 表达式；deadline_at 与发布服务使用同一计算式。
inline std::string directTaskSql(const std::string &id,
                                 const std::string &couple,
                                 const std::string &creator,
                                 const std::string &assignee,
                                 const std::string &dateSql,
                                 const std::string &timeSql,
                                 const std::string &createdAtSql,
                                 int flowers = 1,
                                 int sunshine = 0,
                                 const std::string &key = task::taskKey(),
                                 const std::string &fingerprint = task::taskFingerprint()) {
    return "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers, "
           "reward_sunshine, timezone, deadline_local_date, deadline_local_time, deadline_at, "
           "create_idempotency_key, create_fingerprint, created_at) SELECT " +
           uuidLiteral(id) + ", " + uuidLiteral(couple) + ", " + uuidLiteral(creator) + ", " + uuidLiteral(assignee) +
           ", 'm3b task', '', " + std::to_string(flowers) + ", " + std::to_string(sunshine) +
           ", 'Asia/Shanghai', d.ld, d.lt, "
           "CASE WHEN d.ld IS NULL THEN NULL "
           "     WHEN d.lt IS NULL THEN (d.ld + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' "
           "     ELSE (d.ld + d.lt) AT TIME ZONE 'Asia/Shanghai' END, '" +
           key + "', '" + fingerprint + "', " + createdAtSql + " FROM (SELECT (" + dateSql + ")::date AS ld, (" +
           timeSql + ")::time AS lt) d";
}

// 只用于测试自己写的 ASCII 文本：拒绝引号、反斜杠与非 ASCII，保证可以安全内联为字面量。
inline std::string fixtureLiteral(const std::string &text) {
    for (const char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
        if (!ok) throw std::invalid_argument("fixture text must be plain ASCII");
    }
    return "'" + text + "'";
}

// 昨天/前几天某个北京时间时刻（SQL 表达式），用于模拟历史 created_at。
inline std::string historicalCreatedAtSql(int days, const std::string &hhmm) {
    if (hhmm.size() != 5) throw std::invalid_argument("hh:mm");
    return "(((now() AT TIME ZONE 'Asia/Shanghai')::date + " + std::to_string(days) + ") + time '" + hhmm +
           "') AT TIME ZONE 'Asia/Shanghai'";
}

// 一次“真实发布”应写入的行：所有业务字段、本地截止字段、deadline_at、键与指纹都来自同一个规范化请求。
// 只支持本文件构造的只有日期的 ASCII 请求。
inline std::string publicationRowSql(const std::string &id,
                                     const CommittedSpace &s,
                                     const TaskService::CreateInput &input,
                                     const std::string &key,
                                     const std::string &createdAtSql) {
    if (input.deadlineLocalDate.empty() || !input.deadlineLocalTime.empty()) {
        throw std::invalid_argument("publication fixtures use date-only deadlines");
    }
    // 日期已由 parseCreateRequest 校验为 YYYY-MM-DD；键必须满足服务端规则（只含 [A-Za-z0-9_-]），都可安全加引号。
    if (input.deadlineLocalDate.size() != 10 || input.deadlineLocalDate.find_first_not_of("0123456789-") != std::string::npos ||
        !TaskService::validIdempotencyKey(key)) {
        throw std::invalid_argument("fixture date or key shape");
    }
    const std::string date = "'" + input.deadlineLocalDate + "'";
    return "INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers, "
           "reward_sunshine, timezone, deadline_local_date, deadline_local_time, deadline_at, "
           "create_idempotency_key, create_fingerprint, created_at) VALUES (" +
           uuidLiteral(id) + ", " + uuidLiteral(s.couple) + ", " + uuidLiteral(s.userA) + ", " + uuidLiteral(s.userB) +
           ", " + fixtureLiteral(input.title) + ", " + fixtureLiteral(input.description) + ", " +
           std::to_string(input.rewardFlowers) + ", " + std::to_string(input.rewardSunshine) + ", 'Asia/Shanghai', " +
           date + "::date, NULL, (" + date + "::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai', " +
           "'" + key + "', '" + TaskService::createFingerprint(input) + "', " + createdAtSql + ")";
}

// 夹具自检：$1 任务 id，$2 请求的本地截止日期。deadline_at 用请求日期独立计算后比较。
constexpr const char *kPublicationCheckSql =
    "SELECT title, description, reward_flowers, reward_sunshine, "
    "       to_char(deadline_local_date, 'YYYY-MM-DD') AS ld, (deadline_local_time IS NULL) AS no_time, "
    "       create_idempotency_key AS k, create_fingerprint AS fp, "
    "       (deadline_at = (($2::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai')) AS at_matches, "
    "       (deadline_at <= clock_timestamp()) AS elapsed, (created_at < deadline_at) AS created_before "
    "FROM tasks WHERE id = $1::uuid";

inline bool expectPublicationMatches(const drogon::orm::Result &rows,
                                     const TaskService::CreateInput &input,
                                     const std::string &key,
                                     const std::string &label) {
    bool ok = rows.size() == 1u;
    if (ok) {
        const auto &r = rows[0];
        ok = r["title"].as<std::string>() == input.title && r["description"].as<std::string>() == input.description &&
             r["reward_flowers"].as<int>() == input.rewardFlowers &&
             r["reward_sunshine"].as<int>() == input.rewardSunshine &&
             r["ld"].as<std::string>() == input.deadlineLocalDate && r["no_time"].as<bool>() &&
             r["k"].as<std::string>() == key && r["fp"].as<std::string>() == TaskService::createFingerprint(input) &&
             r["at_matches"].as<bool>() && r["elapsed"].as<bool>() && r["created_before"].as<bool>();
    }
    expectThat(ok, label, "fixture self-check: stored row does not describe the request");
    return ok;
}

inline void expectTaskMatchesInput(const TaskService::TaskData &task,
                                   const TaskService::CreateInput &input,
                                   const std::string &label) {
    const bool ok = task.title == input.title && task.description == input.description &&
                    task.rewardFlowers == input.rewardFlowers && task.rewardSunshine == input.rewardSunshine &&
                    task.deadlineLocalDate == input.deadlineLocalDate && task.deadlineLocalTime.empty();
    expectThat(ok, label, "replayed task does not match the request");
}

inline std::string shanghaiDateSql(int days) {
    return "(now() AT TIME ZONE 'Asia/Shanghai')::date + " + std::to_string(days);
}

inline std::vector<std::string> idsOf(const std::vector<TaskService::TaskData> &tasks) {
    std::vector<std::string> ids;
    for (const auto &task : tasks) ids.push_back(task.id);
    return ids;
}

// 逐页读取，直到 next_cursor 为 null（最多 maxPages 页）。
struct PagedRead {
    bool ok = true;
    std::vector<std::vector<std::string>> pages;
    std::vector<std::optional<TaskService::Cursor>> cursors;  // 每页之后解码的游标
};

inline PagedRead readAllPages(const std::string &userId, TaskService::View view, int limit, int maxPages = 10) {
    PagedRead out;
    std::optional<TaskService::Cursor> cursor;
    for (int page = 0; page < maxPages; ++page) {
        const auto result = runList(userId, view, cursor, limit);
        if (!result || result->outcome != TaskService::Outcome::Ok) {
            out.ok = false;
            return out;
        }
        out.pages.push_back(idsOf(result->tasks));
        if (!result->nextCursor) {
            out.cursors.push_back(std::nullopt);
            return out;
        }
        cursor = TaskService::decodeCursor(*result->nextCursor, view);
        out.cursors.push_back(cursor);
        if (!cursor) {
            out.ok = false;
            return out;
        }
    }
    out.ok = false;  // 页数超过上限：分页没有终止
    return out;
}

inline std::vector<std::string> flatten(const std::vector<std::vector<std::string>> &pages) {
    std::vector<std::string> all;
    for (const auto &page : pages) all.insert(all.end(), page.begin(), page.end());
    return all;
}

// 观察者看到某个连接正在等待锁，且其语句是发布抢占（有界轮询）。
inline bool observeClaimLockWait(const Observer &observer, Millis timeout = Millis(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            const auto rows = observer.client->execSqlSync(
                "SELECT count(*)::integer AS n FROM pg_stat_activity "
                "WHERE datname = current_database() AND pid <> pg_backend_pid() AND wait_event_type = 'Lock' "
                "  AND query LIKE '%ON CONFLICT (creator_id, create_idempotency_key)%'");
            if (rows[0]["n"].as<int>() > 0) {
                return true;
            }
        } catch (const std::exception &) {
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(wallet::kPollStep);
    }
}

// ---------------------------------------------------------------- 用例
inline void runTaskServiceDbTests(const ClientPtr &pool, const std::string &dsn) {
    std::optional<Observer> observerHolder;
    try {
        observerHolder = Observer::connect(dsn);
    } catch (const std::exception &) {
        TEST_CASE("Task service: observer acquisition") { CHECK(false); }
        return;
    }
    const auto &observer = *observerHolder;

    // -----------------------------------------------------------------
    TEST_CASE("Task service: 发布推导接收者、同事务审计、重放、键复用、空间与同伴错误、新发布的过期截止") {
        const std::string label = "task-publish";
        try {
            CommittedSpace s1, single;
            commitSpace(pool, s1, true);
            commitSpace(pool, single, false);
            const std::string unbound = newUuid();
            commitUser(pool, unbound, "unbound");

            Json::Value body(Json::objectValue);
            body["title"] = "  Water the plants  ";
            body["description"] = "line1\r\nline2\n";
            body["reward_flowers"] = 3;
            body["reward_sunshine"] = 0;
            body["deadline"]["local_date"] = localDate(pool, 3);
            body["deadline"]["local_time"] = "21:30";
            const auto input = inputFrom(body);
            const std::string key = newUuid();

            // A. 新发布：201 语义，接收者是同伴，内容已规范化。
            const auto created = runCreate(s1.userA, input, key);
            CHECK(outcomeIs(created, TaskService::Outcome::Created));
            if (!outcomeIs(created, TaskService::Outcome::Created)) throw std::runtime_error("发布未成功");
            const auto &task = created->task;
            CHECK_EQ(task.creatorId, s1.userA);
            CHECK_EQ(task.assigneeId, s1.userB);
            CHECK_EQ(task.title, std::string("Water the plants"));
            CHECK_EQ(task.description, std::string("line1\nline2"));
            CHECK_EQ(task.rewardFlowers, 3);
            CHECK_EQ(task.rewardSunshine, 0);
            CHECK_EQ(task.deadlineLocalTime, std::string("21:30"));
            CHECK_EQ(task.status, std::string("open"));
            CHECK(task.completedAt.empty());
            CHECK(TaskService::isCursorTimestamp(task.deadlineAt));
            CHECK(TaskService::isCursorTimestamp(task.createdAt));

            // 已提交行：空间、接收者、键与服务指纹。
            const auto stored = observer.client->execSqlSync(
                "SELECT couple_id::text AS couple_id, assignee_id::text AS assignee_id, create_fingerprint, "
                "       to_char(deadline_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS at, "
                "       to_char(((deadline_local_date + deadline_local_time) AT TIME ZONE 'Asia/Shanghai') "
                "               AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS expected_at "
                "FROM tasks WHERE id = $1::uuid",
                task.id);
            CHECK_EQ(stored.size(), 1u);
            if (stored.size() == 1u) {
                CHECK_EQ(stored[0]["couple_id"].as<std::string>(), s1.couple);
                CHECK_EQ(stored[0]["assignee_id"].as<std::string>(), s1.userB);
                CHECK_EQ(stored[0]["create_fingerprint"].as<std::string>(), TaskService::createFingerprint(input));
                CHECK_EQ(stored[0]["at"].as<std::string>(), task.deadlineAt);
                CHECK_EQ(stored[0]["expected_at"].as<std::string>(), task.deadlineAt);
            }

            // B. 审计：恰好一条，与任务同一事务（xmin 相同），detail 不含标题与描述文本。
            const auto audit = observer.client->execSqlSync(
                "SELECT a.actor_type, a.actor_id::text AS actor_id, a.couple_id::text AS couple_id, "
                "       (SELECT string_agg(k, ',' ORDER BY k) FROM jsonb_object_keys(a.detail) k) AS keys, "
                "       a.detail->>'reward_flowers' AS rf, a.detail->>'has_deadline' AS hd, "
                "       (position($2 in a.detail::text) > 0 OR position($3 in a.detail::text) > 0) AS leaks, "
                "       (a.xmin::text = t.xmin::text) AS same_tx "
                "FROM audit_logs a JOIN tasks t ON t.id::text = a.target_id "
                "WHERE a.action = 'task.created' AND a.target_type = 'task' AND a.target_id = $1",
                task.id, std::string("Water"), std::string("line1"));
            CHECK_EQ(audit.size(), 1u);
            if (audit.size() == 1u) {
                CHECK_EQ(audit[0]["actor_type"].as<std::string>(), std::string("user"));
                CHECK_EQ(audit[0]["actor_id"].as<std::string>(), s1.userA);
                CHECK_EQ(audit[0]["couple_id"].as<std::string>(), s1.couple);
                CHECK_EQ(audit[0]["keys"].as<std::string>(), std::string("has_deadline,reward_flowers,reward_sunshine"));
                CHECK_EQ(audit[0]["rf"].as<std::string>(), std::string("3"));
                CHECK_EQ(audit[0]["hd"].as<std::string>(), std::string("true"));
                CHECK_FALSE(audit[0]["leaks"].as<bool>());
                CHECK(audit[0]["same_tx"].as<bool>());
            }

            // C. 同键同内容：重放，返回已保存的任务，不写新行或审计。
            const auto replay = runCreate(s1.userA, input, key);
            CHECK(outcomeIs(replay, TaskService::Outcome::Replayed));
            if (replay) CHECK(TaskService::taskJson(replay->task) == TaskService::taskJson(task));
            CHECK_EQ(committedTasksForKey(observer, s1.userA, key), 1);
            CHECK_EQ(auditsForTask(observer, task.id), 1);

            // D. 同键不同内容：IDEMPOTENCY_KEY_REUSED，什么都不写。
            auto changed = input;
            changed.rewardFlowers = 4;
            const auto reused = runCreate(s1.userA, changed, key);
            CHECK(outcomeIs(reused, TaskService::Outcome::KeyReused));
            CHECK_EQ(committedTasksForKey(observer, s1.userA, key), 1);
            CHECK_EQ(createdAuditsByActor(observer, s1.userA), 1);

            // E. 键的作用域是发布者：同伴用同一个键值发布是另一条新任务，接收者是 A。
            const auto partner = runCreate(s1.userB, input, key);
            CHECK(outcomeIs(partner, TaskService::Outcome::Created));
            if (outcomeIs(partner, TaskService::Outcome::Created)) {
                CHECK_EQ(partner->task.assigneeId, s1.userA);
                CHECK_NE(partner->task.id, task.id);
                CHECK_EQ(auditsForTask(observer, partner->task.id), 1);
            }

            // F. 只有一名成员：PARTNER_REQUIRED；没有空间：SPACE_NOT_FOUND；都不写入。
            CHECK(outcomeIs(runCreate(single.userA, input, newUuid()), TaskService::Outcome::PartnerRequired));
            CHECK_EQ(committedTasksByCreator(observer, single.userA), 0);
            CHECK(outcomeIs(runCreate(unbound, input, newUuid()), TaskService::Outcome::SpaceNotFound));
            CHECK_EQ(committedTasksByCreator(observer, unbound), 0);
            CHECK_EQ(createdAuditsByActor(observer, single.userA) + createdAuditsByActor(observer, unbound), 0);

            // G. 新发布的截止时间不在未来：400，候选行随回滚消失，不写审计。
            const std::string expiredKey = newUuid();
            const auto expired = runCreate(s1.userA, simpleInput("Late", 1, 0, localDate(pool, -1)), expiredKey);
            CHECK(outcomeIs(expired, TaskService::Outcome::Validation));
            CHECK_EQ(committedTasksForKey(observer, s1.userA, expiredKey), 0);
            CHECK_EQ(createdAuditsByActor(observer, s1.userA), 1);

            // H. 只有日期：截止于次日 0 点（Asia/Shanghai）；今天的日期仍在未来。
            const std::string today = localDate(pool, 0);
            const auto dateOnly = runCreate(s1.userA, simpleInput("Today", 0, 7, today), newUuid());
            CHECK(outcomeIs(dateOnly, TaskService::Outcome::Created));
            if (outcomeIs(dateOnly, TaskService::Outcome::Created)) {
                const auto expectedAt = observer.client->execSqlSync(
                    "SELECT to_char((($1::date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai') AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS at",
                    today);
                CHECK_EQ(dateOnly->task.deadlineAt, expectedAt[0]["at"].as<std::string>());
                CHECK(dateOnly->task.deadlineLocalTime.empty());
            }

            // I. 读取：双方都能读到；其他空间的成员与不存在的 id 一律 TASK_NOT_FOUND；没有空间 SPACE_NOT_FOUND。
            CHECK(outcomeIs(runGet(s1.userA, task.id), TaskService::Outcome::Ok));
            const auto partnerGet = runGet(s1.userB, task.id);
            CHECK(outcomeIs(partnerGet, TaskService::Outcome::Ok));
            if (outcomeIs(partnerGet, TaskService::Outcome::Ok)) {
                CHECK(TaskService::taskJson(partnerGet->task) == TaskService::taskJson(task));
            }
            CHECK(outcomeIs(runGet(single.userA, task.id), TaskService::Outcome::TaskNotFound));
            CHECK(outcomeIs(runGet(s1.userA, newUuid()), TaskService::Outcome::TaskNotFound));
            CHECK(outcomeIs(runGet(unbound, task.id), TaskService::Outcome::SpaceNotFound));
            const auto noSpaceList = runList(unbound, TaskService::View::Open, std::nullopt, 10);
            CHECK(noSpaceList.has_value() && noSpaceList->outcome == TaskService::Outcome::SpaceNotFound);
            const auto singleList = runList(single.userA, TaskService::View::Open, std::nullopt, 10);
            CHECK(singleList.has_value() && singleList->outcome == TaskService::Outcome::Ok);
            if (singleList) CHECK(singleList->tasks.empty());
        } catch (const std::exception &) {
            expectThat(false, label, "publish case failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task service: 同键并发发布只提交一条任务，另一个请求等待后重放") {
        const std::string label = "task-concurrent-publish";
        try {
            CommittedSpace s;
            commitSpace(pool, s, true);
            const auto input = simpleInput("Concurrent", 2, 0);
            const std::string key = newUuid();
            auto first = startCreate(s.userA, input, key);
            auto second = startCreate(s.userA, input, key);
            const auto a = first->wait();
            const auto b = second->wait();
            CHECK(a.has_value() && b.has_value());
            if (a && b) {
                const int createdCount = (a->outcome == TaskService::Outcome::Created ? 1 : 0) +
                                         (b->outcome == TaskService::Outcome::Created ? 1 : 0);
                const int replayCount = (a->outcome == TaskService::Outcome::Replayed ? 1 : 0) +
                                        (b->outcome == TaskService::Outcome::Replayed ? 1 : 0);
                CHECK_EQ(createdCount, 1);
                CHECK_EQ(replayCount, 1);
                CHECK_EQ(a->task.id, b->task.id);
            }
            CHECK_EQ(committedTasksForKey(observer, s.userA, key), 1);
            CHECK_EQ(createdAuditsByActor(observer, s.userA), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "concurrent publish case failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    // 持有发布者（设计第 6.3 节），夹具自洽：
    //   * 被持有的行就是“同一次发布”本身：标题、描述、奖励、本地截止日期、按 Asia/Shanghai 计算的
    //     deadline_at、键与服务规范指纹全部来自同一个规范化请求（publicationRowSql），插入后自检。
    //   * 模拟的原始时序（不是观察到的）：请求的截止日期是昨天（只有日期，截止于今天 0 点，已过），
    //     行的 created_at 设为昨天 08:00（北京时间），表示原发布在截止前已经被接受，只是尚未提交。
    //   * 观察到的部分：真实发布以同一键与同一请求体调用，300 ms 内不得结束，观察者看到抢占语句的
    //     Lock 等待；随后提交原事务 -> 重放（不是 400）；回滚原事务 -> 作为新请求按截止时间校验 -> 400。
    // 不依赖任何秒级截止时间或睡眠等待，API 的时间精度与测试超时都不变。
    // -----------------------------------------------------------------
    for (const bool commitHolder : {true, false}) {
        TEST_CASE(commitHolder ? "Task service: 持有的原发布（截止时间已过）提交后，等待中的同键发布返回重放"
                               : "Task service: 持有的原发布回滚后，等待中的同键发布按新请求校验截止时间并返回 400") {
            const std::string label = commitHolder ? "task-held-commit" : "task-held-rollback";
            std::optional<Observer> holderConn;
            TxPtr holder;
            int holderPid = 0;
            try {
                CommittedSpace s;
                commitSpace(pool, s, true);
                const auto input = simpleInput("Held publish", 1, 0, localDate(pool, -1));
                const std::string key = newUuid();
                const std::string id = newUuid();
                holderConn = Observer::connect(dsn);  // 独立单连接客户端，不占用工作连接池
                holder = acquireTx(holderConn->client);
                if (!holder) throw std::runtime_error("holder acquisition failed");
                const auto pidOut = exec(holder, "SELECT pg_backend_pid() AS pid");
                if (!pidOut.ok || !pidOut.result) throw std::runtime_error("holder pid failed");
                holderPid = (*pidOut.result)[0]["pid"].as<int>();

                const auto inserted = exec(holder, publicationRowSql(id, s, input, key, historicalCreatedAtSql(-1, "08:00")));
                if (!inserted.ok) throw std::runtime_error("holder insert failed");
                const auto self = exec(holder, kPublicationCheckSql, id, input.deadlineLocalDate);
                if (!self.ok || !self.result) throw std::runtime_error("holder self-check query failed");
                if (!expectPublicationMatches(*self.result, input, key, label)) {
                    throw std::runtime_error("holder fixture is not self-consistent");
                }

                auto publish = startCreate(s.userA, input, key);
                CHECK_FALSE(publish->readyWithin(Millis(300)));
                CHECK(observeClaimLockWait(observer));

                if (commitHolder) {
                    const auto committed = commitTx(holder);
                    CHECK(committed.has_value() && *committed);
                } else {
                    rollbackTx(holder);
                }
                const auto result = publish->wait();
                if (commitHolder) {
                    CHECK(outcomeIs(result, TaskService::Outcome::Replayed));
                    if (result) {
                        CHECK_EQ(result->task.id, id);
                        CHECK_EQ(result->task.status, std::string("expired"));
                        expectTaskMatchesInput(result->task, input, label);
                    }
                    CHECK_EQ(committedTasksForKey(observer, s.userA, key), 1);
                    CHECK_EQ(auditsForTask(observer, id), 0);
                } else {
                    CHECK(outcomeIs(result, TaskService::Outcome::Validation));
                    CHECK_EQ(committedTasksForKey(observer, s.userA, key), 0);
                }
                CHECK_EQ(createdAuditsByActor(observer, s.userA), 0);
                CHECK_EQ(publish->calls(), 1);
            } catch (const std::exception &) {
                expectThat(false, label, "held publisher case failed (details withheld)");
            }
            rollbackTx(holder);  // 已提交或已回滚时为无操作
            if (holderPid && holderConn) {
                const auto teardown = teardownEnded(observer, holderPid);
                expectThat(teardown.ended, label, "holder teardown: " + teardown.observed);
            }
        }
    }

    // -----------------------------------------------------------------
    // 已提交的原发布（自洽夹具：三天前到期，四天前 09:00 创建），用同键同内容再次发布：
    // 重放已保存的任务，不做截止时间检查。
    // -----------------------------------------------------------------
    TEST_CASE("Task service: 已提交且截止时间已过的发布，用同键同内容再次发布仍返回重放") {
        const std::string label = "task-replay-after-deadline";
        try {
            CommittedSpace s;
            commitSpace(pool, s, true);
            const auto input = simpleInput("Past publish", 2, 0, localDate(pool, -3));
            const std::string key = newUuid();
            const std::string id = newUuid();
            pool->execSqlSync(publicationRowSql(id, s, input, key, historicalCreatedAtSql(-4, "09:00")));
            const auto self = pool->execSqlSync(kPublicationCheckSql, id, input.deadlineLocalDate);
            if (!expectPublicationMatches(self, input, key, label)) {
                throw std::runtime_error("committed fixture is not self-consistent");
            }
            const auto replay = runCreate(s.userA, input, key);
            CHECK(outcomeIs(replay, TaskService::Outcome::Replayed));
            if (replay) {
                CHECK_EQ(replay->task.id, id);
                CHECK_EQ(replay->task.status, std::string("expired"));
                expectTaskMatchesInput(replay->task, input, label);
            }
            CHECK_EQ(committedTasksForKey(observer, s.userA, key), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "replay after deadline failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    TEST_CASE("Task service: open 视图跨有/无截止时间与并列键的分页、游标、外部游标与 history 过期边界") {
        const std::string label = "task-pagination";
        try {
            CommittedSpace s, other;
            commitSpace(pool, s, true);
            commitSpace(pool, other, true);
            const std::string c1 = "'2025-12-01T00:00:00.000001Z'::timestamptz";
            const std::string c2 = "'2025-12-02T00:00:00.000002Z'::timestamptz";
            const std::string c3 = "'2025-12-03T00:00:00.000003Z'::timestamptz";
            const std::string c5 = "'2025-12-05T00:00:00.000005Z'::timestamptz";
            const std::string c0 = "'2025-11-01T00:00:00.000000Z'::timestamptz";

            const std::string d1 = newUuid(), d2 = newUuid(), d3 = newUuid();
            const std::string u1 = newUuid(), u2 = newUuid(), u3 = newUuid(), u4 = newUuid();
            const std::string e1 = newUuid(), e2 = newUuid(), e3 = newUuid(), e4 = newUuid(), e5 = newUuid();
            const std::string foreign = newUuid();
            const std::vector<std::string> inserts = {
                directTaskSql(d1, s.couple, s.userA, s.userB, shanghaiDateSql(10), "'09:00'", c2),
                // d2 与 d3：截止时间与创建时间都相同，由 id 决定先后
                directTaskSql(d2, s.couple, s.userB, s.userA, shanghaiDateSql(20), "'09:00'", c1),
                directTaskSql(d3, s.couple, s.userA, s.userB, shanghaiDateSql(20), "'09:00'", c1),
                directTaskSql(u1, s.couple, s.userA, s.userB, "NULL", "NULL", c3),
                directTaskSql(u2, s.couple, s.userB, s.userA, "NULL", "NULL", c3),
                directTaskSql(u3, s.couple, s.userA, s.userB, "NULL", "NULL", c1),
                directTaskSql(u4, s.couple, s.userA, s.userB, "NULL", "NULL", c5),
                // 过期（只有日期）：e1 昨天到期；e2/e3 同一时刻到期（id 决定）；e5 28 天前到期；
                // e4 31 天前到期，不在 30 天窗口内。创建时间都在很久以前（窗口不按创建时间）。
                directTaskSql(e1, s.couple, s.userA, s.userB, shanghaiDateSql(-2), "NULL", c0),
                directTaskSql(e2, s.couple, s.userA, s.userB, shanghaiDateSql(-5), "NULL", c0),
                directTaskSql(e3, s.couple, s.userB, s.userA, shanghaiDateSql(-5), "NULL", c0),
                directTaskSql(e4, s.couple, s.userA, s.userB, shanghaiDateSql(-32), "NULL", c0),
                directTaskSql(e5, s.couple, s.userA, s.userB, shanghaiDateSql(-29), "NULL", c0),
                directTaskSql(foreign, other.couple, other.userA, other.userB, "NULL", "NULL", c0),
            };
            for (const auto &sql : inserts) pool->execSqlSync(sql);

            auto byIdAsc = [](std::string a, std::string b) {
                if (a > b) std::swap(a, b);
                return std::vector<std::string>{a, b};
            };
            std::vector<std::string> expectedOpen = {d1};
            for (const auto &id : byIdAsc(d2, d3)) expectedOpen.push_back(id);
            expectedOpen.push_back(u3);
            for (const auto &id : byIdAsc(u1, u2)) expectedOpen.push_back(id);
            expectedOpen.push_back(u4);

            // limit 2：四页；第二页内发生“有截止 -> 无截止”的过渡；之后在无截止任务内继续。
            const auto two = readAllPages(s.userA, TaskService::View::Open, 2);
            CHECK(two.ok);
            CHECK_EQ(two.pages.size(), 4u);
            CHECK(flatten(two.pages) == expectedOpen);
            if (two.cursors.size() == 4u) {
                CHECK(two.cursors[0].has_value() && two.cursors[0]->deadlineAt.has_value());
                CHECK(two.cursors[1].has_value() && !two.cursors[1]->deadlineAt.has_value());
                CHECK(two.cursors[2].has_value() && !two.cursors[2]->deadlineAt.has_value());
                CHECK_FALSE(two.cursors[3].has_value());
                if (two.cursors[0]) CHECK_EQ(two.cursors[0]->id, expectedOpen[1]);
            }
            // limit 3：第一页恰好结束在最后一个有截止任务上，下一页从无截止任务开始。
            const auto three = readAllPages(s.userA, TaskService::View::Open, 3);
            CHECK(three.ok);
            CHECK_EQ(three.pages.size(), 3u);
            CHECK(flatten(three.pages) == expectedOpen);
            if (!three.cursors.empty() && three.cursors[0]) {
                CHECK(three.cursors[0]->deadlineAt.has_value());
                CHECK_EQ(three.cursors[0]->id, expectedOpen[2]);
            }
            // 同伴看到同一空间的同一集合（D8）。
            const auto partnerPages = readAllPages(s.userB, TaskService::View::Open, 100);
            CHECK(partnerPages.ok && flatten(partnerPages.pages) == expectedOpen);

            // 外部游标：用另一空间任务的值构造合法游标，结果仍只含调用者空间的任务。
            TaskService::Cursor foreignCursor;
            foreignCursor.view = TaskService::View::Open;
            foreignCursor.createdAt = "2025-11-01T00:00:00.000000Z";
            foreignCursor.id = foreign;
            const auto foreignPage = runList(s.userA, TaskService::View::Open,
                                             TaskService::decodeCursor(TaskService::encodeCursor(foreignCursor),
                                                                       TaskService::View::Open),
                                             100);
            CHECK(foreignPage.has_value() && foreignPage->outcome == TaskService::Outcome::Ok);
            if (foreignPage) {
                const std::vector<std::string> undated = {expectedOpen[3], expectedOpen[4], expectedOpen[5],
                                                          expectedOpen[6]};
                CHECK(idsOf(foreignPage->tasks) == undated);
            }
            // 另一空间的成员用本空间最后一行的游标：只看到自己空间的任务。
            const auto otherPages = runList(other.userA, TaskService::View::Open, std::nullopt, 100);
            CHECK(otherPages.has_value() && otherPages->outcome == TaskService::Outcome::Ok);
            if (otherPages) CHECK(idsOf(otherPages->tasks) == std::vector<std::string>{foreign});
            // 视图与游标不匹配：服务直接拒绝，不查询。
            TaskService::Cursor historyCursor;
            historyCursor.view = TaskService::View::History;
            historyCursor.historyAt = "2026-01-01T00:00:00.000000Z";
            historyCursor.id = d1;
            const auto mismatch = runList(s.userA, TaskService::View::Open, historyCursor, 10);
            CHECK(mismatch.has_value() && mismatch->outcome == TaskService::Outcome::Validation);

            // history：只含 30 天内到期的过期任务，按 (到期时间 DESC, id DESC)。
            std::vector<std::string> expectedHistory = {e1};
            {
                auto pair = byIdAsc(e2, e3);
                expectedHistory.push_back(pair[1]);
                expectedHistory.push_back(pair[0]);
            }
            expectedHistory.push_back(e5);
            const auto history = readAllPages(s.userA, TaskService::View::History, 2);
            CHECK(history.ok);
            CHECK_EQ(history.pages.size(), 2u);
            CHECK(flatten(history.pages) == expectedHistory);
            if (!history.cursors.empty() && history.cursors[0]) {
                CHECK_EQ(history.cursors[0]->id, expectedHistory[1]);
                CHECK(TaskService::isCursorTimestamp(history.cursors[0]->historyAt));
            }
            for (const auto &id : flatten(history.pages)) {
                CHECK(id != e4);
            }
            const auto historyStatuses = runList(s.userA, TaskService::View::History, std::nullopt, 100);
            if (historyStatuses) {
                for (const auto &task : historyStatuses->tasks) {
                    CHECK_EQ(task.status, std::string("expired"));
                }
            }
        } catch (const std::exception &) {
            expectThat(false, label, "pagination case failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    // 需要完成记录的 history 用例：只在回滚事务中执行生产查询文本，从不提交钱包结算。
    // -----------------------------------------------------------------
    TEST_CASE("Task service: history 生产查询包含 30 天内完成的旧任务、排除窗口外完成，完成任务不在 open（回滚）") {
        const std::string label = "task-history-completed";
        CaseContext ctx;
        try {
            if (wallet::openCase(pool, ctx, label)) {
                const auto &f = ctx.f;
                const std::string tRecent = newUuid(), tOld = newUuid(), tExpired = newUuid(), tOpen = newUuid();
                const std::string opOld = newUuid(), opRecent = newUuid();
                const std::vector<std::string> setup = {
                    // 40 天前创建、1 小时前完成：窗口按完成时间，应出现。
                    directTaskSql(tRecent, f.coupleOne, f.userA, f.userB, "NULL", "NULL", "now() - interval '40 days'",
                                  2, 0),
                    // 50 天前创建、31 天前完成：不在窗口内。
                    directTaskSql(tOld, f.coupleOne, f.userA, f.userB, "NULL", "NULL", "now() - interval '50 days'",
                                  1, 0),
                    directTaskSql(tExpired, f.coupleOne, f.userA, f.userB, shanghaiDateSql(-4), "NULL",
                                  "now() - interval '10 days'", 1, 0),
                    directTaskSql(tOpen, f.coupleOne, f.userA, f.userB, "NULL", "NULL", "now() - interval '1 day'", 1,
                                  0),
                    wallet::accountSql(f.userB, f.coupleOne, "flower"),
                    task::taskOperationSql(opOld, f.coupleOne, f.userB, tOld),
                    task::completionSql(tOld, f.coupleOne, f.userB, opOld, 1, 0, "now() - interval '31 days'"),
                    wallet::entrySql(opOld, f.coupleOne, f.userB, "flower", 1, 1, 1),
                    task::taskOperationSql(opRecent, f.coupleOne, f.userB, tRecent),
                    task::completionSql(tRecent, f.coupleOne, f.userB, opRecent, 2, 0, "now() - interval '1 hour'"),
                    wallet::entrySql(opRecent, f.coupleOne, f.userB, "flower", 2, 2, 3),
                    "SET CONSTRAINTS ALL IMMEDIATE",  // 夹具本身必须是合法结算
                };
                if (wallet::runSetup(ctx, setup, label)) {
                    using Q = TaskService::ListQuery;
                    auto rowsOf = [&](const ExecOutcome &out) {
                        std::vector<std::vector<std::string>> rows;  // id, status, history_at, completed_at
                        if (!out.ok || !out.result) return rows;
                        for (const auto &row : *out.result) {
                            if (row["id"].isNull()) continue;
                            rows.push_back({row["id"].as<std::string>(), row["status"].as<std::string>(),
                                            row["history_at"].isNull() ? "" : row["history_at"].as<std::string>(),
                                            row["completed_at"].isNull() ? "" : row["completed_at"].as<std::string>()});
                        }
                        return rows;
                    };
                    const auto first = rowsOf(exec(ctx.tx, TaskService::listSql(Q::HistoryFirst), f.userA, std::string("2")));
                    CHECK_EQ(first.size(), 2u);
                    if (first.size() == 2u) {
                        CHECK_EQ(first[0][0], tRecent);
                        CHECK_EQ(first[0][1], std::string("completed"));
                        CHECK_EQ(first[0][2], first[0][3]);  // history_at = completed_at
                        CHECK_EQ(first[1][0], tExpired);
                        CHECK_EQ(first[1][1], std::string("expired"));
                        // limit 1 的下一页：用第一行构造游标，执行生产续页查询。
                        const auto next = rowsOf(exec(ctx.tx, TaskService::listSql(Q::HistoryAfter), f.userA,
                                                      first[0][2], first[0][0], std::string("2")));
                        CHECK_EQ(next.size(), 1u);
                        if (next.size() == 1u) CHECK_EQ(next[0][0], tExpired);
                    }
                    const auto all = rowsOf(exec(ctx.tx, TaskService::listSql(Q::HistoryFirst), f.userB, std::string("101")));
                    CHECK_EQ(all.size(), 2u);
                    for (const auto &row : all) CHECK(row[0] != tOld && row[0] != tOpen);
                    // 完成的任务不在 open 视图；进行中的任务在。
                    const auto open = exec(ctx.tx, TaskService::listSql(Q::OpenFirst), f.userA, std::string("101"));
                    CHECK(open.ok && open.result.has_value());
                    if (open.ok && open.result) {
                        std::vector<std::string> openIds;
                        for (const auto &row : *open.result) {
                            if (!row["id"].isNull()) openIds.push_back(row["id"].as<std::string>());
                        }
                        CHECK(openIds == std::vector<std::string>{tOpen});
                    }
                    // 成员关系只取自 $1：另一空间的 C 看不到任何一条。
                    const auto foreign = exec(ctx.tx, TaskService::listSql(Q::HistoryFirst), f.userC, std::string("101"));
                    CHECK(foreign.ok && foreign.result.has_value() && foreign.result->size() == 1u);
                    if (foreign.ok && foreign.result && foreign.result->size() == 1u) {
                        CHECK((*foreign.result)[0]["id"].isNull());
                    }
                    // 详情：完成任务的状态与完成时间。
                    const auto detail = exec(ctx.tx, TaskService::getSql(), f.userB, tRecent);
                    CHECK(detail.ok && detail.result.has_value() && detail.result->size() == 1u);
                    if (detail.ok && detail.result && detail.result->size() == 1u) {
                        CHECK_EQ((*detail.result)[0]["status"].as<std::string>(), std::string("completed"));
                        CHECK_FALSE((*detail.result)[0]["completed_at"].isNull());
                    }
                }
            }
        } catch (const std::exception &) {
            expectThat(false, label, "rollback-only history case failed");
        }
        task::closeTaskCase(observer, ctx, label);
    }

    // -----------------------------------------------------------------
    // 生命周期：结果回调按值持有哨兵；成功提交、重放回滚、SQL 失败三条路径结束后，
    // 哨兵都要在有界时间内释放（无 ctx -> TxScope -> onError_ -> ctx 环），且回调恰好一次。
    // 提交失败（RESULT_UNKNOWN）无法在测试库中可靠触发，只做代码审查。
    // -----------------------------------------------------------------
    TEST_CASE("Task service: 成功 / 重放 / SQL 失败结束后释放回调持有的哨兵，回调恰好一次") {
        const std::string label = "task-lifetime";
        try {
            CommittedSpace s;
            commitSpace(pool, s, true);
            const auto input = simpleInput("Lifetime", 1, 0);
            const std::string key = newUuid();

            struct Lifetime {
                std::optional<TaskService::Status> status;
                int calls = 0;
                bool released = false;
            };
            auto runWithSentinel = [&input](const std::string &userId, const std::string &k) {
                auto pending = std::make_shared<PendingStatus>();
                auto sentinel = std::make_shared<int>(7);
                const std::weak_ptr<int> watch = sentinel;
                TaskService::instance().createTask(userId, input, k,
                                                   [pending, sentinel](TaskService::Status status) {
                                                       pending->deliver(std::move(status));
                                                   });
                sentinel.reset();
                Lifetime out;
                out.status = pending->wait();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!watch.expired() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(Millis(10));
                }
                out.released = watch.expired();
                out.calls = pending->calls();
                return out;
            };

            const auto created = runWithSentinel(s.userA, key);
            CHECK(outcomeIs(created.status, TaskService::Outcome::Created));
            CHECK_EQ(created.calls, 1);
            CHECK(created.released);

            const auto replay = runWithSentinel(s.userA, key);
            CHECK(outcomeIs(replay.status, TaskService::Outcome::Replayed));
            CHECK_EQ(replay.calls, 1);
            CHECK(replay.released);

            const auto sqlError = runWithSentinel("not-a-uuid", newUuid());
            CHECK(outcomeIs(sqlError.status, TaskService::Outcome::Error));
            if (sqlError.status) {
                CHECK_EQ(sqlError.status->detail, std::string("task_create stage=membership reason=db_error"));
            }
            CHECK_EQ(sqlError.calls, 1);
            CHECK(sqlError.released);

            CHECK_EQ(committedTasksForKey(observer, s.userA, key), 1);
        } catch (const std::exception &) {
            expectThat(false, label, "lifetime case failed (details withheld)");
        }
    }

    // -----------------------------------------------------------------
    // 错误详情只含应用自定义的固定标签：数据库对非法 uuid 的报错会原样回显输入值，
    // 这里让三条路径真实触发这种报错，并确认标记串与数据库错误文本都不进入 detail / message。
    // -----------------------------------------------------------------
    TEST_CASE("Task service: 数据库错误不把输入或原始错误文本带进 detail（固定阶段/原因标签）") {
        const std::string label = "task-error-detail";
        try {
            CommittedSpace s;
            commitSpace(pool, s, true);
            const std::string marker = "MarkerEcho" + usapp::crypto::randomHex(4);
            auto clean = [&marker](const std::string &text) {
                return text.find(marker) == std::string::npos && text.find("uuid") == std::string::npos &&
                       text.find("ERROR") == std::string::npos && text.find("invalid") == std::string::npos &&
                       text.find("syntax") == std::string::npos;
            };

            auto withTitle = simpleInput(marker + " title", 1, 0);
            withTitle.description = marker + " description";
            const auto create = runCreate("not-a-uuid-" + marker, withTitle, newUuid());
            CHECK(outcomeIs(create, TaskService::Outcome::Error));
            if (create) {
                CHECK_EQ(create->detail, std::string("task_create stage=membership reason=db_error"));
                CHECK(clean(create->detail) && clean(create->message));
            }

            const auto get = runGet(s.userA, marker);
            CHECK(outcomeIs(get, TaskService::Outcome::Error));
            if (get) {
                CHECK_EQ(get->detail, std::string("task_get stage=query reason=db_error"));
                CHECK(clean(get->detail) && clean(get->message));
            }

            TaskService::Cursor bad;  // 服务层直接构造；控制器的严格解码不会放行这种 id
            bad.view = TaskService::View::Open;
            bad.createdAt = "2026-01-01T00:00:00.000000Z";
            bad.id = marker;
            const auto list = runList(s.userA, TaskService::View::Open, bad, 10);
            CHECK(list.has_value() && list->outcome == TaskService::Outcome::Error);
            if (list) {
                CHECK_EQ(list->detail, std::string("task_list stage=query reason=db_error"));
                CHECK(clean(list->detail) && clean(list->message));
            }
            CHECK_EQ(committedTasksByCreator(observer, s.userA), 0);
        } catch (const std::exception &) {
            expectThat(false, label, "error detail case failed (details withheld)");
        }
    }

    TEST_CASE("Task startup: usapp_test 上任务表有效权限矩阵允许启动") {
        const auto startup = usapp::db::checkTaskPrivilegesForStartup(pool, kExecTimeout);
        CHECK(startup.allowed());
        if (startup.report) {
            CHECK_EQ(startup.report->currentRole, std::string("usapp_api"));
        }
    }
}

}  // namespace usapptest::taskservice
