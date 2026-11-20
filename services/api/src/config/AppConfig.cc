// =====================================================================
// services/api/src/config/AppConfig.cc
// =====================================================================
#include "config/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/Crypto.h"
#include "core/Env.h"

namespace usapp {

namespace {

std::string lowerCopy(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool isAllDigits(const std::string &s) {
    if (s.empty()) {
        return false;
    }
    return s.find_first_not_of("0123456789") == std::string::npos;
}

// 规范化 origin 为 "scheme://host[:port]"，host 小写、端口显式默认化。
//
// 为什么必须规范化：
//   Origin 是精确匹配的凭据（exact-match Origin）。若只做前缀/后缀比较，
//   `https://localhost:8443.evil.com` 之类的构造就可能绕过 CSRF 防线。
//   规范化后仍使用 == 比较，不做任何模糊匹配。
std::string normalizeOrigin(const std::string &raw, const std::string &name, bool allowHttp) {
    const auto schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos) {
        throw std::runtime_error(name + " 必须是完整 origin（含 scheme）: " + raw);
    }
    const std::string scheme = lowerCopy(raw.substr(0, schemeEnd));
    if (scheme != "https" && !(allowHttp && scheme == "http")) {
        throw std::runtime_error(name + " 只允许 " +
                                 (allowHttp ? "http/https" : "https") + ": " + raw);
    }

    std::string rest = raw.substr(schemeEnd + 3);
    if (rest.find('/') != std::string::npos || rest.find('?') != std::string::npos ||
        rest.find('#') != std::string::npos) {
        throw std::runtime_error(name + " 不能包含路径或查询串: " + raw);
    }
    if (rest.empty()) {
        throw std::runtime_error(name + " 缺少主机名: " + raw);
    }

    // 拒绝 userinfo（https://user@host 形式不属于合法 Origin）
    if (rest.find('@') != std::string::npos) {
        throw std::runtime_error(name + " 不能包含 userinfo: " + raw);
    }

    std::string host = rest;
    std::string port;
    if (host.front() == '[') {  // IPv6 字面量
        const auto close = host.find(']');
        if (close == std::string::npos) {
            throw std::runtime_error(name + " 的 IPv6 字面量不合法: " + raw);
        }
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') {
                throw std::runtime_error(name + " 的端口分隔符不合法: " + raw);
            }
            port = host.substr(close + 2);
        }
        host = host.substr(0, close + 1);
    } else {
        const auto colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
    }
    host = lowerCopy(host);

    if (!port.empty()) {
        if (!isAllDigits(port)) {
            throw std::runtime_error(name + " 的端口不是数字: " + raw);
        }
        const int p = std::stoi(port);
        if (p <= 0 || p > 65535) {
            throw std::runtime_error(name + " 的端口越界: " + raw);
        }
    } else {
        port = (scheme == "https") ? "443" : "80";
    }

    return scheme + "://" + host + ":" + port;
}

}  // namespace

AppConfig &AppConfig::instance() {
    static AppConfig cfg;
    return cfg;
}

