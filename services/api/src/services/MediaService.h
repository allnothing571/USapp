// =====================================================================
// services/api/src/services/MediaService.h
//
// Phase 3 P3-1a: private media (docs/phase3-plan.md Section 4, migration 0029). Bytes never pass through this
// process. READY names one immutable object; every read and attachment resolves to it through
// outcome -> attempt -> final_key, and no path builds a key from a client value.
//
// Completion. No lock is held across any store call:
//   1. read; 2. HEAD staging; 3. transaction: commit an attempt row with a new final key; 4. copy, once, to that key;
//   5. fence read; 6. HEAD, imageInfo, HEAD on the final key; 7. transaction: outcome plus cleanup plan.
// Inspecting the copy means the inspected bytes are the promoted bytes, whatever later happens to staging.
//
// The fence: a cleanup plan blocks every later attempt and outcome, in the database guards as well as here. An attempt
// that resumes after a plan removes its own key and never publishes anything. An attempt that never resumes, or a copy
// that lands after reporting failure, leaves a late object: the reconciler lists the prefixes and removes what
// usapp_media_key_disposition calls late. It reports keys no row names, and never removes them.
//
// With no store configured (the default: no real adapter exists before P3-1d) writes and signed reads answer
// MEDIA_DISABLED and the job writes nothing. Facts stay readable.
//
// D6 (migration 0035, docs/phase3-d6-media-deletion-design.md). Deletion and undo need no store: a person can always hide their
// own photo. In one transaction: member FOR SHARE -> 33025 on (actor, operation, key) -> classification -> 33018 on the photo ->
// state read on the database clock -> append -> audit -> commit.
//
// Reads after a deletion. A signed URL is made holding 33018, after reading the state in a new statement. So a request that starts
// after the commit always answers 404 and signs nothing, for the uploader too. A request already past its read when the deletion
// commits may finish, and the URL it was given, like any URL issued earlier, keeps working until it expires or the object is
// purged. It cannot be recalled, and neither can a copy in a browser cache or on a device.
//
// With an external deletion journal configured (J2b1 delete/undo, J2c2 cleanup), deletion, undo and every cleanup removal first
// have a durable intent; with none, the original paths run unchanged. No production provider is wired.
// =====================================================================
#pragma once

#include <drogon/orm/DbClient.h>
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
#include "services/MediaStore.h"

namespace usapp {

class MediaService {
  public:
    static MediaService &instance();

    enum class Outcome {
        Created, Replayed, Ok, Disabled, NotFound, SpaceNotFound, KeyReused, LimitExceeded, NotUploaded, GrantExpired,
        StoreUnavailable, ResultUnknown, Error,
        AlreadyDeleted,     // 409: this photo already has an unreversed deletion
        UndoClosed,         // 409: the deadline was reached, or a purge plan has committed
        DeletionNotActive,  // 409: this cycle was already undone; a later one is not affected
        DeletionLinked,     // 409: this cycle belongs to a memory's deletion, and is undone only with it
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;      // fixed labels, for logs
        Json::Value media;       // facts; never a key
        Json::Value upload;      // null unless a usable grant exists
        Json::Value deletion;    // deletion and undo only; never a key
        std::string redirectUrl;
        bool success() const { return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok; }
    };
    using Callback = std::function<void(Status)>;

    struct Policy {
        int grantSeconds = 900;
        int maxAttempts = 5;              // also the 0029 CHECK
        int readUrlSeconds = 600;
        int readBucketSeconds = 300;      // the same image keeps one URL within a bucket, so browsers can cache it
        // How long after a plan a late write is still expected. A vendor timing assumption: P3-1d gate.
        int sweepGraceSeconds = 3600;
        int abandonAfterSeconds = 3600;   // after expiry; the 0029 guard's floor is 15 minutes
        int unattachedSeconds = 604800;   // the 0029 guard's floor is 7 days
        int planLimit = 100;
        int scanPageSize = 100;
    };

    // Test only; production never passes these. IgnoreUndoPrecheck reaches the reversal guard as if the deadline had passed, or a
    // plan had appeared, just after the service looked: the race, with no timing involved.
    enum class Fault { None, FailBeforeOutcomeCommit, IgnoreUndoPrecheck };

    struct DeletionPage {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<Json::Value> items;
        std::optional<std::string> nextCursor;
    };

