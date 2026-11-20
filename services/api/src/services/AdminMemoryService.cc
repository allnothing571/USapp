#include "services/AdminMemoryService.h"
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
#define TARGET "WITH target AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM memories WHERE couple_id=$1::uuid AND id=$2::uuid AND NOT usapp_memory_deleted_now(id)) "
#define REVISION " JOIN LATERAL(SELECT x.* FROM memory_revisions x WHERE x.memory_id=m.id AND x.couple_id=m.couple_id ORDER BY x.version DESC LIMIT 1) r ON true JOIN memory_texts tx ON tx.id=r.text_id AND tx.couple_id=m.couple_id "
// Withheld entries preserve position only. Never expose a deleted/purged media identifier through a historical snapshot.
#define PHOTOS(table,cond) "(SELECT COALESCE(jsonb_agg(CASE WHEN usapp_media_deleted_now(ph.media_id) THEN jsonb_build_object('position',ph.position,'available',false) ELSE jsonb_build_object('position',ph.position,'available',true,'media_id',ph.media_id,'uploader_id',mo.uploader_id,'width',mt.width,'height',mt.height) END ORDER BY ph.position),'[]') FROM " table " ph JOIN media_objects mo ON mo.id=ph.media_id AND mo.couple_id=ph.couple_id LEFT JOIN media_upload_outcomes mt ON mt.media_id=ph.media_id WHERE " cond ")"
#define CORE "'id',m.id,'creator_id',m.creator_id,'created_at'," TS("m.created_at") ",'updated_at'," TS("r.created_at") ",'version',r.version,'date'," DATE("r.memory_date") ",'title',tx.title,'photo_count',(SELECT count(*)::integer FROM memory_revision_photos ph WHERE ph.memory_id=m.id AND ph.version=r.version AND NOT usapp_media_deleted_now(ph.media_id))"
#define SUMMARY "'id',p.id,'memory_id',p.memory_id,'proposer_id',p.proposer_id,'base_version',p.base_version,'created_at'," TS("p.created_at") ",'status',COALESCE(d.outcome,'pending'),'decided_by',d.decided_by,'decided_at'," TS("d.decided_at") ",'approved_version',ar.version"
#define DECISIONS " LEFT JOIN memory_edit_decisions d ON d.proposal_id=p.id AND d.couple_id=p.couple_id LEFT JOIN memory_revisions ar ON ar.proposal_id=p.id AND ar.couple_id=p.couple_id "
const char *kList=
 "WITH page AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM memories WHERE couple_id=$1::uuid AND NOT usapp_memory_deleted_now(id) AND ($2::bigint=0 OR ((extract(epoch FROM created_at)*1000000)::bigint,id)<($2::bigint,$3::uuid)) ORDER BY created_at DESC,id DESC LIMIT $4::integer+1) "
 "SELECT jsonb_build_object(" SPACE ",'availability',CASE WHEN usapp_recovery_closed() THEN 'unavailable' ELSE 'available' END,'items',(SELECT COALESCE(jsonb_agg(jsonb_build_object(" CORE ") ORDER BY m.created_at DESC,m.id DESC),'[]') FROM page m " REVISION "),'_keys',(SELECT COALESCE(jsonb_agg(jsonb_build_object('id',id,'micros',((extract(epoch FROM created_at)*1000000)::bigint)::text) ORDER BY created_at DESC,id DESC),'[]') FROM page)," READ_AT ")::text AS payload";
const char *kDetail=TARGET
 "SELECT jsonb_build_object(" SPACE ",'memory',(SELECT jsonb_build_object(" CORE ",'description',tx.description,'photos'," PHOTOS("memory_revision_photos","ph.memory_id=m.id AND ph.version=r.version AND ph.couple_id=m.couple_id") ") FROM target m " REVISION ")," READ_AT ")::text AS payload";
const char *kProposals=TARGET
 ",page AS MATERIALIZED(SELECT p.* FROM memory_edit_proposals p JOIN target m ON m.id=p.memory_id AND m.couple_id=p.couple_id WHERE ($3::bigint=0 OR ((extract(epoch FROM p.created_at)*1000000)::bigint,p.id)<($3::bigint,$4::uuid)) ORDER BY p.created_at DESC,p.id DESC LIMIT $5::integer+1) "
 "SELECT jsonb_build_object(" SPACE ",'memory_id',(SELECT id FROM target),'current_version',(SELECT max(r.version) FROM memory_revisions r JOIN target m ON m.id=r.memory_id AND m.couple_id=r.couple_id),'items',(SELECT COALESCE(jsonb_agg(jsonb_build_object(" SUMMARY ") ORDER BY p.created_at DESC,p.id DESC),'[]') FROM page p " DECISIONS "),'_keys',(SELECT COALESCE(jsonb_agg(jsonb_build_object('id',id,'micros',((extract(epoch FROM created_at)*1000000)::bigint)::text) ORDER BY created_at DESC,id DESC),'[]') FROM page)," READ_AT ")::text AS payload";
