#include "db/TaskStartupCheck.h"

#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace usapp::db {

bool taskStartupAllowed(const std::optional<WalletPrivilegeReport> &report) {
    return report && report->currentRole == WalletPrivilegePolicy::kAppRole &&
           !report->checks.empty() && report->ok();
}

TaskStartupResult checkTaskPrivilegesForStartup(const ClientPtr &client, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<TaskStartupResult>>();
    auto future = promise->get_future();
    try {
        TaskPrivilegePolicy::check(
            client,
            [promise](WalletPrivilegeReport report) {
                try {
                    promise->set_value({TaskStartupResult::State::Report, std::move(report)});
                } catch (...) {
                }
            },
            [promise](const std::string &) {
                // Do not log the exception text: it may contain SQL or connection details.
                try {
                    promise->set_value({TaskStartupResult::State::QueryError, std::nullopt});
                } catch (...) {
                }
            });
    } catch (const std::exception &) {
        return {TaskStartupResult::State::QueryError, std::nullopt};
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        // A late callback owns its promise and cannot refer to the departed stack.
        return {TaskStartupResult::State::Timeout, std::nullopt};
    }
    try {
        return future.get();
    } catch (const std::exception &) {
        return {TaskStartupResult::State::QueryError, std::nullopt};
    }
}

}  // namespace usapp::db
