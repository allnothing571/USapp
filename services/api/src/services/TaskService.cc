// =====================================================================
// services/api/src/services/TaskService.cc
// =====================================================================
#include "services/TaskService.h"
#include "services/TaskStateSql.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <set>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- UTF-8
// 与 ProfileService 的严格解码规则相同（那里的实现位于其匿名命名空间，M1 文件不改动）。
struct CodePoint {
    char32_t value = 0;
    size_t offset = 0;
    size_t length = 0;
};

bool decodeUtf8Strict(const std::string &raw, std::vector<CodePoint> &out) {
    out.clear();
    const size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        const auto lead = static_cast<unsigned char>(raw[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (lead < 0x80) {
            cp = lead;
            len = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            cp = lead & 0x1F;
            len = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            cp = lead & 0x0F;
            len = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            cp = lead & 0x07;
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            const auto cont = static_cast<unsigned char>(raw[i + k]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;
        }
        out.push_back(CodePoint{cp, i, len});
        i += len;
    }
    return true;
}

// 与 0007 的标题检查一致：C0、DEL、C1、U+2028、U+2029。
bool isLineOrControl(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
}

// Unicode Zs（空格分隔符）。
bool isTrimmableSpace(char32_t cp) {
    return cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// ---------------------------------------------------------------- 日期时间形状
bool allDigits(const std::string &s, size_t pos, size_t len) {
    if (pos + len > s.size()) {
        return false;
    }
    for (size_t i = pos; i < pos + len; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

int number(const std::string &s, size_t pos, size_t len) {
    int value = 0;
    for (size_t i = pos; i < pos + len; ++i) {
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    return days[month - 1];
}

// 'YYYY-MM-DD'，年份 minYear..maxYear，日历有效。
bool validDate(const std::string &s, size_t pos, int minYear, int maxYear) {
    if (!allDigits(s, pos, 4) || s.size() < pos + 10 || s[pos + 4] != '-' || !allDigits(s, pos + 5, 2) ||
        s[pos + 7] != '-' || !allDigits(s, pos + 8, 2)) {
        return false;
    }
    const int year = number(s, pos, 4);
    const int month = number(s, pos + 5, 2);
    const int day = number(s, pos + 8, 2);
    if (year < minYear || year > maxYear || month < 1 || month > 12) {
        return false;
    }
    return day >= 1 && day <= daysInMonth(year, month);
}

// 截止日期：允许 1970-9998（次日 0 点仍是四位年份）。
bool validLocalDate(const std::string &s) {
    return s.size() == 10 && validDate(s, 0, 1970, 9998);
}

// 截止时间：'HH:MM'，00:00-23:59。
bool validLocalTime(const std::string &s) {
    return s.size() == 5 && allDigits(s, 0, 2) && s[2] == ':' && allDigits(s, 3, 2) &&
           number(s, 0, 2) <= 23 && number(s, 3, 2) <= 59;
}

bool isStrictInteger(const Json::Value &v) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt();
}

// ---------------------------------------------------------------- base64url（无填充）
const char *kBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64UrlEncode(const std::string &in) {
    std::string out;
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
        out.push_back(kBase64Url[(v >> 6) & 0x3F]);
        out.push_back(kBase64Url[v & 0x3F]);
        i += 3;
    }
    const size_t rest = in.size() - i;
    if (rest == 1) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
    } else if (rest == 2) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
        out.push_back(kBase64Url[(v >> 6) & 0x3F]);
    }
    return out;
}

int base64UrlValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// 严格解码：只接受字母表内字符、不接受填充，剩余位必须为 0（每个字节串只有一种编码）。
std::optional<std::string> base64UrlDecode(const std::string &in) {
    if (in.empty() || in.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : in) {
        const int v = base64UrlValue(c);
        if (v < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// M6 row closure and the derived status live in TaskStateSql.h, shared with the admin task reads (same SQL text).
#define USAPP_ROW_CLOSURE USAPP_TASK_ROW_CLOSURE

const char *kTaskColumns =
    "COALESCE(t.recurrence_id, t.id)::text AS id, t.title, t.description, "
    "t.creator_id::text AS creator_id, t.assignee_id::text AS assignee_id, "
    "t.reward_flowers, t.reward_sunshine, t.completion_mode, t.target_count, "
    // A stored period reports its template's schedule (daily or weekly).
    "COALESCE((SELECT r.schedule_type FROM tasks r WHERE r.id = t.recurrence_id), t.schedule_type) AS schedule_type, "
    "to_char(t.period_date, 'YYYY-MM-DD') AS period_date, "
    "to_char(t.start_local_date, 'YYYY-MM-DD') AS start_date, "
    "CASE WHEN t.completion_mode = 'single' THEN CASE WHEN c.task_id IS NULL THEN 0 ELSE 1 END "
    "ELSE COALESCE((SELECT p.progress_count FROM task_progress_events p WHERE p.task_id = t.id "
    "ORDER BY p.progress_count DESC LIMIT 1), 0) END AS progress_count, "
    "to_char(t.deadline_local_date, 'YYYY-MM-DD') AS deadline_local_date, "
    "substr(t.deadline_local_time::text, 1, 5) AS deadline_local_time, " USAPP_TS("t.deadline_at") " AS deadline_at, "
    USAPP_TS("c.completed_at") " AS completed_at, " USAPP_TS("t.created_at") " AS created_at, "
    USAPP_TASK_STATUS_CASE " AS status, "
    // M6 current derived state: the template/task closure, lineage and an undecided proposal.
    "(SELECT json_build_object('reason', k.reason, 'closed_at', " USAPP_TS("k.closed_at") ", "
    "'replaced_by', d.successor_task_id)::text FROM task_closures k "
    "LEFT JOIN task_change_decisions d ON d.proposal_id = k.proposal_id "
    "WHERE k.task_id = COALESCE(t.recurrence_id, t.id)) AS closure_json, "
    "(SELECT p.task_id::text FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id "
    "WHERE d.successor_task_id = COALESCE(t.recurrence_id, t.id)) AS replaces, "
    "(SELECT json_build_object('id', p.id, 'kind', p.kind, 'proposer_id', p.proposer_id, "
    "'created_at', " USAPP_TS("p.created_at") ", 'title', p.title, 'description', p.description, "
    "'reward_flowers', p.reward_flowers, 'reward_sunshine', p.reward_sunshine, "
    "'completion_mode', p.completion_mode, 'target_count', p.target_count, "
    "'deadline_local_date', to_char(p.deadline_local_date, 'YYYY-MM-DD'), "
    "'deadline_local_time', substr(p.deadline_local_time::text, 1, 5), "
    "'start_date', to_char(p.start_local_date, 'YYYY-MM-DD'))::text "
    "FROM task_change_proposals p WHERE p.task_id = COALESCE(t.recurrence_id, t.id) "
    "AND NOT EXISTS (SELECT 1 FROM task_change_decisions d WHERE d.proposal_id = p.id) "
    // Shown on root rows and on the current period of a template, not on its history.
    "AND (t.recurrence_id IS NULL OR t.deadline_at > now())) AS pending_json, "
    "(SELECT json_build_object('id',rx.id,'task_id',COALESCE(t.recurrence_id,t.id),"
    "'period_date',to_char(t.period_date,'YYYY-MM-DD'),'author_id',rx.author_id,'kind',rx.kind,"
    "'emoji_code',rx.emoji_code,'body',rx.body,'created_at'," USAPP_TS("rx.created_at") ")::text "
    "FROM task_reactions rx WHERE rx.completion_task_id=t.id) AS reaction_json";

// 列表：调用者的成员关系行 LEFT JOIN LATERAL 当前视图的一页。
// 没有成员关系 -> 0 行（SPACE_NOT_FOUND）；有成员关系但没有任务 -> 1 行且 id 为 NULL。
// 外层再按同样的键排序，不依赖 LATERAL 的输出顺序。
std::string openSql(const std::string &predicate, const std::string &limitParam) {
    return std::string("SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "
                       "LEFT JOIN LATERAL (SELECT ") +
           kTaskColumns +
           ", t.deadline_at AS sort_dl, t.created_at AS sort_ca, COALESCE(t.recurrence_id, t.id) AS sort_id "
           "FROM task_read_model t LEFT JOIN task_completions c ON c.task_id = t.id "
           "WHERE t.couple_id = m.couple_id AND c.task_id IS NULL "
           "AND (t.deadline_at IS NULL OR t.deadline_at > now()) "
           "AND NOT EXISTS (SELECT 1 " USAPP_ROW_CLOSURE ") " +
           predicate +
           " ORDER BY t.deadline_at ASC NULLS LAST, t.created_at ASC, COALESCE(t.recurrence_id, t.id) ASC LIMIT " + limitParam +
           "::integer) x ON true WHERE m.user_id = $1::uuid "
           "ORDER BY x.sort_dl ASC NULLS LAST, x.sort_ca ASC, x.sort_id ASC";
}

std::string historySql(const std::string &predicate, const std::string &limitParam) {
    return std::string("SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "
                       "LEFT JOIN LATERAL (SELECT ") +
           kTaskColumns + ", " USAPP_TS("COALESCE(c.completed_at, kc.closed_at, t.deadline_at)") " AS history_at, "
           "COALESCE(c.completed_at, kc.closed_at, t.deadline_at) AS sort_ha, t.id AS sort_id "
           "FROM task_read_model t LEFT JOIN task_completions c ON c.task_id = t.id "
           // M6: a closed row is history from its immutable closed_at (stable pagination key).
           "LEFT JOIN LATERAL (SELECT k.closed_at " USAPP_ROW_CLOSURE ") kc ON true "
           "WHERE t.couple_id = m.couple_id "
           "AND ((c.task_id IS NOT NULL AND c.completed_at > now() - interval '30 days') "
           "  OR (c.task_id IS NULL AND kc.closed_at IS NOT NULL AND kc.closed_at > now() - interval '30 days') "
           "  OR (c.task_id IS NULL AND kc.closed_at IS NULL "
           "      AND t.deadline_at <= now() AND t.deadline_at > now() - interval '30 days')) " +
           predicate +
           " ORDER BY COALESCE(c.completed_at, kc.closed_at, t.deadline_at) DESC, t.id DESC LIMIT " + limitParam +
           "::integer) x ON true WHERE m.user_id = $1::uuid "
           "ORDER BY x.sort_ha DESC, x.sort_id DESC";
}

const std::string &openFirstSql() {
    static const std::string sql = openSql("", "$2");
    return sql;
}

// 上一页最后一行有截止时间：有截止时间且排在其后，或者没有截止时间（过渡到无截止任务）。
const std::string &openAfterDatedSql() {
    static const std::string sql = openSql(
        "AND ((t.deadline_at IS NOT NULL AND (t.deadline_at, t.created_at, COALESCE(t.recurrence_id, t.id)) > "
        "($2::timestamptz, $3::timestamptz, $4::uuid)) OR t.deadline_at IS NULL)",
        "$5");
    return sql;
}

// 上一页最后一行没有截止时间：只在无截止任务内继续。
const std::string &openAfterUndatedSql() {
    static const std::string sql =
        openSql("AND t.deadline_at IS NULL AND (t.created_at, COALESCE(t.recurrence_id, t.id)) > ($2::timestamptz, $3::uuid)", "$4");
    return sql;
}

const std::string &historyFirstSql() {
    static const std::string sql = historySql("", "$2");
    return sql;
}

const std::string &historyAfterSql() {
    static const std::string sql =
        historySql("AND (COALESCE(c.completed_at, kc.closed_at, t.deadline_at), t.id) < ($2::timestamptz, $3::uuid)", "$4");
    return sql;
}

const std::string &getTaskSql(bool dated = false) {
    static const std::string head = std::string("SELECT m.couple_id::text AS couple_id, ") + kTaskColumns +
                                   " FROM couple_members m "
                                   "LEFT JOIN task_read_model t ON t.couple_id = m.couple_id "
                                   "AND COALESCE(t.recurrence_id, t.id) = $2::uuid ";
    static const std::string tail = "LEFT JOIN task_completions c ON c.task_id = t.id WHERE m.user_id = $1::uuid";
    static const std::string current = head + "AND t.current_period " + tail;
    static const std::string period = head + "AND t.period_date = $3::date " + tail;
    return dated ? period : current;
}

// 发布第 2 步：调用者与同伴，锁住调用者的成员行（FOR SHARE，usapp_api 保留 couple_members 的 UPDATE）。
const char *kMembershipSql =
    "SELECT m.couple_id::text AS couple_id, "
    "       (SELECT p.user_id::text FROM couple_members p "
    "         WHERE p.couple_id = m.couple_id AND p.user_id <> m.user_id "
    "         ORDER BY p.slot LIMIT 1) AS partner_id "
    "FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";

// 发布第 3 步：抢占。deadline_at 在 SQL 中按 Asia/Shanghai 计算（与 0007 的一致性检查同一表达式），
// deadline_in_future 在同一语句里由存储值与 clock_timestamp() 计算。
// 根任务没有发布时限触发器；0009 的实例守卫对根行直接返回，不影响同键冲突分类。
const char *kClaimSql =
    "INSERT INTO tasks (couple_id, creator_id, assignee_id, title, description, reward_flowers, "
    "reward_sunshine, timezone, deadline_local_date, deadline_local_time, deadline_at, "
    "create_idempotency_key, create_fingerprint, completion_mode, target_count, schedule_type, start_local_date) "
    "SELECT $1::uuid, $2::uuid, $3::uuid, $4::text, $5::text, $6::integer, $7::integer, 'Asia/Shanghai', d.ld, d.lt, "
    "       CASE WHEN d.ld IS NULL THEN NULL "
    "            WHEN d.lt IS NULL THEN (d.ld + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' "
    "            ELSE (d.ld + d.lt) AT TIME ZONE 'Asia/Shanghai' END, "
    "       $10::text, $11::text, $12::text, $13::integer, $14::text, NULLIF($15::text, '')::date "
    "FROM (SELECT NULLIF($8::text, '')::date AS ld, NULLIF($9::text, '')::time AS lt) d "
    "ON CONFLICT (creator_id, create_idempotency_key) DO NOTHING "
    "RETURNING id::text AS id, (deadline_at IS NULL OR deadline_at > clock_timestamp()) AS deadline_in_future, "
    // M5-C: a new custom range may start today or later (Beijing date at the claim).
    "(start_local_date IS NULL OR start_local_date >= (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date) AS start_not_past";

const std::string &existingByKeySql() {
    static const std::string sql = std::string("SELECT original.create_fingerprint, original.id::text AS original_id, "
                                   "(original.recurrence_id IS NOT NULL) AS internal_period, ") + kTaskColumns +
                                   " FROM tasks original JOIN task_read_model t ON t.current_period "
                                   "AND COALESCE(t.recurrence_id, t.id) = COALESCE(original.recurrence_id, original.id) "
                                   "LEFT JOIN task_completions c ON c.task_id = t.id "
                                   "WHERE original.creator_id = $1::uuid AND original.create_idempotency_key = $2";
    return sql;
}

const std::string &createdTaskSql() {
    static const std::string sql = std::string("SELECT ") + kTaskColumns +
                                   " FROM task_read_model t LEFT JOIN task_completions c ON c.task_id = t.id "
                                   "WHERE t.current_period AND COALESCE(t.recurrence_id, t.id) = $1::uuid";
    return sql;
}

// 审计：同一事务；detail 只含奖励数值与是否有截止时间，不含标题与描述文本。
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, 'task.created', 'task', $3, $4::jsonb)";

#undef USAPP_ROW_CLOSURE
#undef USAPP_TS

std::string textOrEmpty(const drogon::orm::Row &row, const char *name) {
    return row[name].isNull() ? std::string() : row[name].as<std::string>();
}

// The M6 JSON columns are built by the database (json_build_object); strict parse.
bool parseObject(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isObject();
}

// Proposal summary in the public shape; terms mirror the Task fields they would replace.
Json::Value pendingChangeJson(const TaskService::TaskData &task, const Json::Value &p) {
    Json::Value out(Json::objectValue);
    out["id"] = p["id"];
    out["kind"] = p["kind"];
    out["proposer_id"] = p["proposer_id"];
    out["created_at"] = p["created_at"];
    out["terms"] = TaskService::changeTermsJson(p);
    const auto blocked = TaskService::changeBlockedReason(task, p["kind"].asString());
    out["blocked_reason"] = blocked.empty() ? Json::Value(Json::nullValue) : Json::Value(blocked);
    return out;
}

// 读取一行任务列；列缺失或类型不符时抛异常，由调用方转成内部错误。
// 对外经 TaskService::taskFromRow 暴露（M3-C1，D41），签名与行为不变。
TaskService::TaskData mapTaskRow(const drogon::orm::Row &row, bool withHistory = false) {
    TaskService::TaskData task;
    task.id = row["id"].as<std::string>();
    task.title = row["title"].as<std::string>();
    task.description = row["description"].as<std::string>();
    task.creatorId = row["creator_id"].as<std::string>();
    task.assigneeId = row["assignee_id"].as<std::string>();
    task.rewardFlowers = row["reward_flowers"].as<int>();
    task.rewardSunshine = row["reward_sunshine"].as<int>();
    task.completionMode = row["completion_mode"].as<std::string>();
    task.targetCount = row["target_count"].as<int>();
    task.progressCount = row["progress_count"].as<int>();
    task.scheduleType = row["schedule_type"].as<std::string>();
    task.periodDate = textOrEmpty(row, "period_date");
    task.startDate = textOrEmpty(row, "start_date");
    task.deadlineLocalDate = textOrEmpty(row, "deadline_local_date");
    task.deadlineLocalTime = textOrEmpty(row, "deadline_local_time");
    task.deadlineAt = textOrEmpty(row, "deadline_at");
    task.status = row["status"].as<std::string>();
    task.completedAt = textOrEmpty(row, "completed_at");
    task.createdAt = row["created_at"].as<std::string>();
    task.closureJson = textOrEmpty(row, "closure_json");
    task.replaces = textOrEmpty(row, "replaces");
    task.pendingChangeJson = textOrEmpty(row, "pending_json");
    task.reactionJson = textOrEmpty(row, "reaction_json");
    if (withHistory) {
        task.historyAt = row["history_at"].as<std::string>();
    }
    return task;
}

TaskService::Status makeStatus(TaskService::Outcome outcome, std::string message = "", std::string detail = "") {
    TaskService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

// ---------------------------------------------------------------------
// 发布事务的生命周期与所有权（沿用 M1 ProfileService 的模式，第 4.1 节）：
//
//   CreateCtx --(shared)--> TxScope --(onError_，弱引用)--> CreateCtx
//
// 存进 TxScope 的错误回调只弱捕获 ctx；SQL 回调与提交回调强捕获 ctx。
// 所有结束路径都经 complete()，completed 标志保证 done 最多调用一次；
// done 保留到 ctx 析构时才释放（db_gated 用哨兵证明 ctx 被释放）。
// 本文件从不在 TxScope 成员函数执行期间重置 ctx->scope。
//
// commitRequested：调用 scope->commit() 之前置位。此后错误回调只可能来自提交
// （提交回调报告 false，或安装提交回调失败）。Drogon v1.9.11 只给布尔值，
// 无法区分“服务器拒绝提交”与“提交后连接丢失”，因此一律报告 ResultUnknown（第 4.4 节）。
// ---------------------------------------------------------------------
struct CreateCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    TaskService::CreateInput input;
    std::string key;
    std::string fingerprint;
    intent::Choice notifyChoice;
    Json::Value notification;  // original facts of this publication (P2-3b)
    std::string coupleId;
    std::string partnerId;
    std::string taskId;
    TaskService::TaskData created;
    TaskService::StatusCallback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;  // 应用自定义的固定阶段/原因标签；从不包含 ORM 或 TxScope 的消息文本
};

using CreateCtxPtr = std::shared_ptr<CreateCtx>;

void complete(const CreateCtxPtr &ctx, TaskService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "TaskService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) {
        ctx->done(std::move(status));
    }
}

// 记录固定的阶段/原因标签后失败。刻意不读取异常对象：PostgreSQL 错误文本可能回显输入
// （标题、描述、uuid 字面量等），因此不进入 status.detail，也不进入本服务与控制器的日志。
void failAt(const CreateCtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = TaskService::failureDetail("task_create", stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

// 业务分支：请求回滚（同步回调），然后结束。
void rollbackWith(const CreateCtxPtr &ctx, TaskService::Status status) {
    auto shared = std::make_shared<TaskService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

std::string auditDetail(const TaskService::CreateInput &input) {
    Json::Value detail(Json::objectValue);
    detail["reward_flowers"] = input.rewardFlowers;
    detail["reward_sunshine"] = input.rewardSunshine;
    detail["has_deadline"] = !input.deadlineLocalDate.empty();
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, detail);
}

// ---- 第 7 步：提交；只有提交回调确认成功才报告 Created ----
void createStepCommit(const CreateCtxPtr &ctx) {
    ctx->commitRequested = true;
    ctx->scope->commit([ctx]() {
        TaskService::Status status = makeStatus(TaskService::Outcome::Created);
        status.task = ctx->created;
        status.notification = ctx->notification;
        complete(ctx, std::move(status));
    });
}

// ---- 第 6 步（续）：在同一事务内读回新任务（响应用），然后提交 ----
void createStepReadBack(const CreateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        createdTaskSql(),
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) {
                failAt(ctx, "read_back", "row_count");
                return;
            }
            try {
                ctx->created = mapTaskRow(rows[0]);
            } catch (const std::exception &) {
                failAt(ctx, "read_back", "column_format");
                return;
            }
            createStepCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_back", "db_error"); },
        ctx->taskId);
}

// ---- 第 6 步（P2-3b）：同事务记录通知意图（必要时含偏好序号与站内通知），然后读回 ----
void createStepIntent(const CreateCtxPtr &ctx) {
    intent::Source source;
    source.taskId = ctx->taskId;
    intent::record(
        ctx->scope, intent::Operation::TaskPublish, source, ctx->notifyChoice,
        [ctx](Json::Value facts) {
            ctx->notification = std::move(facts);
            createStepReadBack(ctx);
        },
        [ctx]() { failAt(ctx, "notification_intent", "write_failed"); });
}

// ---- 第 6 步：同事务审计 ----
void createStepAudit(const CreateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx](const drogon::orm::Result &) { createStepIntent(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId,
        ctx->coupleId,
        ctx->taskId,
        auditDetail(ctx->input));
}

// ---- 第 4 步：已提交的同键任务 -> 重放或键复用 ----
void createStepClassifyExisting(const CreateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        existingByKeySql(),
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) {
                // ON CONFLICT DO NOTHING 只在已提交冲突后跳过，正常不会走到这里。
                failAt(ctx, "classify_existing", "row_missing");
                return;
            }
            std::string storedFingerprint;
            std::string originalId;
            TaskService::TaskData stored;
            try {
                if (rows[0]["internal_period"].as<bool>()) {
                    rollbackWith(ctx, makeStatus(TaskService::Outcome::KeyReused,
                                                 "该请求标识已用于另一份不同的发布内容"));
                    return;
                }
                storedFingerprint = rows[0]["create_fingerprint"].as<std::string>();
                originalId = rows[0]["original_id"].as<std::string>();
                stored = mapTaskRow(rows[0]);
            } catch (const std::exception &) {
                failAt(ctx, "classify_existing", "column_format");
                return;
            }
            if (storedFingerprint != ctx->fingerprint) {
                rollbackWith(ctx, makeStatus(TaskService::Outcome::KeyReused,
                                             "该请求标识已用于另一份不同的发布内容"));
                return;
            }
            // 重放：不做截止时间检查，原发布的结果保持有效。只读取原始通知意图
            // （旧发布没有意图记录时为 null），不追加意图、通知或偏好。
            auto status = std::make_shared<TaskService::Status>(makeStatus(TaskService::Outcome::Replayed));
            status->task = std::move(stored);
            intent::Source source;
            source.taskId = originalId;
            intent::readOriginal(
                ctx->scope, intent::Operation::TaskPublish, source,
                [ctx, status](Json::Value facts) {
                    status->notification = std::move(facts);
                    rollbackWith(ctx, std::move(*status));
                },
                [ctx]() { failAt(ctx, "classify_existing", "intent_read_failed"); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify_existing", "db_error"); },
        ctx->userId,
        ctx->key);
}

// ---- 第 3 步：抢占 ----
void createStepClaim(const CreateCtxPtr &ctx) {
    const auto &in = ctx->input;
    ctx->scope->tx().execSqlAsync(
        kClaimSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                createStepClassifyExisting(ctx);
                return;
            }
            bool inFuture = false;
            bool startNotPast = true;
            try {
                ctx->taskId = rows[0]["id"].as<std::string>();
                inFuture = rows[0]["deadline_in_future"].as<bool>();
                startNotPast = rows[0]["start_not_past"].as<bool>();
            } catch (const std::exception &) {
                failAt(ctx, "claim", "column_format");
                return;
            }
            if (!startNotPast) {
                // Only a new publication is checked; the candidate row is rolled back.
                rollbackWith(ctx, makeStatus(TaskService::Outcome::Validation, "开始日期不能早于今天（北京时间）"));
                return;
            }
            if (!inFuture) {
                // 第 5 步：只对新发布检查截止时间（D5b）；候选行随回滚消失。
                rollbackWith(ctx, makeStatus(TaskService::Outcome::Validation, "截止时间必须晚于当前时间"));
                return;
            }
            createStepAudit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "claim", "db_error"); },
        ctx->coupleId,
        ctx->userId,
        ctx->partnerId,
        in.title,
        in.description,
        std::to_string(in.rewardFlowers),
        std::to_string(in.rewardSunshine),
        in.deadlineLocalDate,
        in.deadlineLocalTime,
        ctx->key,
        ctx->fingerprint,
        in.completionMode,
        std::to_string(in.targetCount),
        in.scheduleType,
        in.startDate);
}

