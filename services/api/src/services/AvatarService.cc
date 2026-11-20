#include "services/AvatarService.h"

#include <drogon/drogon.h>

#include <cstdint>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "core/Crypto.h"
#include "db/Db.h"

namespace usapp {

namespace {

using Outcome = AvatarService::Outcome;
using Result = AvatarService::Result;

#define AV_TS(x) "to_char(" x " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// One item with its current revision and asset. Layer names only: files are <asset_key>.<layer>.<format>.
#define AV_ITEM_COLUMNS                                                                                                  \
    "i.id::text AS id, i.category, i.starter, i.family, " AV_TS("i.created_at") " AS created_at, r.version, r.name, "     \
    "r.asset_key, r.price_sunshine, r.listed, " AV_TS("r.created_at") " AS revised_at, a.format, a.covers_hair_top, "    \
    "a.accessory_position, (SELECT string_agg(k, ',' ORDER BY k) FROM jsonb_object_keys(a.layers) k) AS layers "
#define AV_ITEM_FROM                                                                                                     \
    "FROM avatar_items i "                                                                                               \
    "JOIN LATERAL (SELECT * FROM avatar_item_revisions x WHERE x.item_id = i.id ORDER BY x.version DESC LIMIT 1) r ON true " \
    "JOIN avatar_assets a ON a.asset_key = r.asset_key "
#define AV_ORDER                                                                                                         \
    "ORDER BY array_position(ARRAY['hair','clothes','expression','hat','handheld','accessory'], i.category), "          \
    "i.starter DESC, i.created_at, i.id"

const char *kCatalogSql = "SELECT " AV_ITEM_COLUMNS AV_ITEM_FROM "WHERE r.listed " AV_ORDER;
// Admin reads only (B2, additive): the number of paid owners; null for a starter, which nobody owns by purchase.
#define AV_OWNER_COUNT ", CASE WHEN i.starter THEN NULL ELSE (SELECT count(*) FROM avatar_purchases o WHERE o.item_id = i.id) END AS owner_count "
const char *kAdminItemsSql = "SELECT " AV_ITEM_COLUMNS AV_OWNER_COUNT AV_ITEM_FROM AV_ORDER;
const char *kItemSql = "SELECT " AV_ITEM_COLUMNS AV_OWNER_COUNT AV_ITEM_FROM "WHERE i.id = $1::uuid";
const char *kRevisionsSql =
    "SELECT version, name, asset_key, price_sunshine, listed, admin_user_id::text AS admin_user_id, " AV_TS("created_at")
    " AS created_at FROM avatar_item_revisions WHERE item_id = $1::uuid ORDER BY version";
// The current configuration and its three items; no rows when none exists.
const char *kAppearanceSql =
    "WITH c AS (SELECT * FROM avatar_starter_configs ORDER BY version DESC LIMIT 1) "
    "SELECT c.version AS starter_version, s.slot, " AV_ITEM_COLUMNS
    "FROM c CROSS JOIN LATERAL (VALUES ('hair', c.hair_item_id), ('clothes', c.clothes_item_id), "
    "('expression', c.expression_item_id)) s(slot, item_id) "
    "JOIN avatar_items i ON i.id = s.item_id "
    "JOIN LATERAL (SELECT * FROM avatar_item_revisions x WHERE x.item_id = i.id ORDER BY x.version DESC LIMIT 1) r ON true "
    "JOIN avatar_assets a ON a.asset_key = r.asset_key";
// B3: the caller's latest saved loadout, one row per filled slot; no rows when the caller has never saved.
const char *kSavedLoadoutSql =
    "WITH e AS (SELECT * FROM avatar_loadout_events WHERE user_id = $1::uuid ORDER BY version DESC LIMIT 1) "
    "SELECT e.version AS loadout_version, e.starter_version AS saved_starter_version, s.slot, " AV_ITEM_COLUMNS
    "FROM e CROSS JOIN LATERAL (VALUES ('hair', e.hair_item_id), ('clothes', e.clothes_item_id), ('expression', e.expression_item_id), "
    "('hat', e.hat_item_id), ('handheld', e.handheld_item_id), ('accessory', e.accessory_item_id)) s(slot, item_id) "
    "JOIN avatar_items i ON i.id = s.item_id "
    "JOIN LATERAL (SELECT * FROM avatar_item_revisions x WHERE x.item_id = i.id ORDER BY x.version DESC LIMIT 1) r ON true "
    "JOIN avatar_assets a ON a.asset_key = r.asset_key";
const char *kStarterSql =
    "SELECT version, hair_item_id::text AS hair, clothes_item_id::text AS clothes, expression_item_id::text AS expression, "
    "admin_user_id::text AS admin_user_id, " AV_TS("created_at") " AS created_at "
    "FROM avatar_starter_configs ORDER BY version DESC LIMIT 1";
const char *kAssetsSql =
    "SELECT a.asset_key, a.category, a.family, a.revision, a.format, a.layers::text AS layers, a.covers_hair_top, "
    "a.accessory_position, " AV_TS("a.created_at") " AS created_at, i.id::text AS item_id "
    "FROM avatar_assets a LEFT JOIN avatar_items i ON i.family = a.family "
    "ORDER BY array_position(ARRAY['hair','clothes','expression','hat','handheld','accessory'], a.category), a.family, a.revision";

const char *kCreateSql =
    "SELECT usapp_avatar_item_create($1::uuid, $2, $3, $4, $5::boolean, $6, $7, NULLIF($8, '')::integer, $9::boolean)::text AS r";
const char *kReviseSql =
    "SELECT usapp_avatar_item_revise($1::uuid, $2, $3, $4::uuid, $5::integer, $6, $7, NULLIF($8, '')::integer, $9::boolean)::text AS r";
const char *kStarterSetSql =
    "SELECT usapp_avatar_starter_set($1::uuid, $2, $3, $4::integer, $5::uuid, $6::uuid, $7::uuid)::text AS r";

const std::set<std::string> &categories() {
    static const std::set<std::string> values = {"hair", "clothes", "expression", "hat", "handheld", "accessory"};
    return values;
}

Json::Value itemJson(const drogon::orm::Row &row, bool admin) {
    Json::Value item(Json::objectValue);
    item["id"] = row["id"].as<std::string>();
    item["category"] = row["category"].as<std::string>();
    item["starter"] = row["starter"].as<bool>();
    item["version"] = row["version"].as<int>();
    item["name"] = row["name"].as<std::string>();
    item["price_sunshine"] = row["price_sunshine"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["price_sunshine"].as<int>());
    Json::Value asset(Json::objectValue);
    asset["key"] = row["asset_key"].as<std::string>();
    asset["format"] = row["format"].as<std::string>();
    Json::Value layers(Json::arrayValue);
    std::stringstream names(row["layers"].as<std::string>());
    for (std::string name; std::getline(names, name, ',');) layers.append(name);
    asset["layers"] = layers;
    asset["covers_hair_top"] = row["covers_hair_top"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["covers_hair_top"].as<bool>());
    asset["accessory_position"] =
        row["accessory_position"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["accessory_position"].as<std::string>());
    item["asset"] = asset;
    if (admin) {
        item["listed"] = row["listed"].as<bool>();
        item["family"] = row["family"].as<std::string>();
        item["created_at"] = row["created_at"].as<std::string>();
        item["revised_at"] = row["revised_at"].as<std::string>();
        item["owner_count"] = row["owner_count"].isNull() ? Json::Value(Json::nullValue) : Json::Value(Json::Int64(row["owner_count"].as<long long>()));
    }
    return item;
}

Result fail(Outcome outcome, const std::string &message) {
    Result r;
    r.outcome = outcome;
    r.message = message;
    return r;
}

std::shared_ptr<AvatarService::Callback> shared(AvatarService::Callback cb) { return std::make_shared<AvatarService::Callback>(std::move(cb)); }

void read(const char *sql, std::function<void(const drogon::orm::Result &, Result &)> shape, AvatarService::Callback cb,
          const std::vector<std::string> &params = {}) {
    if (!db::Pool::instance().ready()) {
        cb(fail(Outcome::Error, "数据库未就绪"));
        return;
    }
    auto done = shared(std::move(cb));
    auto ok = [done, shape](const drogon::orm::Result &rows) {
        Result result;
        result.outcome = Outcome::Ok;
        try {
            shape(rows, result);
        } catch (const std::exception &) {
            result = fail(Outcome::Error, "读取失败");
        }
        (*done)(std::move(result));
    };
    auto bad = [done](const drogon::orm::DrogonDbException &e) {
        LOG_ERROR << db::describePgError("avatar read failed", e.base());
        (*done)(fail(Outcome::Error, "读取失败"));
    };
    auto client = db::Pool::instance().client();
    if (params.empty()) client->execSqlAsync(sql, ok, bad);
    else client->execSqlAsync(sql, ok, bad, params[0]);
}

bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isObject();
}

bool exactKeys(const Json::Value &body, const std::set<std::string> &keys) {
    if (!body.isObject() || body.size() != keys.size()) return false;
    for (const auto &name : body.getMemberNames()) {
        if (!keys.count(name)) return false;
    }
    return true;
}

bool smallInt(const Json::Value &v, int low, int high, int &out) {
    if (!v.isInt() || v.isBool()) return false;
    const auto n = v.asInt64();
    if (n < low || n > high) return false;
    out = static_cast<int>(n);
    return true;
}

bool looksLikeUuid(const std::string &s) {
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(s, pattern);
}

std::string lower(std::string s) {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// price_sunshine: null for a starter, an integer 1-9999 for a paid item. Anything else is invalid.
bool parsePrice(const Json::Value &v, std::optional<int> &out) {
    if (v.isNull()) {
        out.reset();
        return true;
    }
    int n = 0;
    if (!smallInt(v, 1, 9999, n)) return false;
    out = n;
    return true;
}

std::string canonical(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);  // members are written in sorted order
}

Json::Value priceJson(const std::optional<int> &price) { return price ? Json::Value(*price) : Json::Value(Json::nullValue); }

// Runs one 0044 write function, then reads the item or configuration it names, all in one transaction.
void write(const char *sql, std::vector<std::string> params, AvatarService::Fault fault, AvatarService::Callback cb) {
    if (!db::Pool::instance().ready()) {
        cb(fail(Outcome::Error, "数据库未就绪"));
        return;
    }
    auto done = shared(std::move(cb));
    auto answered = std::make_shared<bool>(false);
    auto finish = [done, answered](Result r) {
        if (*answered) return;
        *answered = true;
        (*done)(std::move(r));
    };
    db::TxScope::beginAsync(
        [sql, params, fault, finish](const std::shared_ptr<db::TxScope> &tx) {
            auto onRows = [tx, fault, finish](const drogon::orm::Result &rows) {
                Json::Value answer;
                if (rows.size() != 1u || !parseJson(rows[0]["r"].as<std::string>(), answer)) {
                    finish(fail(Outcome::Error, "服务内部错误"));
                    tx->fail("avatar write: invalid receipt");
                    return;
                }
                if (answer.isMember("error")) {
                    const auto error = answer["error"].asString();
                    Result r;
                    if (error == "key_reused") r = fail(Outcome::KeyReused, "该请求标识已用于另一项操作");
                    else if (error == "not_found") r = fail(Outcome::NotFound, "装扮不存在");
                    else if (error == "conflict") r = fail(Outcome::VersionConflict, "内容已被修改，请刷新后再试");
                    else if (error == "asset_invalid") r = fail(Outcome::AssetInvalid, "素材不可用于这件装扮");
                    else if (error == "starter_invalid") r = fail(Outcome::StarterInvalid, "初始形象必须为每个必选类别指定一件初始装扮");
                    else r = fail(Outcome::Validation, "请求内容无效或没有任何修改");
                    tx->rollback([finish, r]() { finish(r); });
                    return;
                }
                const bool replay = answer["result"].asString() == "replay";
                if (fault == AvatarService::Fault::FailBeforeCommit) {
                    tx->fail("avatar write: test fault before commit");
                    return;
                }
                auto commitWith = [tx, finish, replay](Json::Value body) {
                    body["replayed"] = replay;
                    tx->commit([finish, replay, body]() {
                        Result r;
                        r.outcome = replay ? Outcome::Replayed : Outcome::Created;
                        r.body = body;
                        finish(r);
                    });
                };
                if (answer.isMember("item_id")) {
                    const auto itemId = answer["item_id"].asString();
                    const int version = answer["version"].asInt();
                    tx->tx().execSqlAsync(
                        kItemSql,
                        [tx, commitWith, version, finish](const drogon::orm::Result &item) {
                            if (item.size() != 1u) {
                                finish(fail(Outcome::Error, "服务内部错误"));
                                tx->fail("avatar write: item missing");
                                return;
                            }
                            Json::Value body(Json::objectValue);
                            body["revision_version"] = version;
                            body["item"] = itemJson(item[0], true);
                            commitWith(body);
                        },
                        [tx, finish](const drogon::orm::DrogonDbException &) {
                            finish(fail(Outcome::Error, "服务内部错误"));
                            tx->fail("avatar write: item read failed");
                        },
                        itemId);
                    return;
                }
                tx->tx().execSqlAsync(
                    "SELECT version, hair_item_id::text AS hair, clothes_item_id::text AS clothes, "
                    "expression_item_id::text AS expression, admin_user_id::text AS admin_user_id, " AV_TS("created_at")
                    " AS created_at FROM avatar_starter_configs WHERE version = $1::integer",
                    [tx, commitWith, finish](const drogon::orm::Result &config) {
                        if (config.size() != 1u) {
                            finish(fail(Outcome::Error, "服务内部错误"));
                            tx->fail("avatar write: configuration missing");
                            return;
                        }
                        Json::Value starter(Json::objectValue);
                        starter["version"] = config[0]["version"].as<int>();
                        for (const char *slot : {"hair", "clothes", "expression"}) starter[slot] = config[0][slot].as<std::string>();
                        starter["admin_user_id"] = config[0]["admin_user_id"].as<std::string>();
                        starter["created_at"] = config[0]["created_at"].as<std::string>();
                        Json::Value body(Json::objectValue);
                        body["starter"] = starter;
                        commitWith(body);
                    },
                    [tx, finish](const drogon::orm::DrogonDbException &) {
                        finish(fail(Outcome::Error, "服务内部错误"));
                        tx->fail("avatar write: configuration read failed");
                    },
                    std::to_string(answer["version"].asInt()));
            };
            // A refused statement committed nothing: a definite failure, not an unknown result.
            auto onError = [tx, finish](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << db::describePgError("avatar write failed", e.base());
                finish(fail(Outcome::Error, "服务内部错误"));
                tx->fail("avatar write failed");
            };
            if (params.size() == 9u) {
                tx->tx().execSqlAsync(sql, onRows, onError, params[0], params[1], params[2], params[3], params[4], params[5], params[6],
                                      params[7], params[8]);
            } else {
                tx->tx().execSqlAsync(sql, onRows, onError, params[0], params[1], params[2], params[3], params[4], params[5], params[6]);
            }
        },
        // Otherwise (a failed commit or begin, or the test fault) nothing is known about the commit.
        [finish](const std::string &) { finish(fail(Outcome::ResultUnknown, "结果未确认，请使用相同的请求重试")); });
}

}  // namespace

