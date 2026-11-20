#pragma once
#include <chrono>
#include <future>
#include <thread>

#include "RedemptionDbTests.h"

namespace usapptest::redemptionworkflow {
namespace r=usapptest::redemption;
using r::RS;
using r::RO;
inline auto start(const std::string &user,const std::string &id,const std::string &action,
                  const std::string &proposal="",const std::string &key=r::c::newUuid()) {
    auto pending=std::make_shared<r::PendingRedemption>();
    RS::instance().transition(user,id,key,action,proposal,[pending](RS::Status s){ pending->deliver(std::move(s)); });
    return pending;
}
inline auto act(const std::string &user,const std::string &id,const std::string &action,
                const std::string &proposal="",const std::string &key=r::c::newUuid()) {
    return start(user,id,action,proposal,key)->wait();
}
inline std::string create(const r::ts::CommittedSpace &s,const std::string &mode="request",int price=4) {
    const auto p=r::pr::create(s.userA,r::pr::terms("M9 snapshot",price,mode));
    if(!p) throw std::runtime_error("M9 product fixture failed");
    return p->product.id;
}
inline std::string propose(const std::string &user,const std::string &id) {
    const auto p=act(user,id,"cancel_propose");
    if(!r::is(p,RO::Changed)) throw std::runtime_error("M9 proposal failed");
    return p->mutation["event_id"].asString();
}
inline void runRedemptionWorkflowDbTests(const drogon::orm::DbClientPtr &pool,const std::string &dsn) {
    const auto observer=r::c::Observer::connect(dsn);
    TEST_CASE("M9: request is unfunded, snapshot confirmation, actor rules and durable replay") {
        try {
            r::ts::CommittedSpace s,other;
            r::ts::commitSpace(pool,s,true);r::ts::commitSpace(pool,other,true);
            const auto product=create(s);
            const auto key=r::c::newUuid();
            const auto request=r::redeem(s.userB,product,1,key);
            CHECK(r::is(request,RO::Requested));
            if(!request) throw std::runtime_error("M9 request missing");
            const auto id=request->redemption.id;
            CHECK_EQ(request->redemption.status,std::string("pending_confirmation"));
            CHECK(request->redemption.operationId.empty());
            CHECK_EQ(request->mutation["debited_flowers"].asInt(),0);
            CHECK_EQ(r::flowers(s.userB),0);
            CHECK_EQ(r::effects(observer,s.userB).ops,0);
            CHECK(r::is(r::fulfill(s.userA,id),RO::InvalidState));
            CHECK(r::is(act(s.userB,id,"confirm"),RO::Forbidden));
            CHECK(r::is(act(s.userB,id,"reject"),RO::Forbidden));
            CHECK(r::is(act(s.userA,id,"withdraw"),RO::Forbidden));
            CHECK(r::is(act(other.userA,id,"confirm"),RO::NotFound));
            CHECK(r::is(act(s.userB,id,"cancel_propose"),RO::InvalidState));
            const auto confirmKey=r::c::newUuid();
            CHECK(r::is(act(s.userA,id,"confirm","",confirmKey),RO::InsufficientBalance));
            CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM reward_redemption_events WHERE redemption_id=$1::uuid",id),0);
            const auto stillPending=r::get(s.userB,id);
            CHECK(stillPending.has_value());
            if(stillPending) CHECK_EQ(stillPending->redemption.status,std::string("pending_confirmation"));
            r::fund(s,10);
            CHECK(r::pr::is(r::pr::edit(s.userA,product,1,r::pr::terms("M9 edited",9,"direct")),r::PO::Updated));
            CHECK(r::pr::is(r::pr::archive(s.userA,product,2),r::PO::Updated));
            const auto confirmed=act(s.userA,id,"confirm","",confirmKey);
            CHECK(r::is(confirmed,RO::Changed));
            if(confirmed) {
                CHECK_EQ(confirmed->redemption.priceFlowers,4);
                CHECK_EQ(confirmed->redemption.status,std::string("pending_fulfillment"));
                CHECK_EQ(confirmed->mutation["debited_flowers"].asInt(),4);
            }
            CHECK_EQ(r::flowers(s.userB),6);
            CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE operation_id=(SELECT operation_id FROM reward_redemption_events WHERE redemption_id=$1::uuid AND action='confirm')",id),1);
            CHECK(r::is(r::fulfill(s.userA,id),RO::Fulfilled));
            const auto replay=act(s.userA,id,"confirm","",confirmKey);
            CHECK(r::is(replay,RO::Replayed));
            if(replay && confirmed) CHECK_EQ(r::facts(*replay),r::facts(*confirmed));
            const auto replayRequest=r::redeem(s.userB,product,1,key);
            CHECK(r::is(replayRequest,RO::Replayed));
            if(replayRequest) CHECK_EQ(r::facts(*replayRequest),r::facts(*request));
            CHECK(r::is(act(s.userA,id,"reject","",confirmKey),RO::KeyReused));
            CHECK(r::is(act(s.userA,id,"cancel_propose"),RO::InvalidState));
            CHECK_EQ(r::flowers(s.userB),6);
        } catch(const std::exception &e) { std::cerr << "M9: " << e.what() << std::endl; CHECK(false); }
    }
    TEST_CASE("M9: rejected and withdrawn requests never debit; cancellation decisions bind exact proposal") {
        try {
            r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);
            r::fund(s,12);const auto product=create(s);
            for(const auto &action: {std::string("reject"),std::string("withdraw")}) {
                const auto req=r::redeem(s.userB,product,1);
                if(!req) throw std::runtime_error("M9 request missing");
                const auto done=act(action=="reject"?s.userA:s.userB,req->redemption.id,action);
                CHECK(r::is(done,RO::Changed));
                if(done) {
                    CHECK_EQ(done->redemption.status,std::string("cancelled"));
                    CHECK_EQ(done->redemption.cancelReason,action=="reject"?std::string("rejected"):std::string("withdrawn"));
                }
                CHECK(r::is(act(s.userA,req->redemption.id,"confirm"),RO::InvalidState));
            }
            CHECK_EQ(r::flowers(s.userB),12);
            const auto direct=create(s,"direct");
            const auto paid=r::redeem(s.userB,direct,1);
            if(!paid) throw std::runtime_error("M9 paid fixture missing");
            const auto id=paid->redemption.id;
            CHECK_EQ(r::flowers(s.userB),8);
            const auto p1=propose(s.userB,id);
            CHECK(r::is(act(s.userB,id,"cancel_approve",p1),RO::Forbidden));
            CHECK(r::is(act(s.userA,id,"cancel_withdraw",p1),RO::Forbidden));
            CHECK(r::is(act(s.userA,id,"cancel_reject",p1),RO::Changed));
            const auto p2=propose(s.userA,id);
            CHECK(r::is(act(s.userB,id,"cancel_approve",p1),RO::InvalidState));
            CHECK(r::is(act(s.userA,id,"cancel_withdraw",p2),RO::Changed));
            const auto p3=propose(s.userB,id);
            const auto key=r::c::newUuid();
            const auto refunded=act(s.userA,id,"cancel_approve",p3,key);
            CHECK(r::is(refunded,RO::Changed));
            if(refunded) {
                CHECK_EQ(refunded->redemption.cancelReason,std::string("mutual"));
                CHECK_EQ(refunded->mutation["refunded_flowers"].asInt(),4);
            }
            CHECK_EQ(r::flowers(s.userB),12);
            const auto replay=act(s.userA,id,"cancel_approve",p3,key);
            CHECK(r::is(replay,RO::Replayed));
            if(replay && refunded) CHECK_EQ(r::facts(*replay),r::facts(*refunded));
            CHECK(r::is(r::fulfill(s.userA,id),RO::InvalidState));
            CHECK(r::is(act(s.userA,id,"cancel_approve",p3),RO::InvalidState));
            CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM wallet_operations WHERE source_type='reward_redemption' AND source_id=$1::text AND operation_type='REDEMPTION_REFUND'",id),1);
            CHECK_EQ(r::flowers(s.userB),12);
        } catch(const std::exception &e) { std::cerr << "M9: " << e.what() << std::endl; CHECK(false); }
    }
    TEST_CASE("0041: an event's and a fulfillment's time is assigned after the guard's lock wait, not before it") {
        try {
            r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,10);
            const auto product=create(s);const auto direct=create(s,"direct");
            const auto request=r::redeem(s.userB,product,1);const auto paid=r::redeem(s.userB,direct,1);
            if(!request||!paid) throw std::runtime_error("0041 redemption fixtures missing");
            const auto underLock=[&](const std::string &redemption,const std::string &sql,const std::string &key) {
                // Direct statements, so that nothing before the guard can take this lock.
                auto holder=r::c::acquireTx(pool);
                const auto held=r::c::exec(holder,"SELECT pg_advisory_xact_lock(33008::integer,hashtext($1)) AS l,clock_timestamp()::text AS at",redemption);
                if(!held.ok||!held.result) throw std::runtime_error("0041 lock not held");
                const auto heldAt=(*held.result)[0]["at"].as<std::string>();
                auto done=std::make_shared<std::promise<bool>>();
                pool->execSqlAsync(sql,[done](const drogon::orm::Result &){ done->set_value(true); },
                                   [done](const drogon::orm::DrogonDbException &){ done->set_value(false); },redemption,s.couple,s.userA,key);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                CHECK(r::c::commitTx(holder).value_or(false));
                CHECK(done->get_future().get());
                return heldAt;
            };
            const auto eventHeld=underLock(request->redemption.id,"INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,idempotency_key,request_fingerprint) "
                "VALUES($1::uuid,$2::uuid,$3::uuid,'reject',$4,repeat('a',64))",r::c::newUuid());
            // Well past any clock step seen, and well short of the wait, so a step can't decide this either way.
            CHECK_EQ(r::c::scalar(observer,"SELECT count(*)::bigint AS n FROM reward_redemption_events WHERE redemption_id=$1::uuid AND created_at-$2::timestamptz>=interval '250 milliseconds'",request->redemption.id,eventHeld),1);
            const auto fulfillHeld=underLock(paid->redemption.id,"INSERT INTO reward_redemption_fulfillments(redemption_id,couple_id,fulfilled_by,idempotency_key,request_fingerprint) "
                "VALUES($1::uuid,$2::uuid,$3::uuid,$4,repeat('a',64))",r::c::newUuid());
            CHECK_EQ(r::c::scalar(observer,"SELECT count(*)::bigint AS n FROM reward_redemption_fulfillments WHERE redemption_id=$1::uuid AND fulfilled_at-$2::timestamptz>=interval '250 milliseconds'",paid->redemption.id,fulfillHeld),1);
        } catch(const std::exception &e) { std::cerr << "0041: " << e.what() << std::endl; CHECK(false); }
    }
    TEST_CASE("M9: confirmation versus withdrawal and fulfillment versus refund are mutually exclusive") {
        try {
            r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);r::fund(s,30);
            const auto product=create(s);const auto req=r::redeem(s.userB,product,1);
            if(!req) throw std::runtime_error("M9 request missing");
            const auto id=req->redemption.id;
            auto a=start(s.userA,id,"confirm"), b=start(s.userB,id,"withdraw");
            auto ar=a->wait(),br=b->wait();
            CHECK((r::is(ar,RO::Changed)&&r::is(br,RO::InvalidState)) || (r::is(br,RO::Changed)&&r::is(ar,RO::InvalidState)));
            CHECK_EQ(r::flowers(s.userB),r::is(ar,RO::Changed)?26:30);
            const auto before=r::flowers(s.userB);
            const auto direct=create(s,"direct");
            auto paid=r::redeem(s.userB,direct,1);
            if(!paid) throw std::runtime_error("M9 paid fixture missing");
            const auto rid=paid->redemption.id,p=propose(s.userB,rid);
            auto refund=start(s.userA,rid,"cancel_approve",p);
            auto fulfilled=std::make_shared<r::PendingRedemption>();
            RS::instance().fulfill(s.userA,rid,r::c::newUuid(),[fulfilled](RS::Status v){ fulfilled->deliver(std::move(v)); });
            auto rr=refund->wait(),fr=fulfilled->wait();
            CHECK((r::is(rr,RO::Changed)&&r::is(fr,RO::InvalidState)) || (r::is(fr,RO::Fulfilled)&&r::is(rr,RO::InvalidState)));
            CHECK_EQ(r::flowers(s.userB),r::is(rr,RO::Changed)?before:before-4);
            // Explicit fulfillment-first order also closes any pending cancellation.
            paid=r::redeem(s.userB,direct,1);
            if(!paid) throw std::runtime_error("M9 paid fixture missing");
            const auto rid2=paid->redemption.id,p2=propose(s.userA,rid2);
            CHECK(r::is(r::fulfill(s.userA,rid2),RO::Fulfilled));
            CHECK(r::is(act(s.userB,rid2,"cancel_approve",p2),RO::InvalidState));
            CHECK(r::get(s.userA,rid2)->redemption.cancellationId.empty());
            // Same-key concurrent confirmations debit once and return identical facts.
            const auto req2=r::redeem(s.userB,product,1);
            if(!req2) throw std::runtime_error("M9 request missing");
            const auto k=r::c::newUuid();
            const auto bal=r::flowers(s.userB);
            a=start(s.userA,req2->redemption.id,"confirm","",k);
            b=start(s.userA,req2->redemption.id,"confirm","",k);
            ar=a->wait();br=b->wait();
            CHECK((r::is(ar,RO::Changed)&&r::is(br,RO::Replayed)) || (r::is(br,RO::Changed)&&r::is(ar,RO::Replayed)));
            if(ar&&br) CHECK_EQ(r::facts(*ar),r::facts(*br));
            CHECK_EQ(r::flowers(s.userB),bal-4);
        } catch(const std::exception &e) { std::cerr << "M9: " << e.what() << std::endl; CHECK(false); }
    }
    TEST_CASE("M9: real refund cap rejection preserves proposal; same-key retry after spending succeeds") {
        try {
            r::ts::CommittedSpace s;r::ts::commitSpace(pool,s,true);
            // One isolated cap fixture, no shared initializer or reset. Legal
            // entries remain <=999; a synthetic single oversized entry is invalid.
            observer.client->execSqlSync(
                "DO $fund$ DECLARE op uuid; i bigint; BEGIN "
                "INSERT INTO wallet_accounts(user_id,couple_id,asset_type) VALUES('"+s.userB+"','"+s.couple+"','flower'); "
                "FOR i IN 1..1002 LOOP op:=gen_random_uuid(); "
                "INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) VALUES(op,'"+
                s.couple+"','"+s.userB+"','TASK_REWARD','wallet_test',op::text); "
                "INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) "
                "VALUES(op,'"+s.couple+"','"+s.userB+"','flower',i,CASE WHEN i=1002 THEN 1 ELSE 999 END,LEAST(i*999,1000000)); "
                "END LOOP; END $fund$");
            const auto product=create(s,"direct",999);
            auto paid=r::redeem(s.userB,product,1);
            if(!paid) throw std::runtime_error("M9 cap paid fixture missing");
            CHECK_EQ(r::flowers(s.userB),999001);
            r::fund(s,999);
            CHECK_EQ(r::flowers(s.userB),1000000);
            const auto id=paid->redemption.id,p=propose(s.userB,id),key=r::c::newUuid();
            CHECK(r::is(act(s.userA,id,"cancel_approve",p,key),RO::BalanceLimit));
            CHECK_EQ(r::flowers(s.userB),1000000);
            const auto stillProposed=r::get(s.userB,id);
            CHECK(stillProposed.has_value());
            if(stillProposed) CHECK_EQ(stillProposed->redemption.cancellationId,p);
            CHECK_EQ(r::count(observer,"SELECT count(*)::bigint AS n FROM wallet_operations WHERE source_type='reward_redemption' AND source_id=$1::text AND operation_type='REDEMPTION_REFUND'",id),0);
            CHECK(r::is(r::redeem(s.userB,product,1),RO::Redeemed));
            CHECK(r::is(act(s.userA,id,"cancel_approve",p,key),RO::Changed));
            CHECK_EQ(r::flowers(s.userB),1000000);
        } catch(const std::exception &e) { std::cerr << "M9 cap: " << e.what() << std::endl; CHECK(false); }
    }
}
} // namespace usapptest::redemptionworkflow