// ---- 第 2 步：成员关系与同伴 ----
void createStepMembership(const CreateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kMembershipSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rollbackWith(ctx, makeStatus(TaskService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u || rows[0]["couple_id"].isNull()) {
                failAt(ctx, "membership", "row_shape");
                return;
            }
            if (rows[0]["partner_id"].isNull()) {
                rollbackWith(ctx, makeStatus(TaskService::Outcome::PartnerRequired, "需要对方加入空间后才能发布任务"));
                return;
            }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->partnerId = rows[0]["partner_id"].as<std::string>();
            } catch (const std::exception &) {
                failAt(ctx, "membership", "column_format");
                return;
            }
            createStepClaim(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId);
}

}  // namespace

// =====================================================================
// 纯函数
// =====================================================================

TaskService &TaskService::instance() {
    static TaskService service;
    return service;
}

TaskService::TextError TaskService::normalizeTitle(const std::string &raw, std::string &out, size_t maxCodePoints) {
    out.clear();
    if (raw.size() > kTitleMaxRawBytes) {
        return TextError::TooLong;
    }
    std::vector<CodePoint> cps;
    if (!decodeUtf8Strict(raw, cps)) {
        return TextError::InvalidUtf8;
    }
    for (const auto &cp : cps) {
        if (isLineOrControl(cp.value)) {
            return TextError::ControlCharacter;
        }
    }
    size_t first = 0;
    size_t last = cps.size();
    while (first < last && isTrimmableSpace(cps[first].value)) {
        ++first;
    }
    while (last > first && isTrimmableSpace(cps[last - 1].value)) {
        --last;
    }
    if (first == last) {
        return TextError::Empty;
    }
    if (last - first > maxCodePoints) {
        return TextError::TooLong;
    }
    const size_t begin = cps[first].offset;
    const size_t end = cps[last - 1].offset + cps[last - 1].length;
    out = raw.substr(begin, end - begin);
    return TextError::None;
}

