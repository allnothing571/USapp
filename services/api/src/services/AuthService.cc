// =====================================================================
// services/api/src/services/AuthService.cc
// =====================================================================
#include "services/AuthService.h"

#include <drogon/drogon.h>

#include <memory>
#include <utility>

#include "config/AppConfig.h"
#include "core/Crypto.h"
#include "db/Db.h"
#include "services/RegistrationPolicy.h"
#include "services/SessionService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------------
// 登录事务的上下文。
//
// 为什么要这个结构：登录要在【同一个事务】里串起 7 步
// （锁行 -> 校验 -> 白名单 -> 取/建用户 -> 消费验证码 -> 撤销旧会话 ->
//   建新会话）。用一层层嵌套 lambda 会让 7 个回调互相捕获，
// 一旦漏捕获就是编译错误或错误状态。把状态放进一个共享上下文，
// 每个步骤写成独立函数，只有 ctx 一个捕获，可读且不易错。
// ---------------------------------------------------------------------
struct LoginCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string emailCanonical;
    std::string preauthSessionId;
    std::string userAgentFamily;
    std::string providedHash;
    std::string codeId;
    int authTtlSeconds = 0;
    std::string rawSessionToken;
    std::string sessionTokenHash;
    std::string rawCsrfToken;
    std::string csrfTokenHash;
    std::string userId;
    std::function<void(AuthService::LoginResult)> done;
};

using CtxPtr = std::shared_ptr<LoginCtx>;

AuthService::LoginResult makeResult(AuthService::LoginOutcome outcome, std::string detail = "") {
    AuthService::LoginResult r;
    r.outcome = outcome;
    r.error = std::move(detail);
    return r;
}

// 成功路径的唯一收口点：把结果通过 scope->commit 交出去，
// 保证“先提交、后回调”，客户端拿到结果时会话一定已持久化。
void commitAndReturn(const CtxPtr &ctx, const AuthService::LoginResult &result) {
    ctx->scope->commit([ctx, result]() { ctx->done(result); });
}

void failInternal(const CtxPtr &ctx, const std::string &message) {
    ctx->scope->fail(message);
}

// ---- 步骤 7：创建正式会话 ----
void stepCreateSession(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO sessions "
        "(token_hash, csrf_token_hash, is_authenticated, user_id, user_agent_family, "
        " expires_at, rotated_from_session_id) "
        "VALUES ($1, $2, true, $3::uuid, $4, now() + make_interval(secs => $5::integer), $6::uuid) "
        "RETURNING id::text AS id",
        [ctx](const drogon::orm::Result &r) {
            AuthService::LoginResult result;
            result.outcome = AuthService::LoginOutcome::Success;
            result.userId = ctx->userId;
            result.sessionId = r[0]["id"].as<std::string>();
            // 明文只在这里回给调用方（进而写进 Set-Cookie），数据库里只有散列。
            result.rawSessionToken = ctx->rawSessionToken;
            result.rawCsrfToken = ctx->rawCsrfToken;
            commitAndReturn(ctx, result);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("创建登录会话失败", e.base()));
        },
        ctx->sessionTokenHash,
        ctx->csrfTokenHash,
        ctx->userId,
        ctx->userAgentFamily,
        ctx->authTtlSeconds,
        ctx->preauthSessionId);
}

// ---- 步骤 6：撤销旧的预认证会话，然后建新会话 ----
void stepRotateAndCreateSession(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "UPDATE sessions SET revoked_at = now(), revoked_reason = 'rotated' "
        "WHERE id = $1::uuid AND revoked_at IS NULL",
        [ctx](const drogon::orm::Result &) { stepCreateSession(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("撤销预认证会话失败", e.base()));
        },
        ctx->preauthSessionId);
}

// ---- 步骤 5：消费验证码，然后轮换会话 ----
void stepConsumeCode(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "UPDATE email_verification_codes "
        "SET consumed_at = now(), consumed_reason = 'consumed' WHERE id = $1::uuid",
        [ctx](const drogon::orm::Result &) { stepRotateAndCreateSession(ctx); },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("消费验证码失败", e.base()));
        },
        ctx->codeId);
}

