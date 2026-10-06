// =====================================================================
// services/api/src/http/AdminFilter.h
//
// 管理 API（/api/admin/v1/**）的前置过滤器。
//
// 与 AuthFilter 的关键差别：管理面没有"预认证会话"这个概念
// （admin_sessions.admin_user_id 是 NOT NULL），所以登录接口必须在
// "没有会话"的前提下也能被访问。它的 CSRF 防护由下面三层共同承担：
//   1. 网络层：管理端口只发布到 127.0.0.1（deploy/docker-compose.yml），
//      公开站点不路由 /api/admin/*（deploy/Caddyfile）。
//   2. 应用层：TCP 对端地址必须落在 ADMIN_ALLOWED_CIDRS 内；不匹配一律
//      按 404 处理（不泄露"这里有一个管理入口"）。
//   3. 来源层：状态修改请求必须精确匹配 ADMIN_ORIGIN，且必须带
//      X-Requested-By: usapp-admin —— 跨站表单无法设置自定义头，
//      而我们对跨源请求永不返回 CORS 放行头，浏览器不会发出预检通过后的请求。
//   4. 入口层（关键，且不依赖任何反向代理配置正确）：请求必须带
//      X-Usapp-Admin-Surface: usapp-admin，且*所有*管理请求都要求它。
//      这个头由管理站点（deploy/Caddyfile 的 8444 站点）注入，公网站点
//      显式剥除客户端伪造的同名头。它闭合了"公开监听器上同样注册着
//      管理路由"这个洞：即使有人把 /api/admin/* 错配到公开端口，
//      也拿不到这个头。
//      它是纵深防御的一层，不是唯一一层：真正的边界仍是"管理端口只发布到
//      宿主回环 + 对端地址落在 ADMIN_ALLOWED_CIDRS 内"。
//
// 登录成功之后的所有管理写请求，除上述各层外还必须通过
// admin 会话绑定的 CSRF 令牌（X-CSRF-Token）。
//
// 认证（默认拒绝）：上述各层只限制"从哪里来"，不证明"是谁"。除下列精确
// "方法 + 路径"白名单外，所有管理请求——包括用户 / 空间 / 审计日志列表这类
// 受保护的只读请求——都必须持有有效的管理员会话；缺失 Cookie 或会话无效 /
// 过期 / 已撤销时返回 401 UNAUTHENTICATED，且不进入控制器：
//   GET  /api/admin/v1/health    存活探针
//   GET  /api/admin/v1/session   未登录引导（authenticated=false，csrf_token 为空串）
//   POST /api/admin/v1/sessions  登录（仍需上面的 Origin + 自定义头）
// 白名单不做前缀匹配，也不跳过对端地址、入口标记或登录来源校验。
// 注销 DELETE /api/admin/v1/sessions 不在白名单内。
//
// 请求属性（供控制器读取）：
//   admin.session_id / admin.user_id / admin.username / admin.authenticated
// =====================================================================
#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/HttpRequest.h>

#include <string>

namespace usapp::adminctx {

inline constexpr const char *kSessionId = "admin.session_id";
inline constexpr const char *kUserId = "admin.user_id";
inline constexpr const char *kUsername = "admin.username";
inline constexpr const char *kAuthenticated = "admin.authenticated";

// 自定义头的固定值，配合精确 Origin 一起构成登录接口的 CSRF 防护。
inline constexpr const char *kRequestedByHeader = "X-Requested-By";
inline constexpr const char *kRequestedByValue = "usapp-admin";

// 管理面入口标记：由管理站点（8444）注入，公网站点剥除。
inline constexpr const char *kSurfaceHeader = "X-Usapp-Admin-Surface";
inline constexpr const char *kSurfaceValue = "usapp-admin";

bool isAuthenticated(const drogon::HttpRequestPtr &req);
std::string userId(const drogon::HttpRequestPtr &req);
std::string username(const drogon::HttpRequestPtr &req);
std::string sessionId(const drogon::HttpRequestPtr &req);

}  // namespace usapp::adminctx

namespace usapp {

// 第二个模板参数 AutoCreation=false：关闭 Drogon 的「自动创建并注册」。
// 原因：本过滤器由 main.cc 显式 registerFilter 注册，控制器侧通过
// AdminFilter::classTypeName() 按名引用。若保留默认的 AutoCreation=true，
// Drogon 会在注册表初始化时自动为该类创建一个实例，与 main.cc 的显式注册
// 叠加，导致同一过滤器出现两个实例（各自持有独立状态），行为不确定。
// 参见 AuthFilter.h 的同样写法。
class AdminFilter : public drogon::HttpFilter<AdminFilter, false> {
  public:
    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&ccb) override;
};

}  // namespace usapp
