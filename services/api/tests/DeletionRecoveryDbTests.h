#pragma once
// Harness-only, on a fresh disposable database AFTER the ordinary suites.
// The CLOSED marker deliberately survives this test process. Never register in parallel CTest.
#include "MemoryDeletionDbTests.h"
#include "DeletionRecoveryUnitTests.h"
#include "services/PostgresRecoveryStore.h"

namespace usapptest::deletionrecoverydb {
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
namespace r = usapp::deletionrecovery;
using usapp::deletionrecovery::Receipt;
template <typename F> Receipt receipt(F start) {
    auto p = std::make_shared<ts::Pending<Receipt>>();
    start([p](Receipt v) { p->deliver(v); });
    auto v = p->wait(); return v.value_or(Receipt::Unknown);
}
inline void run(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("J3b: committed quarantine, read and replay fences, immutable manifests, stale generations and persisted closure") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = md::uploaded(s.userA);
            CHECK(md::is(md::complete(s.userA, photo.id), md::MO::Ok));
            const auto key = c::newUuid();
            const auto terms = mm::terms("recovery sentinel", {photo.id});
            const auto made = mm::create(s.userA, terms, key);
            CHECK(mm::is(made, mm::SO::Created));
            const auto memory = mm::idOf(made);
            CHECK(mm::is(mm::getMemory(s.userB, memory), mm::SO::Ok));
            const auto proposal = mm::startPropose(s.userB, memory, 1, mm::terms("suggestion", {photo.id}))->wait();
            CHECK(proposal.has_value() && proposal->success());
            const auto proposalId = mm::proposalOf(proposal);
            auto apiStore = std::make_shared<r::PostgresStore>(pool);
            const r::Request request{c::newUuid(), c::newUuid(), c::newUuid()};
            auto denied = std::make_shared<ts::Pending<Receipt>>();
            apiStore->close(request, [denied](Receipt v, r::Fence) { denied->deliver(v); });
            CHECK(denied->wait().value_or(Receipt::Confirmed) == Receipt::Unknown);
            CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM deletion_recovery_attempts WHERE database_id=$1::uuid", request.databaseId), 0);

            const auto maintenance = usapptest::memorydeletion::maintClient();
            auto store = std::make_shared<r::PostgresStore>(maintenance);
            std::vector<std::string> trace;
            auto source = std::make_shared<usapptest::deletionrecoveryunit::Source>(trace);
            source->value = {{request.journalId, 4, std::string(64, 'a')},
                {{r::Kind::Memory, memory}, {r::Kind::Media, photo.id}}, {{r::Kind::Media, c::newUuid()}}, {c::newUuid()}};
            auto writer = pool->newTransaction();
            writer->execSqlSync("INSERT INTO memory_texts SELECT * FROM memory_texts WHERE false");
            const int pid = maintenance->execSqlSync("SELECT pg_backend_pid() AS pid")[0]["pid"].as<int>();
            auto result = std::make_shared<ts::Pending<r::Report>>();
            r::Coordinator::run(request, source, store, [result](r::Report v) { result->deliver(v); });
            md::waitUntil([&]() {
                return md::obs().client->execSqlSync("SELECT cardinality(pg_blocking_pids($1::integer)) > 0 AS blocked", pid)[0]["blocked"].as<bool>();
            });
            CHECK_EQ(source->snapshots, 0); // no external call before the writer is drained and close commits
            writer->rollback(); writer.reset();
            const auto prepared = result->wait();
            CHECK(prepared.has_value() && prepared->outcome == r::Outcome::VerifiedClosed);
            CHECK_FALSE(r::Report::servingAllowed);
            CHECK(md::obs().client->execSqlSync("SELECT usapp_recovery_closed() AS closed")[0]["closed"].as<bool>());
            CHECK(mm::is(mm::getMemory(s.userA, memory), mm::SO::NotFound));
            CHECK(mm::is(mm::getMemory(s.userB, memory), mm::SO::NotFound));
            CHECK(mm::is(mm::getProposal(s.userA, memory, proposalId), mm::SO::ProposalNotFound));
            const auto replay = mm::create(s.userA, terms, key);
            CHECK(replay.has_value() && !replay->success()); // even a legacy receipt lookup sees no private revision
            CHECK(replay && (replay->memory.isNull() || replay->memory.empty()));
            CHECK(md::is(md::content(s.userA, photo.id), md::MO::NotFound));
            CHECK(md::is(md::content(s.userB, photo.id), md::MO::NotFound));
            CHECK(mm::is(mm::edit(s.userA, memory, 1, terms), mm::SO::NotFound));
            CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM memory_revisions WHERE memory_id=$1::uuid", memory), 0);
            CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM media_objects WHERE id=$1::uuid", photo.id), 0);
            CHECK_EQ(maintenance->execSqlSync("SELECT count(*)::bigint AS n FROM memory_revisions WHERE memory_id=$1::uuid", memory)[0]["n"].as<long long>(), 1);
            const auto row = maintenance->execSqlSync("SELECT generation FROM deletion_recovery_attempts ORDER BY generation DESC LIMIT 1");
            const r::Fence fence{request.databaseId, request.attemptId, row[0]["generation"].as<uint64_t>()};
            CHECK(receipt([&](auto cb) { store->verify(fence, source->value, cb); }) == Receipt::Confirmed);
            CHECK(receipt([&](auto cb) { store->stage(fence, source->value, cb); }) == Receipt::Confirmed);
            auto changed = source->value; changed.unresolved.push_back(c::newUuid());
            CHECK(receipt([&](auto cb) { store->stage(fence, changed, cb); }) == Receipt::Conflict);
            CHECK(receipt([&](auto cb) { store->verify(fence, changed, cb); }) == Receipt::Conflict);
            auto reopened = std::make_shared<r::PostgresStore>(maintenance);
            CHECK(receipt([&](auto cb) { reopened->verify(fence, source->value, cb); }) == Receipt::Confirmed);
            auto newer = std::make_shared<ts::Pending<r::Fence>>();
            reopened->close({request.databaseId,c::newUuid(),request.journalId}, [newer](Receipt v, r::Fence f) {
                if (v == Receipt::Confirmed) newer->deliver(f); else newer->deliver({});
            });
            const auto newFence = newer->wait();
            CHECK(newFence.has_value() && newFence->generation > fence.generation);
            CHECK(receipt([&](auto cb) { store->stage(fence, source->value, cb); }) == Receipt::Conflict);
            CHECK(receipt([&](auto cb) { store->recordVerified(fence, source->value.head, cb); }) == Receipt::Conflict);
            CHECK(newFence && receipt([&](auto cb) { reopened->stage(*newFence, source->value, cb); }) == Receipt::Confirmed);
            CHECK(newFence && receipt([&](auto cb) { reopened->recordVerified(*newFence, source->value.head, cb); }) == Receipt::Confirmed);
            CHECK_EQ(md::count("SELECT count(*)::bigint AS n FROM deletion_recovery_quarantine WHERE target_id=$1::uuid", memory), 2);
            CHECK(md::obs().client->execSqlSync("SELECT usapp_recovery_closed() AS closed")[0]["closed"].as<bool>());
            bool refused = false;
            try { pool->execSqlSync("INSERT INTO memory_texts SELECT * FROM memory_texts WHERE false"); }
            // Drogon's batch path surfaces custom SQLSTATEs as Failure, not SqlError. The SQL ledger checks UJ002;
            // this service-side assertion still requires the exact guard marker, not just any database error.
            catch (const drogon::orm::Failure &e) { refused = std::string(e.what()).find("USAPP_RECOVERY_CLOSED") != std::string::npos; }
            CHECK(refused);
        } catch (const std::exception &e) {
            c::expectThat(false, "recovery-db", md::exceptionCategory(e));
        }
    }
}
} // namespace usapptest::deletionrecoverydb
