// Disposable, synthetic PostgreSQL only. Never used by the API binary.
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <execinfo.h>
#include <signal.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>

// Diagnostic-only signal snapshots inside this disposable process; no debugger package needed.
static void snapshot(int) {
    void *frames[40];
    const int n = backtrace(frames, 40);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
}
static void snapshotThreads() {
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks) return;
    while (auto *entry = readdir(tasks)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0 || tid == syscall(SYS_gettid)) continue;
        std::cerr << "PROBE_THREAD " << tid << std::endl;
        syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    closedir(tasks);
}

static int teardownProbe(const char *dsn, bool closeFirst, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        auto client = drogon::orm::DbClient::newPgClient(dsn, 1);
        client->setTimeout(5.0);
        client->execSqlSync("SELECT 1");
        auto entered = std::make_shared<std::promise<void>>();
        auto proceed = std::make_shared<std::promise<void>>();
        auto proceedFuture = proceed->get_future().share();
        auto enteredFuture = entered->get_future();
        client->execSqlAsync("SELECT 1", [entered, proceedFuture](const drogon::orm::Result &) {
            entered->set_value();
            proceedFuture.wait(); // deliberate test-only callback tail: the owner must not die here
        }, [entered](const drogon::orm::DrogonDbException &) { entered->set_exception(std::make_exception_ptr(std::runtime_error("query"))); });
        if (enteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return 20;
        enteredFuture.get();
        std::weak_ptr<drogon::orm::DbClient> weak = client;
        if (closeFirst) {
            proceed->set_value();
            client->closeAll(); // on the owner thread, with its strong reference still held
            client.reset();
            for (int j = 0; j < 100 && !weak.expired(); ++j) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!weak.expired()) { std::cout << "PROBE_TEARDOWN_RETAINED " << i << std::endl; return 21; }
        } else {
            client.reset();
            proceed->set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "PROBE_TEARDOWN " << i << " close_first=" << closeFirst << std::endl;
    }
    std::cout << "PROBE_COMPLETE" << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGUSR1, snapshot);
    const char *dsn = std::getenv("PROBE_DSN");
    if (!dsn) return 2;
    PGconn *control = PQconnectdb(dsn);
    if (PQstatus(control) != CONNECTION_OK) return 3;
    PGresult *identity = PQexec(control, "SELECT current_database()");
    const bool testDatabase = PQresultStatus(identity) == PGRES_TUPLES_OK && PQntuples(identity) == 1 &&
                              std::string(PQgetvalue(identity, 0, 0)) == "usapp_test";
    PQclear(identity);
    if (!testDatabase) { PQfinish(control); return 4; }
    const int delay = argc > 1 ? std::atoi(argv[1]) : 20;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 12;
    const std::string mode = argc > 3 ? argv[3] : "plain";
    if (mode == "teardown" || mode == "teardown-close") {
        const int result = teardownProbe(dsn, mode == "teardown-close", rounds);
        PQfinish(control);
        return result;
    }
    auto *client = new drogon::orm::DbClientPtr(drogon::orm::DbClient::newPgClient(dsn, 1));
    (*client)->setTimeout(3.0);
    const bool chain = mode != "plain";
    if (chain) (*client)->setTimeout(35.0);
    const bool warm = mode == "warm";
    const std::string query = "SELECT $1::integer AS value /*" + std::string(mode == "short" ? 1 : 40000, 'x') + "*/";
    try {
        if ((*client)->execSqlSync("SELECT current_database() AS db")[0]["db"].as<std::string>() != "usapp_test") return 4;
        for (int i = 0; i < rounds; ++i) {
            if (warm) (*client)->execSqlSync(query, 1);
            const auto token = "export-probe-" + std::to_string(i);
            (*client)->execSqlSync("SELECT set_config('application_name',$1,false)", token);
            (*client)->execSqlSync("BEGIN READ ONLY");
            const int victim = (*client)->execSqlSync("SELECT pg_backend_pid() AS pid,pg_advisory_xact_lock_shared(33026,0)")[0]["pid"].as<int>();
            const auto pid = std::to_string(victim);
            const char *params[] = {pid.c_str(), token.c_str()};
            PGresult *killed = PQexecParams(control,
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE pid=$1::integer AND application_name=$2 AND datname='usapp_test' AND usename=current_user",
                2, nullptr, params, nullptr, nullptr, 0);
            const bool confirmed = PQresultStatus(killed) == PGRES_TUPLES_OK && PQntuples(killed) == 1 && std::string(PQgetvalue(killed,0,0)) == "t";
            PQclear(killed);
            if (!confirmed) return 5;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            auto done = std::make_shared<std::promise<bool>>();
            auto future = done->get_future();
            auto phase = std::make_shared<std::atomic<int>>(0);
            if (chain) {
                auto cleanupStarted = std::make_shared<std::atomic<bool>>(false);
                const auto cleanup = [client, done, phase, cleanupStarted]() {
                    if (cleanupStarted->exchange(true)) return;
                    phase->store(2);
                    (*client)->execSqlAsync("ROLLBACK", [client, done, phase](const drogon::orm::Result &) {
                        phase->store(3);
                        (*client)->execSqlAsync("SELECT current_setting('application_name') AS tag", [done, phase](const drogon::orm::Result &) {
                            phase->store(4); done->set_value(true);
                        }, [done](const drogon::orm::DrogonDbException &) {done->set_value(false);});
                    }, [done](const drogon::orm::DrogonDbException &) {done->set_value(false);});
                };
                phase->store(1);
                (*client)->execSqlAsync(query, [cleanup](const drogon::orm::Result &) {cleanup();},
                    [cleanup](const drogon::orm::DrogonDbException &) {cleanup();}, 1);
                if (mode == "watchdog") {
                    std::thread([cleanup] {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        cleanup();
                    }).detach();
                }
            } else {
                (*client)->execSqlAsync("ROLLBACK", [done](const drogon::orm::Result &) {done->set_value(true);},
                    [done](const drogon::orm::DrogonDbException &) {done->set_value(false);});
            }
            if (future.wait_for(std::chrono::seconds(40)) != std::future_status::ready) {
                std::cout << "PROBE_STALLED round=" << i << " delay_ms=" << delay << " chain=" << chain << " phase=" << phase->load() << std::endl;
                std::ofstream("/tmp/probe-stalled") << "stalled";
                snapshotThreads();
                std::this_thread::sleep_for(std::chrono::seconds(20));
                return 10;
            }
            // A failed first query may be normal; the next statement must reconnect or time out on a live event loop.
            (void)future.get();
            const auto row = (*client)->execSqlSync("SELECT current_setting('application_name') AS tag");
            std::cout << "PROBE_ROUND " << i << " reconnected=" << (row[0]["tag"].as<std::string>() != token) << std::endl;
        }
    } catch (...) { std::cout << "PROBE_EXCEPTION" << std::endl; return 11; }
    PQfinish(control);
    std::cout << "PROBE_COMPLETE" << std::endl;
    return 0;
}
