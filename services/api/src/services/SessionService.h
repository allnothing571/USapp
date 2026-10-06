// =====================================================================
// services/api/src/services/SessionService.h
//
// 会话与 CSRF 的数据库侧实现。
//
// 设计要点：
//   * Cookie 里放的是 64 字符不透明随机令牌；数据库只保存 SHA-256。
//     数据库泄露无法直接冒充用户。
//   * CSRF 令牌同样是随机值，数据库只保存 SHA-256，与会话行一一绑定。
//     客户端从 GET /api/v1/session 拿到明文，放在 X-CSRF-Token 头里回传。
//   * 预认证会话（is_authenticated=false, user_id IS NULL）与正式会话
//     （is_authenticated=true, user_id NOT NULL）用同一张表与同一个 Cookie，
//     区别只在会话行的状态；登录成功时【新建】正式会话并撤销旧的预认证会话，
//     因此会话标识与 CSRF 令牌都会更换（session fixation 防护）。
//   * 过期/撤销/查无 = 匿名，绝不延长或复活。
// =====================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace usapp {

struct SessionRecord {
    std::string id;
    std::string userId;  // 预认证会话为空
    bool authenticated = false;
    std::string csrfTokenHash;
    std::string expiresAt;  // ISO8601 UTC
    bool expired = false;
    bool revoked = false;
};

using SessionCallback = std::function<void(std::optional<SessionRecord>)>;

class SessionService {
  public:
    static SessionService &instance();

    // 按明文令牌查找会话。找不到 / 已撤销 / 已过期都回调 std::nullopt。
    void loadByRawToken(const std::string &rawToken, SessionCallback cb);

    // 新建预认证会话。回调返回 (会话id, 明文令牌, 明文CSRF令牌)。
    using CreatedCallback =
        std::function<void(const std::string &sessionId,
                           const std::string &rawToken,
                           const std::string &rawCsrfToken)>;
    using FailCallback = std::function<void(const std::string &)>;

    void createPreauthSession(const std::string &userAgentFamily,
                              CreatedCallback onCreated,
                              FailCallback onError);

    // 新建正式会话（登录成功时调用）。previousSessionId 若非空，则在
    // 同一事务内撤销它，保证“换会话标识 + 换 CSRF 令牌”是原子的。
    void createAuthenticatedSession(const std::string &userId,
                                    const std::string &previousSessionId,
                                    const std::string &userAgentFamily,
                                    CreatedCallback onCreated,
                                    FailCallback onError);

    // 轮换会话绑定的 CSRF 令牌，回调返回**新的明文** CSRF 令牌。
    //
    // 为什么需要它：数据库只保存 CSRF 令牌的 SHA-256，明文不可反推，而
    // GET /api/v1/session 必须在页面刷新（内存里的令牌已丢）后仍能把令牌
    // 交给同一个浏览器，所以只能重新生成一枚并覆盖旧散列。
    //
    // 代价（已知且刻意接受，阶段 0）：同一个会话在多个标签页里各自引导一次
    // 会互相顶掉令牌，旧标签页的下一个写请求会得到 403 CSRF_TOKEN_INVALID。
    // 前端对 403 CSRF_TOKEN_INVALID 的处理是「重新引导一次并把该请求重试且
    // 只重试一次」。若要根治，需要把明文令牌落库或改成双令牌窗口，那是
    // 阶段 1 的议题，本阶段不做。
    //
    // 会话不存在/已撤销/已过期时回调空字符串（调用方按缺少会话处理）。
    using CsrfRotatedCallback = std::function<void(const std::string &rawCsrfToken)>;
    void rotateCsrfToken(const std::string &sessionId,
                         CsrfRotatedCallback onRotated,
                         FailCallback onError);

    // 撤销会话（注销）。找不到也算成功（幂等）。
    void revokeSession(const std::string &sessionId,
                       const std::string &reason,
                       std::function<void()> onDone,
                       FailCallback onError);

    // 更新 last_seen_at（尽力而为，失败不影响请求）。
    void touchSession(const std::string &sessionId) noexcept;

    // CSRF 校验：恒定时间比较 SHA-256。
    static bool verifyCsrf(const SessionRecord &session, const std::string &providedToken);

    // 从 User-Agent 派生粗粒度家族标签（不保存完整 UA，减少隐私面）。
    static std::string userAgentFamily(const std::string &userAgent);

    // 会话令牌的数据库散列。
    static std::string tokenHash(const std::string &rawToken);

  private:
    SessionService() = default;
};

}  // namespace usapp
