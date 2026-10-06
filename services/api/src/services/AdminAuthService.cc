// =====================================================================
// services/api/src/services/AdminAuthService.cc
// =====================================================================
#include "services/AdminAuthService.h"

#include <drogon/drogon.h>

#include <exception>
#include <memory>
#include <utility>

#include "config/AppConfig.h"
#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AuditLog.h"

namespace usapp {

namespace {

// 账号不存在时用来跑"等价开销假校验"的固定串。必须是合法 pbkdf2 格式，
// 否则 pbkdf2Verify 会走更便宜的分支，时序保护就失效了。
// 这个散列对应一个不可猜的占位口令，不是任何可用口令的散列。
const char *kDummyHash =
    "pbkdf2-sha256$600000$AAAAAAAAAAAAAAAAAAAAAA==$"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

AdminAuthService::LoginResult makeLoginResult(AdminAuthService::LoginOutcome outcome,
                                              std::string detail = "") {
    AdminAuthService::LoginResult result;
    result.outcome = outcome;
    result.error = std::move(detail);
    return result;
}

}  // namespace

AdminAuthService &AdminAuthService::instance() {
    static AdminAuthService service;
    return service;
}

std::string AdminAuthService::tokenHash(const std::string &rawToken) {
    // 与用户会话用同一散列（SHA-256 hex）；表不同、Cookie 名不同，互不通用。
    return crypto::sha256Hex(rawToken);
}

bool AdminAuthService::verifyCsrf(const AdminSessionRecord &session,
                                  const std::string &providedToken) {
    if (providedToken.empty() || session.csrfTokenHash.empty()) {
        return false;
    }
    // 先散列再恒定时间比较；直接比明文会泄露长度/前缀信息。
    const std::string providedHash = crypto::sha256Hex(providedToken);
    return crypto::constantTimeEquals(providedHash, session.csrfTokenHash);
}

// =====================================================================
// 登录
// =====================================================================
void AdminAuthService::login(const std::string &username,
                             const std::string &password,
                             std::function<void(LoginResult)> done) {
    if (username.empty() || password.empty()) {
        done(makeLoginResult(LoginOutcome::Validation, "缺少用户名或口令"));
        return;
    }

    const std::string rawToken = crypto::randomHex(32);
    const std::string rawCsrf = crypto::randomHex(32);
    const std::string tokenHashValue = tokenHash(rawToken);
    const std::string csrfHashValue = crypto::sha256Hex(rawCsrf);

    try {
        db::Pool::instance().client()->execSqlAsync(
            "SELECT id::text AS id, username, password_hash, status "
            "FROM admin_users WHERE username = $1",
            [username, password, rawToken, rawCsrf, tokenHashValue, csrfHashValue, done](
                const drogon::orm::Result &rows) {
                // 无论账号是否存在都执行一次 PBKDF2，响应时间不可用于枚举用户名。
                const bool found = !rows.empty();
                const std::string storedHash =
                    found ? rows[0]["password_hash"].as<std::string>() : std::string(kDummyHash);
                const bool passwordOk = crypto::pbkdf2Verify(password, storedHash);

                if (!found || !passwordOk) {
                    done(makeLoginResult(LoginOutcome::InvalidCredentials, "管理员凭据不正确"));
                    return;
                }
                if (rows[0]["status"].as<std::string>() != "active") {
                    // 与口令错误对外完全一致，避免暴露"这个管理员被停用了"。
                    done(makeLoginResult(LoginOutcome::InvalidCredentials, "管理员账号已停用"));
                    return;
                }

                const std::string adminUserId = rows[0]["id"].as<std::string>();

                try {
                    db::Pool::instance().client()->execSqlAsync(
                        "INSERT INTO admin_sessions "
                        "(token_hash, csrf_token_hash, admin_user_id, expires_at) "
                        "VALUES ($1, $2, $3::uuid, now() + make_interval(secs => $4::integer)) "
                        "RETURNING id::text AS id",
                        [adminUserId, username, rawToken, rawCsrf, done](
                            const drogon::orm::Result &sessionRows) {
                            LoginResult result;
                            result.outcome = LoginOutcome::Success;
                            result.adminUserId = adminUserId;
                            result.sessionId = sessionRows[0]["id"].as<std::string>();
                            result.rawSessionToken = rawToken;
                            result.rawCsrfToken = rawCsrf;

                            // last_login_at 是审计信息，失败不应阻断登录。
                            db::Pool::instance().client()->execSqlAsync(
                                "UPDATE admin_users SET last_login_at = now() WHERE id = $1::uuid",
                                [](const drogon::orm::Result &) {},
                                [](const drogon::orm::DrogonDbException &e) {
                                    LOG_WARN << "更新管理员 last_login_at 失败: " << e.base().what();
                                },
                                adminUserId);

                            AuditLog::record("admin",
                                             adminUserId,
                                             "",
                                             "admin.login",
                                             "admin_user",
                                             adminUserId,
                                             Json::Value(Json::objectValue));
                            done(result);
                        },
                        [done](const drogon::orm::DrogonDbException &e) {
                            done(makeLoginResult(LoginOutcome::Error,
                                                 db::describePgError("创建管理员会话失败", e.base())));
                        },
                        tokenHashValue,
                        csrfHashValue,
                        adminUserId,
                        kAdminSessionTtlSeconds);
                } catch (const std::exception &e) {
                    done(makeLoginResult(LoginOutcome::Error,
                                         std::string("创建管理员会话异常: ") + e.what()));
                }
            },
            [done](const drogon::orm::DrogonDbException &e) {
                done(makeLoginResult(LoginOutcome::Error,
                                     db::describePgError("查询管理员失败", e.base())));
            },
            username);
    } catch (const std::exception &e) {
        done(makeLoginResult(LoginOutcome::Error, std::string("管理员登录异常: ") + e.what()));
    }
}

// =====================================================================
// 按令牌加载
// =====================================================================
void AdminAuthService::loadByRawToken(
    const std::string &rawToken,
    std::function<void(std::optional<AdminSessionRecord>)> cb) {
    if (rawToken.empty()) {
        cb(std::nullopt);
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            "SELECT s.id::text AS id, s.admin_user_id::text AS admin_user_id, "
            "       s.csrf_token_hash, u.username, "
            "       to_char(s.expires_at AT TIME ZONE 'UTC', "
            "               'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires_at, "
            "       (s.expires_at <= now())      AS expired, "
            "       (s.revoked_at IS NOT NULL)   AS revoked "
            "FROM admin_sessions AS s "
            "JOIN admin_users AS u ON u.id = s.admin_user_id "
            "WHERE s.token_hash = $1",
            [cb](const drogon::orm::Result &rows) {
                if (rows.empty()) {
                    cb(std::nullopt);
                    return;
                }
                const auto &row = rows[0];
                AdminSessionRecord record;
                record.id = row["id"].as<std::string>();
                record.adminUserId = row["admin_user_id"].as<std::string>();
                record.username = row["username"].as<std::string>();
                record.csrfTokenHash = row["csrf_token_hash"].as<std::string>();
                record.expiresAt = row["expires_at"].as<std::string>();
                record.expired = row["expired"].as<bool>();
                record.revoked = row["revoked"].as<bool>();
                if (record.expired || record.revoked) {
                    // 过期或已撤销一律按匿名处理，不做"顺手复活"。
                    cb(std::nullopt);
                    return;
                }
                cb(record);
            },
            [cb](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << "加载管理员会话失败: " << e.base().what();
                cb(std::nullopt);  // fail closed
            },
            tokenHash(rawToken));
    } catch (const std::exception &e) {
        LOG_ERROR << "加载管理员会话异常: " << e.what();
        cb(std::nullopt);
    }
}

