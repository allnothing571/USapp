#pragma once
#include "AdminWalletLiveApiTest.h"
#include "AdminNotificationDbTests.h"
namespace usapptest::adminnotificationliveapi {
namespace c=usapptest::taskcompletion;namespace ts=usapptest::taskservice;namespace live=usapptest::reminderliveapi;
inline void run(const c::ClientPtr &pool,const std::string &dsn,const std::string &base){
    const auto observer=c::Observer::connect(dsn);
    TEST_CASE("Admin notifications live: admin-only, exact recipient, no mark-read/send effects or secret fields"){
        try{
            const char *adminBase=std::getenv("USAPP_TEST_ADMIN_API_URL");CHECK(adminBase!=nullptr);if(!adminBase)return;
            ts::CommittedSpace s,other;ts::commitSpace(pool,s,true);ts::commitSpace(pool,other,true);
            const std::string adminId = pool->execSqlSync(
                "INSERT INTO admin_users (username, password_hash, status) VALUES ($1, 'pbkdf2-sha256$1000$AAAA$AAAA', 'active') RETURNING id::text AS id",
                "t_an_" + usapp::crypto::randomHex(5))[0]["id"].as<std::string>();
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


            const auto id=usapptest::adminnotification::notice(s.userA),second=usapptest::adminnotification::notice(s.userA);
            const auto foreign=usapptest::adminnotification::notice(other.userA);
            const auto path="/api/admin/v1/users/"+s.userB+"/notifications";
            const auto before=usapptest::adminnotification::footprint(observer);
            for(const std::string listener:{base,std::string(adminBase)}){
                CHECK_EQ(live::call(listener+path,"",surface,nullptr).status,401L);CHECK_EQ(live::call(listener+path,userToken,surface,nullptr).status,401L);
                CHECK_EQ(live::call(listener+path,outsider,surface,nullptr).status,401L);CHECK_EQ(admin(listener,path,{}).status,403L);CHECK_EQ(admin(listener,path,wrongSurface).status,403L);
                const auto response=admin(listener,path,surface);CHECK_EQ(response.status,200L);CHECK(response.headers.find("no-store")!=std::string::npos);
                CHECK(response.body.find("PRIVATE-")==std::string::npos);CHECK(response.body.find("endpoint")==std::string::npos);CHECK(response.body.find("p256dh")==std::string::npos);
                const auto body=live::parsed(response);CHECK_EQ(body["user"]["id"].asString(),s.userB);CHECK_EQ(body["items"].size(),3u);
                CHECK_EQ(body["items"][0]["id"].asString(),second);CHECK_EQ(body["items"][1]["id"].asString(),id);
                CHECK_EQ(body["items"][2]["event_type"].asString(),std::string("security.session_created"));
                for(const auto &row:body["items"]){CHECK(row["read_at"].isNull());CHECK(row["id"].asString()!=foreign);CHECK_EQ(row["push"]["total"].asString(),std::string("0"));}
            }
            const std::string a(adminBase);
            CHECK_EQ(admin(a,"/api/admin/v1/users/bad/notifications",surface).status,400L);CHECK_EQ(admin(a,"/api/admin/v1/users/"+c::newUuid()+"/notifications",surface).status,404L);
            for(const auto &query:{"?limit=0","?limit=101","?limit=x","?cursor=bad"})CHECK_EQ(admin(a,path+query,surface).status,400L);
            const auto first=live::parsed(admin(a,path+"?limit=1",surface));const auto cursor=first["next_cursor"].asString();CHECK_FALSE(cursor.empty());
            CHECK_EQ(admin(a,path+"?limit=1&cursor="+cursor,surface).status,200L);
            CHECK_EQ(admin(a,"/api/admin/v1/users/"+other.userB+"/notifications?cursor="+cursor,surface).status,400L);
            const auto empty=admin(a,"/api/admin/v1/users/"+s.userA+"/notifications",surface);CHECK_EQ(empty.status,200L);CHECK(live::parsed(empty)["items"].empty());
            CHECK_EQ(usapptest::adminnotification::footprint(observer),before);
        }catch(const std::exception &){c::expectThat(false,"admin-notification-live","fixture/query failure");}
    }
}
}
