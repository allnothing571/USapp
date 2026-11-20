// =====================================================================
// services/api/src/services/ExportService.h
// E1 structured user-data export (docs/user-data-export-design.md). One READ ONLY transaction at READ COMMITTED:
// shared recovery lock (33026, 0) in its own statement, then one generation statement that measures every capped
// section before any array aggregation. Read only: no audit, notification, wallet, journal or object-store effect.
// The admission gate, frequency map and deadline timer are process-local; nothing here bounds other processes. Each slot is a
// dedicated single-connection session; a slot is released only after the transaction's end is confirmed on that session.
// =====================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace usapp {

class ExportService {
  public:
    enum class Outcome {
        Ok,            // complete document in body
        SpaceNotFound, // 404 SPACE_NOT_FOUND
        TooLarge,      // 413 EXPORT_TOO_LARGE (a row, input-byte or final-size cap)
        Busy,          // 429 RATE_LIMITED (process-local admission or frequency)
        Unavailable,   // 503 EXPORT_UNAVAILABLE (deadline, lock or statement timeout, database or session failure, not initialized)
    };

    struct Result {
        Outcome outcome = Outcome::Unavailable;
        std::string body;        // the complete document, only for Ok; never partial
        std::string filename;    // usapp-export-<UTC stamp>.json, only for Ok
        std::string reason;      // fixed label for logs and tests: deadline, lock_timeout, statement_timeout, database,
                                 // section_rows, total_rows, input_bytes, output_bytes, concurrent, global, frequency
        int retryAfterSeconds = 0;
        long long rows = 0;
        long long inputBytes = 0;
    };

    struct Limits {
        int maxSectionRows = 20000;
        long long maxTotalRows = 60000;
        long long maxInputBytes = 15LL * 1024 * 1024;
        long long maxOutputBytes = 16LL * 1024 * 1024;
        int lockTimeoutMs = 5000;
        int statementTimeoutMs = 15000;
        int deadlineMs = 20000;
        int maxGlobal = 2;
        int frequencySeconds = 60;
    };

    using Callback = std::function<void(Result)>;

    // Reported once per admitted export, after its transaction's end was confirmed and its slot released.
    struct Release {
        std::string user;
        int slot = -1;
        int pid = 0;           // diagnostic only: the backend seen after BEGIN (0 when BEGIN's reply was never seen)
        std::string cleanup;   // "ended" (tagged session, outside any transaction) or "session_replaced" (no tagged backend open)
        int verifyAttempts = 0;
    };

    // Creates the dedicated single-connection sessions, one per admission slot (each is one extra PostgreSQL connection per
    // process, kept for the process lifetime and never destroyed). Call once before serving; repeated calls are ignored. Without
    // it every export answers EXPORT_UNAVAILABLE.
    static void init(const std::string &connectionString, int slots);

    // The caller only, from the session. The callback runs exactly once, on any thread.
    static void exportFor(const std::string &userId, Callback cb);

    static Limits limits();
    // Tests only: different bounds, and a pause after the lock is held (the hook must call resume exactly once).
    static void setLimitsForTesting(const Limits &limits);
    static void resetLimitsForTesting();
    static void setAfterLockHookForTesting(std::function<void(std::function<void()> resume)> hook);
    static void clearAdmissionsForTesting();
    // Tests only: pause after the end was requested and before this service's ROLLBACK is sent (call proceed exactly once).
    static void setBeforeEndHookForTesting(std::function<void(std::function<void()> proceed)> hook);
    static void setReleaseObserverForTesting(std::function<void(const Release &)> observer);
    // Tests only: treat the next N end verifications as failed, as an unreachable server would (they are retried).
    static void setVerifyFaultsForTesting(int failures);
    static int busySlotsForTesting();
    // Per slot: idle, working, end_requested or unconfirmed (held because the end could not be confirmed).
    static std::vector<std::string> slotStatesForTesting();
    // Tests only: learn each admitted export's session token; make the next BEGIN's reply count as lost; replace the captured
    // (diagnostic) pid, to show no decision depends on it.
    static void setTokenObserverForTesting(std::function<void(const std::string &)> observer);
    static void setBeginReplyLossForTesting(bool once, bool silent = false);
    static void setGenerationReplyLossForTesting(bool once);
    static void setCapturedPidOverrideForTesting(int pid);

    // The generation statement: $1 user, $2 section row cap, $3 total row cap, $4 input byte cap.
    static const std::string &generationSql();
};

}  // namespace usapp
