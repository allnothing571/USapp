#pragma once

// Phase 4 B2: sunshine purchases of paid avatar items and the caller's owned items (docs/phase4-avatar-shop-design.md Section 6,
// 0045_avatar_purchases). A purchase is one call of usapp_avatar_purchase in one transaction: key classification first, then
// the item lock, catalog checks, ownership, the buyer's sunshine account row lock, balance, the operation, snapshot, exact
// debit and audit, with the deferred links checked inside the statement. Buying never equips, and nothing here grants items.

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

namespace usapp {

class AvatarPurchaseService {
  public:
    enum class Outcome {
        Ok,               // 200: a read
        Created,          // 201: a new purchase
        Replayed,         // 200: same user, key and request
        Validation,       // 400 VALIDATION_ERROR
        SpaceNotFound,    // 404 SPACE_NOT_FOUND
        NotFound,         // 404 AVATAR_ITEM_NOT_FOUND
        Unavailable,      // 409 AVATAR_ITEM_UNAVAILABLE (unlisted, or a starter)
        VersionConflict,  // 409 AVATAR_VERSION_CONFLICT
        AlreadyOwned,     // 409 AVATAR_ALREADY_OWNED
        Insufficient,     // 409 INSUFFICIENT_BALANCE
        KeyReused,        // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,    // 503 RESULT_UNKNOWN
        Error,            // 500
    };
    // Test only; production never passes it. Stops after the function returned a new purchase, before the commit.
    enum class Fault { None, FailBeforeCommit };

    struct Result {
        Outcome outcome = Outcome::Error;
        std::string message;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;

    struct Request {
        std::string itemId;  // lowercase uuid
        int expectedVersion = 0;
    };

    static AvatarPurchaseService &instance();

    // Exactly {item_id, expected_version}.
    static bool parse(const Json::Value &body, Request &out, std::string &message);
    // Server-side, domain-separated SHA-256 of the canonical request.
    static std::string fingerprint(const Request &request);

    void purchase(const std::string &userId, const std::string &key, const Request &request, Callback cb, Fault fault = Fault::None);
    // The caller's own paid items, newest purchase first; limit 1-100; cursor as the previous page returned it.
    void owned(const std::string &userId, int limit, const std::string &cursor, Callback cb);

  private:
    AvatarPurchaseService() = default;
};

}  // namespace usapp
