#pragma once

// D6 M1b: delete requests through the real MemoryService against FakeObjectStore, on literal usapp_test. Runs inside the memory deletion target,
// after that suite has prepared its environment and its holder connection.
#include <json/json.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "MemoryDeletionDbTests.h"

namespace usapptest::memorydeleterequest {

namespace base = usapptest::memorydeletion;
namespace md = usapptest::media;
namespace mm = usapptest::memory;
namespace del = usapptest::mediadeletion;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::MemoryService;
using SO = S::Outcome;
using Decision = S::Decision;
using PendingPtr = std::shared_ptr<mm::PendingS>;

inline PendingPtr startRequest(const std::string &user, const std::string &id, const std::string &key = c::newUuid()) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().requestDeletion(user, id, key, [p](S::Status s) { p->deliver(std::move(s)); });
    return p;
}
inline std::optional<S::Status> request(const std::string &user, const std::string &id, const std::string &key = c::newUuid()) { return startRequest(user, id, key)->wait(); }
inline PendingPtr startDecide(const std::string &user, const std::string &id, const std::string &requestId, Decision d, const std::string &key = c::newUuid(), S::Fault fault = S::Fault::None) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().decideDeleteRequest(user, id, requestId, key, d, [p](S::Status s) { p->deliver(std::move(s)); }, fault);
    return p;
}
inline std::optional<S::Status> decide(const std::string &user, const std::string &id, const std::string &requestId, Decision d, const std::string &key = c::newUuid(), S::Fault fault = S::Fault::None) {
    return startDecide(user, id, requestId, d, key, fault)->wait();
}
inline std::optional<S::Status> read(const std::string &user, const std::string &id, const std::string &requestId) {
    auto p = std::make_shared<mm::PendingS>();
    S::instance().getDeleteRequest(user, id, requestId, [p](S::Status s) { p->deliver(std::move(s)); });
    return p->wait();
}
inline std::string idOf(const std::optional<S::Status> &s) { return s ? s->request["request_id"].asString() : std::string(); }
inline std::string statusOf(const std::optional<S::Status> &s) { return s ? s->request["status"].asString() : std::string(); }
inline long long requests(const std::string &memory) { return c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM memory_delete_requests WHERE memory_id = $1::uuid", memory); }
inline long long decisions(const std::string &memory) { return c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM memory_delete_decisions WHERE memory_id = $1::uuid", memory); }
// Both started while a holder keeps the memory lock, the first queued before the second, then released: they run in that order.
inline std::pair<std::optional<S::Status>, std::optional<S::Status>> raced(const std::string &id, const std::function<PendingPtr()> &first, const std::function<PendingPtr()> &second) {
    auto hold = c::acquireTx(base::holderConn->client);
    CHECK(c::exec(hold, mm::kLockMemory, id).ok);
    auto one = first();
    base::waitMemoryLockWaiters(1);
    auto two = second();
    base::waitMemoryLockWaiters(2);
    CHECK(c::commitTx(hold).value_or(false));
    auto a = one->wait();
    auto b = two->wait();
    return {std::move(a), std::move(b)};
}

inline void runMemoryDeleteRequestDbTests(const c::ClientPtr &pool) {
    const std::string kTitle = "M1B-SENTINEL-TITLE";
    const std::string kDescription = "M1B-SENTINEL-DESCRIPTION";
    // A memory by userB, with one photo of theirs.
    const auto fresh = [&](const ts::CommittedSpace &s, const std::string &title = "") {
        return mm::idOf(mm::create(s.userB, mm::terms(title.empty() ? kTitle : title, {del::readyPhoto(s.userB)}, "2026-03-01", kDescription)));
    };
    const auto noText = [&](const std::optional<S::Status> &status) {
        if (!status) return false;
        const auto raw = base::written(S::responseBody(*status));
        return raw.find(kTitle) == std::string::npos && raw.find(kDescription) == std::string::npos && raw.find("photos") == std::string::npos;
    };

    TEST_CASE("D6 M1b: the partner asks and only the partner; the answer holds only request facts; the creator can find it") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto id = fresh(s);
            CHECK(mm::is(request(s.userB, id), SO::Forbidden));
            CHECK(mm::is(request(other.userA, id), SO::NotFound));
            CHECK(mm::is(request(s.userA, c::newUuid()), SO::NotFound));
            CHECK_EQ(requests(id), 0);

            const auto key = c::newUuid();
            const auto made = request(s.userA, id, key);
            CHECK(mm::is(made, SO::Created));
            if (!made) return;
            CHECK_EQ(made->request.size(), 7u);
            for (const char *field : {"request_id", "memory_id", "requester_id", "created_at", "status", "decided_by", "decided_at"}) CHECK(made->request.isMember(field));
            CHECK_EQ(statusOf(made), std::string("pending"));
            CHECK(made->request["decided_by"].isNull());
            CHECK(noText(made));
            CHECK_EQ(S::responseBody(*made).size(), 1u);
            CHECK_EQ(base::audits(id, "memory.delete_requested"), 1);
            const auto requestId = idOf(made);

            // The memory is unchanged, and both members see the request on it.
            for (const auto &user : {s.userA, s.userB}) {
                const auto got = mm::getMemory(user, id);
                CHECK(mm::is(got, SO::Ok));
                if (got) {
                    CHECK_EQ(got->memory["pending_delete_request"]["id"].asString(), requestId);
                    CHECK_EQ(got->memory["version"].asInt(), 1);
                }
                const auto one = read(user, id, requestId);
                CHECK(mm::is(one, SO::Ok));
                CHECK_EQ(statusOf(one), std::string("pending"));
                CHECK(noText(one));
            }
            CHECK(mm::is(read(other.userA, id, requestId), SO::ProposalNotFound));

            // Same key: the same request, and no second one. Another target: refused, before that target's state.
            const auto replay = request(s.userA, id, key);
            CHECK(mm::is(replay, SO::Replayed));
            CHECK_EQ(idOf(replay), requestId);
            CHECK(noText(replay));
            CHECK_EQ(requests(id), 1);
            const auto second = fresh(s, "second");
            CHECK(mm::is(request(s.userA, second, key), SO::KeyReused));
            CHECK_EQ(requests(second), 0);
            CHECK(mm::is(base::erase(s.userB, second), SO::Created));
            CHECK(mm::is(request(s.userA, second, key), SO::KeyReused));
            // A deleted memory is absent to a new request.
            CHECK(mm::is(request(s.userA, second), SO::NotFound));
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-request", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: approve and reject belong to the creator, withdraw to the requester, across spaces nobody") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto id = fresh(s);
            const auto spare = fresh(s, "spare");
            const auto requestId = idOf(request(s.userA, id));
            CHECK(!requestId.empty());
            CHECK(mm::is(decide(s.userA, id, requestId, Decision::Approve), SO::Forbidden));
            CHECK(mm::is(decide(s.userA, id, requestId, Decision::Reject), SO::Forbidden));
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Withdraw), SO::Forbidden));
            CHECK(mm::is(decide(other.userA, id, requestId, Decision::Approve), SO::NotFound));
            CHECK(mm::is(decide(s.userB, spare, requestId, Decision::Approve), SO::ProposalNotFound));
            CHECK(mm::is(decide(s.userB, id, c::newUuid(), Decision::Approve), SO::ProposalNotFound));
            CHECK_EQ(decisions(id), 0);
            CHECK_EQ(base::cycles(id), 0);

            // Reject: nothing changes about the memory, the request is final, and the partner can ask again.
            const auto rejectKey = c::newUuid();
            const auto rejected = decide(s.userB, id, requestId, Decision::Reject, rejectKey);
            CHECK(mm::is(rejected, SO::Ok));
            CHECK_EQ(statusOf(rejected), std::string("rejected"));
            CHECK(noText(rejected));
            if (rejected) CHECK(!S::responseBody(*rejected).isMember("deletion"));
            CHECK_EQ(base::cycles(id), 0);
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Approve), SO::ProposalDecided));
            CHECK(mm::is(decide(s.userA, id, requestId, Decision::Withdraw), SO::ProposalDecided));
            const auto rejectReplay = decide(s.userB, id, requestId, Decision::Reject, rejectKey);
            CHECK(mm::is(rejectReplay, SO::Replayed));
            CHECK_EQ(statusOf(rejectReplay), std::string("rejected"));
            // The same key with another outcome is another request.
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Approve, rejectKey), SO::KeyReused));

            // Withdraw, the same way.
            const auto again = idOf(request(s.userA, id));
            CHECK(!again.empty() && again != requestId);
            const auto withdrawn = decide(s.userA, id, again, Decision::Withdraw);
            CHECK(mm::is(withdrawn, SO::Ok));
            CHECK_EQ(statusOf(withdrawn), std::string("withdrawn"));
            CHECK(mm::is(request(s.userA, id), SO::Created));
            CHECK_EQ(base::audits(id, "memory.delete_request_rejected"), 1);
            CHECK_EQ(base::audits(id, "memory.delete_request_withdrawn"), 1);
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-decide", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: an edit proposal and a delete request exclude each other, and either blocks a direct edit") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto photo = del::readyPhoto(s.userB);
            const auto id = mm::idOf(mm::create(s.userB, mm::terms("Exclusive", {photo})));
            const auto requestId = idOf(request(s.userA, id));
            CHECK(!requestId.empty());
            CHECK(mm::is(request(s.userA, id), SO::ProposalPending));
            CHECK(mm::is(mm::propose(s.userA, id, 1, mm::terms("Asked", {photo})), SO::ProposalPending));
            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms("Edited", {photo})), SO::ProposalPending));
            CHECK_EQ(base::revisions(id), 1);
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Reject), SO::Ok));

            // Free again, and now the other way around.
            const auto proposal = mm::proposalOf(mm::propose(s.userA, id, 1, mm::terms("Asked", {photo})));
            CHECK(!proposal.empty());
            CHECK(mm::is(request(s.userA, id), SO::ProposalPending));
            CHECK_EQ(requests(id), 1);
            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms("Edited", {photo})), SO::ProposalPending));
            CHECK(mm::is(mm::decide(s.userB, id, proposal, Decision::Reject), SO::Ok));
            CHECK(mm::is(mm::edit(s.userB, id, 1, mm::terms("Edited", {photo})), SO::Created));
            CHECK(mm::is(request(s.userA, id), SO::Created));
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-exclusion", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: approval commits its decision and cycle together; a failed one leaves neither; replays state facts only") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto other = md::space();
            const auto id = fresh(s);
            const auto requestKey = c::newUuid();
            const auto requestId = idOf(request(s.userA, id, requestKey));
            CHECK(!requestId.empty());

            // Fails after both rows are written: neither stays, and the audit neither.
            const auto key = c::newUuid();
            const auto failed = decide(s.userB, id, requestId, Decision::Approve, key, S::Fault::FailDeleteApproval);
            CHECK(mm::is(failed, SO::Error));
            CHECK_EQ(decisions(id), 0);
            CHECK_EQ(base::cycles(id), 0);
            CHECK_EQ(base::audits(id, "memory.delete_request_approved"), 0);
            CHECK_EQ(base::audits(id, "memory.deleted"), 0);
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
            CHECK_EQ(statusOf(read(s.userA, id, requestId)), std::string("pending"));

            // The retry with the same key is the first success.
            const auto approved = decide(s.userB, id, requestId, Decision::Approve, key);
            CHECK(mm::is(approved, SO::Ok));
            if (!approved) return;
            CHECK_EQ(statusOf(approved), std::string("approved"));
            CHECK_EQ(approved->request["decided_by"].asString(), s.userB);
            CHECK_EQ(approved->deletion["state"].asString(), std::string("deleted"));
            CHECK_EQ(approved->deletion["cycle_no"].asInt(), 1);
            CHECK(noText(approved));
            const auto cycle = base::cycleOf(approved);
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM memory_delete_decisions WHERE request_id = $1::uuid AND deletion_id = $2::uuid AND outcome = 'approved'", requestId, cycle), 1);
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM memory_deletions WHERE id = $1::uuid AND deleted_by = $2::uuid", cycle, s.userB), 1);
            CHECK_EQ(base::audits(id, "memory.delete_request_approved"), 1);
            CHECK_EQ(base::audits(id, "memory.deleted"), 1);
            CHECK(base::lists(s.userB, cycle));
            CHECK(base::recoveryIds(s.userA).empty());

            // M1a holds: it is hidden from both, on every read.
            for (const auto &user : {s.userA, s.userB}) {
                CHECK(mm::is(mm::getMemory(user, id), SO::NotFound));
                CHECK(!base::memoryListed(user, id));
                CHECK(mm::is(read(user, id, requestId), SO::ProposalNotFound));
            }
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Reject), SO::NotFound));
            CHECK(mm::is(decide(s.userA, id, requestId, Decision::Withdraw), SO::NotFound));
            CHECK(mm::is(request(s.userA, id), SO::NotFound));
            CHECK(mm::is(decide(other.userA, id, requestId, Decision::Approve), SO::NotFound));

            // Replays while hidden: each actor's own facts. Only the approver's holds the cycle, and neither holds the memory.
            const auto approverReplay = decide(s.userB, id, requestId, Decision::Approve, key);
            CHECK(mm::is(approverReplay, SO::Replayed));
            CHECK_EQ(base::cycleOf(approverReplay), cycle);
            CHECK(noText(approverReplay));
            const auto requesterReplay = request(s.userA, id, requestKey);
            CHECK(mm::is(requesterReplay, SO::Replayed));
            CHECK_EQ(statusOf(requesterReplay), std::string("approved"));
            CHECK(noText(requesterReplay));
            if (requesterReplay) {
                CHECK(!S::responseBody(*requesterReplay).isMember("deletion"));
                CHECK(base::written(S::responseBody(*requesterReplay)).find(cycle) == std::string::npos);
            }
            CHECK_EQ(base::cycles(id), 1);

            // The cycle is an M1a cycle: the partner cannot undo it; the creator can, exactly.
            CHECK(mm::is(base::undo(s.userA, id, cycle), SO::NotFound));
            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::Ok));
            CHECK(mm::is(mm::getMemory(s.userA, id), SO::Ok));
            // The undo revives nothing: the request stays approved, with no pending one left.
            const auto after = read(s.userA, id, requestId);
            CHECK_EQ(statusOf(after), std::string("approved"));
            const auto current = mm::getMemory(s.userB, id);
            if (current) CHECK(current->memory["pending_delete_request"].isNull());
            // Replays say "approved" and "undone" of a visible memory: a fact of that cycle, not a current state.
            const auto later = decide(s.userB, id, requestId, Decision::Approve, key);
            CHECK(mm::is(later, SO::Replayed));
            if (later) CHECK_EQ(later->deletion["state"].asString(), std::string("undone"));
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-approval", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: a direct deletion settles a pending request as approved, naming its own cycle; keys stay in one namespace") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = fresh(s);
            const auto requestId = idOf(request(s.userA, id));
            const auto deleteKey = c::newUuid();
            const auto deleted = base::erase(s.userB, id, deleteKey);
            CHECK(mm::is(deleted, SO::Created));
            const auto cycle = base::cycleOf(deleted);
            CHECK_EQ(base::obs().client->execSqlSync("SELECT count(*)::bigint AS n FROM memory_delete_decisions WHERE request_id = $1::uuid AND deletion_id = $2::uuid AND outcome = 'approved' AND decided_by = $3::uuid", requestId, cycle, s.userB)[0]["n"].as<long long>(), 1);
            CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'memory.delete_request_approved' AND target_id = $1::text AND detail->>'by_deletion' = 'true'", id), 1);
            CHECK(mm::is(decide(s.userB, id, requestId, Decision::Approve), SO::NotFound));
            const auto replay = base::erase(s.userB, id, deleteKey);
            CHECK(mm::is(replay, SO::Replayed));
            CHECK_EQ(base::cycleOf(replay), cycle);

            CHECK(mm::is(base::undo(s.userB, id, cycle), SO::Ok));
            CHECK_EQ(statusOf(read(s.userA, id, requestId)), std::string("approved"));

            // A key spent on a direct deletion cannot then approve: the whole decision rolls back.
            const auto next = idOf(request(s.userA, id));
            CHECK(mm::is(decide(s.userB, id, next, Decision::Approve, deleteKey), SO::KeyReused));
            CHECK_EQ(statusOf(read(s.userB, id, next)), std::string("pending"));
            CHECK_EQ(base::cycles(id), 1);
            // A key spent on an approval cannot then delete directly.
            const auto approveKey = c::newUuid();
            CHECK(mm::is(decide(s.userB, id, next, Decision::Approve, approveKey), SO::Ok));
            const auto spare = fresh(s, "spare");
            CHECK(mm::is(base::erase(s.userB, spare, approveKey), SO::KeyReused));
            CHECK_EQ(base::cycles(spare), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-direct", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: competing decisions and writes serialize on the memory lock, in both orders") {
        try {
            md::useStore();
            const auto s = md::space();
            {   // Approve, then withdraw: the approval stands, the memory is gone.
                const auto id = fresh(s, "approve withdraw");
                const auto q = idOf(request(s.userA, id));
                auto [a, b] = raced(id, [&]() { return startDecide(s.userB, id, q, Decision::Approve); }, [&]() { return startDecide(s.userA, id, q, Decision::Withdraw); });
                CHECK(mm::is(a, SO::Ok));
                CHECK(mm::is(b, SO::NotFound));
                CHECK_EQ(decisions(id), 1);
                CHECK_EQ(base::cycles(id), 1);
            }
            {   // Withdraw, then approve: no cycle.
                const auto id = fresh(s, "withdraw approve");
                const auto q = idOf(request(s.userA, id));
                auto [a, b] = raced(id, [&]() { return startDecide(s.userA, id, q, Decision::Withdraw); }, [&]() { return startDecide(s.userB, id, q, Decision::Approve); });
                CHECK(mm::is(a, SO::Ok));
                CHECK(mm::is(b, SO::ProposalDecided));
                CHECK_EQ(base::cycles(id), 0);
                CHECK(mm::is(mm::getMemory(s.userB, id), SO::Ok));
            }
            {   // Approve and reject: the first decides.
                const auto id = fresh(s, "reject approve");
                const auto q = idOf(request(s.userA, id));
                auto [a, b] = raced(id, [&]() { return startDecide(s.userB, id, q, Decision::Reject); }, [&]() { return startDecide(s.userB, id, q, Decision::Approve); });
                CHECK(mm::is(a, SO::Ok));
                CHECK(mm::is(b, SO::ProposalDecided));
                CHECK_EQ(base::cycles(id), 0);
            }
            {   // Direct deletion, then the request: the request finds it gone.
                const auto id = fresh(s, "delete request");
                auto [a, b] = raced(id, [&]() { return base::startErase(s.userB, id); }, [&]() { return startRequest(s.userA, id); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::NotFound));
                CHECK_EQ(requests(id), 0);
            }
            {   // The request, then direct deletion: it is settled by that deletion.
                const auto id = fresh(s, "request delete");
                auto [a, b] = raced(id, [&]() { return startRequest(s.userA, id); }, [&]() { return base::startErase(s.userB, id); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::Created));
                CHECK_EQ(decisions(id), 1);
                CHECK_EQ(c::scalar(base::obs(), "SELECT count(*)::bigint AS n FROM memory_delete_decisions d JOIN memory_deletions x ON x.id = d.deletion_id WHERE d.memory_id = $1::uuid AND d.outcome = 'approved'", id), 1);
            }
            {   // Direct deletion, then approval of the request it settled.
                const auto id = fresh(s, "delete approve");
                const auto q = idOf(request(s.userA, id));
                auto [a, b] = raced(id, [&]() { return base::startErase(s.userB, id); }, [&]() { return startDecide(s.userB, id, q, Decision::Approve); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::NotFound));
                CHECK_EQ(decisions(id), 1);
                CHECK_EQ(base::cycles(id), 1);
            }
            {   // Approval, then direct deletion: one cycle.
                const auto id = fresh(s, "approve delete");
                const auto q = idOf(request(s.userA, id));
                auto [a, b] = raced(id, [&]() { return startDecide(s.userB, id, q, Decision::Approve); }, [&]() { return base::startErase(s.userB, id); });
                CHECK(mm::is(a, SO::Ok));
                CHECK(mm::is(b, SO::AlreadyDeleted));
                CHECK_EQ(base::cycles(id), 1);
            }
            {   // The request and an edit proposal: one pending.
                const auto photo = del::readyPhoto(s.userB);
                const auto id = mm::idOf(mm::create(s.userB, mm::terms("request propose", {photo})));
                auto [a, b] = raced(id, [&]() { return startRequest(s.userA, id); }, [&]() { return mm::startPropose(s.userA, id, 1, mm::terms("asked", {photo})); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::ProposalPending));
            }
            {   // The edit proposal first.
                const auto photo = del::readyPhoto(s.userB);
                const auto id = mm::idOf(mm::create(s.userB, mm::terms("propose request", {photo})));
                auto [a, b] = raced(id, [&]() { return mm::startPropose(s.userA, id, 1, mm::terms("asked", {photo})); }, [&]() { return startRequest(s.userA, id); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::ProposalPending));
                CHECK_EQ(requests(id), 0);
            }
            {   // The request, then a direct edit: refused.
                const auto photo = del::readyPhoto(s.userB);
                const auto id = mm::idOf(mm::create(s.userB, mm::terms("request edit", {photo})));
                auto [a, b] = raced(id, [&]() { return startRequest(s.userA, id); }, [&]() { return mm::startEdit(s.userB, id, 1, mm::terms("late", {photo})); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::ProposalPending));
                CHECK_EQ(base::revisions(id), 1);
            }
            {   // A direct edit, then the request: both stand.
                const auto photo = del::readyPhoto(s.userB);
                const auto id = mm::idOf(mm::create(s.userB, mm::terms("edit request", {photo})));
                auto [a, b] = raced(id, [&]() { return mm::startEdit(s.userB, id, 1, mm::terms("in time", {photo})); }, [&]() { return startRequest(s.userA, id); });
                CHECK(mm::is(a, SO::Created));
                CHECK(mm::is(b, SO::Created));
                CHECK_EQ(base::revisions(id), 2);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-races", "case failed (details withheld)");
        }
    }

    TEST_CASE("D6 M1b: repeated cycles stay separate; an approval can only name a cycle of its own transaction") {
        try {
            md::useStore();
            const auto s = md::space();
            const auto id = fresh(s);
            const auto first = idOf(request(s.userA, id));
            const auto firstKey = c::newUuid();
            const auto approvedFirst = decide(s.userB, id, first, Decision::Approve, firstKey);
            const auto firstCycle = base::cycleOf(approvedFirst);
            CHECK(mm::is(base::undo(s.userB, id, firstCycle), SO::Ok));

            const auto second = idOf(request(s.userA, id));
            CHECK(!second.empty() && second != first);
            const auto approvedSecond = decide(s.userB, id, second, Decision::Approve);
            const auto secondCycle = base::cycleOf(approvedSecond);
            CHECK(mm::is(approvedSecond, SO::Ok));
            if (approvedSecond) CHECK_EQ(approvedSecond->deletion["cycle_no"].asInt(), 2);
            CHECK(firstCycle != secondCycle);

            // The old cycle cannot undo the new one, and its replay says "undone" while the memory is hidden.
            CHECK(mm::is(base::undo(s.userB, id, firstCycle), SO::DeletionNotActive));
            const auto oldReplay = decide(s.userB, id, first, Decision::Approve, firstKey);
            CHECK(mm::is(oldReplay, SO::Replayed));
            if (oldReplay) CHECK_EQ(oldReplay->deletion["state"].asString(), std::string("undone"));
            CHECK(mm::is(mm::getMemory(s.userB, id), SO::NotFound));
            CHECK(mm::is(base::undo(s.userB, id, secondCycle), SO::Ok));
            CHECK_EQ(base::cycles(id), 2);
            CHECK_EQ(base::reversals(id), 2);
            CHECK_EQ(decisions(id), 2);

            // Direct SQL, in explicit transactions: a pipelined client does not report an error raised at an implicit commit, so the refusal is read
            // at SET CONSTRAINTS, and again at a real commit, whose false is confirmed by the row that did not appear.
            const auto spare = fresh(s, "spare");
            const auto oldCycle = base::cycleOf(base::erase(s.userB, spare));
            CHECK(mm::is(base::undo(s.userB, spare, oldCycle), SO::Ok));
            const auto pending = idOf(request(s.userA, spare));
            const auto approveNaming = [&](const c::TxPtr &tx, const std::string &cycle) {
                return c::exec(tx,
                    "INSERT INTO memory_delete_decisions (request_id, memory_id, couple_id, decided_by, outcome, deletion_id, idempotency_key, request_fingerprint) "
                    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, 'approved', $5::uuid, $6, repeat('a', 64))",
                    pending, spare, s.couple, s.userB, cycle, c::newUuid());
            };
            for (const auto &[cycle, sqlState] : {std::make_pair(oldCycle, std::string("UE005")), std::make_pair(c::newUuid(), std::string("23503"))}) {
                // A cycle from an earlier direct deletion, then one that does not exist.
                auto tx = c::acquireTx(pool);
                CHECK(approveNaming(tx, cycle).ok);
                const auto immediate = c::exec(tx, usapptest::wallet::expectFailureSql("SET CONSTRAINTS ALL IMMEDIATE"));
                CHECK(immediate.completed && !immediate.ok);
                CHECK_EQ(usapptest::wallet::parseFailure(immediate.error).sqlState, sqlState);
                c::rollbackTx(tx);
                auto real = c::acquireTx(pool);
                CHECK(approveNaming(real, cycle).ok);
                const auto committed = c::commitTx(real);
                CHECK(committed.has_value() && !*committed);
                CHECK_EQ(statusOf(read(s.userA, spare, pending)), std::string("pending"));
                CHECK_EQ(decisions(spare), 0);
            }
        } catch (const std::exception &) {
            c::expectThat(false, "m1b-cycles", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::memorydeleterequest
