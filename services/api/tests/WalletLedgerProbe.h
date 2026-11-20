// =====================================================================
// services/api/tests/WalletLedgerProbe.h
//
// 钱包流水账数据库测试的辅助工具（仅测试使用；阶段 1 M2-A）。
//
// 依据 Drogon v1.9.11 源码确认的行为（而不是猜测）：
//   * TransactionImpl 在把 SQL 错误交给调用方的异常回调之前，先请求整个事务回滚；
//     此后该事务上的任何语句都会被拒绝（"The transaction has been rolled back"）。
//     因此每个“预期失败”只能占用一个独立事务，绝不尝试 ROLLBACK TO SAVEPOINT。
//   * PgConnection 把查询错误包装成 Failure(PQerrorMessage(...))，不携带 SQLSTATE；
//     而服务器的错误文本可能被本地化。为了按 SQLSTATE 精确断言，预期失败的语句
//     包在一个 DO 块中执行：块内捕获错误后，以固定格式
//     "USAPP_TEST_SQLSTATE=<code>;CONSTRAINT=<name>;" 重新抛出（消息由我们生成，不会被本地化）。
//     DO 块仍然以错误结束，所以 Drogon 依旧回滚整个事务；这只是暴露 SQLSTATE 的手段，
//     不是在失败后继续使用该事务。DO 块以调用者身份执行，权限语义不变。
//   * newTransactionAsync(callback) 异步获取事务；这里加上有界等待，超时后晚到的事务立即回滚。
//   * 观察者使用独立的 DbClient（newPgClient，1 条连接），不属于工作事务所用的连接池；
//     绝不在被观察事务自己的连接上观察它。
//
// 所有等待都有上限；超时即判失败，不会无限阻塞。
// 输出只含 pid、状态、SQLSTATE、约束名等安全信息，从不打印连接串。
// =====================================================================
#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/Result.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/Crypto.h"
#include "db/Db.h"

namespace usapptest::wallet {

using TxPtr = std::shared_ptr<drogon::orm::Transaction>;
using ClientPtr = usapp::db::ClientPtr;
using Millis = std::chrono::milliseconds;

constexpr Millis kAcquireTimeout{5000};
constexpr Millis kExecTimeout{5000};
constexpr Millis kTeardownTimeout{5000};
constexpr Millis kPollStep{20};

// ---------------------------------------------------------------- 标识符
// 由测试生成的 uuid：只含十六进制与连字符，可以安全地内联进 DO 块。
inline std::string newUuid() {
    const std::string hex = usapp::crypto::randomHex(16);  // 32 个十六进制字符
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

// 内联前的防御性检查：只接受本工具生成的 uuid 形状。
inline std::string uuidLiteral(const std::string &id) {
    if (id.size() != 36) {
        throw std::invalid_argument("uuidLiteral: 非法 uuid 形状");
    }
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (dash ? c != '-' : !hex) {
            throw std::invalid_argument("uuidLiteral: 非法 uuid 字符");
        }
    }
    return "'" + id + "'::uuid";
}

// ---------------------------------------------------------------- 有界获取事务
inline TxPtr acquireTx(const ClientPtr &client, Millis timeout = kAcquireTimeout) {
    struct Slot {
        std::mutex mutex;
        bool abandoned = false;
        std::promise<TxPtr> promise;
    };
    auto slot = std::make_shared<Slot>();
    auto future = slot->promise.get_future();
    client->newTransactionAsync([slot](const TxPtr &tx) {
        std::lock_guard<std::mutex> guard(slot->mutex);
        if (slot->abandoned) {
            // 调用方已超时放弃：晚到的事务立即回滚，不留悬挂事务。
            if (tx) {
                tx->rollback();
            }
            return;
        }
        try {
            slot->promise.set_value(tx);
        } catch (...) {
        }
    });
    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    }
    std::lock_guard<std::mutex> guard(slot->mutex);
    if (future.wait_for(Millis(0)) == std::future_status::ready) {
        return future.get();
    }
    slot->abandoned = true;
    return nullptr;
}

// ---------------------------------------------------------------- 有界执行
struct ExecOutcome {
    bool completed = false;  // 有界时间内收到了回调
    bool ok = false;
    std::optional<drogon::orm::Result> result;
    std::string error;       // 服务器错误文本（可能本地化；只用于定位 USAPP_ 标记）
};

class PendingExec {
  public:
    PendingExec() : future_(promise_.get_future()) {}

