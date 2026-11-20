#pragma once

#include "services/DeletionRecovery.h"
#include <array>

namespace usapp::deletionrecovery::authority {
using PublicKey = std::array<unsigned char, 32>;
using Signature = std::array<unsigned char, 64>;
using Clock = std::function<int64_t()>;

struct TargetBinding {
    Request request;
    std::string instanceToken; // Fresh identity allocated outside the restored database.
    std::string archiveSha256;  // Digest of the archive that was actually restored.
};
struct Attestation {
    TargetBinding target;
    Head checkpoint;
    int64_t issuedAt = 0;
    int64_t expiresAt = 0;
};

// Wire bytes for the independent signing process. Invalid claims have no signing representation.
std::optional<std::string> signingBytes(const Attestation &);

// The caller supplies a separately enrolled key and the identity of its actual restore target.
// This consumes evidence for preparation only and never changes serving permission.
void prepare(const TargetBinding &actual, const PublicKey &trustedKey, const Attestation &, const Signature &,
             std::shared_ptr<Source>, std::shared_ptr<Store>, std::function<void(Report)>, Clock = {});
} // namespace usapp::deletionrecovery::authority
