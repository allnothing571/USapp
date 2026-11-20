// =====================================================================
// services/api/src/services/ReminderGeneration.h
//
// Phase 2 P2-6 R2: in-app reminder generation (docs/phase2-reminders.md Sections 5-9, 11). Runs on the
// scheduler's Transaction under its job lock; READ COMMITTED, so each statement has a fresh snapshot:
//   1. preflight: the 0026 repair and 0027 are present (else fail closed), bounded lock wait for this transaction;
//   2. scan: enumerate -> exclude ineligible -> exclude existing identities -> total order -> LIMIT;
//   3. lock, one statement: sorted 33004 and 33010, then sorted 33017;
//   4. re-evaluate the same definition under a new snapshot and insert occurrences. A candidate that no longer
//      matches on identity, rule revision and instant is dropped, never moved to another period;
//   5. insert exactly one notification per inserted occurrence.
// No network, no afterCommit, no per-row statement from here (the frozen 0025 guards do run per row), and no
// reference to Push, subscriptions or devices. A failure rolls the whole tick back.
// =====================================================================
#pragma once

#include <drogon/orm/DbClient.h>

#include <cstddef>
#include <functional>
#include <string>

namespace usapp::reminder {

struct Policy {
    bool enabled = false;      // REMINDERS_ENABLED. Off: nothing is read or written here.
    int limit = 100;           // candidates per tick (design Section 9.1)
    int lockTimeoutMs = 3000;  // a stuck holder fails the tick, which releases what it already holds
    // Tests only: the scan's clock, so a candidate can be read as of another instant. Insertion always uses the
    // real clock. Never set outside tests.
    std::string scanAtForTest;
};

struct TickReport {
    bool ok = false;
    std::string detail;  // counts and fixed labels only
    size_t scanned = 0;
    size_t created = 0;
    long long lockHeldMs = 0;  // first lock statement sent to notifications inserted; excludes the commit
};

// $1: the scan's clock, '' for the real clock. $2: limit. One row: candidates (a JSON array, in total order) and n.
extern const std::string kScanSql;

class Generator {
  public:
    explicit Generator(Policy policy);

    // All statements use tx. done is called exactly once and after it tx is not used again.
    void runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done) const;

    const Policy &policy() const { return policy_; }

  private:
    Policy policy_;
};

}  // namespace usapp::reminder