    // Pure.
    static bool allowedType(const std::string &contentType);
    static std::string fingerprint(const std::string &contentType, int64_t size);
    static bool parseUploadRequest(const Json::Value &body, std::string &contentType, int64_t &size, std::string &message);
    static std::string rejectionFor(int64_t storedSize, int64_t declaredSize, const std::string &realFormat,
                                    const std::string &declaredType, int width, int height);  // empty: acceptable
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);
    static Json::Value responseBody(const Status &status);
    static std::string deletionFingerprint(const std::string &mediaId);
    static std::string undoFingerprint(const std::string &mediaId, const std::string &deletionId);

    // A null store is the disabled state.
    void configure(Policy policy, std::shared_ptr<media::ObjectStore> store);
    // What the timeline tells clients. Existing memories stay listed either way.
    bool uploadsEnabled() const { return current().second != nullptr; }
    // GET /api/v1/media/capabilities: uploadsEnabled() for a member of a space, read after one membership query. Ok or SpaceNotFound.
    // Never touches the store, creates a grant or writes anything. A snapshot: createUpload and complete still enforce it.
    struct Capabilities {
        Outcome outcome = Outcome::Error;
        std::string message;
        bool uploadsEnabled = false;
    };
    void capabilities(const std::string &userId, std::function<void(Capabilities)> cb);

    void createUpload(const std::string &userId, const std::string &key, const std::string &contentType, int64_t size, Callback cb);
    void complete(const std::string &userId, const std::string &mediaId, Callback cb, Fault fault = Fault::None);
    void facts(const std::string &userId, const std::string &mediaId, Callback cb);
    void content(const std::string &userId, const std::string &mediaId, const std::string &variant, Callback cb);
    // Created, or Replayed with the original cycle's facts. Absent, another's or not READY: NotFound.
    void deletePhoto(const std::string &userId, const std::string &mediaId, const std::string &key, Callback cb);
    // J2b1 integration seam. Null preserves the existing path. No production
    // provider is wired in main; tests inject a fake. Snapshot per request.
    void configureDeletionJournal(std::shared_ptr<deletionjournal::Journal> journal);
    // J2c2. With a journal configured, every cleanup pass, late removal and resumed attempt's removal first has a durable
    // intent (MediaService.cc, "J2c2"). These counts are this process's memory, for tests and logs: not an audit record.
    struct CleanupJournalReport {
        long long confirmed = 0, unknown = 0, refused = 0, exhausted = 0, conflicts = 0, acknowledged = 0, backedOff = 0;
    };
    static CleanupJournalReport cleanupJournalReport();
    static bool cleanupBusy();  // a store-side cleanup round is running
    // Tests only: a stage ("pass_record", "late_record") answered true fails that step. Production never sets it.
    static void setCleanupFaultForTest(std::function<bool(const std::string &stage)> fault);
    // Ok, or Replayed. Only the original deleter of this exact cycle, strictly before its deadline, with no purge plan.
    void undoDeletion(const std::string &userId, const std::string &mediaId, const std::string &deletionId, const std::string &key, Callback cb,
                      Fault fault = Fault::None);
    // The caller's own cycles that can be undone now, newest first. A snapshot: a listed cycle can still close before an undo arrives.
    // The position is the deleted_at and id of the last row of the previous page, as AlbumService::decodeCursor returns them.
    void listUndoable(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                      std::function<void(DeletionPage)> cb);
    // D6 M1c. The caller's own undeleted photos whose parent memory is deleted, newest upload first. Items carry no parent. The position is the
    // uploaded_at and id of the last row of the previous page, as AlbumService::decodeCursor returns them.
    void listRetained(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                      std::function<void(DeletionPage)> cb);
    // The caller's own READY objects in the caller's couple that may take their first home now (MediaEligibility.h), newest upload first.
    // A snapshot only: listing reserves nothing, renews no grant and moves no cleanup deadline; the attachment's own checks under the photo
    // locks still decide. Never a key, grant, signed URL, file name or fingerprint.
    void listUnattached(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                        std::function<void(DeletionPage)> cb);

    // The media.cleanup job's two halves. runTick writes plans and chooses work on the scheduler's transaction, with no
    // network. The returned action makes store calls after commit, and uses neither that transaction nor its scope.
    struct TickReport {
        bool ok = false;
        std::string detail;
        std::function<void()> afterCommit;
    };
    void runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done);

  private:
    MediaService() = default;
    std::pair<Policy, std::shared_ptr<media::ObjectStore>> current() const;

    mutable std::mutex mutex_;
    Policy policy_;
    std::shared_ptr<media::ObjectStore> store_;
    std::shared_ptr<deletionjournal::Journal> deletionJournal_;
};

}  // namespace usapp
