#pragma once

// P2-6 R4: an approved edit copies the assignee's latest rule to the successor, through the real TaskChangeService,
// ReminderService and scan, on literal usapp_test. Runs after ReminderJobDbTests, whose helpers and environment it uses.
#include <optional>
#include <string>

#include "CustomRangeDbTests.h"
#include "ReminderJobDbTests.h"
#include "services/TaskChangeService.h"

namespace usapptest::remindersuccessor {
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace d = usapptest::daily;
namespace r = usapptest::reminder;
namespace j = usapptest::reminderjob;
namespace tc = usapptest::taskchange;
namespace cr = usapptest::customrange;
using Change = usapp::TaskChangeService;

inline const c::Observer &obs() { return j::obs(); }

inline std::optional<Change::Status> propose(const std::string &creator, const std::string &task, const usapp::TaskService::CreateInput &terms) {
    const auto made = tc::propose(creator, task, tc::editInput(terms));
    if (!tc::is(made, tc::CO::Proposed)) throw std::runtime_error("proposal failed");
    return made;
}
// Creator proposes, assignee approves; the successor's id.
inline std::string approveEdit(const ts::CommittedSpace &s, const std::string &task, const usapp::TaskService::CreateInput &terms) {
    const auto made = propose(s.userA, task, terms);
    const auto decided = tc::decide(s.userB, task, tc::idOf(made), Change::Action::Approve);
    if (!tc::is(decided, tc::CO::Decided) || !decided->successor) throw std::runtime_error("approval failed");
    return decided->successor->id;
}
inline usapp::TaskService::CreateInput dated(int days) { return ts::simpleInput("edited", 1, 1, d::day(obs(), days)); }
inline usapp::TaskService::CreateInput recurring(const char *schedule) {
    auto in = ts::simpleInput("edited", 1, 3);
    in.completionMode = "count"; in.targetCount = 2; in.scheduleType = schedule;
    return in;
}

// The list's current state for one target.
inline Json::Value currentOf(const std::string &owner, const std::string &target) {
    const auto listed = r::list(owner);
    if (listed) {
        for (const auto &item : listed->reminders) {
            if (item["target_id"].asString() == target) return item;
        }
    }
    return Json::Value(Json::nullValue);
}
inline long long ruleCount(const std::string &target) { return r::rules(obs(), target); }
inline long long originRows(const std::string &source) {
    return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM reminder_rules WHERE source_rule_id IN "
                            "(SELECT id FROM reminder_rules WHERE task_id = $1::uuid)", source);
}
// The copy is the successor's first revision, with exact terms and provenance to the latest predecessor rule and this decision.
inline bool exactCopy(const std::string &predecessor, const std::string &successor) {
    return c::scalar(obs(),
        "SELECT count(*)::bigint AS n FROM reminder_rules k "
        "JOIN task_change_decisions e ON e.proposal_id = k.source_proposal_id AND e.successor_task_id = k.task_id "
        "JOIN reminder_rules o ON o.id = k.source_rule_id AND o.task_id = $1::uuid AND o.owner_id = k.owner_id "
        "AND o.seq = (SELECT max(m.seq) FROM reminder_rules m WHERE m.task_id = $1::uuid AND m.owner_id = k.owner_id) "
        "AND ROW(o.enabled, o.mode, o.before_minutes, o.days_before, o.at_time, o.at_local_date, o.days_before_set) "
        "IS NOT DISTINCT FROM ROW(k.enabled, k.mode, k.before_minutes, k.days_before, k.at_time, k.at_local_date, k.days_before_set) "
        "WHERE k.task_id = $2::uuid AND k.seq = 1 AND k.origin = 'successor' AND k.idempotency_key IS NULL "
        "AND k.request_fingerprint IS NULL", predecessor, successor) == 1;
}
inline bool needsReset(const Json::Value &current) { return current.isObject() && current["needs_reset"].asBool(); }

