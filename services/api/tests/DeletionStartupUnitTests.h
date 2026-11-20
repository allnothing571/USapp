#pragma once
#include "TestSupport.h"
#include <stdexcept>
#include "services/DeletionStartupCheck.h"
namespace usapptest::deletionstartupunit {
inline void run() {
    namespace s = usapp::deletionstartup;
    TEST_CASE("Deletion startup: unknown, closed and provider-dependent configurations cannot start") {
        for (const auto requirements : {s::Requirements{},s::Requirements{true,false},s::Requirements{false,true},s::Requirements{true,true}}) {
            CHECK(s::check(s::Probe{[](s::Reply cb){cb(true);}},requirements).state==s::State::RecoveryClosed);
            CHECK(s::check(s::Probe{[](s::Reply cb){cb(std::nullopt);}},requirements).state==s::State::QueryError);
            const auto open=s::check(s::Probe{[](s::Reply cb){cb(false);}},requirements);
            CHECK_EQ(open.allowed(),!requirements.mediaStoreRequested&&!requirements.memoryPurgeRequested);
            if(!open.allowed()) CHECK(open.state==s::State::ProviderUnavailable);
        }
        CHECK(s::check(s::Probe{},{}).state==s::State::QueryError);
        CHECK(s::check(s::Probe{[](s::Reply){throw std::runtime_error("private diagnostic");}},{}).state==s::State::QueryError);
        CHECK_EQ(std::string(s::Result{s::State::QueryError}.reason()),std::string("recovery_query_unconfirmed"));
    }
    TEST_CASE("Deletion startup: deadline and duplicate or late replies never turn a refused result into permission") {
        s::Reply late;
        const auto timed=s::check(s::Probe{[&](s::Reply cb){late=std::move(cb);}}, {},std::chrono::milliseconds(1));
        CHECK(timed.state==s::State::Timeout); CHECK_FALSE(timed.allowed());
        late(false); late(true); CHECK_FALSE(timed.allowed());
        const auto first=s::check(s::Probe{[](s::Reply cb){cb(true);cb(false);}},{});
        CHECK(first.state==s::State::RecoveryClosed);CHECK_FALSE(first.allowed());
    }
}
} // namespace usapptest::deletionstartupunit
