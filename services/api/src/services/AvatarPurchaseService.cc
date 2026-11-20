#include "services/AvatarPurchaseService.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <memory>
#include <regex>
#include <sstream>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AlbumService.h"

namespace usapp {

namespace {

using Outcome = AvatarPurchaseService::Outcome;
using Result = AvatarPurchaseService::Result;

#define AP_TS(x) "to_char(" x " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// The original purchase facts: the snapshot and the debit entry, both immutable.
#define AP_PURCHASE_COLUMNS                                                                                                   \
    "p.id::text AS id, p.item_id::text AS item_id, p.item_version, p.name, p.category, p.asset_key, p.price_sunshine, "    \
    "p.operation_id::text AS operation_id, " AP_TS("p.created_at") " AS created_at, -e.amount AS debited, e.balance_after, " \
    "p.created_at AS sort_at, p.id AS sort_id "
#define AP_PURCHASE_FROM "FROM avatar_purchases p JOIN wallet_ledger_entries e ON e.operation_id = p.operation_id "
// Today's catalog view of an item, read separately from the original facts.
#define AP_CURRENT_ITEM                                                                                                       \
    "(SELECT json_build_object('id', i.id, 'category', i.category, 'starter', i.starter, 'version', r.version, 'name', r.name, " \
    "'price_sunshine', r.price_sunshine, 'listed', r.listed, 'asset', json_build_object('key', r.asset_key, 'format', a.format, " \
    "'layers', (SELECT json_agg(k ORDER BY k) FROM jsonb_object_keys(a.layers) k), 'covers_hair_top', a.covers_hair_top, "    \
    "'accessory_position', a.accessory_position))::text FROM avatar_items i "                                                 \
    "JOIN LATERAL (SELECT * FROM avatar_item_revisions x WHERE x.item_id = i.id ORDER BY x.version DESC LIMIT 1) r ON true "   \
    "JOIN avatar_assets a ON a.asset_key = r.asset_key WHERE i.id = p.item_id) AS current_item "

const char *kPurchaseSql = "SELECT usapp_avatar_purchase($1::uuid, $2, $3, $4::uuid, $5::integer)::text AS r";
const char *kFactsSql =
    "SELECT " AP_PURCHASE_COLUMNS ", " AP_CURRENT_ITEM ", "
    "(SELECT x.balance_after FROM wallet_ledger_entries x WHERE x.user_id = p.user_id AND x.asset_type = 'sunshine' "
    "ORDER BY x.seq DESC LIMIT 1) AS sunshine_now " AP_PURCHASE_FROM "WHERE p.id = $1::uuid";
const char *kMemberSql = "SELECT 1 AS member FROM couple_members WHERE user_id = $1::uuid";
const char *kOwnedFirstSql =
    "SELECT " AP_PURCHASE_COLUMNS ", " AP_CURRENT_ITEM AP_PURCHASE_FROM
    "WHERE p.user_id = $1::uuid ORDER BY p.created_at DESC, p.id DESC LIMIT $2::integer";
const char *kOwnedAfterSql =
    "SELECT " AP_PURCHASE_COLUMNS ", " AP_CURRENT_ITEM AP_PURCHASE_FROM
    "WHERE p.user_id = $1::uuid AND (p.created_at, p.id) < ($3::timestamptz, $4::uuid) "
    "ORDER BY p.created_at DESC, p.id DESC LIMIT $2::integer";

Result fail(Outcome outcome, const std::string &message) {
    Result r;
    r.outcome = outcome;
    r.message = message;
    return r;
}

bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

std::string lower(std::string s) {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

Json::Value purchaseJson(const drogon::orm::Row &row) {
    Json::Value p(Json::objectValue);
    p["id"] = row["id"].as<std::string>();
    p["item_id"] = row["item_id"].as<std::string>();
    p["item_version"] = row["item_version"].as<int>();
    p["name"] = row["name"].as<std::string>();
    p["category"] = row["category"].as<std::string>();
    p["asset_key"] = row["asset_key"].as<std::string>();
    p["price_sunshine"] = row["price_sunshine"].as<int>();
    p["debited_sunshine"] = Json::Int64(row["debited"].as<long long>());
    p["sunshine_balance_after"] = Json::Int64(row["balance_after"].as<long long>());
    p["operation_id"] = row["operation_id"].as<std::string>();
    p["created_at"] = row["created_at"].as<std::string>();
    return p;
}

Json::Value currentItem(const drogon::orm::Row &row) {
    Json::Value item;
    if (row["current_item"].isNull() || !parseJson(row["current_item"].as<std::string>(), item)) throw std::runtime_error("item");
    return item;
}

}  // namespace

AvatarPurchaseService &AvatarPurchaseService::instance() {
    static AvatarPurchaseService service;
    return service;
}

bool AvatarPurchaseService::parse(const Json::Value &body, Request &out, std::string &message) {
    message = "请求必须恰好包含 item_id 与 expected_version";
    if (!body.isObject() || body.size() != 2u || !body.isMember("item_id") || !body.isMember("expected_version")) return false;
    static const std::regex uuid("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    if (!body["item_id"].isString() || !std::regex_match(body["item_id"].asString(), uuid)) {
        message = "item_id 无效";
        return false;
    }
    const auto &v = body["expected_version"];
    if (!v.isInt() || v.isBool() || v.asInt64() < 1 || v.asInt64() > 1000000) {
        message = "expected_version 必须是正整数";
        return false;
    }
    out.itemId = lower(body["item_id"].asString());
    out.expectedVersion = static_cast<int>(v.asInt64());
    return true;
}

std::string AvatarPurchaseService::fingerprint(const Request &request) {
    return crypto::sha256Hex("avatar:purchase:v1:" + request.itemId + ":" + std::to_string(request.expectedVersion));
}

void AvatarPurchaseService::purchase(const std::string &userId, const std::string &key, const Request &request, Callback cb, Fault fault) {
    if (!db::Pool::instance().ready()) {
        cb(fail(Outcome::Error, "数据库未就绪"));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    auto answered = std::make_shared<bool>(false);
    auto finish = [done, answered](Result r) {
        if (*answered) return;
        *answered = true;
        (*done)(std::move(r));
    };
    const auto params = std::make_shared<std::array<std::string, 5>>(
        std::array<std::string, 5>{userId, key, fingerprint(request), request.itemId, std::to_string(request.expectedVersion)});
    db::TxScope::beginAsync(
        [params, fault, finish](const std::shared_ptr<db::TxScope> &tx) {
            tx->tx().execSqlAsync(
                kPurchaseSql,
                [tx, fault, finish](const drogon::orm::Result &rows) {
                    Json::Value answer;
                    if (rows.size() != 1u || !parseJson(rows[0]["r"].as<std::string>(), answer) || !answer.isObject()) {
                        finish(fail(Outcome::Error, "服务内部错误"));
                        tx->fail("avatar purchase: invalid receipt");
                        return;
                    }
                    if (answer.isMember("error")) {
                        // A refusal wrote nothing; rolling back also drops a sunshine account row created on the way.
                        const auto error = answer["error"].asString();
                        Result r;
                        if (error == "key_reused") r = fail(Outcome::KeyReused, "该请求标识已用于另一项操作");
                        else if (error == "no_space") r = fail(Outcome::SpaceNotFound, "尚未加入空间");
                        else if (error == "not_found") r = fail(Outcome::NotFound, "装扮不存在");
                        else if (error == "starter" || error == "unlisted") r = fail(Outcome::Unavailable, "这件装扮现在不能购买");
                        else if (error == "conflict") r = fail(Outcome::VersionConflict, "装扮已更新，请确认最新价格后再购买");
                        else if (error == "already_owned") r = fail(Outcome::AlreadyOwned, "已经拥有这件装扮");
                        else if (error == "insufficient") r = fail(Outcome::Insufficient, "阳光值不足，无法购买");
                        else r = fail(Outcome::Error, "服务内部错误");
                        tx->rollback([finish, r]() { finish(r); });
                        return;
                    }
                    const bool replay = answer["result"].asString() == "replay";
                    if (fault == AvatarPurchaseService::Fault::FailBeforeCommit && !replay) {
                        tx->fail("avatar purchase: test fault before commit");
                        return;
                    }
                    tx->tx().execSqlAsync(
                        kFactsSql,
                        [tx, finish, replay](const drogon::orm::Result &facts) {
                            Result r;
                            try {
                                if (facts.size() != 1u) throw std::runtime_error("facts");
                                r.body["replayed"] = replay;
                                r.body["purchase"] = purchaseJson(facts[0]);
                                Json::Value current(Json::objectValue);
                                current["sunshine_balance"] = Json::Int64(facts[0]["sunshine_now"].as<long long>());
                                current["item"] = currentItem(facts[0]);
                                r.body["current"] = current;
                            } catch (const std::exception &) {
                                finish(fail(Outcome::Error, "服务内部错误"));
                                tx->fail("avatar purchase: facts unreadable");
                                return;
                            }
                            r.outcome = replay ? Outcome::Replayed : Outcome::Created;
                            tx->commit([finish, r]() { finish(r); });
                        },
                        [tx, finish](const drogon::orm::DrogonDbException &) {
                            finish(fail(Outcome::Error, "服务内部错误"));
                            tx->fail("avatar purchase: facts read failed");
                        },
                        answer["purchase_id"].asString());
                },
                // A refused statement (including a deferred link checked inside it) committed nothing: definite.
                [tx, finish](const drogon::orm::DrogonDbException &e) {
                    LOG_ERROR << db::describePgError("avatar purchase failed", e.base());
                    finish(fail(Outcome::Error, "服务内部错误"));
                    tx->fail("avatar purchase failed");
                },
                (*params)[0], (*params)[1], (*params)[2], (*params)[3], (*params)[4]);
        },
        // Otherwise (a failed begin or commit, or the test fault) nothing is known about the commit.
        [finish](const std::string &) { finish(fail(Outcome::ResultUnknown, "结果未确认，请使用相同的请求重试")); });
}

void AvatarPurchaseService::owned(const std::string &userId, int limit, const std::string &cursor, Callback cb) {
    if (limit < 1 || limit > 100) {
        cb(fail(Outcome::Validation, "limit 必须是 1 到 100"));
        return;
    }
    std::optional<AlbumService::Cursor> after;
    if (!cursor.empty()) {
        after = AlbumService::decodeCursor(cursor);
        if (!after) {
            cb(fail(Outcome::Validation, "游标无效"));
            return;
        }
    }
    if (!db::Pool::instance().ready()) {
        cb(fail(Outcome::Error, "数据库未就绪"));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    auto client = db::Pool::instance().client();
    const std::string fetch = std::to_string(limit + 1);
    client->execSqlAsync(
        kMemberSql,
        [done, client, userId, fetch, limit, after](const drogon::orm::Result &member) {
            if (member.empty()) {
                (*done)(fail(Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            auto ok = [done, limit](const drogon::orm::Result &rows) {
                Result r;
                r.outcome = Outcome::Ok;
                try {
                    Json::Value items(Json::arrayValue);
                    const size_t count = std::min(rows.size(), static_cast<size_t>(limit));
                    for (size_t i = 0; i < count; ++i) {
                        Json::Value entry(Json::objectValue);
                        entry["purchase"] = purchaseJson(rows[i]);
                        entry["current_item"] = currentItem(rows[i]);
                        items.append(entry);
                    }
                    r.body["items"] = items;
                    r.body["next_cursor"] = Json::nullValue;
                    if (rows.size() > count) {
                        const auto &last = rows[count - 1];
                        r.body["next_cursor"] = AlbumService::encodeCursor({last["created_at"].as<std::string>(), last["id"].as<std::string>()});
                    }
                } catch (const std::exception &) {
                    r = fail(Outcome::Error, "服务内部错误");
                }
                (*done)(std::move(r));
            };
            auto bad = [done](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << db::describePgError("avatar owned read failed", e.base());
                (*done)(fail(Outcome::Error, "服务内部错误"));
            };
            if (after) client->execSqlAsync(kOwnedAfterSql, ok, bad, userId, fetch, after->createdAt, after->id);
            else client->execSqlAsync(kOwnedFirstSql, ok, bad, userId, fetch);
        },
        [done](const drogon::orm::DrogonDbException &) { (*done)(fail(Outcome::Error, "服务内部错误")); }, userId);
}

}  // namespace usapp