// ---- 步骤 4b：白名单拒绝时消费验证码并回错 ----
void stepRejectRegistration(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "UPDATE email_verification_codes "
        "SET consumed_at = now(), consumed_reason = 'rejected' WHERE id = $1::uuid",
        [ctx](const drogon::orm::Result &) {
            commitAndReturn(
                ctx,
                makeResult(AuthService::LoginOutcome::RegistrationNotAllowed,
                           "该邮箱不在注册白名单内"));
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("标记拒注册失败", e.base()));
        },
        ctx->codeId);
}

// ---- 步骤 4a：已有用户时直接继续；否则插入新用户 ----
void stepResolveUser(const CtxPtr &ctx, bool userExists) {
    if (userExists) {
        stepConsumeCode(ctx);
        return;
    }
    ctx->scope->tx().execSqlAsync(
        "INSERT INTO users (email_canonical, email_display) VALUES ($1, $2) "
        "ON CONFLICT (email_canonical) DO UPDATE SET email_display = EXCLUDED.email_display "
        "RETURNING id::text AS id",
        [ctx](const drogon::orm::Result &r) {
            ctx->userId = r[0]["id"].as<std::string>();
            stepConsumeCode(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("创建用户失败", e.base()));
        },
        ctx->emailCanonical,
        ctx->emailCanonical);
}

// ---- 步骤 4：查用户是否存在，并在此处应用白名单 ----
void stepLookupUser(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id FROM users WHERE email_canonical = $1",
        [ctx](const drogon::orm::Result &rows) {
            const bool exists = !rows.empty();
            if (exists) {
                ctx->userId = rows[0]["id"].as<std::string>();
            }
            // 白名单只在这里生效：此时才知道这是“新账号注册”还是“老账号登录”。
            if (!RegistrationPolicy::instance().mayCreateAccount(ctx->emailCanonical, exists)) {
                stepRejectRegistration(ctx);
                return;
            }
            stepResolveUser(ctx, exists);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("查询用户失败", e.base()));
        },
        ctx->emailCanonical);
}

// ---- 步骤 3：验证码不匹配时累加尝试次数并回错 ----
void stepBumpAttempts(const CtxPtr &ctx, const AuthService::LoginResult &result) {
    ctx->scope->tx().execSqlAsync(
        "UPDATE email_verification_codes SET attempts = attempts + 1 WHERE id = $1::uuid",
        [ctx, result](const drogon::orm::Result &) { commitAndReturn(ctx, result); },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("累加验证码尝试次数失败", e.base()));
        },
        ctx->codeId);
}

// ---- 步骤 2：校验锁定到的验证码行 ----
void stepValidateCode(const CtxPtr &ctx, const drogon::orm::Result &rows, int configuredMax) {
    if (rows.empty()) {
        // 没有未消费的验证码：可能是没发过、已用过或已被作废。
        commitAndReturn(ctx, makeResult(AuthService::LoginOutcome::CodeInvalidOrExpired,
                                        "没有未消费的验证码"));
        return;
    }

    ctx->codeId = rows[0]["id"].as<std::string>();
    const std::string storedHash = rows[0]["code_hash"].as<std::string>();
    const int attempts = rows[0]["attempts"].as<int>();
    const int rowMax = rows[0]["max_attempts"].as<int>();
    const bool expired = rows[0]["expired"].as<bool>();
    const int effectiveMax = rowMax > 0 ? rowMax : configuredMax;

    if (expired) {
        commitAndReturn(ctx, makeResult(AuthService::LoginOutcome::CodeInvalidOrExpired,
                                        "验证码已过期"));
        return;
    }
    if (attempts >= effectiveMax) {
        commitAndReturn(ctx, makeResult(AuthService::LoginOutcome::TooManyAttempts,
                                        "验证码尝试次数已达上限"));
        return;
    }
    if (!crypto::constantTimeEquals(ctx->providedHash, storedHash)) {
        stepBumpAttempts(ctx, makeResult(AuthService::LoginOutcome::CodeInvalidOrExpired,
                                         "验证码不匹配"));
        return;
    }
    stepLookupUser(ctx);
}

