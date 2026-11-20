// =====================================================================
// services/api/src/services/AdminWalletService.h
// Admin-only, read-only inspection of one selected user's flower/sunshine balances and ledger history
// (docs/admin-wallet-ledger.md). Each answer is one SQL statement, so one snapshot; the summary and each ledger page are
// separate reads. Numbers are canonical decimal strings (bigint::text). Writes nothing. Caller-only WalletService is untouched.
// =====================================================================
#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>

namespace usapp {

class AdminWalletService {
  public:
    enum class Outcome { Ok, NotFound, Error };
    struct Result {
        Outcome outcome = Outcome::Error;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;

    static bool validAsset(const std::string &asset);
    // w1.<asset>.<seq>.<userId>; seq a positive integer of at most 19 digits that fits a signed 64-bit value.
    static std::string encodeCursor(const std::string &userId, const std::string &asset, std::int64_t seq);
    // Valid only for this user and this asset. Returns false for anything malformed or mismatched.
    static bool decodeCursor(const std::string &raw, const std::string &userId, const std::string &asset, std::int64_t &seq);

    static void summary(const std::string &userId, Callback cb);
    // cursorSeq 0: from the newest entry.
    static void ledger(const std::string &userId, const std::string &asset, int limit, std::int64_t cursorSeq, Callback cb);
};

}  // namespace usapp