TaskService::TextError TaskService::normalizeDescription(const std::string &raw, std::string &out, size_t maxCodePoints) {
    out.clear();
    if (raw.size() > kDescriptionMaxRawBytes) {
        return TextError::TooLong;
    }
    std::vector<CodePoint> cps;
    if (!decodeUtf8Strict(raw, cps)) {
        return TextError::InvalidUtf8;
    }
    // CRLF -> LF；单独的 CR 与 LF 以外的换行/控制字符一律拒绝。
    std::vector<CodePoint> kept;
    kept.reserve(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        const char32_t cp = cps[i].value;
        if (cp == 0x0D) {
            if (i + 1 < cps.size() && cps[i + 1].value == 0x0A) {
                kept.push_back(CodePoint{0x0A, 0, 0});  // length 0：输出时写 "\n"
                ++i;
                continue;
            }
            return TextError::ControlCharacter;
        }
        if (cp != 0x0A && isLineOrControl(cp)) {
            return TextError::ControlCharacter;
        }
        kept.push_back(cps[i]);
    }
    auto trimmable = [](char32_t cp) { return cp == 0x0A || isTrimmableSpace(cp); };
    size_t first = 0;
    size_t last = kept.size();
    while (first < last && trimmable(kept[first].value)) {
        ++first;
    }
    while (last > first && trimmable(kept[last - 1].value)) {
        --last;
    }
    if (last - first > maxCodePoints) {
        return TextError::TooLong;
    }
    for (size_t i = first; i < last; ++i) {
        if (kept[i].length == 0) {
            out.push_back('\n');
        } else {
            out.append(raw, kept[i].offset, kept[i].length);
        }
    }
    return TextError::None;
}

