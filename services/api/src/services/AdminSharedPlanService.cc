#include "services/AdminSharedPlanService.h"
#include <drogon/drogon.h>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define USAPP_PLAN_BODY(v) "jsonb_build_object('title'," v "->'title','date'," v "->'date','time'," v "->'time','place'," v "->'place','note'," v "->'note')"
#define USAPP_PLAN_CORE \
    "'id',p.id,'creator_id',p.creator_id,'created_at'," USAPP_TS("p.created_at") ", " \
    "'title',p.facts->'body'->'title','body_version',(p.facts->>'body_version')::integer," \
    "'status',p.facts->>'status','status_version',(p.facts->>'status_version')::integer"
// Project the existing authoritative read function, once per bounded row; never return its entire JSON.
const char *kListSql=
    "WITH page AS MATERIALIZED (SELECT p.id,p.creator_id,p.created_at FROM shared_plans p WHERE p.couple_id=$1::uuid "
    "AND ($2::bigint=0 OR ((extract(epoch FROM p.created_at)*1000000)::bigint,p.id)<($2::bigint,$3::uuid)) "
    "ORDER BY p.created_at DESC,p.id DESC LIMIT $4::integer+1), "
    "facts AS MATERIALIZED (SELECT p.id,p.creator_id,p.created_at,usapp_shared_plan_read(p.id) AS facts FROM page p) "
    "SELECT (SELECT status FROM couples WHERE id=$1::uuid) AS space_status, "
    "(SELECT COALESCE(json_agg(json_build_object(" USAPP_PLAN_CORE ") ORDER BY p.created_at DESC,p.id DESC),'[]'::json) FROM facts p)::text AS items, "
    "(SELECT COALESCE(json_agg(json_build_object('id',p.id,'micros',((extract(epoch FROM p.created_at)*1000000)::bigint)::text) ORDER BY p.created_at DESC,p.id DESC),'[]'::json) FROM page p)::text AS keys, "
    USAPP_TS("statement_timestamp()") " AS read_at";
const char *kDetailSql=
    "WITH target AS MATERIALIZED (SELECT p.id,p.creator_id,p.created_at,p.couple_id,usapp_shared_plan_read(p.id) AS facts "
    "FROM shared_plans p WHERE p.couple_id=$1::uuid AND p.id=$2::uuid) "
    "SELECT (SELECT status FROM couples WHERE id=$1::uuid) AS space_status, "
    "(SELECT json_build_object(" USAPP_PLAN_CORE ", 'body'," USAPP_PLAN_BODY("(p.facts->'body')") ") FROM target p)::text AS plan, "
    "(SELECT count(*)::text FROM shared_plan_events e JOIN target p ON e.plan_id=p.id AND e.couple_id=p.couple_id) AS event_total, "
    "(SELECT COALESCE(json_agg(json_build_object('id',e.id,'seq',e.seq,'actor_id',e.actor_id,'operation',e.operation, "
    "'body_version',e.body_version,'status_version',e.status_version,'status',e.status,'proposal_id',e.proposal_id,"
    "'outcome',e.outcome,'created_at'," USAPP_TS("e.created_at") ",'body',CASE WHEN e.body IS NULL THEN NULL ELSE " USAPP_PLAN_BODY("e.body") " END) ORDER BY e.seq DESC),'[]'::json) "
    "FROM (SELECT e.id,e.seq,e.actor_id,e.operation,e.body_version,e.status_version,e.status,e.proposal_id,e.outcome,e.created_at,e.body "
    "FROM shared_plan_events e JOIN target p ON e.plan_id=p.id AND e.couple_id=p.couple_id ORDER BY e.seq DESC LIMIT 50) e)::text AS events, "
    USAPP_TS("statement_timestamp()") " AS read_at";
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
std::string AdminSharedPlanService::encodeCursor(const std::string &coupleId, const Cursor &c) {
    return "p1." + std::to_string(c.createdMicros) + "." + c.planId + "." +
           coupleId;
}

