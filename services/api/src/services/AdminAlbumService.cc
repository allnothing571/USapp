#include "services/AdminAlbumService.h"
#include <drogon/drogon.h>
#include <climits>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {
#define TS(x) "to_char((" x ") AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// Same authoritative deleted-now predicate and active-item rule as AlbumService. Parent and all children are couple-scoped.
#define VISIBLE(i) "NOT usapp_media_deleted_now(" i ".media_id)"
#define ACTIVE(i) VISIBLE(i) " AND NOT EXISTS(SELECT 1 FROM album_item_removals rm WHERE rm.album_id=" i ".album_id AND rm.media_id=" i ".media_id AND rm.couple_id=" i ".couple_id)"
#define CORE "'id',a.id,'creator_id',a.creator_id,'created_at'," TS("a.created_at") ",'name',tx.name,'version',r.version," \
 "'photo_count',(SELECT count(*)::text FROM album_items i WHERE i.album_id=a.id AND i.couple_id=a.couple_id AND " ACTIVE("i") ")," \
 "'cover_media_id',(SELECT i.media_id FROM album_items i WHERE i.album_id=a.id AND i.couple_id=a.couple_id AND " ACTIVE("i") \
 " ORDER BY (i.media_id=(SELECT e.media_id FROM album_cover_events e WHERE e.album_id=a.id AND e.couple_id=a.couple_id ORDER BY e.event_no DESC LIMIT 1)) DESC NULLS LAST,i.seq DESC LIMIT 1)"
#define REVISION " JOIN LATERAL(SELECT x.version,x.text_id FROM album_revisions x WHERE x.album_id=a.id AND x.couple_id=a.couple_id ORDER BY x.version DESC LIMIT 1) r ON true JOIN album_texts tx ON tx.id=r.text_id AND tx.couple_id=a.couple_id "
#define SPACE "'space',(SELECT jsonb_build_object('id',id,'status',status) FROM couples WHERE id=$1::uuid)"
#define READ_AT "'read_at'," TS("statement_timestamp()")
const char *kList=
 "WITH page AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM albums WHERE couple_id=$1::uuid "
 "AND ($2::bigint=0 OR ((extract(epoch FROM created_at)*1000000)::bigint,id)<($2::bigint,$3::uuid)) ORDER BY created_at DESC,id DESC LIMIT $4::integer+1) "
 "SELECT jsonb_build_object(" SPACE ",'availability',CASE WHEN usapp_recovery_closed() THEN 'unavailable' ELSE 'available' END,"
 "'items',(SELECT COALESCE(jsonb_agg(jsonb_build_object(" CORE ") ORDER BY a.created_at DESC,a.id DESC),'[]') FROM page a " REVISION "),"
 "'_keys',(SELECT COALESCE(jsonb_agg(jsonb_build_object('id',a.id,'micros',((extract(epoch FROM a.created_at)*1000000)::bigint)::text) ORDER BY a.created_at DESC,a.id DESC),'[]') FROM page a)," READ_AT ")::text AS payload";
const char *kDetail=
 "WITH target AS MATERIALIZED(SELECT id,couple_id,creator_id,created_at FROM albums WHERE couple_id=$1::uuid AND id=$2::uuid) "
 "SELECT jsonb_build_object(" SPACE ",'album',(SELECT jsonb_build_object(" CORE ") FROM target a " REVISION "),"
 "'history_count',(SELECT count(*)::text FROM album_items i JOIN target a ON a.id=i.album_id AND a.couple_id=i.couple_id WHERE " VISIBLE("i") ")," READ_AT ")::text AS payload";
