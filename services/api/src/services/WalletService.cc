#include "services/WalletService.h"

#include <drogon/orm/Exception.h>
#include <exception>
#include <memory>
#include <utility>

#include "db/Db.h"

namespace usapp {
namespace {
constexpr const char *kBalanceSql = R"SQL(
SELECT m.couple_id::text AS couple_id,
       COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e
                  WHERE e.user_id = m.user_id AND e.asset_type = 'flower'
                  ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS flower,
       COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e
                  WHERE e.user_id = m.user_id AND e.asset_type = 'sunshine'
                  ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS sunshine
FROM couple_members m WHERE m.user_id = $1::uuid
)SQL";
}  // namespace

WalletService &WalletService::instance() {
    static WalletService service;
    return service;
}

const char *WalletService::balanceSql() { return kBalanceSql; }

Json::Value WalletService::responseBody(const WalletBalances &balances) {
    Json::Value body(Json::objectValue);
    body["wallet"]["flower"] = Json::Int64(balances.flower);
    body["wallet"]["sunshine"] = Json::Int64(balances.sunshine);
    return body;
}

void WalletService::getOwnWallet(const std::string &userId, Callback callback) {
    if (userId.empty() || !db::Pool::instance().ready()) {
        callback(Outcome::Error, {});
        return;
    }
    // Share only the completion callback, without a transaction or ownership cycle.
    auto done = std::make_shared<Callback>(std::move(callback));
    db::Pool::instance().client()->execSqlAsync(
        balanceSql(),
        [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0) {
                (*done)(Outcome::SpaceNotFound, {});
                return;
            }
            WalletBalances balances;
            if (rows.size() != 1 || rows[0]["flower"].isNull() || rows[0]["sunshine"].isNull()) {
                (*done)(Outcome::Error, {});
                return;
            }
            try {
                balances.flower = rows[0]["flower"].as<long long>();
                balances.sunshine = rows[0]["sunshine"].as<long long>();
            } catch (const std::exception &) {
                (*done)(Outcome::Error, {});
                return;
            }
            (*done)(Outcome::Ok, balances);
        },
        [done](const drogon::orm::DrogonDbException &) { (*done)(Outcome::Error, {}); },
        userId);
}

}  // namespace usapp
