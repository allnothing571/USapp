#pragma once
#include "config/AppConfig.h"
#include "AdminWalletLiveApiTest.h"
#include "AdminWalletAdjustmentDbTests.h"
namespace usapptest::adminadjustmentliveapi {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace live=usapptest::reminderliveapi;namespace a=usapptest::adminadjustment;
inline void run(const c::ClientPtr &pool,const std::string &dsn,const std::string &base){
 TEST_CASE("Admin adjustment live: independent admin session, Origin/CSRF/surface on both listeners; replay, bounds and current-sequence conflict"){
  try{const char *ab=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(ab!=nullptr);if(!ab)return;ts::CommittedSpace s;ts::commitSpace(pool,s,true);const auto adm=a::admin(pool),token=usapp::crypto::randomHex(32),csrf=usapp::crypto::randomHex(32),key=c::newUuid();
   pool->execSqlSync("INSERT INTO admin_sessions(token_hash,csrf_token_hash,admin_user_id,expires_at) VALUES($1,$2,$3::uuid,now()+interval '1 hour')",usapp::AdminAuthService::tokenHash(token),usapp::crypto::sha256Hex(csrf),adm);
   const auto userToken=live::commitToken(pool,s.userB);const auto observer=c::Observer::connect(dsn);
   const std::string path="/api/admin/v1/users/"+s.userB+"/wallet/adjustments",body=R"({"asset":"flower","delta":"1000","expected_seq":"0","reason":"Verified live administrator correction"})";
   const auto headers=[&](const std::string &k){return std::vector<std::string>{"X-Usapp-Admin-Surface: usapp-admin","Origin: https://localhost:8444","X-Requested-By: usapp-admin","X-CSRF-Token: "+csrf,"Content-Type: application/json","Idempotency-Key: "+k};};
   const auto call=[&](const std::string &listener,const std::string &url,const std::vector<std::string> &h,const std::string &b,bool cookie=true){auto copy=h;if(cookie)copy.push_back("Cookie: __Host-usapp-admin="+token);return live::call(listener+url,"",copy,&b);};
   std::string receipt;
   for(const std::string &listener:{base,std::string(ab)}){
    CHECK_EQ(call(listener,path,headers(key),body,false).status,401L);CHECK_EQ(live::call(listener+path,userToken,headers(key),&body).status,401L);
    for(int index:{0,1,2,3}){auto h=headers(key);h.erase(h.begin()+index);CHECK_EQ(call(listener,path,h,body).status,403L);}
    auto h=headers(key);h[1]="Origin: https://evil.example";CHECK_EQ(call(listener,path,h,body).status,403L);h=headers(key);h[3]="X-CSRF-Token: "+usapp::crypto::randomHex(32);CHECK_EQ(call(listener,path,h,body).status,403L);
    const auto ok=call(listener,path,headers(key),body);CHECK_EQ(ok.status,200L);CHECK(usapptest::exportliveapi::headerValue(ok.headers,"Cache-Control").find("no-store")!=std::string::npos);const auto p=live::parsed(ok);CHECK_EQ(p["adjustment"]["admin_id"].asString(),adm);CHECK_EQ(p["adjustment"]["user_id"].asString(),s.userB);CHECK_EQ(p["adjustment"]["operation_count"].asInt(),2);CHECK_EQ(p["adjustment"]["balance_after"].asString(),std::string("1000"));if(receipt.empty())receipt=p["adjustment"]["id"].asString();else CHECK_EQ(p["adjustment"]["id"].asString(),receipt);
    CHECK_EQ(call(listener,path,headers(c::newUuid()),body).status,409L);std::string changed=body;changed.replace(changed.find("1000"),4,"1001");CHECK_EQ(call(listener,path,headers(key),changed).status,409L);
    for(const auto &invalid:{std::string("{}"),std::string(R"({"asset":"flower","delta":1,"expected_seq":"0","reason":"Wrong integer shape"})"),std::string(R"({"asset":"flower","delta":"0","expected_seq":"0","reason":"Wrong zero value"})")})CHECK_EQ(call(listener,path,headers(c::newUuid()),invalid).status,400L);
    h=headers(key);h.pop_back();CHECK_EQ(call(listener,path,h,body).status,400L);
    CHECK_EQ(call(listener,"/api/admin/v1/users/"+c::newUuid()+"/wallet/adjustments",headers(c::newUuid()),body).status,404L);
    const std::string badBalance=R"({"asset":"flower","delta":"-9999","expected_seq":"2","reason":"Refused impossible correction"})";CHECK_EQ(call(listener,path,headers(c::newUuid()),badBalance).status,409L);
   }
   CHECK_EQ(a::count(observer,adm),1LL);CHECK_EQ(a::balance(observer,s.userB,"flower"),std::string("1000"));
  }catch(const std::exception &){c::expectThat(false,"admin-adjustment-live","case failed (details withheld)");}
 }
}
}
