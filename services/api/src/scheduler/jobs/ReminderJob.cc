// =====================================================================
// services/api/src/scheduler/jobs/ReminderJob.cc
// =====================================================================
#include "scheduler/jobs/ReminderJob.h"

#include <utility>

namespace usapp::scheduler {

ReminderJob::ReminderJob(std::shared_ptr<reminder::Generator> generator) : generator_(std::move(generator)) {}

std::shared_ptr<reminder::Generator> ReminderJob::generator() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generator_;
}

void ReminderJob::setGenerator(std::shared_ptr<reminder::Generator> next) {
    std::lock_guard<std::mutex> lock(mutex_);
    generator_ = std::move(next);
}

void ReminderJob::runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) {
    auto current = generator();
    current->runTick(tx, [current, done](reminder::TickReport report) {
        Result result;
        result.ok = report.ok;
        result.detail = report.detail;
        done(std::move(result));
    });
}

}  // namespace usapp::scheduler
