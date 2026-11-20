// =====================================================================
// services/api/src/scheduler/jobs/PushDispatchJob.h
//
// Phase 2 P2-5b: the push.dispatch scheduler job. All claim work runs on the scheduler's
// Transaction under its advisory lock; afterCommit starts the external work and captures only the
// dispatcher and plain attempt keys, never the Transaction or TxScope.
// Registered at startup without writing anything: a startup never enables, disables or moves the
// cutover. With no transport (this batch) the local capability is incomplete, so an enabled
// control fails closed with a structured error.
// =====================================================================
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "scheduler/Scheduler.h"
#include "services/PushDispatch.h"

namespace usapp::scheduler {

class PushDispatchJob : public SchedulerJob {
  public:
    // Stable value. Changing it changes the lock, see LockKey.h.
    static constexpr const char *kJobKey = "push.dispatch";

    explicit PushDispatchJob(std::shared_ptr<push::Dispatcher> dispatcher);

    std::string jobKey() const override { return kJobKey; }
    void runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) override;

    // Replaces the dispatcher for the next tick (tests switch policy per case).
    void setDispatcher(std::shared_ptr<push::Dispatcher> dispatcher);

  private:
    std::shared_ptr<push::Dispatcher> dispatcher() const;

    mutable std::mutex mutex_;
    std::shared_ptr<push::Dispatcher> dispatcher_;
};

}  // namespace usapp::scheduler
