// =====================================================================
// services/api/src/controllers/InviteController.cc
//
// GET    /api/v1/spaces/current/invites              列出邀请（只有元数据）
// POST   /api/v1/spaces/current/invites              生成邀请（明文只回一次）
// DELETE /api/v1/spaces/current/invites/{inviteId}   撤销邀请
// POST   /api/v1/invites/{code}/accept               接受邀请并加入空间
//
// 三条不可动摇的规则：
//   1. 邀请码明文只在【创建响应】里出现在网络上一次；库里只保存 SHA-256，
//      列表接口永远不回明文，也不回码的散列。invite_url 用 PUBLIC_ORIGIN 拼，
//      绝不用请求的 Host 头（Host 由客户端控制，会把码指向攻击者的域名）。
//   2. 撤销与"邀请不属于我"合并成同一个 404：否则可以靠状态码区分
//      "这个邀请 id 存在但不属于我"与"不存在"，从而探测他人邀请。
//   3. 接受邀请的全部校验（未撤销/未过期/未用尽/空间未满/调用者未加入）
//      与写入必须在同一事务内完成——这由 SpaceService::acceptInvite 保证，
//      控制器只做形状校验与错误码映射。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/SpaceService.h"

namespace usapp::controllers {

class InviteController : public drogon::HttpController<InviteController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(InviteController::listInvites, "/api/v1/spaces/current/invites",
                  drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(InviteController::createInvite, "/api/v1/spaces/current/invites",
                  drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(InviteController::revokeInvite,
                  "/api/v1/spaces/current/invites/{inviteId}", drogon::Delete,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(InviteController::acceptInvite, "/api/v1/invites/{code}/accept",
                  drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    // ------------------------------------------------------------------
    // 列出邀请：只回元数据。没有任何字段能反推出邀请码。
    // ------------------------------------------------------------------
    void listInvites(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        SpaceService::instance().listInvites(
            userId,
            [req, callback](SpaceService::Status status,
                            std::vector<SpaceService::InviteInfo> invites) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                Json::Value arr(Json::arrayValue);
                for (const auto &invite : invites) {
                    arr.append(ctrl::inviteJson(invite));
                }
                Json::Value body(Json::objectValue);
                body["invites"] = arr;
                callback(jsonResponse(body, 200));
            });
    }

    // ------------------------------------------------------------------
    // 生成邀请。没有请求体：有效期与最大使用次数由服务端常量决定
    // （SpaceService::kInviteTtlSeconds / kInviteMaxUses），客户端不能要求
    // 更长的有效期。
    // ------------------------------------------------------------------
    void createInvite(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        const AppConfig &cfg = AppConfig::instance();
        SpaceService::instance().createInvite(
            userId,
            [req, callback, &cfg](SpaceService::Status status,
                                  const std::string &rawCode,
                                  const std::string &inviteId,
                                  const std::string &expiresAt) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                Json::Value body(Json::objectValue);
                body["invite"] =
                    ctrl::createdInviteJson(inviteId, expiresAt, rawCode, cfg.publicOrigin);
                callback(jsonResponse(body, 201));
            });
    }

    // ------------------------------------------------------------------
    // 撤销邀请。合并"不存在/不属于我/已撤销/已使用"为同一个 404。
    // ------------------------------------------------------------------
    void revokeInvite(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::string inviteId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        if (!ctrl::looksLikeUuid(inviteId)) {
            callback(ctrl::fail(ErrorCode::SpaceNotFound, "邀请不存在", req));
            return;
        }
        SpaceService::instance().revokeInvite(
            userId, inviteId, [req, callback](SpaceService::Status status) {
                if (!status.ok()) {
                    callback(ctrl::spaceFailure(status, req));
                    return;
                }
                callback(noContentResponse());
            });
    }

    // ------------------------------------------------------------------
    // 接受邀请。非法的码形状在进数据库之前就按"无效邀请"处理，
    // 避免把它当作校验错误（400）而泄露"形状对不对"这一位信息。
    // ------------------------------------------------------------------
    void acceptInvite(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::string rawCode) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) {
            return;
        }
        const std::string normalized = SpaceService::normalizeInviteCode(rawCode);
        if (normalized.empty()) {
            callback(ctrl::fail(ErrorCode::InviteInvalidOrExpired, "邀请无效或已过期", req));
            return;
        }
        SpaceService::instance().acceptInvite(
            userId, normalized, [req, callback](SpaceService::Status status, SpaceInfo space) {
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
