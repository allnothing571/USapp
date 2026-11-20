// =====================================================================
// services/api/src/services/TaskService.h
//
// 一次性任务的发布与读取（阶段 1 M3-B；docs/phase1-m3-design.md 修订 3
// 第 3.2、3.4、4.1、4.2、4.4、4.5 节）。M3-B 不含完成与结算（属于 M3-C）。
//
// 身份：userId 只来自服务端会话；空间与接收者（同一空间的另一成员）由服务端推导，
//   请求体没有 couple_id / assignee_id。
//
// 发布事务（先抢占幂等键，再分类；修订 3 第 4.2 节）：
//   1. 校验请求体与非时间规则（纯函数 parseCreateRequest），计算请求指纹。
//   2. 读取调用者与同伴的成员关系，FOR SHARE OF 调用者的 couple_members 行。
//   3. INSERT ... ON CONFLICT (creator_id, create_idempotency_key) DO NOTHING RETURNING。
//   4. 没有返回行：读取已提交的同键任务；指纹相同为重放（回滚，200），
//      不同为 IDEMPOTENCY_KEY_REUSED（回滚，409）。重放不做截止时间检查。
//   5. 返回了新行：截止时间不在未来则回滚并 400（只对新发布生效）。
//   6. 同一事务写入 task.created 审计（detail 不含标题与描述文本）。
//   7. 只有提交回调确认成功后才回调 Created；提交回调报告失败时结果未知（ResultUnknown）。
//
// 读取（列表与详情）：单条语句、只读、不开事务；成员关系从会话用户实时查询，
//   与游标内容无关。游标只在调用者空间的结果集内定位。
// =====================================================================
#pragma once

#include <drogon/orm/Row.h>
#include <json/json.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "services/NotificationIntent.h"

namespace usapp {

class TaskService {
  public:
    static TaskService &instance();

    // ---------------------------------------------------------------- 规则（D22/D23/D5）
    static constexpr int kMaxRewardFlowers = 999;
    static constexpr int kMaxRewardSunshine = 100000;
    static constexpr size_t kTitleMaxCodePoints = 40;
    static constexpr size_t kDescriptionMaxCodePoints = 500;
    // 超过该字节数直接判为过长，不逐字符解码。
    static constexpr size_t kTitleMaxRawBytes = 1024;
    static constexpr size_t kDescriptionMaxRawBytes = 8192;
    static constexpr size_t kIdempotencyKeyMinLength = 16;
    static constexpr size_t kIdempotencyKeyMaxLength = 128;
    static constexpr int kDefaultPageSize = 50;
    static constexpr int kMaxPageSize = 100;
    static constexpr size_t kMaxCursorLength = 512;

    enum class TextError {
        None,
        InvalidUtf8,       // 不是合法 UTF-8（过长编码、代理项、超出 U+10FFFF、截断）
        ControlCharacter,  // 控制字符或禁止的换行（标题：任何换行；描述：LF 以外的换行）
        Empty,             // 标题去首尾空白后为空
        TooLong,           // 超过码点上限
    };

    // 标题：严格 UTF-8；C0（含 \t \n \r）、DEL、C1、U+2028、U+2029 在任何位置都拒绝；
    //   去掉首尾 Unicode Zs 空白；1..40 个码点（其他资源可传自己的上限）。成功时 out 为规范化后的值。
    static TextError normalizeTitle(const std::string &raw, std::string &out, size_t maxCodePoints = kTitleMaxCodePoints);

    // 描述：严格 UTF-8；先把 CRLF 归一为 LF；之后单独的 CR、LF 以外的 C0、DEL、C1、
    //   U+2028、U+2029 一律拒绝；再去掉首尾的 Unicode Zs 空白与 LF；0..500 个码点（其他资源可传自己的上限）。
    static TextError normalizeDescription(const std::string &raw, std::string &out, size_t maxCodePoints = kDescriptionMaxCodePoints);

    // Idempotency-Key：16..128 个 [A-Za-z0-9_-] 字符（客户端发送随机 UUID）。
    static bool validIdempotencyKey(const std::string &key);

