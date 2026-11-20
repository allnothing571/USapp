// =====================================================================
// services/api/src/services/AnniversaryService.h
//
// Phase 2 P2-2: shared anniversaries and countdowns (docs/phase2-anniversaries.md,
// owner-approved defaults O1-O10). Either member creates an item, also before the
// partner joins. The creator edits directly with expected_version; the partner proposes
// a full replacement against the current version, which the creator approves or rejects
// and the proposer may withdraw. One pending proposal per item; a creator edit is
// blocked while one is pending (option A). Each member pins one item (or none) to their
// own Home. All five tables are append-only (migration 0018).
//
// Writes (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members (joined to the target ids);
//   2. advisory lock 33010 on the anniversary (33011 on the user for a pin);
//   3. key classification on (actor, key): the same fingerprint replays the committed
//      write, read-only; otherwise IDEMPOTENCY_KEY_REUSED. This runs before any state
//      check, so a retry of a committed edit replays instead of conflicting;
//   4. role and state checks;
//   5. INSERT ... ON CONFLICT DO NOTHING (a skipped insert is classified again);
//   6. audit in the same transaction (ids, version, kind, outcome; never text);
//   7. read back the immutable mutation and the current state; commit.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown.
//
// "Today" is the Beijing civil date read once per statement and returned as as_of_date;
// every computed value comes from AnniversaryCalendar against it.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "services/NotificationIntent.h"

namespace usapp {

class AnniversaryService {
  public:
    static AnniversaryService &instance();

    static constexpr size_t kMaxTitleCodePoints = 40;
    static constexpr size_t kMaxNoteCodePoints = 500;

    enum class Outcome {
        Created,          // 201
        Replayed,         // 200: same key and fingerprint
        Ok,               // 200: read, or a decision/pin
        Validation,       // 400
        SpaceNotFound,    // 404 SPACE_NOT_FOUND
        NotFound,         // 404 ANNIVERSARY_NOT_FOUND (absent or another space)
        Forbidden,        // 403 ANNIVERSARY_EDIT_FORBIDDEN
        VersionConflict,  // 409 ANNIVERSARY_VERSION_CONFLICT
        ProposalPending,  // 409 ANNIVERSARY_PROPOSAL_PENDING
        ProposalNotFound, // 404 ANNIVERSARY_PROPOSAL_NOT_FOUND
        ProposalDecided,  // 409 ANNIVERSARY_PROPOSAL_DECIDED
        ProposalStale,    // 409 ANNIVERSARY_PROPOSAL_STALE
        KeyReused,        // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,    // 503 RESULT_UNKNOWN
        Error,            // 500
    };

    // Normalized terms. date is "YYYY-MM-DD" within 1900-2199; kind anniversary|countdown.
    struct Terms {
        std::string title;
        std::string date;
        std::string kind;
        std::string note;
        bool operator==(const Terms &o) const {
            return title == o.title && date == o.date && kind == o.kind && note == o.note;
        }
    };

    struct ProposalData {
        std::string id;
        std::string proposerId;
        int baseVersion = 0;
        Terms terms;
        std::string createdAt;
    };

    struct DecisionData {
        std::string proposalId;
        std::string proposerId;
        std::string outcome;  // approved | rejected | withdrawn
        std::string decidedBy;
        std::string decidedAt;
    };

    struct AnniversaryData {
        std::string id;
        std::string creatorId;
        int version = 0;
        Terms terms;
        std::string createdAt;
        std::string updatedAt;
        bool pinnedByMe = false;
        std::optional<ProposalData> pending;
        std::optional<DecisionData> lastDecision;
    };

    struct PinData {
        int seq = 0;  // 0: never pinned
        std::optional<std::string> anniversaryId;
        std::string updatedAt;  // empty when never pinned
    };

    enum class Decision { Approve, Reject, Withdraw };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;   // user-facing (Chinese), never echoes input
        std::string detail;    // log only: fixed labels
        Json::Value mutation;  // immutable facts of the write (or replay)
        std::string asOfDate;  // Beijing civil date of the read-back
        bool isPin = false;    // response carries "pin" instead of "anniversary"
        AnniversaryData data;
        PinData pin;
        // P2-3b, create and creator edit only: original notification facts (Json null for a
        // legacy revision without a recorded intent).
        bool hasNotification = false;
        Json::Value notification;
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
        std::string asOfDate;
        std::vector<AnniversaryData> anniversaries;
        std::optional<std::string> nextCursor;
    };

    struct HomeResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::string asOfDate;
        std::string source;  // pinned | auto | auto_past | none
        std::optional<AnniversaryData> anniversary;
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;
    using HomeCallback = std::function<void(HomeResult)>;

    // ---------------------------------------------------------------- pure
    // Exactly {title, date, kind[, note]}.
    static bool parseCreateRequest(const Json::Value &body, Terms &terms, std::string &message);
    // Exactly {expected_version, title, date, kind[, note]} (creator edit and partner proposal).
    static bool parseRevisionRequest(const Json::Value &body, int &expectedVersion, Terms &terms,
                                     std::string &message);
    // Exactly {outcome: approve|reject|withdraw}.
    static bool parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message);
    // Exactly {anniversary_id: uuid|null}; the id is lowercased.
    static bool parsePinRequest(const Json::Value &body, std::optional<std::string> &anniversaryId,
                                std::string &message);

    static std::string createFingerprint(const Terms &terms);
    static std::string editFingerprint(const std::string &anniversaryId, int expectedVersion, const Terms &terms);
    static std::string proposeFingerprint(const std::string &anniversaryId, int expectedVersion, const Terms &terms);
    static std::string decisionFingerprint(const std::string &anniversaryId, const std::string &proposalId,
                                           Decision decision);
    static std::string pinFingerprint(const std::optional<std::string> &anniversaryId);
    static const char *decisionName(Decision decision);  // approve | reject | withdraw
    static const char *outcomeName(Decision decision);   // approved | rejected | withdrawn

    static std::string encodeCursor(const Cursor &cursor);
    static std::optional<Cursor> decodeCursor(const std::string &raw);

    // computed: {days_since, next, days_until} against asOfDate ("YYYY-MM-DD").
    static Json::Value computedJson(const Terms &terms, const std::string &asOfDate);
    static Json::Value anniversaryJson(const AnniversaryData &data, const std::string &asOfDate);
    static Json::Value pinJson(const PinData &pin);
    // {"mutation", "as_of_date", "anniversary"[, "notification"]} or {"mutation", "pin"}.
    static Json::Value responseBody(const Status &status);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    // P2-3b: choice is the optional "notify_partner" (see services/NotificationIntent.h).
    void create(const std::string &userId, const std::string &key, const Terms &terms, Callback cb,
                intent::Choice choice = {});
    void edit(const std::string &userId, const std::string &anniversaryId, const std::string &key,
              int expectedVersion, const Terms &terms, Callback cb, intent::Choice choice = {});
    void propose(const std::string &userId, const std::string &anniversaryId, const std::string &key,
                 int expectedVersion, const Terms &terms, Callback cb);
    void decide(const std::string &userId, const std::string &anniversaryId, const std::string &proposalId,
                const std::string &key, Decision decision, Callback cb);
    void pin(const std::string &userId, const std::string &key, const std::optional<std::string> &anniversaryId,
             Callback cb);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);
    void get(const std::string &userId, const std::string &anniversaryId, Callback cb);
    // Reads the complete current dataset of the space (no page limit) and selects in C++.
    void home(const std::string &userId, HomeCallback cb);

  private:
    AnniversaryService() = default;
};

}  // namespace usapp