inline void runReminderSuccessorDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    (void)pool;
    (void)dsn;

    TEST_CASE("P2-6 R4: enabled, disabled and absent rules copy as they are; the predecessor stays") {
        try {
            const auto s = j::space();
            // Enabled, usable: a later deadline with the same lead.
            const auto enabled = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, enabled, r::before(30)), r::RO::Created));
            const auto before = obs().client->execSqlSync(
                "SELECT id::text || seq::text || created_at::text AS v FROM reminder_rules WHERE task_id = $1::uuid", enabled)[0]["v"].as<std::string>();
            const auto next = approveEdit(s, enabled, dated(9));
            CHECK(exactCopy(enabled, next));
            CHECK_EQ(ruleCount(enabled), 1);
            CHECK_EQ(obs().client->execSqlSync(
                "SELECT id::text || seq::text || created_at::text AS v FROM reminder_rules WHERE task_id = $1::uuid", enabled)[0]["v"].as<std::string>(), before);
            const auto copied = currentOf(s.userB, next);
            CHECK_EQ(copied["origin"].asString(), std::string("successor"));
            CHECK_FALSE(copied["needs_reset"].asBool());
            CHECK(copied["enabled"].asBool());
            CHECK_EQ(copied["before_minutes"].asInt(), 30);
            CHECK(copied["next_fire_at"].isString());
            CHECK_FALSE(copied["fired_this_period"].asBool());
            // It is the assignee's alone: the creator lists nothing.
            CHECK(currentOf(s.userA, next).isNull());
            // Disabled: copied as it is, and the latest revision is the source.
            const auto off = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, off, r::before(30)), r::RO::Created));
            CHECK(r::is(r::setTask(s.userB, off, r::disabled()), r::RO::Created));
            const auto offNext = approveEdit(s, off, dated(9));
            CHECK(exactCopy(off, offNext));
            const auto offCopy = currentOf(s.userB, offNext);
            CHECK_FALSE(offCopy["enabled"].asBool());
            CHECK_FALSE(offCopy["needs_reset"].asBool());
            CHECK(offCopy["next_fire_at"].isNull());
            // No rule: none is created. Nor does ordinary publication create one.
            const auto none = r::publishOneOff(s.userA, obs(), 5);
            CHECK_EQ(ruleCount(none), 0);
            const auto noneNext = approveEdit(s, none, dated(9));
            CHECK_EQ(ruleCount(noneNext), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-copy", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R4: a fired predecessor copies no occurrence, notification or fired state; daily and weekly stay usable") {
        try {
            const auto s = j::space();
            j::drain();
            const auto fired = j::dueDaily(s);
            CHECK_EQ(j::tickCreated(), 1);
            CHECK(j::firedOnce(fired));
            const auto next = approveEdit(s, fired, recurring("daily"));
            CHECK_EQ(j::occurrences(next), 0);
            CHECK_EQ(j::noticesFor(next), 0);
            CHECK(j::firedOnce(fired));
            CHECK(exactCopy(fired, next));
            const auto copied = currentOf(s.userB, next);
            CHECK_FALSE(copied["fired_this_period"].asBool());
            CHECK_FALSE(copied["needs_reset"].asBool());
            CHECK(copied["next_fire_at"].isString());
            // Its copy is a new identity, so it can fire in its own right. The instant has passed for this period, so tomorrow's.
            CHECK_EQ(j::scanFor("", next).size(), 0u);
            CHECK_EQ(j::scanFor(d::day(obs(), 1) + " " + j::minuteNow() + ":30+08", next).size(), 1u);

            const auto weekly = r::publishWeekly(s.userA);
            CHECK(r::is(r::setTask(s.userB, weekly, r::before(10080)), r::RO::Created));
            const auto weeklyNext = approveEdit(s, weekly, recurring("weekly"));
            CHECK(exactCopy(weekly, weeklyNext));
            const auto weeklyCopy = currentOf(s.userB, weeklyNext);
            CHECK_FALSE(weeklyCopy["needs_reset"].asBool());
            CHECK_EQ(weeklyCopy["before_minutes"].asInt(), 10080);
            CHECK(weeklyCopy["next_fire_at"].isString());
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-recurring", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R4: unusable copies approve, keep the choice, have no next instant, generate nothing, and can be set again") {
        try {
            const auto s = j::space();
            const auto unusable = [&](const std::string &predecessor, const std::string &successor, int minutes, const std::string &mode) {
                CHECK(exactCopy(predecessor, successor));
                const auto copied = currentOf(s.userB, successor);
                CHECK(needsReset(copied));
                CHECK(copied["enabled"].asBool());
                CHECK_EQ(copied["mode"].asString(), mode);
                if (minutes > 0) CHECK_EQ(copied["before_minutes"].asInt(), minutes);
                CHECK(copied["next_fire_at"].isNull());
            };
            // The deadline was removed under a mode that needs one.
            const auto removed = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, removed, r::before(30)), r::RO::Created));
            const auto removedNext = approveEdit(s, removed, ts::simpleInput("edited", 1, 1));
            unusable(removed, removedNext, 30, "before_deadline");
            // The new deadline puts the instant in the past.
            const auto passed = r::publishOneOff(s.userA, obs(), 9);
            CHECK(r::is(r::setTask(s.userB, passed, r::localTime(7, "09:00")), r::RO::Created));
            const auto passedNext = approveEdit(s, passed, dated(1));
            unusable(passed, passedNext, 0, "local_time");
            // A deadline was added under absolute: the instant is ahead, so only the check keeps it from the scan.
            const auto open = r::publishNoDeadline(s.userA);
            const auto tomorrow = d::day(obs(), 1);
            CHECK(r::is(r::setTask(s.userB, open, r::absolute(tomorrow, "10:00")), r::RO::Created));
            const auto openNext = approveEdit(s, open, dated(3));
            unusable(open, openNext, 0, "absolute");
            CHECK_EQ(j::scanFor(tomorrow + " 10:01:00+08", openNext).size(), 0u);
            // A custom range now starting after the instant.
            const auto custom = cr::publish(s.userA, 2, d::day(obs(), 1), d::day(obs(), 5));
            CHECK(r::is(r::setTask(s.userB, custom, r::localTime(4, "09:00")), r::RO::Created));
            const auto customNext = approveEdit(s, custom, cr::input(2, d::day(obs(), 2), d::day(obs(), 5)));
            unusable(custom, customNext, 0, "local_time");
            CHECK_EQ(j::scanFor(d::day(obs(), 1) + " 09:01:00+08", customNext).size(), 0u);
            // Due before the approval: its instant predates the successor, and the generator writes nothing.
            j::drain();
            const auto due = j::dueOneOff(s);
            const auto dueNext = approveEdit(s, due, ts::simpleInput("edited", 1, 1));
            CHECK(exactCopy(due, dueNext));
            CHECK(needsReset(currentOf(s.userB, dueNext)));
            CHECK_EQ(j::tickCreated(), 0);
            CHECK(j::nothing(due));
            CHECK(j::nothing(dueNext));
            // Setting it again is an ordinary validated write, which clears the state.
            CHECK(r::is(r::setTask(s.userB, removedNext, r::absolute(tomorrow, "10:00")), r::RO::Created));
            const auto again = currentOf(s.userB, removedNext);
            CHECK_FALSE(needsReset(again));
            CHECK_EQ(again["origin"].asString(), std::string("user"));
            CHECK(again["next_fire_at"].isString());
            CHECK_EQ(j::scanFor(tomorrow + " 10:01:00+08", removedNext).size(), 1u);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-unusable", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R4: a replay appends nothing and does not re-copy; other decisions and publication copy nothing") {
        try {
            const auto s = j::space();
            const auto task = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, task, r::before(30)), r::RO::Created));
            const auto made = propose(s.userA, task, dated(9));
            const auto key = c::newUuid();
            const auto first = tc::decide(s.userB, task, tc::idOf(made), Change::Action::Approve, key);
            CHECK(tc::is(first, tc::CO::Decided));
            const auto successor = first && first->successor ? first->successor->id : std::string();
            CHECK(r::is(r::setTask(s.userB, successor, r::before(90)), r::RO::Created));
            const auto replay = tc::decide(s.userB, task, tc::idOf(made), Change::Action::Approve, key);
            CHECK(tc::is(replay, tc::CO::Replayed));
            CHECK_EQ(ruleCount(successor), 2);
            CHECK_EQ(ruleCount(task), 1);
            CHECK_EQ(originRows(task), 1);
            CHECK_EQ(currentOf(s.userB, successor)["before_minutes"].asInt(), 90);

            const auto kept = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, kept, r::before(30)), r::RO::Created));
            const auto rejected = propose(s.userA, kept, dated(9));
            CHECK(tc::is(tc::decide(s.userB, kept, tc::idOf(rejected), Change::Action::Reject), tc::CO::Decided));
            const auto withdrawn = propose(s.userA, kept, dated(9));
            CHECK(tc::is(tc::decide(s.userA, kept, tc::idOf(withdrawn), Change::Action::Withdraw), tc::CO::Decided));
            const auto cancel = tc::propose(s.userA, kept, tc::cancelInput());
            CHECK(tc::is(tc::decide(s.userB, kept, tc::idOf(cancel), Change::Action::Approve), tc::CO::Decided));
            CHECK_EQ(originRows(kept), 0);
            CHECK_EQ(ruleCount(kept), 1);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_closures WHERE task_id = $1::uuid AND reason = 'cancelled'", kept), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-replay", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R4: approval and rule save serialize in both orders") {
        try {
            const auto s = j::space();
            // Save first: it commits before the approval, whose copy statement sees it.
            const auto first = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, first, r::before(30)), r::RO::Created));
            const auto firstProposal = propose(s.userA, first, dated(9));
            auto holder = j::hold(33004, first);
            auto save = r::start(s.userB, r::Kind::Task, first, r::before(90), c::newUuid());
            CHECK(tc::waiters(obs(), holder.pid, 1));
            auto decide = tc::startDecide(s.userB, first, tc::idOf(firstProposal), Change::Action::Approve, c::newUuid());
            CHECK(tc::waiters(obs(), holder.pid, 2));
            j::release(holder, "reminder-successor-race");
            CHECK(r::is(save->wait(), r::RO::Created));
            const auto approved = decide->wait();
            CHECK(tc::is(approved, tc::CO::Decided));
            if (approved && approved->successor) {
                CHECK(exactCopy(first, approved->successor->id));
                CHECK_EQ(currentOf(s.userB, approved->successor->id)["before_minutes"].asInt(), 90);
            }
            // Approval first: the old root is closed when the save gets the lock, so it is refused.
            const auto second = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, second, r::before(30)), r::RO::Created));
            const auto secondProposal = propose(s.userA, second, dated(9));
            auto gate = j::hold(33004, second);
            auto decideFirst = tc::startDecide(s.userB, second, tc::idOf(secondProposal), Change::Action::Approve, c::newUuid());
            CHECK(tc::waiters(obs(), gate.pid, 1));
            auto late = r::start(s.userB, r::Kind::Task, second, r::before(90), c::newUuid());
            CHECK(tc::waiters(obs(), gate.pid, 2));
            j::release(gate, "reminder-successor-race");
            const auto approvedFirst = decideFirst->wait();
            CHECK(tc::is(approvedFirst, tc::CO::Decided));
            CHECK(r::is(late->wait(), r::RO::TargetClosed));
            CHECK_EQ(ruleCount(second), 1);
            if (approvedFirst && approvedFirst->successor) {
                CHECK(exactCopy(second, approvedFirst->successor->id));
                CHECK_EQ(currentOf(s.userB, approvedFirst->successor->id)["before_minutes"].asInt(), 30);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R4: a copy refused by the guard rolls back the whole approval, with no wallet write; a same-key retry succeeds") {
        try {
            const auto s = j::space();
            const auto task = r::publishOneOff(s.userA, obs(), 5);
            CHECK(r::is(r::setTask(s.userB, task, r::before(30)), r::RO::Created));
            const auto made = propose(s.userA, task, dated(9));
            const auto proposal = tc::idOf(made);
            const auto wallet = [&]() {
                return c::scalar(obs(), "SELECT count(*)::bigint AS n FROM wallet_operations WHERE couple_id = $1::uuid", s.couple);
            };
            const auto walletBefore = wallet();
            const auto key = c::newUuid();
            auto pending = std::make_shared<tc::PendingChange>();
            Change::instance().decide(s.userB, task, proposal, key, Change::Action::Approve,
                                      [pending](Change::Status status) { pending->deliver(std::move(status)); },
                                      Change::Fault::RuleCopyRefused);
            const auto refused = pending->wait();
            CHECK(tc::is(refused, tc::CO::Error));
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM task_change_decisions WHERE proposal_id = $1::uuid", proposal), 0);
            CHECK_EQ(tc::closures(obs(), task), 0);
            CHECK_EQ(c::scalar(obs(), "SELECT count(*)::bigint AS n FROM tasks WHERE create_idempotency_key = 'chg:' || $1", proposal), 0);
            CHECK_EQ(ruleCount(task), 1);
            CHECK_EQ(originRows(task), 0);
            CHECK_EQ(wallet(), walletBefore);
            const auto retried = tc::decide(s.userB, task, proposal, Change::Action::Approve, key);
            CHECK(tc::is(retried, tc::CO::Decided));
            if (retried && retried->successor) CHECK(exactCopy(task, retried->successor->id));
            CHECK_EQ(wallet(), walletBefore);
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-successor-rollback", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::remindersuccessor
