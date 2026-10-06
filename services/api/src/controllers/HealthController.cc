// =====================================================================
// services/api/src/controllers/HealthController.cc
//
// GET /api/v1/health —— 存活探针。
//
// 刻意不挂任何过滤器，理由（与契约一致）：
//   * 探针必须能在"数据库挂了、会话表读不出来"时依然回答，
//     否则编排层会把一个活着的进程判成死的；因此它不读会话、不访问数据库。
//   * 它不返回任何业务数据，也不返回构建细节，只有版本号与 UTC 时间。
//   * 响应仍是 no-store（由 jsonResponse 统一保证）。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "http/HttpUtil.h"

namespace usapp::controllers {

class HealthController : public drogon::HttpController<HealthController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::health, "/api/v1/health", drogon::Get);
    METHOD_LIST_END

    void health(const drogon::HttpRequestPtr & /*req*/,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        Json::Value body(Json::objectValue);
        body["status"] = "ok";
        body["version"] = AppConfig::instance().appVersion;
        body["time"] = http::nowIso8601Utc();
        callback(jsonResponse(body, 200));
    }
};

}  // namespace usapp::controllers
