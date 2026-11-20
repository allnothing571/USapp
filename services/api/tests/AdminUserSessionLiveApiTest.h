#pragma once
#include "AdminWalletLiveApiTest.h"
#include "AdminUserSessionDbTests.h"
namespace usapptest::adminusersessionliveapi {
namespace c=usapptest::taskcompletion;
namespace ts=usapptest::taskservice;
namespace live=usapptest::reminderliveapi;
inline void run(const c::ClientPtr &pool,const std::string &dsn,const std::string &base){
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin user sessions live: both-listener admin boundary, user scope, no secrets or session writes"){
        try{
            const char *adminBase=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(adminBase!=nullptr);if(!adminBase)return;
            ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);
            const std::string adminId = pool->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) VALUES ($1, 'pbkdf2-sha256$1000$AAAA$AAAA', 'active') RETURNING id::text AS id",
                "t_us_" + usapp::crypto::randomHex(5))[0]["id"].as<std::string>();
            const std::string adminToken = usapp::crypto::randomHex(32);
            pool->execSqlSync("INSERT INTO admin_sessions (token_hash, csrf_token_hash, admin_user_id, expires_at) "
                              "VALUES ($1, $2, $3::uuid, now() + interval '1 hour')",
                              usapp::AdminAuthService::tokenHash(adminToken), usapp::crypto::sha256Hex(usapp::crypto::randomHex(32)), adminId);
            const std::string userToken = live::commitToken(pool, s.userB);
            const std::string outsider = live::commitToken(pool, other.userB);
            const std::vector<std::string> surface = {"X-Usapp-Admin-Surface: usapp-admin"};
            const std::vector<std::string> wrongSurface = {"X-Usapp-Admin-Surface: usapp-web"};
            const auto admin = [&](const std::string &listener, const std::string &path, const std::vector<std::string> &headers) {
                auto h = headers;
                h.push_back("Cookie: __Host-usapp-admin=" + adminToken);
                return live::call(listener + path, "", h, nullptr);
            };

            const auto path="/api/admin/v1/users/"+s.userB+"/sessions";
            const auto extra=usapptest::adminusersession::insert(pool,s.userB,"now()-interval '2 days'","now()-interval '1 day'");
            const auto before=usapptest::adminusersession::footprint(observer);
            for(const std::string listener:{base,std::string(adminBase)}){
                CHECK_EQ(live::call(listener+path,"",surface,nullptr).status,401L);
                CHECK_EQ(live::call(listener+path,userToken,surface,nullptr).status,401L);
                CHECK_EQ(live::call(listener+path,outsider,surface,nullptr).status,401L);
                CHECK_EQ(admin(listener,path,{}).status,403L);CHECK_EQ(admin(listener,path,wrongSurface).status,403L);
                const auto response=admin(listener,path,surface);CHECK_EQ(response.status,200L);
                CHECK(response.headers.find("no-store")!=std::string::npos);
                CHECK(response.body.find("token_hash")==std::string::npos);CHECK(response.body.find("csrf")==std::string::npos);
                CHECK(response.body.find(userToken)==std::string::npos);CHECK(response.body.find(usapp::crypto::sha256Hex(userToken))==std::string::npos);
                CHECK(response.body.find("user_agent")==std::string::npos);
                const auto body=live::parsed(response);CHECK_EQ(body["user"]["id"].asString(),s.userB);CHECK_EQ(body["items"].size(),2u);
                CHECK_EQ(body["items"][1]["id"].asString(),extra);CHECK_EQ(body["items"][1]["state"].asString(),std::string("expired"));
            }
            const std::string a(adminBase);
            CHECK_EQ(admin(a,"/api/admin/v1/users/not-a-uuid/sessions",surface).status,400L);
            CHECK_EQ(admin(a,"/api/admin/v1/users/"+c::newUuid()+"/sessions",surface).status,404L);
            for(const auto &query:{"?limit=0","?limit=101","?limit=x","?cursor=bad"})CHECK_EQ(admin(a,path+query,surface).status,400L);
            const auto first=live::parsed(admin(a,path+"?limit=1",surface));const auto cursor=first["next_cursor"].asString();CHECK_FALSE(cursor.empty());
            const auto next=admin(a,path+"?limit=1&cursor="+cursor,surface);CHECK_EQ(next.status,200L);CHECK_EQ(live::parsed(next)["items"][0]["id"].asString(),extra);
            CHECK_EQ(admin(a,"/api/admin/v1/users/"+other.userB+"/sessions?cursor="+cursor,surface).status,400L);
            const auto empty=admin(a,"/api/admin/v1/users/"+s.userA+"/sessions",surface);CHECK_EQ(empty.status,200L);CHECK(live::parsed(empty)["items"].empty());
            CHECK_EQ(usapptest::adminusersession::footprint(observer),before);
        }catch(const std::exception &){c::expectThat(false,"admin-user-session-live","fixture or query failure");}
    }
}
}