const char *kComparison=TARGET
 "SELECT jsonb_build_object(" SPACE ",'memory_id',(SELECT id FROM target),'current_version',(SELECT max(r.version) FROM memory_revisions r JOIN target m ON m.id=r.memory_id AND m.couple_id=r.couple_id),'comparison',(SELECT jsonb_build_object('proposal',jsonb_build_object(" SUMMARY "),'original',jsonb_build_object('version',b.version,'date'," DATE("b.memory_date") ",'title',bt.title,'description',bt.description,'photos'," PHOTOS("memory_revision_photos","ph.memory_id=m.id AND ph.version=b.version AND ph.couple_id=m.couple_id") "),'proposed',jsonb_build_object('date'," DATE("p.memory_date") ",'title',pt.title,'description',pt.description,'photos'," PHOTOS("memory_proposal_photos","ph.proposal_id=p.id AND ph.couple_id=m.couple_id") ")) FROM target m JOIN memory_edit_proposals p ON p.memory_id=m.id AND p.couple_id=m.couple_id AND p.id=$3::uuid JOIN memory_texts pt ON pt.id=p.text_id AND pt.couple_id=m.couple_id JOIN memory_revisions b ON b.memory_id=m.id AND b.version=p.base_version AND b.couple_id=m.couple_id JOIN memory_texts bt ON bt.id=b.text_id AND bt.couple_id=m.couple_id " DECISIONS ")," READ_AT ")::text AS payload";
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
using A=AdminMemoryService;
A::Result result(const drogon::orm::Result &rows,const char *target=nullptr){
 A::Result r;if(rows.size()!=1u||!json(rows[0]["payload"].as<std::string>(),r.body))return {};
 if(r.body["space"].isNull()){r.body=Json::nullValue;r.outcome=A::Outcome::SpaceNotFound;return r;}
 if(target&&r.body[target].isNull()){r.body=Json::nullValue;r.outcome=A::Outcome::NotFound;return r;}
 r.outcome=A::Outcome::Ok;return r;
}
void paged(A::Result &r,const std::string &couple,int limit,const std::string &memory){
 if(r.outcome!=A::Outcome::Ok)return;auto &items=r.body["items"];auto keys=r.body["_keys"];
 if(!items.isArray()||!keys.isArray()||items.size()!=keys.size()){r={};return;}
 r.body["next_cursor"]=Json::nullValue;
 if(items.size()>static_cast<unsigned>(limit)){A::Cursor next;if(!positive(keys[limit-1]["micros"].asString(),next.createdMicros)){r={};return;}next.id=keys[limit-1]["id"].asString();r.body["next_cursor"]=A::encodeCursor(couple,next,memory);items.resize(static_cast<unsigned>(limit));}
 r.body.removeMember("_keys");
}
} // namespace
std::string A::encodeCursor(const std::string &couple,const Cursor &c,const std::string &memory){return std::string(memory.empty()?"am1.":"mp1.")+std::to_string(c.createdMicros)+"."+c.id+"."+(memory.empty()?"":memory+".")+couple;}
bool A::decodeCursor(const std::string &raw,const std::string &couple,Cursor &out,const std::string &memory){
 const std::string prefix=memory.empty()?"am1.":"mp1.";
 if(raw.size()>200||raw.compare(0,4,prefix)!=0||!uuid(couple)||(!memory.empty()&&!uuid(memory)))return false;
 auto dot=raw.find('.',4);if(dot==std::string::npos)return false;auto tail=raw.substr(dot+1);
 const auto suffix=memory.empty()?couple:memory+"."+couple;
 if(tail.size()!=37+suffix.size()||tail[36]!='.'||tail.substr(37)!=suffix||!uuid(tail.substr(0,36))||!positive(raw.substr(4,dot-4),out.createdMicros))return false;
 out.id=tail.substr(0,36);return true;
}
void A::list(const std::string &couple,int limit,const Cursor *cursor,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));const long long micros=cursor?cursor->createdMicros:0LL;const std::string after=cursor?cursor->id:"00000000-0000-0000-0000-000000000000";
 db::Pool::instance().client()->execSqlAsync(kList,[done,couple,limit](const drogon::orm::Result &rows){auto r=result(rows);paged(r,couple,limit,"");(*done)(std::move(r));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin memory list failed",e.base());(*done)({});},couple,micros,after,limit);
}
void A::detail(const std::string &couple,const std::string &memory,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));db::Pool::instance().client()->execSqlAsync(kDetail,[done](const drogon::orm::Result &rows){(*done)(result(rows,"memory"));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin memory detail failed",e.base());(*done)({});},couple,memory);
}
void A::proposals(const std::string &couple,const std::string &memory,int limit,const Cursor *cursor,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));const long long micros=cursor?cursor->createdMicros:0LL;const std::string after=cursor?cursor->id:"00000000-0000-0000-0000-000000000000";
 db::Pool::instance().client()->execSqlAsync(kProposals,[done,couple,memory,limit](const drogon::orm::Result &rows){auto r=result(rows,"memory_id");paged(r,couple,limit,memory);(*done)(std::move(r));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin memory proposals failed",e.base());(*done)({});},couple,memory,micros,after,limit);
}
void A::comparison(const std::string &couple,const std::string &memory,const std::string &proposal,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));db::Pool::instance().client()->execSqlAsync(kComparison,[done](const drogon::orm::Result &rows){(*done)(result(rows,"comparison"));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin memory comparison failed",e.base());(*done)({});},couple,memory,proposal);
}
}
