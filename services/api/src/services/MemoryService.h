// =====================================================================
// services/api/src/services/MemoryService.h
//
// Phase 3 P3-1b: photo memories (docs/phase3-plan.md, migration 0030). Either member creates a memory. The creator edits
// directly with expected_version; the partner proposes a full replacement, including the ordered photos, which the creator
// approves or rejects and the proposer may withdraw. One pending proposal per memory; a creator edit is blocked while one is
// pending, as anniversaries are. Revisions and proposals are immutable, so the original and the suggestion stay reviewable.
//
// Writes (one TxScope, READ COMMITTED), in a fixed lock order that completion and cleanup cannot invert, because they hold
// only 33018 and never 33020:
//   1. caller's member row FOR SHARE, joined to the target ids;
//   2. advisory lock 33020 on the memory;
//   3. key classification: the same fingerprint replays the committed write, read-only, before any state check;
//   4. role and state checks;
//   5. 33018 on each photo, in sorted order, then eligibility, read after the waits;
//   6. inserts; bindings before photos; then audit and read-back; commit.
//
// Photo references. A revision's photos are BOUND for good, so history is never eligible for cleanup. A pending proposal's
// new photos are only RESERVED: approval binds them in the revision's transaction, and a rejection or withdrawal releases them
// by deciding the proposal, after which the unattached period counts from that decision.
//
// Notifications are not written here: they need a new event family and a Push worker change, left for review.
//
// D6 M1a (migration 0036, docs/phase3-d6-memory-deletion-design.md). The creator deletes and undoes, under the same lock order and with the
// key classified first. Every read answers as if a deleted memory were absent, and that includes a replay, which reads today's memory: a
// replay of a create or an edit never brings deleted content back. Deletion and undo answer with the facts of the exact cycle, never text or
// photos, and a replay of an old undo says only that its cycle was undone, not that the memory is visible now. All text and bytes stay.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "services/DeletionJournal.h"
#include "services/NotificationIntent.h"

namespace usapp {

class MemoryService {
  public:
    static MemoryService &instance();
    // No production provider configured in main. Tests inject a fake; each
    // destructive request snapshots it. Requests/reject/withdraw stay ordinary.
    void configureDeletionJournal(std::shared_ptr<deletionjournal::Journal> journal);

    static constexpr size_t kMaxTitleCodePoints = 60;
    static constexpr size_t kMaxDescriptionCodePoints = 2000;
    static constexpr size_t kMaxPhotos = 9;

    enum class Outcome {
        Created,           // 201
        Replayed,          // 200: same key and fingerprint
        Ok,                // 200: a read or a decision
        Validation,        // 400
        SpaceNotFound,     // 404 SPACE_NOT_FOUND
        NotFound,          // 404 MEMORY_NOT_FOUND (absent or another space)
        Forbidden,         // 403 MEMORY_EDIT_FORBIDDEN
        VersionConflict,   // 409 MEMORY_VERSION_CONFLICT
        ProposalPending,   // 409 MEMORY_PROPOSAL_PENDING
        ProposalNotFound,  // 404 MEMORY_PROPOSAL_NOT_FOUND
        ProposalDecided,   // 409 MEMORY_PROPOSAL_DECIDED
        ProposalStale,     // 409 MEMORY_PROPOSAL_STALE
        PhotoUnavailable,  // 409 MEMORY_PHOTO_UNAVAILABLE
        KeyReused,         // 409 IDEMPOTENCY_KEY_REUSED
        PlanNotFound,     // 404 NOT_FOUND
        PlanConflict,     // 409 PLAN_CONFLICT
        AlreadyDeleted,    // 409 MEMORY_ALREADY_DELETED
        UndoClosed,        // 409 MEMORY_UNDO_CLOSED
        DeletionNotActive, // 409 MEMORY_DELETION_NOT_ACTIVE
        ResultUnknown,     // 503 RESULT_UNKNOWN
        Error,             // 500
    };

    // Normalized terms. date is "YYYY-MM-DD"; mediaIds are lowercase and in display order.
    struct Terms {
        std::string date;
        std::string title;
        std::string description;
        std::vector<std::string> mediaIds;
    };

    enum class Decision { Approve, Reject, Withdraw };
    // Test only; production never passes these. IgnoreUndoPrecheck reaches the reversal guard as if the deadline had passed just after the
    // service looked: the race, with no timing involved.
    // FailDeleteApproval stops an approval after its decision and cycle are written, before its commit.
    // FailAfterCascade stops a direct deletion after its cycle and linked photo cycles are written, before its commit.
    enum class Fault { None, FailPhotoBinding, FailPlanLink, IgnoreUndoPrecheck, FailDeleteApproval, FailAfterCascade };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;    // fixed labels, for logs
        Json::Value mutation;  // immutable facts of the write or replay
        Json::Value memory;    // current state
        Json::Value proposal;  // a proposal read: original, proposed and decision
        Json::Value deletion;  // deletion and undo: the facts of one exact cycle, never text or photos
        Json::Value request;   // a delete request: its facts and status, never text or photos
        bool hasNotification = false;  // D20: a direct create, new or replayed, carries its original notification decision
        Json::Value notification;
        bool success() const { return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok; }
    };
    using Callback = std::function<void(Status)>;