void AppConfig::load() {
    appEnv = lowerCopy(env::get("APP_ENV", "development"));
    appVersion = env::get("APP_VERSION", "0.1.0");
    const bool allowHttp = appEnv != "production";

    publicOrigin = normalizeOrigin(env::get("PUBLIC_ORIGIN", "https://localhost:8443"),
                                   "PUBLIC_ORIGIN",
                                   allowHttp);
    adminOrigin = normalizeOrigin(env::get("ADMIN_ORIGIN", "https://localhost:8444"),
                                  "ADMIN_ORIGIN",
                                  allowHttp);

    publicBind = env::get("API_PUBLIC_BIND", "0.0.0.0");
    publicPort = static_cast<uint16_t>(env::getInt("API_PUBLIC_PORT", 8443));
    adminBind = env::get("API_ADMIN_BIND", "127.0.0.1");
    adminPort = static_cast<uint16_t>(env::getInt("API_ADMIN_PORT", 8081));
    if (publicPort == 0 || adminPort == 0) {
        throw std::runtime_error("API_PUBLIC_PORT / API_ADMIN_PORT 不能为 0");
    }
    if (publicPort == adminPort) {
        throw std::runtime_error("公共端口与管理端口不能相同");
    }

    adminAllowedCidrs = env::getList("ADMIN_ALLOWED_CIDRS");
    if (adminAllowedCidrs.empty()) {
        adminAllowedCidrs.push_back("127.0.0.1/32");
        adminAllowedCidrs.push_back("::1/128");
    }

    postgresHost = env::get("POSTGRES_HOST", "127.0.0.1");
    postgresPort = static_cast<uint16_t>(env::getInt("POSTGRES_PORT", 5432));
    postgresDb = env::get("POSTGRES_DB", "usapp");
    postgresUser = env::get("POSTGRES_USER", "usapp_api");
    postgresPassword = env::require("POSTGRES_PASSWORD");
    postgresPoolSize = static_cast<uint32_t>(env::getInt("POSTGRES_POOL_SIZE", 8));
    if (postgresPoolSize == 0 || postgresPoolSize > 64) {
        throw std::runtime_error("POSTGRES_POOL_SIZE 必须在 1..64 之间");
    }

    registrationMode = lowerCopy(env::get("REGISTRATION_MODE", "whitelist"));
    if (registrationMode != "whitelist" && registrationMode != "open") {
        throw std::runtime_error("REGISTRATION_MODE 只能是 whitelist 或 open");
    }
    const auto rawEmails = env::getList("ALLOWED_EMAILS");
    for (const auto &raw : rawEmails) {
        crypto::CanonicalEmail parsed;
        if (!crypto::canonicalizeEmail(raw, parsed)) {
            throw std::runtime_error("ALLOWED_EMAILS 中存在非法邮箱: " + raw);
        }
        allowedEmails.insert(parsed.canonical);
    }
    if (registrationMode == "whitelist" && allowedEmails.empty()) {
        // 白名单模式但名单为空 = 任何人都注册不了。明确失败，避免上线后才发现。
        throw std::runtime_error(
            "REGISTRATION_MODE=WHITELIST 但 ALLOWED_EMAILS 为空：请配置允许注册的邮箱");
    }

    emailCodeTtlSeconds = static_cast<int>(env::getInt("EMAIL_CODE_TTL_SECONDS", 600));
    emailCodeMaxAttempts = static_cast<int>(env::getInt("EMAIL_CODE_MAX_ATTEMPTS", 5));
    emailCodeResendCooldownSeconds =
        static_cast<int>(env::getInt("EMAIL_CODE_RESEND_COOLDOWN_SECONDS", 60));
    emailCodeHourlyLimit = static_cast<int>(env::getInt("EMAIL_CODE_HOURLY_LIMIT", 8));
    emailCodeDigits = static_cast<int>(env::getInt("EMAIL_CODE_DIGITS", 6));
    if (emailCodeTtlSeconds < 60 || emailCodeTtlSeconds > 3600) {
        throw std::runtime_error("EMAIL_CODE_TTL_SECONDS 必须在 60..3600 之间");
    }
    if (emailCodeMaxAttempts < 1 || emailCodeMaxAttempts > 20) {
        throw std::runtime_error("EMAIL_CODE_MAX_ATTEMPTS 必须在 1..20 之间");
    }
    if (emailCodeDigits < 4 || emailCodeDigits > 10) {
        throw std::runtime_error("EMAIL_CODE_DIGITS 必须在 4..10 之间");
    }

    sessionCookieName = env::get("SESSION_COOKIE_NAME", "__Host-usapp-session");
    adminSessionCookieName = env::get("ADMIN_SESSION_COOKIE_NAME", "__Host-usapp-admin");
    // __Host- 前缀是浏览器强制的安全保证：Secure + Path=/ + 无 Domain。
    // 如果这里被改名成非 __Host- 名字，就失去了这层保证，因此启动即拒绝。
    if (sessionCookieName.rfind("__Host-", 0) != 0) {
        throw std::runtime_error("SESSION_COOKIE_NAME 必须使用 __Host- 前缀: " + sessionCookieName);
    }
    if (adminSessionCookieName.rfind("__Host-", 0) != 0) {
        throw std::runtime_error("ADMIN_SESSION_COOKIE_NAME 必须使用 __Host- 前缀: " +
                                 adminSessionCookieName);
    }
    preauthSessionTtlSeconds = static_cast<int>(env::getInt("PREAUTH_SESSION_TTL_SECONDS", 1800));
    authSessionTtlSeconds = static_cast<int>(env::getInt("AUTH_SESSION_TTL_SECONDS", 2592000));
    if (authSessionTtlSeconds < preauthSessionTtlSeconds) {
        throw std::runtime_error("AUTH_SESSION_TTL_SECONDS 不应短于 PREAUTH_SESSION_TTL_SECONDS");
    }
    sessionSecret = env::require("SESSION_SECRET");
    if (sessionSecret.size() < 32) {
        throw std::runtime_error("SESSION_SECRET 至少 32 字符");
    }

    mailMode = lowerCopy(env::get("MAIL_MODE", "log"));
    if (mailMode != "log" && mailMode != "smtp") {
        throw std::runtime_error("MAIL_MODE 只能是 log 或 smtp");
    }
    mailFrom = env::get("SMTP_FROM", env::get("MAIL_FROM", "no-reply@usapp.local"));
    smtpHost = env::get("SMTP_HOST", "");
    smtpPort = static_cast<uint16_t>(env::getInt("SMTP_PORT", 587));
    smtpUser = env::get("SMTP_USERNAME", env::get("SMTP_USER", ""));
    smtpPassword = env::get("SMTP_PASSWORD", "");
    smtpStartTls = env::getBool("SMTP_STARTTLS", true);
    mailTimeoutSeconds = static_cast<int>(env::getInt("MAIL_TIMEOUT_SECONDS", 10));
    if (mailTimeoutSeconds < 1 || mailTimeoutSeconds > 120) {
        throw std::runtime_error("MAIL_TIMEOUT_SECONDS 必须在 1..120 之间");
    }
    if (mailMode == "smtp") {
        if (smtpHost.empty()) {
            throw std::runtime_error("MAIL_MODE=smtp 时必须提供 SMTP_HOST");
        }
        if (smtpPort == 0) {
            throw std::runtime_error("SMTP_PORT 不能为 0");
        }
        if (mailFrom.empty()) {
            throw std::runtime_error("MAIL_MODE=smtp 时 SMTP_FROM 不能为空");
        }
    }
    if (appEnv == "production" && mailMode == "log") {
        // 生产环境把验证码写日志 = 任何人都能拿到登录码。明确拒绝启动。
        throw std::runtime_error("生产环境不允许 MAIL_MODE=log（验证码会写入日志）");
    }

    schedulerEnabled = env::getBool("SCHEDULER_ENABLED", true);
    schedulerTickIntervalMs = static_cast<int>(env::getInt("SCHEDULER_TICK_INTERVAL_MS", 15000));
    if (schedulerTickIntervalMs < 1000 || schedulerTickIntervalMs > 3600000) {
        throw std::runtime_error("SCHEDULER_TICK_INTERVAL_MS 必须在 1000..3600000 之间");
    }
    schedulerInstanceId = env::get("SCHEDULER_INSTANCE_ID", "local");
    schedulerKeyNamespace = env::get("SCHEDULER_KEY_NAMESPACE", "usapp.scheduler");
    remindersEnabled = env::getBool("REMINDERS_ENABLED", false);
    memoryPurgeEnabled = env::getBool("MEMORY_PURGE_ENABLED", false);

    adminBootstrapUsername = env::get("ADMIN_BOOTSTRAP_USERNAME", "");
    adminBootstrapPasswordHash = env::get("ADMIN_BOOTSTRAP_PASSWORD_HASH", "");
    if (!adminBootstrapPasswordHash.empty() &&
        !crypto::looksLikePbkdf2Hash(adminBootstrapPasswordHash)) {
        throw std::runtime_error(
            "ADMIN_BOOTSTRAP_PASSWORD_HASH 格式不正确，应为 "
            "pbkdf2-sha256$<iterations>$<salt-b64>$<hash-b64>");
    }

    loaded_ = true;
}

std::string AppConfig::postgresConnectionString() const {
    // libpq 连接串。口令只在此处拼接，从不打印。
    std::string s = "host=" + postgresHost + " port=" + std::to_string(postgresPort) +
                    " dbname=" + postgresDb + " user=" + postgresUser +
                    " password=" + postgresPassword +
                    " connect_timeout=5 application_name=usapp_api";
    return s;
}

bool AppConfig::isAllowedPublicOrigin(const std::string &origin) const {
    if (origin.empty()) {
        return false;
    }
    // 精确比较：规范化后的等价形式必须完全一致。
    try {
        return normalizeOrigin(origin, "Origin", !isProduction()) == publicOrigin;
    } catch (const std::exception &) {
        return false;
    }
}

bool AppConfig::isAllowedAdminOrigin(const std::string &origin) const {
    if (origin.empty()) {
        return false;
    }
    try {
        return normalizeOrigin(origin, "Origin", !isProduction()) == adminOrigin;
    } catch (const std::exception &) {
        return false;
    }
}

}  // namespace usapp
