#pragma once
#include "config/AppConfig.h"
#include "NotificationDbTests.h"
#include "AdminRedemptionCancellationDbTests.h"
#include "services/SessionService.h"
namespace usapptest::securitynotice {
namespace n=usapptest::notification;namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;
namespace a=usapptest::adminadjustment;namespace ac=usapptest::admincancel;namespace r=usapptest::redemption;
inline std::optional<std::string> login(const std::string &user,const std::string &previous=""){
 auto p=std::make_shared<ts::Pending<std::string>>();usapp::SessionService::instance().createAuthenticatedSession(user,previous,"test",[p](const std::string &id,const std::string &,const std::string &){p->deliver(id);},[p](const std::string &){p->deliver("");});return p->wait();
}
struct LoginConfig {
 int previous=usapp::AppConfig::instance().authSessionTtlSeconds;
 LoginConfig(){usapp::AppConfig::instance().authSessionTtlSeconds=3600;}
 ~LoginConfig(){usapp::AppConfig::instance().authSessionTtlSeconds=previous;}
};
inline void run(const c::ClientPtr &pool,const std::string &dsn){LoginConfig config;const auto observer=c::Observer::connect(dsn);
 TEST_CASE("Security notices: no-space login is discoverable to its owner, recipient-scoped reads, rotation once and no touch/revoke notice"){
  try{const auto user=c::newUuid(),other=c::newUuid();ts::commitUser(pool,user,"security-a");ts::commitUser(pool,other,"security-b");const auto s=login(user);CHECK(s&& !s->empty());if(!s||s->empty())return;
   auto page=n::list(user);CHECK(page&&page->status==200);if(!page)return;CHECK_EQ(page->body["notifications"].size(),1u);const auto notice=page->body["notifications"][0];CHECK_EQ(notice["event_type"].asString(),std::string("security.session_created"));CHECK(notice["actor_id"].isNull());CHECK_EQ(notice["target_id"].asString(),user);CHECK_EQ(notice.size(),7u);const auto id=notice["id"].asString();CHECK(n::read(other,id)->status==404);CHECK(n::read(user,id)->status==200);CHECK(n::read(user,id)->status==200);CHECK_EQ(n::unreadCount(user)->body["unread_count"].asInt(),0);CHECK_EQ(n::list(other)->body["notifications"].size(),0u);
   pool->execSqlSync("UPDATE sessions SET last_seen_at=clock_timestamp(),csrf_token_hash=$2 WHERE id=$1::uuid",*s,usapp::crypto::sha256Hex("rotated-test-only"));CHECK_EQ(n::list(user)->body["notifications"].size(),1u);const auto rotated=login(user,*s);CHECK(rotated&& !rotated->empty());CHECK_EQ(n::list(user)->body["notifications"].size(),2u);
   pool->execSqlSync("UPDATE sessions SET revoked_at=clock_timestamp(),revoked_reason='logout' WHERE user_id=$1::uuid",user);CHECK_EQ(n::list(user)->body["notifications"].size(),2u);
   CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM notifications WHERE recipient_id=$1::uuid AND couple_id IS NULL",user),2);
  }catch(const std::exception &){c::expectThat(false,"security-login","case failed (details withheld)");}
 }
 TEST_CASE("Security notices: financial original receipt replay has one personal notice; operator reasons and identities stay private"){
  try{ts::CommittedSpace s;ts::commitSpace(pool,s,true);const auto admin=a::admin(pool),key=c::newUuid();auto v=a::adjust(admin,s.userB,key,a::command("flower","12"),a::A::Fault::DiscardConfirmedCommitReply);CHECK(a::is(v,a::A::Outcome::Unknown));CHECK(a::is(a::adjust(admin,s.userB,key,a::command("flower","12")),a::A::Outcome::Ok));
   auto page=n::list(s.userB);CHECK(page&&page->status==200);if(!page)return;CHECK_EQ(page->body["notifications"].size(),1u);CHECK_EQ(page->body["notifications"][0]["event_type"].asString(),std::string("security.wallet_adjusted"));CHECK_EQ(n::list(s.userA)->body["notifications"].size(),0u);
   const auto redemption=ac::paid(s),cancelKey=c::newUuid();CHECK(ac::is(ac::cancel(admin,s.couple,redemption,cancelKey,"pending_fulfillment",ac::A::Fault::DiscardConfirmedCommitReply),ac::A::Outcome::Unknown));CHECK(ac::is(ac::cancel(admin,s.couple,redemption,cancelKey),ac::A::Outcome::Ok));
   page=n::list(s.userB);CHECK(page&&page->status==200);if(!page)return;CHECK_EQ(page->body["notifications"].size(),2u);Json::StreamWriterBuilder w;const auto json=Json::writeString(w,page->body);CHECK(json.find("Reviewed exceptional cancellation")==std::string::npos);CHECK(json.find("Verified administrator correction")==std::string::npos);CHECK(json.find(admin)==std::string::npos);CHECK(json.find("session_id")==std::string::npos);
   CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM notifications WHERE recipient_id=$1::uuid AND event_type LIKE 'security.%'",s.userA),0);
  }catch(const std::exception &){c::expectThat(false,"security-financial","case failed (details withheld)");}
 }
}
}
