// =====================================================================
// services/api/src/services/AuthService.h
//
// 邮箱验证码登录。
//
// 流程（全部在一个数据库事务里完成，见 .cc 中的行锁说明）：
//   1. 发送：POST /auth/email-codes
//        - 同一邮箱冷却期内重复请求 -> RateLimited（对外仍回 202，防枚举）
//        - 同一邮箱一小时内超过上限 -> RateLimited
//        - 之前的未消费验证码标记为 superseded（部分唯一索引要求）
//        - 事务提交后再发信：网络操作绝不占用数据库事务
//   2. 登录：POST /auth/sessions
//        - SELECT ... FOR UPDATE 锁住验证码行，避免并发重放
//        - 校验 attempts / expires_at / 散列
//        - 白名单只在这一步生效（此时才知道是不是新账号）
//        - 插入或取回用户
//        - 消费验证码
//        - 撤销旧的预认证会话并新建正式会话（同一事务 -> 会话标识与
//          CSRF 令牌一起更换，没有中间态）
//
// 防账号枚举：发送阶段无论邮箱是否允许注册、是否已存在，返回一致。
//             只有拿着正确验证码时才会得到 REGISTRATION_NOT_ALLOWED。
// =====================================================================
#pragma once

#include <functional>
#include <string>

namespace usapp {

class AuthService {
  public:
    static AuthService &instance();

    // ---- 发送验证码 ----
    enum class SendOutcome {
        Accepted,   // 新码已生成（对外一律 202）
        Throttled,  // 冷却或频率上限（对外也回 202，但不会真发信）
        Error,
    };
    struct SendResult {
        SendOutcome outcome = SendOutcome::Error;
        std::string code;   // 明文验证码，仅用于交给 Mailer；绝不入库、绝不进日志
        std::string error;  // 内部错误描述（只写日志）
    };
    // emailCanonical 必须是已规范化的邮箱；sessionId 是发起请求的预认证会话。
    void sendCode(const std::string &emailCanonical,
                  const std::string &sessionId,
                  std::function<void(SendResult)> done);

    // ---- 用验证码登录 ----
    enum class LoginOutcome {
        Success,
        CodeInvalidOrExpired,
        TooManyAttempts,
        RegistrationNotAllowed,
        NoSession,
        Error,
    };
    struct LoginResult {
        LoginOutcome outcome = LoginOutcome::Error;
        std::string userId;
        std::string sessionId;
        std::string rawSessionToken;
        std::string rawCsrfToken;
        std::string error;
    };
    void loginWithCode(const std::string &emailCanonical,
                       const std::string &code,
                       const std::string &preauthSessionId,
                       const std::string &userAgentFamily,
                       std::function<void(LoginResult)> done);

  private:
    AuthService() = default;
};

}  // namespace usapp