    struct CreateInput {
        std::string title;        // 已规范化
        std::string description;  // 已规范化，可为空串
        int rewardFlowers = 0;
        int rewardSunshine = 0;
        std::string completionMode = "single";
        int targetCount = 1;
        std::string scheduleType = "one_off";
        std::string deadlineLocalDate;  // 'YYYY-MM-DD'，空串表示没有截止时间
        std::string deadlineLocalTime;  // 'HH:MM'，空串表示只有日期
        // M5-C custom range: inclusive start date; the inclusive end date is
        // deadlineLocalDate (date only). Empty for every other schedule.
        std::string startDate;
    };

    // 纯函数：校验请求体（必须恰好由允许的字段组成）与非时间规则。
    // 失败时返回 false，message 为不回显输入的中文提示。截止时间是否在未来不在这里检查。
    static bool parseCreateRequest(const Json::Value &body, CreateInput &out, std::string &message);
    static bool validPeriodDate(const std::string &date);
    // M5-B: a valid period date that is a Monday (ISO weekday 1), the stored
    // identity of a weekly period in Asia/Shanghai.
    static bool isWeekStart(const std::string &date);

    // 发布请求指纹：小写 SHA-256 十六进制；规范串为 "create-task:v1:" 加上
    // 按固定顺序、带字节长度的各字段（第 4.5 节）。
    static std::string canonicalCreateString(const CreateInput &input);
    static std::string createFingerprint(const CreateInput &input);

    // ---------------------------------------------------------------- 游标（第 3.4 节）
    enum class View { Open, History };
    static std::optional<View> parseView(const std::string &raw);
    static const char *viewName(View view);

    struct Cursor {
        View view = View::Open;
        std::optional<std::string> deadlineAt;  // open：dl（可为 null）
        std::string createdAt;                  // open：ca
        std::string historyAt;                  // history：ha
        std::string id;                         // 小写规范 uuid
    };

    static std::string encodeCursor(const Cursor &cursor);
    // 严格解码：任何不符合第 3.4 节格式的输入都返回 std::nullopt（不执行查询）。
    static std::optional<Cursor> decodeCursor(const std::string &raw, View expectedView);

    // 'YYYY-MM-DDTHH:MM:SS.ffffffZ'，日期与时间的取值都必须有效。
    static bool isCursorTimestamp(const std::string &value);
    static bool isCanonicalUuid(const std::string &value);

    // limit 查询参数：空串为默认 50；否则必须是 1..100 的十进制整数（不收敛，非法即 400）。
    static std::optional<int> parseLimit(const std::string &raw);

    // ---------------------------------------------------------------- 生产 SQL
    // 暴露给测试，保证回滚测试执行的就是生产查询文本。
    // 参数顺序：
    //   OpenFirst:      $1 user_id, $2 limit+1
    //   OpenAfterDated: $1 user_id, $2 dl, $3 ca, $4 id, $5 limit+1
    //   OpenAfterUndated: $1 user_id, $2 ca, $3 id, $4 limit+1
    //   HistoryFirst:   $1 user_id, $2 limit+1
    //   HistoryAfter:   $1 user_id, $2 ha, $3 id, $4 limit+1
    enum class ListQuery { OpenFirst, OpenAfterDated, OpenAfterUndated, HistoryFirst, HistoryAfter };
    static const char *listSql(ListQuery query);
    static ListQuery listQueryFor(View view, const std::optional<Cursor> &cursor);
    static const char *getSql();  // $1 user_id, $2 task_id