AvatarService &AvatarService::instance() {
    static AvatarService service;
    return service;
}

bool AvatarService::validAssetKey(const std::string &key) {
    static const std::regex pattern("^[a-z]+\\.[a-z0-9_]{1,40}\\.v[1-9][0-9]{0,2}$");
    return key.size() <= 64 && std::regex_match(key, pattern) && categories().count(key.substr(0, key.find('.')));
}

// 1-20 code points of well-formed UTF-8; no C0, DEL or C1 controls; no leading or trailing space (the database's btrim).
bool AvatarService::validName(const std::string &name) {
    if (name.empty() || name.front() == ' ' || name.back() == ' ') return false;
    size_t points = 0;
    for (size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80) { len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > name.size()) return false;
        for (size_t j = 1; j < len; ++j) {
            const auto d = static_cast<unsigned char>(name[i + j]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) {
            return false;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
        ++points;
        i += len;
    }
    return points >= 1 && points <= 20;
}

bool AvatarService::parseCreate(const Json::Value &body, ItemInput &out, std::string &message) {
    message = "请求必须恰好包含 category、starter、name、asset_key、price_sunshine、listed";
    if (!exactKeys(body, {"category", "starter", "name", "asset_key", "price_sunshine", "listed"})) return false;
    if (!body["category"].isString() || !categories().count(body["category"].asString())) {
        message = "category 无效";
        return false;
    }
    if (!body["starter"].isBool() || !body["listed"].isBool()) {
        message = "starter 与 listed 必须是布尔值";
        return false;
    }
    if (!body["name"].isString() || !validName(body["name"].asString())) {
        message = "name 必须是 1 到 20 个字符，不含控制字符，首尾无空格";
        return false;
    }
    if (!body["asset_key"].isString() || !validAssetKey(body["asset_key"].asString())) {
        message = "asset_key 必须是登记过的素材标识";
        return false;
    }
    if (!parsePrice(body["price_sunshine"], out.price) || out.price.has_value() == body["starter"].asBool()) {
        message = "初始装扮的 price_sunshine 必须为 null；付费装扮必须是 1 到 9999 的整数";
        return false;
    }
    out.category = body["category"].asString();
    out.starter = body["starter"].asBool();
    out.name = body["name"].asString();
    out.assetKey = body["asset_key"].asString();
    out.listed = body["listed"].asBool();
    if (out.assetKey.substr(0, out.assetKey.find('.')) != out.category) {
        message = "asset_key 与 category 不一致";
        return false;
    }
    return true;
}

bool AvatarService::parseRevision(const Json::Value &body, RevisionInput &out, std::string &message) {
    message = "请求必须恰好包含 expected_version、name、asset_key、price_sunshine、listed";
    if (!exactKeys(body, {"expected_version", "name", "asset_key", "price_sunshine", "listed"})) return false;
    if (!smallInt(body["expected_version"], 1, 1000000, out.expectedVersion)) {
        message = "expected_version 必须是正整数";
        return false;
    }
    if (!body["listed"].isBool()) {
        message = "listed 必须是布尔值";
        return false;
    }
    if (!body["name"].isString() || !validName(body["name"].asString())) {
        message = "name 必须是 1 到 20 个字符，不含控制字符，首尾无空格";
        return false;
    }
    if (!body["asset_key"].isString() || !validAssetKey(body["asset_key"].asString())) {
        message = "asset_key 必须是登记过的素材标识";
        return false;
    }
    if (!parsePrice(body["price_sunshine"], out.price)) {
        message = "price_sunshine 必须为 null 或 1 到 9999 的整数";
        return false;
    }
    out.name = body["name"].asString();
    out.assetKey = body["asset_key"].asString();
    out.listed = body["listed"].asBool();
    return true;
}

bool AvatarService::parseStarter(const Json::Value &body, StarterInput &out, std::string &message) {
    message = "请求必须恰好包含 expected_version、hair、clothes、expression";
    if (!exactKeys(body, {"expected_version", "hair", "clothes", "expression"})) return false;
    if (!smallInt(body["expected_version"], 0, 1000000, out.expectedVersion)) {
        message = "expected_version 必须是非负整数";
        return false;
    }
    for (const char *slot : {"hair", "clothes", "expression"}) {
        if (!body[slot].isString() || !looksLikeUuid(body[slot].asString())) {
            message = std::string(slot) + " 必须是装扮 id";
            return false;
        }
    }
    out.hair = lower(body["hair"].asString());
    out.clothes = lower(body["clothes"].asString());
    out.expression = lower(body["expression"].asString());
    return true;
}

std::string AvatarService::fingerprint(const ItemInput &in) {
    Json::Value v(Json::objectValue);
    v["category"] = in.category;
    v["starter"] = in.starter;
    v["name"] = in.name;
    v["asset_key"] = in.assetKey;
    v["price_sunshine"] = priceJson(in.price);
    v["listed"] = in.listed;
    return crypto::sha256Hex("avatar:create:v1:" + canonical(v));
}

std::string AvatarService::fingerprint(const std::string &itemId, const RevisionInput &in) {
    Json::Value v(Json::objectValue);
    v["item_id"] = lower(itemId);
    v["expected_version"] = in.expectedVersion;
    v["name"] = in.name;
    v["asset_key"] = in.assetKey;
    v["price_sunshine"] = priceJson(in.price);
    v["listed"] = in.listed;
    return crypto::sha256Hex("avatar:revise:v1:" + canonical(v));
}

std::string AvatarService::fingerprint(const StarterInput &in) {
    Json::Value v(Json::objectValue);
    v["expected_version"] = in.expectedVersion;
    v["hair"] = in.hair;
    v["clothes"] = in.clothes;
    v["expression"] = in.expression;
    return crypto::sha256Hex("avatar:starter:v1:" + canonical(v));
}

void AvatarService::catalog(Callback cb) {
    read(kCatalogSql, [](const drogon::orm::Result &rows, Result &r) {
        Json::Value items(Json::arrayValue);
        for (const auto &row : rows) items.append(itemJson(row, false));
        r.body["items"] = items;
    }, std::move(cb));
}

// B1 has no saved outfit: every caller's appearance is the current starter configuration, said as such.
// B1: the starter defaults. B3 (additive): once the caller has saved a loadout, that loadout, with loadout_version; otherwise the
// B1 answer with loadout_version 0. Only the caller's own state; the read needs a session, never a couple.
void AvatarService::appearance(const std::string &userId, Callback cb) {
    if (userId.empty()) {
        cb(fail(Outcome::Validation, "缺少用户"));
        return;
    }
    auto done = shared(std::move(cb));
    read(kSavedLoadoutSql, [](const drogon::orm::Result &rows, Result &r) {
        if (rows.empty()) return;  // never saved: answered below from the starter defaults
        Json::Value appearance(Json::objectValue);
        appearance["source"] = "saved_loadout";
        appearance["complete"] = true;
        appearance["loadout_version"] = rows[0]["loadout_version"].as<int>();
        appearance["starter_version"] =
            rows[0]["saved_starter_version"].isNull() ? Json::Value(Json::nullValue) : Json::Value(rows[0]["saved_starter_version"].as<int>());
        Json::Value slots(Json::objectValue);
        for (const char *slot : {"hair", "clothes", "expression", "hat", "handheld", "accessory"}) slots[slot] = Json::nullValue;
        for (const auto &row : rows) slots[row["slot"].as<std::string>()] = itemJson(row, false);
        if (slots["hair"].isNull() || slots["clothes"].isNull() || slots["expression"].isNull()) throw std::runtime_error("saved loadout");
        appearance["slots"] = slots;
        r.body["appearance"] = appearance;
    }, [done](Result saved) {
        if (saved.outcome != Outcome::Ok || saved.body.isMember("appearance")) {
            (*done)(std::move(saved));
            return;
        }
        read(kAppearanceSql, [](const drogon::orm::Result &rows, Result &r) {
            Json::Value appearance(Json::objectValue);
            appearance["source"] = "starter_defaults";
            Json::Value slots(Json::objectValue);
            for (const char *slot : {"hair", "clothes", "expression", "hat", "handheld", "accessory"}) slots[slot] = Json::nullValue;
            appearance["complete"] = rows.size() == 3u;
            appearance["starter_version"] = Json::nullValue;
            appearance["loadout_version"] = 0;
            if (rows.size() == 3u) {
                appearance["starter_version"] = rows[0]["starter_version"].as<int>();
                for (const auto &row : rows) slots[row["slot"].as<std::string>()] = itemJson(row, false);
            } else if (!rows.empty()) {
                throw std::runtime_error("partial starter configuration");
            }
            appearance["slots"] = slots;
            r.body["appearance"] = appearance;
        }, [done](Result r) { (*done)(std::move(r)); });
    }, {userId});
}

void AvatarService::adminAssets(Callback cb) {
    read(kAssetsSql, [](const drogon::orm::Result &rows, Result &r) {
        Json::Value items(Json::arrayValue);
        for (const auto &row : rows) {
            Json::Value a(Json::objectValue);
            a["key"] = row["asset_key"].as<std::string>();
            a["category"] = row["category"].as<std::string>();
            a["family"] = row["family"].as<std::string>();
            a["revision"] = row["revision"].as<int>();
            a["format"] = row["format"].as<std::string>();
            Json::Value layers;
            if (!parseJson(row["layers"].as<std::string>(), layers)) throw std::runtime_error("layers");
            a["layers"] = layers;
            a["covers_hair_top"] = row["covers_hair_top"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["covers_hair_top"].as<bool>());
            a["accessory_position"] =
                row["accessory_position"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["accessory_position"].as<std::string>());
            a["item_id"] = row["item_id"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["item_id"].as<std::string>());
            a["created_at"] = row["created_at"].as<std::string>();
            items.append(a);
        }
        r.body["assets"] = items;
    }, std::move(cb));
}

void AvatarService::adminItems(Callback cb) {
    read(kAdminItemsSql, [](const drogon::orm::Result &rows, Result &r) {
        Json::Value items(Json::arrayValue);
        for (const auto &row : rows) items.append(itemJson(row, true));
        r.body["items"] = items;
    }, std::move(cb));
}

void AvatarService::adminItem(const std::string &itemId, Callback cb) {
    if (!looksLikeUuid(itemId)) {
        cb(fail(Outcome::NotFound, "装扮不存在"));
        return;
    }
    const auto id = lower(itemId);
    auto done = shared(std::move(cb));
    read(kItemSql, [](const drogon::orm::Result &rows, Result &r) {
        if (rows.size() != 1u) {
            r.outcome = Outcome::NotFound;
            r.message = "装扮不存在";
            return;
        }
        r.body["item"] = itemJson(rows[0], true);
    }, [done, id](Result item) {
        if (item.outcome != Outcome::Ok) {
            (*done)(std::move(item));
            return;
        }
        read(kRevisionsSql, [item](const drogon::orm::Result &rows, Result &r) {
            r.body = item.body;
            Json::Value revisions(Json::arrayValue);
            for (const auto &row : rows) {
                Json::Value v(Json::objectValue);
                v["version"] = row["version"].as<int>();
                v["name"] = row["name"].as<std::string>();
                v["asset_key"] = row["asset_key"].as<std::string>();
                v["price_sunshine"] = row["price_sunshine"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["price_sunshine"].as<int>());
                v["listed"] = row["listed"].as<bool>();
                v["admin_user_id"] = row["admin_user_id"].as<std::string>();
                v["created_at"] = row["created_at"].as<std::string>();
                revisions.append(v);
            }
            r.body["revisions"] = revisions;
        }, [done](Result r) { (*done)(std::move(r)); }, {id});
    }, {id});
}

void AvatarService::adminStarter(Callback cb) {
    read(kStarterSql, [](const drogon::orm::Result &rows, Result &r) {
        if (rows.empty()) {
            r.body["starter"] = Json::nullValue;
            return;
        }
        Json::Value starter(Json::objectValue);
        starter["version"] = rows[0]["version"].as<int>();
        for (const char *slot : {"hair", "clothes", "expression"}) starter[slot] = rows[0][slot].as<std::string>();
        starter["admin_user_id"] = rows[0]["admin_user_id"].as<std::string>();
        starter["created_at"] = rows[0]["created_at"].as<std::string>();
        r.body["starter"] = starter;
    }, std::move(cb));
}

void AvatarService::createItem(const std::string &adminId, const std::string &key, const ItemInput &in, Callback cb, Fault fault) {
    write(kCreateSql,
          {adminId, key, fingerprint(in), in.category, in.starter ? "true" : "false", in.name, in.assetKey,
           in.price ? std::to_string(*in.price) : std::string(), in.listed ? "true" : "false"},
          fault, std::move(cb));
}

void AvatarService::reviseItem(const std::string &adminId, const std::string &key, const std::string &itemId, const RevisionInput &in,
                               Callback cb, Fault fault) {
    if (!looksLikeUuid(itemId)) {
        cb(fail(Outcome::NotFound, "装扮不存在"));
        return;
    }
    write(kReviseSql,
          {adminId, key, fingerprint(itemId, in), lower(itemId), std::to_string(in.expectedVersion), in.name, in.assetKey,
           in.price ? std::to_string(*in.price) : std::string(), in.listed ? "true" : "false"},
          fault, std::move(cb));
}

void AvatarService::setStarter(const std::string &adminId, const std::string &key, const StarterInput &in, Callback cb, Fault fault) {
    write(kStarterSetSql, {adminId, key, fingerprint(in), std::to_string(in.expectedVersion), in.hair, in.clothes, in.expression}, fault,
          std::move(cb));
}

}  // namespace usapp
