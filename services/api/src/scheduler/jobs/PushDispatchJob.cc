// =====================================================================
// services/api/src/scheduler/jobs/PushDispatchJob.cc
// =====================================================================
#include "scheduler/jobs/PushDispatchJob.h"

#include <utility>
#include <vector>

namespace usapp::scheduler {

PushDispatchJob::PushDispatchJob(std::shared_ptr<push::Dispatcher> dispatcher) : dispatcher_(std::move(dispatcher)) {}

std::shared_ptr<push::Dispatcher> PushDispatchJob::dispatcher() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatcher_;
}

void PushDispatchJob::setDispatcher(std::shared_ptr<push::Dispatcher> next) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = std::move(next);
}

void PushDispatchJob::runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) {
    auto current = dispatcher();
    current->runTick(tx, [current, done](push::TickReport report) {
        Result result;
        result.ok = report.ok;
        result.detail = report.detail;
        if (report.ok && !report.claims.empty()) {
            // Runs only after the scheduler's confirmed commit; captures no transaction.
            result.afterCommit = [current, claims = std::move(report.claims)]() { current->sendClaims(claims); };
        }
        done(std::move(result));
    });
}

}  // namespace usapp::scheduler
