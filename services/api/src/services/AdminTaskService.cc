// =====================================================================
// services/api/src/services/AdminTaskService.cc
// =====================================================================
#include "services/AdminTaskService.h"

#include <drogon/drogon.h>

#include <memory>

#include "db/Db.h"
#include "services/TaskStateSql.h"

namespace usapp {

namespace {

#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// Stored row kind: a stored period (recurrence_id set), a recurring template (daily/weekly root) or a plain task
// (one_off/custom root). Virtual periods that were never stored are not rows and are not listed.
#define USAPP_ADMIN_TASK_KIND                                                                          \
    "CASE WHEN t.recurrence_id IS NOT NULL THEN 'period' "                                             \
    "     WHEN t.schedule_type IN ('daily', 'weekly') THEN 'template' ELSE 'task' END"

// A stored period reports its template's schedule (as the public read does).
#define USAPP_ADMIN_TASK_SCHEDULE                                                                      \
    "COALESCE((SELECT r.schedule_type FROM tasks r WHERE r.id = t.recurrence_id AND r.couple_id = t.couple_id), " \
    "t.schedule_type)"

#define USAPP_ADMIN_TASK_PROGRESS                                                                      \
    "CASE WHEN t.completion_mode = 'single' THEN CASE WHEN c.task_id IS NULL THEN 0 ELSE 1 END "       \
    "ELSE COALESCE((SELECT max(p.progress_count) FROM task_progress_events p "                         \
    "               WHERE p.task_id = t.id AND p.couple_id = t.couple_id), 0) END"

// Allowlisted fields shared by the list item and the detail; never keys, fingerprints or wallet operation ids.
#define USAPP_ADMIN_TASK_CORE                                                                          \
    "'id', t.id, 'kind', " USAPP_ADMIN_TASK_KIND ", 'schedule_type', " USAPP_ADMIN_TASK_SCHEDULE ", "   \
    "'template_id', t.recurrence_id, 'period_date', to_char(t.period_date, 'YYYY-MM-DD'), "            \
    "'title', t.title, 'creator_id', t.creator_id, 'assignee_id', t.assignee_id, "                     \
    "'reward_flowers', t.reward_flowers::text, 'reward_sunshine', t.reward_sunshine::text, "           \
    "'completion_mode', t.completion_mode, 'target_count', t.target_count, "                           \
    "'progress_count', " USAPP_ADMIN_TASK_PROGRESS ", 'status', " USAPP_TASK_STATUS_CASE ", "          \
    "'start_date', to_char(t.start_local_date, 'YYYY-MM-DD'), "                                        \
    "'deadline_at', " USAPP_TS("t.deadline_at") ", 'created_at', " USAPP_TS("t.created_at") ", "       \
    "'completed_at', " USAPP_TS("c.completed_at")

#define USAPP_ADMIN_TASK_KIND_FILTER                                                                   \
    "($2::text = '' "                                                                                  \
    " OR ($2::text = 'task' AND t.recurrence_id IS NULL AND t.schedule_type IN ('one_off', 'custom')) " \
    " OR ($2::text = 'template' AND t.recurrence_id IS NULL AND t.schedule_type IN ('daily', 'weekly')) " \
    " OR ($2::text = 'period' AND t.recurrence_id IS NOT NULL))"

// Exact integer microseconds (extract returns numeric on PostgreSQL 14+), so the cursor key compares without rounding.
#define USAPP_ADMIN_TASK_KEY "(extract(epoch FROM t.created_at) * 1000000)::bigint"

// One page, newest first by (created_at, id), limit + 1 rows to know whether more exist. Space existence and the page share
// this statement's snapshot. Every task and child query is filtered by the selected couple.
const char *kListSql =
    "WITH page AS ("
    "  SELECT json_build_object(" USAPP_ADMIN_TASK_CORE ") AS item, " USAPP_ADMIN_TASK_KEY "::text AS ck, "
    "         t.created_at AS sk, t.id AS si "
    "    FROM tasks t LEFT JOIN task_completions c ON c.task_id = t.id AND c.couple_id = t.couple_id "
    "   WHERE t.couple_id = $1::uuid AND " USAPP_ADMIN_TASK_KIND_FILTER " "
    "     AND ($3::bigint = 0 OR (" USAPP_ADMIN_TASK_KEY ", t.id) < ($3::bigint, $4::uuid)) "
    "   ORDER BY t.created_at DESC, t.id DESC LIMIT $5::integer + 1) "
    "SELECT (SELECT s.status FROM couples s WHERE s.id = $1::uuid) AS space_status, "
    "(SELECT COALESCE(json_agg(json_build_object('item', p.item, 'ck', p.ck) ORDER BY p.sk DESC, p.si DESC), '[]'::json) "
    "   FROM page p)::text AS rows, "
    USAPP_TS("statement_timestamp()") " AS read_at";

#define USAPP_ADMIN_PROPOSAL_OBJECT                                                                    \
    "json_build_object('id', x.id, 'kind', x.kind, 'proposer_id', x.proposer_id, "                    \
    "'created_at', " USAPP_TS("x.created_at") ", "                                                     \
    "'terms', CASE WHEN x.kind = 'edit' THEN json_build_object('title', x.title, 'description', x.description, " \
    "  'reward_flowers', x.reward_flowers::text, 'reward_sunshine', x.reward_sunshine::text, "         \
    "  'completion_mode', x.completion_mode, 'target_count', x.target_count, "                         \
    "  'deadline_local_date', to_char(x.deadline_local_date, 'YYYY-MM-DD'), "                          \
    "  'deadline_local_time', substr(x.deadline_local_time::text, 1, 5), "                             \
    "  'start_date', to_char(x.start_local_date, 'YYYY-MM-DD')) END, "                                 \
    "'decision', CASE WHEN d.proposal_id IS NULL THEN NULL ELSE json_build_object('outcome', d.outcome, " \
    "  'decided_by', d.decided_by, 'decided_at', " USAPP_TS("d.decided_at") ", "                       \
    "  'successor_task_id', d.successor_task_id) END)"

// The whole detail in one statement (one snapshot). tk is empty unless the task belongs to the selected couple, and every
// child query is filtered by the couple and by this task (or by its root, for changes and closure, which live on the root).
const char *kDetailSql =
    "WITH tk AS (SELECT t.* FROM tasks t WHERE t.id = $2::uuid AND t.couple_id = $1::uuid), "
    "own AS (SELECT COALESCE(t.recurrence_id, t.id) AS id FROM tk t) "
    "SELECT (SELECT s.status FROM couples s WHERE s.id = $1::uuid) AS space_status, "
    "(SELECT json_build_object(" USAPP_ADMIN_TASK_CORE ", 'description', t.description, 'timezone', t.timezone, "
    "   'deadline_local_date', to_char(t.deadline_local_date, 'YYYY-MM-DD'), "
    "   'deadline_local_time', substr(t.deadline_local_time::text, 1, 5)) "
    "   FROM tk t LEFT JOIN task_completions c ON c.task_id = t.id AND c.couple_id = t.couple_id)::text AS task, "
    "(SELECT json_build_object('completed_by', c.completed_by, 'completed_at', " USAPP_TS("c.completed_at") ", "
    "   'reward_flowers', c.reward_flowers::text, 'reward_sunshine', c.reward_sunshine::text) "
    "   FROM tk t JOIN task_completions c ON c.task_id = t.id AND c.couple_id = $1::uuid)::text AS completion, "
    "(SELECT count(*) FROM tk t JOIN task_progress_events e ON e.task_id = t.id AND e.couple_id = $1::uuid) AS progress_total, "
    "(SELECT COALESCE(json_agg(json_build_object('id', x.id, 'user_id', x.user_id, 'progress_count', x.progress_count, "
    "   'target_count', x.target_count, 'recorded_at', " USAPP_TS("x.recorded_at") ", "
    "   'completes_task', x.operation_id IS NOT NULL) ORDER BY x.progress_count DESC), '[]'::json) "
    "   FROM (SELECT e.* FROM tk t JOIN task_progress_events e ON e.task_id = t.id AND e.couple_id = $1::uuid "
    "         ORDER BY e.progress_count DESC LIMIT $3::integer) x)::text AS progress, "
    "(SELECT json_build_object('id', rx.id, 'author_id', rx.author_id, 'kind', rx.kind, 'emoji_code', rx.emoji_code, "
    "   'body', rx.body, 'created_at', " USAPP_TS("rx.created_at") ") "
    "   FROM tk t JOIN task_reactions rx ON rx.completion_task_id = t.id AND rx.couple_id = $1::uuid)::text AS reaction, "
    "(SELECT json_build_object('owner_task_id', k.task_id, 'reason', k.reason, 'closed_at', " USAPP_TS("k.closed_at") ", "
    "   'proposal_id', k.proposal_id, 'successor_task_id', d.successor_task_id, "
    "   'applies_to_this_task', (t.deadline_at IS NULL OR t.deadline_at > k.closed_at)) "
    "   FROM tk t JOIN task_closures k ON k.task_id = COALESCE(t.recurrence_id, t.id) AND k.couple_id = $1::uuid "
    "   LEFT JOIN task_change_decisions d ON d.proposal_id = k.proposal_id AND d.couple_id = $1::uuid)::text AS closure, "
    "(SELECT json_build_object('task_id', p.task_id, 'proposal_id', p.id, 'decided_at', " USAPP_TS("d.decided_at") ") "
    "   FROM own o JOIN task_change_decisions d ON d.successor_task_id = o.id AND d.couple_id = $1::uuid "
    "   JOIN task_change_proposals p ON p.id = d.proposal_id AND p.couple_id = $1::uuid)::text AS replaces, "
    "(SELECT o.id::text FROM own o) AS owner_id, "
    "(SELECT count(*) FROM own o JOIN task_change_proposals p ON p.task_id = o.id AND p.couple_id = $1::uuid) AS proposals_total, "
    "(SELECT COALESCE(json_agg(" USAPP_ADMIN_PROPOSAL_OBJECT " ORDER BY x.created_at DESC, x.id DESC), '[]'::json) "
    "   FROM (SELECT p.* FROM own o JOIN task_change_proposals p ON p.task_id = o.id AND p.couple_id = $1::uuid "
    "         ORDER BY p.created_at DESC, p.id DESC LIMIT $4::integer) x "
    "   LEFT JOIN task_change_decisions d ON d.proposal_id = x.id AND d.couple_id = $1::uuid)::text AS proposals, "
    USAPP_TS("statement_timestamp()") " AS read_at";

bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

bool optionalJson(const drogon::orm::Row &row, const char *column, Json::Value &out) {
    if (row[column].isNull()) {
        out = Json::nullValue;
        return true;
    }
    return parseJson(row[column].as<std::string>(), out) && out.isObject();
}

bool positiveDecimal(const std::string &digits, std::int64_t &out) {
    if (digits.empty() || digits.size() > 19 || digits[0] == '0') return false;
    std::uint64_t value = 0;
    for (char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

// Canonical lowercase uuid only (the controller lowercases path ids; cursors carry what the server encoded).
bool lowerUuid(const std::string &s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

Json::Value space(const std::string &coupleId, const drogon::orm::Row &row) {
    Json::Value s;
    s["id"] = coupleId;
    s["status"] = row["space_status"].as<std::string>();
    return s;
}

}  // namespace

bool AdminTaskService::validKind(const std::string &kind) {
    return kind.empty() || kind == "task" || kind == "template" || kind == "period";
}

std::string AdminTaskService::encodeCursor(const std::string &coupleId, const std::string &kind, const Cursor &c) {
    return "t1." + (kind.empty() ? std::string("all") : kind) + "." + std::to_string(c.createdMicros) + "." + c.taskId + "." +
           coupleId;
}

bool AdminTaskService::decodeCursor(const std::string &raw, const std::string &coupleId, const std::string &kind, Cursor &out) {
    if (raw.size() > 160) return false;
    const std::string prefix = "t1." + (kind.empty() ? std::string("all") : kind) + ".";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    const std::string rest = raw.substr(dot + 1);  // <taskId>.<coupleId>
    if (rest.size() != 36 + 1 + 36 || rest[36] != '.') return false;
    const std::string taskId = rest.substr(0, 36);
    if (!lowerUuid(taskId) || rest.substr(37) != coupleId) return false;
    std::int64_t micros = 0;
    if (!positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), micros)) return false;
    out.createdMicros = micros;
    out.taskId = taskId;
    return true;
}

void AdminTaskService::list(const std::string &coupleId, const std::string &kind, int limit, const Cursor *cursor, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    const long long micros = cursor ? static_cast<long long>(cursor->createdMicros) : 0;
    const std::string after = cursor ? cursor->taskId : std::string("00000000-0000-0000-0000-000000000000");
    db::Pool::instance().client()->execSqlAsync(
        kListSql,
        [done, coupleId, kind, limit](const drogon::orm::Result &rows) {
            Result r;
            if (rows.size() != 1u) { (*done)(Result{}); return; }
            const auto &row = rows[0];
            if (row["space_status"].isNull()) { r.outcome = Outcome::SpaceNotFound; (*done)(std::move(r)); return; }
            Json::Value fetched;
            if (!parseJson(row["rows"].as<std::string>(), fetched) || !fetched.isArray()) { (*done)(Result{}); return; }
            Json::Value items(Json::arrayValue);
            std::string lastKey, lastId;
            for (Json::ArrayIndex i = 0; i < fetched.size() && static_cast<int>(i) < limit; ++i) {
                items.append(fetched[i]["item"]);
                lastKey = fetched[i]["ck"].asString();
                lastId = fetched[i]["item"]["id"].asString();
            }
            r.body["space"] = space(coupleId, row);
            r.body["kind"] = kind.empty() ? Json::Value(Json::nullValue) : Json::Value(kind);
            r.body["items"] = items;
            r.body["read_at"] = row["read_at"].as<std::string>();
            if (static_cast<int>(fetched.size()) > limit && !items.empty()) {
                Cursor next;
                if (!positiveDecimal(lastKey, next.createdMicros) || !lowerUuid(lastId)) { (*done)(Result{}); return; }
                next.taskId = lastId;
                r.body["next_cursor"] = encodeCursor(coupleId, kind, next);
            } else {
                r.body["next_cursor"] = Json::nullValue;
            }
            r.outcome = Outcome::Ok;
            (*done)(std::move(r));
        },
        [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin task list failed", e.base());
            (*done)(Result{});
        },
        coupleId, kind, micros, after, limit);
}

void AdminTaskService::detail(const std::string &coupleId, const std::string &taskId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kDetailSql,
        [done, coupleId](const drogon::orm::Result &rows) {
            Result r;
            if (rows.size() != 1u) { (*done)(Result{}); return; }
            const auto &row = rows[0];
            if (row["space_status"].isNull()) { r.outcome = Outcome::SpaceNotFound; (*done)(std::move(r)); return; }
            if (row["task"].isNull()) { r.outcome = Outcome::TaskNotFound; (*done)(std::move(r)); return; }
            Json::Value task, completion, reaction, closure, replaces, progress, proposals;
            if (!optionalJson(row, "task", task) || !optionalJson(row, "completion", completion) ||
                !optionalJson(row, "reaction", reaction) || !optionalJson(row, "closure", closure) ||
                !optionalJson(row, "replaces", replaces) || !parseJson(row["progress"].as<std::string>(), progress) ||
                !progress.isArray() || !parseJson(row["proposals"].as<std::string>(), proposals) || !proposals.isArray()) {
                (*done)(Result{});
                return;
            }
            r.body["space"] = space(coupleId, row);
            r.body["task"] = task;
            r.body["completion"] = completion;
            r.body["progress"]["total"] = static_cast<Json::Int64>(row["progress_total"].as<std::int64_t>());
            r.body["progress"]["items"] = progress;
            r.body["reaction"] = reaction;
            r.body["changes"]["owner_task_id"] = row["owner_id"].as<std::string>();
            r.body["changes"]["total"] = static_cast<Json::Int64>(row["proposals_total"].as<std::int64_t>());
            r.body["changes"]["items"] = proposals;
            r.body["closure"] = closure;
            r.body["replaces"] = replaces;
            r.body["read_at"] = row["read_at"].as<std::string>();
            r.outcome = Outcome::Ok;
            (*done)(std::move(r));
        },
        [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin task detail failed", e.base());
            (*done)(Result{});
        },
        coupleId, taskId, kProgressItems, kProposalItems);
}

}  // namespace usapp
