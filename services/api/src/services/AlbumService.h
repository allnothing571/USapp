// =====================================================================
// services/api/src/services/AlbumService.h
//
// Phase 3 P3-2a: shared albums (docs/phase3-p32-albums-design.md revision 2, migration 0031). An album holds independently
// uploaded photos only: each photo's one home is its album, recorded by a media_bindings row of kind 'album'. Nothing is ever
// rebound. Either member creates an album, adds their own READY photos and chooses a cover. Only a photo's uploader removes it.
//
// Writes (one TxScope, READ COMMITTED), in a fixed lock order. Completion and cleanup hold only 33018, and memory writes take
// 33020 and never 33021, so nothing waits in the other direction:
//   0. ids normalized: lowercase, canonical, duplicates refused, before anything hashes them;
//   1. caller's member row FOR SHARE, joined to the album;
//   2. advisory lock 33021 on the album;
//   3. key classification: the same fingerprint replays the committed write, read-only, before any state check;
//   4. role and state checks;
//   5. add only: 33018 on each photo, in sorted order, then eligibility, read after the waits; a cover takes 33018 on its photo (D6);
//   6. inserts; batch, bindings, items by ascending seq; then audit and read-back; commit.
//
// Removal hides, it does not delete. The binding and the bytes stay until D6, so a removed photo stays out of cleanup and can
// never enter any album again. After its commit the partner cannot obtain a new signed URL; one already issued may stay usable
// until it expires, and nothing here can revoke it.
//
// Order. An add of N photos takes the next N seq values and the first photo chosen gets the highest, so seq DESC shows newer adds
// first and the chosen order inside each. The fallback cover is the highest active seq: the first photo chosen in the latest add.
//
// P3-2b (docs/phase3-p32b-album-rename-design.md, D18 Option A, migration 0032). The creator renames directly with
// expected_version. The partner requests a rename, the creator approves or rejects it, and the partner may withdraw it. One pending
// request per album, and a direct rename is refused while one is pending, so its base stays current: there is no reachable stale
// approval and no public code for one. These take only 33021. Photos and covers do not change the version.
//
// Notifications, deletion, reorder and move are not offered.
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

class AlbumService {
  public:
    static AlbumService &instance();

    static constexpr size_t kMaxNameCodePoints = 40;
    static constexpr size_t kMaxPhotosPerAdd = 20;

    enum class Outcome {
        Created,           // 201
        Replayed,          // 200: same key and fingerprint
        Ok,                // 200: a read or a removal
        Validation,        // 400
        SpaceNotFound,     // 404 SPACE_NOT_FOUND
        NotFound,          // 404 ALBUM_NOT_FOUND (absent or another space)
        PhotoUnavailable,  // 409 ALBUM_PHOTO_UNAVAILABLE
        PhotoNotFound,     // 404 ALBUM_PHOTO_NOT_FOUND
        PhotoForbidden,    // 403 ALBUM_PHOTO_FORBIDDEN
        PhotoRemoved,      // 409 ALBUM_PHOTO_REMOVED
        EditForbidden,     // 403 ALBUM_EDIT_FORBIDDEN
        VersionConflict,   // 409 ALBUM_VERSION_CONFLICT
        ProposalPending,   // 409 ALBUM_PROPOSAL_PENDING
        ProposalNotFound,  // 404 ALBUM_PROPOSAL_NOT_FOUND
        ProposalDecided,   // 409 ALBUM_PROPOSAL_DECIDED
        KeyReused,         // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,     // 503 RESULT_UNKNOWN
        Error,             // 500
    };

    enum class Decision { Approve, Reject, Withdraw };
    enum class Fault { None, FailAfterBinding, FailAfterDecision };  // test only; production never passes it

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;    // fixed labels, for logs
        Json::Value mutation;  // immutable facts of the write or replay
        Json::Value album;     // current state
        Json::Value proposal;  // a proposal read: original, proposed and decision
        bool hasNotification = false;  // D20: an add batch, new or replayed, carries its original notification decision
        Json::Value notification;
        bool success() const { return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok; }
    };
    using Callback = std::function<void(Status)>;

    struct Cursor {
        std::string createdAt;
        std::string id;
    };
    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<Json::Value> items;
        std::optional<std::string> nextCursor;
        long long currentVersion = 0;  // listProposals only: the album's latest name version, read by the same statement
    };
    using ListCallback = std::function<void(ListResult)>;

    // Pure. mediaIds and mediaId come back lowercase, in request order.
    static bool parseCreateRequest(const Json::Value &body, std::string &name, std::string &message);
    static bool parseAddRequest(const Json::Value &body, std::vector<std::string> &mediaIds, std::string &message);
    static bool parseCoverRequest(const Json::Value &body, std::string &mediaId, std::string &message);
    static bool parseRevisionRequest(const Json::Value &body, int &expectedVersion, std::string &name, std::string &message);
    static bool parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message);
    static std::string renameFingerprint(const std::string &albumId, int expectedVersion, const std::string &name);
    static std::string proposeFingerprint(const std::string &albumId, int expectedVersion, const std::string &name);
    static std::string decisionFingerprint(const std::string &albumId, const std::string &proposalId, Decision decision);
    static std::string createFingerprint(const std::string &name);
    static std::string addFingerprint(const std::string &albumId, const std::vector<std::string> &mediaIds);
    static std::string removeFingerprint(const std::string &albumId, const std::string &mediaId);
    static std::string coverFingerprint(const std::string &albumId, const std::string &mediaId);
    static std::string encodeCursor(const Cursor &cursor);
    static std::optional<Cursor> decodeCursor(const std::string &raw);
    static std::optional<int> decodePhotoCursor(const std::string &raw);
    static Json::Value responseBody(const Status &status);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // Actions.
    void create(const std::string &userId, const std::string &key, const std::string &name, Callback cb);
    // D20: `choice` is the optional notify_partner of the whole batch, default off: at most one notice per batch, never one per photo.
    void addPhotos(const std::string &userId, const std::string &albumId, const std::string &key, const std::vector<std::string> &mediaIds, Callback cb,
                   Fault fault = Fault::None, intent::Choice choice = {});
    void removePhoto(const std::string &userId, const std::string &albumId, const std::string &mediaId, const std::string &key, Callback cb);
    void setCover(const std::string &userId, const std::string &albumId, const std::string &mediaId, const std::string &key, Callback cb);
    void rename(const std::string &userId, const std::string &albumId, const std::string &key, int expectedVersion, const std::string &name, Callback cb);
    void propose(const std::string &userId, const std::string &albumId, const std::string &key, int expectedVersion, const std::string &name, Callback cb);
    void decide(const std::string &userId, const std::string &albumId, const std::string &proposalId, const std::string &key, Decision decision, Callback cb,
                Fault fault = Fault::None);
    void getProposal(const std::string &userId, const std::string &albumId, const std::string &proposalId, Callback cb);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);
    void get(const std::string &userId, const std::string &albumId, Callback cb);
    void listPhotos(const std::string &userId, const std::string &albumId, int limit, const std::optional<int> &beforeSeq, ListCallback cb);
    // One album's rename requests, pending and decided, newest first by (created_at, id). Summaries only: no names, keys or fingerprints;
    // getProposal has the original and proposed names. Both members; an absent or other space's album, or a caller in no space, is
    // NotFound, as listPhotos answers. Reads only. The position is the created_at and id of the previous page's last row (decodeCursor).
    void listProposals(const std::string &userId, const std::string &albumId, int limit, const std::optional<Cursor> &after, ListCallback cb);

  private:
    AlbumService() = default;
};

}  // namespace usapp
