// =====================================================================
// services/api/src/controllers/RewardController.cc
//
// 阶段 1 M7：奖励商店商品的上架、列表、详情、修改与下架（契约 0.11.0）。
// 所有路由都挂 AuthFilter：写请求由过滤器校验 Origin + 会话绑定的 CSRF，
// 读请求只要求正式会话。用户只取自会话；空间与商品的拥有者（对方）由服务端推导。
// 阶段 1 M8/M9：直接兑换、申请确认、协商取消退款与履约（契约 0.13.0）。
// =====================================================================
#include <drogon/HttpController.h>

#include <functional>
#include <optional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/ProductService.h"
#include "services/RedemptionService.h"
#include "services/TaskCompletionService.h"
#include "services/TaskService.h"

namespace usapp::controllers {

class RewardController : public drogon::HttpController<RewardController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RewardController::createProduct, "/api/v1/rewards/products", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::listProducts, "/api/v1/rewards/products", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::getProduct, "/api/v1/rewards/products/{productId}", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::editProduct, "/api/v1/rewards/products/{productId}/revisions", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::archiveProduct, "/api/v1/rewards/products/{productId}/archive", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::redeemProduct, "/api/v1/rewards/products/{productId}/redemptions", drogon::Post,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::listRedemptions, "/api/v1/rewards/redemptions", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::getRedemption, "/api/v1/rewards/redemptions/{redemptionId}", drogon::Get,
                  AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::fulfillRedemption, "/api/v1/rewards/redemptions/{redemptionId}/fulfillment",
                  drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(RewardController::transitionRedemption, "/api/v1/rewards/redemptions/{redemptionId}/transitions",
                  drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    // M8: the product owner redeems the version it was shown (direct mode only).
    void redeemProduct(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string productId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(productId)) {
            callback(ctrl::fail(ErrorCode::ProductNotFound, "奖励不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        int expectedVersion = 0;
        std::string message;
        if (!RedemptionService::parseRedeemRequest(body, expectedVersion, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        RedemptionService::instance().redeem(userId, productId, key, expectedVersion,
                                             [req, callback](RedemptionService::Status status) {
                                                 respondRedemption(req, callback, status, "兑换奖励");
                                             });
    }

    void listRedemptions(const drogon::HttpRequestPtr &req,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<RedemptionService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            cursor = RedemptionService::decodeCursor(rawCursor);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        RedemptionService::instance().list(
            userId, *limit, cursor, [req, callback](RedemptionService::ListResult result) {
                if (result.outcome != RedemptionService::Outcome::Ok) {
                    RedemptionService::Status status;
                    status.outcome = result.outcome;
                    status.message = result.message;
                    status.detail = result.detail;
                    respondRedemption(req, callback, status, "读取兑换记录");
                    return;
                }
                Json::Value body(Json::objectValue);
                Json::Value items(Json::arrayValue);
                for (const auto &d : result.redemptions) items.append(RedemptionService::redemptionJson(d));
                body["redemptions"] = items;
                body["next_cursor"] =
                    result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
                callback(jsonResponse(body, 200));
            });
    }

    void getRedemption(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string redemptionId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(redemptionId)) {
            callback(ctrl::fail(ErrorCode::RedemptionNotFound, "兑换记录不存在", req));
            return;
        }
        RedemptionService::instance().get(userId, redemptionId, [req, callback](RedemptionService::Status status) {
            if (!status.success()) {
                respondRedemption(req, callback, status, "读取兑换详情");
                return;
            }
            Json::Value body(Json::objectValue);
            body["redemption"] = RedemptionService::redemptionJson(status.redemption);
            callback(jsonResponse(body, 200));
        });
    }

    // M8: the product creator marks a pending redemption fulfilled (final, no charge).
    void fulfillRedemption(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string redemptionId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(redemptionId)) {
            callback(ctrl::fail(ErrorCode::RedemptionNotFound, "兑换记录不存在", req));
            return;
        }
        const auto body = req->body();
        if (!TaskCompletionService::isEmptyCompletionBody(std::string(body.data(), body.size()))) {
            callback(ctrl::fail(ErrorCode::ValidationError, "确认履约的请求体必须为空", req));
            return;
        }
        RedemptionService::instance().fulfill(userId, redemptionId, key,
                                              [req, callback](RedemptionService::Status status) {
                                                  respondRedemption(req, callback, status, "确认履约");
                                              });
    }

    void transitionRedemption(const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string redemptionId) {
        std::string userId,key;
        if(!prepareMutation(req,callback,userId,key)) return;
        if(!ctrl::looksLikeUuid(redemptionId)) { callback(ctrl::fail(ErrorCode::RedemptionNotFound,"兑换记录不存在",req)); return; }
        Json::Value body;
        if(!ctrl::parseBody(req,callback,body)) return;
        if(!body.isObject() || !body["action"].isString() ||
           !RedemptionService::validTransition(body["action"].asString()) ||
           (body.isMember("proposal_id") && !body["proposal_id"].isString())) {
            callback(ctrl::fail(ErrorCode::ValidationError,"操作参数不合法",req)); return;
        }
        for(const auto &name:body.getMemberNames()) {
            if(name!="action" && name!="proposal_id") { callback(ctrl::fail(ErrorCode::ValidationError,"请求包含未知字段",req)); return; }
        }
        RedemptionService::instance().transition(userId,redemptionId,key,body["action"].asString(),
            body.isMember("proposal_id") ? body["proposal_id"].asString() : "",
            [req,callback](RedemptionService::Status status) { respondRedemption(req,callback,status,"处理兑换"); });
    }

    void createProduct(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        ProductService::Terms terms;
        std::string message;
        if (!ProductService::parseCreateRequest(body, terms, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        ProductService::instance().create(userId, key, terms, [req, callback](ProductService::Status status) {
            respond(req, callback, status, "上架奖励");
        });
    }

    void listProducts(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        if (!limit) {
            callback(ctrl::fail(ErrorCode::ValidationError, "limit 必须是 1 到 100 的整数", req));
            return;
        }
        std::optional<ProductService::Cursor> cursor;
        const std::string rawCursor = req->getParameter("cursor");
        if (!rawCursor.empty()) {
            // An invalid cursor is a 400 without any query.
            cursor = ProductService::decodeCursor(rawCursor);
            if (!cursor) {
                callback(ctrl::fail(ErrorCode::ValidationError, "分页游标无效，请从第一页重新加载", req));
                return;
            }
        }
        ProductService::instance().list(userId, *limit, cursor, [req, callback](ProductService::ListResult result) {
            if (result.outcome != ProductService::Outcome::Ok) {
                ProductService::Status status;
                status.outcome = result.outcome;
                status.message = result.message;
                status.detail = result.detail;
                respond(req, callback, status, "读取奖励列表");
                return;
            }
            Json::Value body(Json::objectValue);
            Json::Value products(Json::arrayValue);
            for (const auto &product : result.products) products.append(ProductService::productJson(product));
            body["products"] = products;
            body["next_cursor"] = result.nextCursor ? Json::Value(*result.nextCursor) : Json::Value(Json::nullValue);
            callback(jsonResponse(body, 200));
        });
    }

    void getProduct(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string productId) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        if (!ctrl::looksLikeUuid(productId)) {
            callback(ctrl::fail(ErrorCode::ProductNotFound, "奖励不存在", req));
            return;
        }
        ProductService::instance().get(userId, productId, [req, callback](ProductService::Status status) {
            if (!status.success()) {
                respond(req, callback, status, "读取奖励详情");
                return;
            }
            Json::Value body(Json::objectValue);
            body["product"] = ProductService::productJson(status.product);
            callback(jsonResponse(body, 200));
        });
    }

    void editProduct(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string productId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(productId)) {
            callback(ctrl::fail(ErrorCode::ProductNotFound, "奖励不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        ProductService::Terms terms;
        int expectedVersion = 0;
        std::string message;
        if (!ProductService::parseEditRequest(body, terms, expectedVersion, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        ProductService::instance().edit(userId, productId, key, expectedVersion, terms,
                                        [req, callback](ProductService::Status status) {
                                            respond(req, callback, status, "修改奖励");
                                        });
    }

    void archiveProduct(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::string productId) {
        std::string userId;
        std::string key;
        if (!prepareMutation(req, callback, userId, key)) return;
        if (!ctrl::looksLikeUuid(productId)) {
            callback(ctrl::fail(ErrorCode::ProductNotFound, "奖励不存在", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) return;
        int expectedVersion = 0;
        std::string message;
        if (!ProductService::parseArchiveRequest(body, expectedVersion, message)) {
            callback(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        ProductService::instance().archive(userId, productId, key, expectedVersion,
                                           [req, callback](ProductService::Status status) {
                                               respond(req, callback, status, "下架奖励");
                                           });
    }

  private:
    static void respondRedemption(const drogon::HttpRequestPtr &req,
                                  const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                  const RedemptionService::Status &status, const char *action) {
        if (status.success()) {
            callback(jsonResponse(RedemptionService::responseBody(status),
                                  RedemptionService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == RedemptionService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复扣除", req));
            return;
        }
        if (status.outcome == RedemptionService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(RedemptionService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }

    static bool prepareMutation(const drogon::HttpRequestPtr &req,
                                const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                                std::string &userId, std::string &key) {
        if (!ctrl::requireUser(req, callback, userId)) return false;
        key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            callback(ctrl::fail(ErrorCode::ValidationError,
                                "缺少或无效的 Idempotency-Key 请求头（16-128 个字母、数字、- 或 _）", req));
            return false;
        }
        return true;
    }

    static void respond(const drogon::HttpRequestPtr &req,
                        const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                        const ProductService::Status &status, const char *action) {
        if (status.success()) {
            callback(jsonResponse(ProductService::responseBody(status), ProductService::httpStatusFor(status.outcome)));
            return;
        }
        if (status.outcome == ProductService::Outcome::ResultUnknown) {
            LOG_ERROR << action << "：提交结果未确认 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用同一请求重试，不会重复处理", req));
            return;
        }
        if (status.outcome == ProductService::Outcome::Error) {
            LOG_ERROR << action << "失败 detail=" << status.detail << " request_id=" << http::requestId(req);
            callback(ctrl::fail(ErrorCode::InternalError, "服务内部错误", req));
            return;
        }
        callback(ctrl::fail(ProductService::errorCodeFor(status.outcome),
                            status.message.empty() ? std::string("请求无法完成") : status.message, req));
    }
};

}  // namespace usapp::controllers