    // ---------------------------------------------------------------- 结果
    struct TaskData {
        std::string id;
        std::string title;
        std::string description;
        std::string creatorId;
        std::string assigneeId;
        int rewardFlowers = 0;
        int rewardSunshine = 0;
        std::string completionMode = "single";
        int targetCount = 1;
        int progressCount = 0;
        std::string scheduleType = "one_off";
        std::string periodDate;  // Daily date or weekly Monday in Asia/Shanghai; empty for one-off tasks.
        std::string startDate;   // Custom range start date (inclusive); empty otherwise.
        std::string deadlineLocalDate;  // 空串：没有截止时间
        std::string deadlineLocalTime;  // 空串：只有日期
        std::string deadlineAt;         // 六位小数 UTC
        // open | completed | expired | upcoming (custom range not started) | cancelled | superseded (M6)
        std::string status;
        std::string completedAt;        // 空串：未完成
        std::string createdAt;          // 六位小数 UTC
        // M6 current derived state (database JSON text; empty when absent): the root's
        // closure, the predecessor root of a successor, and an undecided change proposal.
        std::string closureJson;
        std::string replaces;
        std::string pendingChangeJson;
        std::string reactionJson;  // P2-4: the immutable reaction to this exact completion.
        // 游标字段（不进入响应）
        std::string historyAt;
    };

    static Json::Value taskJson(const TaskData &task);

    // M3-C1（D41）：与发布、列表、详情相同的行映射与列清单，供完成服务读回与重放复用。
    // 列表 history 视图以 withHistory = true 调用以填充 historyAt；其他调用使用默认值。
    // 列缺失或类型不符时抛异常。列清单引用别名 t（tasks）与 c（task_completions）。
    static TaskData taskFromRow(const drogon::orm::Row &row, bool withHistory = false);
    static const char *taskColumnsSql();

    // M6: why an undecided proposal of this kind cannot be approved now, judged from
    // the row's current derived state ("" when approvable; "progress", "completed",
    // "expired" or "closed" otherwise). Reject and withdraw always stay possible.
    static std::string changeBlockedReason(const TaskData &task, const std::string &kind);
    // M6: public edit terms from a flat proposal object (database json_build_object keys);
    // null for a cancellation. Shared by task reads and change responses.
    static Json::Value changeTermsJson(const Json::Value &flatProposal);

    enum class Outcome {
        Created,            // 201：新任务，提交已确认
        Replayed,           // 200：同键同指纹的已提交任务
        Ok,                 // 200：读取成功
        Validation,         // 400
        SpaceNotFound,      // 404 SPACE_NOT_FOUND
        TaskNotFound,       // 404 TASK_NOT_FOUND（不存在或属于其他空间，不区分）
        PartnerRequired,    // 409 PARTNER_REQUIRED
        KeyReused,          // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,      // 503 RESULT_UNKNOWN：提交结果无法确认
        Error,              // 500
    };

    // 内部失败描述（只写日志）：固定格式 "<operation> stage=<stage> reason=<reason>"，
    // 三段都只保留 [a-z_]。数据库与事务层的异常文本从不进入 detail（它们可能回显输入）。
    static std::string failureDetail(const std::string &operation, const std::string &stage, const std::string &reason);

    // 失败结果对应的错误码；成功结果不应调用（返回 InternalError）。
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // 面向用户（中文，不回显输入）
        std::string detail;   // 只写日志；固定的阶段/原因标签（failureDetail），不含输入或数据库错误文本
        TaskData task;
        // P2-3b: original notification facts of a publication (Json null: legacy operation
        // without a recorded intent, or not a publication).
        Json::Value notification;
        bool success() const {
            return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok;
        }
    };

    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<TaskData> tasks;
        std::optional<std::string> nextCursor;
    };

    using StatusCallback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // 发布。userId 来自会话；idempotencyKey 必须已通过 validIdempotencyKey。
    // P2-3b: notifyChoice is the optional "notify_partner" (omitted keeps the legacy
    // fingerprint and is never remembered); see services/NotificationIntent.h.
    void createTask(const std::string &userId,
                    const CreateInput &input,
                    const std::string &idempotencyKey,
                    StatusCallback cb,
                    intent::Choice notifyChoice = {});

    void listTasks(const std::string &userId,
                   View view,
                   const std::optional<Cursor> &cursor,
                   int limit,
                   ListCallback cb);

    void getTask(const std::string &userId, const std::string &taskId, StatusCallback cb,
                 const std::string &periodDate = "");

  private:
    TaskService() = default;
};

}  // namespace usapp
