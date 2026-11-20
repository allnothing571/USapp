// =====================================================================
// services/api/src/services/AdminTaskService.h
// Admin-only, read-only inspection of the stored task rows of one selected couple/space (docs/admin-task-inspection.md).
// Each answer is one SQL statement, so one snapshot; list pages and the detail are separate reads with no cross-read snapshot.
// Status comes from the shared TaskStateSql.h expression. Rewards are canonical decimal strings. Writes nothing.
// The public task reads (TaskService) are unchanged.
// =====================================================================
#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>

namespace usapp {

class AdminTaskService {
  public:
    enum class Outcome { Ok, SpaceNotFound, TaskNotFound, Error };
    struct Result {
        Outcome outcome = Outcome::Error;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;

    // Optional list filter: "" (all), "task" (one_off/custom), "template" (daily/weekly root), "period" (stored period row).
    static bool validKind(const std::string &kind);

    struct Cursor {
        std::int64_t createdMicros = 0;  // microseconds since the Unix epoch of the last row's created_at
        std::string taskId;              // lowercase uuid of the last row
    };
    // t1.<all|task|template|period>.<micros>.<taskId>.<coupleId>
    static std::string encodeCursor(const std::string &coupleId, const std::string &kind, const Cursor &c);
    // Valid only for this couple and this filter. Returns false for anything malformed or mismatched.
    static bool decodeCursor(const std::string &raw, const std::string &coupleId, const std::string &kind, Cursor &out);

    // Newest first by (created_at, id). Without a cursor, from the newest row.
    static void list(const std::string &coupleId, const std::string &kind, int limit, const Cursor *cursor, Callback cb);
    static void detail(const std::string &coupleId, const std::string &taskId, Callback cb);

    static constexpr int kProgressItems = 200;   // newest progress events returned in the detail (total always reported)
    static constexpr int kProposalItems = 100;   // newest change proposals returned in the detail (total always reported)
};

}  // namespace usapp
