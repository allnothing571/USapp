#pragma once
#include "TaskCompletionDbTests.h"
#include "services/AdminWalletAdjustmentService.h"
namespace usapptest::adminadjustment {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;
using A=usapp::AdminWalletAdjustmentService;
inline std::string admin(const c::ClientPtr &pool){return pool->execSqlSync("INSERT INTO admin_users(username,password_hash) VALUES($1,'fixture-only-hash') RETURNING id::text AS id","t_adjust_"+usapp::crypto::randomHex(6))[0]["id"].as<std::string>();}
inline A::Request command(const std::string &asset="flower",const std::string &delta="1000",const std::string &seq="0"){return {asset,delta,seq,"Verified administrator correction"};}
inline auto start(const std::string &adm,const std::string &user,const std::string &key,const A::Request &request,A::Fault fault=A::Fault::None){auto p=std::make_shared<ts::Pending<A::Result>>();A::adjust(adm,user,key,request,[p](auto r){p->deliver(std::move(r));},fault);return p;}
inline auto adjust(const std::string &adm,const std::string &user,const std::string &key,const A::Request &request,A::Fault fault=A::Fault::None){return start(adm,user,key,request,fault)->wait();}
inline bool is(const std::optional<A::Result>&r,A::Outcome o){return r&&r->outcome==o;}
inline long long count(const c::Observer &o,const std::string &adm){return o.client->execSqlSync("SELECT count(*)::bigint AS n FROM admin_wallet_adjustments WHERE admin_id=$1::uuid",adm)[0]["n"].as<long long>();}
inline std::string balance(const c::Observer &o,const std::string &user,const std::string &asset){return o.client->execSqlSync("SELECT COALESCE((SELECT balance_after FROM wallet_ledger_entries WHERE user_id=$1::uuid AND asset_type=$2 ORDER BY seq DESC LIMIT 1),0)::text AS n",user,asset)[0]["n"].as<std::string>();}
inline void run(const c::ClientPtr &pool,const std::string &dsn){const auto observer=c::Observer::connect(dsn);
 TEST_CASE("Admin adjustment: exact input, atomic bounded children, original receipt replay, true admin identity and definite refusals"){
  try{const auto adm=admin(pool);ts::CommittedSpace s;ts::commitSpace(pool,s,true);const auto key=c::newUuid();A::Request parsed;Json::Value body;body["asset"]="flower";body["delta"]="9999";body["expected_seq"]="0";body["reason"]="Verified administrator correction";CHECK(A::parse(body,parsed));for(const auto &v:{"0","-0","01","10000","1.5"}){body["delta"]=v;CHECK_FALSE(A::parse(body,parsed));}body["delta"]=1;CHECK_FALSE(A::parse(body,parsed));body["delta"]="1";body["expected_seq"]="9223372036854775808";CHECK_FALSE(A::parse(body,parsed));
   auto r=adjust(adm,s.userB,key,command());CHECK(is(r,A::Outcome::Ok));if(!is(r,A::Outcome::Ok))return;const auto receipt=r->body;CHECK_EQ(receipt["adjustment"]["operation_count"].asInt(),2);CHECK_EQ(receipt["adjustment"]["balance_after"].asString(),std::string("1000"));CHECK_EQ(count(observer,adm),1LL);
   auto replay=adjust(adm,s.userB,key,command());CHECK(is(replay,A::Outcome::Ok));if(replay)CHECK(replay->body==receipt);CHECK_EQ(count(observer,adm),1LL);CHECK(is(adjust(adm,s.userB,key,command("flower","999")),A::Outcome::KeyReused));
   CHECK(is(adjust(adm,s.userB,c::newUuid(),command("flower","1","0")),A::Outcome::Conflict));CHECK(is(adjust(adm,s.userB,c::newUuid(),command("flower","-1001","2")),A::Outcome::BalanceBound));CHECK_EQ(balance(observer,s.userB,"flower"),std::string("1000"));
   CHECK(is(adjust(adm,s.userB,c::newUuid(),command("flower","-1000","2")),A::Outcome::Ok));CHECK_EQ(balance(observer,s.userB,"flower"),std::string("0"));CHECK(is(adjust(c::newUuid(),s.userB,c::newUuid(),command()),A::Outcome::Forbidden));CHECK(is(adjust(adm,c::newUuid(),c::newUuid(),command()),A::Outcome::NotFound));
   const auto actor=observer.client->execSqlSync("SELECT count(*)::bigint AS n FROM wallet_operations WHERE operation_type='ADMIN_ADJUSTMENT' AND actor_admin_id=$1::uuid AND actor_user_id IS NOT NULL",adm);CHECK_EQ(actor[0]["n"].as<long long>(),0LL);
   CHECK_EQ(count(observer,adm),2LL);const auto audit=observer.client->execSqlSync("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_type='admin' AND actor_id=$1::uuid AND action='admin.wallet.adjusted'",adm);CHECK_EQ(audit[0]["n"].as<long long>(),2LL);
  }catch(const std::exception &){c::expectThat(false,"admin-adjustment-protocol","case failed (details withheld)");}
 }
 TEST_CASE("Admin adjustment: a deliberately discarded confirmed commit answer replays without another posting"){
  try{const auto adm=admin(pool);ts::CommittedSpace s;ts::commitSpace(pool,s,true);const auto key=c::newUuid();CHECK(is(adjust(adm,s.userA,key,command("sunshine","9999"),A::Fault::DiscardConfirmedCommitReply),A::Outcome::Unknown));CHECK_EQ(count(observer,adm),1LL);CHECK_EQ(balance(observer,s.userA,"sunshine"),std::string("9999"));auto r=adjust(adm,s.userA,key,command("sunshine","9999"));CHECK(is(r,A::Outcome::Ok));if(r){CHECK_EQ(r->body["adjustment"]["expected_seq"].asString(),std::string("0"));CHECK_EQ(r->body["adjustment"]["balance_after"].asString(),std::string("9999"));}CHECK_EQ(count(observer,adm),1LL);
  }catch(const std::exception &){c::expectThat(false,"admin-adjustment-reply-loss","case failed (details withheld)");}
 }
 TEST_CASE("Admin adjustment: concurrent same key replays, competing reviewed sequences serialize, ordinary task reward remains intact"){
  try{const auto adm=admin(pool);ts::CommittedSpace s;ts::commitSpace(pool,s,true);const auto key=c::newUuid();auto p=start(adm,s.userA,key,command("flower","1")),q=start(adm,s.userA,key,command("flower","1"));auto a=p->wait(),b=q->wait();CHECK(is(a,A::Outcome::Ok));CHECK(is(b,A::Outcome::Ok));if(a&&b)CHECK(a->body==b->body);CHECK_EQ(count(observer,adm),1LL);CHECK_EQ(balance(observer,s.userA,"flower"),std::string("1"));
   p=start(adm,s.userA,c::newUuid(),command("flower","1","1"));q=start(adm,s.userA,c::newUuid(),command("flower","2","1"));a=p->wait();b=q->wait();CHECK((is(a,A::Outcome::Ok)&&is(b,A::Outcome::Conflict))||(is(b,A::Outcome::Ok)&&is(a,A::Outcome::Conflict)));CHECK_EQ(count(observer,adm),2LL);
   const auto task=c::publish(s.userA,"Adjustment versus ordinary reward",3,0);auto reward=c::startComplete(s.userB,task,c::newUuid());p=start(adm,s.userB,c::newUuid(),command("flower","5","0"));auto completed=reward->wait();a=p->wait();CHECK(c::is(completed,c::Outcome::Completed));CHECK(is(a,A::Outcome::Ok)||is(a,A::Outcome::Conflict));CHECK_EQ(balance(observer,s.userB,"flower"),is(a,A::Outcome::Ok)?std::string("8"):std::string("3"));
  }catch(const std::exception &){c::expectThat(false,"admin-adjustment-concurrent","case failed (details withheld)");}
 }
}
}
