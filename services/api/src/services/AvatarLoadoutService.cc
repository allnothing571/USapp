#include "services/AvatarLoadoutService.h"

#include <drogon/drogon.h>

#include <memory>
#include <regex>
#include <set>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AvatarService.h"

namespace usapp {

namespace {

using Outcome = AvatarLoadoutService::Outcome;
using Result = AvatarLoadoutService::Result;

#define AL_TS(x) "to_char(" x " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

const char *kSaveSql = "SELECT usapp_avatar_loadout_save($1::uuid, $2, $3, $4::integer, $5::jsonb)::text AS r";
// The immutable facts of one saved loadout.
const char *kFactsSql =
    "SELECT id::text AS id, version, hair_item_id::text AS hair, clothes_item_id::text AS clothes, "
    "expression_item_id::text AS expression, hat_item_id::text AS hat, handheld_item_id::text AS handheld, "
    "accessory_item_id::text AS accessory, requested::text AS requested, starter_version, " AL_TS("created_at") " AS created_at "
    "FROM avatar_loadout_events WHERE id = $1::uuid";

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
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isObject();
}

std::string lower(std::string s) {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string canonical(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

}  // namespace

AvatarLoadoutService &AvatarLoadoutService::instance() {
    static AvatarLoadoutService service;
    return service;
}

bool AvatarLoadoutService::parse(const Json::Value &body, Request &out, std::string &message) {
    message = "请求必须恰好包含 expected_version 与 slots（hair、clothes、expression、hat、handheld、accessory）";
    if (!body.isObject() || body.size() != 2u || !body.isMember("expected_version") || !body.isMember("slots")) return false;
    const auto &v = body["expected_version"];
    if (!v.isInt() || v.isBool() || v.asInt64() < 0 || v.asInt64() > 1000000) {
        message = "expected_version 必须是非负整数";
        return false;
    }
    const auto &slots = body["slots"];
    if (!slots.isObject() || slots.size() != kSlots.size()) return false;
    static const std::regex uuid("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    for (size_t i = 0; i < kSlots.size(); ++i) {
        if (!slots.isMember(kSlots[i])) return false;
        const auto &s = slots[kSlots[i]];
        if (s.isNull()) {
            out.slots[i].reset();
        } else if (s.isString() && std::regex_match(s.asString(), uuid)) {
            out.slots[i] = lower(s.asString());
        } else {
            message = std::string(kSlots[i]) + " 必须是装扮 id 或 null";
            return false;
        }
    }
    out.expectedVersion = static_cast<int>(v.asInt64());
    return true;
}

std::string AvatarLoadoutService::requestedJson(const Request &request) {
    Json::Value requested(Json::objectValue);
    for (size_t i = 0; i < kSlots.size(); ++i) {
        requested[kSlots[i]] = request.slots[i] ? Json::Value(*request.slots[i]) : Json::Value(Json::nullValue);
    }
    return canonical(requested);
}

std::string AvatarLoadoutService::fingerprint(const Request &request) {
    return crypto::sha256Hex("avatar:loadout:v1:" + std::to_string(request.expectedVersion) + ":" + requestedJson(request));
}

void AvatarLoadoutService::save(const std::string &userId, const std::string &key, const Request &request, Callback cb, Fault fault) {
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
    // After a confirmed commit: today's appearance, read separately and labelled as such.
    auto withAppearance = [finish, userId](Result r) {
        AvatarService::instance().appearance(userId, [finish, r](AvatarService::Result now) mutable {
            if (now.outcome != AvatarService::Outcome::Ok) {
                // The save itself is confirmed; only the current view could not be read.
                r.body["current"] = Json::nullValue;
            } else {
                r.body["current"]["appearance"] = now.body["appearance"];
            }
            finish(std::move(r));
        });
    };
    const auto params = std::make_shared<std::array<std::string, 5>>(std::array<std::string, 5>{
        userId, key, fingerprint(request), std::to_string(request.expectedVersion), requestedJson(request)});
    db::TxScope::beginAsync(
        [params, fault, finish, withAppearance](const std::shared_ptr<db::TxScope> &tx) {
            tx->tx().execSqlAsync(
                kSaveSql,
                [tx, fault, finish, withAppearance](const drogon::orm::Result &rows) {
                    Json::Value answer;
                    if (rows.size() != 1u || !parseJson(rows[0]["r"].as<std::string>(), answer)) {
                        finish(fail(Outcome::Error, "服务内部错误"));
                        tx->fail("avatar loadout: invalid receipt");
                        return;
                    }
                    if (answer.isMember("error")) {
                        const auto error = answer["error"].asString();
                        Result r;
                        if (error == "key_reused") r = fail(Outcome::KeyReused, "该请求标识已用于另一项操作");
                        else if (error == "no_space") r = fail(Outcome::SpaceNotFound, "尚未加入空间");
                        else if (error == "conflict") r = fail(Outcome::VersionConflict, "形象已在别处更新，请刷新后再保存");
                        else if (error == "defaults_unavailable") r = fail(Outcome::DefaultsUnavailable, "还没有可用的初始形象，暂时不能清空必选部位");
                        else if (error == "not_found") r = fail(Outcome::NotFound, "装扮不存在");
                        else if (error == "slot_invalid") r = fail(Outcome::SlotInvalid, "装扮不属于这个部位");
                        else if (error == "not_owned") r = fail(Outcome::NotOwned, "还没有拥有这件装扮");
                        else if (error == "invalid") r = fail(Outcome::Validation, "请求内容无效");
                        else r = fail(Outcome::Error, "服务内部错误");
                        tx->rollback([finish, r]() { finish(r); });
                        return;
                    }
                    const bool replay = answer["result"].asString() == "replay";
                    if (fault == AvatarLoadoutService::Fault::FailBeforeCommit && !replay) {
                        tx->fail("avatar loadout: test fault before commit");
                        return;
                    }
                    tx->tx().execSqlAsync(
                        kFactsSql,
                        [tx, finish, withAppearance, replay](const drogon::orm::Result &facts) {
                            Result r;
                            try {
                                if (facts.size() != 1u) throw std::runtime_error("facts");
                                const auto &row = facts[0];
                                Json::Value loadout(Json::objectValue);
                                loadout["id"] = row["id"].as<std::string>();
                                loadout["version"] = row["version"].as<int>();
                                Json::Value slots(Json::objectValue);
                                for (const char *slot : AvatarLoadoutService::kSlots) {
                                    slots[slot] = row[slot].isNull() ? Json::Value(Json::nullValue) : Json::Value(row[slot].as<std::string>());
                                }
                                loadout["slots"] = slots;
                                Json::Value requested;
                                if (!parseJson(row["requested"].as<std::string>(), requested)) throw std::runtime_error("requested");
                                loadout["requested"] = requested;
                                loadout["starter_version"] = row["starter_version"].isNull() ? Json::Value(Json::nullValue)
                                                                                            : Json::Value(row["starter_version"].as<int>());
                                loadout["created_at"] = row["created_at"].as<std::string>();
                                r.body["replayed"] = replay;
                                r.body["loadout"] = loadout;
                            } catch (const std::exception &) {
                                finish(fail(Outcome::Error, "服务内部错误"));
                                tx->fail("avatar loadout: facts unreadable");
                                return;
                            }
                            r.outcome = replay ? Outcome::Replayed : Outcome::Created;
                            tx->commit([withAppearance, r]() { withAppearance(r); });
                        },
                        [tx, finish](const drogon::orm::DrogonDbException &) {
                            finish(fail(Outcome::Error, "服务内部错误"));
                            tx->fail("avatar loadout: facts read failed");
                        },
                        answer["event_id"].asString());
                },
                // A refused statement committed nothing: definite.
                [tx, finish](const drogon::orm::DrogonDbException &e) {
                    LOG_ERROR << db::describePgError("avatar loadout save failed", e.base());
                    finish(fail(Outcome::Error, "服务内部错误"));
                    tx->fail("avatar loadout save failed");
                },
                (*params)[0], (*params)[1], (*params)[2], (*params)[3], (*params)[4]);
        },
        // Otherwise (a failed begin or commit, or the test fault) nothing is known about the commit.
        [finish](const std::string &) { finish(fail(Outcome::ResultUnknown, "结果未确认，请使用相同的请求重试")); });
}

}  // namespace usapp
