#pragma once
#include "AnniversaryDbTests.h"
#include "RedemptionWorkflowDbTests.h"
#include "services/NotificationService.h"
#include "services/NotificationWriter.h"

namespace usapptest::notification {
namespace a = usapptest::anniversary;
namespace r = usapptest::redemption;
namespace rw = usapptest::redemptionworkflow;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using NS = usapp::NotificationService;
inline auto list(const std::string &user, int limit = 100, const std::optional<NS::Cursor> &cursor = std::nullopt,
                 bool unread = false) {
    auto p = std::make_shared<ts::Pending<NS::Result>>();
    NS::list(user, limit, cursor, unread, [p](NS::Result s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline auto read(const std::string &user, const std::string &id) {
    auto p = std::make_shared<ts::Pending<NS::Result>>();
    NS::markRead(user, id, [p](NS::Result s) { p->deliver(std::move(s)); }); return p->wait();
}
inline auto unreadCount(const std::string &user) {
    auto p = std::make_shared<ts::Pending<NS::Result>>();
    NS::unreadCount(user, [p](NS::Result s) { p->deliver(std::move(s)); }); return p->wait();
}
inline long long notices(const c::Observer &o, const std::string &space) {
    return c::scalar(o, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id=$1::uuid", space);
}
inline void runNotificationDbTests(const drogon::orm::DbClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);
    TEST_CASE("P2-3a: anniversary proposal/outcome, receipt authorization/replay, pages") {
        try {
            ts::CommittedSpace s, other; ts::commitSpace(pool,s,true); ts::commitSpace(pool,other,true);
            auto created = a::create(s.userA,a::terms("p23","2020-01-01","anniversary"));
            CHECK(a::is(created,a::AO::Created)); CHECK_EQ(notices(observer,s.couple),0);
            auto id=a::idOf(created); auto key=c::newUuid();
            auto p=a::propose(s.userB,id,1,a::terms("p23 edit","2020-01-02","anniversary"),key);
            CHECK(a::is(p,a::AO::Created)); CHECK_EQ(notices(observer,s.couple),1);
            auto replay=a::propose(s.userB,id,1,a::terms("p23 edit","2020-01-02","anniversary"),key);
            CHECK(a::is(replay,a::AO::Replayed)); CHECK_EQ(notices(observer,s.couple),1);
            auto inbox=list(s.userA,1); CHECK(inbox && inbox->status==200);
            if (!inbox || inbox->body["notifications"].empty()) throw std::runtime_error("missing notification");
            auto n=inbox->body["notifications"][0]; auto nid=n["id"].asString();
            CHECK_EQ(n["event_type"].asString(),std::string("anniversary.edit_proposed"));
            auto countBefore = unreadCount(s.userA);
            CHECK(countBefore && countBefore->status == 200 && countBefore->body["unread_count"].asInt64() == 1);
            auto forbidden = read(s.userB,nid), crossSpace = read(other.userA,nid);
            CHECK(forbidden && forbidden->status==404); CHECK(crossSpace && crossSpace->status==404);
            auto first=read(s.userA,nid), again=read(s.userA,nid);
            CHECK(first && again && first->status==200 && again->status==200);
            if(first && again) CHECK(first->body==again->body);
            auto countAfter = unreadCount(s.userA);
            CHECK(countAfter && countAfter->status == 200 && countAfter->body["unread_count"].asInt64() == 0);
            auto unread=list(s.userA,100,std::nullopt,true);
            CHECK(unread && unread->body["notifications"].empty());
            auto decided=a::decide(s.userA,id,p->mutation["proposal_id"].asString(),a::AS::Decision::Approve);
            CHECK(a::is(decided,a::AO::Ok)); CHECK_EQ(notices(observer,s.couple),2);
            auto partner=list(s.userB); CHECK(partner && partner->body["notifications"].size()==1u);
            CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id=$1::uuid",s.couple),0);
        } catch(const std::exception &) { c::expectThat(false,"p23-anniversary","case failed (details withheld)"); }
    }
    TEST_CASE("P2-3a: direct/request/confirm/cancellation/refund/fulfillment notify exactly once") {
        try {
            ts::CommittedSpace s; ts::commitSpace(pool,s,true); r::fund(s,20);
            auto product=rw::create(s,"direct",2); auto key=c::newUuid();
            auto direct=r::redeem(s.userB,product,1,key); CHECK(r::is(direct,r::RO::Redeemed));
            CHECK_EQ(notices(observer,s.couple),1);
            CHECK(r::is(r::redeem(s.userB,product,1,key),r::RO::Replayed)); CHECK_EQ(notices(observer,s.couple),1);
            if(!direct) throw std::runtime_error("redeem missing");
            CHECK(r::is(r::fulfill(s.userA,direct->redemption.id),r::RO::Fulfilled)); CHECK_EQ(notices(observer,s.couple),2);
            auto request=r::redeem(s.userB,rw::create(s),1); CHECK(r::is(request,r::RO::Requested));
            if(!request) throw std::runtime_error("request missing");
            auto rid=request->redemption.id;
            CHECK_EQ(notices(observer,s.couple),3);
            CHECK(r::is(rw::act(s.userA,rid,"confirm"),r::RO::Changed)); CHECK_EQ(notices(observer,s.couple),4);
            auto proposal=rw::propose(s.userB,rid); CHECK_EQ(notices(observer,s.couple),5);
            CHECK(r::is(rw::act(s.userA,rid,"cancel_approve",proposal),r::RO::Changed));
            CHECK_EQ(notices(observer,s.couple),6); CHECK_EQ(r::flowers(s.userB),18);
            auto page=list(s.userA,1); CHECK(page && page->body["next_cursor"].isString());
            if(page) {
                auto cursor=usapp::MessageService::decodeCursor(page->body["next_cursor"].asString());
                auto more=list(s.userA,1,cursor); CHECK(more && more->body["notifications"].size()==1u);
                if(more) CHECK(page->body["notifications"][0]["id"]!=more->body["notifications"][0]["id"]);
            }
        } catch(const std::exception &) { c::expectThat(false,"p23-redemption","case failed (details withheld)"); }
    }
    TEST_CASE("P2-3a: task approval successor gives one outcome, no publish notice") {
        try {
            ts::CommittedSpace s; ts::commitSpace(pool,s,true);
            auto task=c::publish(s.userA,"p23 task",1,0);
            auto terms=ts::simpleInput("p23 successor",1,0);
            auto proposal=r::tc::propose(s.userA,task,r::tc::editInput(terms));
            CHECK(r::tc::is(proposal,r::tc::CO::Proposed));
            if(!proposal) throw std::runtime_error("proposal missing");
            auto decided=r::tc::decide(s.userB,task,r::tc::idOf(proposal),r::tc::Change::Action::Approve);
            CHECK(r::tc::is(decided,r::tc::CO::Decided)); CHECK_EQ(notices(observer,s.couple),2);
        } catch(const std::exception &) { c::expectThat(false,"p23-task","case failed (details withheld)"); }
    }
    TEST_CASE("P2-3a: writer failure rolls back staged ledger and audit in caller TxScope") {
        try {
            ts::CommittedSpace s; ts::commitSpace(pool,s,true); r::fund(s,2);
            auto ready=std::make_shared<ts::Pending<std::shared_ptr<usapp::db::TxScope>>>();
            auto failed=std::make_shared<ts::Pending<bool>>();
            usapp::db::TxScope::begin([ready](std::shared_ptr<usapp::db::TxScope> scope){ready->deliver(scope);},
                                     [failed](const std::string &){failed->deliver(true);});
            auto result=ready->wait(); if(!result) throw std::runtime_error("scope missing");
            auto scope=*result; auto latest=c::latestEntry(observer,s.userB,"flower");
            std::string op=c::newUuid();
            scope->tx().execSqlSync("INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) "
                                   "VALUES($1::uuid,$2::uuid,$3::uuid,'TASK_REWARD','p23_failure',$1::uuid)",op,s.couple,s.userB);
            std::string seq=std::to_string(latest.seq+1),balance=std::to_string(latest.balance+1);
            scope->tx().execSqlSync("INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) "
                                   "VALUES($1::uuid,$2::uuid,$3::uuid,'flower',$4::bigint,1,$5::bigint)",op,s.couple,s.userB,seq,balance);
            scope->tx().execSqlSync("INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id) "
                                   "VALUES('user',$1::uuid,$2::uuid,'p23.failure','notification',$3)",s.userB,s.couple,op);
            usapp::writeNotification(scope,"redemption",c::newUuid(),
                [failed](){failed->deliver(false);},[scope](){scope->fail("p23:injected_missing_source");});
            auto outcome=failed->wait(); CHECK(outcome && *outcome);
            // Wait for the requested rollback to release the wallet row lock before observing.
            observer.client->execSqlSync("SELECT 1 FROM wallet_accounts WHERE user_id=$1::uuid FOR UPDATE",s.userB);
            CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM wallet_operations WHERE id=$1::uuid",op),0);
            CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM wallet_ledger_entries WHERE operation_id=$1::uuid",op),0);
            CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id=$1",op),0);
        } catch(const std::exception &) { c::expectThat(false,"p23-rollback","case failed (details withheld)"); }
    }
}
}
