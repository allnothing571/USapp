// =====================================================================
// services/api/src/controllers/SpaceController.cc
//
// POST /api/v1/spaces                 创建情侣空间（调用者占 slot=1） -> 201
// GET  /api/v1/spaces/current         查询当前空间与成员              -> 200 / 404
// GET  /api/v1/spaces/{spaceId}       按 id 查询（仅本空间成员）       -> 200 / 404
//
// 路由顺序：字面量 /spaces/current 必须注册在参数路由 /spaces/{spaceId}
// 之前。Drogon 的路由树本身就优先匹配静态段，这里再显式排一次顺序，
// 是为了让"current 不是一个合法 uuid"这件事在代码里一眼可见。
//
// 隐私：非本空间成员与空间不存在都返回 404 SPACE_NOT_FOUND（同一个错误码、
// 同一句文案），因此无法用 id 探测别人的空间是否存在。
// 成员列表刻意不含邮箱（契约 SpaceMember 无 email 字段）。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>
#include <string>

#include "core/Errors.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/SpaceService.h"

namespace usapp::controllers {

class SpaceController : public drogon::HttpController<SpaceController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SpaceController::createSpace, "/api/v1/spaces", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(SpaceController::currentSpace, "/api/v1/spaces/current", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(SpaceController::spaceById, "/api/v1/spaces/{spaceId}", drogon::Get,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    // ------------------------------------------------------------------
    // 创建空间。没有请求体（契约未声明 requestBody）：空间名与创建者都由
    // 服务端决定——名字默认为空串，创建者取自会话，绝不取自客户端。
    // ------------------------------------------------------------------
    void createSpace(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        SpaceService::instance().createSpaceForUser(
            userId, [req, callback](SpaceService::Status status, SpaceInfo space) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                Json::Value body(Json::objectValue);
                body["space"] = ctrl::spaceJson(space);
                callback(jsonResponse(body, 201));
            });
    }

    // ------------------------------------------------------------------
    // 当前空间。刚注册还没加入空间的用户会得到 404 SPACE_NOT_FOUND——
    // 这不是异常，前端据此显示"创建空间/输入邀请码"。
    // ------------------------------------------------------------------
    void currentSpace(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        SpaceService::instance().getCurrentSpace(
            userId, [req, callback](SpaceService::Status status, SpaceInfo space) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                Json::Value body(Json::objectValue);
                body["space"] = ctrl::spaceJson(space);
                callback(jsonResponse(body, 200));
            });
    }

    // ------------------------------------------------------------------
    // 按 id 查询。形状不合法的 id 直接按"不存在"处理，而不是让它进 SQL
    // 触发类型转换错误变成 500。
    // ------------------------------------------------------------------
    void spaceById(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                   std::string spaceId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        if (!ctrl::looksLikeUuid(spaceId)) {
            callback(ctrl::fail(ErrorCode::SpaceNotFound, "空间不存在", req));
            return;
        }
        SpaceService::instance().getSpaceForMember(
            userId, spaceId, [req, callback](SpaceService::Status status, SpaceInfo space) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                Json::Value body(Json::objectValue);
                body["space"] = ctrl::spaceJson(space);
                callback(jsonResponse(body, 200));
            });
    }
};

}  // namespace usapp::controllers
