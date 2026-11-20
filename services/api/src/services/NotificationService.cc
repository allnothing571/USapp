#include "services/NotificationService.h"
#include "db/Db.h"
#include <memory>
#include <algorithm>

namespace usapp {
namespace {
#define N_TS(x) "to_char(" x " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define N_VISIBLE "(n.admin_adjustment_id IS NOT NULL OR n.admin_cancellation_id IS NOT NULL OR n.login_event_id IS NOT NULL OR EXISTS(SELECT 1 FROM couple_members m WHERE m.couple_id=n.couple_id AND m.user_id=$1::uuid))"
const char *kList =
    "SELECT n.id::text,n.event_type,n.actor_id::text,n.target_kind,n.target_id::text,"
    N_TS("n.created_at") " AS created_at," N_TS("r.read_at") " AS read_at "
    "FROM notifications n "
    "LEFT JOIN notification_reads r ON r.notification_id=n.id "
    "WHERE n.recipient_id=$1::uuid AND " N_VISIBLE " AND (NOT $2::boolean OR r.notification_id IS NULL) "
    "AND ($3='' OR (n.created_at,n.id)<(NULLIF($3,'')::timestamptz,NULLIF($4,'')::uuid)) "
    "ORDER BY n.created_at DESC,n.id DESC LIMIT $5::integer";
}

void NotificationService::list(const std::string &user, int limit, const std::optional<Cursor> &cursor,
                               bool unreadOnly, Callback cb) {
    if (user.empty() || limit < 1 || limit > 100) { cb({400, {}}); return; }
    if (!db::Pool::instance().ready()) { cb({500, {}}); return; }
    std::string before = cursor ? cursor->createdAt : "", id = cursor ? cursor->id : "";
    std::string unread = unreadOnly ? "true" : "false", fetch = std::to_string(limit + 1);
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(kList,
        [done, limit](const drogon::orm::Result &rows) {
            Result result;
            try {
                result.body["notifications"] = Json::Value(Json::arrayValue);
                result.body["next_cursor"] = Json::nullValue;
                size_t count = std::min(rows.size(), static_cast<size_t>(limit));
                for (size_t i = 0; i < count; ++i) {
                    const auto &row = rows[i];
                    Json::Value item;
                    for (const char *key : {"id","event_type","target_kind","target_id","created_at"})
                        item[key] = row[key].as<std::string>();
                    // Reminders and mandatory personal security notices have no member actor.
                    item["actor_id"] = row["actor_id"].isNull() ? Json::Value(Json::nullValue) :
                                       Json::Value(row["actor_id"].as<std::string>());
                    item["read_at"] = row["read_at"].isNull() ? Json::Value(Json::nullValue) :
                                       Json::Value(row["read_at"].as<std::string>());
                    result.body["notifications"].append(item);
                }
                if (rows.size() > count) {
                    const auto &last = rows[count - 1];
                    result.body["next_cursor"] = MessageService::encodeCursor(
                        {last["created_at"].as<std::string>(), last["id"].as<std::string>()});
                }
                result.status = 200;
            } catch (const std::exception &) { result = Result{}; }
            (*done)(std::move(result));
        }, [done](const drogon::orm::DrogonDbException &) { (*done)({500, {}}); },
        user, unread, before, id, fetch);
}

void NotificationService::unreadCount(const std::string &user, Callback cb) {
    if (!db::Pool::instance().ready()) { cb({500, {}}); return; }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        "SELECT count(*) AS n FROM notifications n WHERE n.recipient_id=$1::uuid AND " N_VISIBLE " AND NOT EXISTS "
        "(SELECT 1 FROM notification_reads r WHERE r.notification_id=n.id)",
        [done](const drogon::orm::Result &rows) {
            Result r;
            try { r.body["unread_count"] = Json::Int64(rows[0]["n"].as<long long>()); r.status = 200; }
            catch (const std::exception &) { r = Result{}; }
            (*done)(std::move(r));
        }, [done](const drogon::orm::DrogonDbException &) { (*done)({500, {}}); }, user);
}

void NotificationService::markRead(const std::string &user, const std::string &id, Callback cb) {
    if (!db::Pool::instance().ready()) { cb({500, {}}); return; }
    struct ReadCtx {
        std::shared_ptr<db::TxScope> scope;
        Callback done;
        bool committing = false;
    };
    auto ctx = std::make_shared<ReadCtx>(); ctx->done = std::move(cb);
    std::weak_ptr<ReadCtx> weak = ctx;
    db::TxScope::begin([ctx, user, id](std::shared_ptr<db::TxScope> scope) {
    ctx->scope = std::move(scope);
    // The receipt INSERT is naturally idempotent. Explicit TxScope commit gates success.
    ctx->scope->tx().execSqlAsync(
        "WITH target AS MATERIALIZED (SELECT n.id,n.couple_id,n.recipient_id FROM notifications n "
        "WHERE n.id=$2::uuid AND n.recipient_id=$1::uuid AND " N_VISIBLE "), inserted AS ("
        "INSERT INTO notification_reads(notification_id,couple_id,recipient_id) "
        "SELECT id,couple_id,recipient_id FROM target ON CONFLICT DO NOTHING RETURNING read_at) "
        "SELECT EXISTS(SELECT 1 FROM target) AS found, "
        N_TS("COALESCE((SELECT read_at FROM inserted),(SELECT read_at FROM notification_reads WHERE notification_id=$2::uuid AND recipient_id=$1::uuid))") " AS read_at",
        [ctx, id](const drogon::orm::Result &rows) {
            Result result;
            try {
                if (!rows[0]["found"].as<bool>()) result.status = 404;
                else if (rows[0]["read_at"].isNull()) result.status = 503;
                else {
                    result.status = 200;
                    result.body["receipt"]["notification_id"] = id;
                    result.body["receipt"]["read_at"] = rows[0]["read_at"].as<std::string>();
                }
            } catch (const std::exception &) { result.status = 503; }
            auto response = std::make_shared<Result>(std::move(result));
            if (response->status != 200) {
                ctx->scope->rollback([ctx, response]() { ctx->done(std::move(*response)); });
                return;
            }
            ctx->committing = true;
            ctx->scope->commit([ctx, response]() { ctx->done(std::move(*response)); });
        }, [ctx](const drogon::orm::DrogonDbException &) { ctx->scope->fail("notification_read:query"); }, user, id);
    }, [weak](const std::string &) {
        if (auto c = weak.lock()) c->done({c->committing ? 503 : 500, {}});
    });
}
}
