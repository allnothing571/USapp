#pragma once
#include "AdminUserSessionDbTests.h"
#include "PushDbTests.h"
#include "NotificationDbTests.h"
#include "services/AdminNotificationService.h"
#include "services/MessageService.h"
namespace usapptest::adminnotification {
namespace c=usapptest::taskcompletion;
namespace ts=usapptest::taskservice;
namespace p=usapptest::push;
using A=usapp::AdminNotificationService;
inline auto list(const std::string &user,int limit=100,const std::string &raw="") {
    A::Cursor cursor;if(!raw.empty()&&!A::decodeCursor(raw,user,cursor))throw std::runtime_error("cursor fixture");
    auto pending=std::make_shared<ts::Pending<A::Result>>();
    A::list(user,limit,raw.empty()?nullptr:&cursor,[pending](A::Result r){pending->deliver(std::move(r));});return pending->wait();
}
inline bool ok(const std::optional<A::Result> &v){return v&&v->outcome==A::Outcome::Ok;}
inline std::string notice(const std::string &actor){
    auto pending=std::make_shared<ts::Pending<usapp::MessageService::Status>>();usapp::intent::Choice choice;choice.provided=true;choice.value=true;
    usapp::MessageService::instance().create(actor,c::newUuid(),"PRIVATE-NOTIFICATION-CONTENT",[pending](auto s){pending->deliver(std::move(s));},choice);
    const auto v=pending->wait();if(!v||!v->success()||!v->notification["notification_id"].isString())throw std::runtime_error("notice fixture");return v->notification["notification_id"].asString();
}
inline std::string footprint(const c::Observer &o){
    return o.client->execSqlSync("SELECT concat_ws('|',(SELECT count(*) FROM notifications),(SELECT count(*) FROM notification_reads),"
        "(SELECT count(*) FROM push_deliveries),(SELECT count(*) FROM push_attempts),(SELECT count(*) FROM push_attempt_observations),"
        "(SELECT count(*) FROM push_attempt_expiries),(SELECT count(*) FROM push_delivery_outcomes),(SELECT count(*) FROM push_subscription_revocations),"
        "(SELECT count(*) FROM push_dispatch_control),(SELECT count(*) FROM audit_logs)) AS f")[0]["f"].as<std::string>();
}
inline void safe(const Json::Value &v){
    const std::set<std::string> allowed={"user","id","status","items","next_cursor","read_at","event_type","created_at","push","total","generation_id","final_state","decided_at","latest_attempt","attempt_no","claimed_at","lease_until","expired_at","observation","class","outcome","http_status","retry_after_s","observed_at"};
    if(v.isObject())for(const auto &key:v.getMemberNames()){CHECK(allowed.count(key)==1u);safe(v[key]);}else if(v.isArray())for(const auto &row:v)safe(row);
}
inline void run(const c::ClientPtr &pool,const std::string &dsn){
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin notifications: real notices/read receipts, bounded push metadata, latest attempt and unchanged effects"){
        try{
            ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);
            const auto n=notice(s.userA),unread=notice(s.userA),foreign=notice(other.userA);
            const auto receipt=usapptest::notification::read(s.userB,n);CHECK(receipt.has_value());if(!receipt)return;CHECK_EQ(receipt->status,200);
            const auto session=p::commitSession(pool,s.userB);
            for(int j=0;j<6;++j){
                const auto registration=p::reg(s.userB,session,c::newUuid(),p::device().reg);CHECK(p::is(registration,p::PO::Created));
                const auto g=p::gen(registration);if(g.empty())throw std::runtime_error("generation fixture");
                // Maintenance-only immutable facts. No dispatcher/control enablement or provider call. Parents are real services.
                std::string sql="INSERT INTO push_deliveries(notification_id,generation_id,recipient_id,couple_id,created_at) VALUES('"+n+"','"+g+"','"+s.userB+"','"+s.couple+"',clock_timestamp()); ";
                if(j>0){
                    const auto lease=j==4?"clock_timestamp()-interval '1 second'":"clock_timestamp()+interval '20 seconds'";
                    sql+="INSERT INTO push_attempts(notification_id,generation_id,attempt_no,instance_id,claimed_at,lease_until) VALUES('"+n+"','"+g+"',1,'PRIVATE-INSTANCE',clock_timestamp()-interval '10 seconds',"+lease+"); ";
                    if(j==1)sql+="INSERT INTO push_attempt_observations(notification_id,generation_id,attempt_no,outcome,class,http_status) VALUES('"+n+"','"+g+"',1,'accepted','provider',201); INSERT INTO push_delivery_outcomes(notification_id,generation_id,final_state,attempt_no) VALUES('"+n+"','"+g+"','accepted',1); ";
                    if(j==2||j==5)sql+="INSERT INTO push_attempt_observations(notification_id,generation_id,attempt_no,outcome,class,http_status,retry_after_s) VALUES('"+n+"','"+g+"',1,'transient','provider',503,60); ";
                    if(j==2)sql+="INSERT INTO push_attempts(notification_id,generation_id,attempt_no,instance_id,claimed_at,lease_until) VALUES('"+n+"','"+g+"',2,'PRIVATE-INSTANCE',clock_timestamp(),clock_timestamp()+interval '20 seconds'); INSERT INTO push_attempt_observations(notification_id,generation_id,attempt_no,outcome,class) VALUES('"+n+"','"+g+"',2,'busy','internal'); ";
                    if(j==4)sql+="INSERT INTO push_attempt_expiries(notification_id,generation_id,attempt_no) VALUES('"+n+"','"+g+"',1); ";
                }
                usapptest::memorydeletion::asMaintenance(sql);
            }
            const auto before=footprint(observer);const auto page=list(s.userB);CHECK(ok(page));if(!ok(page))return;
            CHECK_EQ(page->body["items"].size(),3u); // Two message notices plus the formal push-session security notice.
            for(const auto &row:page->body["items"]){
                CHECK(row["id"].asString()!=foreign);
                if(row["event_type"].asString()=="security.session_created"){
                    CHECK(row["read_at"].isNull());CHECK_EQ(row["push"]["total"].asString(),std::string("0"));CHECK(row["push"]["items"].empty());continue;
                }
                if(row["id"].asString()==unread){CHECK(row["read_at"].isNull());CHECK_EQ(row["push"]["total"].asString(),std::string("0"));CHECK(row["push"]["items"].empty());continue;}
                CHECK_EQ(row["id"].asString(),n);CHECK(row["read_at"].isString());CHECK_EQ(row["push"]["total"].asString(),std::string("6"));CHECK_EQ(row["push"]["items"].size(),5u);
                const auto expected=observer.client->execSqlSync("SELECT generation_id::text AS g FROM push_deliveries WHERE notification_id=$1::uuid ORDER BY created_at DESC,generation_id DESC LIMIT 5",n);
                for(Json::ArrayIndex i=0;i<row["push"]["items"].size();++i){
                    const auto &d=row["push"]["items"][i];const auto g=d["generation_id"].asString();CHECK_EQ(g,expected[i]["g"].as<std::string>());
                    const auto attempts=observer.client->execSqlSync("SELECT a.attempt_no,o.outcome,o.class,o.http_status,x.expired_at FROM push_attempts a LEFT JOIN push_attempt_observations o USING(notification_id,generation_id,attempt_no) LEFT JOIN push_attempt_expiries x USING(notification_id,generation_id,attempt_no) WHERE a.notification_id=$1::uuid AND a.generation_id=$2::uuid ORDER BY a.attempt_no DESC LIMIT 1",n,g);
                    const auto &last=d["latest_attempt"];
                    if(attempts.empty()){CHECK(last.isNull());continue;}
                    CHECK_EQ(last["attempt_no"].asInt(),attempts[0]["attempt_no"].as<int>());
                    CHECK_EQ(last["expired_at"].isNull(),attempts[0]["expired_at"].isNull());
                    if(attempts[0]["outcome"].isNull())CHECK(last["observation"].isNull());else{CHECK_EQ(last["observation"]["outcome"].asString(),attempts[0]["outcome"].as<std::string>());CHECK_EQ(last["observation"]["class"].asString(),attempts[0]["class"].as<std::string>());}
                }
            }
            safe(page->body);Json::StreamWriterBuilder w;const auto encoded=Json::writeString(w,page->body);
            CHECK(encoded.find("PRIVATE-")==std::string::npos);CHECK(encoded.find("mozilla.com")==std::string::npos);
            CHECK_EQ(footprint(observer),before);
            CHECK(ok(list(other.userB))); // Separate recipient scope, not the selected user's receipt or deliveries.
        }catch(const std::exception &){c::expectThat(false,"admin-notification-facts","fixture/query failure");}
    }
    TEST_CASE("Admin notifications: empty/unknown user, cursor binding and complete keyset walk"){
        try{
            ts::CommittedSpace s;ts::commitSpace(pool,s,true);
            CHECK(ok(list(s.userA)));const auto empty=list(s.userA);if(ok(empty))CHECK(empty->body["items"].empty());
            const auto missing=list(c::newUuid());CHECK(missing&&missing->outcome==A::Outcome::UserNotFound);
            for(int i=0;i<4;++i)notice(s.userA);
            std::vector<std::string> expected;for(const auto &r:observer.client->execSqlSync("SELECT id::text FROM notifications WHERE recipient_id=$1::uuid ORDER BY created_at DESC,id DESC",s.userB))expected.push_back(r["id"].as<std::string>());
            for(int limit:{1,2,4,100}){std::vector<std::string> actual;std::string cursor;for(int i=0;i<10;++i){const auto page=list(s.userB,limit,cursor);CHECK(ok(page));if(!ok(page))break;for(const auto &row:page->body["items"])actual.push_back(row["id"].asString());if(page->body["next_cursor"].isNull())break;cursor=page->body["next_cursor"].asString();}CHECK(actual==expected);}
            const auto first=list(s.userB,1);CHECK(ok(first));if(!ok(first))return;const auto raw=first->body["next_cursor"].asString();A::Cursor c1;
            CHECK(A::decodeCursor(raw,s.userB,c1));CHECK_FALSE(A::decodeCursor(raw,s.userA,c1));CHECK_FALSE(A::decodeCursor(raw+"x",s.userB,c1));
            CHECK_FALSE(A::decodeCursor("n1.9223372036854775808."+expected[0]+"."+s.userB,s.userB,c1));
        }catch(const std::exception &){c::expectThat(false,"admin-notification-pages","fixture/query failure");}
    }
}
}
