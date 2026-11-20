// =====================================================================
// services/api/src/controllers/AdminController.cc
//
// GET    /api/admin/v1/system/status                已登录管理员只读运维快照
// GET    /api/admin/v1/health                       存活探针
// GET    /api/admin/v1/session                      管理会话引导 / CSRF 获取
// POST   /api/admin/v1/sessions                     管理员登录（独立账号体系）
// DELETE /api/admin/v1/sessions                     管理员注销
// GET    /api/admin/v1/users                        用户列表（只读分页）
// GET    /api/admin/v1/couples                      空间列表（只读分页）
// GET    /api/admin/v1/audit-logs                   审计日志（只读分页）
//
// 全部挂 AdminFilter。它的四层边界（对端 CIDR / 入口标记头 / 精确 Origin +
// 自定义头 / 会话绑定的 CSRF）对包含 health 在内的每个路由都生效，
// 所以管理接口在公网方向上连"存在"都不会被确认（CIDR 不符一律 404）。
//
// 管理权限与情侣角色完全无关：这里没有任何一处读取 couple_members，
// 管理员身份只来自 admin_users / admin_sessions 这一套独立表。
//
// 列表分页是阶段 0 骨架：偏移量分页 + 不透明游标（见 ControllerSupport.h
// 的 encodeCursor/decodeCursor）。只读，不提供任何写操作。
// =====================================================================

#include <drogon/HttpController.h>
#include <json/json.h>

#include <functional>
#include <string>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "db/Db.h"
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminAuthService.h"
#include "services/MediaService.h"

namespace usapp::controllers {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

std::string readString(const Json::Value &body, const char *key) {
    return body.isMember(key) && body[key].isString() ? body[key].asString() : "";
}

// 统一的列表失败出口：数据库报错只进日志，响应体里只有通用文案。
void respondListError(const drogon::HttpRequestPtr &req,
                      const Callback &callback,
                      const std::string &logPrefix,
                      const std::string &error) {
    LOG_ERROR << logPrefix << "：" << error << " request_id=" << http::requestId(req);
    callback(errorResponse(ErrorCode::InternalError, "服务内部错误", 0, http::requestId(req)));
}

}  // namespace

