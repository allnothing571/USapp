// =====================================================================
// services/api/src/services/AuditLog.cc
// =====================================================================
#include "services/AuditLog.h"

#include <drogon/drogon.h>

#include <exception>
#include <memory>

#include "config/AppConfig.h"
#include "db/Db.h"

namespace usapp {

namespace {

const char *kInsertSql =
    "INSERT INTO audit_logs "
    "(actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ($1, "
    "        CASE WHEN $2 = '' THEN NULL ELSE $2::uuid END, "
    "        CASE WHEN $3 = '' THEN NULL ELSE $3::uuid END, "
    "        $4, "
    "        CASE WHEN $5 = '' THEN NULL ELSE $5 END, "
    "        CASE WHEN $6 = '' THEN NULL ELSE $6 END, "
    "        $7::jsonb)";

std::string toJsonText(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

}  // namespace

void AuditLog::record(const std::string &actorType,
                      const std::string &actorId,
                      const std::string &coupleId,
                      const std::string &action,
                      const std::string &targetType,
                      const std::string &targetId,
                      const Json::Value &detail) noexcept {
    try {
        if (!db::Pool::instance().ready()) {
            LOG_WARN << "audit_logs 写入被跳过：数据库连接池尚未初始化 action=" << action;
            return;
        }

        // 防止调用方传进非法 actor_type 触发 CHECK 失败（静默丢审计比报错更糟，
        // 所以这里显式降级为 system 并留痕）。
        std::string safeActor = actorType;
        if (safeActor != "user" && safeActor != "admin" && safeActor != "system") {
            LOG_WARN << "audit_logs actor_type 非法，降级为 system: " << actorType;
            safeActor = "system";
        }

        const std::string detailText = toJsonText(detail);
        const std::string actionCopy = action;

        db::Pool::instance().client()->execSqlAsync(
            kInsertSql,
            [actionCopy](const drogon::orm::Result &) { LOG_DEBUG << "audit_logs 已写入 " << actionCopy; },
            [actionCopy](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << "audit_logs 写入失败 action=" << actionCopy << " err=" << e.base().what();
            },
            safeActor,
            actorId,
            coupleId,
            action,
            targetType,
            targetId,
            detailText);
    } catch (const std::exception &e) {
        LOG_ERROR << "audit_logs 写入异常 action=" << action << " err=" << e.what();
    } catch (...) {
        LOG_ERROR << "audit_logs 写入异常（未知类型） action=" << action;
    }
}

}  // namespace usapp
