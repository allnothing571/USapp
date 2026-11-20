// =====================================================================
// services/api/src/config/AppConfig.h
//
// 进程级配置。全部来自环境变量；密钥不落代码库、不落镜像。
// 启动时校验一次，不合法直接退出（fail fast），不做静默降级。
// =====================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace usapp {

class AppConfig {
  public:
    static AppConfig &instance();

    // 从环境变量加载并校验。失败抛 std::runtime_error。
    void load();
    bool loaded() const { return loaded_; }

    // ---- 运行环境 ----
    std::string appEnv;      // development | staging | production
    std::string appVersion;
    bool isProduction() const { return appEnv == "production"; }

    // ---- 同源与监听 ----
    std::string publicOrigin;        // 例 https://localhost:8443，精确匹配 Origin 用
    std::string adminOrigin;         // 例 https://localhost:8444
    std::string publicBind;          // 0.0.0.0
    uint16_t publicPort;             // 8443（由 Caddy/容器转发；本进程按需直接监听）
    std::string adminBind;           // 必须回环或私网
    uint16_t adminPort;

    // ---- 管理面边界 ----
    // 主机层已把管理端口只发布到 127.0.0.1；应用层再做一次 CIDR 白名单，
    // 防止误把管理端口暴露到公网网卡。
    std::vector<std::string> adminAllowedCidrs;

    // ---- 数据库 ----
    std::string postgresHost;
    uint16_t postgresPort;
    std::string postgresDb;
    std::string postgresUser;
    std::string postgresPassword;
    uint32_t postgresPoolSize;
    std::string postgresConnectionString() const;

    // ---- 注册策略 ----
    std::string registrationMode;  // whitelist | open
    std::unordered_set<std::string> allowedEmails;  // 已规范化
    bool isWhitelistMode() const { return registrationMode == "whitelist"; }

    // ---- 验证码 ----
    int emailCodeTtlSeconds;
    int emailCodeMaxAttempts;
    int emailCodeResendCooldownSeconds;
    int emailCodeHourlyLimit;
    int emailCodeDigits;

    // ---- 会话 ----
    std::string sessionCookieName;       // __Host-usapp-session
    std::string adminSessionCookieName;  // __Host-usapp-admin
    int preauthSessionTtlSeconds;
    int authSessionTtlSeconds;
    std::string sessionSecret;  // 预留：未来用于令牌派生；当前仅要求存在且足够长

    // ---- 邮件 ----
    // 变量名与仓库根 .env.example 一一对应（SMTP_HOST/SMTP_PORT/SMTP_USERNAME/
    // SMTP_PASSWORD/SMTP_FROM/SMTP_STARTTLS）。改名必须同时改 .env.example、
    // deploy/docker-compose.yml 与 services/api/README.md。
    std::string mailMode;  // log | smtp
    std::string mailFrom;
    std::string smtpHost;  // 不含 scheme 的主机名，端口单独给出
    uint16_t smtpPort;     // 587（STARTTLS）| 465（隐式 TLS）| 25
    std::string smtpUser;
    std::string smtpPassword;
    bool smtpStartTls;
    int mailTimeoutSeconds;

    // 隐式 TLS（smtps，典型端口 465）与 STARTTLS（明文端口升级，典型端口 587）的区别：
    // 前者连接即加密，后者先明文握手再升级。Mailer 需要据此选择 libcurl 的选项。
    bool smtpImplicitTls() const { return smtpPort == 465; }

    // ---- 调度 ----
    bool schedulerEnabled;
    bool remindersEnabled;  // P2-6 R2: REMINDERS_ENABLED, default false
    bool memoryPurgeEnabled;  // D6 M1d: MEMORY_PURGE_ENABLED, default false. Irreversible; see docs/phase3-d6-memory-deletion-design.md Section 9
    int schedulerTickIntervalMs;
    std::string schedulerInstanceId;
    std::string schedulerKeyNamespace;

    // ---- 管理引导 ----
    std::string adminBootstrapUsername;
    std::string adminBootstrapPasswordHash;

    // 精确 Origin 匹配（scheme + host + port）。
    // 不使用 StartsWith / 后缀匹配：`https://evil.com` 不得因为 `https://localhost:8443`
    // 被当作可信来源。
    bool isAllowedPublicOrigin(const std::string &origin) const;
    bool isAllowedAdminOrigin(const std::string &origin) const;

  private:
    AppConfig() = default;
    bool loaded_ = false;
};

}  // namespace usapp
