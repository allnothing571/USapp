#include "services/RecoveryAuthority.h"
#include <chrono>
#include <openssl/evp.h>

namespace usapp::deletionrecovery::authority {
namespace {
constexpr int64_t kMaximumLifetimeSeconds = 300;

bool digest(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    });
}

bool matches(const TargetBinding &actual, const TargetBinding &claimed) {
    return actual.request.databaseId == claimed.request.databaseId &&
           actual.request.attemptId == claimed.request.attemptId &&
           actual.request.journalId == claimed.request.journalId &&
           actual.instanceToken == claimed.instanceToken && actual.archiveSha256 == claimed.archiveSha256;
}

bool verifies(const PublicKey &key, const Signature &signature, const std::string &body) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size()), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return publicKey && context && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, publicKey.get()) == 1 &&
        EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                         reinterpret_cast<const unsigned char *>(body.data()), body.size()) == 1;
}

int64_t unixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
} // namespace

std::optional<std::string> signingBytes(const Attestation &attestation) {
    const auto &target = attestation.target;
    const auto &request = target.request;
    if (!deletionjournal::uuidShape(request.databaseId) || !deletionjournal::uuidShape(request.attemptId) ||
        !deletionjournal::uuidShape(request.journalId) || !digest(target.instanceToken) ||
        !digest(target.archiveSha256) || !validHead(attestation.checkpoint) ||
        attestation.checkpoint.journalId != request.journalId || attestation.issuedAt <= 0 ||
        attestation.expiresAt <= attestation.issuedAt ||
        attestation.expiresAt - attestation.issuedAt > kMaximumLifetimeSeconds) return std::nullopt;

    return "usapp-recovery-attestation-v1\n" + request.databaseId + "\n" + request.attemptId + "\n" +
        request.journalId + "\n" + target.instanceToken + "\n" + target.archiveSha256 + "\n" +
        std::to_string(attestation.checkpoint.seq) + "\n" + attestation.checkpoint.hash + "\n" +
        std::to_string(attestation.issuedAt) + "\n" + std::to_string(attestation.expiresAt) + "\n";
}

void prepare(const TargetBinding &actual, const PublicKey &trustedKey, const Attestation &attestation,
             const Signature &signature, std::shared_ptr<Source> source, std::shared_ptr<Store> store,
             std::function<void(Report)> done, Clock clock) {
    using namespace std::chrono;
    const auto body = signingBytes(attestation);
    if (!body || !matches(actual, attestation.target) || !verifies(trustedKey, signature, *body)) {
        done({Outcome::Blocked});
        return;
    }
    if (!clock) clock = unixSeconds;
    int64_t now;
    try { now = clock(); }
    catch (...) { done({Outcome::Blocked}); return; }
    if (now < attestation.issuedAt || now >= attestation.expiresAt) {
        done({Outcome::Blocked});
        return;
    }
    const auto deadline = steady_clock::now() + seconds(attestation.expiresAt - now);
    auto current = [clock = std::move(clock), last = now, deadline, expires = attestation.expiresAt]() mutable {
        try {
            const auto observed = clock();
            if (observed < last || observed >= expires || steady_clock::now() >= deadline) return false;
            last = observed;
            return true;
        } catch (...) { return false; }
    };
    Coordinator::runPinned(actual.request, attestation.checkpoint, std::move(source), std::move(store),
                           std::move(done), std::move(current));
}
} // namespace usapp::deletionrecovery::authority
