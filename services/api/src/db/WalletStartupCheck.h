#pragma once

#include <chrono>
#include <optional>
#include "db/WalletPrivilegePolicy.h"

namespace usapp::db {

// No report (query error or timeout) always refuses startup.
bool walletStartupAllowed(const std::optional<WalletPrivilegeReport> &report);

struct WalletStartupResult {
    enum class State { Report, QueryError, Timeout };
    State state = State::QueryError;
    std::optional<WalletPrivilegeReport> report;
    bool allowed() const { return state == State::Report && walletStartupAllowed(report); }
};

// Invoked before app().run(); callbacks capture heap state, never stack references.
WalletStartupResult checkWalletPrivilegesForStartup(
    const ClientPtr &client, std::chrono::milliseconds timeout = std::chrono::seconds(10));

}  // namespace usapp::db