    void deliver(ExecOutcome outcome) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (delivered_) {
            return;
        }
        delivered_ = true;
        promise_.set_value(std::move(outcome));
    }

    // 有界等待；超时返回 completed=false，不阻塞在未完成的回调上。
    ExecOutcome wait(Millis timeout = kExecTimeout) {
        if (future_.wait_for(timeout) != std::future_status::ready) {
            return ExecOutcome{};
        }
        return future_.get();
    }

    bool readyWithin(Millis timeout) { return future_.wait_for(timeout) == std::future_status::ready; }

  private:
    std::mutex mutex_;
    bool delivered_ = false;
    std::promise<ExecOutcome> promise_;
    std::future<ExecOutcome> future_;
};

template <typename... Args>
std::shared_ptr<PendingExec> startExec(const TxPtr &tx, const std::string &sql, Args &&...args) {
    auto pending = std::make_shared<PendingExec>();
    tx->execSqlAsync(
        sql,
        [pending](const drogon::orm::Result &result) {
            ExecOutcome outcome;
            outcome.completed = true;
            outcome.ok = true;
            outcome.result = result;
            pending->deliver(std::move(outcome));
        },
        [pending](const drogon::orm::DrogonDbException &e) {
            ExecOutcome outcome;
            outcome.completed = true;
            outcome.ok = false;
            outcome.error = e.base().what();
            pending->deliver(std::move(outcome));
        },
        std::forward<Args>(args)...);
    return pending;
}

template <typename... Args>
ExecOutcome exec(const TxPtr &tx, const std::string &sql, Args &&...args) {
    return startExec(tx, sql, std::forward<Args>(args)...)->wait();
}

// 请求回滚并释放本地引用。只是“请求”：完成与否由 teardownEnded() 观察。
inline void rollbackTx(TxPtr &tx) {
    if (tx) {
        tx->rollback();
        tx.reset();
    }
}

// 真正提交：装上提交回调后释放唯一引用（Drogon 在事务对象析构时提交）。
// 返回 nullopt 表示有界时间内没有收到提交结果。
inline std::optional<bool> commitTx(TxPtr &tx, Millis timeout = kExecTimeout) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    tx->setCommitCallback([promise](bool committed) {
        try {
            promise->set_value(committed);
        } catch (...) {
        }
    });
    tx.reset();
    if (future.wait_for(timeout) != std::future_status::ready) {
        return std::nullopt;
    }
    return future.get();
}

// ---------------------------------------------------------------- SQLSTATE 暴露
// statement 必须是单条语句，且只含字面量（不能有绑定参数）。
inline std::string expectFailureSql(const std::string &statement) {
    return "DO $usapp_case$\n"
           "DECLARE\n"
           "    v_constraint text;\n"
           "BEGIN\n"
           "    " + statement + ";\n"
           "    RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'USAPP_TEST_UNEXPECTED_SUCCESS';\n"
           "EXCEPTION\n"
           "    WHEN SQLSTATE 'UWT00' THEN\n"
           "        RAISE;\n"
           "    WHEN OTHERS THEN\n"
           "        GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;\n"
           "        RAISE EXCEPTION USING ERRCODE = 'UWT01',\n"
           "            MESSAGE = 'USAPP_TEST_SQLSTATE=' || SQLSTATE || ';CONSTRAINT='\n"
           "                      || COALESCE(v_constraint, '') || ';';\n"
           "END\n"
           "$usapp_case$";
}

struct FailureCode {
    bool unexpectedSuccess = false;
    std::string sqlState;    // 空串表示错误文本里没有我们的标记
    std::string constraint;
};

inline FailureCode parseFailure(const std::string &error) {
    FailureCode code;
    if (error.find("USAPP_TEST_UNEXPECTED_SUCCESS") != std::string::npos) {
        code.unexpectedSuccess = true;
        return code;
    }
    const std::string stateTag = "USAPP_TEST_SQLSTATE=";
    const auto statePos = error.find(stateTag);
    if (statePos == std::string::npos) {
        return code;
    }
    const auto stateEnd = error.find(';', statePos);
    code.sqlState = error.substr(statePos + stateTag.size(),
                                 stateEnd == std::string::npos ? std::string::npos
                                                               : stateEnd - statePos - stateTag.size());
    const std::string constraintTag = "CONSTRAINT=";
    const auto cPos = error.find(constraintTag, statePos);
    if (cPos != std::string::npos) {
        const auto cEnd = error.find(';', cPos);
        code.constraint = error.substr(cPos + constraintTag.size(),
                                       cEnd == std::string::npos ? std::string::npos
                                                                 : cEnd - cPos - constraintTag.size());
    }
    return code;
}

