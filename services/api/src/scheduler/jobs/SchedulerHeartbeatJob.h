// =====================================================================
// services/api/src/scheduler/jobs/SchedulerHeartbeatJob.h
//
// 阶段 0 唯一注册的调度任务：心跳。
//
// 它【不结算任何任务周期、不动任何资产、不发任何通知】。存在的唯一
// 目的是让调度骨架可被真实执行与观测：证明
//   * 锁键能算出来并且稳定；
//   * pg_try_advisory_xact_lock 在持锁事务里被正确调用；
//   * scheduler_ticks 的写入与锁在同一事务、同一连接；
//   * afterCommit 动作发生在提交（= 释放锁）之后。
//
// 阶段 1 起，业务结算任务以同样的方式实现 SchedulerJob 并注册进来。
// =====================================================================
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "scheduler/Scheduler.h"

namespace usapp::scheduler {

class SchedulerHeartbeatJob : public SchedulerJob {
  public:
    // 稳定值。改它等于换锁，见 LockKey.h。
    static constexpr const char *kJobKey = "heartbeat";

    SchedulerHeartbeatJob() = default;
    ~SchedulerHeartbeatJob() override = default;

    std::string jobKey() const override { return kJobKey; }

    // 在持锁事务里只做一次 SELECT 1，然后登记 phase0 说明。
    void runLocked(drogon::orm::Transaction &tx,
                   std::function<void(Result)> done) override;

    // 测试钩子：设置后会在【提交之后】被调用。用来证明"网络操作不占用
    // 锁事务" —— 钩子执行时锁已经释放，另一个连接可以立即取得同一把锁。
    // 生产代码不设置它。
    void setDeferredEffectForTest(std::function<void()> effect);

    // 测试钩子：统计本任务实际进入持锁段（即成功取到锁）的次数。
    long lockedRuns() const;

  private:
    mutable std::mutex mutex_;
    std::function<void()> deferredEffect_;
    long lockedRuns_ = 0;
};

}  // namespace usapp::scheduler