// ---- 步骤 1：锁定验证码行 ----
void stepLockCode(const CtxPtr &ctx, int configuredMax) {
    ctx->scope->tx().execSqlAsync(
        "SELECT id::text AS id, code_hash, attempts, max_attempts, "
        "       (expires_at <= now()) AS expired "
        "FROM email_verification_codes "
        "WHERE requested_by_session_id = $1::uuid AND email_canonical = $2 "
        "  AND consumed_at IS NULL "
        "ORDER BY created_at DESC LIMIT 1 FOR UPDATE",
        [ctx, configuredMax](const drogon::orm::Result &rows) {
            stepValidateCode(ctx, rows, configuredMax);
        },
        [ctx](const drogon::orm::DrogonDbException &e) {
            failInternal(ctx, db::describePgError("锁定验证码失败", e.base()));
        },
        ctx->preauthSessionId,
        ctx->emailCanonical);
}

}  // namespace

AuthService &AuthService::instance() {
    static AuthService service;
    return service;
}

// =====================================================================
// 发送验证码
// =====================================================================
void AuthService::sendCode(const std::string &emailCanonical,
                           const std::string &sessionId,
                           std::function<void(SendResult)> done) {
    const auto cfg = AppConfig::instance();
    // 明文验证码只活在这个局部变量与 Mailer 调用中；数据库只保存散列。
    const std::string rawCode = crypto::randomNumericCode(cfg.emailCodeDigits);
    const std::string codeHash = crypto::sha256Hex(rawCode);
    const int cooldown = cfg.emailCodeResendCooldownSeconds;
    const long long hourlyLimit = cfg.emailCodeHourlyLimit;
    const int ttl = cfg.emailCodeTtlSeconds;

    auto throttled = [done](std::shared_ptr<db::TxScope> scope) {
        scope->rollback([done]() {
            SendResult r;
            // 冷却/超限对外仍等价于“已受理”，避免据此探测账号状态。
            r.outcome = SendOutcome::Throttled;
            done(r);
        });
    };

    db::TxScope::begin(
        [emailCanonical, sessionId, codeHash, rawCode, cooldown, hourlyLimit, ttl, done, throttled](
            std::shared_ptr<db::TxScope> scope) {
            auto onError = [done](const std::string &msg) {
                SendResult r;
                r.outcome = SendOutcome::Error;
                r.error = msg;
                done(r);
            };

            // ---- 1) 冷却时间 ----
            scope->tx().execSqlAsync(
                "SELECT COALESCE(EXTRACT(EPOCH FROM (now() - max(created_at))), 1e9)::bigint "
                "         AS age_seconds "
                "FROM email_verification_codes WHERE email_canonical = $1",
                [scope, emailCanonical, sessionId, codeHash, rawCode, cooldown, hourlyLimit, ttl,
                 done, throttled, onError](const drogon::orm::Result &cooldownRows) {
                    const long long age = cooldownRows[0]["age_seconds"].as<long long>();
                    if (age < cooldown) {
                        throttled(scope);
                        return;
                    }

                    // ---- 2) 每小时上限 ----
                    scope->tx().execSqlAsync(
                        "SELECT count(*)::bigint AS cnt FROM email_verification_codes "
                        "WHERE email_canonical = $1 AND created_at > now() - interval '1 hour'",
                        [scope, sessionId, codeHash, rawCode, hourlyLimit, ttl, done, throttled, onError,
                         emailCanonical](const drogon::orm::Result &countRows) {
                            const long long cnt = countRows[0]["cnt"].as<long long>();
                            if (cnt >= hourlyLimit) {
                                throttled(scope);
                                return;
                            }

                            // ---- 3) 作废此前未消费的验证码（部分唯一索引要求） ----
                            scope->tx().execSqlAsync(
                                "UPDATE email_verification_codes "
                                "SET consumed_at = now(), consumed_reason = 'superseded' "
                                "WHERE requested_by_session_id = $1::uuid "
                                "  AND consumed_at IS NULL",
                                [scope, emailCanonical, sessionId, codeHash, rawCode, ttl, done, onError](
                                    const drogon::orm::Result &) {
                                    // ---- 4) 写入新验证码 ----
                                    scope->tx().execSqlAsync(
                                        "INSERT INTO email_verification_codes "
                                        "(email_canonical, code_hash, requested_by_session_id, "
                                        " expires_at) "
                                        "VALUES ($1, $2, $3::uuid, "
                                        "        now() + make_interval(secs => $4::integer))",
                                        [scope, rawCode, done](const drogon::orm::Result &) {
                                            // 事务提交后才返回；发信由调用方在提交后执行，
                                            // 网络操作绝不占用数据库事务。
                                            scope->commit([done, rawCode]() {
                                                SendResult r;
                                                r.outcome = SendOutcome::Accepted;
                                                r.code = rawCode;
                                                done(r);
                                            });
                                        },
                                        [scope, onError](const drogon::orm::DrogonDbException &e) {
                                            scope->fail(
                                                db::describePgError("写入验证码失败", e.base()));
                                        },
                                        emailCanonical,
                                        codeHash,
                                        sessionId,
                                        ttl);
                                },
                                [scope, onError](const drogon::orm::DrogonDbException &e) {
                                    scope->fail(
                                        db::describePgError("作废旧验证码失败", e.base()));
                                },
                                sessionId);
                        },
                        [scope, onError](const drogon::orm::DrogonDbException &e) {
                            scope->fail(db::describePgError("统计验证码频率失败", e.base()));
                        },
                        emailCanonical);
                },
                [scope, onError](const drogon::orm::DrogonDbException &e) {
                    scope->fail(db::describePgError("查询验证码冷却失败", e.base()));
                },
                emailCanonical);
        },
        [done](const std::string &msg) {
            SendResult r;
            r.outcome = SendOutcome::Error;
            r.error = msg;
            done(r);
        });
}

