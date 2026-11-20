#pragma once

// P2-5b: pure dispatch decisions (no database).
#include <string>

#include "TestSupport.h"
#include "scheduler/LockKey.h"
#include "scheduler/jobs/PushDispatchJob.h"
#include "services/PushDispatch.h"

namespace usapptest::pushdispatchunit {

namespace d = usapp::push;

inline void runPushDispatchUnitTests() {
    TEST_CASE("P2-5b: outcome classes keep provider and internal failures distinct") {
        for (const char *o : {"accepted", "gone", "rejected", "transient", "unknown", "provider_auth_failed"}) {
            CHECK_EQ(d::outcomeClass(o), std::string("provider"));
        }
        for (const char *o : {"worker_auth_failed", "transport_unavailable", "busy", "inactive_skipped", "ttl_skipped",
                              "cutover_skipped", "control_skipped", "terminal_skipped", "lease_lost_skipped"}) {
            CHECK_EQ(d::outcomeClass(o), std::string("internal"));
        }
        CHECK(d::outcomeClass("displayed").empty());
        // 0041: a deferral is internal, decides no terminal state, pauses nothing and is a valid result to record.
        CHECK_EQ(d::outcomeClass("notice_ahead_skipped"), std::string("internal"));
        CHECK(d::terminalForOutcome("notice_ahead_skipped").empty());
        CHECK(d::pauseReasonForOutcome("notice_ahead_skipped").empty());
        CHECK(d::validResult({"notice_ahead_skipped", std::nullopt, std::nullopt}));
        CHECK_EQ(d::pauseReasonForOutcome("worker_auth_failed"), std::string("worker_auth_failed"));
        CHECK_EQ(d::pauseReasonForOutcome("provider_auth_failed"), std::string("provider_auth_failed"));
        CHECK(d::pauseReasonForOutcome("busy").empty());
        CHECK_EQ(d::terminalForOutcome("accepted"), std::string("accepted"));
        CHECK_EQ(d::terminalForOutcome("inactive_skipped"), std::string("inactive"));
        CHECK_EQ(d::terminalForOutcome("ttl_skipped"), std::string("expired"));
        CHECK(d::terminalForOutcome("transient").empty());
        CHECK(d::terminalForOutcome("control_skipped").empty());
        CHECK(d::validResult({"transient", 503, 120}));
        CHECK_FALSE(d::validResult({"displayed", std::nullopt, std::nullopt}));
        CHECK_FALSE(d::validResult({"transient", 99, std::nullopt}));
        CHECK_FALSE(d::validResult({"transient", std::nullopt, 86401}));
    }

    TEST_CASE("0041: a notice ahead of the clock defers; every terminal fence still answers first; the TTL stays in the worker's range") {
        d::PreSendFacts ahead{"enabled", false, false, false, true};
        ahead.noticeAhead = true;
        CHECK(d::decidePreSend(ahead) == d::PreSend::NoticeAhead);
        CHECK_EQ(d::skipOutcome(d::PreSend::NoticeAhead), std::string("notice_ahead_skipped"));
        auto stale = ahead;
        stale.sourceStale = true;  // read from a clock that is behind, so it is judged again at the retry
        CHECK(d::decidePreSend(stale) == d::PreSend::NoticeAhead);
        auto under = ahead;
        under.windowUnderASecond = true;
        CHECK(d::decidePreSend(under) == d::PreSend::NoticeAhead);
        auto inactive = ahead;
        inactive.generationActive = false;
        CHECK(d::decidePreSend(inactive) == d::PreSend::Inactive);
        auto paused = ahead;
        paused.controlState = "paused";
        CHECK(d::decidePreSend(paused) == d::PreSend::Control);
        auto cutover = ahead;
        cutover.beforeCutover = true;
        CHECK(d::decidePreSend(cutover) == d::PreSend::Cutover);
        auto expired = ahead;
        expired.ttlReached = true;
        CHECK(d::decidePreSend(expired) == d::PreSend::Ttl);
        auto lease = ahead;
        lease.leaseValid = false;
        CHECK(d::decidePreSend(lease) == d::PreSend::LeaseLost);

        CHECK_EQ(d::boundedTtlSeconds(90, 86400.0), 90);
        CHECK_EQ(d::boundedTtlSeconds(1, 86400.0), 1);
        CHECK_EQ(d::boundedTtlSeconds(86400, 86400.0), 86400);
        CHECK_EQ(d::boundedTtlSeconds(86401, 86400.0), 86400);
        CHECK_EQ(d::boundedTtlSeconds(90000000000LL, 86400.0), 86400);
        CHECK_EQ(d::boundedTtlSeconds(5000, 3600.0), 3600);
        CHECK_EQ(d::boundedTtlSeconds(200000, 200000.0), 86400);
        CHECK_EQ(d::boundedTtlSeconds(0, 86400.0), 0);
        CHECK_EQ(d::boundedTtlSeconds(-90000000000LL, 86400.0), 0);
        CHECK_EQ(d::boundedTtlSeconds(5, 0.9), 0);
    }

    TEST_CASE("P2-5b: control and local capability fail closed; pre-send precedence") {
        CHECK(d::planTick("disabled", true) == d::TickPlan::Idle);
        CHECK(d::planTick("disabled", false) == d::TickPlan::Idle);
        CHECK(d::planTick("enabled", false) == d::TickPlan::FailClosed);
        CHECK(d::planTick("paused", false) == d::TickPlan::FailClosed);
        CHECK(d::planTick("paused", true) == d::TickPlan::Maintain);
        CHECK(d::planTick("enabled", true) == d::TickPlan::Dispatch);
        CHECK(d::planTick("anything", true) == d::TickPlan::Idle);

        d::PreSendFacts ok{"enabled", false, false, false, true};
        CHECK(d::decidePreSend(ok) == d::PreSend::Send);
        CHECK(d::skipOutcome(d::PreSend::Send).empty());
        auto f = ok;
        f.generationActive = false;
        CHECK(d::decidePreSend(f) == d::PreSend::Inactive);
        f.controlState = "paused";
        CHECK(d::decidePreSend(f) == d::PreSend::Control);
        f.beforeCutover = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Cutover);
        f.ttlReached = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Ttl);
        f.hasOutcome = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Terminal);
        CHECK_EQ(d::skipOutcome(d::PreSend::Control), std::string("control_skipped"));

        // The exact attempt is fenced before anything else; a stale claim never reaches Send.
        for (auto stale : {&d::PreSendFacts::expired, &d::PreSendFacts::superseded}) {
            auto s = ok;
            s.*stale = true;
            CHECK(d::decidePreSend(s) == d::PreSend::LeaseLost);
        }
        auto lapsed = ok;
        lapsed.leaseValid = false;
        CHECK(d::decidePreSend(lapsed) == d::PreSend::LeaseLost);
        CHECK_EQ(d::skipOutcome(d::PreSend::LeaseLost), std::string("lease_lost_skipped"));
        auto seen = lapsed;
        seen.observed = true;
        CHECK(d::decidePreSend(seen) == d::PreSend::AlreadyObserved);
        CHECK(d::skipOutcome(d::PreSend::AlreadyObserved).empty());
        auto foreign = seen;
        foreign.ownClaim = false;
        CHECK(d::decidePreSend(foreign) == d::PreSend::NotOwnClaim);
        CHECK(d::skipOutcome(d::PreSend::NotOwnClaim).empty());
        // lease_lost_skipped never reached a provider, so it can never spend the budget.
        CHECK_EQ(d::outcomeClass("lease_lost_skipped"), std::string("internal"));
        CHECK(d::terminalForOutcome("lease_lost_skipped").empty());
        CHECK(d::pauseReasonForOutcome("lease_lost_skipped").empty());
    }

    TEST_CASE("P2-6 R5: a stale source is internal and terminal, and answers only after every existing fence") {
        CHECK_EQ(d::outcomeClass("source_stale_skipped"), std::string("internal"));
        CHECK_EQ(d::terminalForOutcome("source_stale_skipped"), std::string("source_stale"));
        CHECK(d::pauseReasonForOutcome("source_stale_skipped").empty());
        CHECK_EQ(d::skipOutcome(d::PreSend::SourceStale), std::string("source_stale_skipped"));
        d::PreSendFacts stale{"enabled", false, false, false, true};
        stale.sourceStale = true;
        CHECK(d::decidePreSend(stale) == d::PreSend::SourceStale);
        // Stale and past the delivery's lifetime: the lifetime answers, as the tick's own expiry sweep would.
        auto f = stale;
        f.ttlReached = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Ttl);
        // Every existing fence keeps its answer over a stale source.
        f = stale;
        f.beforeCutover = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Cutover);
        f = stale;
        f.controlState = "paused";
        CHECK(d::decidePreSend(f) == d::PreSend::Control);
        f = stale;
        f.generationActive = false;
        CHECK(d::decidePreSend(f) == d::PreSend::Inactive);
        f = stale;
        f.hasOutcome = true;
        CHECK(d::decidePreSend(f) == d::PreSend::Terminal);
        f = stale;
        f.leaseValid = false;
        CHECK(d::decidePreSend(f) == d::PreSend::LeaseLost);
        f = stale;
        f.observed = true;
        CHECK(d::decidePreSend(f) == d::PreSend::AlreadyObserved);
        f = stale;
        f.ownClaim = false;
        CHECK(d::decidePreSend(f) == d::PreSend::NotOwnClaim);
        // Under a second before stale_at, source still valid: the existing guard. A stale source says so instead.
        d::PreSendFacts soon{"enabled", false, false, false, true};
        soon.windowUnderASecond = true;
        CHECK(d::decidePreSend(soon) == d::PreSend::Ttl);
        soon.sourceStale = true;
        CHECK(d::decidePreSend(soon) == d::PreSend::SourceStale);
        // A paused control leaves the attempt pending even under that second.
        soon.sourceStale = false;
        soon.controlState = "paused";
        CHECK(d::decidePreSend(soon) == d::PreSend::Control);
    }

    TEST_CASE("P2-5b: capability needs the local flag, a transport, a key id and a marker secret") {
        d::Policy policy;
        CHECK_FALSE(d::Dispatcher(policy, nullptr).capable());
        policy.localEnabled = true;
        policy.vapidKeyId = std::string(64, 'a');
        policy.markerSecret = "secret";
        // Without a configured transport the capability is incomplete, whatever the flags say.
        CHECK_FALSE(d::Dispatcher(policy, nullptr).capable());
        // The stable scheduler identity and the production defaults of Section 6.
        usapp::scheduler::validateJobKey(usapp::scheduler::PushDispatchJob::kJobKey);
        CHECK_EQ(std::string(usapp::scheduler::PushDispatchJob::kJobKey), std::string("push.dispatch"));
        const d::Policy defaults;
        CHECK_EQ(defaults.capacity, 4);
        CHECK_EQ(defaults.perRecipient, 2);
        CHECK_EQ(defaults.fanoutLimit, 100);
        CHECK(defaults.leaseSeconds == 60.0);
        CHECK(defaults.ttlSeconds == 86400.0);
        CHECK(defaults.backoffSeconds == (std::vector<double>{60.0, 300.0, 1800.0, 7200.0}));
        CHECK_EQ(d::kProviderBudget, 5);
    }
}

}  // namespace usapptest::pushdispatchunit
