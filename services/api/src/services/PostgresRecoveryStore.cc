#include "services/PostgresRecoveryStore.h"
#include <json/json.h>

namespace usapp::deletionrecovery {
namespace {
std::string payload(Snapshot snapshot) {
    if (!normalize(snapshot)) throw std::invalid_argument("Invalid recovery snapshot");
    Json::Value root(Json::objectValue);
    root["head"]["journal_id"] = snapshot.head.journalId;
    root["head"]["seq"] = Json::UInt64(snapshot.head.seq);
    root["head"]["hash"] = snapshot.head.hash;
    for (auto pair : {std::make_pair("hidden", &snapshot.hidden), std::make_pair("purge_barriers", &snapshot.purgeBarriers)}) {
        root[pair.first] = Json::arrayValue;
        for (const auto &t : *pair.second) {
            Json::Value v(Json::objectValue); v["kind"] = t.kind == Kind::Memory ? "memory" : "media"; v["id"] = t.id;
            root[pair.first].append(std::move(v));
        }
    }
    root["unresolved"] = Json::arrayValue;
    for (const auto &intent : snapshot.unresolved) root["unresolved"].append(intent);
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    return Json::writeString(writer, root);
}
struct Reply {
    bool answered = false;
    std::function<void(Receipt, uint64_t)> callback;
    void done(Receipt r, uint64_t g = 0) {
        if (answered) return;
        answered = true; auto cb = std::move(callback); cb(r, g);
    }
};
template <typename... Args>
void query(const db::ClientPtr &client, const char *sql, bool generation, std::function<void(Receipt, uint64_t)> cb, Args... args) {
    auto reply = std::make_shared<Reply>(); reply->callback = std::move(cb);
    db::TxScope::beginOnClientAsync(client,
        [reply, sql, generation, args...](std::shared_ptr<db::TxScope> scope) {
            scope->tx().execSqlAsync(sql,
                [reply, scope, generation](const drogon::orm::Result &rows) {
                    try {
                        if (rows.size() != 1u || rows[0]["result"].isNull()) { scope->fail("Recovery result missing"); return; }
                        const uint64_t value = generation ? rows[0]["result"].as<uint64_t>() : (rows[0]["result"].as<bool>() ? 1u : 0u);
                        // Never acknowledge a query result until the explicit transaction's commit is confirmed.
                        scope->commit([reply, value]() { reply->done(value ? Receipt::Confirmed : Receipt::Conflict, value); });
                    } catch (...) { scope->fail("Recovery result invalid"); }
                },
                [scope](const drogon::orm::DrogonDbException &) { scope->fail("Recovery statement failed"); }, args...);
        },
        [reply](const std::string &) { reply->done(Receipt::Unknown); });
}
bool validFence(const Fence &f) {
    return deletionjournal::uuidShape(f.databaseId) && deletionjournal::uuidShape(f.attemptId) && f.generation > 0 &&
        f.generation <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}
}
void PostgresStore::close(const Request &r, std::function<void(Receipt, Fence)> cb) {
    if (!deletionjournal::uuidShape(r.databaseId) || !deletionjournal::uuidShape(r.attemptId) || !deletionjournal::uuidShape(r.journalId)) {
        cb(Receipt::Conflict, {}); return;
    }
    query(client_, "SELECT public.usapp_recovery_close($1::uuid,$2::uuid,$3::uuid) AS result", true,
        [r, cb](Receipt receipt, uint64_t g) { cb(receipt, {r.databaseId, r.attemptId, g}); }, r.databaseId, r.attemptId, r.journalId);
}
void PostgresStore::stage(const Fence &f, const Snapshot &s, std::function<void(Receipt)> cb) {
    if (!validFence(f)) { cb(Receipt::Conflict); return; }
    std::string data;
    try { data = payload(s); } catch (...) { cb(Receipt::Conflict); return; }
    query(client_, "SELECT public.usapp_recovery_stage($1::uuid,$2::uuid,$3::bigint,$4::jsonb) AS result", false,
        [cb](Receipt r, uint64_t) { cb(r); }, f.databaseId, f.attemptId, std::to_string(f.generation), data);
}
void PostgresStore::verify(const Fence &f, const Snapshot &s, std::function<void(Receipt)> cb) {
    if (!validFence(f)) { cb(Receipt::Conflict); return; }
    std::string data;
    try { data = payload(s); } catch (...) { cb(Receipt::Conflict); return; }
    query(client_, "SELECT public.usapp_recovery_verify($1::uuid,$2::uuid,$3::bigint,$4::jsonb) AS result", false,
        [cb](Receipt r, uint64_t) { cb(r); }, f.databaseId, f.attemptId, std::to_string(f.generation), data);
}
void PostgresStore::recordVerified(const Fence &f, const Head &h, std::function<void(Receipt)> cb) {
    if (!validFence(f) || !validHead(h)) { cb(Receipt::Conflict); return; }
    query(client_, "SELECT public.usapp_recovery_record($1::uuid,$2::uuid,$3::bigint,$4::bigint,$5::text) AS result", false,
        [cb](Receipt r, uint64_t) { cb(r); }, f.databaseId, f.attemptId, std::to_string(f.generation), std::to_string(h.seq), h.hash);
}
} // namespace usapp::deletionrecovery
