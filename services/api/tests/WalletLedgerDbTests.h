// =====================================================================
// services/api/tests/WalletLedgerDbTests.h
//
// 阶段 1 M2-A：钱包流水账数据库回归测试（由 DbGatedTest.cc 的 main 调用）。
//
// 原则（docs/phase1-plan.md 9.10 / 9.11）：
//   * 以应用角色 usapp_api 运行；权限矩阵不符时用例失败，而不是跳过。
//   * 每个“预期失败”占用一个独立事务；Drogon 在错误回调前已请求回滚，
//     失败之后不再在该事务上执行任何语句（不使用 ROLLBACK TO SAVEPOINT）。
//   * 正向用例最后执行 SET CONSTRAINTS ALL IMMEDIATE 触发延迟检查，然后回滚，从不提交。
//   * 唯一的真实提交用例是“没有流水的操作”：提交必须失败。若该检查本身失效而提交成功，
//     用例会明确报告残留的已提交行。
//   * 事务内夹具随回滚消失；并发用例使用已提交的非钱包夹具，钱包事务两边都回滚，
//     夹具按标识符删除。
//   * 回滚证据：观察者连接上看不到已提交行，只证明“没有可见的已提交行”；
//     另以 pg_stat_activity / pg_locks 有界轮询确认服务器端事务结束并释放锁。
//     两者都不能证明 Drogon 回调时机或连接已回到连接池。
//   * 不测试（M3 要求）：已存在账户上的 FOR UPDATE 串行化、提交后快照可见性。
// =====================================================================
#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "WalletLedgerProbe.h"
#include "db/WalletPrivilegePolicy.h"

