#pragma once
#include "AdminWalletDbTests.h"
#include "services/AdminUserSessionService.h"
namespace usapptest::adminusersession {
namespace c=usapptest::taskcompletion;
namespace ts=usapptest::taskservice;
using A=usapp::AdminUserSessionService;
inline auto list(const std::string &user,int limit=100,const std::string &raw="") {
    A::Cursor cursor;
    if(!raw.empty()&&!A::decodeCursor(raw,user,cursor))throw std::runtime_error("cursor fixture");
    auto p=std::make_shared<ts::Pending<A::Result>>();
    A::list(user,limit,raw.empty()?nullptr:&cursor,[p](A::Result r){p->deliver(std::move(r));});return p->wait();
}
inline bool ok(const std::optional<A::Result> &v){return v&&v->outcome==A::Outcome::Ok;}
inline std::string insert(const c::ClientPtr &pool,const std::string &user,const std::string &created,const std::string &expires,const std::string &revoked="NULL") {
    const auto token=usapp::crypto::randomHex(32),csrf=usapp::crypto::randomHex(32);
    return pool->execSqlSync("INSERT INTO sessions(user_id,is_authenticated,token_hash,csrf_token_hash,created_at,last_seen_at,expires_at,revoked_at,user_agent_family) "
        "VALUES(NULLIF($1,'')::uuid,$2,$3,$4,"+created+","+created+","+expires+","+revoked+",'PRIVATE-USER-AGENT') RETURNING id::text AS id",
        user,!user.empty(),usapp::crypto::sha256Hex(token),usapp::crypto::sha256Hex(csrf))[0]["id"].as<std::string>();
}
inline std::string footprint(const c::Observer &o){
    return o.client->execSqlSync("SELECT concat_ws('|',(SELECT md5(COALESCE(string_agg(row_to_json(s)::text,'' ORDER BY id),'')) FROM sessions s),"
        "(SELECT count(*) FROM audit_logs),(SELECT count(*) FROM wallet_operations)) AS f")[0]["f"].as<std::string>();
}
inline void run(const c::ClientPtr &pool,const std::string &dsn) {
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin user sessions: exact user scope, timestamps, revocation precedence, disabled users, ties and no writes") {
        try {
            ts::CommittedSpace s;ts::commitSpace(pool,s,true);
            const std::string at="TIMESTAMPTZ '2020-01-01 00:00:00.123456+00'";
            const auto valid=insert(pool,s.userA,at,"now()+interval '1 hour'");
            const auto expired=insert(pool,s.userA,at,"TIMESTAMPTZ '2020-01-02 00:00:00+00'");
            const auto revoked=insert(pool,s.userA,at,"now()+interval '1 hour'","now()");
            const auto both=insert(pool,s.userA,at,"TIMESTAMPTZ '2020-01-02 00:00:00+00'","now()");
            const auto foreign=insert(pool,s.userB,at,"now()+interval '1 hour'");
            const auto preauth=insert(pool,"",at,"now()+interval '1 hour'");
            const auto before=footprint(observer);
            const auto all=list(s.userA);CHECK(ok(all));if(!ok(all))return;
            CHECK_EQ(all->body["items"].size(),4u);CHECK_EQ(all->body["user"]["id"].asString(),s.userA);
            std::vector<std::string> expected;
            for(const auto &row:observer.client->execSqlSync("SELECT id::text FROM sessions WHERE user_id=$1::uuid ORDER BY created_at DESC,id DESC",s.userA))expected.push_back(row["id"].as<std::string>());
            std::vector<std::string> got;
            const std::set<std::string> fields={"id","created_at","last_seen_at","expires_at","revoked_at","state"};
            for(const auto &row:all->body["items"]){
                const auto id=row["id"].asString();got.push_back(id);
                CHECK(id!=foreign&&id!=preauth);
                CHECK_EQ(row["created_at"].asString(),std::string("2020-01-01T00:00:00.123456Z"));
                for(const auto &key:row.getMemberNames())CHECK(fields.count(key)==1u);
                CHECK_EQ(row["state"].asString(),id==valid?std::string("valid"):id==expired?std::string("expired"):std::string("revoked"));
                if(id==revoked||id==both)CHECK(row["revoked_at"].isString());
            }
            CHECK(got==expected);
            for(int limit:{1,2,3,4,100}){
                std::string cursor;std::vector<std::string> walked;
                for(int page=0;page<10;++page){const auto p=list(s.userA,limit,cursor);CHECK(ok(p));if(!ok(p))break;
                    for(const auto &row:p->body["items"])walked.push_back(row["id"].asString());
                    if(p->body["next_cursor"].isNull())break;cursor=p->body["next_cursor"].asString();}
                CHECK(walked==expected);
            }
            const auto page=list(s.userA,1);CHECK(ok(page));if(!ok(page))return;
            const auto cursor=page->body["next_cursor"].asString();A::Cursor decoded;
            CHECK(A::decodeCursor(cursor,s.userA,decoded));CHECK_EQ(decoded.createdMicros,1577836800123456LL);
            CHECK_FALSE(A::decodeCursor(cursor,s.userB,decoded));CHECK_FALSE(A::decodeCursor(cursor+"x",s.userA,decoded));
            CHECK_FALSE(A::decodeCursor("s1.9223372036854775808."+valid+"."+s.userA,s.userA,decoded));
            CHECK_FALSE(A::decodeCursor("s1.01."+valid+"."+s.userA,s.userA,decoded));
            CHECK_EQ(footprint(observer),before);
            usapptest::memorydeletion::asMaintenance("UPDATE users SET status='disabled' WHERE id='"+s.userA+"';");
            const auto disabled=list(s.userA);CHECK(ok(disabled));if(ok(disabled))CHECK_EQ(disabled->body["user"]["status"].asString(),std::string("disabled"));
        }catch(const std::exception &){c::expectThat(false,"admin-user-session-metadata","fixture/query failure");}
    }
    TEST_CASE("Admin user sessions: existing user without space or sessions, unknown user, between-page insertion"){
        try{
            const auto user=pool->execSqlSync("INSERT INTO users(email_canonical,email_display) VALUES($1,$1) RETURNING id::text AS id","t_us_"+usapp::crypto::randomHex(8)+"@example.test")[0]["id"].as<std::string>();
            const auto empty=list(user);CHECK(ok(empty));if(ok(empty))CHECK(empty->body["items"].empty());
            const auto unknown=list(c::newUuid());CHECK(unknown&&unknown->outcome==A::Outcome::UserNotFound);
            insert(pool,user,"now()-interval '1 day'","now()+interval '1 hour'");insert(pool,user,"now()-interval '2 days'","now()+interval '1 hour'");
            const auto p=list(user,1);CHECK(ok(p));if(!ok(p))return;
            const auto late=insert(pool,user,"now()","now()+interval '1 hour'");
            const auto next=list(user,100,p->body["next_cursor"].asString());CHECK(ok(next));if(ok(next)){CHECK_EQ(next->body["items"].size(),1u);CHECK(next->body["items"][0]["id"].asString()!=late);}
        }catch(const std::exception &){c::expectThat(false,"admin-user-session-empty","fixture/query failure");}
    }
}
}
