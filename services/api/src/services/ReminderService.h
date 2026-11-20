// =====================================================================
// services/api/src/services/ReminderService.h
//
// Phase 2 P2-6 R1: the caller's own reminder rules (docs/phase2-reminders.md, migration 0025). This
// service writes rules only: nothing here creates an occurrence or a notification, and no job reads
// these rows in R1. A task rule belongs to the task's assignee; an anniversary rule to any member, for
// themselves. The owner is always the session's user, never a request field.
//
// Writes (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members, joined to the target of the same space;
//      assignee check (immutable, so before the key);
//   2. domain lock: 33004 on the root task, or 33010 on the anniversary;
//   3. key classification on (owner, key), BEFORE any mutable state: the same fingerprint replays the
//      original facts read-only, appending and restoring nothing; otherwise IDEMPOTENCY_KEY_REUSED;
//   4. task state and schedule bounds through usapp_reminder_task_rule_problem, as the guard does;
//   5. rule lock 33017 on (owner, target), then INSERT at seq + 1 ... ON CONFLICT DO NOTHING
//      (a skipped insert is classified again);
//   6. audit in the same transaction (ids, seq, enabled, mode; never times or text);
//   7. read back the immutable facts and the current state; commit.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown, and retrying the
// same key and body is safe.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"

namespace usapp {

class ReminderService {
  public:
    static ReminderService &instance();

    enum class Outcome {
        Created,        // 201
        Replayed,       // 200: same key and fingerprint
        Ok,             // 200: read
        Validation,     // 400
        SpaceNotFound,  // 404 SPACE_NOT_FOUND
        TaskNotFound,   // 404 TASK_NOT_FOUND (absent or another space)
        AnniversaryNotFound,  // 404 ANNIVERSARY_NOT_FOUND (absent or another space)
        NotAssignee,    // 403 REMINDER_NOT_ASSIGNEE
        TargetClosed,   // 409 REMINDER_TARGET_CLOSED
        TimeInvalid,    // 400 REMINDER_TIME_INVALID
        TimePassed,     // 409 REMINDER_TIME_PASSED
        KeyReused,      // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,  // 503 RESULT_UNKNOWN
        Error,          // 500
    };

    enum class TargetKind { Task, Anniversary };

    // Normalized terms. A disabled rule carries nothing else. Text fields are empty when absent.
    struct Terms {
        bool enabled = false;
        std::string mode;           // before_deadline | local_time | absolute | anniversary_offsets
        std::optional<int> beforeMinutes;
        std::optional<int> daysBefore;
        std::string atTime;         // HH:MM
        std::string atLocalDate;    // YYYY-MM-DD
        std::vector<int> offsets;   // ascending, distinct
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // user-facing (Chinese), never echoes input
        std::string detail;   // log only: fixed labels
        Json::Value mutation; // immutable facts of the write or replay
        Json::Value current;  // current derived state, separate from those facts
        bool success() const {
            return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok;
        }
    };

    struct Cursor {
        std::string createdAt;
        std::string id;
    };

    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<Json::Value> reminders;
        std::optional<std::string> nextCursor;
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // ---------------------------------------------------------------- pure
    // Exactly {enabled:false}, or enabled:true with one mode's fields. Schedule-specific bounds are the
    // database's (they need the task); these are the widest values of any schedule.
    static bool parseTaskRequest(const Json::Value &body, Terms &terms, std::string &message);
    // Exactly {enabled:false}, or {enabled:true, days_before:[...][, at_time]}; offsets are sorted.
    static bool parseAnniversaryRequest(const Json::Value &body, Terms &terms, std::string &message);
    static bool validTime(const std::string &text);
    static std::string fingerprint(TargetKind kind, const std::string &targetId, const Terms &terms);
    static std::string offsetsLiteral(const std::vector<int> &offsets);  // {1,3}, or empty
    static Json::Value responseBody(const Status &status);
    // Configuration status only: generation needs both the job's flag and the scheduler that runs it. It says nothing
    // about whether a tick ran or a reminder was delivered, and nothing about Push.
    static bool generationEnabled(bool remindersEnabled, bool schedulerEnabled);
    static Json::Value listBody(const ListResult &result, bool generationEnabled);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    void set(const std::string &userId, TargetKind kind, const std::string &targetId, const std::string &key,
             const Terms &terms, Callback cb);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);

  private:
    ReminderService() = default;
};

}  // namespace usapp
