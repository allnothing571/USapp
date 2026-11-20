#pragma once

// P2-6 R5: usapp_reminder_occurrence_valid against reminders the real job generated and the real completion, closure,
// rule and anniversary services, on literal usapp_test. The pending push itself is tested in PushDispatchDbTest.cc.
// Runs after ReminderSuccessorDbTests, whose helpers and environment it uses.
#include <string>

#include "ReminderSuccessorDbTests.h"

namespace usapptest::remindersource {
namespace c = usapptest::taskcompletion;
namespace d = usapptest::daily;
namespace r = usapptest::reminder;
namespace j = usapptest::reminderjob;
namespace tc = usapptest::taskchange;
namespace a = usapptest::anniversary;
namespace rs = usapptest::remindersuccessor;
using Change = usapp::TaskChangeService;

inline const c::Observer &obs() { return j::obs(); }

inline std::string noticeOf(const std::string &target) {
    const auto rows = obs().client->execSqlSync(
        "SELECT n.id::text AS id FROM notifications n JOIN reminder_occurrences o ON o.id = n.reminder_occurrence_id "
        "WHERE o.task_id = $1::uuid OR o.anniversary_id = $1::uuid", target);
    if (rows.size() != 1u) throw std::runtime_error("no single reminder notice");
    return rows[0]["id"].as<std::string>();
}
inline bool valid(const std::string &notice) {
    return obs().client->execSqlSync("SELECT usapp_reminder_occurrence_valid($1::uuid) AS v", notice)[0]["v"].as<bool>();
}
inline std::string inboxRow(const std::string &notice) {
    return obs().client->execSqlSync("SELECT to_jsonb(n)::text AS v FROM notifications n WHERE n.id = $1::uuid", notice)[0]["v"].as<std::string>();
}

inline void runReminderSourceDbTests() {
    TEST_CASE("P2-6 R5: completion, cancellation and supersession end a task reminder's source; the inbox row stays") {
        try {
            const auto s = j::space();
            j::usePolicy(j::onPolicy());
            j::drain();
            const auto done = j::dueOneOff(s);
            const auto cancelled = j::dueOneOff(s);
            const auto edited = j::dueOneOff(s);
            const auto pendingCancel = j::dueOneOff(s);
            const auto retimed = j::dueOneOff(s);
            CHECK_EQ(j::tickCreated(), 5);
            const auto nDone = noticeOf(done), nCancelled = noticeOf(cancelled), nEdited = noticeOf(edited);
            const auto nPending = noticeOf(pendingCancel), nRetimed = noticeOf(retimed);
            for (const auto &notice : {nDone, nCancelled, nEdited, nPending, nRetimed}) CHECK(valid(notice));

            const auto before = inboxRow(nDone);
            c::runComplete(s.userB, done, c::newUuid());
            CHECK_FALSE(valid(nDone));
            CHECK_EQ(inboxRow(nDone), before);

            const auto cancel = tc::propose(s.userA, cancelled, tc::cancelInput());
            CHECK(tc::is(cancel, tc::CO::Proposed));
            // A pending proposal changes nothing.
            CHECK(valid(nCancelled));
            CHECK(tc::is(tc::decide(s.userB, cancelled, tc::idOf(cancel), Change::Action::Approve), tc::CO::Decided));
            CHECK_FALSE(valid(nCancelled));

            // Any approved edit, a title alone included, supersedes: the reminder belongs to the closed task.
            const auto next = rs::approveEdit(s, edited, rs::dated(9));
            CHECK_FALSE(valid(nEdited));
            CHECK_EQ(j::occurrences(next), 0);

            // A pending cancellation leaves it valid; so does a changed instant, through the real rule service.
            CHECK(tc::is(tc::propose(s.userA, pendingCancel, tc::cancelInput()), tc::CO::Proposed));
            CHECK(valid(nPending));
            CHECK(r::is(r::setTask(s.userB, retimed, r::absolute(d::day(obs(), 3), "09:00")), r::RO::Created));
            CHECK(valid(nRetimed));
            CHECK(r::is(r::setTask(s.userB, retimed, r::disabled()), r::RO::Created));
            CHECK_FALSE(valid(nRetimed));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-source-tasks", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: a period's source ends only with that period's completion; partial progress keeps it") {
        try {
            const auto s = j::space();
            j::drain();
            const auto daily = j::dueDaily(s);
            const auto weekly = j::dueWeekly(s);
            CHECK_EQ(j::tickCreated(), 2);
            const auto nDaily = noticeOf(daily), nWeekly = noticeOf(weekly);
            const auto date = j::today();
            d::run(s.userB, daily, date, c::newUuid());
            CHECK(valid(nDaily));
            d::run(s.userB, daily, date, c::newUuid());
            CHECK_FALSE(valid(nDaily));
            // Another task's completion is nobody else's.
            CHECK(valid(nWeekly));
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-source-periods", "case failed (details withheld)");
        }
    }

    TEST_CASE("P2-6 R5: an anniversary's source follows the current schedule; title and note are not part of it") {
        try {
            const auto s = j::space();
            j::drain();
            const auto item = j::dueAnniversary(s, s.userB, 1);
            const auto off = j::dueAnniversary(s, s.userB, 1);
            CHECK_EQ(j::tickCreated(), 2);
            const auto notice = noticeOf(item.id), nOff = noticeOf(off.id);
            CHECK(valid(notice));
            a::edit(s.userA, item.id, 1, a::terms("renamed", item.base, "anniversary", "note"));
            CHECK(valid(notice));
            a::edit(s.userA, item.id, 2, a::terms("moved", j::baseFor(3), "anniversary"));
            CHECK_FALSE(valid(notice));
            a::edit(s.userA, item.id, 3, a::terms("back", item.base, "anniversary"));
            CHECK(valid(notice));
            // Still one occurrence: moving back never fires a second one.
            CHECK_EQ(j::tickCreated(), 0);
            CHECK_EQ(j::occurrences(item.id), 1);
            // The same day of the target's year, as a countdown.
            a::edit(s.userA, item.id, 4, a::terms("kind", d::day(obs(), 1), "countdown"));
            CHECK(valid(notice));
            a::edit(s.userA, item.id, 5, a::terms("kind away", d::day(obs(), 2), "countdown"));
            CHECK_FALSE(valid(notice));
            // A changed time keeps it. Disabling ends it.
            CHECK(r::is(r::setAnniversary(s.userB, off.id, r::offsets({1}, "23:59")), r::RO::Created));
            CHECK(valid(nOff));
            CHECK(r::is(r::setAnniversary(s.userB, off.id, r::offsets({3}, "23:59")), r::RO::Created));
            CHECK_FALSE(valid(nOff));
            CHECK(r::is(r::setAnniversary(s.userB, off.id, r::offsets({1, 3}, "23:59")), r::RO::Created));
            CHECK(valid(nOff));
            CHECK(r::is(r::setAnniversary(s.userB, off.id, r::disabled()), r::RO::Created));
            CHECK_FALSE(valid(nOff));
            j::usePolicy(j::gen::Policy{});
        } catch (const std::exception &) {
            c::expectThat(false, "reminder-source-anniversary", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::remindersource