bool TaskService::validIdempotencyKey(const std::string &key) {
    if (key.size() < kIdempotencyKeyMinLength || key.size() > kIdempotencyKeyMaxLength) {
        return false;
    }
    for (const char c : key) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool TaskService::parseCreateRequest(const Json::Value &body, CreateInput &out, std::string &message) {
    out = CreateInput{};
    if (!body.isObject()) {
        message = "请求体必须是一个 JSON 对象";
        return false;
    }
    static const std::set<std::string> allowed = {"title", "description", "reward_flowers", "reward_sunshine",
                                                  "deadline", "completion_mode", "target_count", "schedule_type",
                                                  "start_date", "end_date"};
    for (const auto &name : body.getMemberNames()) {
        if (allowed.find(name) == allowed.end()) {
            message = "请求体包含不支持的字段";
            return false;
        }
    }

    if (body.isMember("completion_mode")) {
        if (!body["completion_mode"].isString() ||
            (body["completion_mode"].asString() != "single" && body["completion_mode"].asString() != "count")) {
            message = "完成方式必须是 single 或 count";
            return false;
        }
        out.completionMode = body["completion_mode"].asString();
    }
    if (body.isMember("target_count")) {
        if (!isStrictInteger(body["target_count"]) || body["target_count"].asInt() < 1) {
            message = "目标次数必须是 1 到 2147483647 的整数";
            return false;
        }
        out.targetCount = body["target_count"].asInt();
    } else if (out.completionMode == "count") {
        message = "计数任务需要目标次数";
        return false;
    }
    if (out.completionMode == "single" && out.targetCount != 1) {
        message = "单次任务的目标次数必须为 1";
        return false;
    }
    if (body.isMember("schedule_type")) {
        const std::string schedule = body["schedule_type"].isString() ? body["schedule_type"].asString() : "";
        if (schedule != "one_off" && schedule != "daily" && schedule != "weekly" && schedule != "custom") {
            message = "重复方式必须是 one_off、daily、weekly 或 custom";
            return false;
        }
        out.scheduleType = schedule;
    }
    const bool hasDeadline = body.isMember("deadline") && !body["deadline"].isNull();
    if ((out.scheduleType == "daily" || out.scheduleType == "weekly") &&
        (out.completionMode != "count" || hasDeadline)) {
        message = out.scheduleType == "daily" ? "每天重复仅支持计数，按北京时间当天结束时到期"
                                              : "每周重复仅支持计数，按北京时间每周日结束时到期";
        return false;
    }
    if (out.scheduleType != "custom" && (body.isMember("start_date") || body.isMember("end_date"))) {
        message = "只有指定日期范围的任务可以设置开始和结束日期";
        return false;
    }
    if (out.scheduleType == "custom") {
        // M5-C: one bounded range, inclusive start/end dates in Asia/Shanghai.
        if (out.completionMode != "count" || hasDeadline) {
            message = "指定日期范围仅支持计数，按结束日期当天结束时到期";
            return false;
        }
        if (!body.isMember("start_date") || !body["start_date"].isString() ||
            !validLocalDate(body["start_date"].asString()) || !body.isMember("end_date") ||
            !body["end_date"].isString() || !validLocalDate(body["end_date"].asString())) {
            message = "开始和结束日期格式应为 YYYY-MM-DD";
            return false;
        }
        out.startDate = body["start_date"].asString();
        out.deadlineLocalDate = body["end_date"].asString();
        if (out.deadlineLocalDate < out.startDate) {  // same-width ISO dates compare lexically
            message = "结束日期不能早于开始日期";
            return false;
        }
    }

    if (!body.isMember("title") || !body["title"].isString()) {
        message = "标题必须是字符串";
        return false;
    }
    switch (normalizeTitle(body["title"].asString(), out.title)) {
        case TextError::None:
            break;
        case TextError::InvalidUtf8:
            message = "标题包含无法识别的字符";
            return false;
        case TextError::ControlCharacter:
            message = "标题不能包含换行或控制字符";
            return false;
        case TextError::Empty:
            message = "标题不能为空";
            return false;
        case TextError::TooLong:
            message = "标题最多 40 个字符";
            return false;
    }

    if (body.isMember("description")) {
        if (!body["description"].isString()) {
            message = "描述必须是字符串";
            return false;
        }
        switch (normalizeDescription(body["description"].asString(), out.description)) {
            case TextError::None:
            case TextError::Empty:
                break;
            case TextError::InvalidUtf8:
                message = "描述包含无法识别的字符";
                return false;
            case TextError::ControlCharacter:
                message = "描述只允许普通换行，不能包含其他控制字符";
                return false;
            case TextError::TooLong:
                message = "描述最多 500 个字符";
                return false;
        }
    }

    if (!body.isMember("reward_flowers") || !isStrictInteger(body["reward_flowers"]) ||
        !body.isMember("reward_sunshine") || !isStrictInteger(body["reward_sunshine"])) {
        message = "奖励必须是整数";
        return false;
    }
    out.rewardFlowers = body["reward_flowers"].asInt();
    out.rewardSunshine = body["reward_sunshine"].asInt();
    if (out.rewardFlowers < 0 || out.rewardFlowers > kMaxRewardFlowers) {
        message = "小红花奖励必须在 0 到 999 之间";
        return false;
    }
    if (out.rewardSunshine < 0 || out.rewardSunshine > kMaxRewardSunshine) {
        message = "阳光值奖励必须在 0 到 100000 之间";
        return false;
    }
    if (out.rewardFlowers == 0 && out.rewardSunshine == 0) {
        message = "至少需要一项大于 0 的奖励";
        return false;
    }

    if (body.isMember("deadline") && !body["deadline"].isNull()) {
        const Json::Value &deadline = body["deadline"];
        if (!deadline.isObject()) {
            message = "截止时间格式不正确";
            return false;
        }
        for (const auto &name : deadline.getMemberNames()) {
            if (name != "local_date" && name != "local_time") {
                message = "截止时间包含不支持的字段";
                return false;
            }
        }
        if (!deadline.isMember("local_date") || !deadline["local_date"].isString() ||
            !validLocalDate(deadline["local_date"].asString())) {
            message = "截止日期格式应为 YYYY-MM-DD";
            return false;
        }
        out.deadlineLocalDate = deadline["local_date"].asString();
        if (deadline.isMember("local_time") && !deadline["local_time"].isNull()) {
            if (!deadline["local_time"].isString() || !validLocalTime(deadline["local_time"].asString())) {
                message = "截止时间格式应为 HH:MM";
                return false;
            }
            out.deadlineLocalTime = deadline["local_time"].asString();
        }
    }
    return true;
}

std::string TaskService::canonicalCreateString(const CreateInput &input) {
    auto field = [](const char *name, const std::string &value) {
        return std::string(name) + "=" + std::to_string(value.size()) + ":" + value + ";";
    };
    const std::string base = std::string("create-task:v1:") + field("title", input.title) +
           field("description", input.description) + field("reward_flowers", std::to_string(input.rewardFlowers)) +
           field("reward_sunshine", std::to_string(input.rewardSunshine)) +
           field("deadline_local_date", input.deadlineLocalDate) +
           field("deadline_local_time", input.deadlineLocalTime);
    // Preserve fingerprints for every existing single task and old-client replay.
    const auto normalized = input.completionMode == "single" && input.targetCount == 1 ? base :
        base + field("completion_mode", input.completionMode) + field("target_count", std::to_string(input.targetCount));
    if (input.scheduleType == "one_off") return normalized;
    const auto scheduled = normalized + field("schedule_type", input.scheduleType);
    // The custom end date is already deadline_local_date; only the start is added.
    return input.scheduleType == "custom" ? scheduled + field("start_date", input.startDate) : scheduled;
}

std::string TaskService::createFingerprint(const CreateInput &input) {
    return crypto::sha256Hex(canonicalCreateString(input));
}

std::optional<TaskService::View> TaskService::parseView(const std::string &raw) {
    if (raw == "open") return View::Open;
    if (raw == "history") return View::History;
    return std::nullopt;
}

const char *TaskService::viewName(View view) {
    return view == View::Open ? "open" : "history";
}

bool TaskService::isCursorTimestamp(const std::string &s) {
    // YYYY-MM-DDTHH:MM:SS.ffffffZ
    if (s.size() != 27 || !validDate(s, 0, 1970, 9999) || s[10] != 'T' || !allDigits(s, 11, 2) || s[13] != ':' ||
        !allDigits(s, 14, 2) || s[16] != ':' || !allDigits(s, 17, 2) || s[19] != '.' || !allDigits(s, 20, 6) ||
        s[26] != 'Z') {
        return false;
    }
    return number(s, 11, 2) <= 23 && number(s, 14, 2) <= 59 && number(s, 17, 2) <= 59;
}

bool TaskService::isCanonicalUuid(const std::string &value) {
    if (value.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (dash ? c != '-' : !hex) {
            return false;
        }
    }
    return true;
}

std::string TaskService::encodeCursor(const Cursor &cursor) {
    // 固定字段顺序的紧凑 JSON；所有值都是服务器生成的安全字符。
    std::string json = std::string("{\"v\":1,\"view\":\"") + viewName(cursor.view) + "\",";
    if (cursor.view == View::Open) {
        json += "\"dl\":" + (cursor.deadlineAt ? "\"" + *cursor.deadlineAt + "\"" : std::string("null")) +
                ",\"ca\":\"" + cursor.createdAt + "\"";
    } else {
        json += "\"ha\":\"" + cursor.historyAt + "\"";
    }
    json += ",\"id\":\"" + cursor.id + "\"}";
    return base64UrlEncode(json);
}

std::optional<TaskService::Cursor> TaskService::decodeCursor(const std::string &raw, View expectedView) {
    if (raw.empty() || raw.size() > kMaxCursorLength) {
        return std::nullopt;
    }
    const auto json = base64UrlDecode(raw);
    if (!json) {
        return std::nullopt;
    }
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["allowSpecialFloats"] = false;
    Json::Value value;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json->data(), json->data() + json->size(), &value, &errors) || !value.isObject()) {
        return std::nullopt;
    }
    const auto names = value.getMemberNames();
    const std::set<std::string> present(names.begin(), names.end());
    const std::set<std::string> openKeys = {"v", "view", "dl", "ca", "id"};
    const std::set<std::string> historyKeys = {"v", "view", "ha", "id"};
    if (present != (expectedView == View::Open ? openKeys : historyKeys)) {
        return std::nullopt;
    }
    const Json::Value &v = value["v"];
    if (!isStrictInteger(v) || v.asInt() != 1) {
        return std::nullopt;
    }
    if (!value["view"].isString() || value["view"].asString() != viewName(expectedView)) {
        return std::nullopt;
    }
    if (!value["id"].isString() || !isCanonicalUuid(value["id"].asString())) {
        return std::nullopt;
    }
    Cursor cursor;
    cursor.view = expectedView;
    cursor.id = value["id"].asString();
    if (expectedView == View::Open) {
        const Json::Value &dl = value["dl"];
        if (dl.isNull()) {
            cursor.deadlineAt = std::nullopt;
        } else if (dl.isString() && isCursorTimestamp(dl.asString())) {
            cursor.deadlineAt = dl.asString();
        } else {
            return std::nullopt;
        }
        if (!value["ca"].isString() || !isCursorTimestamp(value["ca"].asString())) {
            return std::nullopt;
        }
        cursor.createdAt = value["ca"].asString();
    } else {
        if (!value["ha"].isString() || !isCursorTimestamp(value["ha"].asString())) {
            return std::nullopt;
        }
        cursor.historyAt = value["ha"].asString();
    }
    return cursor;
}

