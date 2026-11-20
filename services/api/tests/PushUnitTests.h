#pragma once

// P2-5a: pure PushService validation, fingerprint, marker, gate and response shape.
#include <atomic>
#include <string>

#include "TestSupport.h"
#include "core/Crypto.h"
#include "services/PushService.h"

namespace usapptest::pushunit {

using PS = usapp::PushService;

// base64url without padding (test-only encoder for fixture key material).
inline std::string b64url(const std::string &bytes) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned int buffer = 0;
    int bits = 0;
    for (unsigned char c : bytes) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 0x3Fu]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(buffer << (6 - bits)) & 0x3Fu]);
    return out;
}

inline std::string randomBytes(size_t n) {
    const std::string hex = usapp::crypto::randomHex(n);
    std::string out;
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<char>(std::stoi(hex.substr(i * 2, 2), nullptr, 16)));
    return out;
}

// Published test points only, so a fixture is a real point on P-256: RFC 8291 Section 5's user agent
// and application server keys, and RFC 8292 Section 2.4's example key. No key pair is generated.
inline const char *const kPublishedPoints[3] = {
    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4",
    "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8",
    "BA1Hxzyi1RUM1b5wjxsn7nGxAszw2u61m164i3MrAIxHF6YK5h4SDYic-dRuU_RCPCfA5aq9ojSwk5Y2EmClBPs",
};
inline std::string p256dh() {
    static std::atomic<unsigned> next{0};
    return kPublishedPoints[next.fetch_add(1) % 3u];
}
// For a re-key: a valid point that is certainly a different one.
inline std::string p256dhOtherThan(const std::string &current) {
    return current == kPublishedPoints[0] ? kPublishedPoints[1] : kPublishedPoints[0];
}
inline std::string authSecret() { return b64url(randomBytes(16)); }

inline Json::Value body(const std::string &endpoint, const std::string &p, const std::string &a) {
    Json::Value b(Json::objectValue);
    b["endpoint"] = endpoint;
    b["keys"]["p256dh"] = p;
    b["keys"]["auth"] = a;
    return b;
}