class AdminController : public drogon::HttpController<AdminController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminController::health, "/api/admin/v1/health", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::systemStatus, "/api/admin/v1/system/status", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::bootstrap, "/api/admin/v1/session", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::login, "/api/admin/v1/sessions", drogon::Post,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::logout, "/api/admin/v1/sessions", drogon::Delete,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::listUsers, "/api/admin/v1/users", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::listCouples, "/api/admin/v1/couples", drogon::Get,
                  AdminFilter::classTypeName());
    ADD_METHOD_TO(AdminController::listAuditLogs, "/api/admin/v1/audit-logs", drogon::Get,
                  AdminFilter::classTypeName());
    METHOD_LIST_END

    void health(const drogon::HttpRequestPtr & /*req*/,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        Json::Value body(Json::objectValue);
        body["status"] = "ok";
        body["version"] = AppConfig::instance().appVersion;
        body["time"] = http::nowIso8601Utc();
        callback(jsonResponse(body, 200));
    }

    // Authenticated, read-only, explicit allowlist. A configuration flag is not worker liveness or release readiness.
    void systemStatus(const drogon::HttpRequestPtr &req, Callback &&callback) {
        if (adminctx::userId(req).empty()) {
            callback(ctrl::fail(ErrorCode::Unauthenticated, "管理员未登录", req));
            return;
        }
        const auto &cfg = AppConfig::instance();
        Json::Value process(Json::objectValue);
        process["scheduler_configured"] = cfg.schedulerEnabled;
        process["reminders_configured"] = cfg.remindersEnabled;
        process["memory_purge_configured"] = cfg.memoryPurgeEnabled;
        process["uploads_enabled"] = MediaService::instance().uploadsEnabled();
        process["mail_mode"] = cfg.mailMode;
        process["registration_mode"] = cfg.registrationMode;
        db::Pool::instance().client()->execSqlAsync(
            "SELECT count(*)::integer AS migration_count, max(version) AS latest_migration FROM schema_migrations",
            [callback, process](const drogon::orm::Result &rows) {
                Json::Value result(Json::objectValue), database(Json::objectValue);
                database["migration_count"] = rows[0]["migration_count"].as<int>();
                database["latest_migration"] = rows[0]["latest_migration"].isNull() ? Json::Value(Json::nullValue)
                    : Json::Value(rows[0]["latest_migration"].as<std::string>());
                result["database"] = database;
                result["process"] = process;
                result["read_at"] = http::nowIso8601Utc();
                callback(jsonResponse(result, 200));
            },
            [req, callback](const drogon::orm::DrogonDbException &) {
                // Do not disclose DSNs, server diagnostics, credentials or host paths.
                callback(ctrl::fail(ErrorCode::InternalError, "运维快照读取失败", req));
            });
    }

    // ------------------------------------------------------------------
    // 管理会话引导。未登录时 csrf_token 是空串——这不是错误：管理面没有
    // "预认证会话"，登录端点本身不需要 CSRF（见 AdminFilter.cc 的分层说明），
    // 而登录之后的每个写请求都需要这里返回的令牌。
    // ------------------------------------------------------------------
    void bootstrap(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string sessionId = adminctx::sessionId(req);
        if (sessionId.empty()) {
            Json::Value body(Json::objectValue);
            body["authenticated"] = false;
            body["csrf_token"] = "";
            callback(jsonResponse(body, 200));
            return;
        }
        AdminAuthService::instance().rotateCsrfToken(
            sessionId,
            [req, callback](const std::string &rawCsrfToken) {
                if (rawCsrfToken.empty()) {
                    // 会话在过滤器放行之后失效（竞态）：按未登录回答。
                    Json::Value body(Json::objectValue);
                    body["authenticated"] = false;
                    body["csrf_token"] = "";
                    callback(jsonResponse(body, 200));
                    return;
                }
                Json::Value body(Json::objectValue);
                body["authenticated"] = true;
                body["csrf_token"] = rawCsrfToken;
                Json::Value admin(Json::objectValue);
                admin["id"] = adminctx::userId(req);
                admin["username"] = adminctx::username(req);
                body["admin"] = admin;
                callback(jsonResponse(body, 200));
            },
            [req, callback](const std::string &error) {
                LOG_ERROR << "管理会话引导轮换 CSRF 令牌失败：" << error
                          << " request_id=" << http::requestId(req);
                callback(errorResponse(ErrorCode::InternalError, "服务内部错误", 0,
                                       http::requestId(req)));
            });
    }

    // ------------------------------------------------------------------
    // 管理员登录。请求体 {"username": "...", "password": "..."}。
    // 账号不存在与口令错误对外完全同形（AdminAuthService::login 还会对不存在的
    // 账号跑一次等价的 PBKDF2，抹平时间差）。
    // ------------------------------------------------------------------
    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        Json::Value body;
        if (!ctrl::parseBody(req, callback, body)) {
            return;
        }
        const std::string username = readString(body, "username");
        const std::string password = readString(body, "password");
        if (username.empty() || username.size() > 64 ||
            password.empty() || password.size() > 1024) {
            callback(ctrl::fail(ErrorCode::ValidationError, "用户名或密码格式不正确", req));
            return;
        }

        const AppConfig &cfg = AppConfig::instance();
        AdminAuthService::instance().login(
            username, password,
            [req, callback, &cfg, username](AdminAuthService::LoginResult result) {
                switch (result.outcome) {
                    case AdminAuthService::LoginOutcome::Success: {
                        Json::Value body(Json::objectValue);
                        body["authenticated"] = true;
                        body["csrf_token"] = result.rawCsrfToken;
                        Json::Value admin(Json::objectValue);
                        admin["id"] = result.adminUserId;
                        admin["username"] = username;
                        body["admin"] = admin;
                        auto resp = jsonResponse(body, 200);
                        http::setSessionCookie(resp, cfg.adminSessionCookieName,
                                               result.rawSessionToken,
                                               AdminAuthService::kAdminSessionTtlSeconds);
                        callback(resp);
                        return;
                    }
                    case AdminAuthService::LoginOutcome::InvalidCredentials:
                        callback(errorResponse(ErrorCode::Unauthenticated,
                                               "用户名或密码不正确", 0,
                                               http::requestId(req)));
                        return;
                    case AdminAuthService::LoginOutcome::Validation:
                        callback(ctrl::fail(ErrorCode::ValidationError,
                                            "用户名或密码格式不正确", req));
                        return;
                    case AdminAuthService::LoginOutcome::Error:
                    default:
                        LOG_ERROR << "管理员登录失败：" << result.error
                                  << " request_id=" << http::requestId(req);
                        callback(errorResponse(ErrorCode::InternalError, "服务内部错误", 0,
                                               http::requestId(req)));
                        return;
                }
            });
    }

    // ------------------------------------------------------------------
    // 管理员注销。与用户注销同样：服务端撤销失败也必须清掉 Cookie。
    // ------------------------------------------------------------------
    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const AppConfig &cfg = AppConfig::instance();
        const std::string sessionId = adminctx::sessionId(req);
        auto respondCleared = [callback, &cfg]() {
            auto resp = noContentResponse();
            http::clearSessionCookie(resp, cfg.adminSessionCookieName);
            callback(resp);
        };
        if (sessionId.empty()) {
            respondCleared();
            return;
        }
        AdminAuthService::instance().revokeSession(
            sessionId, "admin_logout", respondCleared,
            [req, respondCleared](const std::string &error) {
                LOG_ERROR << "撤销管理会话失败（仍清空客户端 Cookie）：" << error
                          << " request_id=" << http::requestId(req);
                respondCleared();
            });
    }

    // ------------------------------------------------------------------
    // 用户列表。读的是 0005 里的只读视图 admin_user_overview。
    // query 是模糊搜索：绑定的参数不会造成 SQL 注入，但其中的 % 与 _
    // 会被当作通配符——对后台搜索框而言这是可接受的。
    // ------------------------------------------------------------------
    void listUsers(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const int limit = ctrl::clampLimit(req);
        long long offset = 0;
        if (!ctrl::decodeCursor(req->getParameter("cursor"), offset)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效", req));
            return;
        }
        const std::string query = req->getParameter("query");
        static const char *kSql =
            R"(SELECT user_id::text AS id,
                      email,
                      display_name,
                      status,
                      to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
                 FROM admin_user_overview
                WHERE $1 = '' OR email ILIKE '%' || $1 || '%' OR display_name ILIKE '%' || $1 || '%'
                ORDER BY created_at DESC, user_id
                LIMIT $2::integer OFFSET $3::bigint)";
        db::Pool::instance().client()->execSqlAsync(
            kSql,
            [req, callback, limit, offset](const drogon::orm::Result &rows) {
                Json::Value items(Json::arrayValue);
                for (const auto &row : rows) {
                    Json::Value item(Json::objectValue);
                    item["id"] = row["id"].as<std::string>();
                    item["email"] = row["email"].as<std::string>();
                    item["display_name"] = row["display_name"].as<std::string>();
                    item["status"] = row["status"].as<std::string>();
                    item["created_at"] = row["created_at"].as<std::string>();
                    items.append(item);
                }
                Json::Value body(Json::objectValue);
                body["items"] = items;
                body["next_cursor"] = ctrl::nextCursorOrNull(rows.size(), limit, offset);
                callback(jsonResponse(body, 200));
            },
            [req, callback](const drogon::orm::DrogonDbException &e) {
                respondListError(req, callback, "查询用户列表失败",
                                 db::describePgError("查询用户列表失败", e.base()));
            },
            query, limit, offset);
    }

    // ------------------------------------------------------------------
    // 空间列表。读视图 admin_couple_overview，只回契约声明的字段。
    // ------------------------------------------------------------------
    void listCouples(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const int limit = ctrl::clampLimit(req);
        long long offset = 0;
        if (!ctrl::decodeCursor(req->getParameter("cursor"), offset)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效", req));
            return;
        }
        static const char *kSql =
            R"(SELECT couple_id::text AS id,
                      name,
                      member_count,
                      to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
                 FROM admin_couple_overview
                ORDER BY created_at DESC, couple_id
                LIMIT $1::integer OFFSET $2::bigint)";
        db::Pool::instance().client()->execSqlAsync(
            kSql,
            [req, callback, limit, offset](const drogon::orm::Result &rows) {
                Json::Value items(Json::arrayValue);
                for (const auto &row : rows) {
                    Json::Value item(Json::objectValue);
                    item["id"] = row["id"].as<std::string>();
                    item["name"] = row["name"].as<std::string>();
                    item["member_count"] = row["member_count"].as<int>();
                    item["created_at"] = row["created_at"].as<std::string>();
                    items.append(item);
                }
                Json::Value body(Json::objectValue);
                body["items"] = items;
                body["next_cursor"] = ctrl::nextCursorOrNull(rows.size(), limit, offset);
                callback(jsonResponse(body, 200));
            },
            [req, callback](const drogon::orm::DrogonDbException &e) {
                respondListError(req, callback, "查询空间列表失败",
                                 db::describePgError("查询空间列表失败", e.base()));
            },
            limit, offset);
    }

    // ------------------------------------------------------------------
    // 审计日志。按 id 倒序（bigserial 单调递增，等价于按时间倒序且无并列）。
    // 刻意不回 detail 字段：契约没声明它，而且它可能含面向前端的内部细节。
    // ------------------------------------------------------------------
    void listAuditLogs(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const int limit = ctrl::clampLimit(req);
        long long offset = 0;
        if (!ctrl::decodeCursor(req->getParameter("cursor"), offset)) {
            callback(ctrl::fail(ErrorCode::ValidationError, "游标无效", req));
            return;
        }
        static const char *kSql =
            R"(SELECT id,
                      actor_type,
                      action,
                      to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at
                 FROM audit_logs
                ORDER BY id DESC
                LIMIT $1::integer OFFSET $2::bigint)";
        db::Pool::instance().client()->execSqlAsync(
            kSql,
            [req, callback, limit, offset](const drogon::orm::Result &rows) {
                Json::Value items(Json::arrayValue);
                for (const auto &row : rows) {
                    Json::Value item(Json::objectValue);
                    // id 是 bigserial：用 Int64 而不是 int，避免大表溢出。
                    item["id"] = static_cast<Json::Int64>(row["id"].as<long long>());
                    item["actor_type"] = row["actor_type"].as<std::string>();
                    item["action"] = row["action"].as<std::string>();
                    item["created_at"] = row["created_at"].as<std::string>();
                    items.append(item);
                }
                Json::Value body(Json::objectValue);
                body["items"] = items;
                body["next_cursor"] = ctrl::nextCursorOrNull(rows.size(), limit, offset);
                callback(jsonResponse(body, 200));
            },
            [req, callback](const drogon::orm::DrogonDbException &e) {
                respondListError(req, callback, "查询审计日志失败",
                                 db::describePgError("查询审计日志失败", e.base()));
            },
            limit, offset);
    }
};

}  // namespace usapp::controllers
