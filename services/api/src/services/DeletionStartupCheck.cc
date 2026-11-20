#include "services/DeletionStartupCheck.h"
#include <future>
#include <memory>

namespace usapp::deletionstartup {
const char *Result::reason() const {
    switch (state) {
        case State::AllowedWithoutProvider: return "no_media_provider_requested";
        case State::RecoveryClosed: return "recovery_closed";
        case State::QueryError: return "recovery_query_unconfirmed";
        case State::Timeout: return "recovery_query_timeout";
        case State::ProviderUnavailable: return "durable_journal_provider_unavailable";
    }
    return "recovery_query_unconfirmed";
}
Result check(const Probe &probe, Requirements requirements, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    try {
        if (!probe) return {State::QueryError};
        probe([promise, requirements](std::optional<bool> closed) {
            Result result;
            if (!closed) result.state = State::QueryError;
            else if (*closed) result.state = State::RecoveryClosed;
            // No production C++ journal adapter exists yet. No environment flag, file or fake can assert readiness.
            else if (requirements.mediaStoreRequested || requirements.memoryPurgeRequested) result.state = State::ProviderUnavailable;
            else result.state = State::AllowedWithoutProvider;
            try { promise->set_value(result); } catch (...) { /* Duplicate callback; first observation wins. */ }
        });
    } catch (...) { return {State::QueryError}; }
    if (future.wait_for(timeout) != std::future_status::ready) return {State::Timeout};
    try { return future.get(); } catch (...) { return {State::QueryError}; }
}
Result check(const db::ClientPtr &client, Requirements requirements, std::chrono::milliseconds timeout) {
    return check(Probe{[client](Reply reply) {
        if (!client) { reply(std::nullopt); return; }
        client->execSqlAsync("SELECT public.usapp_recovery_closed() AS closed",
            [reply](const drogon::orm::Result &rows) {
                std::optional<bool> closed;
                try { if (rows.size() == 1 && !rows[0]["closed"].isNull()) closed = rows[0]["closed"].as<bool>(); }
                catch (...) { closed = std::nullopt; }
                reply(closed);
            },
            [reply](const drogon::orm::DrogonDbException &) { reply(std::nullopt); });
    }}, requirements, timeout);
}
} // namespace usapp::deletionstartup