inline void runPushUnitTests() {
    TEST_CASE("P2-5a: endpoint allowlist is exact or label-boundary; https, no port, userinfo or fragment") {
        for (const char *ok : {"https://fcm.googleapis.com/fcm/send/abc:def",
                               "https://updates.push.services.mozilla.com/wpush/v2/gAAAA",
                               "https://web.push.apple.com/QGx0", "https://a.b.push.apple.com/x",
                               "https://db5p.notify.windows.com/w/?token=AQE%3d"}) {
            CHECK(PS::validEndpoint(ok));
        }
        for (const char *bad : {"http://fcm.googleapis.com/fcm/send/x", "https://fcm.googleapis.com",
                                "https://fcm.googleapis.com:443/x", "https://user@fcm.googleapis.com/x",
                                "https://FCM.googleapis.com/x", "https://fcm.googleapis.com./x",
                                "https://evil.fcm.googleapis.com/x", "https://xpush.apple.com/x",
                                "https://push.apple.com/x", "https://evilpush.apple.com.attacker.net/x",
                                "https://web.push.apple.com.attacker.net/x", "https://-a.push.apple.com/x",
                                "https://a..push.apple.com/x", "https://notify.windows.com/x",
                                "https://fcm.googleapis.com/x#frag", "https://fcm.googleapis.com/a b",
                                "https://fcm.googleapis.com/a\\b", "https://10.0.0.1/x", ""}) {
            CHECK_FALSE(PS::validEndpoint(bad));
        }
        CHECK(PS::validEndpoint("https://fcm.googleapis.com/" + std::string(2048 - 27, 'a')));
        CHECK_FALSE(PS::validEndpoint("https://fcm.googleapis.com/" + std::string(2048 - 26, 'a')));
    }

    TEST_CASE("P2-5a: strict base64url key material and exact request shape") {
        for (const char *point : kPublishedPoints) CHECK(PS::validPublicKey(point));
        // The right shape, and not on the curve: the zero point, and coordinates that do not satisfy it.
        CHECK_FALSE(PS::validPublicKey(b64url(std::string(1, '\x04') + std::string(64, '\0'))));
        CHECK_FALSE(PS::validPublicKey(b64url(std::string(1, '\x04') + std::string(64, '\x01'))));
        CHECK_FALSE(PS::validPublicKey(b64url(std::string(1, '\x04') + std::string(64, '\xff'))));
        // One flipped coordinate byte of a real point.
        {
            auto raw = *PS::base64UrlDecode(kPublishedPoints[0]);
            raw[64] = static_cast<char>(raw[64] ^ 0x01);
            CHECK_FALSE(PS::validPublicKey(b64url(raw)));
        }
        CHECK_FALSE(PS::validPublicKey(b64url(std::string(1, '\x02') + randomBytes(64))));  // compressed form
        CHECK_FALSE(PS::validPublicKey(b64url(std::string(1, '\x04') + randomBytes(63))));
        CHECK_FALSE(PS::validPublicKey(p256dh() + "="));
        CHECK(PS::base64UrlDecode("AQ").has_value());
        CHECK_FALSE(PS::base64UrlDecode("AR").has_value());  // non-zero trailing bits
        CHECK_FALSE(PS::base64UrlDecode("A+").has_value());
        CHECK_FALSE(PS::base64UrlDecode("A").has_value());

        const std::string endpoint = "https://updates.push.services.mozilla.com/wpush/v2/unit";
        PS::Registration reg;
        std::string message;
        CHECK(PS::parseRegistration(body(endpoint, p256dh(), authSecret()), reg, message));
        CHECK_EQ(reg.endpoint, endpoint);
        CHECK_FALSE(PS::parseRegistration(body(endpoint, b64url(std::string(1, '\x04') + std::string(64, '\x01')), authSecret()), reg, message));
        auto extra = body(endpoint, p256dh(), authSecret());
        extra["expirationTime"] = Json::Value(Json::nullValue);
        CHECK_FALSE(PS::parseRegistration(extra, reg, message));
        auto extraKey = body(endpoint, p256dh(), authSecret());
        extraKey["keys"]["x"] = "y";
        CHECK_FALSE(PS::parseRegistration(extraKey, reg, message));
        CHECK_FALSE(PS::parseRegistration(body("https://example.com/push", p256dh(), authSecret()), reg, message));
        CHECK_FALSE(PS::parseRegistration(body(endpoint, p256dh(), b64url(randomBytes(15))), reg, message));
        CHECK_FALSE(PS::parseRegistration(body(endpoint, authSecret(), authSecret()), reg, message));
        // The fixed message never echoes the endpoint.
        CHECK(message.find("mozilla") == std::string::npos);
    }

    TEST_CASE("P2-5a: fingerprint covers keys, key id and session; marker is opaque and per generation") {
        const PS::Registration reg{"https://fcm.googleapis.com/fcm/send/u", p256dh(), authSecret()};
        const std::string hash = PS::endpointHash(reg.endpoint);
        CHECK_EQ(hash, usapp::crypto::sha256Hex(reg.endpoint));
        const std::string vapid = usapp::crypto::sha256Hex("vapid");
        const std::string base = PS::fingerprint(hash, reg, vapid, "s1");
        CHECK_EQ(base, PS::fingerprint(hash, reg, vapid, "s1"));
        CHECK(base != PS::fingerprint(hash, reg, vapid, "s2"));
        CHECK(base != PS::fingerprint(hash, reg, usapp::crypto::sha256Hex("other"), "s1"));
        PS::Registration changed = reg;
        changed.auth = authSecret();
        CHECK(base != PS::fingerprint(hash, changed, vapid, "s1"));

        const std::string m1 = PS::markerFor("secret-one-with-enough-length-000", "g1");
        CHECK_EQ(m1.size(), 22u);
        CHECK_EQ(m1, PS::markerFor("secret-one-with-enough-length-000", "g1"));
        CHECK(m1 != PS::markerFor("secret-one-with-enough-length-000", "g2"));
        CHECK(m1 != PS::markerFor("secret-two-with-enough-length-000", "g1"));
        CHECK(m1.find("g1") == std::string::npos);
    }

    TEST_CASE("P2-5a: subscription gate stays closed without transport; lists omit endpoint facts") {
        const std::string key = p256dh();
        auto cfg = PS::configFrom(true, key, false);
        CHECK_FALSE(PS::configFrom(true, b64url(std::string(1, '\x04') + std::string(64, '\x01')), true).subscribeAvailable);
        CHECK_FALSE(cfg.subscribeAvailable);
        CHECK_EQ(cfg.vapidKeyId, usapp::crypto::sha256Hex(key));
        CHECK_FALSE(PS::configFrom(false, key, true).subscribeAvailable);
        cfg = PS::configFrom(true, "not-a-key", true);
        CHECK_FALSE(cfg.subscribeAvailable);
        CHECK(cfg.vapidKeyId.empty());
        CHECK(PS::configFrom(true, key, true).subscribeAvailable);  // test-injected transport only

        PS::Generation g;
        g.id = "g";
        g.state = "active";
        g.endpointHash = std::string(64, 'a');
        g.vapidKeyId = std::string(64, 'b');
        g.provider = "fcm";
        const auto listed = PS::generationJson(g, false);
        CHECK_FALSE(listed.isMember("endpoint_hash"));
        CHECK_FALSE(listed.isMember("endpoint"));
        CHECK(listed["revoked_reason"].isNull());
        CHECK(listed["user_agent_family"].isNull());
        CHECK(PS::generationJson(g, true).isMember("endpoint_hash"));

        CHECK(PS::errorCodeFor(PS::Outcome::DeviceLimit) == usapp::ErrorCode::PushDeviceLimit);
        CHECK(PS::errorCodeFor(PS::Outcome::Unavailable) == usapp::ErrorCode::PushUnavailable);
        CHECK(PS::errorCodeFor(PS::Outcome::NotFound) == usapp::ErrorCode::PushSubscriptionNotFound);
        CHECK(PS::errorCodeFor(PS::Outcome::SessionInvalid) == usapp::ErrorCode::Unauthenticated);
        CHECK_EQ(PS::httpStatusFor(PS::Outcome::DeviceLimit), 409);
        CHECK_EQ(PS::httpStatusFor(PS::Outcome::NotFound), 404);
        CHECK_EQ(PS::httpStatusFor(PS::Outcome::Created), 201);
        CHECK_EQ(PS::httpStatusFor(PS::Outcome::Replayed), 200);
        CHECK_EQ(PS::httpStatusFor(PS::Outcome::ResultUnknown), 503);
        CHECK_EQ(std::string(usapp::errorCodeToString(usapp::ErrorCode::PushDeviceLimit)),
                 std::string("PUSH_DEVICE_LIMIT"));
    }
}

}  // namespace usapptest::pushunit
