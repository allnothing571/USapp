// =====================================================================
// services/api/src/mailer/Mailer.h
//
// 邮件发送。阶段 0 只发一种邮件：登录验证码。
//
// 安全与并发约束：
//   * 发送是【网络操作】。绝不允许在持锁事务（pg_try_advisory_xact_lock
//     所在的那个事务）里调用 Mailer —— 见 scheduler/Scheduler.h 的
//     afterCommit 约定。AuthService 的做法是：事务提交拿到明文码之后，
//     由控制器调用 Mailer。
//   * MAIL_MODE=log 只写日志、不投递，且 AppConfig 已强制
//     「生产环境 + log 模式」直接拒绝启动，因此明文验证码不会出现在
//     生产日志里。
//   * 验证码只出现在日志与邮件正文中，绝不写数据库（库里只有 sha256）。
//   * 不对明文 SMTP 降级连接：TLS 校验永远开启，不提供"跳过证书校验"开关。
// =====================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace usapp::mailer {

struct SendResult {
    bool ok = false;
    // 失败原因的短描述。绝不含验证码、密码或收件人之外的隐私。
    std::string error;
};

class Mailer {
  public:
    static Mailer &instance();

    // 发送登录验证码。回调一定被调用一次，且一定在 Drogon 事件循环线程上
    // （若事件循环不可用则同步调用）。
    void sendVerificationCode(const std::string &toEmail, const std::string &code, int ttlSeconds,
                              std::function<void(SendResult)> done);

    // 供测试与手动排查：当前是否真的会投递邮件（MAIL_MODE=smtp）。
    bool deliveringRealMail() const;

    // 供测试注入：替换实际传输层。传空函数恢复默认行为。
    // 生产代码不要调用它。
    using Transport = std::function<SendResult(const std::string &rawMessage)>;
    void setTransportForTest(Transport transport);

  private:
    Mailer() = default;

    // 实际投递。在后台线程执行，绝不在调用者线程阻塞事件循环。
    SendResult deliver(const std::string &toEmail, const std::string &code, int ttlSeconds,
                       const std::string &rawMessage);
    static void postToLoop(SendResult result, const std::function<void(SendResult)> &done);
    static std::string buildMessage(const std::string &toEmail, const std::string &code,
                                    int ttlSeconds);
    static bool containsCrLf(const std::string &value);
    static SendResult sendViaSmtp(const std::string &toEmail, const std::string &rawMessage);
    static SendResult writeToLog(const std::string &toEmail, const std::string &code,
                                 int ttlSeconds);
};

}  // namespace usapp::mailer
