// =====================================================================
// services/api/src/scheduler/jobs/MemoryPurgeJob.cc
// =====================================================================
#include "scheduler/jobs/MemoryPurgeJob.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <mutex>
#include <set>
#include <vector>
#include "core/Crypto.h"
#include "db/Db.h"

namespace usapp::scheduler {

struct MemoryPurgeJournalState {
    explicit MemoryPurgeJournalState(bool on) : enabled(on) {}
    std::atomic<bool> enabled;
    std::atomic<bool> busy{false};
    std::mutex mutex;
    std::shared_ptr<deletionjournal::Journal> journal;
    std::string reconcileAfter;
    MemoryPurgeJob::BackgroundReport report;
};

namespace {

// The one definition of what is due, shared with database/memory_purge_enablement_check.sql. A memory whose text an unerased memory uses is refused by the
// function, so the batch does not offer it: one refused row would fail the transaction and hold back every other due memory for good.
// Sorted, so that the 33020 locks are always taken in one order. One statement, so the batch is one step of this transaction.
const char *kEraseDueSql =
    "SELECT count(*) FILTER (WHERE o = 'erased')::integer AS erased FROM ("
    "SELECT usapp_memory_erase(c.deletion_id) AS o FROM (SELECT d.deletion_id, d.memory_id FROM usapp_memory_purge_due() d "
    "WHERE NOT d.blocked_shared_text ORDER BY d.purge_after, d.deletion_id LIMIT $1::integer) c ORDER BY hashtext(c.memory_id::text)) e";
const char *kBlockedSql = "SELECT count(*)::integer AS blocked FROM usapp_memory_purge_due() d WHERE d.blocked_shared_text";

const char *kJournalDueSql =
    "SELECT d.memory_id::text AS memory_id, d.deletion_id::text AS deletion_id FROM ("
    "SELECT * FROM usapp_memory_purge_due() WHERE NOT blocked_shared_text ORDER BY purge_after, deletion_id LIMIT $1::integer) d "
    "ORDER BY hashtext(d.memory_id::text)";
const char *kReconcileSql =
    "SELECT memory_id::text AS memory_id, deletion_id::text AS deletion_id FROM memory_purges "
    "WHERE $1::text = '' OR deletion_id > NULLIF($1::text, '')::uuid ORDER BY deletion_id LIMIT $2::integer";

namespace dj = deletionjournal;
using Work = std::pair<std::string, std::string>;  // memory, exact deletion cycle
std::string purgeUuid(const std::string &cycle, const char *label) {
    auto h = crypto::sha256Hex(std::string("usapp.memory-text-purge-journal.v1:") + label + ":" + cycle);
    h[12] = '8'; h[16] = 'a';
    return h.substr(0,8) + "-" + h.substr(8,4) + "-" + h.substr(12,4) + "-" + h.substr(16,4) + "-" + h.substr(20,12);
}

struct EraseEffect : dj::Effect {
    Work work;
    std::shared_ptr<MemoryPurgeJournalState> state;
    EraseEffect(Work item, std::shared_ptr<MemoryPurgeJournalState> s) : work(std::move(item)), state(std::move(s)) {}
    void applyOrClassify(const dj::Operation &, std::function<void(dj::EffectState)> done) override {
        if (!state->enabled) { done(dj::EffectState::Unknown); return; }
        auto answer = std::make_shared<std::function<void(dj::EffectState)>>(std::move(done));
        db::TxScope::beginAsync(
            [item = work, state = state, answer](std::shared_ptr<db::TxScope> scope) {
                scope->tx().execSqlAsync("SELECT pg_advisory_xact_lock(33020::integer, hashtext($1::text))",
                    [item, state, scope, answer](const drogon::orm::Result &) {
                        if (!state->enabled) { scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return; }
                        scope->tx().execSqlAsync("SELECT 1 FROM memory_deletions WHERE id=$1::uuid AND memory_id=$2::uuid",
                            [item, state, scope, answer](const drogon::orm::Result &rows) {
                                if (rows.size() != 1u) { scope->commit([answer]() { (*answer)(dj::EffectState::Unknown); }); return; }
                                scope->tx().execSqlAsync("SELECT usapp_memory_erase($1::uuid) AS outcome",
                                    [state, scope, answer](const drogon::orm::Result &erased) {
                                        try {
                                            const auto outcome = erased.at(0)["outcome"].as<std::string>();
                                            if (outcome != "erased" && outcome != "already") { scope->fail("memory purge outcome"); return; }
                                            if (!state->enabled) { scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return; }
                                            scope->commit([answer]() { (*answer)(dj::EffectState::Committed); });
                                        } catch (const std::exception &) { scope->fail("memory purge outcome"); }
                                    }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory purge erase"); }, item.second);
                            }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory purge identity"); }, item.second, item.first);
                    }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory purge lock"); }, item.first);
            }, [answer](const std::string &) { (*answer)(dj::EffectState::Unknown); });
    }
};

struct JournalRound : std::enable_shared_from_this<JournalRound> {
    std::shared_ptr<MemoryPurgeJournalState> state;
    std::shared_ptr<dj::Journal> journal;
    std::vector<Work> work;
    MemoryPurgeJob::BackgroundReport report;
    size_t next = 0;
    void finish() {
        { std::lock_guard<std::mutex> lock(state->mutex); state->report = report; }
        state->busy = false;
    }
    void runNext() {
        if (!state->enabled) { report.cancelled += static_cast<int>(work.size() - next); finish(); return; }
        if (next == work.size()) { finish(); return; }
        const auto item = work[next++];
        dj::Operation op;
        op.id = purgeUuid(item.second, "operation"); op.action = dj::Action::Purge;
        op.requestFingerprint = crypto::sha256Hex("memory-text-purge:v1:" + item.first + ":" + item.second);
        op.subjects = {{dj::Kind::Memory, item.first, item.second, purgeUuid(item.second, "intent")}};
        auto self = shared_from_this();
        dj::Coordinator::run(std::move(op), journal, std::make_shared<EraseEffect>(item, state), [self, item](dj::Result result) {
            if (result != dj::Result::Committed) { self->report.unknown++; self->runNext(); return; }
            // A journal terminal alone cannot prove a restored database contains
            // the corresponding erase. Never rerun an effect on that basis.
            db::Pool::instance().client()->execSqlAsync(
                "SELECT 1 FROM memory_purges WHERE deletion_id=$1::uuid AND memory_id=$2::uuid",
                [self](const drogon::orm::Result &rows) {
                    if (rows.size() == 1u) self->report.confirmed++; else self->report.unknown++;
                    self->runNext();
                }, [self](const drogon::orm::DrogonDbException &) { self->report.unknown++; self->runNext(); }, item.second, item.first);
        });
    }
    void loadReconcile(std::string after, bool wrapped = false) {
        auto self = shared_from_this();
        db::Pool::instance().client()->execSqlAsync(kReconcileSql,
            [self, after, wrapped](const drogon::orm::Result &rows) {
                if (rows.empty() && !after.empty() && !wrapped) { self->loadReconcile("", true); return; }
                try {
                    std::set<std::string> selected;
                    for (const auto &item : self->work) selected.insert(item.second);
                    std::string last;
                    for (const auto &row : rows) {
                        const auto cycle = row["deletion_id"].as<std::string>(); last = cycle;
                        if (selected.insert(cycle).second) self->work.emplace_back(row["memory_id"].as<std::string>(), cycle);
                    }
                    { std::lock_guard<std::mutex> lock(self->state->mutex); self->state->reconcileAfter = last; }
                    self->runNext();
                } catch (const std::exception &) { self->report.unknown++; self->finish(); }
            }, [self](const drogon::orm::DrogonDbException &) { self->report.unknown++; self->finish(); }, after, std::to_string(MemoryPurgeJob::kBatch));
    }
};

void startJournalRound(const std::shared_ptr<MemoryPurgeJournalState> &state,
                       const std::shared_ptr<dj::Journal> &journal, std::vector<Work> work) {
    if (!state->enabled) return;
    bool idle = false;
    if (!state->busy.compare_exchange_strong(idle, true)) return;
    auto round = std::make_shared<JournalRound>();
    round->state = state; round->journal = journal; round->work = std::move(work);
    std::string after;
    { std::lock_guard<std::mutex> lock(state->mutex); after = state->reconcileAfter; }
    round->loadReconcile(std::move(after));
}

}  // namespace

MemoryPurgeJob::MemoryPurgeJob(bool enabled) : journalState_(std::make_shared<MemoryPurgeJournalState>(enabled)) {}
void MemoryPurgeJob::setEnabled(bool enabled) { journalState_->enabled = enabled; }
void MemoryPurgeJob::configureJournal(std::shared_ptr<deletionjournal::Journal> journal) {
    std::lock_guard<std::mutex> lock(journalState_->mutex);
    journalState_->journal = std::move(journal);
    if (!journalState_->busy) journalState_->reconcileAfter.clear();
}
bool MemoryPurgeJob::journalBusy() const { return journalState_->busy; }
MemoryPurgeJob::BackgroundReport MemoryPurgeJob::journalReport() const {
    std::lock_guard<std::mutex> lock(journalState_->mutex);
    return journalState_->report;
}

void MemoryPurgeJob::runLocked(drogon::orm::Transaction &tx, std::function<void(Result)> done) {
    if (!journalState_->enabled) {
        Result result;
        result.detail = "purge=off";
        done(std::move(result));
        return;
    }
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> lock(journalState_->mutex); journal = journalState_->journal; }
    if (journal) {
        if (journalState_->busy) { Result result; result.detail = "journal=busy"; done(std::move(result)); return; }
        tx.execSqlAsync(kJournalDueSql,
            [done, state = journalState_, journal](const drogon::orm::Result &rows) {
                try {
                    std::vector<Work> work;
                    for (const auto &row : rows) work.emplace_back(row["memory_id"].as<std::string>(), row["deletion_id"].as<std::string>());
                    Result result;
                    result.detail = "journal=queued due=" + std::to_string(work.size());
                    result.afterCommit = [state, journal, work = std::move(work)]() { startJournalRound(state, journal, work); };
                    done(std::move(result));
                } catch (const std::exception &) { Result result; result.ok = false; result.detail = "memory.purge stage=journal_prepare"; done(std::move(result)); }
            }, [done](const drogon::orm::DrogonDbException &) { Result result; result.ok = false; result.detail = "memory.purge stage=journal_prepare"; done(std::move(result)); }, std::to_string(kBatch));
        return;
    }
    auto finish = std::make_shared<std::function<void(Result)>>(std::move(done));
    auto fail = [finish](const drogon::orm::DrogonDbException &) {
        // Fixed text only: a database message can hold values. The tick's transaction rolls back whole, and the next tick tries again.
        Result result;
        result.ok = false;
        result.detail = "memory.purge stage=erase";
        (*finish)(std::move(result));
    };
    auto *transaction = &tx;
    tx.execSqlAsync(
        kEraseDueSql,
        [finish, fail, transaction](const drogon::orm::Result &erased) {
            int count = 0;
            try {
                count = erased.at(0)["erased"].as<int>();
            } catch (const std::exception &) {
                Result result;
                result.ok = false;
                result.detail = "memory.purge stage=count";
                (*finish)(std::move(result));
                return;
            }
            transaction->execSqlAsync(
                kBlockedSql,
                [finish, count](const drogon::orm::Result &rows) {
                    Result result;
                    int blocked = 0;
                    try {
                        blocked = rows.at(0)["blocked"].as<int>();
                    } catch (const std::exception &) {
                        result.ok = false;
                        result.detail = "memory.purge stage=count";
                        (*finish)(std::move(result));
                        return;
                    }
                    if (blocked > 0) LOG_ERROR << "memory.purge left " << blocked << " memories whose text another memory uses";
                    result.detail = "erased=" + std::to_string(count) + " blocked_shared_text=" + std::to_string(blocked);
                    (*finish)(std::move(result));
                },
                fail);
        },
        fail, std::to_string(kBatch));
}

}  // namespace usapp::scheduler
