#pragma once
#include "AdminUserSessionDbTests.h"
#include "services/AdminSharedPlanService.h"
namespace usapptest::adminsharedplan {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;using A=usapp::AdminSharedPlanService;
inline std::string json(const Json::Value &v){Json::StreamWriterBuilder w;return Json::writeString(w,v);}
inline Json::Value body(const std::string &title){Json::Value b;b["title"]=title;for(const auto *k:{"date","time","place","note"})b[k]=Json::nullValue;b["note"]="O12 independent plan note";return b;}
inline Json::Value write(const c::ClientPtr &pool,const std::string &actor,const std::string &plan,const std::string &op,Json::Value request){
    auto tx=c::acquireTx(pool);auto r=c::exec(tx,"SELECT usapp_shared_plan_write($1::uuid,NULLIF($2,'')::uuid,$3,$4,$5::jsonb)::text AS facts",actor,plan,op,c::newUuid(),json(request));
    if(!r.ok||!r.result)throw std::runtime_error("write fixture");Json::Value v;Json::Reader parser;
    if(!parser.parse((*r.result)[0]["facts"].as<std::string>(),v)||v.isMember("error"))throw std::runtime_error("write refused");
    if(!c::commitTx(tx).value_or(false))throw std::runtime_error("fixture commit");return v;
}
inline std::string create(const c::ClientPtr &pool,const std::string &actor,const std::string &title="Plan original"){Json::Value v;v["body"]=body(title);return write(pool,actor,"","create",v)["plan"]["id"].asString();}
inline Json::Value editTerms(int version,const std::string &title){Json::Value v;v["expected_version"]=version;v["body"]=body(title);return v;}
inline auto list(const std::string &couple,int limit=100,const std::string &raw=""){
    A::Cursor cursor;if(!raw.empty()&&!A::decodeCursor(raw,couple,cursor))throw std::runtime_error("cursor fixture");
    auto p=std::make_shared<ts::Pending<A::Result>>();A::list(couple,limit,raw.empty()?nullptr:&cursor,[p](auto r){p->deliver(std::move(r));});return p->wait();
}
inline auto detail(const std::string &couple,const std::string &plan){auto p=std::make_shared<ts::Pending<A::Result>>();A::detail(couple,plan,[p](auto r){p->deliver(std::move(r));});return p->wait();}
inline bool ok(const std::optional<A::Result>&v){return v&&v->outcome==A::Outcome::Ok;}
inline std::string footprint(const c::Observer &o){return o.client->execSqlSync("SELECT concat_ws('|',(SELECT count(*) FROM shared_plans),(SELECT count(*) FROM shared_plan_events),(SELECT count(*) FROM shared_plan_memory_links),(SELECT count(*) FROM memories),(SELECT count(*) FROM media_objects),(SELECT count(*) FROM notifications),(SELECT count(*) FROM wallet_operations),(SELECT count(*) FROM audit_logs)) AS f")[0]["f"].as<std::string>();}
inline void safe(const Json::Value &v){const std::set<std::string> fields={"space","id","status","items","next_cursor","read_at","creator_id","created_at","title","body_version","status_version","plan","body","date","time","place","note","events","total","seq","actor_id","operation","proposal_id","outcome"};if(v.isObject())for(const auto &key:v.getMemberNames()){CHECK(fields.count(key)==1u);safe(v[key]);}else if(v.isArray())for(const auto &row:v)safe(row);}
inline void run(const c::ClientPtr &pool,const std::string &dsn){
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin shared plans: authoritative versions, approved/rejected/withdrawn history, explicit bodies and no writes"){
        try{
            ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);const auto id=create(pool,s.userA),foreign=create(pool,other.userA);
            auto propose=[&](const std::string &title){return write(pool,s.userB,id,"propose",editTerms(2,title))["plan"]["pending_proposal"]["id"].asString();};
            const auto first=write(pool,s.userB,id,"propose",editTerms(1,"Approved title"))["plan"]["pending_proposal"]["id"].asString();
            auto decide=[&](const std::string &p,const std::string &outcome,const std::string &actor){Json::Value v;v["proposal_id"]=p;v["outcome"]=outcome;return write(pool,actor,id,"decide",v);};
            decide(first,"approved",s.userA);Json::Value advance;advance["expected_status"]="want";write(pool,s.userB,id,"advance",advance);
            decide(propose("Rejected title"),"rejected",s.userA);decide(propose("Withdrawn title"),"withdrawn",s.userB);
            write(pool,s.userA,id,"edit",editTerms(2,"Plan current"));advance["expected_status"]="plan";write(pool,s.userA,id,"advance",advance);
            // Maintenance-only extra properties prove projection doesn't forward raw body/request JSON.
            usapptest::memorydeletion::asMaintenance("UPDATE shared_plan_events SET body=body||'{\"memory_id\":\"DO-NOT-EXPORT\",\"private_field\":\"DO-NOT-EXPORT\"}'::jsonb WHERE plan_id='"+id+"' AND body IS NOT NULL;");
            const auto before=footprint(observer);const auto d=detail(s.couple,id);CHECK(ok(d));if(!ok(d))return;
            CHECK_EQ(d->body["plan"]["title"].asString(),std::string("Plan current"));CHECK_EQ(d->body["plan"]["body_version"].asInt(),3);CHECK_EQ(d->body["plan"]["status_version"].asInt(),3);CHECK_EQ(d->body["plan"]["status"].asString(),std::string("finished"));
            CHECK_EQ(d->body["plan"]["body"]["note"].asString(),std::string("O12 independent plan note"));
            CHECK_EQ(d->body["events"]["total"].asString(),std::string("10"));CHECK_EQ(d->body["events"]["items"].size(),10u);CHECK_EQ(d->body["events"]["items"][0]["seq"].asInt(),10);
            std::set<std::string> outcomes;for(const auto &e:d->body["events"]["items"])if(!e["outcome"].isNull())outcomes.insert(e["outcome"].asString());CHECK(outcomes==std::set<std::string>({"approved","rejected","withdrawn"}));
            const auto authoritative=observer.client->execSqlSync("SELECT usapp_shared_plan_read($1::uuid)::text AS v",id);Json::Value facts;Json::Reader reader;CHECK(reader.parse(authoritative[0]["v"].as<std::string>(),facts));CHECK_EQ(d->body["plan"]["status"].asString(),facts["status"].asString());CHECK_EQ(d->body["plan"]["body_version"].asInt(),facts["body_version"].asInt());
            const auto all=list(s.couple);CHECK(ok(all));if(ok(all)){CHECK_EQ(all->body["items"].size(),1u);safe(all->body);}
            safe(d->body);CHECK(json(d->body).find("DO-NOT-EXPORT")==std::string::npos);
            const auto wrong=detail(s.couple,foreign),missing=detail(s.couple,c::newUuid());CHECK(wrong&&wrong->outcome==A::Outcome::PlanNotFound);CHECK(missing&&missing->outcome==A::Outcome::PlanNotFound);
            CHECK_EQ(footprint(observer),before);
        }catch(const std::exception &){c::expectThat(false,"admin-plan-history","fixture/query failure");}
    }
    TEST_CASE("Admin shared plans: bounded long history, pagination, empty/unknown space and cursor binding"){
        try{
            ts::CommittedSpace s,empty;ts::commitSpace(pool,s,true);ts::commitSpace(pool,empty,true);
            const auto none=list(empty.couple);CHECK(ok(none));if(ok(none))CHECK(none->body["items"].empty());
            const auto unknown=list(c::newUuid());CHECK(unknown&&unknown->outcome==A::Outcome::SpaceNotFound);
            for(int i=0;i<4;++i)create(pool,s.userA,"Page plan");const auto longPlan=create(pool,s.userA,"Long plan");
            for(int version=1;version<55;++version)write(pool,s.userA,longPlan,"edit",editTerms(version,"Long plan "+std::to_string(version)));
            const auto d=detail(s.couple,longPlan);CHECK(ok(d));if(ok(d)){CHECK_EQ(d->body["events"]["total"].asString(),std::string("55"));CHECK_EQ(d->body["events"]["items"].size(),50u);CHECK_EQ(d->body["events"]["items"][0]["seq"].asInt(),55);CHECK_EQ(d->body["events"]["items"][49]["seq"].asInt(),6);}
            std::vector<std::string> expected;for(const auto &row:observer.client->execSqlSync("SELECT id::text FROM shared_plans WHERE couple_id=$1::uuid ORDER BY created_at DESC,id DESC",s.couple))expected.push_back(row["id"].as<std::string>());
            for(int limit:{1,2,5,100}){std::vector<std::string> got;std::string cursor;for(int i=0;i<10;++i){const auto p=list(s.couple,limit,cursor);CHECK(ok(p));if(!ok(p))break;for(const auto &item:p->body["items"])got.push_back(item["id"].asString());if(p->body["next_cursor"].isNull())break;cursor=p->body["next_cursor"].asString();}CHECK(got==expected);}
            const auto first=list(s.couple,1);CHECK(ok(first));if(!ok(first))return;const auto raw=first->body["next_cursor"].asString();A::Cursor cursor;CHECK(A::decodeCursor(raw,s.couple,cursor));CHECK_FALSE(A::decodeCursor(raw,empty.couple,cursor));CHECK_FALSE(A::decodeCursor(raw+"x",s.couple,cursor));CHECK_FALSE(A::decodeCursor("p1.9223372036854775808."+longPlan+"."+s.couple,s.couple,cursor));
        }catch(const std::exception &){c::expectThat(false,"admin-plan-page","fixture/query failure");}
    }
}
}
