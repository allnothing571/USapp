#include "services/AdminUserSessionService.h"
#include <drogon/drogon.h>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// Never read token_hash, csrf_token_hash, raw user agents or rotation references.
// State is time/revocation only, not an assertion that authentication would succeed.
const char *kPageSql =
    "WITH page AS (SELECT s.id,s.created_at,s.last_seen_at,s.expires_at,s.revoked_at, "
    "CASE WHEN s.revoked_at IS NOT NULL THEN 'revoked' WHEN s.expires_at<=statement_timestamp() THEN 'expired' ELSE 'valid' END AS state "
    "FROM sessions s WHERE s.user_id=$1::uuid "
    "AND ($2::bigint=0 OR ((extract(epoch FROM s.created_at)*1000000)::bigint,s.id)<($2::bigint,$3::uuid)) "
    "ORDER BY s.created_at DESC,s.id DESC LIMIT $4::integer+1) "
    "SELECT (SELECT status FROM users WHERE id=$1::uuid) AS user_status, "
    "(SELECT COALESCE(json_agg(json_build_object('id',s.id,'created_at'," USAPP_TS("s.created_at") ", "
    "'last_seen_at'," USAPP_TS("s.last_seen_at") ", 'expires_at'," USAPP_TS("s.expires_at") ", "
    "'revoked_at'," USAPP_TS("s.revoked_at") ", 'state',s.state) ORDER BY s.created_at DESC,s.id DESC),'[]'::json) FROM page s)::text AS items, "
    USAPP_TS("statement_timestamp()") " AS read_at, "
    "(SELECT COALESCE(json_agg(json_build_object('id',s.id,'micros',((extract(epoch FROM s.created_at)*1000000)::bigint)::text) "
    "ORDER BY s.created_at DESC,s.id DESC),'[]'::json) FROM page s)::text AS keys";
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
std::string AdminUserSessionService::encodeCursor(const std::string &userId, const Cursor &c) {
    return "s1." + std::to_string(c.createdMicros) + "." + c.sessionId + "." +
           userId;
}

bool AdminUserSessionService::decodeCursor(const std::string &raw, const std::string &userId, Cursor &out) {
    if (raw.size() > 160) return false;
    const std::string prefix = "s1.";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    const std::string rest = raw.substr(dot + 1);  // <sessionId>.<userId>
    if (rest.size() != 36 + 1 + 36 || rest[36] != '.') return false;
    const std::string sessionId = rest.substr(0, 36);
    if (!lowerUuid(sessionId) || rest.substr(37) != userId) return false;
    std::int64_t micros = 0;
    if (!positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), micros)) return false;
    out.createdMicros = micros;
    out.sessionId = sessionId;
    return true;
}


void AdminUserSessionService::list(const std::string &userId,int limit,const Cursor *cursor,Callback cb) {
    auto done=std::make_shared<Callback>(std::move(cb));
    const auto micros=cursor?static_cast<long long>(cursor->createdMicros):0LL;
    const auto after=cursor?cursor->sessionId:std::string("00000000-0000-0000-0000-000000000000");
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
                next.sessionId=keys[limit-1]["id"].asString();
                r.body["next_cursor"]=encodeCursor(userId,next);
            }
            r.body["read_at"]=row["read_at"].as<std::string>();r.outcome=Outcome::Ok;(*done)(std::move(r));
        },[done](const drogon::orm::DrogonDbException &e){
            LOG_ERROR<<db::describePgError("admin user-session list failed",e.base());(*done)(Result{});
        },userId,micros,after,limit);
}
}
