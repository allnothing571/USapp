#include <drogon/HttpController.h>
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AchievementService.h"
namespace usapp::controllers {
class AchievementController : public drogon::HttpController<AchievementController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AchievementController::read, "/api/v1/achievements", drogon::Get, AuthFilter::classTypeName());
    METHOD_LIST_END
    void read(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string user;
        if (!ctrl::requireUser(req, callback, user)) return;
        AchievementService::read(user, [req, callback](AchievementService::Result r) {
            if (r.outcome == AchievementService::Outcome::Ok) callback(jsonResponse(r.body));
            else if (r.outcome == AchievementService::Outcome::SpaceNotFound)
                callback(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req));
            else callback(ctrl::fail(ErrorCode::InternalError, "成就读取失败", req));
        });
    }
};
}
