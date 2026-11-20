#pragma once
#include "RedemptionWorkflowDbTests.h"
#include "services/AdminRedemptionService.h"
#include <set>
namespace usapptest::adminredemption {
namespace r=usapptest::redemption;
namespace rw=usapptest::redemptionworkflow;
namespace c=usapptest::taskcompletion;
namespace ts=usapptest::taskservice;
using A=usapp::AdminRedemptionService;
inline auto page(const std::string &couple,const std::string &status="",int limit=100,const std::string &after="") {
    A::Cursor cursor;
    if(!after.empty()&&!A::decodeCursor(after,couple,status,cursor))throw std::runtime_error("cursor fixture");
    auto p=std::make_shared<ts::Pending<A::Result>>();
    A::list(couple,status,limit,after.empty()?nullptr:&cursor,[p](A::Result v){p->deliver(std::move(v));});return p->wait();
}
inline auto detail(const std::string &couple,const std::string &id) {
    auto p=std::make_shared<ts::Pending<A::Result>>();
    A::detail(couple,id,[p](A::Result v){p->deliver(std::move(v));});return p->wait();
}
inline bool ok(const std::optional<A::Result>&v){return v&&v->outcome==A::Outcome::Ok;}
inline void safeKeys(const Json::Value &v) {
    if(v.isObject())for(const auto &k:v.getMemberNames()) {
        CHECK(k!="idempotency_key"&&k!="request_fingerprint"&&k!="operation_id"&&k!="token_hash");safeKeys(v[k]);
    }else if(v.isArray())for(const auto &x:v)safeKeys(x);
}
inline std::string footprint(const c::Observer &o) {
    return o.client->execSqlSync("SELECT concat_ws('|',(SELECT count(*) FROM reward_redemptions),(SELECT count(*) FROM reward_redemption_events),"
        "(SELECT count(*) FROM reward_redemption_fulfillments),(SELECT count(*) FROM wallet_operations),(SELECT count(*) FROM wallet_ledger_entries),"
        "(SELECT count(*) FROM audit_logs),(SELECT count(*) FROM notifications)) AS f")[0]["f"].as<std::string>();
}
inline void run(const c::ClientPtr &pool,const std::string &dsn) {
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin redemptions: real workflow snapshots, status view, events, immutable terms and read-only footprint") {
        try {
            ts::CommittedSpace s;ts::commitSpace(pool,s,true);r::fund(s,30);
            const auto direct=rw::create(s,"direct",4), request=rw::create(s,"request",5);
            const auto paid=r::redeem(s.userB,direct,1), pending=r::redeem(s.userB,request,1);
            CHECK(r::is(paid,r::RO::Redeemed));CHECK(r::is(pending,r::RO::Requested));
            if(!paid||!pending)return;
            const auto id=paid->redemption.id;
            const auto p1=rw::propose(s.userB,id);
            CHECK(r::is(rw::act(s.userA,id,"cancel_reject",p1),r::RO::Changed));
            const auto p2=rw::propose(s.userA,id);
            CHECK(r::is(rw::act(s.userB,id,"cancel_approve",p2),r::RO::Changed));
            const auto fulfilled=r::redeem(s.userB,request,1);
            CHECK(r::is(fulfilled,r::RO::Requested));if(!fulfilled)return;
            CHECK(r::is(rw::act(s.userA,fulfilled->redemption.id,"confirm"),r::RO::Changed));
            CHECK(r::is(r::fulfill(s.userA,fulfilled->redemption.id),r::RO::Fulfilled));
            CHECK(r::pr::is(r::pr::edit(s.userA,direct,1,r::pr::terms("changed product",9,"request")),r::PO::Updated));
            CHECK(r::pr::is(r::pr::archive(s.userA,direct,2),r::PO::Updated));
            const auto before=footprint(observer);
            const auto all=page(s.couple);CHECK(ok(all));if(!ok(all))return;
            CHECK_EQ(all->body["items"].size(),3u);
            for(const auto &item:all->body["items"]) {
                const auto userRead=r::get(s.userB,item["id"].asString());CHECK(userRead.has_value());
                if(userRead)CHECK_EQ(item["status"].asString(),userRead->redemption.status);
                CHECK(item["price_flowers"].isString());
            }
            const auto d=detail(s.couple,id);CHECK(ok(d));if(!ok(d))return;
            CHECK_EQ(d->body["redemption"]["title"].asString(),std::string("M9 snapshot"));
            CHECK_EQ(d->body["redemption"]["price_flowers"].asString(),std::string("4"));
            CHECK_EQ(d->body["redemption"]["product_version"].asInt(),1);
            CHECK_EQ(d->body["redemption"]["status"].asString(),std::string("cancelled"));
            CHECK_EQ(d->body["redemption"]["cancel_reason"].asString(),std::string("mutual"));
            CHECK_EQ(d->body["events"]["total"].asInt64(),4);
            CHECK_EQ(d->body["events"]["items"][0]["action"].asString(),std::string("cancel_approve"));
            CHECK_EQ(d->body["events"]["items"][0]["proposal_id"].asString(),p2);
            const auto f=detail(s.couple,fulfilled->redemption.id);CHECK(ok(f));
            if(ok(f)) {CHECK_EQ(f->body["redemption"]["fulfilled_by"].asString(),s.userA);CHECK(f->body["redemption"]["fulfilled_at"].isString());CHECK_EQ(f->body["events"]["total"].asInt64(),1);safeKeys(f->body);}
            for(const auto &status:{"pending_confirmation","fulfilled","cancelled","pending_fulfillment"}) {
                const auto filtered=page(s.couple,status);CHECK(ok(filtered));
                if(ok(filtered))for(const auto &item:filtered->body["items"])CHECK_EQ(item["status"].asString(),std::string(status));
            }
            safeKeys(all->body);safeKeys(d->body);CHECK_EQ(footprint(observer),before);
        }catch(const std::exception &){c::expectThat(false,"admin-redemption-workflow","fixture or query failed");}
    }
    TEST_CASE("Admin redemptions: couple isolation, empty/unknown, paging and cursor scope") {
        try {
            ts::CommittedSpace s,other,empty;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);ts::commitSpace(pool,empty,true);
            const auto product=rw::create(s), foreignProduct=rw::create(other);
            std::vector<std::string> ids;
            for(int i=0;i<5;++i){const auto req=r::redeem(s.userB,product,1);CHECK(r::is(req,r::RO::Requested));if(req)ids.insert(ids.begin(),req->redemption.id);}
            const auto foreign=r::redeem(other.userB,foreignProduct,1);CHECK(foreign.has_value());if(!foreign)return;
            const auto hidden=detail(s.couple,foreign->redemption.id);CHECK(hidden&&hidden->outcome==A::Outcome::RedemptionNotFound);
            CHECK(ok(detail(other.couple,foreign->redemption.id)));
            const auto missing=detail(s.couple,c::newUuid());CHECK(missing&&missing->outcome==A::Outcome::RedemptionNotFound);
            const auto unknown=page(c::newUuid());CHECK(unknown&&unknown->outcome==A::Outcome::SpaceNotFound);
            const auto none=page(empty.couple);CHECK(ok(none));if(ok(none))CHECK(none->body["items"].empty());
            for(int limit:{1,2,5,100}) {
                std::vector<std::string> walked;std::string cursor;
                for(int i=0;i<10;++i){const auto p=page(s.couple,"",limit,cursor);CHECK(ok(p));if(!ok(p))break;
                    for(const auto &x:p->body["items"])walked.push_back(x["id"].asString());
                    if(p->body["next_cursor"].isNull())break;cursor=p->body["next_cursor"].asString();}
                CHECK(walked==ids);
            }
            const auto first=page(s.couple,"",1);CHECK(ok(first));if(!ok(first))return;
            const auto cursor=first->body["next_cursor"].asString();A::Cursor decoded;
            CHECK(A::decodeCursor(cursor,s.couple,"",decoded));
            CHECK_FALSE(A::decodeCursor(cursor,other.couple,"",decoded));CHECK_FALSE(A::decodeCursor(cursor,s.couple,"cancelled",decoded));
            CHECK_FALSE(A::decodeCursor(cursor+"x",s.couple,"",decoded));
            CHECK_FALSE(A::decodeCursor("r1.all.9223372036854775808."+ids[0]+"."+s.couple,s.couple,"",decoded));
            CHECK_FALSE(A::validStatus("approved"));CHECK(A::validStatus("cancelled"));
        }catch(const std::exception &){c::expectThat(false,"admin-redemption-paging","fixture or query failed");}
    }
}
}
