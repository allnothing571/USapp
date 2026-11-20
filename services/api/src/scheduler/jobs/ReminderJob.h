// =====================================================================
// services/api/src/scheduler/jobs/ReminderJob.h
//
// Phase 2 P2-6 R2: the reminder.generate scheduler job. All work runs on the scheduler's Transaction under its
// advisory lock. Registered at startup, and before push.dispatch, without writing anything. REMINDERS_ENABLED
// (default false) off: the tick reports dispatch=off, so only the scheduler's own tick row is written.
// =====================================================================
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "scheduler/Scheduler.h"
#include "services/ReminderGeneration.h"

namespace usapp::scheduler {

class ReminderJob : public SchedulerJob {
  public:
    // Stable value. Changing it changes the lock, see LockKey.h.
    static constexpr const char *kJobKey = "reminder.generate";

    explicit ReminderJob(std::shared_ptr<reminder::Generator> generator);

    std::string jobKey() const override { return kJobKey; }
    void runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) override;

    // Replaces the generator for the next tick (tests switch policy per case).
    void setGenerator(std::shared_ptr<reminder::Generator> generator);

  private:
    std::shared_ptr<reminder::Generator> generator() const;

    mutable std::mutex mutex_;
    std::shared_ptr<reminder::Generator> generator_;
};

}  // namespace usapp::scheduler
