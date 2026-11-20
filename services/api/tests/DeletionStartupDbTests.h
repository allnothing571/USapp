#pragma once
#include "TestSupport.h"
#include "services/DeletionStartupCheck.h"
namespace usapptest::deletionstartupdb {
inline void run(const usapp::db::ClientPtr &client, bool closed) {
    namespace s = usapp::deletionstartup;
    TEST_CASE("Deletion startup: actual durable recovery marker is read without mutation and cannot be overridden by feature settings") {
        try {
            const auto before=client->execSqlSync("SELECT (SELECT count(*) FROM deletion_recovery_attempts)+(SELECT count(*) FROM deletion_recovery_manifests)+(SELECT count(*) FROM deletion_recovery_quarantine)+(SELECT count(*) FROM deletion_recovery_verifications) AS n")[0]["n"].as<long long>();
            for(const auto requirements:{s::Requirements{},s::Requirements{true,false},s::Requirements{false,true},s::Requirements{true,true}}){
                const auto result=s::check(client,requirements);
                if(closed){CHECK(result.state==s::State::RecoveryClosed);CHECK_FALSE(result.allowed());}
                else if(requirements.mediaStoreRequested||requirements.memoryPurgeRequested){CHECK(result.state==s::State::ProviderUnavailable);CHECK_FALSE(result.allowed());}
                else{CHECK(result.state==s::State::AllowedWithoutProvider);CHECK(result.allowed());}
            }
            CHECK_EQ(client->execSqlSync("SELECT (SELECT count(*) FROM deletion_recovery_attempts)+(SELECT count(*) FROM deletion_recovery_manifests)+(SELECT count(*) FROM deletion_recovery_quarantine)+(SELECT count(*) FROM deletion_recovery_verifications) AS n")[0]["n"].as<long long>(),before);
        } catch (...) { CHECK(false); }
    }
}
} // namespace usapptest::deletionstartupdb
