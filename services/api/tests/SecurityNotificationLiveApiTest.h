#pragma once
#include "ReminderLiveApiTest.h"
namespace usapptest::securitynoticelive {
namespace live=usapptest::reminderliveapi;namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;
inline void run(const c::ClientPtr &pool,const std::string &,const std::string &base){
 TEST_CASE("Security notice live: no-space login, only recipient reads, CSRF acknowledgement and private fixed projection"){
  try{const auto user=c::newUuid(),other=c::newUuid();ts::commitUser(pool,user,"security-live");ts::commitUser(pool,other,"security-other");const auto csrf=usapp::crypto::randomHex(32),token=live::commitToken(pool,user,csrf),foreign=live::commitToken(pool,other);
   CHECK_EQ(live::get(base+"/api/v1/notifications").status,401L);const auto page=live::get(base+"/api/v1/notifications",token);CHECK_EQ(page.status,200L);const auto items=live::parsed(page)["notifications"];CHECK_EQ(items.size(),1u);if(items.empty())return;const auto notice=items[0];const auto id=notice["id"].asString();CHECK_EQ(notice["event_type"].asString(),std::string("security.session_created"));CHECK(notice["actor_id"].isNull());CHECK_EQ(notice["target_id"].asString(),user);CHECK_EQ(notice.size(),7u);CHECK(page.body.find("session_id")==std::string::npos);CHECK(page.body.find("token_hash")==std::string::npos);CHECK_EQ(live::parsed(live::get(base+"/api/v1/notifications/unread-count",token))["unread_count"].asInt(),1);
   const std::string body="{}",path=base+"/api/v1/notifications/"+id+"/read";const std::vector<std::string> headers{"Origin: https://localhost:8443","X-Requested-By: usapp-web","X-CSRF-Token: "+csrf,"Content-Type: application/json"};
   CHECK_EQ(live::call(path,token,{},&body).status,403L);CHECK_EQ(live::call(path,token,headers,&body).status,200L);CHECK_EQ(live::call(path,token,headers,&body).status,200L);CHECK_EQ(live::parsed(live::get(base+"/api/v1/notifications/unread-count",token))["unread_count"].asInt(),0);
   const auto foreignPage=live::get(base+"/api/v1/notifications",foreign);CHECK(foreignPage.body.find(id)==std::string::npos);const auto otherCsrf=usapp::crypto::randomHex(32),otherToken=live::commitToken(pool,other,otherCsrf);auto wrong=headers;wrong[2]="X-CSRF-Token: "+otherCsrf;CHECK_EQ(live::call(path,otherToken,wrong,&body).status,404L);
  }catch(const std::exception &){c::expectThat(false,"security-notice-live","case failed (details withheld)");}
 }
}
}
