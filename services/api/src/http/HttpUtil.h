// =====================================================================
// services/api/src/http/HttpUtil.h
//
// HTTP 层小工具：Cookie 构造、Origin 判定、CSRF 判定、客户端地址、
// 时间格式化、CIDR 匹配、请求 ID。
//
// 安全不变量（本文件是它们的唯一实现处，不要在别处另写一份）：
//   1. 会话 Cookie 永远带 Secure + HttpOnly + SameSite=Lax + Path=/，
//      永远不设置 Domain（__Host- 前缀要求）。
//   2. 状态修改方法（非 GET/HEAD/OPTIONS）必须同时满足：
//      Origin 精确匹配 && CSRF 令牌与会话绑定且散列一致。
//   3. 任何“连不上/查不到”都按拒绝处理（fail closed）。
// =====================================================================
#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace usapp::http {

// ---- Cookie ----
std::string readCookie(const drogon::HttpRequestPtr &req, const std::string &name);

// 会话 Cookie：Secure + HttpOnly + SameSite=Lax + Path=/，无 Domain。
void setSessionCookie(const drogon::HttpResponsePtr &resp,
                      const std::string &name,
                      const std::string &value,
                      int maxAgeSeconds);

// 清除 Cookie（Max-Age=0），属性与上面完全一致，否则浏览器不会覆盖旧值。
void clearSessionCookie(const drogon::HttpResponsePtr &resp,
                        const std::string &name);

// ---- 安全判定 ----
bool isStateChangingMethod(const drogon::HttpMethod &method);

// 读取 Origin 头（原样返回，空串表示缺失）。
std::string requestOrigin(const drogon::HttpRequestPtr &req);

// 状态修改请求是否允许跨源（false -> 必须拒绝）。
bool originMatches(const std::string &requestOriginHeader, const std::string &expectedOrigin);

// 是否要求 CSRF 校验。GET/HEAD/OPTIONS 不要求（它们是安全方法）。
bool requiresCsrf(const drogon::HttpMethod &method);

// ---- 客户端地址与 CIDR ----
std::string peerIp(const drogon::HttpRequestPtr &req);
bool ipInCidr(const std::string &ip, const std::string &cidr);
bool ipInAnyCidr(const std::string &ip, const std::vector<std::string> &cidrs);

// ---- 时间与标识 ----
std::string nowIso8601Utc();
std::string toIso8601Utc(std::time_t t);
std::string requestId(const drogon::HttpRequestPtr &req);

// 从 JSON body 里安全取字符串字段（缺失或非字符串都返回空串）。
std::string jsonString(const Json::Value &body, const std::string &key);

// 校验 body 是 JSON 对象；不是则返回 false（调用方回 VALIDATION_ERROR）。
bool isJsonObject(const drogon::HttpRequestPtr &req, Json::Value &out);

}  // namespace usapp::http
