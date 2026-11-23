#pragma once

// Phase 4 B3: saving the caller's whole avatar loadout (docs/phase4-avatar-shop-design.md Section 7, 0046_avatar_loadouts).
// One call of usapp_avatar_loadout_save in one transaction: per-user lock, key classification first, membership, version,
// fallback from one starter configuration when a required slot is null, eligibility, the event and its audit row. No wallet,
// purchase or catalog write; buying never equips and equipping never buys.

#include <json/json.h>

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace usapp {

class AvatarLoadoutService {
  public:
    enum class Outcome {
        Created,             // 201: a new saved loadout
        Replayed,            // 200: same user, key and request
        Validation,          // 400 VALIDATION_ERROR
        SpaceNotFound,       // 404 SPACE_NOT_FOUND
        NotFound,            // 404 AVATAR_ITEM_NOT_FOUND
        SlotInvalid,         // 400 AVATAR_SLOT_INVALID (wrong category)
        NotOwned,            // 409 AVATAR_NOT_OWNED (a paid item the caller did not buy)
        VersionConflict,     // 409 AVATAR_VERSION_CONFLICT
        DefaultsUnavailable, // 409 AVATAR_DEFAULTS_UNAVAILABLE (a required slot is null and no starter configuration exists)
        KeyReused,           // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,       // 503 RESULT_UNKNOWN
        Error,               // 500
    };
    // Test only; production never passes it. Stops after the function saved a new loadout, before the commit.
    enum class Fault { None, FailBeforeCommit };

    struct Result {
        Outcome outcome = Outcome::Error;
        std::string message;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;

    static constexpr std::array<const char *, 6> kSlots = {"hair", "clothes", "expression", "hat", "handheld", "accessory"};

    struct Request {
        int expectedVersion = 0;
        std::array<std::optional<std::string>, 6> slots;  // lowercase uuid or empty, in kSlots order
        std::string characterGender; // Empty preserves a previously saved choice.
        std::optional<bool> hairHidden;
    };

    static AvatarLoadoutService &instance();

    // {expected_version, slots:{the six keys}, character_gender?}; each slot a uuid string or null.
    static bool parse(const Json::Value &body, Request &out, std::string &message);
    // Server-side SHA-256 of the canonical requested terms (never of a resolved default).
    static std::string fingerprint(const Request &request);
    static std::string requestedJson(const Request &request);

    void save(const std::string &userId, const std::string &key, const Request &request, Callback cb, Fault fault = Fault::None);

  private:
    AvatarLoadoutService() = default;
};

}  // namespace usapp