namespace usapptest::wallet {

// 失败时附带用例标签，便于定位（只输出安全信息）。
inline void expectThat(bool condition, const std::string &label, const std::string &detail) {
    CHECK(condition);
    if (!condition) {
        usapptest::note("[wallet] " + label + ": " + detail);
    }
}

struct CaseContext {
    TxPtr tx;
    int pid = 0;
    Fixture f;
};

inline bool openCase(const ClientPtr &pool, CaseContext &ctx, const std::string &label) {
    ctx.tx = acquireTx(pool);
    if (!ctx.tx) {
        expectThat(false, label, "有界时间内未获取到事务");
        return false;
    }
    const ExecOutcome pidOut = exec(ctx.tx, "SELECT pg_backend_pid() AS pid");
    if (!pidOut.completed || !pidOut.ok || !pidOut.result.has_value()) {
        expectThat(false, label, "读取 pg_backend_pid() 失败");
        rollbackTx(ctx.tx);
        return false;
    }
    ctx.pid = (*pidOut.result)[0]["pid"].as<int>();
    const std::string why = seedFixture(ctx.tx, ctx.f);
    if (!why.empty()) {
        expectThat(false, label, why);
        return false;
    }
    return true;
}

// 请求回滚（已回滚时为无操作）→ 有界确认服务器端事务结束 → 确认没有可见的已提交行。
inline void closeCase(const Observer &observer, CaseContext &ctx, const std::string &label) {
    rollbackTx(ctx.tx);
    if (ctx.pid != 0) {
        const TeardownResult teardown = teardownEnded(observer, ctx.pid);
        expectThat(teardown.ended, label, "teardown 未在有界时间内结束: " + teardown.observed);
    }
    try {
        const long long committed = committedRowsFor(observer, ctx.f.userIds());
        expectThat(committed == 0, label, "观察者看到已提交的夹具或钱包行: " + std::to_string(committed));
    } catch (const std::exception &e) {
        expectThat(false, label, std::string("已提交行检查失败: ") + e.what());
    }
}

inline bool runSetup(CaseContext &ctx, const std::vector<std::string> &setup, const std::string &label) {
    for (size_t i = 0; i < setup.size(); ++i) {
        const ExecOutcome out = exec(ctx.tx, setup[i]);
        if (!out.completed || !out.ok) {
            expectThat(false, label,
                       "准备语句 #" + std::to_string(i) + (out.completed ? " 失败: " + out.error : " 超时"));
            return false;
        }
    }
    return true;
}

using SqlList = std::function<std::vector<std::string>(const Fixture &)>;
using SqlOne = std::function<std::string(const Fixture &)>;

// 预期失败：准备语句全部成功后，执行被 DO 包裹的目标语句，按 SQLSTATE（及约束名）断言。
inline void expectFailure(const ClientPtr &pool,
                          const Observer &observer,
                          const std::string &label,
                          const SqlList &setup,
                          const SqlOne &failing,
                          const std::string &expectedState,
                          const std::string &expectedConstraint = "") {
    CaseContext ctx;
    if (openCase(pool, ctx, label) && runSetup(ctx, setup(ctx.f), label)) {
        const ExecOutcome out = exec(ctx.tx, expectFailureSql(failing(ctx.f)));
        if (!out.completed) {
            expectThat(false, label, "目标语句在有界时间内没有结果");
        } else if (out.ok) {
            expectThat(false, label, "目标语句意外成功");
        } else {
            const FailureCode code = parseFailure(out.error);
            expectThat(!code.unexpectedSuccess, label, "目标语句意外成功（DO 块报告）");
            expectThat(code.sqlState == expectedState, label,
                       "SQLSTATE 期望 " + expectedState + " 实际 " +
                           (code.sqlState.empty() ? "<无标记>" : code.sqlState));
            if (!expectedConstraint.empty()) {
                expectThat(code.constraint == expectedConstraint, label,
                           "约束期望 " + expectedConstraint + " 实际 " + code.constraint);
            }
        }
    }
    closeCase(observer, ctx, label);
}

// 正向：准备语句全部成功 → SET CONSTRAINTS ALL IMMEDIATE 成功 → 可选校验 → 回滚。
inline void expectSuccess(const ClientPtr &pool,
                          const Observer &observer,
                          const std::string &label,
                          const SqlList &setup,
                          const std::function<void(CaseContext &)> &verify = nullptr) {
    CaseContext ctx;
    if (openCase(pool, ctx, label) && runSetup(ctx, setup(ctx.f), label)) {
        const ExecOutcome immediate = exec(ctx.tx, "SET CONSTRAINTS ALL IMMEDIATE");
        expectThat(immediate.completed && immediate.ok, label, "SET CONSTRAINTS ALL IMMEDIATE 未成功");
        if (immediate.completed && immediate.ok && verify) {
            verify(ctx);
        }
    }
    closeCase(observer, ctx, label);
}

inline std::string fingerprint() {
    return usapp::crypto::randomHex(32);  // 64 个小写十六进制字符
}

inline std::string keyAndFingerprint(const std::string &key, const std::string &fp) {
    return "'" + key + "', '" + fp + "'";
}

// 读取事务内某账户的最新余额（无流水为 0）。
inline long long latestBalance(CaseContext &ctx, const std::string &userId, const std::string &asset) {
    const ExecOutcome out = exec(ctx.tx,
                                 "SELECT COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e "
                                 " WHERE e.user_id = $1::uuid AND e.asset_type = $2 "
                                 " ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS balance",
                                 userId, asset);
    if (!out.completed || !out.ok || !out.result.has_value()) {
        return -1;
    }
    return (*out.result)[0]["balance"].as<long long>();
}

inline void runWalletLedgerDbTests(const ClientPtr &pool, const std::string &dsn) {
    std::optional<Observer> observerHolder;
    try {
        observerHolder = Observer::connect(dsn);
    } catch (const std::exception &e) {
        TEST_CASE("Wallet: 观察者连接") {
            expectThat(false, "observer", e.what());
        }
        return;
    }
    const Observer &observer = *observerHolder;

    // -----------------------------------------------------------------
    // W1. 有效权限矩阵与身份（与 M2-B 的 API 启动检查共用同一实现）
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 有效权限矩阵、身份、所有权、角色成员与 PUBLIC") {
        auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto future = promise->get_future();
        usapp::db::WalletPrivilegePolicy::check(
            pool,
            [promise](usapp::db::WalletPrivilegeReport report) {
                try {
                    promise->set_value({report.ok(), report.summary()});
                } catch (...) {
                }
            },
            [promise](const std::string &error) {
                try {
                    promise->set_value({false, "query error: " + error});
                } catch (...) {
                }
            });
        if (future.wait_for(kExecTimeout) != std::future_status::ready) {
            expectThat(false, "privilege-matrix", "矩阵查询在有界时间内没有结果");
        } else {
            const auto result = future.get();
            usapptest::note(result.second);  // 只含角色名、表/列/权限名与布尔值
            expectThat(result.first, "privilege-matrix", "有效权限矩阵与 docs/phase1-plan.md 9.6 不符");
        }
    }

