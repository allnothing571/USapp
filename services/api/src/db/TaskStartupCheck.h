#pragma once

#include <chrono>
#include <optional>
#include "db/TaskPrivilegePolicy.h"

namespace usapp::db {

// M3-B: the task-table privilege matrix is enforced at API startup, after the wallet check.
// No report (query error or timeout), a missing table, a wrong role or any mismatch refuses startup.
bool taskStartupAllowed(const std::optional<WalletPrivilegeReport> &report);

struct TaskStartupResult {
    enum class State { Report, QueryError, Timeout };
    State state = State::QueryError;
    std::optional<WalletPrivilegeReport> report;
    bool allowed() const { return state == State::Report && taskStartupAllowed(report); }
};

// Invoked before app().run(); callbacks capture heap state, never stack references.
TaskStartupResult checkTaskPrivilegesForStartup(
    const ClientPtr &client, std::chrono::milliseconds timeout = std::chrono::seconds(10));

}  // namespace usapp::db