const char *kItems=
 "WITH target AS MATERIALIZED(SELECT id,couple_id FROM albums WHERE couple_id=$1::uuid AND id=$2::uuid),"
 "page AS MATERIALIZED(SELECT i.seq,i.media_id,mo.uploader_id,i.added_by,i.created_at,mt.width,mt.height,rm.removed_by,rm.removed_at "
 "FROM target a JOIN album_items i ON i.album_id=a.id AND i.couple_id=a.couple_id "
 "JOIN media_objects mo ON mo.id=i.media_id AND mo.couple_id=i.couple_id "
 "LEFT JOIN media_upload_outcomes mt ON mt.media_id=i.media_id "
 "LEFT JOIN album_item_removals rm ON rm.album_id=i.album_id AND rm.media_id=i.media_id AND rm.couple_id=i.couple_id "
 "WHERE " VISIBLE("i") " AND ($3::integer=1 OR rm.media_id IS NULL) AND ($4::integer=0 OR i.seq<$4::integer) ORDER BY i.seq DESC LIMIT $5::integer+1) "
 "SELECT jsonb_build_object(" SPACE ",'album_id',(SELECT id FROM target),'view',CASE WHEN $3::integer=1 THEN 'history' ELSE 'current' END,"
 "'items',(SELECT COALESCE(jsonb_agg(jsonb_build_object('seq',p.seq,'media_id',p.media_id,'uploader_id',p.uploader_id,'added_by',p.added_by,"
 "'added_at'," TS("p.created_at") ",'width',p.width,'height',p.height,'removed_by',p.removed_by,'removed_at'," TS("p.removed_at") ") ORDER BY p.seq DESC),'[]') FROM page p)," READ_AT ")::text AS payload";
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
AdminAlbumService::Result result(const drogon::orm::Result &rows,const char *target=nullptr){
 AdminAlbumService::Result r;
 if(rows.size()!=1u||!json(rows[0]["payload"].as<std::string>(),r.body))return {};
 if(r.body["space"].isNull()){r.body=Json::nullValue;r.outcome=AdminAlbumService::Outcome::SpaceNotFound;return r;}
 if(target&&r.body[target].isNull()){r.body=Json::nullValue;r.outcome=AdminAlbumService::Outcome::AlbumNotFound;return r;}
 r.outcome=AdminAlbumService::Outcome::Ok;return r;
}
} // namespace
std::string AdminAlbumService::encodeCursor(const std::string &couple,const Cursor &c){return "aa1."+std::to_string(c.createdMicros)+"."+c.albumId+"."+couple;}
bool AdminAlbumService::decodeCursor(const std::string &raw,const std::string &couple,Cursor &out){
 if(raw.size()>160||raw.compare(0,4,"aa1.")!=0||!uuid(couple))return false;auto dot=raw.find('.',4);if(dot==std::string::npos)return false;
 auto tail=raw.substr(dot+1);if(tail.size()!=73||tail[36]!='.'||tail.substr(37)!=couple||!uuid(tail.substr(0,36)))return false;
 if(!positive(raw.substr(4,dot-4),out.createdMicros))return false;out.albumId=tail.substr(0,36);return true;
}
std::string AdminAlbumService::encodeItemCursor(const std::string &couple,const std::string &album,bool history,int seq){return std::string("ai1.")+(history?"history.":"current.")+std::to_string(seq)+"."+album+"."+couple;}
bool AdminAlbumService::decodeItemCursor(const std::string &raw,const std::string &couple,const std::string &album,bool history,int &out){
 const std::string prefix=std::string("ai1.")+(history?"history.":"current.");
 if(raw.size()>170||raw.compare(0,prefix.size(),prefix)!=0||!uuid(couple)||!uuid(album))return false;
 auto dot=raw.find('.',prefix.size());if(dot==std::string::npos||raw.substr(dot+1)!=album+"."+couple)return false;
 std::int64_t seq=0;if(!positive(raw.substr(prefix.size(),dot-prefix.size()),seq)||seq>INT_MAX)return false;out=static_cast<int>(seq);return true;
}
void AdminAlbumService::list(const std::string &couple,int limit,const Cursor *cursor,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));const long long micros=cursor?cursor->createdMicros:0LL;
 const std::string after=cursor?cursor->albumId:std::string("00000000-0000-0000-0000-000000000000");
 db::Pool::instance().client()->execSqlAsync(kList,[done,couple,limit](const drogon::orm::Result &rows){
  auto r=result(rows);if(r.outcome!=Outcome::Ok){(*done)(std::move(r));return;}
  auto &items=r.body["items"];auto keys=r.body["_keys"];
  if(!items.isArray()||!keys.isArray()||keys.size()!=items.size()){(*done)({});return;}
  r.body["next_cursor"]=Json::nullValue;
  if(items.size()>static_cast<unsigned>(limit)){Cursor next;if(!positive(keys[limit-1]["micros"].asString(),next.createdMicros)){(*done)({});return;}next.albumId=keys[limit-1]["id"].asString();r.body["next_cursor"]=encodeCursor(couple,next);items.resize(static_cast<unsigned>(limit));}
  r.body.removeMember("_keys");(*done)(std::move(r));
 },[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin album list failed",e.base());(*done)({});},couple,micros,after,limit);
}
void AdminAlbumService::detail(const std::string &couple,const std::string &album,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));db::Pool::instance().client()->execSqlAsync(kDetail,[done](const drogon::orm::Result &rows){(*done)(result(rows,"album"));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin album detail failed",e.base());(*done)({});},couple,album);
}
void AdminAlbumService::items(const std::string &couple,const std::string &album,bool history,int limit,int before,Callback cb){
 auto done=std::make_shared<Callback>(std::move(cb));const int includeRemoved=history?1:0;
 db::Pool::instance().client()->execSqlAsync(kItems,[done,couple,album,history,limit](const drogon::orm::Result &rows){auto r=result(rows,"album_id");if(r.outcome==Outcome::Ok){auto &items=r.body["items"];if(!items.isArray()){(*done)({});return;}r.body["next_cursor"]=Json::nullValue;if(items.size()>static_cast<unsigned>(limit)){r.body["next_cursor"]=encodeItemCursor(couple,album,history,items[limit-1]["seq"].asInt());items.resize(static_cast<unsigned>(limit));}}(*done)(std::move(r));},[done](const drogon::orm::DrogonDbException &e){LOG_ERROR<<db::describePgError("admin album items failed",e.base());(*done)({});},couple,album,includeRemoved,before,limit);
}
}
