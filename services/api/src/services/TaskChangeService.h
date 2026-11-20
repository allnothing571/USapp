// =====================================================================
// services/api/src/services/TaskChangeService.h
//
// M6: confirmed task edits and cancellations (docs/phase1-m6-design.md).
//
// Propose (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members, joined to the root task;
//   2. per-root advisory lock (33004, hashtext(task));
//   3. key classification (same fingerprint -> replay, else IDEMPOTENCY_KEY_REUSED);
//   4. state: closed / undecided proposal / completed / expired / stale terms;
//   5. proposal INSERT ... ON CONFLICT DO NOTHING; audit; read back; commit.
// Decide (approve | reject | withdraw):
//   1. membership FOR SHARE joined to the proposal and its task; actor rules;
//   2. the same per-root advisory lock; 3. key classification; 4. already decided?
//   5. decision INSERT (decided_at is the decision time); for approve, state checks
//      against decided_at, then the successor root (edit) and the closure, all in
//      the same transaction; for an approved edit, one statement copying the assignee's latest
//      reminder rule (P2-6 R4), taking 33017 on the successor last; audit; read back; commit.
//      Rule writes take the same root lock, so a save committed first is the one copied and a
//      later one finds the root closed. A copy failure fails the whole approval.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown.
// Responses separate immutable facts ("change") from current derived state
// ("task", "successor"). Failure details are fixed stage/reason labels only.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

#include "core/Errors.h"
#include "services/TaskService.h"

namespace usapp {

class TaskChangeService {
  public:
    static TaskChangeService &instance();

    enum class Outcome {
        Proposed,          // 201
        Decided,           // 200
        Replayed,          // 200: same key and fingerprint
        Validation,        // 400
        SpaceNotFound,     // 404 SPACE_NOT_FOUND
        TaskNotFound,      // 404 TASK_NOT_FOUND
        ChangeNotFound,    // 404 TASK_CHANGE_NOT_FOUND
        Forbidden,         // 403 TASK_CHANGE_FORBIDDEN
        Pending,           // 409 TASK_CHANGE_PENDING
        HasProgress,       // 409 TASK_HAS_PROGRESS
        AlreadyDecided,    // 409 TASK_CHANGE_DECIDED
        AlreadyCompleted,  // 409 TASK_ALREADY_COMPLETED
        Expired,           // 409 TASK_EXPIRED (task, or the proposed dates, no longer valid)
        Cancelled,         // 409 TASK_CANCELLED
        Superseded,        // 409 TASK_SUPERSEDED
        KeyReused,         // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,     // 503 RESULT_UNKNOWN
        Error,             // 500
    };

    enum class Action { Approve, Reject, Withdraw };

    // Tests only: the copy statement asks for a revision the database guard refuses.
    enum class Fault { None, RuleCopyRefused };

    struct ProposeInput {
        std::string kind;              // "edit" | "cancel"
        TaskService::CreateInput terms;  // edit only; the full new term set
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // user-facing (Chinese), never echoes input
        std::string detail;   // log only: fixed labels
        Json::Value change;   // immutable facts: the proposal (and, for decisions, its decision)
        TaskService::TaskData task;                  // current derived state of the root
        std::optional<TaskService::TaskData> successor;
        bool success() const {
            return outcome == Outcome::Proposed || outcome == Outcome::Decided || outcome == Outcome::Replayed;
        }
    };

    using Callback = std::function<void(Status)>;

    // ---------------------------------------------------------------- pure
    // {"kind":"edit","terms":{...CreateTaskRequest...}} or {"kind":"cancel"}.
    static bool parseProposeRequest(const Json::Value &body, ProposeInput &out, std::string &message);
    // {"action":"approve"|"reject"|"withdraw"}.
    static bool parseDecisionRequest(const Json::Value &body, Action &out, std::string &message);
    static const char *actionName(Action action);
    static std::string proposeFingerprint(const std::string &taskId, const ProposeInput &input);
    static std::string decisionFingerprint(const std::string &proposalId, Action action);
    // {"change": ..., "task": Task, "successor": Task|null}
    static Json::Value responseBody(const Status &status);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    // userId comes from the session; ids passed the uuid shape check; the key passed
    // TaskService::validIdempotencyKey.
    void propose(const std::string &userId, const std::string &taskId, const std::string &key,
                 const ProposeInput &input, Callback cb);
    void decide(const std::string &userId, const std::string &taskId, const std::string &proposalId,
                const std::string &key, Action action, Callback cb, Fault fault = Fault::None);

  private:
    TaskChangeService() = default;
};

}  // namespace usapp
