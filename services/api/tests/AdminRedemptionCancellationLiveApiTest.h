#pragma once
#include "AdminWalletAdjustmentLiveApiTest.h"
#include "AdminRedemptionCancellationDbTests.h"
namespace usapptest::admincancellive {
namespace a=usapptest::admincancel;namespace r=usapptest::redemption;namespace live=usapptest::reminderliveapi;
inline void run(const r::c::ClientPtr &pool,const std::string &dsn,const std::string &base){
 TEST_CASE("Admin cancellation live: both-listener authentication/CSRF/Origin boundaries, replay, original refund and separate admin detail"){
  try{(void)dsn;const char *ab=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(ab!=nullptr);if(!ab)return;r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,12);const auto id=a::paid(s),admin=usapptest::adminadjustment::admin(pool),token=usapp::crypto::randomHex(32),csrf=usapp::crypto::randomHex(32),key=r::c::newUuid();
   pool->execSqlSync("INSERT INTO admin_sessions(token_hash,csrf_token_hash,admin_user_id,expires_at) VALUES($1,$2,$3::uuid,now()+interval '1 hour')",usapp::AdminAuthService::tokenHash(token),usapp::crypto::sha256Hex(csrf),admin);
   const auto user=live::commitToken(pool,s.userB);const std::string root="/api/admin/v1/couples/"+s.couple+"/redemptions/",path=root+id+"/cancellations",body=R"({"expected_status":"pending_fulfillment","reason":"Verified live exceptional cancellation"})";
   const auto headers=[&](const std::string &k){return std::vector<std::string>{"X-Usapp-Admin-Surface: usapp-admin","Origin: https://localhost:8444","X-Requested-By: usapp-admin","X-CSRF-Token: "+csrf,"Content-Type: application/json","Idempotency-Key: "+k};};
   const auto call=[&](const std::string &listener,const std::string &url,std::vector<std::string> h,const std::string &b,bool cookie=true){if(cookie)h.push_back("Cookie: __Host-usapp-admin="+token);return live::call(listener+url,"",h,&b);};
   std::string receipt;
   for(const std::string &listener:{base,std::string(ab)}){
    CHECK_EQ(call(listener,path,headers(key),body,false).status,401L);CHECK_EQ(live::call(listener+path,user,headers(key),&body).status,401L);
    for(int i:{0,1,2,3}){auto h=headers(key);h.erase(h.begin()+i);CHECK_EQ(call(listener,path,h,body).status,403L);}
    auto h=headers(key);h[1]="Origin: https://evil.example";CHECK_EQ(call(listener,path,h,body).status,403L);
    const auto ok=call(listener,path,headers(key),body);CHECK_EQ(ok.status,200L);const auto v=live::parsed(ok);CHECK_EQ(v["cancellation"]["refunded_flowers"].asString(),std::string("4"));CHECK_EQ(v["cancellation"]["admin_id"].asString(),admin);CHECK_EQ(v["cancellation"]["balance_after"].asString(),std::string("12"));if(receipt.empty())receipt=v["cancellation"]["id"].asString();else CHECK_EQ(v["cancellation"]["id"].asString(),receipt);
    CHECK_EQ(call(listener,path,headers(r::c::newUuid()),body).status,409L);
    for(const auto &bad:{std::string("{}"),std::string(R"({"expected_status":"fulfilled","reason":"Invalid terminal status"})"),std::string(R"({"expected_status":"pending_fulfillment","reason":"Invalid amount override","amount":4})")})CHECK_EQ(call(listener,path,headers(r::c::newUuid()),bad).status,400L);
    CHECK_EQ(call(listener,root+r::c::newUuid()+"/cancellations",headers(r::c::newUuid()),body).status,404L);
    auto getHeaders=std::vector<std::string>{"X-Usapp-Admin-Surface: usapp-admin","Cookie: __Host-usapp-admin="+token};const auto detail=live::parsed(live::call(listener+root+id,"",getHeaders,nullptr));CHECK_EQ(detail["redemption"]["cancel_reason"].asString(),std::string("admin"));CHECK_EQ(detail["admin_cancellation"]["admin_id"].asString(),admin);CHECK(detail["admin_cancellation"]["reason"].isString());CHECK_FALSE(detail["admin_cancellation"].isMember("idempotency_key"));
   }
   const auto publicDetail=live::parsed(live::call(base+"/api/v1/rewards/redemptions/"+id,user,{},nullptr));CHECK_EQ(publicDetail["redemption"]["cancel_reason"].asString(),std::string("admin"));CHECK_FALSE(publicDetail.isMember("admin_cancellation"));CHECK_EQ(r::flowers(s.userB),12);
  }catch(const std::exception &){r::c::expectThat(false,"admin-cancel-live","case failed (details withheld)");}
 }
}
}
