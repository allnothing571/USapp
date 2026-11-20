#include "services/AdminNotificationService.h"
#include <drogon/drogon.h>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// Read stored facts only. No subscription credentials, content targets, claim instance ids or payload text.
// At most limit+1 notifications, five newest delivery generations and one latest attempt per generation.
const char *kPageSql =
    "WITH page AS (SELECT n.id,n.couple_id,n.recipient_id,n.event_type,n.created_at "
    "FROM notifications n WHERE n.recipient_id=$1::uuid "
    "AND ($2::bigint=0 OR ((extract(epoch FROM n.created_at)*1000000)::bigint,n.id)<($2::bigint,$3::uuid)) "
    "ORDER BY n.created_at DESC,n.id DESC LIMIT $4::integer+1) "
    "SELECT (SELECT status FROM users WHERE id=$1::uuid) AS user_status, "
    "(SELECT COALESCE(json_agg(json_build_object('id',n.id,'event_type',n.event_type,'created_at'," USAPP_TS("n.created_at") ", "
    "'read_at'," USAPP_TS("r.read_at") ", 'push',json_build_object("
    "'total',(SELECT count(*)::text FROM push_deliveries z WHERE z.notification_id=n.id AND z.recipient_id=n.recipient_id AND z.couple_id=n.couple_id), "
    "'items',(SELECT COALESCE(json_agg(json_build_object('generation_id',d.generation_id,'created_at'," USAPP_TS("d.created_at") ", "
    "'final_state',f.final_state,'decided_at'," USAPP_TS("f.decided_at") ",'latest_attempt',a.item) ORDER BY d.created_at DESC,d.generation_id DESC),'[]'::json) "
    "FROM (SELECT z.generation_id,z.created_at FROM push_deliveries z WHERE z.notification_id=n.id AND z.recipient_id=n.recipient_id AND z.couple_id=n.couple_id "
    "ORDER BY z.created_at DESC,z.generation_id DESC LIMIT 5) d "
    "LEFT JOIN push_delivery_outcomes f ON f.notification_id=n.id AND f.generation_id=d.generation_id "
    "LEFT JOIN LATERAL (SELECT json_build_object('attempt_no',p.attempt_no,'claimed_at'," USAPP_TS("p.claimed_at") ", "
    "'lease_until'," USAPP_TS("p.lease_until") ",'expired_at'," USAPP_TS("x.expired_at") ", "
    "'observation',CASE WHEN o.attempt_no IS NULL THEN NULL ELSE json_build_object('class',o.class,'outcome',o.outcome,"
    "'http_status',o.http_status,'retry_after_s',o.retry_after_s,'observed_at'," USAPP_TS("o.observed_at") ") END) AS item "
    "FROM push_attempts p LEFT JOIN push_attempt_observations o ON o.notification_id=p.notification_id AND o.generation_id=p.generation_id AND o.attempt_no=p.attempt_no "
    "LEFT JOIN push_attempt_expiries x ON x.notification_id=p.notification_id AND x.generation_id=p.generation_id AND x.attempt_no=p.attempt_no "
    "WHERE p.notification_id=n.id AND p.generation_id=d.generation_id ORDER BY p.attempt_no DESC LIMIT 1) a ON true "
    "))) ORDER BY n.created_at DESC,n.id DESC),'[]'::json) FROM page n "
    "LEFT JOIN notification_reads r ON r.notification_id=n.id AND r.recipient_id=n.recipient_id AND r.couple_id IS NOT DISTINCT FROM n.couple_id)::text AS items, "
    USAPP_TS("statement_timestamp()") " AS read_at, "
    "(SELECT COALESCE(json_agg(json_build_object('id',n.id,'micros',((extract(epoch FROM n.created_at)*1000000)::bigint)::text) "
    "ORDER BY n.created_at DESC,n.id DESC),'[]'::json) FROM page n)::text AS keys";
bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

bool positiveDecimal(const std::string &digits, std::int64_t &out) {
    if (digits.empty() || digits.size() > 19 || digits[0] == '0') return false;
    std::uint64_t value = 0;
    for (char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

// Canonical lowercase uuid only (the controller lowercases path ids; cursors carry what the server encoded).
bool lowerUuid(const std::string &s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace
std::string AdminNotificationService::encodeCursor(const std::string &userId, const Cursor &c) {
    return "n1." + std::to_string(c.createdMicros) + "." + c.notificationId + "." +
           userId;
}

bool AdminNotificationService::decodeCursor(const std::string &raw, const std::string &userId, Cursor &out) {
    if (raw.size() > 160) return false;
    const std::string prefix = "n1.";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    const std::string rest = raw.substr(dot + 1);  // <notificationId>.<userId>
    if (rest.size() != 36 + 1 + 36 || rest[36] != '.') return false;
    const std::string notificationId = rest.substr(0, 36);
    if (!lowerUuid(notificationId) || rest.substr(37) != userId) return false;
    std::int64_t micros = 0;
    if (!positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), micros)) return false;
    out.createdMicros = micros;
    out.notificationId = notificationId;
    return true;
}


void AdminNotificationService::list(const std::string &userId,int limit,const Cursor *cursor,Callback cb) {
    auto done=std::make_shared<Callback>(std::move(cb));
    const auto micros=cursor?static_cast<long long>(cursor->createdMicros):0LL;
    const auto after=cursor?cursor->notificationId:std::string("00000000-0000-0000-0000-000000000000");
    db::Pool::instance().client()->execSqlAsync(kPageSql,
        [done,userId,limit](const drogon::orm::Result &rows){
            Result r;
            if(rows.size()!=1u){(*done)(std::move(r));return;}
            const auto &row=rows[0];
            if(row["user_status"].isNull()){r.outcome=Outcome::UserNotFound;(*done)(std::move(r));return;}
            Json::Value items,keys;
            if(!parseJson(row["items"].as<std::string>(),items)||!items.isArray()||
               !parseJson(row["keys"].as<std::string>(),keys)||!keys.isArray()||keys.size()!=items.size()){(*done)(std::move(r));return;}
            r.body["user"]["id"]=userId;
            r.body["user"]["status"]=row["user_status"].as<std::string>();
            r.body["items"]=Json::Value(Json::arrayValue);
            for(Json::ArrayIndex i=0;i<items.size()&&static_cast<int>(i)<limit;++i)r.body["items"].append(items[i]);
            r.body["next_cursor"]=Json::nullValue;
            if(static_cast<int>(items.size())>limit){
                Cursor next;
                if(!positiveDecimal(keys[limit-1]["micros"].asString(),next.createdMicros)||!lowerUuid(keys[limit-1]["id"].asString())){(*done)(Result{});return;}
                next.notificationId=keys[limit-1]["id"].asString();
                r.body["next_cursor"]=encodeCursor(userId,next);
            }
            r.body["read_at"]=row["read_at"].as<std::string>();r.outcome=Outcome::Ok;(*done)(std::move(r));
        },[done](const drogon::orm::DrogonDbException &e){
            LOG_ERROR<<db::describePgError("admin notification list failed",e.base());(*done)(Result{});
        },userId,micros,after,limit);
}
}
