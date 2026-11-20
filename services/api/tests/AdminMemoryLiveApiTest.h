#pragma once
#include "AdminWalletLiveApiTest.h"
#include "AdminMemoryDbTests.h"
namespace usapptest::adminmemoryliveapi {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace live=usapptest::reminderliveapi;
inline void run(const c::ClientPtr &pool,const std::string &dsn,const std::string &base){
 usapptest::media::prepareEnv(pool,dsn);
 TEST_CASE("Admin memories live: all four reads on both listeners require independent admin session and surface; scoped cursors and validation"){
  try{const char *ab=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(ab!=nullptr);if(!ab)return;usapptest::media::useStore();const auto s=usapptest::media::space(),other=usapptest::media::space();const auto id=usapptest::adminmemory::create(s.userB),second=usapptest::adminmemory::create(s.userB),foreign=usapptest::adminmemory::create(other.userA);
   const auto photo=usapptest::adminmemory::detail(s.couple,id)->body["memory"]["photos"][0]["media_id"].asString();auto proposal=usapptest::memory::propose(s.userA,id,1,usapptest::memory::terms("PROPOSED",{photo}));CHECK(usapptest::memory::is(proposal,usapptest::memory::SO::Created));const auto pid=usapptest::memory::proposalOf(proposal);
   const auto adminId=pool->execSqlSync("INSERT INTO admin_users(username,password_hash,status) VALUES($1,'pbkdf2-sha256$1000$AAAA$AAAA','active') RETURNING id::text AS id","t_am_"+usapp::crypto::randomHex(5))[0]["id"].as<std::string>();const auto adminToken=usapp::crypto::randomHex(32);pool->execSqlSync("INSERT INTO admin_sessions(token_hash,csrf_token_hash,admin_user_id,expires_at) VALUES($1,$2,$3::uuid,now()+interval '1 hour')",usapp::AdminAuthService::tokenHash(adminToken),usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)),adminId);
   const auto user=live::commitToken(pool,s.userB),outsider=live::commitToken(pool,other.userA);const std::vector<std::string> surface={"X-Usapp-Admin-Surface: usapp-admin"};
   const auto admin=[&](const std::string &listener,const std::string &path,const std::vector<std::string> &headers){auto h=headers;h.push_back("Cookie: __Host-usapp-admin="+adminToken);return live::call(listener+path,"",h,nullptr);};
   const std::string root="/api/admin/v1/couples/"+s.couple+"/memories",d=root+"/"+id,h=d+"/proposals",cmp=h+"/"+pid;const auto before=usapptest::memoryproposalhistory::footprint(id,s.couple);
   for(const std::string listener:{base,std::string(ab)}){
    for(const auto &path:{root,d,h,cmp}){CHECK_EQ(live::call(listener+path,"",surface,nullptr).status,401L);CHECK_EQ(live::call(listener+path,user,surface,nullptr).status,401L);CHECK_EQ(admin(listener,path,{}).status,403L);CHECK_EQ(admin(listener,path,{"X-Usapp-Admin-Surface: usapp-web"}).status,403L);auto r=admin(listener,path,surface);CHECK_EQ(r.status,200L);CHECK(usapptest::exportliveapi::headerValue(r.headers,"Cache-Control").find("no-store")!=std::string::npos);CHECK(r.body.find("fingerprint")==std::string::npos);CHECK(r.body.find("idempotency")==std::string::npos);CHECK(r.body.find("object_key")==std::string::npos);}
    CHECK_EQ(live::parsed(admin(listener,d,surface))["memory"]["id"].asString(),id);CHECK_EQ(live::parsed(admin(listener,cmp,surface))["comparison"]["proposal"]["id"].asString(),pid);
    CHECK_EQ(admin(listener,root+"/"+foreign,surface).status,404L);CHECK_EQ(admin(listener,root+"/"+second+"/proposals/"+pid,surface).status,404L);CHECK_EQ(admin(listener,h+"/"+c::newUuid(),surface).status,404L);
    for(auto path:{root+"?limit=0",root+"?limit=101",root+"?cursor=invalid",h+"?limit=abc",h+"?cursor=invalid",root+"/bad",h+"/bad"})CHECK_EQ(admin(listener,path,surface).status,400L);
    auto page=live::parsed(admin(listener,root+"?limit=1",surface));CHECK(!page["next_cursor"].isNull());const auto cursor=page["next_cursor"].asString();CHECK_EQ(admin(listener,root+"?cursor="+cursor,surface).status,200L);CHECK_EQ(admin(listener,"/api/admin/v1/couples/"+other.couple+"/memories?cursor="+cursor,surface).status,400L);
    const auto pc=usapp::AdminMemoryService::encodeCursor(s.couple,{1,pid},id);CHECK_EQ(admin(listener,h+"?cursor="+pc,surface).status,200L);CHECK_EQ(admin(listener,root+"/"+second+"/proposals?cursor="+pc,surface).status,400L);
   }
   CHECK_EQ(live::call(base+"/api/v1/memories/"+id,outsider,{},nullptr).status,404L);CHECK_EQ(live::call(base+"/api/v1/memories/"+id,user,{},nullptr).status,200L);CHECK_EQ(usapptest::memoryproposalhistory::footprint(id,s.couple),before);
  }catch(const std::exception &e){c::expectThat(false,"admin-memory-live",usapptest::media::exceptionCategory(e));}
 }
}
}