// =====================================================================
// 注销
// =====================================================================
void AdminAuthService::revokeSession(const std::string &sessionId,
                                     const std::string &reason,
                                     std::function<void()> onDone,
                                     FailCallback onError) {
    if (sessionId.empty()) {
        onDone();
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            "UPDATE admin_sessions SET revoked_at = now(), revoked_reason = $2 "
            "WHERE id = $1::uuid AND revoked_at IS NULL "
            "RETURNING admin_user_id::text AS admin_user_id",
            [onDone](const drogon::orm::Result &rows) {
                if (!rows.empty()) {
                    AuditLog::record("admin",
                                     rows[0]["admin_user_id"].as<std::string>(),
                                     "",
                                     "admin.logout",
                                     "admin_user",
                                     rows[0]["admin_user_id"].as<std::string>(),
                                     Json::Value(Json::objectValue));
                }
                // 找不到也视为成功：注销必须幂等。
                onDone();
            },
            [onError](const drogon::orm::DrogonDbException &e) {
                onError(db::describePgError("撤销管理员会话失败", e.base()));
            },
            sessionId,
            reason);
    } catch (const std::exception &e) {
        onError(std::string("撤销管理员会话异常: ") + e.what());
    }
}

void AdminAuthService::touchSession(const std::string &sessionId) noexcept {
    if (sessionId.empty() || !db::Pool::instance().ready()) {
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            "UPDATE admin_sessions SET last_seen_at = now() WHERE id = $1::uuid",
            [](const drogon::orm::Result &) {},
            [](const drogon::orm::DrogonDbException &e) {
                LOG_DEBUG << "touch 管理员会话失败（忽略）: " << e.base().what();
            },
            sessionId);
    } catch (...) {
        // 尽力而为，绝不因为 touch 失败影响请求。
    }
}