std::optional<int> TaskService::parseLimit(const std::string &raw) {
    if (raw.empty()) {
        return kDefaultPageSize;
    }
    if (raw.size() > 3 || !allDigits(raw, 0, raw.size())) {
        return std::nullopt;
    }
    const int value = number(raw, 0, raw.size());
    if (value < 1 || value > kMaxPageSize) {
        return std::nullopt;
    }
    return value;
}

const char *TaskService::listSql(ListQuery query) {
    switch (query) {
        case ListQuery::OpenFirst:
            return openFirstSql().c_str();
        case ListQuery::OpenAfterDated:
            return openAfterDatedSql().c_str();
        case ListQuery::OpenAfterUndated:
            return openAfterUndatedSql().c_str();
        case ListQuery::HistoryFirst:
            return historyFirstSql().c_str();
        case ListQuery::HistoryAfter:
            return historyAfterSql().c_str();
    }
    return historyFirstSql().c_str();
}

TaskService::ListQuery TaskService::listQueryFor(View view, const std::optional<Cursor> &cursor) {
    if (view == View::History) {
        return cursor ? ListQuery::HistoryAfter : ListQuery::HistoryFirst;
    }
    if (!cursor) {
        return ListQuery::OpenFirst;
    }
    return cursor->deadlineAt ? ListQuery::OpenAfterDated : ListQuery::OpenAfterUndated;
}

