#pragma once

#include "TestSupport.h"
#include "WalletLedgerProbe.h"

namespace usapptest::observerlifetime {
inline bool expired(const std::weak_ptr<drogon::orm::DbClient> &client) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!client.expired() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return client.expired();
}
inline bool backendGone(const wallet::ClientPtr &pool, int pid) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (pool->execSqlSync("SELECT count(*)::bigint AS n FROM pg_stat_activity WHERE pid=$1::integer", pid)[0]["n"].as<long long>() == 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

inline void run(const wallet::ClientPtr &pool, const std::string &dsn) {
    TEST_CASE("Observer: success/error callback tails end before owner-thread client disposal; no retained clients/backends") {
        try {
            for (bool error : {false, true}) {
                for (int i = 0; i < 12; ++i) {
                    std::weak_ptr<drogon::orm::DbClient> weak;
                    int pid = 0;
                    {
                        auto observer = wallet::Observer::connect(dsn);
                        weak = observer.client;
                        pid = observer.client->execSqlSync("SELECT pg_backend_pid() AS pid")[0]["pid"].as<int>();
                        auto entered = std::make_shared<std::promise<void>>();
                        auto proceed = std::make_shared<std::promise<void>>();
                        auto enteredFuture = entered->get_future();
                        auto proceedFuture = proceed->get_future().share();
                        const auto tail = [entered, proceedFuture] {
                            entered->set_value();
                            // Deliberately bounded callback tail, only in this regression. Never wait on a test mutex.
                            proceedFuture.wait_for(std::chrono::seconds(5));
                        };
                        observer.client->execSqlAsync(error ? "SELECT 1/0" : "SELECT 1",
                            [tail](const drogon::orm::Result &) { tail(); },
                            [tail](const drogon::orm::DrogonDbException &) { tail(); });
                        const bool started = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
                        proceed->set_value();
                        CHECK(started);
                        // Immediately end the external owner while the driver's callback/timeout tail may still own refs.
                    }
                    CHECK(expired(weak));
                    CHECK(backendGone(pool, pid));
                }
            }
        } catch (const std::exception &) { CHECK(false); }
    }
    TEST_CASE("Observer: copies share lifetime; replacing/moving owners closes only the replaced client") {
        try {
            std::weak_ptr<drogon::orm::DbClient> original, replacement;
            {
                std::optional<wallet::Observer> first(wallet::Observer::connect(dsn));
                auto copy = *first;
                original = copy.client;
                first.reset();
                CHECK(!original.expired());
                CHECK_EQ(copy.client->execSqlSync("SELECT 1 AS n")[0]["n"].as<int>(), 1);
                auto next = wallet::Observer::connect(dsn);
                replacement = next.client;
                copy = std::move(next);
                CHECK(expired(original));
                CHECK(!replacement.expired());
                CHECK_EQ(copy.client->execSqlSync("SELECT 1 AS n")[0]["n"].as<int>(), 1);
            }
            CHECK(expired(replacement));
        } catch (const std::exception &) { CHECK(false); }
    }
}
}  // namespace usapptest::observerlifetime
