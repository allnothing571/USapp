// =====================================================================
// services/api/src/services/SessionService.cc
// =====================================================================
#include "services/SessionService.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>

#include "config/AppConfig.h"
#include "core/Crypto.h"
#include "db/Db.h"

namespace usapp {

namespace {

constexpr const char *kSelectSessionColumns =
    " id::text AS id, "
    " COALESCE(user_id::text, '') AS user_id, "
    " is_authenticated, "
    " csrf_token_hash, "
    " to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS expires_at, "
    " (expires_at <= now()) AS expired, "
    " (revoked_at IS NOT NULL) AS revoked ";

SessionRecord rowToSession(const drogon::orm::Row &row) {
    SessionRecord s;
    s.id = row["id"].as<std::string>();
    s.userId = row["user_id"].as<std::string>();
    s.authenticated = row["is_authenticated"].as<bool>();
    s.csrfTokenHash = row["csrf_token_hash"].as<std::string>();
    s.expiresAt = row["expires_at"].as<std::string>();
    s.expired = row["expired"].as<bool>();
    s.revoked = row["revoked"].as<bool>();
    return s;
}

}  // namespace

SessionService &SessionService::instance() {
    static SessionService service;
    return service;
}

std::string SessionService::tokenHash(const std::string &rawToken) {
    return crypto::sha256Hex(rawToken);
}

void SessionService::loadByRawToken(const std::string &rawToken, SessionCallback cb) {
    if (rawToken.empty()) {
        cb(std::nullopt);
        return;
    }
    const std::string hash = tokenHash(rawToken);
    db::Pool::instance().client()->execSqlAsync(
        std::string("SELECT") + kSelectSessionColumns + " FROM sessions WHERE token_hash = $1",
        [cb](const drogon::orm::Result &r) {
            if (r.empty()) {
                cb(std::nullopt);
                return;
            }
            cb(rowToSession(r[0]));
        },
        [cb](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "loadByRawToken 失败: " << e.base().what();
            // 查不到就按匿名处理（fail closed），不把数据库故障变成放行。
            cb(std::nullopt);
        },
        hash);
}

void SessionService::createPreauthSession(const std::string &userAgentFamily,
                                          CreatedCallback onCreated,
                                          FailCallback onError) {
    const std::string rawToken = crypto::randomHex(32);
    const std::string rawCsrf = crypto::randomHex(32);
    const std::string tokenHashValue = tokenHash(rawToken);
    const std::string csrfHash = crypto::sha256Hex(rawCsrf);
    const int ttl = AppConfig::instance().preauthSessionTtlSeconds;

    db::Pool::instance().client()->execSqlAsync(
        "INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, "
        "                      user_agent_family, expires_at) "
        "VALUES ($1, $2, false, NULL, $3, now() + make_interval(secs => $4::integer)) "
        "RETURNING id::text AS id",
        [onCreated, rawToken, rawCsrf](const drogon::orm::Result &r) {
            onCreated(r[0]["id"].as<std::string>(), rawToken, rawCsrf);
        },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(db::describePgError("创建预认证会话失败", e.base()));
        },
        tokenHashValue,
        csrfHash,
        userAgentFamily,
        ttl);
}

