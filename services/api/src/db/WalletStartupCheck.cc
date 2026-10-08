#include "db/WalletStartupCheck.h"

#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace usapp::db {

bool walletStartupAllowed(const std::optional<WalletPrivilegeReport> &report) {
    return report && report->currentRole == WalletPrivilegePolicy::kAppRole &&
           !report->checks.empty() && report->ok();
}

WalletStartupResult checkWalletPrivilegesForStartup(const ClientPtr &client,
                                                   std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<WalletStartupResult>>();
    auto future = promise->get_future();
    try {
        WalletPrivilegePolicy::check(
            client,
            [promise](WalletPrivilegeReport report) {
                promise->set_value({WalletStartupResult::State::Report, std::move(report)});
            },
            [promise](const std::string &) {
                // Do not log the exception text: it may contain SQL or connection details.
                promise->set_value({WalletStartupResult::State::QueryError, std::nullopt});
            });
    } catch (const std::exception &) {
        return {WalletStartupResult::State::QueryError, std::nullopt};
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        // A late callback owns its promise and cannot refer to the departed stack.
        return {WalletStartupResult::State::Timeout, std::nullopt};
    }
    try {
        return future.get();
    } catch (const std::exception &) {
        return {WalletStartupResult::State::QueryError, std::nullopt};
    }
}

}  // namespace usapp::db