const char *TaskService::getSql() {
    return getTaskSql().c_str();
}

TaskService::TaskData TaskService::taskFromRow(const drogon::orm::Row &row, bool withHistory) {
    return mapTaskRow(row, withHistory);
}

const char *TaskService::taskColumnsSql() {
    return kTaskColumns;
}

Json::Value TaskService::changeTermsJson(const Json::Value &p) {
    if (p["kind"].asString() != "edit") return Json::Value(Json::nullValue);
    Json::Value terms(Json::objectValue);
    terms["title"] = p["title"];
    terms["description"] = p["description"];
    terms["reward_flowers"] = p["reward_flowers"];
    terms["reward_sunshine"] = p["reward_sunshine"];
    terms["completion_mode"] = p["completion_mode"];
    terms["target_count"] = p["target_count"];
    if (p["deadline_local_date"].isNull()) {
        terms["deadline"] = Json::Value(Json::nullValue);
    } else {
        terms["deadline"]["local_date"] = p["deadline_local_date"];
        terms["deadline"]["local_time"] = p["deadline_local_time"];
    }
    if (!p["start_date"].isNull()) terms["start_date"] = p["start_date"];
    return terms;
}

std::string TaskService::changeBlockedReason(const TaskData &task, const std::string &kind) {
    if (task.status == "cancelled" || task.status == "superseded") return "closed";
    const bool recurring = task.scheduleType == "daily" || task.scheduleType == "weekly";
    if (kind == "edit") {
        if (task.status == "completed") return "completed";
        if (task.progressCount > 0) return "progress";
        if (task.status == "expired") return "expired";
        return "";
    }
    // Cancelling a template stays possible after its current period completed.
    if (!recurring && task.status == "completed") return "completed";
    if (!recurring && task.status == "expired") return "expired";
    return "";
}