    // -----------------------------------------------------------------
    // W2. 触发器目录：存在且启用
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 触发器存在且启用") {
        const std::vector<std::pair<std::string, std::string>> expected = {
            {"wallet_accounts", "wallet_accounts_append_only"},
            {"wallet_operations", "wallet_operations_append_only"},
            {"wallet_ledger_entries", "wallet_ledger_entries_append_only"},
            {"wallet_ledger_entries", "wallet_ledger_entries_chain"},
            {"wallet_ledger_entries", "wallet_ledger_entries_single_row"},
            {"wallet_operations", "wallet_operations_require_entries"},
        };
        for (const auto &item : expected) {
            try {
                const auto rows = observer.client->execSqlSync(
                    "SELECT t.tgenabled::text AS enabled FROM pg_trigger t "
                    "JOIN pg_class c ON c.oid = t.tgrelid "
                    "WHERE c.relname = $1 AND t.tgname = $2 AND NOT t.tgisinternal",
                    item.first, item.second);
                expectThat(rows.size() == 1u, item.second, "触发器不存在");
                if (rows.size() == 1u) {
                    expectThat(rows[0]["enabled"].as<std::string>() == "O", item.second, "触发器未处于启用状态 O");
                }
            } catch (const std::exception &e) {
                expectThat(false, item.second, std::string("目录查询失败: ") + e.what());
            }
        }
    }

    const std::string F = "flower";
    const std::string S = "sunshine";

    // -----------------------------------------------------------------
    // W3. 正向：多资产操作、同账户多条、扣减到零、单条上限、延迟检查、行锁权限
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 链式流水正向用例") {
        const std::string op1 = newUuid(), op2 = newUuid(), op3 = newUuid(), op4 = newUuid(), op5 = newUuid();
        expectSuccess(
            pool, observer, "chain-positive",
            [&](const Fixture &f) {
                return std::vector<std::string>{
                    accountSql(f.userA, f.coupleOne, F),
                    accountSql(f.userA, f.coupleOne, S),
                    operationSql(op1, f.coupleOne, f.userA),
                    entrySql(op1, f.coupleOne, f.userA, F, 1, 5, 5),
                    // 一个操作同时奖励两种资产：两条独立语句、同一事务。
                    operationSql(op2, f.coupleOne, f.userA),
                    entrySql(op2, f.coupleOne, f.userA, F, 2, 3, 8),
                    entrySql(op2, f.coupleOne, f.userA, S, 1, 40, 40),
                    // Generic debit to zero: since 0015 every REWARD_REDEMPTION operation must be
                    // linked to a redemption row (deferred UM003), so a plain operation is used here.
                    operationSql(op3, f.coupleOne, f.userA),
                    entrySql(op3, f.coupleOne, f.userA, F, 3, -8, 0),
                    operationSql(op4, f.coupleOne, f.userA),
                    entrySql(op4, f.coupleOne, f.userA, F, 4, 999, 999),
                    operationSql(op5, f.coupleOne, f.userA),
                    entrySql(op5, f.coupleOne, f.userA, S, 2, 100000, 100040),
                };
            },
            [&](CaseContext &ctx) {
                expectThat(latestBalance(ctx, ctx.f.userA, F) == 999, "chain-positive", "小红花余额应为 999");
                expectThat(latestBalance(ctx, ctx.f.userA, S) == 100040, "chain-positive", "阳光值余额应为 100040");
                // 行锁依赖 lock_token 列级 UPDATE 权限。
                const ExecOutcome lock = exec(ctx.tx,
                                              "SELECT 1 FROM wallet_accounts WHERE user_id = $1::uuid "
                                              "AND asset_type = 'flower' FOR UPDATE",
                                              ctx.f.userA);
                expectThat(lock.completed && lock.ok, "chain-positive", "SELECT ... FOR UPDATE 应当成功");
            });
    }

    // -----------------------------------------------------------------
    // W4. 链式篡改与起始状态
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 链式篡改、伪造起始余额与序号") {
        auto base = [&](const std::string &op) {
            return [&, op](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                accountSql(f.userA, f.coupleOne, S),
                                                operationSql(op, f.coupleOne, f.userA)};
            };
        };
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "fabricated-opening-balance", base(op),
                          [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userA, F, 1, 1, 1000); },
                          "UW004");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "first-seq-not-one", base(op),
                          [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userA, F, 2, 1, 1); },
                          "UW003");
        }
        auto withFirst = [&](const std::string &op1, const std::string &op2) {
            return [&, op1, op2](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                operationSql(op1, f.coupleOne, f.userA),
                                                entrySql(op1, f.coupleOne, f.userA, F, 1, 5, 5),
                                                operationSql(op2, f.coupleOne, f.userA)};
            };
        };
        {
            const std::string op1 = newUuid(), op2 = newUuid();
            expectFailure(pool, observer, "tampered-seq-skip", withFirst(op1, op2),
                          [&, op2](const Fixture &f) { return entrySql(op2, f.coupleOne, f.userA, F, 3, 1, 6); },
                          "UW003");
        }
        {
            const std::string op1 = newUuid(), op2 = newUuid();
            expectFailure(pool, observer, "tampered-seq-repeat", withFirst(op1, op2),
                          [&, op2](const Fixture &f) { return entrySql(op2, f.coupleOne, f.userA, F, 1, 1, 6); },
                          "UW003");
        }
        {
            const std::string op1 = newUuid(), op2 = newUuid();
            expectFailure(pool, observer, "tampered-intermediate-balance", withFirst(op1, op2),
                          [&, op2](const Fixture &f) { return entrySql(op2, f.coupleOne, f.userA, F, 2, 3, 508); },
                          "UW004");
        }
        {
            const std::string op1 = newUuid(), op2 = newUuid(), op3 = newUuid();
            expectFailure(
                pool, observer, "stale-third-entry",
                [&, op1, op2, op3](const Fixture &f) {
                    return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                    operationSql(op1, f.coupleOne, f.userA),
                                                    entrySql(op1, f.coupleOne, f.userA, F, 1, 5, 5),
                                                    operationSql(op2, f.coupleOne, f.userA),
                                                    entrySql(op2, f.coupleOne, f.userA, F, 2, 3, 8),
                                                    operationSql(op3, f.coupleOne, f.userA)};
                },
                // 基于第一条（5）而不是第二条（8）计算：5 + 2 = 7。
                [&, op3](const Fixture &f) { return entrySql(op3, f.coupleOne, f.userA, F, 3, 2, 7); },
                "UW004");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "account-missing", base(op),
                          [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userB, F, 1, 1, 1); },
                          "UW002");
        }
        {
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "multi-row-insert", base(op),
                [&, op](const Fixture &f) {
                    // 两行分属两个账户，各自链式合法；只能被“每条语句一行”拒绝。
                    return "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, "
                           "amount, balance_after) VALUES (" +
                           uuidLiteral(op) + ", " + uuidLiteral(f.coupleOne) + ", " + uuidLiteral(f.userA) +
                           ", 'flower', 1, 1, 1), (" + uuidLiteral(op) + ", " + uuidLiteral(f.coupleOne) + ", " +
                           uuidLiteral(f.userA) + ", 'sunshine', 1, 1, 1)";
                },
                "UW005");
        }
    }

    // -----------------------------------------------------------------
    // W5. 已批准上限（D2）与数值约束
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 上限与数值约束（拒绝而非截断）") {
        auto base = [&](const std::string &op) {
            return [&, op](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                accountSql(f.userA, f.coupleOne, S),
                                                operationSql(op, f.coupleOne, f.userA)};
            };
        };
        struct Bound {
            std::string label;
            std::string asset;
            long long amount;
            long long balance;
            std::string constraint;
        };
        const std::vector<Bound> bounds = {
            {"zero-amount", F, 0, 0, "wallet_ledger_entries_amount_nonzero"},
            {"negative-balance", F, -1, -1, "wallet_ledger_entries_balance_bound"},
            {"flower-entry-cap", F, 1000, 1000, "wallet_ledger_entries_amount_bound"},
            {"sunshine-entry-cap", S, 100001, 100001, "wallet_ledger_entries_amount_bound"},
        };
        for (const auto &b : bounds) {
            const std::string op = newUuid();
            expectFailure(pool, observer, b.label, base(op),
                          [&, op, b](const Fixture &f) {
                              return entrySql(op, f.coupleOne, f.userA, b.asset, 1, b.amount, b.balance);
                          },
                          "23514", b.constraint);
        }
        {
            // 小红花余额上限：先在一条 DO 语句中（每次 INSERT 仍是单行语句）累计到恰好 1,000,000，
            // 再加 1 必须被拒绝。阳光值余额上限需要 10,000 条流水，只在 psql 脚本中测试。
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "flower-balance-cap",
                [&, op](const Fixture &f) {
                    const std::string couple = uuidLiteral(f.coupleOne);
                    const std::string user = uuidLiteral(f.userA);
                    return std::vector<std::string>{
                        accountSql(f.userA, f.coupleOne, F),
                        "DO $usapp_setup$\n"
                        "DECLARE i integer; op uuid; bal bigint := 0; amt bigint;\n"
                        "BEGIN\n"
                        "  FOR i IN 1..1002 LOOP\n"
                        "    amt := CASE WHEN i <= 1001 THEN 999 ELSE 1 END;\n"
                        "    op := gen_random_uuid();\n"
                        "    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, "
                        "source_id) VALUES (op, " + couple + ", " + user + ", 'TASK_REWARD', 'wallet_test', op::text);\n"
                        "    bal := bal + amt;\n"
                        "    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, "
                        "amount, balance_after) VALUES (op, " + couple + ", " + user + ", 'flower', i, amt, bal);\n"
                        "  END LOOP;\n"
                        "  IF bal <> 1000000 THEN RAISE EXCEPTION 'cap setup mismatch'; END IF;\n"
                        "END\n"
                        "$usapp_setup$",
                        operationSql(op, f.coupleOne, f.userA)};
                },
                [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userA, F, 1003, 1, 1000001); },
                "23514", "wallet_ledger_entries_balance_bound");
        }
    }

    // -----------------------------------------------------------------
    // W6. 结构、成员与空间一致性、非空
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 结构、成员一致性与非空约束") {
        {
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "second-entry-same-operation-account",
                [&, op](const Fixture &f) {
                    return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                    operationSql(op, f.coupleOne, f.userA),
                                                    entrySql(op, f.coupleOne, f.userA, F, 1, 1, 1)};
                },
                [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userA, F, 2, 1, 2); },
                "23505", "wallet_ledger_entries_operation_account_unique");
        }
        auto twoCouples = [&](const std::string &op) {
            return [&, op](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                accountSql(f.userC, f.coupleTwo, F),
                                                operationSql(op, f.coupleOne, f.userA)};
            };
        };
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "entry-couple-differs-from-operation", twoCouples(op),
                          [&, op](const Fixture &f) { return entrySql(op, f.coupleTwo, f.userC, F, 1, 1, 1); },
                          "23503", "wallet_ledger_entries_operation_fk");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "entry-user-not-member-of-couple", twoCouples(op),
                          [&, op](const Fixture &f) { return entrySql(op, f.coupleOne, f.userC, F, 1, 1, 1); },
                          "23503", "wallet_ledger_entries_member_fk");
        }
        {
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "operation-actor-not-member", [](const Fixture &) { return std::vector<std::string>{}; },
                [&, op](const Fixture &f) { return operationSql(op, f.coupleOne, f.userC); }, "23503",
                "wallet_operations_actor_member_fk");
        }
        expectFailure(pool, observer, "account-user-not-member",
                      [](const Fixture &) { return std::vector<std::string>{}; },
                      [&](const Fixture &f) { return accountSql(f.userC, f.coupleOne, F); }, "23503",
                      "wallet_accounts_member_fk");
        expectFailure(pool, observer, "account-unknown-asset",
                      [](const Fixture &) { return std::vector<std::string>{}; },
                      [&](const Fixture &f) { return accountSql(f.userA, f.coupleOne, "gold"); }, "23514",
                      "wallet_accounts_asset_type_valid");
        expectFailure(pool, observer, "operation-actor-null",
                      [](const Fixture &) { return std::vector<std::string>{}; },
                      [&](const Fixture &f) {
                          return "INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, "
                                 "source_type, source_id) VALUES (" +
                                 uuidLiteral(f.coupleOne) + ", NULL, 'TASK_REWARD', 'wallet_test', '" + newUuid() +
                                 "')";
                      },
                      "23502");
        expectFailure(pool, observer, "account-couple-null",
                      [](const Fixture &) { return std::vector<std::string>{}; },
                      [&](const Fixture &f) {
                          return "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (" +
                                 uuidLiteral(f.userA) + ", 'flower', NULL)";
                      },
                      "23502");
        {
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "entry-amount-null",
                [&, op](const Fixture &f) {
                    return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                    operationSql(op, f.coupleOne, f.userA)};
                },
                [&, op](const Fixture &f) {
                    return "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, "
                           "amount, balance_after) VALUES (" +
                           uuidLiteral(op) + ", " + uuidLiteral(f.coupleOne) + ", " + uuidLiteral(f.userA) +
                           ", 'flower', 1, NULL, 1)";
                },
                "23502");
        }
    }

    // -----------------------------------------------------------------
    // W7. 延迟完整性：SET CONSTRAINTS ALL IMMEDIATE 与真实提交
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 没有流水的操作在立即检查与提交时都被拒绝") {
        {
            const std::string op = newUuid();
            expectFailure(
                pool, observer, "operation-without-entries-immediate",
                [&, op](const Fixture &f) { return std::vector<std::string>{operationSql(op, f.coupleOne, f.userA)}; },
                [](const Fixture &) { return std::string("SET CONSTRAINTS ALL IMMEDIATE"); }, "UW006");
        }
        {
            const std::string label = "operation-without-entries-commit";
            const std::string op = newUuid();
            CaseContext ctx;
            if (openCase(pool, ctx, label) &&
                runSetup(ctx, {operationSql(op, ctx.f.coupleOne, ctx.f.userA)}, label)) {
                const std::optional<bool> committed = commitTx(ctx.tx);
                expectThat(committed.has_value(), label, "提交结果在有界时间内未返回");
                if (committed.has_value()) {
                    expectThat(!*committed, label,
                               "提交意外成功：usapp_test 中已残留本用例的已提交夹具与操作行（无法删除）");
                }
            }
            closeCase(observer, ctx, label);
        }
    }

    // -----------------------------------------------------------------
    // W8. 幂等作用域与来源唯一
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 幂等作用域与业务来源唯一") {
        const std::string key = "wallet-test-key-" + usapp::crypto::randomHex(8);
        {
            const std::string op1 = newUuid(), op2 = newUuid();
            const std::string fp = fingerprint();
            expectFailure(
                pool, observer, "idempotency-duplicate-same-scope",
                [&, op1, fp](const Fixture &f) {
                    return std::vector<std::string>{
                        accountSql(f.userA, f.coupleOne, F),
                        operationSql(op1, f.coupleOne, f.userA, "TASK_REWARD", "", keyAndFingerprint(key, fp)),
                        entrySql(op1, f.coupleOne, f.userA, F, 1, 1, 1)};
                },
                [&, op2, fp](const Fixture &f) {
                    return operationSql(op2, f.coupleOne, f.userA, "TASK_REWARD", "", keyAndFingerprint(key, fp));
                },
                "23505", "wallet_operations_idempotency_unique");
        }
        {
            const std::string op1 = newUuid(), op2 = newUuid(), op3 = newUuid();
            expectSuccess(pool, observer, "idempotency-other-actor-or-type-allowed", [&, op1, op2, op3](const Fixture &f) {
                return std::vector<std::string>{
                    accountSql(f.userA, f.coupleOne, F),
                    accountSql(f.userB, f.coupleOne, F),
                    operationSql(op1, f.coupleOne, f.userA, "TASK_REWARD", "", keyAndFingerprint(key, fingerprint())),
                    entrySql(op1, f.coupleOne, f.userA, F, 1, 5, 5),
                    operationSql(op2, f.coupleOne, f.userB, "TASK_REWARD", "", keyAndFingerprint(key, fingerprint())),
                    entrySql(op2, f.coupleOne, f.userB, F, 1, 5, 5),
                    // Same actor and key, another type (REDEMPTION_REFUND: REWARD_REDEMPTION needs a
                    // linked redemption row since 0015).
                    operationSql(op3, f.coupleOne, f.userA, "REDEMPTION_REFUND", "",
                                 keyAndFingerprint(key, fingerprint())),
                    entrySql(op3, f.coupleOne, f.userA, F, 2, 1, 6)};
            });
        }
        {
            const std::string op1 = newUuid(), op2 = newUuid();
            const std::string source = newUuid();
            expectFailure(
                pool, observer, "duplicate-business-source",
                [&, op1, source](const Fixture &f) {
                    return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                    operationSql(op1, f.coupleOne, f.userA, "TASK_REWARD", source),
                                                    entrySql(op1, f.coupleOne, f.userA, F, 1, 1, 1)};
                },
                [&, op2, source](const Fixture &f) {
                    return operationSql(op2, f.coupleOne, f.userB, "TASK_REWARD", source);
                },
                "23505", "wallet_operations_source_unique");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "key-without-fingerprint",
                          [](const Fixture &) { return std::vector<std::string>{}; },
                          [&, op](const Fixture &f) {
                              return operationSql(op, f.coupleOne, f.userA, "TASK_REWARD", "", "'" + key + "', NULL");
                          },
                          "23514", "wallet_operations_fingerprint_pairing");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "key-too-short",
                          [](const Fixture &) { return std::vector<std::string>{}; },
                          [&, op](const Fixture &f) {
                              return operationSql(op, f.coupleOne, f.userA, "TASK_REWARD", "",
                                                  keyAndFingerprint("short-key-15chr", fingerprint()));
                          },
                          "23514", "wallet_operations_idempotency_key_shape");
        }
    }

    // -----------------------------------------------------------------
    // W9. usapp_api 的权限结果
    //   lock_token：列级授权通过 → 触发器拒绝（UW001，而非 42501）
    //   其他列 UPDATE、流水/操作 UPDATE、DELETE、TRUNCATE：42501
    // 维护角色上的触发器拒绝由 psql 脚本单独验证。
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: usapp_api 的列级 UPDATE、42501 与触发器拒绝") {
        auto withRows = [&](const std::string &op) {
            return [&, op](const Fixture &f) {
                return std::vector<std::string>{accountSql(f.userA, f.coupleOne, F),
                                                operationSql(op, f.coupleOne, f.userA),
                                                entrySql(op, f.coupleOne, f.userA, F, 1, 1, 1)};
            };
        };
        auto accountWhere = [](const Fixture &f) {
            return " WHERE user_id = " + uuidLiteral(f.userA) + " AND asset_type = 'flower'";
        };
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "update-lock-token-trigger", withRows(op),
                          [&](const Fixture &f) { return "UPDATE wallet_accounts SET lock_token = 0" + accountWhere(f); },
                          "UW001");
        }
        for (const std::string column : {"user_id", "asset_type", "couple_id", "created_at"}) {
            const std::string op = newUuid();
            expectFailure(pool, observer, "update-account-" + column, withRows(op),
                          [&, column](const Fixture &f) {
                              return "UPDATE wallet_accounts SET " + column + " = " + column + accountWhere(f);
                          },
                          "42501");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "update-account-lock-token-and-other", withRows(op),
                          [&](const Fixture &f) {
                              return "UPDATE wallet_accounts SET lock_token = 0, created_at = created_at" +
                                     accountWhere(f);
                          },
                          "42501");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "update-ledger", withRows(op),
                          [&, op](const Fixture &) {
                              return "UPDATE wallet_ledger_entries SET amount = amount WHERE operation_id = " +
                                     uuidLiteral(op);
                          },
                          "42501");
        }
        {
            const std::string op = newUuid();
            expectFailure(pool, observer, "update-operation", withRows(op),
                          [&, op](const Fixture &) {
                              return "UPDATE wallet_operations SET source_id = source_id WHERE id = " + uuidLiteral(op);
                          },
                          "42501");
        }
        for (const std::string table : {"wallet_accounts", "wallet_operations", "wallet_ledger_entries"}) {
            {
                const std::string op = newUuid();
                expectFailure(pool, observer, "delete-" + table, withRows(op),
                              [table](const Fixture &f) {
                                  const std::string column = table == "wallet_operations" ? "actor_user_id" : "user_id";
                                  return "DELETE FROM " + table + " WHERE " + column + " = " + uuidLiteral(f.userA);
                              },
                              "42501");
            }
            {
                const std::string op = newUuid();
                expectFailure(pool, observer, "truncate-" + table, withRows(op),
                              [table](const Fixture &) { return "TRUNCATE " + table; }, "42501");
            }
        }
    }

    // -----------------------------------------------------------------
    // W10. 账户创建竞争（已提交非钱包夹具；两个钱包事务都回滚）
    //
    // 只证明：同一账户的并发创建由唯一键等待化解，A 中止后 B 才完成插入。
    // 不证明：已存在账户上的 FOR UPDATE 串行化，也不证明等待者能看到已提交流水
    // （A 内创建的账户对 B 不可见，B 无法锁它）。这两项是 M3 的要求。
    // -----------------------------------------------------------------
    TEST_CASE("Wallet: 账户创建竞争（ON CONFLICT DO NOTHING，两边回滚）") {
        const std::string label = "account-creation-contention";
        const std::string user = newUuid();
        const std::string couple = newUuid();
        bool userCreated = false, coupleCreated = false, memberCreated = false;
        try {
            const std::string email = "wallet_race_" + usapp::crypto::randomHex(4) + "@example.com";
            pool->execSqlSync("INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $2, $2)",
                              user, email);
            userCreated = true;
            pool->execSqlSync("INSERT INTO couples (id, name, created_by_user_id) "
                              "VALUES ($1::uuid, 'wallet-race', $2::uuid)",
                              couple, user);
            coupleCreated = true;
            pool->execSqlSync("INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 1)",
                              user, couple);
            memberCreated = true;
        } catch (const std::exception &e) {
            expectThat(false, label, std::string("已提交非钱包夹具创建失败: ") + e.what());
        }

        if (memberCreated) {
            const std::string insertAccount =
                "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES ($1::uuid, 'flower', $2::uuid) "
                "ON CONFLICT (user_id, asset_type) DO NOTHING";
            TxPtr txA = acquireTx(pool);
            TxPtr txB = txA ? acquireTx(pool) : nullptr;
            int pidA = 0, pidB = 0;
            if (!txA || !txB) {
                expectThat(false, label, "有界时间内未获取到两个事务");
            } else {
                const ExecOutcome pa = exec(txA, "SELECT pg_backend_pid() AS pid");
                const ExecOutcome pb = exec(txB, "SELECT pg_backend_pid() AS pid");
                if (pa.ok && pa.result) pidA = (*pa.result)[0]["pid"].as<int>();
                if (pb.ok && pb.result) pidB = (*pb.result)[0]["pid"].as<int>();
                expectThat(pidA != 0 && pidB != 0 && pidA != pidB, label, "两个事务应位于不同后端");

                const ExecOutcome insA = exec(txA, insertAccount, user, couple);
                expectThat(insA.completed && insA.ok && insA.result && insA.result->affectedRows() == 1, label,
                           "A 应插入 1 行");

                auto pendingB = startExec(txB, insertAccount, user, couple);
                expectThat(!pendingB->readyWithin(Millis(300)), label, "B 在 A 未结束时不应完成");

                // 佐证：B 的后端处于锁等待（同一角色可见 wait_event_type）。
                bool sawLockWait = false;
                const auto deadline = std::chrono::steady_clock::now() + Millis(2000);
                while (!sawLockWait && std::chrono::steady_clock::now() < deadline) {
                    try {
                        const auto rows = observer.client->execSqlSync(
                            "SELECT COALESCE(wait_event_type, '') AS w FROM pg_stat_activity WHERE pid = $1::integer",
                            pidB);
                        sawLockWait = rows.size() == 1u && rows[0]["w"].as<std::string>() == "Lock";
                    } catch (const std::exception &) {
                    }
                    if (!sawLockWait) std::this_thread::sleep_for(kPollStep);
                }
                expectThat(sawLockWait, label, "观察者未看到 B 处于 Lock 等待");

                rollbackTx(txA);
                const ExecOutcome insB = pendingB->wait();
                expectThat(insB.completed && insB.ok && insB.result && insB.result->affectedRows() == 1, label,
                           "A 回滚后 B 应完成并插入 1 行");
                rollbackTx(txB);
            }
            rollbackTx(txA);
            rollbackTx(txB);
            for (const int pid : {pidA, pidB}) {
                if (pid != 0) {
                    const TeardownResult t = teardownEnded(observer, pid);
                    expectThat(t.ended, label, "teardown 未在有界时间内结束: " + t.observed);
                }
            }
            try {
                expectThat(committedWalletRowsFor(observer, user) == 0, label, "观察者看到已提交的钱包行");
            } catch (const std::exception &e) {
                expectThat(false, label, std::string("已提交行检查失败: ") + e.what());
            }
        }

        // 按标识符、按依赖顺序清理已提交的非钱包夹具；任何失败都计为断言失败。
        auto cleanupStep = [&](bool needed, const char *what, const char *sql, const std::string &id) {
            if (!needed) return;
            try {
                const auto r = pool->execSqlSync(sql, id);
                expectThat(r.affectedRows() == 1, label, std::string("清理 ") + what + " 删除行数不为 1");
            } catch (const std::exception &e) {
                expectThat(false, label, std::string("清理 ") + what + " 失败: " + e.what());
            }
        };
        cleanupStep(memberCreated, "couple_members", "DELETE FROM couple_members WHERE user_id = $1::uuid", user);
        cleanupStep(coupleCreated, "couples", "DELETE FROM couples WHERE id = $1::uuid", couple);
        cleanupStep(userCreated, "users", "DELETE FROM users WHERE id = $1::uuid", user);
    }
}

}  // namespace usapptest::wallet
