#include "services/AdminAnniversaryService.h"
#include <drogon/drogon.h>
#include <climits>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {
#define TS(x) "to_char((" x ") AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define DATE(x) "to_char(" x ",'YYYY-MM-DD')"
#define SPACE "'space',(SELECT jsonb_build_object('id',id,'status',status) FROM couples WHERE id=$1::uuid)"
#define READ_AT "'read_at'," TS("statement_timestamp()")
#define REVISION " JOIN LATERAL(SELECT x.version,x.title,x.event_date,x.kind,x.note,x.created_at FROM anniversary_revisions x WHERE x.anniversary_id=a.id AND x.couple_id=a.couple_id ORDER BY x.version DESC LIMIT 1) r ON true "
#define CORE "'id',a.id,'creator_id',a.creator_id,'created_at'," TS("a.created_at") ",'updated_at'," TS("r.created_at") ",'version',r.version,'title',r.title,'date'," DATE("r.event_date") ",'kind',r.kind"
// Scope every child through its parent. No pin, notification, request or receipt fields are projected.
const char *kList=
 "WITH page AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM anniversaries WHERE couple_id=$1::uuid AND ($2::bigint=0 OR ((extract(epoch FROM created_at)*1000000)::bigint,id)<($2::bigint,$3::uuid)) ORDER BY created_at DESC,id DESC LIMIT $4::integer+1) "
 "SELECT jsonb_build_object(" SPACE ",'items',(SELECT COALESCE(jsonb_agg(jsonb_build_object(" CORE ") ORDER BY a.created_at DESC,a.id DESC),'[]') FROM page a " REVISION "),'_keys',(SELECT COALESCE(jsonb_agg(jsonb_build_object('id',id,'micros',((extract(epoch FROM created_at)*1000000)::bigint)::text) ORDER BY created_at DESC,id DESC),'[]') FROM page)," READ_AT ")::text AS payload";
const char *kDetail=
 "WITH target AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM anniversaries WHERE couple_id=$1::uuid AND id=$2::uuid) "
 "SELECT jsonb_build_object(" SPACE ",'anniversary',(SELECT jsonb_build_object(" CORE ",'note',r.note) FROM target a " REVISION "),"
 "'pending_proposal',(SELECT jsonb_build_object('id',p.id,'proposer_id',p.proposer_id,'base_version',p.base_version,'title',p.title,'date'," DATE("p.event_date") ",'kind',p.kind,'note',p.note,'created_at'," TS("p.created_at") ") FROM target a JOIN anniversary_edit_proposals p ON p.anniversary_id=a.id AND p.couple_id=a.couple_id WHERE NOT EXISTS(SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id=p.id AND d.couple_id=p.couple_id) ORDER BY p.created_at DESC,p.id DESC LIMIT 1),"
 "'last_decision',(SELECT jsonb_build_object('proposal_id',p.id,'proposer_id',p.proposer_id,'base_version',p.base_version,'outcome',d.outcome,'decided_by',d.decided_by,'decided_at'," TS("d.decided_at") ",'approved_version',ar.version) FROM target a JOIN anniversary_edit_proposals p ON p.anniversary_id=a.id AND p.couple_id=a.couple_id JOIN anniversary_edit_decisions d ON d.proposal_id=p.id AND d.couple_id=p.couple_id LEFT JOIN anniversary_revisions ar ON ar.proposal_id=p.id AND ar.couple_id=p.couple_id ORDER BY d.decided_at DESC,d.proposal_id DESC LIMIT 1)," READ_AT ")::text AS payload";
bool uuid(const std::string &s) {
 if(s.size()!=36)return false;
 for(std::size_t i=0;i<s.size();++i){char ch=s[i];if(i==8||i==13||i==18||i==23){if(ch!='-')return false;}else if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;}return true;
}
bool positive(const std::string &s,std::int64_t &out){
 if(s.empty()||s.size()>19||s[0]=='0')return false;std::uint64_t n=0;
 for(char c:s){if(c<'0'||c>'9')return false;n=n*10+static_cast<unsigned>(c-'0');}
 if(n>static_cast<std::uint64_t>(INT64_MAX))return false;out=static_cast<std::int64_t>(n);return true;
}
bool json(const std::string &s,Json::Value &out){Json::CharReaderBuilder b;std::unique_ptr<Json::CharReader> reader(b.newCharReader());std::string error;return reader->parse(s.data(),s.data()+s.size(),&out,&error)&&out.isObject();}
using A=AdminAnniversaryService;
A::Result result(const drogon::orm::Result &rows,bool detail=false){
 A::Result r;if(rows.size()!=1u||!json(rows[0]["payload"].as<std::string>(),r.body))return {};
 if(r.body["space"].isNull()){r.body=Json::nullValue;r.outcome=A::Outcome::SpaceNotFound;return r;}
 if(detail&&r.body["anniversary"].isNull()){r.body=Json::nullValue;r.outcome=A::Outcome::NotFound;return r;}
 r.outcome=A::Outcome::Ok;return r;
}
} // namespace
std::string A::encodeCursor(const std::string &couple,const Cursor &c){return "an1."+std::to_string(c.createdMicros)+"."+c.anniversaryId+"."+couple;}
bool A::decodeCursor(const std::string &raw,const std::string &couple,Cursor &out){
 if(raw.size()>160||raw.compare(0,4,"an1.")!=0||!uuid(couple))return false;auto dot=raw.find('.',4);if(dot==std::string::npos)return false;auto tail=raw.substr(dot+1);
 if(tail.size()!=73||tail[36]!='.'||tail.substr(37)!=couple||!uuid(tail.substr(0,36))||!positive(raw.substr(4,dot-4),out.createdMicros))return false;out.anniversaryId=tail.substr(0,36);return true;
}
void A::list(const std::string &couple,int limit,const Cursor *cursor,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));const long long micros=cursor?cursor->createdMicros:0LL;const std::string after=cursor?cursor->anniversaryId:"00000000-0000-0000-0000-000000000000";
 db::Pool::instance().client()->execSqlAsync(kList,[done,couple,limit](const drogon::orm::Result &rows){auto r=result(rows);if(r.outcome!=Outcome::Ok){(*done)(std::move(r));return;}auto &items=r.body["items"];auto keys=r.body["_keys"];if(!items.isArray()||!keys.isArray()||items.size()!=keys.size()){(*done)({});return;}r.body["next_cursor"]=Json::nullValue;if(items.size()>static_cast<unsigned>(limit)){Cursor next;if(!positive(keys[limit-1]["micros"].asString(),next.createdMicros)){(*done)({});return;}next.anniversaryId=keys[limit-1]["id"].asString();r.body["next_cursor"]=encodeCursor(couple,next);items.resize(static_cast<unsigned>(limit));}r.body.removeMember("_keys");(*done)(std::move(r));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin anniversary list failed",e.base());(*done)({});},couple,micros,after,limit);
}
void A::detail(const std::string &couple,const std::string &anniversary,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));db::Pool::instance().client()->execSqlAsync(kDetail,[done](const drogon::orm::Result &rows){(*done)(result(rows,true));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin anniversary detail failed",e.base());(*done)({});},couple,anniversary);
}
}
