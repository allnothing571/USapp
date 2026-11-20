#pragma once

// Phase 4 B1: the global avatar catalog and the starter appearance (docs/phase4-avatar-shop-design.md, 0044_avatar_catalog).
//
// User reads: the listed catalog and the caller's appearance. In B1 an appearance is always the configured starter defaults:
// there is no ownership, purchase or equip yet, and the response says so instead of inventing them.
// Admin reads and writes are reached only through AdminFilter. Each write runs one 0044 function in one transaction: it
// classifies the idempotency key first, then checks, writes and audits; the commit makes all of it visible at once.

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

namespace usapp {

class AvatarService {
  public:
    enum class Outcome {
        Ok,               // 200: a read
        Created,          // 201: a new item, revision or starter version
        Replayed,         // 200: same admin, key and payload
        Validation,       // 400 VALIDATION_ERROR
        NotFound,         // 404 AVATAR_ITEM_NOT_FOUND
        VersionConflict,  // 409 AVATAR_VERSION_CONFLICT
        AssetInvalid,     // 400 AVATAR_ASSET_INVALID
        StarterInvalid,   // 400 AVATAR_STARTER_INVALID
        KeyReused,        // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,    // 503 RESULT_UNKNOWN
        Error,            // 500
    };
    // Test only; production never passes it. Stops a write after its function returned, before the commit.
    enum class Fault { None, FailBeforeCommit };

    struct Result {
        Outcome outcome = Outcome::Error;
        std::string message;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;

    struct ItemInput {
        std::string category;
        bool starter = false;
        std::string name;
        std::string assetKey;
        std::optional<int> price;  // empty exactly for a starter
        bool listed = false;
    };
    struct RevisionInput {
        int expectedVersion = 0;
        std::string name;
        std::string assetKey;
        std::optional<int> price;
        bool listed = false;
    };
    struct StarterInput {
        int expectedVersion = 0;  // 0 when no configuration exists yet
        std::string hair;
        std::string clothes;
        std::string expression;
    };

    static AvatarService &instance();

    // Strict request parsers: exactly the documented fields, known categories, a registry-shaped asset key (never a URL or
    // path), names of 1-20 code points without control characters or outer spaces, and prices 1-9999 for paid items only.
    static bool parseCreate(const Json::Value &body, ItemInput &out, std::string &message);
    static bool parseRevision(const Json::Value &body, RevisionInput &out, std::string &message);
    static bool parseStarter(const Json::Value &body, StarterInput &out, std::string &message);
    static bool validAssetKey(const std::string &key);
    static bool validName(const std::string &name);
    // Domain-separated SHA-256 over the canonical request, so a changed payload under the same key is key reuse.
    static std::string fingerprint(const ItemInput &in);
    static std::string fingerprint(const std::string &itemId, const RevisionInput &in);
    static std::string fingerprint(const StarterInput &in);

    void catalog(Callback cb);
    void appearance(const std::string &userId, Callback cb);

    void adminAssets(Callback cb);
    void adminItems(Callback cb);
    void adminItem(const std::string &itemId, Callback cb);
    void adminStarter(Callback cb);

    void createItem(const std::string &adminId, const std::string &key, const ItemInput &in, Callback cb, Fault fault = Fault::None);
    void reviseItem(const std::string &adminId, const std::string &key, const std::string &itemId, const RevisionInput &in, Callback cb,
                    Fault fault = Fault::None);
    void setStarter(const std::string &adminId, const std::string &key, const StarterInput &in, Callback cb, Fault fault = Fault::None);

  private:
    AvatarService() = default;
};

}  // namespace usapp