Json::Value TaskService::taskJson(const TaskData &task) {
    Json::Value out(Json::objectValue);
    out["id"] = task.id;
    out["title"] = task.title;
    out["description"] = task.description;
    out["creator_id"] = task.creatorId;
    out["assignee_id"] = task.assigneeId;
    Json::Value reaction;
    out["reaction"] = !task.reactionJson.empty() && parseObject(task.reactionJson, reaction)
                      ? reaction : Json::Value(Json::nullValue);
    out["reward_flowers"] = task.rewardFlowers;
    out["reward_sunshine"] = task.rewardSunshine;
    // Keep the original single-task response shape (including old completion
    // replay bodies). Counting metadata is additive only for counting tasks.
    if (task.completionMode == "count") {
        out["completion_mode"] = task.completionMode;
        out["target_count"] = task.targetCount;
        out["progress_count"] = task.progressCount;
    }
    if (task.scheduleType != "one_off") {
        out["schedule_type"] = task.scheduleType;
    }
    if (task.scheduleType == "daily" || task.scheduleType == "weekly") {
        out["period_date"] = task.periodDate;
    }
    if (task.scheduleType == "custom") {
        out["start_date"] = task.startDate;
    }
    if (task.deadlineLocalDate.empty()) {
        out["deadline"] = Json::Value(Json::nullValue);
    } else {
        Json::Value deadline(Json::objectValue);
        deadline["local_date"] = task.deadlineLocalDate;
        deadline["local_time"] =
            task.deadlineLocalTime.empty() ? Json::Value(Json::nullValue) : Json::Value(task.deadlineLocalTime);
        deadline["at"] = task.deadlineAt;
        out["deadline"] = deadline;
    }
    out["status"] = task.status;
    out["completed_at"] = task.completedAt.empty() ? Json::Value(Json::nullValue) : Json::Value(task.completedAt);
    out["created_at"] = task.createdAt;
    // M6 fields are additive and only present when they apply, so earlier bodies keep their shape.
    Json::Value closure;
    if (!task.closureJson.empty() && parseObject(task.closureJson, closure)) {
        Json::Value c(Json::objectValue);
        c["reason"] = closure["reason"];
        c["closed_at"] = closure["closed_at"];
        c["replaced_by"] = closure["replaced_by"];
        out["closure"] = c;
    }
    if (!task.replaces.empty()) out["replaces"] = task.replaces;
    Json::Value pending;
    if (!task.pendingChangeJson.empty() && parseObject(task.pendingChangeJson, pending)) {
        out["pending_change"] = pendingChangeJson(task, pending);
    }
    return out;
}

std::string TaskService::failureDetail(const std::string &operation,
                                       const std::string &stage,
                                       const std::string &reason) {
    auto label = [](const std::string &raw) {
        std::string out;
        for (const char c : raw) {
            out.push_back((c >= 'a' && c <= 'z') || c == '_' ? c : '?');
        }
        return out.empty() ? std::string("?") : out;
    };
    return label(operation) + " stage=" + label(stage) + " reason=" + label(reason);
}

ErrorCode TaskService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation:
            return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound:
            return ErrorCode::SpaceNotFound;
        case Outcome::TaskNotFound:
            return ErrorCode::TaskNotFound;
        case Outcome::PartnerRequired:
            return ErrorCode::PartnerRequired;
        case Outcome::KeyReused:
            return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown:
            return ErrorCode::ResultUnknown;
        case Outcome::Created:
        case Outcome::Replayed:
        case Outcome::Ok:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int TaskService::httpStatusFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Created:
            return 201;
        case Outcome::Replayed:
        case Outcome::Ok:
            return 200;
        default:
            return defaultStatusFor(errorCodeFor(outcome));
    }
}

// =====================================================================
// 发布
// =====================================================================

