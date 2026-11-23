#include <drogon/HttpController.h>

#include "SharedRoomLive.h"

#include "db/Db.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "services/AvatarService.h"
#include "services/TaskService.h"

namespace usapp::controllers {
namespace {
using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using Request = drogon::HttpRequestPtr;
using RoomRead = std::function<void(Json::Value)>;

void readRoom(const std::string &user, RoomRead done, std::function<void()> failed) {
    db::Pool::instance().client()->execSqlAsync(
        "SELECT public.usapp_shared_room_read($1::uuid) || jsonb_build_object("
        "'furniture', public.usapp_room_furniture_read($1::uuid), 'positions', public.usapp_room_positions_read($1::uuid)) AS room",
        [done, failed](const drogon::orm::Result &rows) {
            if (rows.size() != 1) { failed(); return; }
            if (rows[0]["room"].isNull()) { done(Json::Value()); return; }
            const auto raw = rows[0]["room"].as<std::string>();
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value result;
            std::string errors;
            if (!reader->parse(raw.data(), raw.data() + raw.size(), &result, &errors) || !result.isObject()) {
                failed(); return;
            }
            done(std::move(result));
        }, [failed](const drogon::orm::DrogonDbException &) { failed(); }, user);
}

void answerRoom(const Request &req, const Callback &reply, Json::Value room, Json::Value appearance = {}) {
    if (room.isNull()) {
        reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req));
        return;
    }
    room.removeMember("partner_id");
    room["partner_appearance"] = std::move(appearance);
    reply(jsonResponse(room, 200));
}

template<typename... Args>
void writeRoom(const Request &req, const Callback &reply, const char *sql, Json::Value body, const std::string &user, Args... args) {
    db::TxScope::beginAsync(
        [req, reply, sql, body, user, args...](std::shared_ptr<db::TxScope> tx) {
            tx->tx().execSqlAsync(sql,
                [req, reply, tx, body, user](const drogon::orm::Result &rows) {
                    if (rows.size() != 1 || rows[0]["status"].isNull()) { tx->fail("room status missing"); return; }
                    const auto status = rows[0]["status"].as<std::string>();
                    if (status == "saved" || status == "replay") {
                        tx->commit([reply, body, user, status]() {
                            if (status == "saved") notifySharedRoomChange(user);
                            reply(jsonResponse(body, 200));
                        });
                        return;
                    }
                    if (status == "no_space" || status == "conflict" || status == "invalid" || status == "position_conflict" ||
                        status == "owned" || status == "insufficient" || status == "key_reused") {
                        tx->rollback([req, reply, status]() {
                            if (status == "no_space") reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req));
                            else if (status == "conflict") reply(ctrl::fail(ErrorCode::SharedRoomVersionConflict, "房间布局已变化，请先读取最新布局", req));
                            else if (status == "position_conflict") reply(ctrl::fail(ErrorCode::SharedRoomVersionConflict, "换位申请已变化，请查看最新站位", req));
                            else if (status == "insufficient") reply(ctrl::fail(ErrorCode::InsufficientBalance, "阳光不足", req));
                            else if (status == "key_reused") reply(ctrl::fail(ErrorCode::IdempotencyKeyReused, "购买请求编号已被使用", req));
                            else if (status == "owned") reply(ctrl::fail(ErrorCode::RoomFurnitureOwned, "空间已拥有这件家具", req));
                            else reply(ctrl::fail(ErrorCode::ValidationError, "家具位置或种类无效", req));
                        });
                        return;
                    }
                    tx->fail("unknown room status");
                },
                [tx](const drogon::orm::DrogonDbException &) { tx->fail("room statement failed"); }, user, args...);
        },
        [req, reply](const std::string &) {
            reply(ctrl::fail(ErrorCode::ResultUnknown, "操作结果待确认，请重新读取房间", req));
        });
}
} // namespace