void SessionService::createAuthenticatedSession(const std::string &userId,
                                                const std::string &previousSessionId,
                                                const std::string &userAgentFamily,
                                                CreatedCallback onCreated,
                                                FailCallback onError) {
    const std::string rawToken = crypto::randomHex(32);
    const std::string rawCsrf = crypto::randomHex(32);
    const std::string tokenHashValue = tokenHash(rawToken);
    const std::string csrfHash = crypto::sha256Hex(rawCsrf);
    const int ttl = AppConfig::instance().authSessionTtlSeconds;

    // 撤销旧会话 + 新建正式会话必须原子：否则可能出现“旧会话仍有效”
    // 或“新会话已发但旧会话未撤销”的窗口。
    db::TxScope::begin(
        [userId,
         previousSessionId,
         userAgentFamily,
         ttl,
         tokenHashValue,
         csrfHash,
         rawToken,
         rawCsrf,
         onCreated,
         onError](std::shared_ptr<db::TxScope> scope) {
            auto runInsert = [scope,
                              userId,
                              previousSessionId,
                              userAgentFamily,
                              ttl,
                              tokenHashValue,
                              csrfHash,
                              rawToken,
                              rawCsrf,
                              onCreated,
                              onError]() {
                // rotated_from_session_id 必须按值捕获：它在外层lambda 的
                // 捕获列表里，runInsert 若不捕获就是编译错误（未声明标识符）。
                // 有了它，下面按 previousSessionId 是否为空写出不同的 SQL，
                // 从而在【同一条语句】里保持"撤销旧会话 + 新建会话"的原子性。
                if (previousSessionId.empty()) {
                    // 无旧会话：明确写 SQL NULL。
                    // 不能把空字符串交给 $6::uuid——那会报invalid input syntax
                    // for type uuid，或在某些驱动下写成 ''::uuid 而非 NULL，
                    // 破坏 rotated_from 的外键/可空语义。
                    scope->tx().execSqlAsync(
                        "INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, "
                        "user_id, user_agent_family, expires_at, rotated_from_session_id) "
                        "VALUES ($1, $2, true, $3::uuid, $4, now() + make_interval(secs => $5::integer), "
                        "        NULL) "
                        "RETURNING id::text AS id",
                        [scope, rawToken, rawCsrf, onCreated, onError](const drogon::orm::Result &r) {
                            const std::string newId = r[0]["id"].as<std::string>();
                            scope->commit([onCreated, newId, rawToken, rawCsrf]() {
                                onCreated(newId, rawToken, rawCsrf);
                            });
                        },
                        [scope, onError](const drogon::orm::DrogonDbException &e) {
                            scope->fail(db::describePgError("创建正式会话失败", e.base()));
                        },
                        tokenHashValue,
                        csrfHash,
                        userId,
                        userAgentFamily,
                        ttl);
                    return;
                }
                // 有旧会话：与"撤销旧会话"写在同一条 INSERT 里（同一事务、同一连接）。
                scope->tx().execSqlAsync(
                    "INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, "
                    "                      user_agent_family, expires_at, rotated_from_session_id) "
                    "VALUES ($1, $2, true, $3::uuid, $4, now() + make_interval(secs => $5::integer), "
                    "        $6::uuid) "
                    "RETURNING id::text AS id",
                    [scope, rawToken, rawCsrf, onCreated, onError](const drogon::orm::Result &r) {
                        const std::string newId = r[0]["id"].as<std::string>();
                        scope->commit([onCreated, newId, rawToken, rawCsrf]() {
                            onCreated(newId, rawToken, rawCsrf);
                        });
                    },
                    [scope, onError](const drogon::orm::DrogonDbException &e) {
                        scope->fail(db::describePgError("创建正式会话失败", e.base()));
                    },
                    tokenHashValue,
                    csrfHash,
                    userId,
                    userAgentFamily,
                    ttl,
                    previousSessionId);
            };

            if (previousSessionId.empty()) {
                runInsert();
                return;
            }
            scope->tx().execSqlAsync(
                "UPDATE sessions SET revoked_at = now(), revoked_reason = 'rotated' "
                "WHERE id = $1::uuid AND revoked_at IS NULL",
                [runInsert](const drogon::orm::Result &) { runInsert(); },
                [scope, onError](const drogon::orm::DrogonDbException &e) {
                    scope->fail(db::describePgError("撤销旧会话失败", e.base()));
                },
                previousSessionId);
        },
        onError);
}

void SessionService::rotateCsrfToken(const std::string &sessionId,
                                     CsrfRotatedCallback onRotated,
                                     FailCallback onError) {
    if (sessionId.empty()) {
        onRotated(std::string());
        return;
    }
    const std::string rawCsrf = crypto::randomHex(32);
    const std::string csrfHash = crypto::sha256Hex(rawCsrf);

    // WHERE 里重复一遍「未撤销且未过期」：引导接口在只读路径上，不能因为
    // 拿到一个已失效的会话 id 就把它的令牌续上。
    db::Pool::instance().client()->execSqlAsync(
        "UPDATE sessions SET csrf_token_hash = $2 "
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
            onError(db::describePgError("轮换 CSRF 令牌失败", e.base()));
        },
        sessionId,
        csrfHash);
}

void SessionService::revokeSession(const std::string &sessionId,
                                   const std::string &reason,
                                   std::function<void()> onDone,
                                   FailCallback onError) {
    if (sessionId.empty()) {
        onDone();
        return;
    }
    db::Pool::instance().client()->execSqlAsync(
        "UPDATE sessions SET revoked_at = now(), revoked_reason = $2 "
        "WHERE id = $1::uuid AND revoked_at IS NULL",
        [onDone](const drogon::orm::Result &) { onDone(); },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(db::describePgError("撤销会话失败", e.base()));
        },
        sessionId,
        reason);
}

void SessionService::touchSession(const std::string &sessionId) noexcept {
    if (sessionId.empty()) {
        return;
    }
    try {
        db::Pool::instance().client()->execSqlAsync(
            "UPDATE sessions SET last_seen_at = now() WHERE id = $1::uuid",
            [](const drogon::orm::Result &) {},
            [](const drogon::orm::DrogonDbException &) {},
            sessionId);
    } catch (const std::exception &) {
        // 尽力而为：last_seen_at 更新失败不影响业务请求。
    }
}

bool SessionService::verifyCsrf(const SessionRecord &session, const std::string &providedToken) {
    if (providedToken.empty() || session.csrfTokenHash.empty()) {
        return false;
    }
    const std::string providedHash = crypto::sha256Hex(providedToken);
    return crypto::constantTimeEquals(providedHash, session.csrfTokenHash);
}

std::string SessionService::userAgentFamily(const std::string &userAgent) {
    std::string ua = userAgent;
    std::transform(ua.begin(), ua.end(), ua.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ua.empty()) {
        return "unknown";
    }
    if (ua.find("edg/") != std::string::npos) {
        return "edge";
    }
    if (ua.find("chrome/") != std::string::npos && ua.find("chromium") == std::string::npos) {
        return "chrome";
    }
    if (ua.find("firefox/") != std::string::npos) {
        return "firefox";
    }
    if (ua.find("safari/") != std::string::npos) {
        return "safari";
    }
    if (ua.find("curl/") != std::string::npos) {
        return "curl";
    }
    return "other";
}

}  // namespace usapp
