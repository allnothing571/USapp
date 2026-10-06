// =====================================================================
// services/api/src/services/AdminAuthService.h
//
// 管理后台认证。与情侣账号体系完全隔离：
//   * 管理员不是 users 行，admin_sessions 也不引用 users。
//   * 管理员身份绝不来自 couple_members.slot 或任何情侣角色。
//   * 独立 Cookie 名（ADMIN_SESSION_COOKIE_NAME，默认 __Host-usapp-admin）
//     与独立 CSRF 令牌，和用户会话互不通用。
//
// 与用户会话一致的安全不变量：
//   * Cookie 里是 64 字符不透明随机令牌；数据库只存 SHA-256。
//   * CSRF 令牌与会话行绑定，数据库只存 SHA-256，比较用恒定时间。
//   * 过期/撤销/查无 = 匿名，绝不延长或复活。
//   * 口令用 PBKDF2-SHA256 校验；账号不存在时也走一次等价开销的假校验，
//     避免用响应时间枚举管理员用户名。
// =====================================================================
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace usapp {

struct AdminSessionRecord {
    std::string id;
    std::string adminUserId;
    std::string username;
    std::string csrfTokenHash;
    std::string expiresAt;  // ISO8601 UTC
    bool expired = false;
    bool revoked = false;
};

class AdminAuthService {
  public:
    // 阶段 0 固定 12 小时。管理员会话比用户会话短得多：管理面虽然只绑回环，
    // 但一旦本机被他人接触，短会话能显著收窄窗口。若日后需要可配置，
    // 再进 AppConfig（不要在这里读环境变量）。
    static constexpr int kAdminSessionTtlSeconds = 12 * 60 * 60;

    using FailCallback = std::function<void(const std::string &)>;

    enum class LoginOutcome {
        Success,
        InvalidCredentials,  // 用户名不存在 / 口令错 / 状态非 active，对外统一
        Validation,
        Error,
    };

    struct LoginResult {
        LoginOutcome outcome = LoginOutcome::Error;
        std::string adminUserId;
        std::string sessionId;
        std::string rawSessionToken;  // 明文只在这一次回调里出现
        std::string rawCsrfToken;
        std::string error;
    };

    static AdminAuthService &instance();

    void login(const std::string &username,
               const std::string &password,
               std::function<void(LoginResult)> done);

    // 找不到 / 已撤销 / 已过期都回调 std::nullopt。
    void loadByRawToken(const std::string &rawToken,
                        std::function<void(std::optional<AdminSessionRecord>)> cb);

    void revokeSession(const std::string &sessionId,
                       const std::string &reason,
                       std::function<void()> onDone,
                       FailCallback onError);

    void touchSession(const std::string &sessionId) noexcept;

    // 轮换管理会话绑定的 CSRF 令牌，回调返回新的明文令牌；会话无效时回调空串。
    // 理由与 SessionService::rotateCsrfToken 相同：库里只有 SHA-256，页面刷新后
    // 只能重新生成一枚交给同一个浏览器。
    void rotateCsrfToken(const std::string &sessionId,
                         std::function<void(const std::string &rawCsrfToken)> onRotated,
                         FailCallback onError);

    static bool verifyCsrf(const AdminSessionRecord &session, const std::string &providedToken);
    static std::string tokenHash(const std::string &rawToken);

    // 用 ADMIN_BOOTSTRAP_USERNAME / ADMIN_BOOTSTRAP_PASSWORD_HASH 引导管理员账号。
    // 两者任一为空则什么都不做（本地开发默认如此）。
    // 已存在同名管理员时【覆盖】其 password_hash —— 这是有意的：负责人改环境变量
    // 后重启即可轮换口令，不需要手工改库。明文口令永不入库、永不进镜像、永不进日志。
    void syncBootstrapAdmin(std::function<void(bool)> onDone) noexcept;

  private:
    AdminAuthService() = default;
};

}  // namespace usapp
