// =====================================================================
// services/api/src/services/AdminWalletService.cc
// =====================================================================
#include "services/AdminWalletService.h"

#include <drogon/drogon.h>

#include <memory>

#include "db/Db.h"

namespace usapp {

namespace {

#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// Existence, membership and both balances in one statement (one snapshot). Balance = the newest entry's balance_after by the
// per-user/per-asset seq; no entry = 0. wallet_accounts has no balance column; it only says whether an account row exists.
const char *kSummarySql =
    "SELECT u.id::text AS user_id, u.status, m.couple_id::text AS couple_id, m.slot, "
    "(SELECT json_agg(json_build_object("
    "  'asset', a.asset, "
    "  'account_exists', EXISTS (SELECT 1 FROM wallet_accounts w WHERE w.user_id = u.id AND w.asset_type = a.asset), "
    "  'balance', COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e WHERE e.user_id = u.id AND e.asset_type = a.asset "
    "                       ORDER BY e.seq DESC LIMIT 1), 0)::text, "
    "  'latest_seq', COALESCE((SELECT max(e.seq) FROM wallet_ledger_entries e WHERE e.user_id = u.id AND e.asset_type = a.asset), 0)::text"
    ") ORDER BY a.ord) FROM (VALUES ('flower', 1), ('sunshine', 2)) a(asset, ord))::text AS balances, "
    USAPP_TS("statement_timestamp()") " AS read_at "
    "FROM users u LEFT JOIN couple_members m ON m.user_id = u.id WHERE u.id = $1::uuid";

// One page, newest first by seq, filtered by the selected user AND asset everywhere; limit + 1 rows to know whether more exist.
// Existence, account flag, balance, latest seq and the page share this statement's snapshot.
const char *kLedgerSql =
    "WITH page AS ("
    "  SELECT e.seq, e.amount, e.balance_after, e.created_at, e.operation_id, o.operation_type, o.source_type, o.source_id "
    "    FROM wallet_ledger_entries e JOIN wallet_operations o ON o.id = e.operation_id "
    "   WHERE e.user_id = $1::uuid AND e.asset_type = $2 AND ($3::bigint = 0 OR e.seq < $3::bigint) "
    "   ORDER BY e.seq DESC LIMIT $4::integer + 1) "
    "SELECT EXISTS (SELECT 1 FROM users WHERE id = $1::uuid) AS user_exists, "
    "EXISTS (SELECT 1 FROM wallet_accounts w WHERE w.user_id = $1::uuid AND w.asset_type = $2) AS account_exists, "
    "COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e WHERE e.user_id = $1::uuid AND e.asset_type = $2 "
    "          ORDER BY e.seq DESC LIMIT 1), 0)::text AS balance, "
    "COALESCE((SELECT max(e.seq) FROM wallet_ledger_entries e WHERE e.user_id = $1::uuid AND e.asset_type = $2), 0)::text AS latest_seq, "
    "(SELECT COALESCE(json_agg(json_build_object('seq', p.seq::text, 'amount', p.amount::text, 'balance_after', p.balance_after::text, "
    "   'recorded_at', " USAPP_TS("p.created_at") ", 'operation', json_build_object('id', p.operation_id, 'type', p.operation_type, "
    "   'source_type', p.source_type, 'source_id', p.source_id)) ORDER BY p.seq DESC), '[]'::json) FROM page p)::text AS items, "
    USAPP_TS("statement_timestamp()") " AS read_at";

bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

bool positiveDecimal(const std::string &digits, std::int64_t &out) {
    if (digits.empty() || digits.size() > 19 || digits[0] == '0') return false;
    std::uint64_t value = 0;
    for (char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

}  // namespace

bool AdminWalletService::validAsset(const std::string &asset) { return asset == "flower" || asset == "sunshine"; }

std::string AdminWalletService::encodeCursor(const std::string &userId, const std::string &asset, std::int64_t seq) {
    return "w1." + asset + "." + std::to_string(seq) + "." + userId;
}

bool AdminWalletService::decodeCursor(const std::string &raw, const std::string &userId, const std::string &asset, std::int64_t &seq) {
    if (raw.size() > 128) return false;
    const std::string prefix = "w1." + asset + ".";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    if (raw.substr(dot + 1) != userId) return false;
    return positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), seq);
}

void AdminWalletService::summary(const std::string &userId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kSummarySql,
        [done](const drogon::orm::Result &rows) {
            Result r;
            if (rows.empty()) { r.outcome = Outcome::NotFound; (*done)(std::move(r)); return; }
            const auto &row = rows[0];
            Json::Value balances;
            if (!parseJson(row["balances"].as<std::string>(), balances) || !balances.isArray()) { (*done)(Result{}); return; }
            r.body["user"]["id"] = row["user_id"].as<std::string>();
            r.body["user"]["status"] = row["status"].as<std::string>();
            if (row["couple_id"].isNull()) {
                r.body["space"] = Json::nullValue;
            } else {
                r.body["space"]["couple_id"] = row["couple_id"].as<std::string>();
                r.body["space"]["slot"] = row["slot"].as<int>();
            }
            r.body["balances"] = balances;
            r.body["read_at"] = row["read_at"].as<std::string>();
            r.outcome = Outcome::Ok;
            (*done)(std::move(r));
        },
        [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin wallet summary failed", e.base());
            (*done)(Result{});
        },
        userId);
}

void AdminWalletService::ledger(const std::string &userId, const std::string &asset, int limit, std::int64_t cursorSeq, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kLedgerSql,
        [done, userId, asset, limit](const drogon::orm::Result &rows) {
            Result r;
            if (rows.size() != 1u) { (*done)(Result{}); return; }
            const auto &row = rows[0];
            if (!row["user_exists"].as<bool>()) { r.outcome = Outcome::NotFound; (*done)(std::move(r)); return; }
            Json::Value fetched;
            if (!parseJson(row["items"].as<std::string>(), fetched) || !fetched.isArray()) { (*done)(Result{}); return; }
            Json::Value items(Json::arrayValue);
            for (Json::ArrayIndex i = 0; i < fetched.size() && static_cast<int>(i) < limit; ++i) items.append(fetched[i]);
            r.body["user_id"] = userId;
            r.body["asset"] = asset;
            r.body["account_exists"] = row["account_exists"].as<bool>();
            r.body["balance"] = row["balance"].as<std::string>();
            r.body["latest_seq"] = row["latest_seq"].as<std::string>();
            r.body["read_at"] = row["read_at"].as<std::string>();
            r.body["items"] = items;
            if (static_cast<int>(fetched.size()) > limit && !items.empty()) {
                std::int64_t last = 0;
                if (!positiveDecimal(items[items.size() - 1]["seq"].asString(), last)) { (*done)(Result{}); return; }
                r.body["next_cursor"] = encodeCursor(userId, asset, last);
            } else {
                r.body["next_cursor"] = Json::nullValue;
            }
            r.outcome = Outcome::Ok;
            (*done)(std::move(r));
        },
        [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin wallet ledger failed", e.base());
            (*done)(Result{});
        },
        userId, asset, static_cast<long long>(cursorSeq), limit);
}

}  // namespace usapp
