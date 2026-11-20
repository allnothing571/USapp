#pragma once
#include "AdminWalletLiveApiTest.h"
#include "AdminAnniversaryDbTests.h"
namespace usapptest::adminanniversaryliveapi {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace live=usapptest::reminderliveapi;
inline void run(const c::ClientPtr &pool,const std::string &dsn,const std::string &base){
 TEST_CASE("Admin anniversaries live: both listeners, independent admin auth, scoped paging and validation, public read unchanged"){
  try{const char *ab=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(ab!=nullptr);if(!ab)return;ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);const auto id=usapptest::adminanniversary::create(s.userA),second=usapptest::adminanniversary::create(s.userB),foreign=usapptest::adminanniversary::create(other.userA);
   const auto adminId=pool->execSqlSync("INSERT INTO admin_users(username,password_hash,status) VALUES($1,'pbkdf2-sha256$1000$AAAA$AAAA','active') RETURNING id::text AS id","t_an_"+usapp::crypto::randomHex(5))[0]["id"].as<std::string>();const auto adminToken=usapp::crypto::randomHex(32);pool->execSqlSync("INSERT INTO admin_sessions(token_hash,csrf_token_hash,admin_user_id,expires_at) VALUES($1,$2,$3::uuid,now()+interval '1 hour')",usapp::AdminAuthService::tokenHash(adminToken),usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)),adminId);
   const auto user=live::commitToken(pool,s.userB),outsider=live::commitToken(pool,other.userA);const std::vector<std::string> surface={"X-Usapp-Admin-Surface: usapp-admin"};
   const auto admin=[&](const std::string &listener,const std::string &path,const std::vector<std::string> &headers){auto h=headers;h.push_back("Cookie: __Host-usapp-admin="+adminToken);return live::call(listener+path,"",h,nullptr);};
   const std::string root="/api/admin/v1/couples/"+s.couple+"/anniversaries",d=root+"/"+id;const auto observer=c::Observer::connect(dsn);const auto before=usapptest::adminanniversary::footprint(observer);
   for(const std::string &listener:{base,std::string(ab)}){
    for(const auto &path:{root,d}){CHECK_EQ(live::call(listener+path,"",surface,nullptr).status,401L);CHECK_EQ(live::call(listener+path,user,surface,nullptr).status,401L);CHECK_EQ(admin(listener,path,{}).status,403L);CHECK_EQ(admin(listener,path,{"X-Usapp-Admin-Surface: usapp-web"}).status,403L);auto r=admin(listener,path,surface);CHECK_EQ(r.status,200L);CHECK(usapptest::exportliveapi::headerValue(r.headers,"Cache-Control").find("no-store")!=std::string::npos);for(auto word:{"fingerprint","idempotency","notification","pinned_by_me"})CHECK(r.body.find(word)==std::string::npos);}
    CHECK_EQ(live::parsed(admin(listener,d,surface))["anniversary"]["id"].asString(),id);CHECK_EQ(live::parsed(admin(listener,root,surface))["items"].size(),2u);CHECK_EQ(admin(listener,root+"/"+foreign,surface).status,404L);CHECK_EQ(admin(listener,root+"/"+c::newUuid(),surface).status,404L);CHECK_EQ(admin(listener,"/api/admin/v1/couples/"+c::newUuid()+"/anniversaries/"+id,surface).status,404L);
    for(auto path:{root+"?limit=0",root+"?limit=101",root+"?limit=abc",root+"?cursor=invalid",root+"/bad",std::string("/api/admin/v1/couples/bad/anniversaries")})CHECK_EQ(admin(listener,path,surface).status,400L);
    auto page=live::parsed(admin(listener,root+"?limit=1",surface));CHECK(!page["next_cursor"].isNull());const auto cursor=page["next_cursor"].asString();CHECK_EQ(admin(listener,root+"?cursor="+cursor,surface).status,200L);CHECK_EQ(admin(listener,"/api/admin/v1/couples/"+other.couple+"/anniversaries?cursor="+cursor,surface).status,400L);
   }
   CHECK_EQ(live::call(base+"/api/v1/anniversaries/"+id,outsider,{},nullptr).status,404L);CHECK_EQ(live::call(base+"/api/v1/anniversaries/"+id,user,{},nullptr).status,200L);CHECK_EQ(usapptest::adminanniversary::footprint(observer),before);
  }catch(const std::exception &){c::expectThat(false,"admin-anniversary-live","case failed (details withheld)");}
 }
}
}
