// =====================================================================
// services/api/src/scheduler/Scheduler.h
//
// 进程内调度入口（阶段 0 只搭基础，不实现任何任务结算）。
//
// 一次 tick 的完整顺序（顺序本身是需求的一部分，不要重排）：
//
//   1. TxScope::begin()  —— 从连接池取【一条】连接，开事务。
//   2. SELECT pg_try_advisory_xact_lock(<固定的 64 位键>)
//        * false -> 立刻【请求回滚】并记Skipped。【不写任何行】。
//   3. true  -> 调用 SchedulerJob::runLocked()，任务的全部 DB 操作
//               必须使用传进来的这个 Transaction（同一连接、同一事务）。
//   4. 同一事务内 INSERT scheduler_ticks(..., outcome='completed')。
//   5. commit —— 提交即释放锁（事务级 advisory lock）。
//      【关键】Drogon v1.9.11 没有 Transaction::commit()；提交是
//      setCommitCallback(true) + 释放事务引用，由 ~TransactionImpl 发出的
//      commit 语句完成。只有该回调收到 true，第 6 步才允许执行。
//   6. 【提交成功之后】才执行 job 登记的 afterCommit 动作（发信/推送/HTTP）。
//      网络操作绝不允许出现在第 2..5 步之间，否则锁会被网络超时拖住。
//
// 失败路径：runLocked 报错 -> 【请求回滚】-> 用一个独立的
// 语句写 outcome='failed' 行。失败行不属于持锁事务，因此不影响
// 「锁与 DB 操作同事务」这一可验证性质。
//
// 【rollback() 的准确语义】Drogon 的 Transaction::rollback() 无参数、无回调，
// 只是把一条 "rollback" 命令排进连接队列，由连接下一次 IO 空闲时下发。
// 因此它在任何地方都只表示"回滚已【请求】"，绝不能被当作"回滚已完成、
// 锁已释放"。本文件中所有 rollback 回调都按此语义使用。
// 涉及"必须等锁真的释放才能做"的场景，一律不使用事务内语句，而是另起独立连接。
//
// 不引入 Redis、不加额外调度服务、不由 Node 进程接管业务调度。
// =====================================================================
#pragma once

// SchedulerJob::runLocked 的 tx 参数类型是 drogon::orm::Transaction。该类型由
// <drogon/orm/DbClient.h> 定义——Drogon v1.9.11 不存在 drogon/orm/Transaction.h，
// 包含它会直接报致命错误：fatal error: drogon/orm/Transaction.h: No such file or directory。
#include <drogon/orm/DbClient.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usapp::scheduler {

// 一个可调度任务。阶段 0 只注册一个无副作用的心跳任务。
class SchedulerJob {
  public:
    virtual ~SchedulerJob() = default;

    // 稳定、跨实例一致的标识，见 LockKey.h。改它等于换锁：
    // 滚动升级期间新旧实例会各持一把锁并同时执行。要改就一次性全量重启。
    virtual std::string jobKey() const = 0;

    struct Result {
        bool ok = true;
        // 人类可读的补充说明。Scheduler 会把它包成 JSON 对象
        // ({"message": "..."}) 再写 jsonb，因此这里永远不需要是合法 JSON。
        std::string detail;
        // 提交后动作。绝不允许捕获或使用 Transaction / TxScope。
        std::function<void()> afterCommit;
    };

    // 持锁的 DB 工作。必须在 done() 之前把所有语句都发到 tx 上，
    // 且 done() 之后不得再使用 tx。
    virtual void runLocked(drogon::orm::Transaction &tx,
                          std::function<void(Result)> done) = 0;
};

class Scheduler {
  public:
    enum class TickOutcome { Completed, Skipped, Failed };

    struct TickReport {
        std::string jobKey;
        int64_t lockKey = 0;
        TickOutcome outcome = TickOutcome::Skipped;
        std::string detail;
    };

    static Scheduler &instance();

    // 幂等。tickIntervalMs 只影响自动循环，不影响 tickOnce()。
    void configure(bool enabled, int64_t tickIntervalMs, std::string instanceId);

    // 注册任务。jobKey 重复、为空或形状非法都抛异常（宁可启动失败）。
    void registerJob(std::shared_ptr<SchedulerJob> job);

    // 启动后台线程。未 configure() 时抛异常。重复调用无副作用。
    void start();
    // 请求停止并等待线程退出。可重复调用。
    void stop();

    bool running() const { return running_.load(); }
    bool enabled() const { return enabled_; }
    size_t jobCount() const;

    // 手动驱动一个完整 tick（测试、运维 dry-run 用）。
    // 任务按注册顺序【串行】执行：任意时刻最多持有一把锁。
    // 没有注册任务时同步调用 onComplete。
    using JobTickCallback = std::function<void(const TickReport &)>;
    void tickOnce(JobTickCallback onJob, std::function<void()> onComplete = nullptr);

    // 前一个 tick 尚未结束时，自动循环不会启动新的 tick（调度不重入）。
    bool tickInFlight() const { return tickInFlight_.load(); }

    static const char *outcomeToString(TickOutcome outcome);

  private:
    Scheduler() = default;

    // 串行 tick 的推进状态。定义在 .cc（嵌套类可访问 Scheduler 的私有成员）。
    struct TickChain;
    void tickStep(const std::shared_ptr<TickChain> &chain);

    void runJob(const std::shared_ptr<SchedulerJob> &job, JobTickCallback onJob,
                std::function<void()> onDone);
    void recordFailedTick(const std::string &jobKey, int64_t lockKey,
                          const std::string &instanceId, const std::string &detail,
                          std::function<void()> onDone);
    void threadMain();
    void sleepInterruptible(int64_t totalMs) const;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<SchedulerJob>> jobs_;
    std::string instanceId_ = "unconfigured";
    int64_t tickIntervalMs_ = 15000;
    bool enabled_ = false;
    bool configured_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> tickInFlight_{false};
    std::thread thread_;
};

}  // namespace usapp::scheduler
