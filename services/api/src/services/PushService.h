// =====================================================================
// services/api/src/services/PushService.h
//
// Phase 2 P2-5a: Web Push device subscription lifecycle only (docs/phase2-web-push.md
// revision 3, Section 4; migration 0022). No dispatch, transport, key generation or
// outbound request happens here.
//
// - A subscription GENERATION is immutable and never reactivated; activity is derived
//   (no revocation, session authenticated/not revoked/not expired, configured key id).
// - Every accepted registration key is persisted with its ORIGINAL result
//   (`created` or `already_active`) in push_registration_commands; a same-key replay returns
//   those facts plus the generation's CURRENT state and never creates, revokes or restores.
// - Revoke targets one immutable generation id and is naturally idempotent.
// Lock order (same as the 0022 guards): caller session row + member row FOR SHARE ->
// user lock 33013 -> endpoint lock 33014. Device cap: 10 active generations per user;
// a new device at the cap is rejected (PUSH_DEVICE_LIMIT), never by revoking another.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"

namespace usapp {

class PushService {
  public:
    static PushService &instance();

    static constexpr int kMaxActiveDevices = 10;
    static constexpr size_t kMaxEndpointBytes = 2048;
    // List: every slot-occupying generation (complete, at most kMaxActiveDevices) plus at most
    // kHistoryLimit newest other generations (revoked or session ended).
    static constexpr int kHistoryLimit = 20;

    enum class Outcome {
        Created,       // 201 a new generation
        Replayed,      // 200 same key / already revoked
        Ok,            // 200 read, already_active, first revocation
        Validation,    // 400
        SpaceNotFound, // 404
        SessionInvalid, // 401 the caller's session ended during the request
        Unavailable,   // 409 PUSH_UNAVAILABLE (subscription closed)
        DeviceLimit,   // 409 PUSH_DEVICE_LIMIT
        NotFound,      // 404 PUSH_SUBSCRIPTION_NOT_FOUND
        KeyReused,     // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown, // 503
        Error,         // 500
    };

    struct Registration {
        std::string endpoint;
        std::string p256dh;
        std::string auth;
    };

    // Current state of one generation (never the endpoint or keys).
    struct Generation {
        std::string id;
        std::string state;  // active | revoked | inactive
        std::string activatedAt;
        std::string revokedReason;  // empty when not revoked
        std::string revokedAt;
        bool currentSession = false;
        std::string provider;  // fcm | mozilla | apple | windows
        std::string userAgentFamily;
        std::string endpointHash;
        std::string vapidKeyId;
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        bool isRevoke = false;
        Json::Value command;     // registration: immutable {result, generation_id, recorded_at}
        Json::Value revocation;  // revoke: immutable {generation_id, reason, revoked_by, revoked_at}
        Generation generation;   // current state at read time
        std::string marker;      // opaque device marker, only for an active generation
        bool success() const {
            return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok;
        }
    };

    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<Generation> devices;  // complete: every generation occupying a cap slot
        std::vector<Generation> history;  // bounded: newest revoked / session-ended generations
    };

    // Server-side configuration. P2-5a has no transport, so subscription is never available
    // outside tests; the public key is published only when it is a valid P-256 point.
    struct Config {
        bool subscribeAvailable = false;
        std::string vapidPublicKey;  // empty when not configured or invalid
        std::string vapidKeyId;      // sha256 hex of the public key, empty when not configured
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // ---------------------------------------------------------------- pure
    static Config configFrom(bool subscribeEnabled, const std::string &vapidPublicKey, bool transportReady);
    // Body exactly {endpoint, keys: {p256dh, auth}}; endpoint and keys per Section 8.3.
    static bool parseRegistration(const Json::Value &body, Registration &out, std::string &message);
    // https, default port, no userinfo, allowlisted host (exact FCM/Mozilla; label-boundary
    // .push.apple.com / .notify.windows.com), printable-ASCII path, <= 2048 bytes.
    static bool validEndpoint(const std::string &endpoint);
    static std::optional<std::string> base64UrlDecode(const std::string &text);
    // A P-256 VAPID public key: base64url of a 65-byte uncompressed point.
    static bool validPublicKey(const std::string &key);
    static std::string endpointHash(const std::string &endpoint);
    static std::string vapidKeyId(const std::string &publicKey);
    static std::string fingerprint(const std::string &endpointHash, const Registration &registration,
                                   const std::string &vapidKeyId, const std::string &sessionId);
    // Opaque marker: base64url(HMAC-SHA-256(secret, "push-marker:v1:" + generation id)[0..16)).
    static std::string markerFor(const std::string &secret, const std::string &generationId);
    // includeHash: the register/revoke responses carry endpoint_hash for client checks; lists do not.
    static Json::Value generationJson(const Generation &generation, bool includeHash);
    static Json::Value responseBody(const Status &status);
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    // vapidKeyId is the server's configured key id; markerSecret derives the device marker.
    void registerSubscription(const std::string &userId, const std::string &sessionId, const std::string &key,
                              const Registration &registration, const std::string &vapidKeyId,
                              const std::string &markerSecret, Callback cb);
    void revoke(const std::string &userId, const std::string &sessionId, const std::string &generationId,
                const std::string &vapidKeyId, Callback cb);
    void list(const std::string &userId, const std::string &sessionId, const std::string &vapidKeyId,
              ListCallback cb);

  private:
    PushService() = default;
};

}  // namespace usapp
