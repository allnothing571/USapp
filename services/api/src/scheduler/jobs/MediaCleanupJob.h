// =====================================================================
// services/api/src/scheduler/jobs/MediaCleanupJob.h
//
// Phase 3 P3-1a: the media.cleanup scheduler job. Plans are written on the scheduler's transaction; deletes and listing
// run after its commit. With no store configured it writes nothing. See services/MediaService.h.
// =====================================================================
#pragma once

#include "scheduler/Scheduler.h"
#include "services/MediaService.h"

namespace usapp::scheduler {

class MediaCleanupJob : public SchedulerJob {
  public:
    // Stable value. Changing it changes the lock, see LockKey.h.
    static constexpr const char *kJobKey = "media.cleanup";

    std::string jobKey() const override { return kJobKey; }
    void runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) override {
        MediaService::instance().runTick(tx, [done](MediaService::TickReport report) {
            Result result;
            result.ok = report.ok;
            result.detail = report.detail;
            result.afterCommit = std::move(report.afterCommit);
            done(std::move(result));
        });
    }
};

}  // namespace usapp::scheduler
