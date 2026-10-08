#pragma once

#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include "WalletLedgerDbTests.h"
#include "services/WalletService.h"
#include "db/WalletStartupCheck.h"

namespace usapptest::wallet {

inline void runWalletReadDbTests(const ClientPtr &pool, const std::string &dsn) {
    std::optional<Observer> observerHolder;
    try {
        observerHolder = Observer::connect(dsn);
    } catch (const std::exception &) {
        TEST_CASE("Wallet read: observer acquisition") { CHECK(false); }
        return;
    }
    const auto &observer = *observerHolder;

    TEST_CASE("Wallet read: real service zero balances, no space, no writes and startup check") {
        const std::string label = "wallet-read-service";
        Fixture f;
        const std::string unbound = newUuid();
        TxPtr seed;
        int pid = 0;
        bool committed = false;
        try {
            seed = acquireTx(pool);
            if (!seed) throw std::runtime_error("seed acquisition failed");
            const auto pidResult = exec(seed, "SELECT pg_backend_pid() AS pid");
            if (!pidResult.ok || !pidResult.result) throw std::runtime_error("seed pid failed");
            pid = (*pidResult.result)[0]["pid"].as<int>();
            if (!seedFixture(seed, f).empty()) throw std::runtime_error("non-wallet fixture failed");
            const std::string email = "wallet_read_" + usapp::crypto::randomHex(8) + "@example.com";
            const auto user = exec(seed,
                "INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $2, $2)",
                unbound, email);
            if (!user.ok) throw std::runtime_error("unbound fixture failed");
            const auto commit = commitTx(seed);
            committed = commit.has_value() && *commit;
            if (!committed) throw std::runtime_error("non-wallet fixture commit failed");

            auto read = [](const std::string &id) {
                using Reply = std::pair<usapp::WalletService::Outcome, usapp::WalletBalances>;
                auto promise = std::make_shared<std::promise<Reply>>();
                auto future = promise->get_future();
                usapp::WalletService::instance().getOwnWallet(id,
                    [promise](usapp::WalletService::Outcome outcome, usapp::WalletBalances balances) {
                        promise->set_value({outcome, balances});
                    });
                if (future.wait_for(kExecTimeout) != std::future_status::ready) {
                    throw std::runtime_error("service read timeout");
                }
                return future.get();
            };
            for (const auto &id : {f.userA, f.userB}) {
                const auto reply = read(id);
                CHECK(reply.first == usapp::WalletService::Outcome::Ok);
                CHECK_EQ(reply.second.flower, 0);
                CHECK_EQ(reply.second.sunshine, 0);
                CHECK_EQ(committedWalletRowsFor(observer, id), 0);
            }
            CHECK(read(unbound).first == usapp::WalletService::Outcome::SpaceNotFound);
            CHECK(read(newUuid()).first == usapp::WalletService::Outcome::SpaceNotFound);
            CHECK(read("not-a-uuid").first == usapp::WalletService::Outcome::Error);
            const auto startup = usapp::db::checkWalletPrivilegesForStartup(pool, kExecTimeout);
            CHECK(startup.allowed());
            const auto audits = pool->execSqlSync(
                "SELECT count(*)::bigint AS n FROM audit_logs "
                "WHERE actor_type = 'user' AND actor_id IN ($1::uuid, $2::uuid)",
                f.userA, f.userB);
            CHECK_EQ(audits[0]["n"].as<long long>(), 0);
        } catch (const std::exception &) {
            expectThat(false, label, "fixture/read/startup check failed (details withheld)");
        }
        rollbackTx(seed);
        if (pid) CHECK(teardownEnded(observer, pid).ended);

        // Only non-wallet rows were committed. Always clean by this run's UUIDs.
        try {
            for (const auto &id : f.userIds()) {
                CHECK_EQ(committedWalletRowsFor(observer, id), 0);
                const auto deleted = pool->execSqlSync("DELETE FROM couple_members WHERE user_id=$1::uuid", id);
                if (committed) CHECK_EQ(deleted.affectedRows(), 1u);
            }
            for (const auto &id : {f.coupleOne, f.coupleTwo}) {
                const auto deleted = pool->execSqlSync("DELETE FROM couples WHERE id=$1::uuid", id);
                if (committed) CHECK_EQ(deleted.affectedRows(), 1u);
            }
            auto users = f.userIds();
            users.push_back(unbound);
            for (const auto &id : users) {
                const auto deleted = pool->execSqlSync("DELETE FROM users WHERE id=$1::uuid", id);
                if (committed) CHECK_EQ(deleted.affectedRows(), 1u);
            }
            CHECK_EQ(committedRowsFor(observer, users), 0);
        } catch (const std::exception &) {
            expectThat(false, label, "cleanup failed");
        }
    }

    TEST_CASE("Wallet read: production SQL uses latest sequence, both assets and only caller membership") {
        const std::string label = "wallet-read-snapshot";
        CaseContext ctx;
        const auto unbound = newUuid();
        try {
            if (openCase(pool, ctx, label)) {
                const auto verify = [&](const std::string &id, long long flower, long long sunshine, bool member = true) {
                    const auto out = exec(ctx.tx, usapp::WalletService::balanceSql(), id);
                    CHECK(out.completed && out.ok && out.result.has_value());
                    if (!out.ok || !out.result) return;
                    CHECK_EQ(out.result->size(), member ? 1u : 0u);
                    if (member && out.result->size() == 1) {
                        CHECK_EQ((*out.result)[0]["flower"].as<long long>(), flower);
                        CHECK_EQ((*out.result)[0]["sunshine"].as<long long>(), sunshine);
                    }
                };
                verify(ctx.f.userA, 0, 0);
                const auto op1 = newUuid(), op2 = newUuid(), opB = newUuid(), opC = newUuid();
                const auto &f = ctx.f;
                const std::vector<std::string> statements = {
                    accountSql(f.userA, f.coupleOne, "flower"),
                    accountSql(f.userA, f.coupleOne, "sunshine"),
                    accountSql(f.userB, f.coupleOne, "flower"),
                    accountSql(f.userB, f.coupleOne, "sunshine"),
                    accountSql(f.userC, f.coupleTwo, "flower"),
                    operationSql(op1, f.coupleOne, f.userA),
                    entrySql(op1, f.coupleOne, f.userA, "flower", 1, 3, 3),
                    entrySql(op1, f.coupleOne, f.userA, "sunshine", 1, 7, 7),
                    operationSql(op2, f.coupleOne, f.userA),
                    entrySql(op2, f.coupleOne, f.userA, "flower", 2, -1, 2),
                    operationSql(opB, f.coupleOne, f.userB),
                    entrySql(opB, f.coupleOne, f.userB, "flower", 1, 9, 9),
                    entrySql(opB, f.coupleOne, f.userB, "sunshine", 1, 11, 11),
                    operationSql(opC, f.coupleTwo, f.userC),
                    entrySql(opC, f.coupleTwo, f.userC, "flower", 1, 19, 19),
                };
                if (runSetup(ctx, statements, label)) {
                    CHECK(exec(ctx.tx, "SET CONSTRAINTS ALL IMMEDIATE").ok);
                    verify(f.userA, 2, 7);
                    verify(f.userB, 9, 11);
                    verify(f.userC, 19, 0);
                    verify(newUuid(), 0, 0, false);
                    const auto email = "wallet_read_" + usapp::crypto::randomHex(8) + "@example.com";
                    CHECK(exec(ctx.tx,
                        "INSERT INTO users (id, email_canonical, email_display) VALUES ($1::uuid, $2, $2)",
                        unbound, email).ok);
                    verify(unbound, 0, 0, false);
                    const auto audits = exec(ctx.tx,
                        "SELECT count(*)::bigint AS n FROM audit_logs "
                        "WHERE actor_type = 'user' AND actor_id IN ($1::uuid,$2::uuid,$3::uuid)",
                        f.userA, f.userB, f.userC);
                    CHECK(audits.ok && audits.result.has_value());
                    if (audits.ok && audits.result) CHECK_EQ((*audits.result)[0]["n"].as<long long>(), 0);
                }
            }
        } catch (const std::exception &) {
            expectThat(false, label, "rollback-only snapshot case failed");
        }
        closeCase(observer, ctx, label);
        try {
            CHECK_EQ(committedRowsFor(observer, {unbound}), 0);
        } catch (const std::exception &) {
            expectThat(false, label, "unbound fixture visibility check failed");
        }
    }
}

}  // namespace usapptest::wallet
