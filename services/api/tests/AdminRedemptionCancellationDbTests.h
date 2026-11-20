#pragma once
#include "AdminWalletAdjustmentDbTests.h"
#include "RedemptionWorkflowDbTests.h"
#include "services/AdminRedemptionCancellationService.h"
#include "services/AdminRedemptionService.h"
namespace usapptest::admincancel {
namespace r=usapptest::redemption;namespace w=usapptest::redemptionworkflow;namespace a=usapptest::adminadjustment;
using A=usapp::AdminRedemptionCancellationService;
inline auto start(const std::string &admin,const std::string &space,const std::string &id,const std::string &key,const std::string &status="pending_fulfillment",A::Fault fault=A::Fault::None){auto p=std::make_shared<r::ts::Pending<A::Result>>();A::cancel(admin,space,id,key,{status,"Reviewed exceptional cancellation"},[p](auto v){p->deliver(std::move(v));},fault);return p;}
inline auto cancel(const std::string &admin,const std::string &space,const std::string &id,const std::string &key=r::c::newUuid(),const std::string &status="pending_fulfillment",A::Fault fault=A::Fault::None){return start(admin,space,id,key,status,fault)->wait();}
inline bool is(const std::optional<A::Result>&v,A::Outcome o){return v&&v->outcome==o;}
inline std::string paid(const r::ts::CommittedSpace &s){const auto product=w::create(s,"direct",4);const auto request=r::redeem(s.userB,product,1);if(!request||request->redemption.id.empty())throw std::runtime_error("paid fixture");return request->redemption.id;}
inline void run(const r::c::ClientPtr &pool,const std::string &dsn){const auto observer=r::c::Observer::connect(dsn);
 TEST_CASE("Admin cancellation: original charged snapshot, separate admin fact, replay, uncharged request and terminal guards"){
  try{const auto admin=a::admin(pool);r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,12);const auto id=paid(s),key=r::c::newUuid();const auto proposal=w::propose(s.userA,id);CHECK_EQ(r::flowers(s.userB),8);
   const auto old=r::get(s.userB,id);if(!old)throw std::runtime_error("snapshot fixture");CHECK(r::pr::is(r::pr::edit(s.userA,old->redemption.productId,1,r::pr::terms("Changed catalog price",9,"request")),r::PO::Updated));CHECK(r::pr::is(r::pr::archive(s.userA,old->redemption.productId,2),r::PO::Updated));
   A::Request parsed;Json::Value input;input["expected_status"]="pending_fulfillment";input["reason"]="Reviewed exceptional cancellation";CHECK(A::parse(input,parsed));input["refund_amount"]=4;CHECK_FALSE(A::parse(input,parsed));input.removeMember("refund_amount");input["expected_status"]="fulfilled";CHECK_FALSE(A::parse(input,parsed));
   CHECK(is(cancel(admin,s.couple,id,r::c::newUuid(),"pending_confirmation"),A::Outcome::Conflict));auto v=cancel(admin,s.couple,id,key);CHECK(is(v,A::Outcome::Ok));if(!v||v->outcome!=A::Outcome::Ok)return;CHECK_EQ(v->body["cancellation"]["refunded_flowers"].asString(),std::string("4"));CHECK_EQ(r::flowers(s.userB),12);auto replay=cancel(admin,s.couple,id,key);CHECK(is(replay,A::Outcome::Ok));if(replay)CHECK(replay->body==v->body);
   CHECK(is(cancel(admin,s.couple,id,key,"pending_confirmation"),A::Outcome::KeyReused));CHECK(is(cancel(admin,s.couple,id),A::Outcome::Conflict));CHECK(is(cancel(admin,r::c::newUuid(),id),A::Outcome::NotFound));CHECK(is(cancel(r::c::newUuid(),s.couple,id),A::Outcome::Forbidden));
   auto current=r::get(s.userB,id);CHECK(current.has_value());if(current){CHECK_EQ(current->redemption.status,std::string("cancelled"));CHECK_EQ(current->redemption.cancelReason,std::string("admin"));CHECK(current->redemption.cancellationId.empty());}CHECK(r::is(r::fulfill(s.userA,id),r::RO::InvalidState));CHECK(r::is(w::act(s.userB,id,"cancel_approve",proposal),r::RO::InvalidState));
   const auto p=w::create(s,"request",4);auto req=r::redeem(s.userB,p,1);CHECK(r::is(req,r::RO::Requested));if(!req)return;auto unpaid=cancel(admin,s.couple,req->redemption.id,r::c::newUuid(),"pending_confirmation");CHECK(is(unpaid,A::Outcome::Ok));if(unpaid){CHECK_EQ(unpaid->body["cancellation"]["refunded_flowers"].asString(),std::string("0"));CHECK(unpaid->body["cancellation"]["refund_operation_id"].isNull());}CHECK_EQ(r::flowers(s.userB),12);
   CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM wallet_operations WHERE actor_admin_id=$1::uuid AND operation_type='ADMIN_REDEMPTION_REFUND'",admin),1);CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM reward_redemption_events WHERE id=$1::uuid",proposal),1);
  }catch(const std::exception &){r::c::expectThat(false,"admin-cancel-protocol","case failed (details withheld)");}
 }
 TEST_CASE("Admin cancellation: discarded confirmed commit reply and concurrent same-key commands never duplicate refunds"){
  try{const auto admin=a::admin(pool);r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,20);auto id=paid(s),key=r::c::newUuid();CHECK(is(cancel(admin,s.couple,id,key,"pending_fulfillment",A::Fault::DiscardConfirmedCommitReply),A::Outcome::Unknown));CHECK_EQ(r::flowers(s.userB),20);CHECK(is(cancel(admin,s.couple,id,key),A::Outcome::Ok));CHECK_EQ(r::flowers(s.userB),20);
   id=paid(s);key=r::c::newUuid();auto p=start(admin,s.couple,id,key),q=start(admin,s.couple,id,key);auto x=p->wait(),y=q->wait();CHECK(is(x,A::Outcome::Ok));CHECK(is(y,A::Outcome::Ok));if(x&&y)CHECK(x->body==y->body);CHECK_EQ(r::flowers(s.userB),20);CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM admin_redemption_cancellations WHERE admin_id=$1::uuid",admin),2);
  }catch(const std::exception &){r::c::expectThat(false,"admin-cancel-replay","case failed (details withheld)");}
 }
 TEST_CASE("Admin cancellation: same legacy transition lock fences fulfillment and member refund races"){
  try{const auto admin=a::admin(pool);r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,20);auto id=paid(s);auto p=start(admin,s.couple,id,r::c::newUuid());auto f=std::make_shared<r::PendingRedemption>();r::RS::instance().fulfill(s.userA,id,r::c::newUuid(),[f](auto value){f->deliver(std::move(value));});auto x=p->wait();auto fulfilled=f->wait();CHECK((is(x,A::Outcome::Ok)&&r::is(fulfilled,r::RO::InvalidState))||(is(x,A::Outcome::Conflict)&&r::is(fulfilled,r::RO::Fulfilled)));const int balanceAfterFirst=is(x,A::Outcome::Ok)?20:16;CHECK_EQ(r::flowers(s.userB),balanceAfterFirst);
   id=paid(s);const auto proposal=w::propose(s.userA,id);p=start(admin,s.couple,id,r::c::newUuid());auto q=w::start(s.userB,id,"cancel_approve",proposal);x=p->wait();auto member=q->wait();CHECK((is(x,A::Outcome::Ok)&&r::is(member,r::RO::InvalidState))||(is(x,A::Outcome::Conflict)&&r::is(member,r::RO::Changed)));CHECK_EQ(r::flowers(s.userB),balanceAfterFirst);
  }catch(const std::exception &){r::c::expectThat(false,"admin-cancel-races","case failed (details withheld)");}
 }
}
}