// ---------------------------------------------------------------- 观察者
// 独立 DbClient（不属于工作连接池）。只用于观察其他连接上的事务状态与已提交数据。
struct Observer {
private:
    // The owning test thread keeps this anchor through closeAll(). A sync query may wake that thread before Drogon
    // disposes its timeout/callback objects; a bare shared_ptr reset can otherwise make DbLoop destroy/join itself.
    // Observer scopes (including copies) belong to test-owner threads, never database callbacks.
    struct Lifetime {
        explicit Lifetime(ClientPtr value) : client(std::move(value)) {}
        ~Lifetime() { client->closeAll(); }
        ClientPtr client;
    };
    // Declared before client: on the final Observer's destruction its public reference is dropped FIRST, while the anchor
    // still owns the client. Copies share one anchor; assignment closes the replaced client's anchor before dropping it.
    std::shared_ptr<Lifetime> lifetime;

public:
    ClientPtr client;

    static Observer connect(const std::string &dsn) {
        Observer observer;
        observer.client = drogon::orm::DbClient::newPgClient(dsn, 1);
        observer.lifetime = std::make_shared<Lifetime>(observer.client);
        observer.client->setTimeout(5.0);
        // 连接异步建立：用有界重试确认可用，不打印连接串。
        const auto deadline = std::chrono::steady_clock::now() + kTeardownTimeout;
        while (true) {
            try {
                observer.client->execSqlSync("SELECT 1");
                return observer;
            } catch (const std::exception &) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("观察者连接在有界时间内不可用");
                }
                std::this_thread::sleep_for(kPollStep);
            }
        }
    }
};

struct TeardownResult {
    bool ended = false;
    std::string observed;  // 只含 pid 与状态
};

// 有界轮询：该 pid 处于 idle、没有 backend_xid、且不再持有 relation / tuple /
// transactionid 锁，即视为服务器端事务已结束并释放锁；后端已断开也视为结束。
// 这不能证明 Drogon 回调的时机，也不能证明连接已回到连接池。
inline TeardownResult teardownEnded(const Observer &observer, int pid, Millis timeout = kTeardownTimeout) {
    TeardownResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            const auto rows = observer.client->execSqlSync(
                "SELECT a.state, (a.backend_xid IS NULL) AS no_xid, "
                "       (SELECT count(*)::integer FROM pg_locks l "
                "         WHERE l.pid = a.pid AND l.locktype IN ('relation', 'tuple', 'transactionid')) AS held "
                "FROM pg_stat_activity a WHERE a.pid = $1::integer",
                pid);
            if (rows.size() == 0) {
                result.ended = true;
                result.observed = "pid=" + std::to_string(pid) + " gone";
                return result;
            }
            const std::string state = rows[0]["state"].isNull() ? "<hidden>" : rows[0]["state"].as<std::string>();
            const bool noXid = rows[0]["no_xid"].as<bool>();
            const int held = rows[0]["held"].as<int>();
            result.observed = "pid=" + std::to_string(pid) + " state=" + state +
                              " no_xid=" + (noXid ? "true" : "false") + " held_locks=" + std::to_string(held);
            if (state == "idle" && noXid && held == 0) {
                result.ended = true;
                return result;
            }
        } catch (const std::exception &) {
            result.observed = "pid=" + std::to_string(pid) + " observer query failed";
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return result;
        }
        std::this_thread::sleep_for(kPollStep);
    }
}

// 已提交行检查：只能证明“其他连接看不到已提交的行”，不能证明回滚已完成。
inline long long committedRowsFor(const Observer &observer, const std::vector<std::string> &userIds) {
    long long total = 0;
    for (const auto &id : userIds) {
        const auto rows = observer.client->execSqlSync(
            "SELECT ((SELECT count(*) FROM wallet_accounts WHERE user_id = $1::uuid) "
            "      + (SELECT count(*) FROM wallet_ledger_entries WHERE user_id = $1::uuid) "
            "      + (SELECT count(*) FROM wallet_operations WHERE actor_user_id = $1::uuid) "
            "      + (SELECT count(*) FROM couple_members WHERE user_id = $1::uuid) "
            "      + (SELECT count(*) FROM users WHERE id = $1::uuid))::bigint AS n",
            id);
        total += rows[0]["n"].as<long long>();
    }
    return total;
}

// 已提交行检查（只看钱包表）：用于使用已提交非钱包夹具的并发用例。
inline long long committedWalletRowsFor(const Observer &observer, const std::string &userId) {
    const auto rows = observer.client->execSqlSync(
        "SELECT ((SELECT count(*) FROM wallet_accounts WHERE user_id = $1::uuid) "
        "      + (SELECT count(*) FROM wallet_ledger_entries WHERE user_id = $1::uuid) "
        "      + (SELECT count(*) FROM wallet_operations WHERE actor_user_id = $1::uuid))::bigint AS n",
        userId);
    return rows[0]["n"].as<long long>();
}