void TaskService::createTask(const std::string &userId,
                             const CreateInput &input,
                             const std::string &idempotencyKey,
                             StatusCallback cb,
                             intent::Choice notifyChoice) {
    if (userId.empty() || !validIdempotencyKey(idempotencyKey)) {
        cb(makeStatus(Outcome::Validation, "请求标识不合法", failureDetail("task_create", "input", "invalid_key")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", failureDetail("task_create", "pool", "not_ready")));
        return;
    }

    auto ctx = std::make_shared<CreateCtx>();
    ctx->userId = userId;
    ctx->input = input;
    ctx->key = idempotencyKey;
    // Omitted/false: the literal legacy fingerprint; true: the domain-separated extension.
    ctx->fingerprint = intent::fingerprint(createFingerprint(input), notifyChoice);
    ctx->notifyChoice = notifyChoice;
    ctx->done = std::move(cb);

    // onReady 只在 begin() 内同步调用、不被保存，可以强捕获；
    // 错误回调会被 TxScope 保存为 onError_，只能弱捕获。
    std::weak_ptr<CreateCtx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            createStepMembership(ctx);
        },
        [weakCtx](const std::string &) {
            // 不使用回调收到的消息：它可能来自事务创建异常或其他内部文本。
            // 只依据本服务记录的状态生成固定标签。
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(Outcome::ResultUnknown, "",
                                                failureDetail("task_create", "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(Outcome::Error, "",
                                                failureDetail("task_create", "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "TaskService 事务错误回调触发时上下文已释放，无法回调 stage=transaction";
        });
}

// =====================================================================
// 读取
// =====================================================================

void TaskService::listTasks(const std::string &userId,
                            View view,
                            const std::optional<Cursor> &cursor,
                            int limit,
                            ListCallback cb) {
    ListResult early;
    if (userId.empty() || limit < 1 || limit > kMaxPageSize || (cursor && cursor->view != view)) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = failureDetail("task_list", "pool", "not_ready");
        cb(std::move(early));
        return;
    }

    auto done = std::make_shared<ListCallback>(std::move(cb));
    const ListQuery query = listQueryFor(view, cursor);
    const bool history = view == View::History;
    auto onRows = [done, view, history, limit](const drogon::orm::Result &rows) {
        ListResult result;
        std::string historyLastId;
        if (rows.size() == 0u) {
            result.outcome = Outcome::SpaceNotFound;
            result.message = "尚未加入空间";
            (*done)(std::move(result));
            return;
        }
        try {
            for (const auto &row : rows) {
                if (row["id"].isNull()) {
                    continue;  // 有成员关系、没有任务的占位行
                }
                result.tasks.push_back(mapTaskRow(row, history));
                if (history && result.tasks.size() == static_cast<size_t>(limit)) {
                    // The stored instance id breaks equal-time ties across periods;
                    // the public task id remains the recurring template id.
                    historyLastId = row["sort_id"].as<std::string>();
                }
            }
        } catch (const std::exception &) {
            result = ListResult{};
            result.outcome = Outcome::Error;
            result.detail = failureDetail("task_list", "query", "column_format");
            (*done)(std::move(result));
            return;
        }
        if (result.tasks.size() > static_cast<size_t>(limit)) {
            result.tasks.resize(static_cast<size_t>(limit));
            const TaskData &last = result.tasks.back();
            Cursor next;
            next.view = view;
            next.id = history ? historyLastId : last.id;
            if (history) {
                next.historyAt = last.historyAt;
            } else {
                if (!last.deadlineAt.empty()) {
                    next.deadlineAt = last.deadlineAt;
                }
                next.createdAt = last.createdAt;
            }
            result.nextCursor = encodeCursor(next);
        }
        result.outcome = Outcome::Ok;
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult result;
        result.outcome = Outcome::Error;
        result.detail = failureDetail("task_list", "query", "db_error");
        (*done)(std::move(result));
    };

    const auto &client = db::Pool::instance().client();
    const std::string fetch = std::to_string(limit + 1);
    switch (query) {
        case ListQuery::OpenFirst:
        case ListQuery::HistoryFirst:
            client->execSqlAsync(listSql(query), std::move(onRows), std::move(onError), userId, fetch);
            return;
        case ListQuery::OpenAfterDated:
            client->execSqlAsync(listSql(query), std::move(onRows), std::move(onError), userId,
                                 *cursor->deadlineAt, cursor->createdAt, cursor->id, fetch);
            return;
        case ListQuery::OpenAfterUndated:
            client->execSqlAsync(listSql(query), std::move(onRows), std::move(onError), userId, cursor->createdAt,
                                 cursor->id, fetch);
            return;
        case ListQuery::HistoryAfter:
            client->execSqlAsync(listSql(query), std::move(onRows), std::move(onError), userId, cursor->historyAt,
                                 cursor->id, fetch);
            return;
    }
}

bool TaskService::validPeriodDate(const std::string &date) { return validLocalDate(date); }

bool TaskService::isWeekStart(const std::string &date) {
    if (!validLocalDate(date)) {
        return false;
    }
    // Days since 1970-01-01 (a Thursday), proleptic Gregorian; years are 1970-9998.
    int year = number(date, 0, 4);
    const int month = number(date, 5, 2);
    const int day = number(date, 8, 2);
    year -= month <= 2 ? 1 : 0;
    const long era = year / 400;
    const long yoe = year - era * 400;
    const long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + doe - 719468;
    return days % 7 == 4;  // 1970-01-05 (days = 4) was a Monday
}

void TaskService::getTask(const std::string &userId, const std::string &taskId, StatusCallback cb,
                          const std::string &periodDate) {
    if (!periodDate.empty() && !validPeriodDate(periodDate)) {
        cb(makeStatus(Outcome::Validation, "周期日期不合法"));
        return;
    }
    if (userId.empty()) {
        cb(makeStatus(Outcome::Error, "", failureDetail("task_get", "input", "missing_user")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", failureDetail("task_get", "pool", "not_ready")));
        return;
    }
    auto done = std::make_shared<StatusCallback>(std::move(cb));
    auto onRows = [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                (*done)(makeStatus(Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) {
                (*done)(makeStatus(Outcome::Error, "", failureDetail("task_get", "query", "row_count")));
                return;
            }
            if (rows[0]["id"].isNull()) {
                (*done)(makeStatus(Outcome::TaskNotFound, "任务不存在"));
                return;
            }
            Status status = makeStatus(Outcome::Ok);
            try {
                status.task = mapTaskRow(rows[0]);
            } catch (const std::exception &) {
                (*done)(makeStatus(Outcome::Error, "", failureDetail("task_get", "query", "column_format")));
                return;
            }
            (*done)(std::move(status));
        };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
            (*done)(makeStatus(Outcome::Error, "", failureDetail("task_get", "query", "db_error")));
        };
    if (periodDate.empty()) {
        db::Pool::instance().client()->execSqlAsync(getTaskSql(), std::move(onRows), std::move(onError), userId, taskId);
    } else {
        db::Pool::instance().client()->execSqlAsync(getTaskSql(true), std::move(onRows), std::move(onError),
                                                  userId, taskId, periodDate);
    }
}

}  // namespace usapp