bool AdminSharedPlanService::decodeCursor(const std::string &raw, const std::string &coupleId, Cursor &out) {
    if (raw.size() > 160) return false;
    const std::string prefix = "p1.";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    const std::string rest = raw.substr(dot + 1);  // <planId>.<coupleId>
    if (rest.size() != 36 + 1 + 36 || rest[36] != '.') return false;
    const std::string planId = rest.substr(0, 36);
    if (!lowerUuid(planId) || rest.substr(37) != coupleId) return false;
    std::int64_t micros = 0;
    if (!positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), micros)) return false;
    out.createdMicros = micros;
    out.planId = planId;
    return true;
}



void AdminSharedPlanService::list(const std::string &coupleId,int limit,const Cursor *cursor,Callback cb){
    auto done=std::make_shared<Callback>(std::move(cb));
    const long long micros=cursor?static_cast<long long>(cursor->createdMicros):0LL;
    const std::string after=cursor?cursor->planId:std::string("00000000-0000-0000-0000-000000000000");
    db::Pool::instance().client()->execSqlAsync(kListSql,[done,coupleId,limit](const drogon::orm::Result &rows){
        Result r;if(rows.size()!=1u){(*done)(std::move(r));return;}const auto &row=rows[0];
        if(row["space_status"].isNull()){r.outcome=Outcome::SpaceNotFound;(*done)(std::move(r));return;}
        Json::Value items,keys;
        if(!parseJson(row["items"].as<std::string>(),items)||!items.isArray()||!parseJson(row["keys"].as<std::string>(),keys)||!keys.isArray()||keys.size()!=items.size()){(*done)(Result{});return;}
        r.body["space"]["id"]=coupleId;r.body["space"]["status"]=row["space_status"].as<std::string>();
        r.body["items"]=Json::Value(Json::arrayValue);r.body["next_cursor"]=Json::nullValue;
        for(Json::ArrayIndex i=0;i<items.size()&&static_cast<int>(i)<limit;++i)r.body["items"].append(items[i]);
        if(static_cast<int>(items.size())>limit){Cursor next;if(!positiveDecimal(keys[limit-1]["micros"].asString(),next.createdMicros)||!lowerUuid(keys[limit-1]["id"].asString())){(*done)(Result{});return;}next.planId=keys[limit-1]["id"].asString();r.body["next_cursor"]=encodeCursor(coupleId,next);}
        r.body["read_at"]=row["read_at"].as<std::string>();r.outcome=Outcome::Ok;(*done)(std::move(r));
    },[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin shared-plan list failed",e.base());(*done)(Result{});},coupleId,micros,after,limit);
}
void AdminSharedPlanService::detail(const std::string &coupleId,const std::string &planId,Callback cb){
    auto done=std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(kDetailSql,[done,coupleId](const drogon::orm::Result &rows){
        Result r;if(rows.size()!=1u){(*done)(std::move(r));return;}const auto &row=rows[0];
        if(row["space_status"].isNull()){r.outcome=Outcome::SpaceNotFound;(*done)(std::move(r));return;}
        if(row["plan"].isNull()){r.outcome=Outcome::PlanNotFound;(*done)(std::move(r));return;}
        Json::Value plan,events;if(!parseJson(row["plan"].as<std::string>(),plan)||!plan.isObject()||!parseJson(row["events"].as<std::string>(),events)||!events.isArray()){(*done)(Result{});return;}
        r.body["space"]["id"]=coupleId;r.body["space"]["status"]=row["space_status"].as<std::string>();
        r.body["plan"]=plan;r.body["events"]["total"]=row["event_total"].as<std::string>();r.body["events"]["items"]=events;
        r.body["read_at"]=row["read_at"].as<std::string>();r.outcome=Outcome::Ok;(*done)(std::move(r));
    },[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin shared-plan detail failed",e.base());(*done)(Result{});},coupleId,planId);
}
}
