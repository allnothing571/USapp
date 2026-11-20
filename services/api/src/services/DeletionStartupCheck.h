#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include "db/Db.h"

namespace usapp::deletionstartup {
// This is a refusal gate, NOT external-checkpoint validation or recovery release authorization.
enum class State { AllowedWithoutProvider, RecoveryClosed, QueryError, Timeout, ProviderUnavailable };
struct Requirements { bool mediaStoreRequested = false; bool memoryPurgeRequested = false; };
struct Result {
    State state = State::QueryError;
    bool allowed() const { return state == State::AllowedWithoutProvider; }
    const char *reason() const;
};
using Reply = std::function<void(std::optional<bool>)>;
// Probe must enqueue nonblocking work. Missing, malformed and unavailable observations are nullopt.
using Probe = std::function<void(Reply)>;
Result check(const Probe &, Requirements, std::chrono::milliseconds timeout = std::chrono::seconds(5));
Result check(const db::ClientPtr &, Requirements, std::chrono::milliseconds timeout = std::chrono::seconds(5));
} // namespace usapp::deletionstartup