    struct Cursor {
        std::string date;
        std::string createdAt;
        std::string id;
    };
    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<Json::Value> memories;
        std::optional<std::string> nextCursor;
        long long currentVersion = 0;  // listProposals only: the memory's latest version, read by the same statement
    };
    using ListCallback = std::function<void(ListResult)>;

    // Pure.
    static bool parseCreateRequest(const Json::Value &body, Terms &terms, std::string &message);
    static bool parseRevisionRequest(const Json::Value &body, int &expectedVersion, Terms &terms, std::string &message);
    static bool parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message);
    static std::string createFingerprint(const Terms &terms);
    static std::string editFingerprint(const std::string &memoryId, int expectedVersion, const Terms &terms);
    static std::string proposeFingerprint(const std::string &memoryId, int expectedVersion, const Terms &terms);
    static std::string decisionFingerprint(const std::string &memoryId, const std::string &proposalId, Decision decision);
    static std::string deletionFingerprint(const std::string &memoryId);
    static std::string undoFingerprint(const std::string &memoryId, const std::string &deletionId);
    static std::string deleteRequestFingerprint(const std::string &memoryId);
    static std::string deleteDecisionFingerprint(const std::string &memoryId, const std::string &requestId, Decision decision);
    static std::string encodeCursor(const Cursor &cursor);
    static std::optional<Cursor> decodeCursor(const std::string &raw);
    static Json::Value responseBody(const Status &status);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // Actions.
    // D20: `choice` is the optional notify_partner of a direct create, default off. Plan conversion (createForPlan) never notifies.
    void create(const std::string &userId, const std::string &key, const Terms &terms, Callback cb, intent::Choice choice = {});
    void createForPlan(const std::string &userId, const std::string &planId, const std::string &key,
                       const Terms &terms, Callback cb, Fault fault = Fault::None);
    void edit(const std::string &userId, const std::string &memoryId, const std::string &key, int expectedVersion, const Terms &terms, Callback cb);
    void propose(const std::string &userId, const std::string &memoryId, const std::string &key, int expectedVersion, const Terms &terms, Callback cb);
    void decide(const std::string &userId, const std::string &memoryId, const std::string &proposalId, const std::string &key, Decision decision,
                Callback cb, Fault fault = Fault::None);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);
    void get(const std::string &userId, const std::string &memoryId, Callback cb);
    void getProposal(const std::string &userId, const std::string &memoryId, const std::string &proposalId, Callback cb);
    // One memory's edit proposals, pending and decided, newest first by (created_at, id). Summaries only: no text, photos or keys; the detail
    // read has those. Both members while the memory is visible; an absent, deleted, purged or other space's memory is NotFound. Reads only.
    // The position is the created_at and id of the last row of the previous page, as AlbumService::decodeCursor returns them.
    void listProposals(const std::string &userId, const std::string &memoryId, int limit,
                       const std::optional<std::pair<std::string, std::string>> &after, ListCallback cb);
    // Created, or Replayed with the original cycle's facts as they are today. Any pending edit proposal is rejected in the same transaction.
    // D6 M1c: the creator's own photos bound to it, not already deleted, are deleted with it, with the same time and deadline.
    void deleteMemory(const std::string &userId, const std::string &memoryId, const std::string &key, Callback cb, Fault fault = Fault::None);
    // Ok, or Replayed. Only the original deleter of this exact cycle, strictly before its deadline.
    void undoDeletion(const std::string &userId, const std::string &memoryId, const std::string &deletionId, const std::string &key, Callback cb,
                      Fault fault = Fault::None);
    // The caller's own cycles that can be undone now, newest first. A snapshot: a listed cycle can still close before an undo arrives.
    // The position is the deleted_at and id of the last row of the previous page, as AlbumService::decodeCursor returns them.
    void listDeletions(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after, ListCallback cb);
    // D6 M1b. The partner asks; the creator approves or rejects; the requester withdraws. Created, or Replayed with that request's facts as they
    // are today, which say nothing about whether the memory is visible now.
    void requestDeletion(const std::string &userId, const std::string &memoryId, const std::string &key, Callback cb);
    // Ok, or Replayed. An approval's decision and cycle commit together, and only the approver's answer carries the cycle.
    void decideDeleteRequest(const std::string &userId, const std::string &memoryId, const std::string &requestId, const std::string &key, Decision decision,
                             Callback cb, Fault fault = Fault::None);
    void getDeleteRequest(const std::string &userId, const std::string &memoryId, const std::string &requestId, Callback cb);

  private:
    MemoryService() = default;
    std::mutex journalMutex_;
    std::shared_ptr<deletionjournal::Journal> deletionJournal_;
};

}  // namespace usapp
