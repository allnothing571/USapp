#pragma once
#include "AlbumDbTests.h"
#include "MediaDeletionDbTests.h"
#include "MemoryDeletionDbTests.h"
#include "services/AdminAlbumService.h"
namespace usapptest::adminalbum {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace md=usapptest::media;namespace al=usapptest::album;namespace del=usapptest::mediadeletion;
using A=usapp::AdminAlbumService;
inline std::string create(const c::ClientPtr &,const std::string &user,const std::string &name="Admin album"){
 auto made=al::create(user,name);if(!al::is(made,al::AO::Created))throw std::runtime_error("album fixture");return al::idOf(made);
}
inline auto list(const std::string &couple,int limit=100,const std::string &raw=""){
 A::Cursor cursor;if(!raw.empty()&&!A::decodeCursor(raw,couple,cursor))throw std::runtime_error("cursor fixture");
 auto p=std::make_shared<ts::Pending<A::Result>>();A::list(couple,limit,raw.empty()?nullptr:&cursor,[p](auto r){p->deliver(std::move(r));});return p->wait();
}
inline auto detail(const std::string &couple,const std::string &album){auto p=std::make_shared<ts::Pending<A::Result>>();A::detail(couple,album,[p](auto r){p->deliver(std::move(r));});return p->wait();}
inline auto items(const std::string &couple,const std::string &album,bool history=false,int limit=100,const std::string &cursor=""){
 int before=0;if(!cursor.empty()&&!A::decodeItemCursor(cursor,couple,album,history,before))throw std::runtime_error("item cursor");
 auto p=std::make_shared<ts::Pending<A::Result>>();A::items(couple,album,history,limit,before,[p](auto r){p->deliver(std::move(r));});return p->wait();
}
inline bool ok(const std::optional<A::Result>&v){return v&&v->outcome==A::Outcome::Ok;}
inline std::string json(const Json::Value &v){Json::StreamWriterBuilder w;return Json::writeString(w,v);}
inline std::string footprint(const c::Observer &o){return o.client->execSqlSync("SELECT concat_ws('|',(SELECT count(*) FROM albums),(SELECT count(*) FROM album_texts),(SELECT count(*) FROM album_revisions),(SELECT count(*) FROM album_items),(SELECT count(*) FROM album_photo_batches),(SELECT count(*) FROM album_item_removals),(SELECT count(*) FROM album_cover_events),(SELECT count(*) FROM media_bindings),(SELECT count(*) FROM media_deletions),(SELECT count(*) FROM media_cleanup_passes),(SELECT count(*) FROM notifications),(SELECT count(*) FROM wallet_operations),(SELECT count(*) FROM audit_logs)) AS f")[0]["f"].as<std::string>();}
inline void safe(const Json::Value &v){const std::set<std::string> fields={"space","id","status","availability","items","next_cursor","read_at","album_id","view","creator_id","created_at","name","version","photo_count","cover_media_id","album","history_count","seq","media_id","uploader_id","added_by","added_at","width","height","removed_by","removed_at"};if(v.isObject())for(const auto &k:v.getMemberNames()){CHECK(fields.count(k)==1u);safe(v[k]);}else if(v.isArray())for(const auto &row:v)safe(row);}
inline void run(const c::ClientPtr &pool,const std::string &dsn){
 md::prepareEnv(pool,dsn);const auto observer=c::Observer::connect(dsn);
 TEST_CASE("Admin albums: same-couple snapshots, current/removal history, authoritative cover, deletion/undo/purge hiding and read-only"){
  try{md::useStore();const auto s=md::space(),other=md::space();const auto album=create(pool,s.userA),foreign=create(pool,other.userA,"FOREIGN-ALBUM-NAME");
   const auto first=del::readyPhoto(s.userA),second=del::agedWithObjects(s,s.userB),third=del::readyPhoto(s.userA);
   CHECK(al::is(al::add(s.userA,album,{first,third}),al::AO::Created));CHECK(al::is(al::add(s.userB,album,{second}),al::AO::Created));CHECK(al::is(al::cover(s.userA,album,first),al::AO::Created));
   CHECK(al::is(al::removePhoto(s.userB,album,second),al::AO::Ok));
   const auto before=footprint(observer);const auto d=detail(s.couple,album);CHECK(ok(d));if(!ok(d))return;
   CHECK_EQ(d->body["album"]["photo_count"].asString(),std::string("2"));CHECK_EQ(d->body["history_count"].asString(),std::string("3"));CHECK_EQ(d->body["album"]["cover_media_id"].asString(),first);safe(d->body);
   const auto current=items(s.couple,album),history=items(s.couple,album,true);CHECK(ok(current));CHECK(ok(history));if(!ok(current)||!ok(history))return;
   CHECK_EQ(current->body["items"].size(),2u);CHECK_EQ(history->body["items"].size(),3u);CHECK_EQ(history->body["items"][0]["media_id"].asString(),second);CHECK_EQ(history->body["items"][0]["removed_by"].asString(),s.userB);safe(current->body);safe(history->body);
   CHECK(detail(s.couple,foreign)->outcome==A::Outcome::AlbumNotFound);CHECK(items(s.couple,foreign)->outcome==A::Outcome::AlbumNotFound);CHECK(list(c::newUuid())->outcome==A::Outcome::SpaceNotFound);
   CHECK_EQ(footprint(observer),before);
   const auto deleted=del::erase(s.userA,first);CHECK(md::is(deleted,md::MO::Created));
   const auto hidden=detail(s.couple,album);CHECK(ok(hidden));if(!ok(hidden))return;CHECK_EQ(hidden->body["album"]["cover_media_id"].asString(),third);CHECK_EQ(hidden->body["history_count"].asString(),std::string("2"));CHECK(json(items(s.couple,album,true)->body).find(first)==std::string::npos);
   CHECK(md::is(del::undo(s.userA,first,del::cycleOf(deleted)),md::MO::Ok));CHECK_EQ(detail(s.couple,album)->body["album"]["cover_media_id"].asString(),first);
   // Real fake-store purge of a removed item: no retained identifier in the history afterwards.
   del::cycleAged(second,s.couple,s.userB,"168 hours 1 second");md::tickUntil([&]{return del::sweeps(second)==1;});CHECK(json(items(s.couple,album,true)->body).find(second)==std::string::npos);
   const auto afterDelete=footprint(observer);safe(list(s.couple)->body);safe(detail(s.couple,album)->body);safe(items(s.couple,album,true)->body);CHECK_EQ(footprint(observer),afterDelete);
  }catch(const std::exception &){c::expectThat(false,"admin-album-visibility","fixture/query failure");}
 }
 TEST_CASE("Admin albums: tied-time list pagination and view/album/couple-bound item cursors"){
  try{md::useStore();const auto s=md::space(),other=md::space();std::vector<std::string> expected;for(int i=0;i<5;i++)expected.push_back(create(pool,s.userA,"Album "+std::to_string(i)));
   std::string ids;for(const auto &id:expected){if(!ids.empty())ids+=",";ids+="'"+id+"'";}usapptest::memorydeletion::asMaintenance("UPDATE albums SET created_at='2026-01-01 00:00:00.123456+00' WHERE id IN ("+ids+");");std::sort(expected.rbegin(),expected.rend());
   for(int limit:{1,2,100}){std::string cursor;std::vector<std::string> got;for(int n=0;n<10;n++){auto p=list(s.couple,limit,cursor);CHECK(ok(p));if(!ok(p))break;for(const auto &v:p->body["items"])got.push_back(v["id"].asString());if(p->body["next_cursor"].isNull())break;cursor=p->body["next_cursor"].asString();}CHECK(got==expected);}
   auto p=list(s.couple,1);A::Cursor parsed;CHECK(A::decodeCursor(p->body["next_cursor"].asString(),s.couple,parsed));CHECK_FALSE(A::decodeCursor(p->body["next_cursor"].asString(),other.couple,parsed));CHECK_FALSE(A::decodeCursor("aa1.9223372036854775808."+expected[0]+"."+s.couple,s.couple,parsed));
   std::vector<std::string> photos;for(int i=0;i<4;i++)photos.push_back(del::readyPhoto(s.userA));CHECK(al::is(al::add(s.userA,expected[0],photos),al::AO::Created));
   std::string cursor;std::vector<int> seq;for(int i=0;i<5;i++){auto r=items(s.couple,expected[0],true,1,cursor);CHECK(ok(r));if(!ok(r))break;for(auto &v:r->body["items"])seq.push_back(v["seq"].asInt());if(r->body["next_cursor"].isNull())break;cursor=r->body["next_cursor"].asString();}CHECK(seq==std::vector<int>({4,3,2,1}));
   int before=0;auto raw=A::encodeItemCursor(s.couple,expected[0],true,3);CHECK(A::decodeItemCursor(raw,s.couple,expected[0],true,before));CHECK_EQ(before,3);CHECK_FALSE(A::decodeItemCursor(raw,s.couple,expected[0],false,before));CHECK_FALSE(A::decodeItemCursor(raw,other.couple,expected[0],true,before));CHECK_FALSE(A::decodeItemCursor(raw,s.couple,expected[1],true,before));CHECK_FALSE(A::decodeItemCursor("ai1.history.2147483648."+expected[0]+"."+s.couple,s.couple,expected[0],true,before));
  }catch(const std::exception &){c::expectThat(false,"admin-album-pagination","fixture/query failure");}
 }
}
inline void runClosed(const c::ClientPtr &,const std::string &dsn){
 TEST_CASE("Admin albums: recovery closure hides parent, counts, cover and every item through application-role RLS"){
  try{const char *maint=std::getenv("USAPP_TEST_MAINT_CONNINFO");if(!maint)throw std::runtime_error("maintenance required");auto owner=c::Observer::connect(maint);auto row=owner.client->execSqlSync("SELECT couple_id::text AS couple,id::text AS id FROM albums LIMIT 1");CHECK(!row.empty());if(row.empty())return;const auto couple=row[0]["couple"].as<std::string>(),id=row[0]["id"].as<std::string>();auto p=list(couple);CHECK(ok(p));if(ok(p)){CHECK_EQ(p->body["availability"].asString(),std::string("unavailable"));CHECK_EQ(p->body["items"].size(),0u);CHECK(p->body["next_cursor"].isNull());}CHECK(detail(couple,id)->outcome==A::Outcome::AlbumNotFound);CHECK(items(couple,id,true)->outcome==A::Outcome::AlbumNotFound);
  }catch(const std::exception &){c::expectThat(false,"admin-album-closed","closed-state query failure");}
 }
}
}
