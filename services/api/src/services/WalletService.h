#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <json/json.h>

namespace usapp {

struct WalletBalances {
    std::int64_t flower = 0;
    std::int64_t sunshine = 0;
};

// M2-B reads only. Membership and both latest ledger balances share one snapshot.
class WalletService {
  public:
    enum class Outcome { Ok, SpaceNotFound, Error };
    using Callback = std::function<void(Outcome, WalletBalances)>;

    static WalletService &instance();
    static const char *balanceSql();
    static Json::Value responseBody(const WalletBalances &balances);
    void getOwnWallet(const std::string &userId, Callback callback);

  private:
    WalletService() = default;
};

}  // namespace usapp
