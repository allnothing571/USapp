// =====================================================================
// services/api/src/scheduler/jobs/MemoryPurgeJob.h
//
// D6 memory.purge. Without a journal, retains the M1d transaction path. With an
// injected journal, selects candidates only and starts external work after
// scheduler commit; each erase revalidates through usapp_memory_erase in a new
// transaction. MEMORY_PURGE_ENABLED stays off by default. Shared text is never
// erased while another unerased memory uses it. No object-store calls here.
// =====================================================================
#pragma once

#include <atomic>
#include <string>
#include <memory>

#include "scheduler/Scheduler.h"
#include "services/DeletionJournal.h"

namespace usapp::scheduler {

struct MemoryPurgeJournalState;

class MemoryPurgeJob : public SchedulerJob {
  public:
    // Stable value. Changing it changes the lock, see LockKey.h.
    static constexpr const char *kJobKey = "memory.purge";
    static constexpr int kBatch = 50;

    explicit MemoryPurgeJob(bool enabled);

    std::string jobKey() const override { return kJobKey; }
    void runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) override;

    // Tests switch it per case. Production sets it once, at startup.
    void setEnabled(bool enabled);
    // Injection seam only; main configures no real provider. A journal path
    // queues after-commit work, never an external call under the scheduler tx.
    void configureJournal(std::shared_ptr<deletionjournal::Journal> journal);
    struct BackgroundReport { int confirmed = 0, unknown = 0, cancelled = 0; };
    bool journalBusy() const;
    BackgroundReport journalReport() const;

  private:
    std::shared_ptr<MemoryPurgeJournalState> journalState_;
};

}  // namespace usapp::scheduler
