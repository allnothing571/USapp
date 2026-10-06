// =====================================================================
// services/api/src/scheduler/jobs/SchedulerHeartbeatJob.cc
// =====================================================================
#include "scheduler/jobs/SchedulerHeartbeatJob.h"

#include <drogon/drogon.h>

#include <mutex>
#include <utility>

#include "db/Db.h"

namespace usapp::scheduler {

void SchedulerHeartbeatJob::setDeferredEffectForTest(std::function<void()> effect) {
    std::lock_guard<std::mutex> lock(mutex_);
    deferredEffect_ = std::move(effect);
}

long SchedulerHeartbeatJob::lockedRuns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lockedRuns_;
}

void SchedulerHeartbeatJob::runLocked(drogon::orm::Transaction &tx,
                                     std::function<void(Result)> done) {
    // 阶段 0 的持锁 DB 工作刻意做到最小：一次只读查询，不写任何业务表。
    tx.execSqlAsync(
        "SELECT 1 AS ok",
        [this, done](const drogon::orm::Result &r) {
            Result result;
            result.ok = !r.empty() && r[0]["ok"].as<int>() == 1;
            if (!result.ok) {
                result.detail = "心跳查询返回了意外的结果";
                done(result);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++lockedRuns_;
                if (deferredEffect_) {
                    result.afterCommit = deferredEffect_;
                }
            }
            result.detail = "调度心跳完成（阶段 0 不执行任何任务结算）";
            done(result);
        },
        [done](const drogon::orm::DrogonDbException &e) {
            Result result;
            result.ok = false;
            result.detail = db::describePgError("调度心跳查询失败", e.base());
            done(result);
        });
}

}  // namespace usapp::scheduler