// =====================================================================
// 引导管理员
// =====================================================================
void AdminAuthService::syncBootstrapAdmin(std::function<void(bool)> onDone) noexcept {
    try {
        const auto &cfg = AppConfig::instance();
        if (cfg.adminBootstrapUsername.empty() || cfg.adminBootstrapPasswordHash.empty()) {
            LOG_INFO << "未配置 ADMIN_BOOTSTRAP_USERNAME/HASH，跳过管理员引导"
                        "（管理后台需先手工创建管理员账号）";
            if (onDone) {
                onDone(false);
            }
            return;
        }
        if (!crypto::looksLikePbkdf2Hash(cfg.adminBootstrapPasswordHash)) {
            // 只报"格式不对"，绝不回显散列内容。
            LOG_ERROR << "ADMIN_BOOTSTRAP_PASSWORD_HASH 不是合法的 pbkdf2-sha256 格式，拒绝引导";
            if (onDone) {
                onDone(false);
            }
            return;
        }

        const std::string username = cfg.adminBootstrapUsername;
        const std::string passwordHash = cfg.adminBootstrapPasswordHash;

        if (!db::Pool::instance().ready()) {
            LOG_ERROR << "数据库连接池未就绪，无法引导管理员账号";
            if (onDone) {
                onDone(false);
            }
            return;
        }

        db::Pool::instance().client()->execSqlAsync(
            "INSERT INTO admin_users (username, password_hash) VALUES ($1, $2) "
            "ON CONFLICT (username) DO UPDATE SET password_hash = EXCLUDED.password_hash, "
            "                                     updated_at = now()",
            [username, onDone](const drogon::orm::Result &) {
                LOG_INFO << "管理员引导完成 username=" << username
                         << "（口令散列来自环境变量，未写入日志）";
                if (onDone) {
                    onDone(true);
                }
            },
            [onDone](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << "管理员引导失败: " << e.base().what();
                if (onDone) {
                    onDone(false);
                }
            },
            username,
            passwordHash);
    } catch (const std::exception &e) {
        LOG_ERROR << "管理员引导异常: " << e.what();
        if (onDone) {
            onDone(false);
        }
    } catch (...) {
        LOG_ERROR << "管理员引导异常（未知类型）";
        if (onDone) {
            onDone(false);
        }
    }
}

void AdminAuthService::rotateCsrfToken(
    const std::string &sessionId,
    std::function<void(const std::string &rawCsrfToken)> onRotated,
    FailCallback onError) {
    if (sessionId.empty()) {
        onRotated(std::string());
        return;
    }
    const std::string rawCsrf = crypto::randomHex(32);
    const std::string csrfHash = crypto::sha256Hex(rawCsrf);
    try {
        db::Pool::instance().client()->execSqlAsync(
            "UPDATE admin_sessions SET csrf_token_hash = $2 "
            "WHERE id = $1::uuid AND revoked_at IS NULL AND expires_at > now() "
            "RETURNING id::text AS id",
            [onRotated, rawCsrf](const drogon::orm::Result &r) {
                if (r.empty()) {
                    onRotated(std::string());
                    return;
                }
                onRotated(rawCsrf);
            },
            [onError](const drogon::orm::DrogonDbException &e) {
                onError(db::describePgError("轮换管理员 CSRF 令牌失败", e.base()));
            },
            sessionId,
            csrfHash);
    } catch (const std::exception &e) {
        onError(std::string("轮换管理员 CSRF 令牌异常: ") + e.what());
    }
}

}  // namespace usapp
