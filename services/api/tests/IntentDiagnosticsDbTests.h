#pragma once

// Design Section 13: the diagnostic for the intermittent USAPP_NOTIFICATION_INTENT_SOURCE, on literal usapp_test. This tests the instrument, and proves
// nothing about that failure's cause. Since 0041 a write with no time of its own follows a source dated ahead instead of being refused, so what makes the
// real guard refuse the real write path on demand is the source changing while that write waits for its preference lock. Only a maintenance connection
// can change it. Runs inside the memory deletion target, which has one.
#include <json/json.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MemoryCascadeDbTests.h"
#include "NotificationChoiceDbTests.h"

namespace usapptest::intentdiag {

namespace base = usapptest::memorydeletion;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace choice = usapptest::notificationchoice;
namespace intent = usapp::intent;

inline intent::SourcePredicates probe(const std::string &id, const std::string &operation, const std::string &couple, const std::string &actor, const std::string &recipient) {
    const auto rows = base::obs().client->execSqlSync(intent::sourcePredicateProbeSql(), std::string("message"), id, std::string(), operation, couple, actor, recipient);
    intent::SourcePredicates p;
    const auto &row = rows.at(0);
    p.found = row["found"].as<bool>();
    p.operation = row["operation_match"].as<bool>();
    p.couple = row["couple_match"].as<bool>();
    p.actor = row["actor_match"].as<bool>();
    p.recipient = row["recipient_match"].as<bool>();
    p.sourceAgeUs = row["source_age_us"].as<long long>();
    return p;
}
inline bool startsWith(const std::string &text, const std::string &prefix) { return text.rfind(prefix, 0) == 0u; }

inline void runIntentDiagnosticsDbTests() {
    TEST_CASE("Intent diagnostics: each of the guard's predicates has its own label") {
        try {
            const auto s = md::space();
            const auto other = md::space();
            const auto made = choice::message(s.userA, "diag probe", c::newUuid(), choice::kOff);
            CHECK(made.has_value());
            const std::string id = made ? made->data.id : std::string();
            const char *op = "message_create";
            const auto label = [&](const std::string &source, const std::string &operation, const std::string &couple, const std::string &actor, const std::string &recipient) {
                return intent::describeSourceFailure(op, probe(source, operation, couple, actor, recipient));
            };
            // Nothing failed here at all: the identity predicates hold and the clock is forwards, so it says it cannot tell.
            const auto clean = probe(id, op, s.couple, s.userA, s.userB);
            CHECK(clean.found && clean.operation && clean.couple && clean.actor && clean.recipient);
            CHECK(clean.sourceAgeUs >= 0);
            CHECK(startsWith(intent::describeSourceFailure(op, clean), "failed_predicate=time_unresolved "));
            CHECK(startsWith(label(c::newUuid(), op, s.couple, s.userA, s.userB), "failed_predicate=source_found "));
            CHECK(startsWith(label(id, "message_reply", s.couple, s.userA, s.userB), "failed_predicate=operation "));
            CHECK(startsWith(label(id, op, other.couple, s.userA, s.userB), "failed_predicate=couple "));
            CHECK(startsWith(label(id, op, s.couple, s.userB, s.userB), "failed_predicate=actor "));
            CHECK(startsWith(label(id, op, s.couple, s.userA, s.userA), "failed_predicate=recipient "));
            // A partnerless write's empty recipient is compared as NULL.
            CHECK(startsWith(label(id, op, s.couple, s.userA, std::string()), "failed_predicate=recipient "));
            // The two clock comparisons, which only timing can reach.
            intent::SourcePredicates future = clean;
            future.sourceAgeUs = -1;
            CHECK(startsWith(intent::describeSourceFailure(op, future), "failed_predicate=time_created_before_source "));
            intent::SourcePredicates backwards = clean;
            backwards.clockAfterProbeUs = -1;
            CHECK(startsWith(intent::describeSourceFailure(op, backwards), "failed_predicate=time_clock_regressed "));
        } catch (const std::exception &failure) {
            c::expectThat(false, "intent-diag-labels", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("Intent diagnostics: the real guard refuses the real write; the sink says why, with no id or text; the write still fails") {
        std::mutex lines;
        std::vector<std::string> seen;
        try {
            const auto s = md::space();
            const auto id = c::newUuid();
            const std::string body = "INTENT-DIAG-SENTINEL";
            base::asMaintenance("INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at) "
                                "VALUES ('" + id + "', '" + s.couple + "', '" + s.userA + "', '" + s.userB + "', '" + body + "', '" + c::newUuid() + "', repeat('a', 64), clock_timestamp());");
            usapp::intent::setSourceFailureSink([&lines, &seen](const std::string &line) {
                std::lock_guard<std::mutex> guard(lines);
                seen.push_back(line);
            });
            // The write reads its source, then waits for this lock. An explicit choice is what takes it.
            const std::string lockKey = s.userA + ":message_create";
            auto holder = c::acquireTx(usapp::db::Pool::instance().client());
            CHECK(c::exec(holder, "SELECT pg_advisory_xact_lock(33012::integer, hashtext($1)) AS l", lockKey).ok);
            auto outcome = std::make_shared<ts::Pending<std::string>>();
            usapp::db::TxScope::beginAsync(
                [outcome, id](std::shared_ptr<usapp::db::TxScope> scope) {
                    intent::Source source;
                    source.messageId = id;
                    intent::Choice explicitOff;
                    explicitOff.provided = true;
                    explicitOff.value = false;
                    intent::record(
                        scope, intent::Operation::MessageCreate, source, explicitOff,
                        [outcome, scope](Json::Value) { scope->rollback([outcome]() { outcome->deliver("written"); }); },
                        [outcome, scope]() { scope->rollback([outcome]() { outcome->deliver("failed"); }); });
                },
                [outcome](const std::string &) { outcome->deliver("failed"); });
            for (int tries = 0; tries < 500; ++tries) {
                if (c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33012 AND objid = hashtext($1)::oid AND NOT granted", lockKey) == 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM pg_locks WHERE locktype = 'advisory' AND classid = 33012 AND objid = hashtext($1)::oid AND NOT granted", lockKey), 1);
            base::asMaintenance("UPDATE messages SET author_id = '" + s.userB + "', recipient_id = '" + s.userA + "' WHERE id = '" + id + "';");
            CHECK(c::commitTx(holder).value_or(false));
            const auto result = outcome->wait();
            CHECK(result.has_value() && *result == "failed");
            // Back to the run's sink before anything can throw on the way out.
            usapp::intent::setSourceFailureSink([](const std::string &line) {
                std::printf("INTENT_SOURCE_DIAG %s\n", line.c_str());
                std::fflush(stdout);
            });
            std::lock_guard<std::mutex> guard(lines);
            CHECK_EQ(seen.size(), 1u);
            if (!seen.empty()) {
                const auto &line = seen[0];
                CHECK(startsWith(line, "failed_predicate=actor operation=message_create "));
                CHECK(line.find("source_found=true") != std::string::npos);
                CHECK(line.find("actor_match=false") != std::string::npos);
                CHECK(line.find("clock_after_probe_us=") != std::string::npos);
                CHECK(line.find("clock_after_probe=unread") == std::string::npos);
                for (const auto &secret : {id, s.couple, s.userA, s.userB, body}) CHECK(line.find(secret) == std::string::npos);
            }
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM notification_intents WHERE message_id = $1::uuid", id), 0);
        } catch (const std::exception &failure) {
            usapp::intent::setSourceFailureSink([](const std::string &line) {
                std::printf("INTENT_SOURCE_DIAG %s\n", line.c_str());
                std::fflush(stdout);
            });
            c::expectThat(false, "intent-diag-real", md::exceptionCategory(failure));
        }
    }

    TEST_CASE("0041: a source dated ahead no longer refuses the real write: the intent takes exactly the source's time, and the sink stays silent") {
        std::mutex lines;
        std::vector<std::string> seen;
        try {
            const auto s = md::space();
            const auto id = c::newUuid();
            base::asMaintenance("INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at) "
                                "VALUES ('" + id + "', '" + s.couple + "', '" + s.userA + "', '" + s.userB + "', 'p0041', '" + c::newUuid() + "', repeat('a', 64), clock_timestamp() + interval '1 hour');");
            usapp::intent::setSourceFailureSink([&lines, &seen](const std::string &line) {
                std::lock_guard<std::mutex> guard(lines);
                seen.push_back(line);
            });
            auto outcome = std::make_shared<ts::Pending<std::string>>();
            usapp::db::TxScope::beginAsync(
                [outcome, id](std::shared_ptr<usapp::db::TxScope> scope) {
                    intent::Source source;
                    source.messageId = id;
                    intent::record(
                        scope, intent::Operation::MessageCreate, source, choice::kOff,
                        [outcome, scope](Json::Value) { scope->commit([outcome]() { outcome->deliver("written"); }); },
                        [outcome, scope]() { scope->rollback([outcome]() { outcome->deliver("failed"); }); });
                },
                [outcome](const std::string &) { outcome->deliver("failed"); });
            const auto result = outcome->wait();
            usapp::intent::setSourceFailureSink([](const std::string &line) {
                std::printf("INTENT_SOURCE_DIAG %s\n", line.c_str());
                std::fflush(stdout);
            });
            CHECK(result.has_value() && *result == "written");
            std::lock_guard<std::mutex> guard(lines);
            CHECK(seen.empty());
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM notification_intents i JOIN messages m ON m.id = i.message_id WHERE i.message_id = $1::uuid AND i.created_at = m.created_at", id), 1);
        } catch (const std::exception &failure) {
            usapp::intent::setSourceFailureSink([](const std::string &line) {
                std::printf("INTENT_SOURCE_DIAG %s\n", line.c_str());
                std::fflush(stdout);
            });
            c::expectThat(false, "intent-diag-ahead", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::intentdiag