class SharedRoomController : public drogon::HttpController<SharedRoomController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SharedRoomController::get, "/api/v1/shared-room", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedRoomController::save, "/api/v1/shared-room", drogon::Put, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedRoomController::share, "/api/v1/shared-room/avatar-sharing", drogon::Put, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedRoomController::buy, "/api/v1/shared-room/furniture/purchases", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedRoomController::positions, "/api/v1/shared-room/positions", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedRoomController::swap, "/api/v1/shared-room/positions", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void get(const Request &req, Callback &&reply) {
        std::string user;
        if (!ctrl::requireUser(req, reply, user)) return;
        const auto failed = [req, reply]() { reply(ctrl::fail(ErrorCode::InternalError, "暂时无法读取房间", req)); };
        readRoom(user, [req, reply, user, failed](Json::Value room) {
            if (room.isNull() || room["partner_id"].isNull()) { answerRoom(req, reply, std::move(room)); return; }
            const auto partner = room["partner_id"].asString();
            AvatarService::instance().appearance(partner, [req, reply, user, partner, failed](AvatarService::Result result) {
                const auto appearance = result.body.get("appearance", Json::Value());
                // Recheck membership and consent after the independent appearance read.
                readRoom(user, [req, reply, partner, appearance](Json::Value current) {
                    const bool visible = !current.isNull() && current["partner_id"].asString() == partner;
                    answerRoom(req, reply, std::move(current), visible ? appearance : Json::Value());
                }, failed);
            });
        }, failed);
    }

    void save(const Request &req, Callback &&reply) {
        std::string user;
        Json::Value body;
        if (!ctrl::requireUser(req, reply, user) || !ctrl::parseBody(req, reply, body)) return;
        if (!body.isObject() || body.size() != 2 || !body["expected_version"].isInt() ||
            body["expected_version"].asInt() < 0 || body["expected_version"].asInt() >= 1000000000 ||
            !body["layout"].isArray() || body["layout"].size() > 4) {
            reply(ctrl::fail(ErrorCode::ValidationError, "布局或版本无效", req)); return;
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        Json::Value result(Json::objectValue);
        const int version = body["expected_version"].asInt();
        result["version"] = version + 1;
        writeRoom(req, reply, "SELECT public.usapp_shared_room_save($1::uuid,$2::int,$3::jsonb) AS status",
                  result, user, version, Json::writeString(writer, body["layout"]));
    }

    void buy(const Request &req, Callback &&reply) {
        std::string user;
        Json::Value body;
        if (!ctrl::requireUser(req, reply, user) || !ctrl::parseBody(req, reply, body)) return;
        const auto key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key) || !body.isObject() || body.size() != 1 ||
            !body["item"].isString() || body["item"].asString().size() > 32) {
            reply(ctrl::fail(ErrorCode::ValidationError, "家具或购买请求编号无效", req)); return;
        }
        Json::Value receipt(Json::objectValue);
        receipt["item"] = body["item"];
        receipt["owned"] = true;
        writeRoom(req, reply, "SELECT public.usapp_room_furniture_buy($1::uuid,$2::text,$3::text) AS status",
                  receipt, user, body["item"].asString(), key);
    }

    void share(const Request &req, Callback &&reply) {
        std::string user;
        Json::Value body;
        if (!ctrl::requireUser(req, reply, user) || !ctrl::parseBody(req, reply, body)) return;
        if (!body.isObject() || body.size() != 1 || !body["share_avatar"].isBool()) {
            reply(ctrl::fail(ErrorCode::ValidationError, "请选择是否展示形象", req)); return;
        }
        writeRoom(req, reply, "SELECT public.usapp_shared_room_share($1::uuid,$2::boolean) AS status",
                  body, user, body["share_avatar"].asBool());
    }

    void positions(const Request &req, Callback &&reply) {
        std::string user;
        if (!ctrl::requireUser(req, reply, user)) return;
        readRoom(user, [req, reply](Json::Value room) {
            if (room.isNull()) { reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req)); return; }
            reply(jsonResponse(room["positions"], 200));
        }, [req, reply]() { reply(ctrl::fail(ErrorCode::InternalError, "暂时无法读取站位", req)); });
    }

    void swap(const Request &req, Callback &&reply) {
        std::string user;
        Json::Value body;
        if (!ctrl::requireUser(req, reply, user) || !ctrl::parseBody(req, reply, body)) return;
        if (!body.isObject() || body.size() != 2 || !body["id"].isString() ||
            !ctrl::looksLikeUuid(body["id"].asString()) ||
            (body["action"] != "request" && body["action"] != "accept" && body["action"] != "decline" && body["action"] != "cancel")) {
            reply(ctrl::fail(ErrorCode::ValidationError, "换位操作无效", req)); return;
        }
        Json::Value receipt(Json::objectValue);
        const auto &action = body["action"];
        receipt["id"] = body["id"];
        receipt["action"] = action;
        writeRoom(req, reply, "SELECT public.usapp_room_positions_write($1::uuid,$2::uuid,$3::text) AS status",
                  receipt, user, body["id"].asString(), action.asString());
    }
};
} // namespace usapp::controllers
