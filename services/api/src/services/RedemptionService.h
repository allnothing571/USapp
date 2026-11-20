// =====================================================================
// services/api/src/services/RedemptionService.h
//
// M8: direct redemption of reward products (docs/phase1-m8-redemption.md).
//
// Redeem (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members, joined to the product;
//   2. per-product advisory lock (33007, hashtext(product)), shared with M7 edit/archive;
//   3. key classification on (redeemer, key): same fingerprint replays (also after
//      later edits, archive or fulfillment); otherwise IDEMPOTENCY_KEY_REUSED;
//   4. owner only, active, direct mode, expected_version equals the current version;
//   5. REWARD_REDEMPTION operation claim; flower account create + FOR UPDATE;
//   6. latest entry read after the lock; balance < price -> INSUFFICIENT_BALANCE;
//   7. redemption snapshot insert (ON CONFLICT DO NOTHING, re-classified when skipped);
//   8. one debit entry (-price), audit, read back, commit.
// Fulfill: membership -> advisory (33008, hashtext(redemption)) -> key classification
//   -> creator only, not yet fulfilled -> insert -> audit -> commit. No wallet access.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown.
// Responses separate the immutable facts ("mutation") from the current redemption
// and never include balances. M9 adds request snapshots without a debit and
// append-only transitions; confirmation and refund take the redemption lock,
// then the redeemer's account lock. See docs/phase1-m9-redemption.md.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"

namespace usapp {

class RedemptionService {
  public:
    static RedemptionService &instance();

    // D2 flower balance cap (0006 CHECK); a debit can never exceed it, kept for symmetry.
    static constexpr long long kMaxBalanceFlowers = 1000000;

    enum class Outcome {
        Redeemed,           // 201
        Requested,          // 201, no reservation or debit
        Changed,            // 200, immutable M9 transition
        Fulfilled,          // 200
        Replayed,           // 200: same key and fingerprint
        Ok,                 // 200: read
        Validation,         // 400
        SpaceNotFound,      // 404 SPACE_NOT_FOUND
        ProductNotFound,    // 404 PRODUCT_NOT_FOUND
        ProductArchived,    // 409 PRODUCT_ARCHIVED
        VersionConflict,    // 409 PRODUCT_VERSION_CONFLICT: displayed terms are stale
        ModeUnsupported,    // legacy error; supported modes are direct and request
        Forbidden,          // 403 REDEMPTION_FORBIDDEN: creator redeeming / non-creator fulfilling
        InsufficientBalance,  // 409 INSUFFICIENT_BALANCE: nothing reserved or written
        NotFound,           // 404 REDEMPTION_NOT_FOUND
        AlreadyFulfilled,   // 409 REDEMPTION_ALREADY_FULFILLED
        InvalidState,       // 409 REDEMPTION_STATE_CONFLICT
        BalanceLimit,       // 409 WALLET_BALANCE_LIMIT, refund rejected whole
        KeyReused,          // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,      // 503 RESULT_UNKNOWN
        Error,              // 500
    };

    struct RedemptionData {
        std::string id;
        std::string productId;
        int productVersion = 0;
        std::string creatorId;
        std::string redeemerId;
        std::string title;
        std::string description;
        int priceFlowers = 0;
        std::string redemptionMode;
        std::string operationId;
        std::string status;  // pending_confirmation | pending_fulfillment | fulfilled | cancelled
        std::string createdAt;
        std::string fulfilledAt;  // empty while pending
        std::string fulfilledBy;  // empty while pending
        std::string cancelReason;
        std::string cancelledAt;
        std::string cancellationId;
        std::string cancellationBy;
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // user-facing (Chinese), never echoes input
        std::string detail;   // log only: fixed labels
        Json::Value mutation;  // immutable facts of the redeem or fulfil
        RedemptionData redemption;  // current state at read time
        bool success() const {
            return outcome == Outcome::Redeemed || outcome == Outcome::Requested || outcome == Outcome::Changed || outcome == Outcome::Fulfilled || outcome == Outcome::Replayed ||
                   outcome == Outcome::Ok;
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
        std::vector<RedemptionData> redemptions;
        std::optional<std::string> nextCursor;
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // ---------------------------------------------------------------- pure
    static bool parseRedeemRequest(const Json::Value &body, int &expectedVersion, std::string &message);
    static std::string redeemFingerprint(const std::string &productId, int expectedVersion);
    static std::string fulfillFingerprint(const std::string &redemptionId);
    static bool validTransition(const std::string &action);
    static std::string transitionFingerprint(const std::string &id, const std::string &action, const std::string &proposalId);
    static std::string encodeCursor(const Cursor &cursor);
    static std::optional<Cursor> decodeCursor(const std::string &raw);
    static Json::Value redemptionJson(const RedemptionData &redemption);
    static Json::Value responseBody(const Status &status);  // {"mutation", "redemption"}
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    void redeem(const std::string &userId, const std::string &productId, const std::string &key, int expectedVersion,
                Callback cb);
    void fulfill(const std::string &userId, const std::string &redemptionId, const std::string &key, Callback cb);
    void transition(const std::string &userId, const std::string &redemptionId, const std::string &key,
                    const std::string &action, const std::string &proposalId, Callback cb);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);
    void get(const std::string &userId, const std::string &redemptionId, Callback cb);

  private:
    RedemptionService() = default;
};

}  // namespace usapp
