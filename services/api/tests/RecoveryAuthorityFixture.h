#pragma once

#include "services/RecoveryAuthority.h"
#include <openssl/evp.h>
#include <stdexcept>

namespace usapptest::recoveryauthority {
namespace a = usapp::deletionrecovery::authority;

// Synthetic signer only. Production verifier receives the public key.
class Signer {
  public:
    Signer() {
        std::array<unsigned char, 32> seed{};
        seed.fill(41);
        key_.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        auto size = publicKey.size();
        if (!key_ || EVP_PKEY_get_raw_public_key(key_.get(), publicKey.data(), &size) != 1 || size != publicKey.size())
            throw std::runtime_error("synthetic authority key");
    }

    a::Signature sign(const a::Attestation &attestation) {
        const auto body = a::signingBytes(attestation);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        a::Signature signedBytes{};
        auto size = signedBytes.size();
        if (!body || !ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key_.get()) != 1 ||
            EVP_DigestSign(ctx.get(), signedBytes.data(), &size,
                           reinterpret_cast<const unsigned char *>(body->data()), body->size()) != 1 ||
            size != signedBytes.size()) throw std::runtime_error("synthetic authority signature");
        return signedBytes;
    }

    a::PublicKey publicKey{};

  private:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
};
} // namespace usapptest::recoveryauthority
