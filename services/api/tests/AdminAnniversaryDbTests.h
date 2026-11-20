#pragma once
#include "AnniversaryDbTests.h"
#include "MemoryDeletionDbTests.h"
#include "services/AdminAnniversaryService.h"
namespace usapptest::adminanniversary {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace an=usapptest::anniversary;
using A=usapp::AdminAnniversaryService;
inline auto list(const std::string &couple,int limit=100,const std::string &raw=""){A::Cursor cursor;if(!raw.empty()&&!A::decodeCursor(raw,couple,cursor))throw std::runtime_error("cursor fixture");auto p=std::make_shared<ts::Pending<A::Result>>();A::list(couple,limit,raw.empty()?nullptr:&cursor,[p](auto r){p->deliver(std::move(r));});return p->wait();}
inline auto detail(const std::string &couple,const std::string &id){auto p=std::make_shared<ts::Pending<A::Result>>();A::detail(couple,id,[p](auto r){p->deliver(std::move(r));});return p->wait();}
inline bool ok(const std::optional<A::Result> &r){return r&&r->outcome==A::Outcome::Ok;}
inline std::string create(const std::string &user,const std::string &title="Admin anniversary"){auto r=an::create(user,an::terms(title,"2020-05-20","anniversary","Current note"));if(!an::is(r,an::AO::Created))throw std::runtime_error("anniversary fixture");return an::idOf(r);}
inline std::string footprint(const c::Observer &o){return o.client->execSqlSync("SELECT concat_ws('|',(SELECT count(*) FROM anniversaries),(SELECT count(*) FROM anniversary_revisions),(SELECT count(*) FROM anniversary_edit_proposals),(SELECT count(*) FROM anniversary_edit_decisions),(SELECT count(*) FROM anniversary_home_pins),(SELECT count(*) FROM audit_logs),(SELECT count(*) FROM notifications),(SELECT count(*) FROM notification_intents),(SELECT count(*) FROM wallet_operations)) AS n")[0]["n"].as<std::string>();}
inline void safe(const Json::Value &v){const std::set<std::string> fields={"space","id","status","items","next_cursor","read_at","creator_id","created_at","updated_at","version","title","date","kind","note","anniversary","pending_proposal","proposer_id","base_version","last_decision","proposal_id","outcome","decided_by","decided_at","approved_version"};if(v.isObject())for(const auto &k:v.getMemberNames()){CHECK(fields.count(k)==1u);safe(v[k]);}else if(v.isArray())for(const auto &x:v)safe(x);}
inline void run(const c::ClientPtr &pool,const std::string &dsn){const auto observer=c::Observer::connect(dsn);
 TEST_CASE("Admin anniversaries: scoped latest content, pending request and historical last decision, no pins or writes"){
  try{ts::CommittedSpace s,other,empty;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);ts::commitSpace(pool,empty,false);const auto id=create(s.userA),foreign=create(other.userA,"FOREIGN-SECRET");auto d=detail(s.couple,id);CHECK(ok(d));if(!ok(d))return;CHECK(d->body["pending_proposal"].isNull());CHECK(d->body["last_decision"].isNull());CHECK_EQ(d->body["anniversary"]["note"].asString(),std::string("Current note"));
   const auto propose=[&](int v){auto r=an::propose(s.userB,id,v,an::terms("Asked title","2027-12-31","countdown","Asked note"));if(!an::is(r,an::AO::Created))throw std::runtime_error("proposal fixture");return r->mutation["proposal_id"].asString();};
   const auto approved=propose(1);CHECK(an::is(an::decide(s.userA,id,approved,an::AS::Decision::Approve),an::AO::Ok));CHECK(an::is(an::edit(s.userA,id,2,an::terms("Later title","2020-01-01","anniversary","Later note")),an::AO::Created));
   d=detail(s.couple,id);CHECK(ok(d));if(!ok(d))return;CHECK_EQ(d->body["anniversary"]["version"].asInt(),3);CHECK_EQ(d->body["last_decision"]["approved_version"].asInt(),2);CHECK_EQ(d->body["anniversary"]["title"].asString(),std::string("Later title"));
   const auto rejected=propose(3);CHECK(an::is(an::decide(s.userA,id,rejected,an::AS::Decision::Reject),an::AO::Ok));CHECK_EQ(detail(s.couple,id)->body["last_decision"]["outcome"].asString(),std::string("rejected"));
   const auto withdrawn=propose(3);CHECK(an::is(an::decide(s.userB,id,withdrawn,an::AS::Decision::Withdraw),an::AO::Ok));const auto pending=propose(3);CHECK(an::is(an::pin(s.userA,id),an::AO::Ok));
   const auto before=footprint(observer);d=detail(s.couple,id);auto page=list(s.couple);CHECK(ok(d));CHECK(ok(page));if(!ok(d)||!ok(page))return;CHECK_EQ(d->body["pending_proposal"]["id"].asString(),pending);CHECK_EQ(d->body["pending_proposal"]["note"].asString(),std::string("Asked note"));CHECK_EQ(d->body["pending_proposal"]["base_version"].asInt(),3);CHECK_EQ(d->body["last_decision"]["proposal_id"].asString(),withdrawn);CHECK(d->body["last_decision"]["approved_version"].isNull());safe(d->body);safe(page->body);
   CHECK(detail(s.couple,foreign)->outcome==A::Outcome::NotFound);CHECK(detail(other.couple,id)->outcome==A::Outcome::NotFound);CHECK(detail(s.couple,c::newUuid())->outcome==A::Outcome::NotFound);CHECK(list(c::newUuid())->outcome==A::Outcome::SpaceNotFound);CHECK(detail(c::newUuid(),id)->outcome==A::Outcome::SpaceNotFound);CHECK_EQ(list(empty.couple)->body["items"].size(),0u);CHECK_EQ(footprint(observer),before);
  }catch(const std::exception &){c::expectThat(false,"admin-anniversary-content","fixture/query failure");}
 }
 TEST_CASE("Admin anniversaries: tied creation time pagination remains stable after mutable date/title edit"){
  try{ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);std::vector<std::string> expected;for(int i=0;i<5;i++)expected.push_back(create(s.userA));std::string ids;for(auto &id:expected){if(!ids.empty())ids+=",";ids+="'"+id+"'";}usapptest::memorydeletion::asMaintenance("UPDATE anniversaries SET created_at='2026-01-01 00:00:00.123456+00' WHERE id IN ("+ids+");");std::sort(expected.rbegin(),expected.rend());
   CHECK(an::is(an::edit(s.userA,expected[0],1,an::terms("Changed date","2199-12-31","countdown")),an::AO::Created));
   for(int limit:{1,2,100}){std::string cursor;std::vector<std::string> got;for(int n=0;n<8;n++){auto p=list(s.couple,limit,cursor);CHECK(ok(p));if(!ok(p))break;for(auto &v:p->body["items"])got.push_back(v["id"].asString());if(p->body["next_cursor"].isNull())break;cursor=p->body["next_cursor"].asString();}CHECK(got==expected);}
   const auto cursor=list(s.couple,1)->body["next_cursor"].asString();A::Cursor decoded;CHECK(A::decodeCursor(cursor,s.couple,decoded));CHECK_FALSE(A::decodeCursor(cursor,other.couple,decoded));for(auto value:{"0","01","-1","9223372036854775808"})CHECK_FALSE(A::decodeCursor(std::string("an1.")+value+"."+expected[0]+"."+s.couple,s.couple,decoded));CHECK_FALSE(A::decodeCursor("am1.1."+expected[0]+"."+s.couple,s.couple,decoded));
  }catch(const std::exception &){c::expectThat(false,"admin-anniversary-paging","fixture/query failure");}
 }
}
}
