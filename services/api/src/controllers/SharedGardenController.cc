#include <drogon/HttpController.h>

#include "db/Db.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"

namespace usapp::controllers {
namespace {
using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
using Request = drogon::HttpRequestPtr;

bool hasDateShape(const std::string &date) {
    if (date.size() != 10) return false;
    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i == 4 || i == 7) {
            if (date[i] != '-') return false;
        } else if (date[i] < '0' || date[i] > '9') {
            return false;
        }
    }
    return true;
}

Json::Value wateringDays(const drogon::orm::Result &rows) {
    Json::Value result(Json::arrayValue);
    for (const auto &row : rows) {
        Json::Value day(Json::objectValue);
        day["watering_date"] = row["day_date"].as<std::string>();
        day["watered"] = row["watered"].as<bool>();
        day["partner_watered"] = row["partner_watered"].as<bool>();
        result.append(std::move(day));
    }
    return result;
}
} // namespace

class SharedGardenController : public drogon::HttpController<SharedGardenController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SharedGardenController::get, "/api/v1/shared-garden", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedGardenController::history, "/api/v1/shared-garden/history", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedGardenController::water, "/api/v1/shared-garden/watering", drogon::Put, AuthFilter::classTypeName());
    METHOD_LIST_END

    void get(const Request &req, Callback &&reply) {
        std::string user;
        if (!ctrl::requireUser(req, reply, user)) return;
        const auto failed = [req, reply]() {
            reply(ctrl::fail(ErrorCode::InternalError, "暂时无法读取花园", req));
        };
        db::Pool::instance().client()->execSqlAsync(
            "SELECT garden.*, days.watering_date AS day_date, days.watered, days.partner_watered "
            "FROM public.usapp_shared_garden_read($1::uuid) AS garden "
            "CROSS JOIN public.usapp_shared_garden_recent($1::uuid) AS days "
            "ORDER BY days.watering_date DESC",
            [req, reply, failed](const drogon::orm::Result &rows) {
                if (rows.empty()) { reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req)); return; }
                if (rows.size() != 7) { failed(); return; }
                const auto row = rows[0];
                Json::Value garden(Json::objectValue);
                garden["watering_date"] = row["watering_date"].as<std::string>();
                garden["total_waterings"] = static_cast<Json::Int64>(row["total_waterings"].as<long long>());
                garden["watered_today"] = row["watered_today"].as<bool>();
                garden["partner_watered_today"] = row["partner_watered_today"].as<bool>();
                garden["recent_waterings"] = wateringDays(rows);
                reply(jsonResponse(garden, 200));
            },
            [failed](const drogon::orm::DrogonDbException &) { failed(); },
            user);
    }

    void history(const Request &req, Callback &&reply) {
        std::string user;
        if (!ctrl::requireUser(req, reply, user)) return;
        const auto month = req->getParameter("month");
        if (!month.empty() && (!hasDateShape(month + "-01") || month.substr(0, 4) == "0000" ||
                               month.substr(5) < "01" || month.substr(5) > "12")) {
            reply(ctrl::fail(ErrorCode::ValidationError, "请选择有效月份", req));
            return;
        }
        const auto failed = [req, reply]() {
            reply(ctrl::fail(ErrorCode::InternalError, "暂时无法读取浇水月历", req));
        };
        db::Pool::instance().client()->execSqlAsync(
            "SELECT requested_month, current_month, watering_date AS day_date, watered, partner_watered "
            "FROM public.usapp_shared_garden_month($1::uuid,$2::text) ORDER BY watering_date",
            [req, reply, failed](const drogon::orm::Result &rows) {
                if (rows.empty()) { reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req)); return; }
                if (rows[0]["day_date"].isNull()) {
                    reply(ctrl::fail(ErrorCode::ValidationError, "月份不能晚于当前北京时间月份", req));
                    return;
                }
                if (rows.size() > 31) { failed(); return; }
                Json::Value result(Json::objectValue);
                result["month"] = rows[0]["requested_month"].as<std::string>();
                result["current_month"] = rows[0]["current_month"].as<std::string>();
                result["days"] = wateringDays(rows);
                reply(jsonResponse(result, 200));
            },
            [failed](const drogon::orm::DrogonDbException &) { failed(); },
            user, month);
    }

    void water(const Request &req, Callback &&reply) {
        std::string user;
        Json::Value body;
        if (!ctrl::requireUser(req, reply, user) || !ctrl::parseBody(req, reply, body)) return;
        if (!body.isObject() || body.size() != 1 || !body["watering_date"].isString() ||
            !hasDateShape(body["watering_date"].asString())) {
            reply(ctrl::fail(ErrorCode::ValidationError, "请选择页面显示的浇水日期", req));
            return;
        }
        const auto date = body["watering_date"].asString();
        db::TxScope::beginAsync(
            [req, reply, user, date](std::shared_ptr<db::TxScope> tx) {
                tx->tx().execSqlAsync(
                    "SELECT public.usapp_shared_garden_water($1::uuid,$2::text) AS status",
                    [req, reply, date, tx](const drogon::orm::Result &rows) {
                        if (rows.size() != 1 || rows[0]["status"].isNull()) { tx->fail("watering status missing"); return; }
                        const auto status = rows[0]["status"].as<std::string>();
                        if (status == "watered") {
                            tx->commit([reply, date]() {
                                Json::Value receipt(Json::objectValue);
                                receipt["watering_date"] = date;
                                reply(jsonResponse(receipt, 200));
                            });
                        } else if (status == "no_space" || status == "date_changed") {
                            tx->rollback([req, reply, status]() {
                                if (status == "no_space") reply(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req));
                                else reply(ctrl::fail(ErrorCode::SharedGardenDateChanged, "浇水日期已变更，请读取今天的花园", req));
                            });
                        } else {
                            tx->fail("unknown watering status");
                        }
                    },
                    [tx](const drogon::orm::DrogonDbException &) { tx->fail("watering statement failed"); },
                    user, date);
            },
            [req, reply](const std::string &) {
                reply(ctrl::fail(ErrorCode::ResultUnknown, "浇水结果待确认，请读取花园", req));
            });
    }
};
} // namespace usapp::controllers
