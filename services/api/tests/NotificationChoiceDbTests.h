#pragma once

// P2-3b: optional notifications and remembered choices on literal usapp_test, after the
// migration/database guard. Isolated committed fixtures (@m3-fixture.test spaces); never resets.
#include "NotificationDbTests.h"
#include "services/NotificationIntent.h"

namespace usapptest::notificationchoice {
namespace a = usapptest::anniversary;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace intent = usapp::intent;
using usapp::AnniversaryService;
using usapp::MessageService;
using usapp::TaskService;

inline const intent::Choice kOmitted{};
inline const intent::Choice kOff{true, false};
inline const intent::Choice kOn{true, true};

inline std::shared_ptr<ts::Pending<TaskService::Status>> startPublish(const std::string &user,
    const TaskService::CreateInput &input, const std::string &key, intent::Choice choice) {
    auto p = std::make_shared<ts::Pending<TaskService::Status>>();
    TaskService::instance().createTask(user, input, key, [p](TaskService::Status s) { p->deliver(std::move(s)); }, choice);
    return p;
}
inline std::optional<TaskService::Status> publish(const std::string &user, const TaskService::CreateInput &input,
                                                  const std::string &key, intent::Choice choice) {
    return startPublish(user, input, key, choice)->wait();
}
inline std::optional<AnniversaryService::Status> createAnniversary(const std::string &user, const std::string &key,
                                                                    intent::Choice choice, const std::string &title = "p23b") {
    auto p = std::make_shared<ts::Pending<AnniversaryService::Status>>();
    AnniversaryService::instance().create(user, key, a::terms(title, "2020-02-02", "anniversary"),
                                          [p](AnniversaryService::Status s) { p->deliver(std::move(s)); }, choice);
    return p->wait();
}
inline std::optional<AnniversaryService::Status> editAnniversary(const std::string &user, const std::string &id,
                                                                  int expected, const std::string &title,
                                                                  intent::Choice choice, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<ts::Pending<AnniversaryService::Status>>();
    AnniversaryService::instance().edit(user, id, key, expected, a::terms(title, "2020-02-02", "anniversary"),
                                        [p](AnniversaryService::Status s) { p->deliver(std::move(s)); }, choice);
    return p->wait();
}
inline std::shared_ptr<ts::Pending<MessageService::Status>> startMessage(const std::string &user, const std::string &text,
                                                                         const std::string &key, intent::Choice choice) {
    auto p = std::make_shared<ts::Pending<MessageService::Status>>();
    MessageService::instance().create(user, key, text, [p](MessageService::Status s) { p->deliver(std::move(s)); }, choice);
    return p;
}
inline std::optional<MessageService::Status> message(const std::string &user, const std::string &text,
                                                     const std::string &key, intent::Choice choice) {
    return startMessage(user, text, key, choice)->wait();
}
inline std::optional<MessageService::Status> reply(const std::string &user, const std::string &id, const std::string &text,
                                                   const std::string &key, intent::Choice choice) {
    auto p = std::make_shared<ts::Pending<MessageService::Status>>();
    MessageService::instance().reply(user, id, key, text, [p](MessageService::Status s) { p->deliver(std::move(s)); },
                                     choice);
    return p->wait();
}
inline std::optional<intent::ChoicesResult> choices(const std::string &user) {
    auto p = std::make_shared<ts::Pending<intent::ChoicesResult>>();
    intent::readChoices(user, [p](intent::ChoicesResult r) { p->deliver(std::move(r)); });
    return p->wait();
}
// {enabled, seq} of one operation for the caller; seq -1 when nothing is remembered.
inline std::pair<bool, int> remembered(const std::string &user, const char *operation) {
    const auto r = choices(user);
    if (!r || r->status != 200) throw std::runtime_error("choices failed");
    const auto &item = r->body["choices"][operation];
    return {item["enabled"].asBool(), item["seq"].isNull() ? -1 : item["seq"].asInt()};
}
inline std::string factsText(const Json::Value &v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}
inline long long count(const c::Observer &o, const std::string &sql, const std::string &x, const std::string &y = "") {
    return c::scalar(o, sql, x, y);
}

inline void runNotificationChoiceDbTests(const drogon::orm::DbClientPtr &pool, const std::string &dsn) {
    const auto observer = c::Observer::connect(dsn);

    TEST_CASE("P2-3b: task publication: omission, explicit choices, key reuse, replay and literal fingerprints") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto input = ts::simpleInput("p23b task", 1, 0);
            const auto legacy = TaskService::createFingerprint(input);

            const auto omittedKey = c::newUuid();
            const auto omitted = publish(s.userA, input, omittedKey, kOmitted);
            CHECK(omitted && omitted->outcome == TaskService::Outcome::Created);
            if (omitted) {
                CHECK_EQ(omitted->notification["reason"].asString(), std::string("not_requested"));
                CHECK_FALSE(omitted->notification["requested"].asBool());
            }
            CHECK(remembered(s.userA, "task_publish") == std::make_pair(false, -1));  // omission is never remembered
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM tasks WHERE creator_id = $1::uuid "
                                     "AND create_idempotency_key = $2 AND create_fingerprint = '" + legacy + "'",
                           s.userA, omittedKey), 1);  // literal legacy bytes for omission
            // Omitted and false classify as the same request (same legacy fingerprint).
            const auto asFalse = publish(s.userA, input, omittedKey, kOff);
            CHECK(asFalse && asFalse->outcome == TaskService::Outcome::Replayed);
            if (asFalse && omitted) CHECK_EQ(factsText(asFalse->notification), factsText(omitted->notification));
            CHECK(remembered(s.userA, "task_publish") == std::make_pair(false, -1));  // replay changes nothing
            CHECK(publish(s.userA, input, omittedKey, kOn)->outcome == TaskService::Outcome::KeyReused);

            const auto onKey = c::newUuid();
            const auto on = publish(s.userA, input, onKey, kOn);
            CHECK(on && on->outcome == TaskService::Outcome::Created);
            std::string noticeId;
            if (on) {
                CHECK(on->notification["created"].asBool());
                CHECK_EQ(on->notification["reason"].asString(), std::string("delivered_to_inbox"));
                noticeId = on->notification["notification_id"].asString();
            }
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE id = $1::uuid "
                                     "AND event_type = 'task.published' AND recipient_id = $2::uuid", noticeId, s.userB), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM tasks WHERE creator_id = $1::uuid "
                                     "AND create_idempotency_key = $2 AND create_fingerprint = '" +
                                     intent::fingerprint(legacy, kOn) + "'", s.userA, onKey), 1);
            CHECK(remembered(s.userA, "task_publish") == std::make_pair(true, 1));
            CHECK(publish(s.userA, input, onKey, kOff)->outcome == TaskService::Outcome::KeyReused);
            CHECK(publish(s.userA, input, onKey, kOmitted)->outcome == TaskService::Outcome::KeyReused);

            const auto off = publish(s.userA, input, c::newUuid(), kOff);
            CHECK(off && off->outcome == TaskService::Outcome::Created);
            CHECK(remembered(s.userA, "task_publish") == std::make_pair(false, 2));
            // Replaying the older "on" request after a newer "off" choice restores nothing.
            const auto oldReplay = publish(s.userA, input, onKey, kOn);
            CHECK(oldReplay && oldReplay->outcome == TaskService::Outcome::Replayed);
            if (oldReplay && on) CHECK_EQ(factsText(oldReplay->notification), factsText(on->notification));
            CHECK(remembered(s.userA, "task_publish") == std::make_pair(false, 2));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid "
                                     "AND event_type = 'task.published'", s.couple), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents WHERE couple_id = $1::uuid",
                           s.couple), 3);
            CHECK(remembered(s.userB, "task_publish") == std::make_pair(false, -1));  // per user

            // A legacy publication (no intent recorded) replays its business result with null facts.
            const auto legacyKey = c::newUuid();
            pool->execSqlSync("INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, "
                              "reward_sunshine, create_idempotency_key, create_fingerprint) VALUES ($1::uuid, $2::uuid, "
                              "$3::uuid, 'p23b task', 1, 0, $4, $5)", s.couple, s.userA, s.userB, legacyKey, legacy);
            const auto legacyReplay = publish(s.userA, input, legacyKey, kOmitted);
            CHECK(legacyReplay && legacyReplay->outcome == TaskService::Outcome::Replayed);
            if (legacyReplay) CHECK(legacyReplay->notification.isNull());
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents WHERE couple_id = $1::uuid",
                           s.couple), 3);
        } catch (const std::exception &) { c::expectThat(false, "p23b-task", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-3b: anniversaries: no partner, later join, distinct revision sources, no approval double notice") {
        try {
            ts::CommittedSpace alone;
            ts::commitSpace(pool, alone, false);
            const auto key = c::newUuid();
            const auto made = createAnniversary(alone.userA, key, kOn);
            CHECK(made && made->outcome == AnniversaryService::Outcome::Created);
            if (made) {
                CHECK(made->hasNotification);
                CHECK(made->notification["requested"].asBool());
                CHECK_FALSE(made->notification["created"].asBool());
                CHECK_EQ(made->notification["reason"].asString(), std::string("no_partner"));
            }
            CHECK(remembered(alone.userA, "anniversary_create") == std::make_pair(false, -1));
            // The partner joins later: the old replay is still no_partner and creates nothing.
            ts::commitUser(pool, alone.userB, "b");
            pool->execSqlSync("INSERT INTO couple_members (user_id, couple_id, slot) VALUES ($1::uuid, $2::uuid, 2)",
                              alone.userB, alone.couple);
            const auto replay = createAnniversary(alone.userA, key, kOn);
            CHECK(replay && replay->outcome == AnniversaryService::Outcome::Replayed);
            if (replay && made) CHECK_EQ(factsText(replay->notification), factsText(made->notification));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid",
                           alone.couple), 0);
            CHECK(remembered(alone.userA, "anniversary_create") == std::make_pair(false, -1));

            // Two direct edits with notify on: two notices with distinct (anniversary, version) sources.
            const std::string id = made ? made->data.id : "";
            const auto e2 = editAnniversary(alone.userA, id, 1, "p23b v2", kOn);
            const auto e3 = editAnniversary(alone.userA, id, 2, "p23b v3", kOn);
            CHECK(e2 && e2->outcome == AnniversaryService::Outcome::Created);
            CHECK(e3 && e3->outcome == AnniversaryService::Outcome::Created);
            CHECK_EQ(count(observer, "SELECT count(DISTINCT anniversary_version)::bigint AS n FROM notifications "
                                     "WHERE anniversary_id = $1::uuid AND event_type = 'anniversary.edited' "
                                     "AND recipient_id = $2::uuid", id, alone.userB), 2);
            CHECK(remembered(alone.userA, "anniversary_edit") == std::make_pair(true, 2));
            // A proposal approval notifies only through the mandatory outcome, never as a direct edit.
            const auto p = a::propose(alone.userB, id, 3, a::terms("p23b proposed", "2020-02-03", "anniversary"));
            const std::string pid = p ? p->mutation["proposal_id"].asString() : "";
            CHECK(a::is(a::decide(alone.userA, id, pid, AnniversaryService::Decision::Approve), a::AO::Ok));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE anniversary_id = $1::uuid "
                                     "AND anniversary_version = 4", id), 0);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE event_type = "
                                     "'anniversary.edit_approved' AND anniversary_proposal_id = $1::uuid", pid), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents WHERE anniversary_id = $1::uuid",
                           id), 3);  // create + two direct edits; no intent for the approval
        } catch (const std::exception &) { c::expectThat(false, "p23b-anniversary", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-3b: messages and replies: recipients, replay, key reuse and concurrent same-key writes") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            const auto key = c::newUuid();
            const auto m = message(s.userA, "p23b note", key, kOn);
            CHECK(m && m->outcome == MessageService::Outcome::Created);
            const std::string id = m ? m->data.id : "";
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE message_id = $1::uuid "
                                     "AND event_type = 'message.created' AND recipient_id = $2::uuid", id, s.userB), 1);
            const auto again = message(s.userA, "p23b note", key, kOn);
            CHECK(again && again->outcome == MessageService::Outcome::Replayed);
            if (again && m) CHECK_EQ(factsText(again->notification), factsText(m->notification));
            CHECK(message(s.userA, "p23b note", key, kOff)->outcome == MessageService::Outcome::KeyReused);

            CHECK(reply(s.userA, id, "self", c::newUuid(), kOn)->outcome == MessageService::Outcome::Forbidden);
            const auto r = reply(s.userB, id, "p23b reply", c::newUuid(), kOn);
            CHECK(r && r->outcome == MessageService::Outcome::Created);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE message_reply_id = $1::uuid "
                                     "AND event_type = 'message.replied' AND recipient_id = $2::uuid", id, s.userA), 1);
            CHECK(remembered(s.userB, "message_reply") == std::make_pair(true, 1));
            CHECK(remembered(s.userB, "message_create") == std::make_pair(false, -1));

            const auto racing = c::newUuid();
            const auto x = startMessage(s.userB, "p23b race", racing, kOn);
            const auto y = startMessage(s.userB, "p23b race", racing, kOn);
            const auto rx = x->wait(), ry = y->wait();
            CHECK((rx && ry) && ((rx->outcome == MessageService::Outcome::Created && ry->outcome == MessageService::Outcome::Replayed) ||
                                 (ry->outcome == MessageService::Outcome::Created && rx->outcome == MessageService::Outcome::Replayed)));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents i JOIN messages d "
                                     "ON d.id = i.message_id WHERE d.idempotency_key = $1", racing), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications n JOIN messages d "
                                     "ON d.id = n.message_id WHERE d.idempotency_key = $1", racing), 1);
            CHECK(remembered(s.userB, "message_create") == std::make_pair(true, 1));
        } catch (const std::exception &) { c::expectThat(false, "p23b-messages", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-3b: a failure after staging rolls back business, audit, intent, notice and preference") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            auto ready = std::make_shared<ts::Pending<std::shared_ptr<usapp::db::TxScope>>>();
            auto ended = std::make_shared<ts::Pending<bool>>();
            usapp::db::TxScope::begin([ready](std::shared_ptr<usapp::db::TxScope> scope) { ready->deliver(scope); },
                                      [ended](const std::string &) { ended->deliver(true); });
            auto got = ready->wait();
            if (!got) throw std::runtime_error("scope missing");
            auto scope = *got;
            const std::string id = c::newUuid();
            scope->tx().execSqlSync("INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, "
                                    "request_fingerprint) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'staged', $5, $6)",
                                    id, s.couple, s.userA, s.userB, task::taskKey(), task::taskFingerprint());
            scope->tx().execSqlSync("INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id) "
                                    "VALUES ('user', $1::uuid, $2::uuid, 'p23b.failure', 'message', $3)",
                                    s.userA, s.couple, id);
            intent::Source source;
            source.messageId = id;
            auto staged = std::make_shared<ts::Pending<std::string>>();
            intent::record(scope, intent::Operation::MessageCreate, source, kOn,
                           [staged](Json::Value facts) { staged->deliver(facts["notification_id"].asString()); },
                           [staged]() { staged->deliver(""); });
            const auto notice = staged->wait();
            CHECK(notice && !notice->empty());  // intent, preference and notice were staged
            scope->fail("p23b:injected_after_staging");
            const auto outcome = ended->wait();
            CHECK(outcome && *outcome);
            // Wait for the rollback to release the preference lock before observing.
            observer.client->execSqlSync("SELECT pg_advisory_lock(33012::integer, hashtext($1 || ':message_create'))",
                                         s.userA);
            observer.client->execSqlSync("SELECT pg_advisory_unlock(33012::integer, hashtext($1 || ':message_create'))",
                                         s.userA);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM messages WHERE id = $1::uuid", id), 0);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1", id), 0);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents WHERE message_id = $1::uuid", id), 0);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE message_id = $1::uuid", id), 0);
            CHECK(remembered(s.userA, "message_create") == std::make_pair(false, -1));
            // A missing source fails the helper instead of inventing facts.
            auto ready2 = std::make_shared<ts::Pending<std::shared_ptr<usapp::db::TxScope>>>();
            auto ended2 = std::make_shared<ts::Pending<bool>>();
            usapp::db::TxScope::begin([ready2](std::shared_ptr<usapp::db::TxScope> sc) { ready2->deliver(sc); },
                                      [ended2](const std::string &) { ended2->deliver(true); });
            auto got2 = ready2->wait();
            if (!got2) throw std::runtime_error("scope missing");
            auto scope2 = *got2;
            intent::Source missing;
            missing.messageId = c::newUuid();
            auto failed = std::make_shared<ts::Pending<bool>>();
            intent::record(scope2, intent::Operation::MessageCreate, missing, kOn,
                           [failed](Json::Value) { failed->deliver(false); }, [failed]() { failed->deliver(true); });
            const auto f = failed->wait();
            CHECK(f && *f);
            scope2->fail("p23b:missing_source");
            ended2->wait();
        } catch (const std::exception &) { c::expectThat(false, "p23b-rollback", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-3b: mandatory notices stay mandatory; successors and lazy periods add no optional notice") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            // Remember "off" for every relevant operation first.
            CHECK(publish(s.userA, ts::simpleInput("p23b off", 1, 0), c::newUuid(), kOff)->outcome ==
                  TaskService::Outcome::Created);
            const auto made = a::create(s.userA, a::terms("p23b mandatory", "2020-01-01", "anniversary"));
            const std::string id = a::idOf(made);
            CHECK(editAnniversary(s.userA, id, 1, "p23b mandatory 2", kOff)->outcome == AnniversaryService::Outcome::Created);
            const auto p = a::propose(s.userB, id, 2, a::terms("p23b proposal", "2020-01-03", "anniversary"));
            CHECK(a::is(p, a::AO::Created));
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid "
                                     "AND event_type = 'anniversary.edit_proposed'", s.couple), 1);
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notifications WHERE couple_id = $1::uuid "
                                     "AND event_type IN ('task.published', 'anniversary.created', 'anniversary.edited')",
                           s.couple), 0);
            // Successor tasks and internal periods are never optional notification sources.
            CHECK_EQ(count(observer, "SELECT count(*)::bigint AS n FROM notification_intents i JOIN tasks t ON t.id = i.task_id "
                                     "WHERE t.couple_id = $1::uuid AND t.recurrence_id IS NOT NULL", s.couple), 0);
        } catch (const std::exception &) { c::expectThat(false, "p23b-mandatory", "case failed (details withheld)"); }
    }

    TEST_CASE("P2-3b: direct SQL guards: forged notices, mismatched intents, sequences, append-only") {
        try {
            ts::CommittedSpace s;
            ts::commitSpace(pool, s, true);
            ts::CommittedSpace other;
            ts::commitSpace(pool, other, true);
            const auto m = message(s.userA, "p23b guard", c::newUuid(), kOff);
            const std::string id = m ? m->data.id : "";
            const auto om = message(other.userA, "p23b other", c::newUuid(), kOff);
            const std::string otherId = om ? om->data.id : "";
            auto intentSql = [&](const std::string &actor, const std::string &message, const std::string &seq,
                                 const std::string &recipient) {
                return "INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, "
                       "requested, recipient_id, preference_seq) VALUES ('" + s.couple + "', '" + actor +
                       "', 'message_create', '" + message + "', true, false, " + recipient + ", " + seq + ")";
            };
            for (const auto &[sql, state] : std::vector<std::pair<std::string, std::string>>{
                     // An optional notice without its requesting intent (checked at statement end below).
                     {"INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, "
                      "message_id) VALUES ('" + s.couple + "', '" + s.userA + "', '" + s.userB + "', 'message.created', "
                      "'message', '" + id + "', '" + id + "'); SET CONSTRAINTS ALL IMMEDIATE", "UN105"},
                     // Wrong recipient for the source.
                     {"INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, "
                      "message_id) VALUES ('" + s.couple + "', '" + s.userB + "', '" + s.userA + "', 'message.created', "
                      "'message', '" + id + "', '" + id + "')", "UN102"},
                     // A second intent for the same message.
                     {intentSql(s.userA, id, "2", "'" + s.userB + "'"), "23505"},
                     // Another space's message (the guard runs before the foreign keys).
                     {intentSql(s.userA, otherId, "NULL", "'" + s.userB + "'"), "UN104"},
                     // Not the author.
                     {"INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, "
                      "requested, recipient_id) VALUES ('" + s.couple + "', '" + s.userB + "', 'message_create', '" + id +
                      "', false, false, '" + s.userA + "')", "UN104"},
                     // Wrong family for the operation.
                     {"INSERT INTO notification_intents (couple_id, actor_id, operation, task_id, choice_explicit, "
                      "requested, recipient_id) VALUES ('" + s.couple + "', '" + s.userA + "', 'message_create', '" + id +
                      "', false, false, '" + s.userB + "')", "23514"},
                     {"UPDATE notification_intents SET requested = true WHERE message_id = '" + id + "'", "42501"},
                     {"DELETE FROM notification_intents WHERE message_id = '" + id + "'", "42501"}}) {
                auto tx = c::acquireTx(pool);
                const auto r = wallet::exec(tx, wallet::expectFailureSql(sql));
                CHECK(!r.ok);
                CHECK_EQ(wallet::parseFailure(r.error).sqlState, state);
                c::rollbackTx(tx);
            }
            CHECK(remembered(s.userA, "message_create") == std::make_pair(false, 1));
        } catch (const std::exception &) { c::expectThat(false, "p23b-sql", "case failed (details withheld)"); }
    }
}

}  // namespace usapptest::notificationchoice