// ---------------------------------------------------------------- 事务内夹具
// 两个成员的空间（A 在 slot 1，B 在 slot 2），以及一个属于另一空间的用户 C。
// 全部在事务内创建，随事务回滚消失。
struct Fixture {
    std::string userA = newUuid();
    std::string userB = newUuid();
    std::string userC = newUuid();
    std::string coupleOne = newUuid();
    std::string coupleTwo = newUuid();

    std::vector<std::string> userIds() const { return {userA, userB, userC}; }
};

// 返回空串表示成功，否则为失败描述（不含连接信息）。
inline std::string seedFixture(const TxPtr &tx, const Fixture &f) {
    const std::string tag = usapp::crypto::randomHex(4);
    struct Step {
        const char *label;
        ExecOutcome outcome;
    };
    std::vector<Step> steps;
    auto insertUser = [&tx, &tag](const std::string &id, const char *name) {
        const std::string email = std::string("wallet_") + name + "_" + tag + "@example.com";
        return exec(tx,
                    "INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $2, $2)",
                    id, email);
    };
    steps.push_back({"user A", insertUser(f.userA, "a")});
    steps.push_back({"user B", insertUser(f.userB, "b")});
    steps.push_back({"user C", insertUser(f.userC, "c")});
    steps.push_back({"couple one", exec(tx, "INSERT INTO couples (id, name, created_by_user_id) "
                                            "VALUES ($1::uuid, 'wallet-test', $2::uuid)",
                                        f.coupleOne, f.userA)});
    steps.push_back({"couple two", exec(tx, "INSERT INTO couples (id, name, created_by_user_id) "
                                            "VALUES ($1::uuid, 'wallet-test-2', $2::uuid)",
                                        f.coupleTwo, f.userC)});
    steps.push_back({"member A", exec(tx, "INSERT INTO couple_members (user_id, couple_id, slot) "
                                          "VALUES ($1::uuid, $2::uuid, 1)",
                                      f.userA, f.coupleOne)});
    steps.push_back({"member B", exec(tx, "INSERT INTO couple_members (user_id, couple_id, slot) "
                                          "VALUES ($1::uuid, $2::uuid, 2)",
                                      f.userB, f.coupleOne)});
    steps.push_back({"member C", exec(tx, "INSERT INTO couple_members (user_id, couple_id, slot) "
                                          "VALUES ($1::uuid, $2::uuid, 1)",
                                      f.userC, f.coupleTwo)});
    for (const auto &step : steps) {
        if (!step.outcome.completed) {
            return std::string("夹具超时: ") + step.label;
        }
        if (!step.outcome.ok) {
            return std::string("夹具失败: ") + step.label;
        }
    }
    return "";
}

// ---------------------------------------------------------------- SQL 片段（只含字面量）
inline std::string accountSql(const std::string &userId, const std::string &coupleId, const std::string &asset) {
    return "INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (" + uuidLiteral(userId) +
           ", '" + asset + "', " + uuidLiteral(coupleId) + ") ON CONFLICT (user_id, asset_type) DO NOTHING";
}

inline std::string operationSql(const std::string &opId,
                                const std::string &coupleId,
                                const std::string &actorId,
                                const std::string &type = "TASK_REWARD",
                                const std::string &sourceId = "",
                                const std::string &keyAndFingerprint = "NULL, NULL") {
    const std::string source = sourceId.empty() ? opId : sourceId;
    return "INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, "
           "idempotency_key, request_fingerprint) VALUES (" +
           uuidLiteral(opId) + ", " + uuidLiteral(coupleId) + ", " + uuidLiteral(actorId) + ", '" + type +
           "', 'wallet_test', '" + source + "', " + keyAndFingerprint + ")";
}

inline std::string entrySql(const std::string &opId,
                            const std::string &coupleId,
                            const std::string &userId,
                            const std::string &asset,
                            long long seq,
                            long long amount,
                            long long balanceAfter) {
    return "INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, "
           "balance_after) VALUES (" +
           uuidLiteral(opId) + ", " + uuidLiteral(coupleId) + ", " + uuidLiteral(userId) + ", '" + asset +
           "', " + std::to_string(seq) + ", " + std::to_string(amount) + ", " + std::to_string(balanceAfter) +
           ")";
}

}  // namespace usapptest::wallet
