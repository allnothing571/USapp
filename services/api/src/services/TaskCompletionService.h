// =====================================================================
// services/api/src/services/TaskCompletionService.h
//
// 阶段 1 M3-C1：一次性任务的完成与奖励结算（docs/phase1-m3c-design.md 修订 3 第 4 节）。
//
// 已批准（2026-10-14）：D30 方案 A（不锁任务行）、D32 封闭枚举故障注入、D36-D39、D41。
//
// 事务顺序（单个 TxScope，READ COMMITTED）：
//   1. 读取调用者成员关系（FOR SHARE OF couple_members）与同空间的任务；只有接收者可以完成。
//   1b. M6（D30 修订）：任务级 advisory 锁 (33004, hashtext(task))，与计数、变更审批同一顺序；
//       新抢占之后检查关闭记录（已取消/已替换 -> 409，回滚无写入）；同键重放先于该检查。
//   2. 抢占：INSERT INTO wallet_operations ... ON CONFLICT DO NOTHING RETURNING。
//   3. 没有返回行：按 (发起者, TASK_REWARD, 键) 查找 -> 同指纹重放 / 不同指纹键复用；
//      按业务来源查找 -> 已被其他键完成。从不依赖 SQLSTATE。
//   4. 每个正奖励资产按 (user_id, asset_type) 升序：创建账户（ON CONFLICT DO NOTHING）并 FOR UPDATE。
//   5. 全部账户加锁之后读取最新流水；6. 余额上限检查（拒绝，不截断）。
//   7. 一条语句读取 clock_timestamp() 作为决策时间并插入完成记录（截止时间严格晚于决策时间）。
//   8. 每个正奖励资产一条流水（int64 + ::bigint）；9. 同事务审计 task.completed；
//   10. 事务内读回响应事实；11. 提交，只有提交回调确认成功才报告成功；
//       提交回调报告失败（或无法安装提交回调）一律 ResultUnknown。
//
// 响应只含不可变事实（任务与结算金额），从不含当前余额。
// 失败详情只用固定的阶段/原因标签；从不读取或记录数据库异常文本。
// =====================================================================
#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/Errors.h"
#include "services/TaskService.h"

namespace usapp {

class TaskCompletionService {
  public:
    static TaskCompletionService &instance();

    // D32：只有测试会传入非 None 值；控制器调用不带该参数。
    enum class SettlementFault { None, FailAfterFirstEntry };
    static constexpr SettlementFault kDefaultFault = SettlementFault::None;

    // D2 余额上限（与 0006 的 CHECK 一致）。
    static constexpr std::int64_t kMaxBalanceFlowers = 1000000;
    static constexpr std::int64_t kMaxBalanceSunshine = 1000000000;

    enum class Outcome {
        Completed,         // 200：首次完成，提交已确认
        Replayed,          // 200：同键同指纹的已提交结算
        Progressed,        // 200: a non-final counting increment
        Ignored,           // 200: the immutable target was already reached
        WrongMode,         // 409: use the endpoint for the task's mode
        Validation,        // 400
        SpaceNotFound,     // 404 SPACE_NOT_FOUND
        TaskNotFound,      // 404 TASK_NOT_FOUND
        NotAssignee,       // 403 TASK_NOT_ASSIGNEE
        AlreadyCompleted,  // 409 TASK_ALREADY_COMPLETED
        KeyReused,         // 409 IDEMPOTENCY_KEY_REUSED
        Expired,           // 409 TASK_EXPIRED
        BalanceLimit,      // 409 WALLET_BALANCE_LIMIT
        NotStarted,        // 409 TASK_NOT_STARTED: custom range before its start date
        Cancelled,         // 409 TASK_CANCELLED: M6 closure (no write)
        Superseded,        // 409 TASK_SUPERSEDED: M6 closure by an approved edit (no write)
        ResultUnknown,     // 503 RESULT_UNKNOWN
        Error,             // 500
    };

    struct Settlement {
        std::string operationId;
        int flower = 0;
        int sunshine = 0;
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // 面向用户（中文，不回显输入）
        std::string detail;   // 只写日志：固定标签
        TaskService::TaskData task;
        Settlement settlement;
        struct Progress {
            std::string eventId;
            int count = 0;
            int target = 1;
            std::string recordedAt;
            bool ignored = false;
        };
        std::optional<Progress> progress;
        bool hasSettlement = false;
        std::string periodDate;
        bool success() const {
            return outcome == Outcome::Completed || outcome == Outcome::Replayed ||
                   outcome == Outcome::Progressed || outcome == Outcome::Ignored;
        }
    };

    using Callback = std::function<void(Status)>;

    // ---------------------------------------------------------------- 纯函数
    // 完成请求的规范串与指纹：taskId 必须是数据库返回的小写规范 uuid。
    static std::string canonicalCompleteString(const std::string &taskId);
    static std::string completeFingerprint(const std::string &taskId);

    // 请求体只能为空（或只有空白），或恰好是一个空 JSON 对象 {}（允许空白）。
    static bool isEmptyCompletionBody(const std::string &raw);

    // balance + reward 不超过 cap（reward 为 0 时恒为 true）。
    static bool withinCap(std::int64_t balance, std::int64_t reward, std::int64_t cap);

    // 响应体：{"task": Task, "settlement": {"operation_id","flower","sunshine"}}，没有余额字段。
    static Json::Value responseBody(const Status &status);
    static Json::Value progressResponseBody(const Status &status);

    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- 结算
    // userId 来自会话；taskId 已通过 uuid 形状检查；idempotencyKey 已通过
    // TaskService::validIdempotencyKey。fault 只供测试使用（D32）。
    void completeTask(const std::string &userId,
                      const std::string &taskId,
                      const std::string &idempotencyKey,
                      Callback cb,
                      SettlementFault fault = kDefaultFault);

    // M4: progress keys are scoped to (assignee, progress action), independently
    // of completion keys. A final operation uses source uniqueness and NULL
    // wallet idempotency fields; the committed progress event owns its public key.
    void progressTask(const std::string &userId,
                      const std::string &taskId,
                      const std::string &idempotencyKey,
                      Callback cb,
                      SettlementFault fault = kDefaultFault);

    // Recurring counting tasks (M5-A daily, M5-B weekly). periodDate is the
    // displayed period identity: the Beijing date for daily, the Monday for weekly.
    void progressDailyTask(const std::string &userId, const std::string &taskId,
                           const std::string &idempotencyKey, const std::string &periodDate,
                           Callback cb, SettlementFault fault = kDefaultFault);

  private:
    TaskCompletionService() = default;
    void runTaskAction(const std::string &userId, const std::string &taskId,
                       const std::string &key, Callback cb, SettlementFault fault, bool progress,
                       const std::string &periodDate = "");
};

}  // namespace usapp