// =====================================================================
// 用验证码登录（单事务）
// =====================================================================
void AuthService::loginWithCode(const std::string &emailCanonical,
                                const std::string &code,
                                const std::string &preauthSessionId,
                                const std::string &userAgentFamily,
                                std::function<void(LoginResult)> done) {
    auto finishError = [done](LoginOutcome outcome, const std::string &detail) {
        LoginResult r;
        r.outcome = outcome;
        r.error = detail;
        done(r);
    };

    if (preauthSessionId.empty()) {
        finishError(LoginOutcome::NoSession, "缺少预认证会话");
        return;
    }
    if (code.empty()) {
        finishError(LoginOutcome::CodeInvalidOrExpired, "验证码为空");
        return;
    }

    const auto cfg = AppConfig::instance();

    auto ctx = std::make_shared<LoginCtx>();
    ctx->emailCanonical = emailCanonical;
    ctx->preauthSessionId = preauthSessionId;
    ctx->userAgentFamily = userAgentFamily;
    ctx->providedHash = crypto::sha256Hex(code);
    ctx->authTtlSeconds = cfg.authSessionTtlSeconds;
    ctx->rawSessionToken = crypto::randomHex(32);
    ctx->sessionTokenHash = SessionService::tokenHash(ctx->rawSessionToken);
    ctx->rawCsrfToken = crypto::randomHex(32);
    ctx->csrfTokenHash = crypto::sha256Hex(ctx->rawCsrfToken);
    ctx->done = std::move(done);

    const int configuredMax = cfg.emailCodeMaxAttempts;

    db::TxScope::begin(
        [ctx, configuredMax](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            stepLockCode(ctx, configuredMax);
        },
        [ctx](const std::string &msg) {
            LoginResult r;
            r.outcome = LoginOutcome::Error;
            r.error = msg;
            ctx->done(r);
        });
}

}  // namespace usapp
