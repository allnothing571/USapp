// =====================================================================
// services/api/src/http/AuthFilter.h
//
// 公开 API（/api/v1/**）的安全前置过滤器。
//
// 它只做"必须对所有路由一致"的两件事，其余交给控制器：
//   1. 状态修改请求：Origin 必须与 PUBLIC_ORIGIN 精确相等（scheme+host+port）。
//      只认 Origin 头，不看 Referer（Referer 可被静默丢弃，Origin 在跨源
//      POST 上必发）。
//   2. 解析会话 Cookie，并把会话状态放进请求属性；状态修改请求必须携带
//      与会话绑定的 CSRF 令牌（X-CSRF-Token），散列一致才放行。
//
// 拒绝顺序刻意是：Origin 先于会话。Origin 检查不查库，而且"跨源 + 未认证"
// 必须报 ORIGIN_NOT_ALLOWED 而不是 UNAUTHENTICATED，否则攻击者能靠响应码
// 区分"这个会话是否有效"。
//
// 请求属性（供控制器读取，键见 authctx）：
//   usapp.session_id    会话行 id（预认证会话也有）
//   usapp.user_id       仅正式会话
//   usapp.authenticated 是否已登录
//   usapp.preauth       是否登录前的预认证会话
//   usapp.expires_at    会话过期时间（ISO8601 UTC）
// =====================================================================
#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/HttpRequest.h>

#include <string>

namespace usapp::authctx {

inline constexpr const char *kSessionId = "usapp.session_id";
inline constexpr const char *kUserId = "usapp.user_id";
inline constexpr const char *kAuthenticated = "usapp.authenticated";
inline constexpr const char *kPreauth = "usapp.preauth";
inline constexpr const char *kExpiresAt = "usapp.expires_at";

// 下面几个读取器在属性缺失/类型不符时一律返回"未认证"/空值，即 fail closed。
bool isAuthenticated(const drogon::HttpRequestPtr &req);
std::string userId(const drogon::HttpRequestPtr &req);
std::string sessionId(const drogon::HttpRequestPtr &req);
bool isPreauth(const drogon::HttpRequestPtr &req);
std::string expiresAt(const drogon::HttpRequestPtr &req);

}  // namespace usapp::authctx

namespace usapp {

// 第二个模板参数 AutoCreation=false：关闭 Drogon 的「自动创建并注册」。
// 原因：本过滤器由 main.cc 显式 registerFilter 注册，控制器侧通过
// AuthFilter::classTypeName() 按名引用。若保留默认的 AutoCreation=true，
// Drogon 会在注册表初始化时自动为该类创建一个实例，与 main.cc 的显式注册
// 叠加，导致同一过滤器出现两个实例（各自持有独立状态），行为不确定。
// 参见 AdminFilter.h 的同样写法。
class AuthFilter : public drogon::HttpFilter<AuthFilter, false> {
  public:
    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&ccb) override;
};

}  // namespace usapp
