// =====================================================================
// services/api/src/scheduler/Scheduler.cc
// =====================================================================
#include "scheduler/Scheduler.h"

#include <drogon/drogon.h>
#include <json/json.h>

#include <chrono>
#include <stdexcept>
#include <utility>

#include "config/AppConfig.h"
#include "db/Db.h"
#include "scheduler/LockKey.h"

namespace usapp::scheduler {

// 串行推进一个 tick 的状态。作为 Scheduler 的嵌套类，它可以访问
// Scheduler 的私有成员（这正是 runJob/tickStep 保持私有的原因）。
struct Scheduler::TickChain {
    std::vector<std::shared_ptr<SchedulerJob>> jobs;
    JobTickCallback onJob;
    std::function<void()> onComplete;
    size_t idx = 0;
};

namespace {

constexpr int64_t kStopPollSliceMs = 100;

// 把普通字符串包成 JSON 对象。任务只需给一句人话，不需要自己拼 JSON。
std::string wrapDetailAsJson(const std::string &message) {
    Json::Value obj(Json::objectValue);
    obj["message"] = message;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, obj);
}

}  // namespace

Scheduler &Scheduler::instance() {
    static Scheduler scheduler;
    return scheduler;
}

const char *Scheduler::outcomeToString(TickOutcome outcome) {
    switch (outcome) {
        case TickOutcome::Completed:
            return "completed";
        case TickOutcome::Skipped:
            return "skipped";
        case TickOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

void Scheduler::configure(bool enabled, int64_t tickIntervalMs, std::string instanceId) {
    if (tickIntervalMs < 1000 || tickIntervalMs > 3600000) {
        throw std::invalid_argument("调度间隔必须在 1000..3600000 毫秒之间");
    }
    if (instanceId.empty()) {
        instanceId = "unconfigured";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    tickIntervalMs_ = tickIntervalMs;
    instanceId_ = std::move(instanceId);
    configured_ = true;
}

void Scheduler::registerJob(std::shared_ptr<SchedulerJob> job) {
    if (!job) {
        throw std::invalid_argument("调度任务指针不能为空");
    }
    const std::string key = job->jobKey();
    // 在这里而不是在持锁之后再校验：job_key 形状非法会得到一把跨实例
    // 不一致的锁，必须在启动阶段就炸掉。
    validateJobKey(key);

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &existing : jobs_) {
        if (existing->jobKey() == key) {
            throw std::invalid_argument("调度 job_key 重复注册: " + key);
        }
    }
    jobs_.push_back(std::move(job));
}

size_t Scheduler::jobCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

void Scheduler::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            throw std::runtime_error("Scheduler::start() 之前必须先 configure()");
        }
        if (!enabled_) {
            LOG_INFO << "调度未启用（SCHEDULER_ENABLED=false），不启动调度线程";
            return;
        }
        if (running_.load()) {
            return;
        }
        running_.store(true);
    }
    stopRequested_.store(false);
    thread_ = std::thread([this]() { threadMain(); });
}

void Scheduler::stop() {
    stopRequested_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

void Scheduler::sleepInterruptible(int64_t totalMs) const {
    int64_t slept = 0;
    while (slept < totalMs && !stopRequested_.load()) {
        const int64_t slice = (totalMs - slept) < kStopPollSliceMs ? (totalMs - slept) : kStopPollSliceMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
}

void Scheduler::threadMain() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_INFO << "调度线程启动 instance_id=" << instanceId_
                 << " tick_interval_ms=" << tickIntervalMs_ << " jobs=" << jobs_.size();
    }
    while (!stopRequested_.load()) {
        int64_t interval = 15000;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interval = tickIntervalMs_;
        }
        sleepInterruptible(interval);
        if (stopRequested_.load()) {
            break;
        }
        // 调度不重入：上一个 tick 没结束就不开新的。
        bool expected = false;
        if (!tickInFlight_.compare_exchange_strong(expected, true)) {
            LOG_WARN << "上一个调度 tick 尚未结束，本次跳过";
            continue;
        }
        tickOnce(
            [](const TickReport &report) {
                LOG_INFO << "调度 tick job=" << report.jobKey
                         << " outcome=" << outcomeToString(report.outcome)
                         << " lock_key=" << report.lockKey;
            },
            [this]() { tickInFlight_.store(false); });
    }
    running_.store(false);
    LOG_INFO << "调度线程退出";
}

void Scheduler::tickOnce(JobTickCallback onJob, std::function<void()> onComplete) {
    auto chain = std::make_shared<TickChain>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chain->jobs = jobs_;
    }
    chain->onJob = std::move(onJob);
    chain->onComplete = std::move(onComplete);
    if (chain->jobs.empty()) {
        if (chain->onComplete) {
            chain->onComplete();
        }
        return;
    }
    tickStep(chain);
}

void Scheduler::tickStep(const std::shared_ptr<TickChain> &chain) {
    if (chain->idx >= chain->jobs.size()) {
        auto onComplete = chain->onComplete;
        if (onComplete) {
            onComplete();
        }
        return;
    }
    auto job = chain->jobs[chain->idx++];
    // 任务串行：任意时刻最多持有一把 advisory lock。
    // this 是函数内静态单例，生命周期到进程结束，捕获裸指针是安全的；
    // 从而不产生 shared_ptr 自引用环。
    runJob(job, chain->onJob, [this, chain]() { tickStep(chain); });
}

void Scheduler::runJob(const std::shared_ptr<SchedulerJob> &job, JobTickCallback onJob,
                       std::function<void()> onDone) {
    const std::string jobKey = job->jobKey();

    int64_t lockKey = 0;
    try {
        lockKey = LockKey::forJob(jobKey).value();
    } catch (const std::exception &e) {
        LOG_ERROR << "无法为调度任务计算锁键: " << e.what();
        if (onJob) {
            TickReport report;
            report.jobKey = jobKey;
            report.lockKey = 0;
            report.outcome = TickOutcome::Failed;
            report.detail = e.what();
            onJob(report);
        }
        if (onDone) {
            onDone();
        }
        return;
    }

    // 快照实例标识：configure() 可能在别的线程改写它，持锁事务里不要再去读。
    std::string instanceId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        instanceId = instanceId_;
    }
    const int64_t key = lockKey;

    auto finish = [onJob, jobKey, key, onDone](TickOutcome outcome, const std::string &detail) {
        if (onJob) {
            TickReport report;
            report.jobKey = jobKey;
            report.lockKey = key;
            report.outcome = outcome;
            report.detail = detail;
            onJob(report);
        }
        if (onDone) {
            onDone();
        }
    };

    // Non-blocking: from the second job on, this runs inside the previous job's commit or rollback callback, on a
    // database event-loop thread that must return first.
    db::TxScope::beginAsync(
        [this, job, jobKey, key, instanceId, finish](std::shared_ptr<db::TxScope> scope) {
            // 第 1 步：在这一条连接、这一个事务上尝试取锁。
            scope->tx().execSqlAsync(
                "SELECT pg_try_advisory_xact_lock($1::bigint) AS acquired",
                [this, job, jobKey, key, instanceId, finish, scope](const drogon::orm::Result &r) {
                    bool acquired = false;
                    if (!r.empty()) {
                        acquired = r[0]["acquired"].as<bool>();
                    }
                    if (!acquired) {
                        // 没拿到锁：立刻请求回滚，并且【不写任何行】。
                        // scheduler_ticks 里看不到 skipped —— 被跳过不落库，
                        // 这正是「锁与 DB 操作同事务同连接」的可验证证据。
                        // 注意：下面这个回调的语义是"回滚已【请求】"，
                        // 不是"回滚已完成、锁已释放"。Drogon 的 rollback() 只把
                        // rollback 命令排进连接队列，服务器端真正执行完才释放锁。
                        // 本例不需要等锁释放——没拿到锁，本来就没锁。
                        LOG_INFO << "任务 " << jobKey << " 未取得锁（另一实例在跑），本次跳过";
                        scope->rollback([finish]() { finish(TickOutcome::Skipped, "另一个实例持有锁"); });
                        return;
                    }

                    // 第 2 步：持锁执行任务的 DB 工作。任务只能使用这个 tx。
                    job->runLocked(scope->tx(),
                                   [this, jobKey, key, instanceId, finish, scope](
                                       SchedulerJob::Result result) {
                                       if (!result.ok) {
                                           const std::string detail = result.detail.empty()
                                                                          ? "任务报告失败"
                                                                          : result.detail;
                                           // 下面的回调是"回滚已【请求】"，不是"回滚已完成"。
                                           // 当前任务已经持有锁，锁在服务器执行回滚后才释放。
                                           // recordFailedTick 使用连接池中的独立语句记录失败，
                                           // 不重用已结束的 scope，也不据此宣称回滚已完成。
                                           scope->rollback([this, jobKey, key, instanceId, detail,
                                                            finish]() {
                                               // 回滚已请求：失败行由独立语句写入。
                                               recordFailedTick(jobKey, key, instanceId, detail,
                                                                [finish, detail]() {
                                                                    finish(TickOutcome::Failed,
                                                                           detail);
                                                                });
                                           });
                                           return;
                                       }

                                       const std::string detailJson = wrapDetailAsJson(result.detail);
                                       // 第 3 步：同一事务内记录 tick。
                                       scope->tx().execSqlAsync(
                                           "INSERT INTO scheduler_ticks "
                                           "(job_key, lock_key, instance_id, outcome, detail) "
                                           "VALUES ($1, $2, $3, 'completed', $4::jsonb)",
                                           [scope, result, finish](const drogon::orm::Result &) {
                                               // 第 4 步：提交。
                                               scope->commit([result, finish]() {
                                                   // 第 5 步：锁已释放，才执行网络/IO 动作。
                                                   if (result.afterCommit) {
                                                       try {
                                                           result.afterCommit();
                                                       } catch (const std::exception &e) {
                                                           LOG_ERROR << "调度任务的提交后动作抛出异常: "
                                                                     << e.what();
                                                       }
                                                   }
                                                   finish(TickOutcome::Completed, "ok");
                                               });
                                           },
                                           [scope](const drogon::orm::DrogonDbException &e) {
                                               scope->fail(db::describePgError(
                                                   "写入 scheduler_ticks 失败", e.base()));
                                           },
                                           jobKey,
                                           key,
                                           instanceId,
                                           detailJson);
                                   });
                },
                [scope](const drogon::orm::DrogonDbException &e) {
                    scope->fail(db::describePgError("获取调度 advisory lock 失败", e.base()));
                },
                key);
        },
        [this, jobKey, key, instanceId, finish](const std::string &err) {
            LOG_ERROR << "调度任务 " << jobKey << " 的事务失败: " << err;
            // 事务已【请求回滚】（commit 失败或 begin 失败时不会有事务在跑）。
            // 锁的释放发生在服务器端执行 rollback 之后，本回调不做该断言。
            // 失败行用独立语句写入，与本事务无关，因此可以立即写。
            recordFailedTick(jobKey, key, instanceId, err,
                             [finish, err]() { finish(TickOutcome::Failed, err); });
        });
}

void Scheduler::recordFailedTick(const std::string &jobKey, int64_t lockKey,
                                 const std::string &instanceId, const std::string &detail,
                                 std::function<void()> onDone) {
    auto done = [onDone]() {
        if (onDone) {
            onDone();
        }
    };
    if (!db::Pool::instance().ready()) {
        LOG_ERROR << "连接池未就绪，无法记录失败 tick: " << jobKey;
        done();
        return;
    }
    const std::string detailJson = wrapDetailAsJson(detail);
    try {
        db::Pool::instance().client()->execSqlAsync(
            "INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome, detail) "
            "VALUES ($1, $2, $3, 'failed', $4::jsonb)",
            [done, jobKey](const drogon::orm::Result &) {
                LOG_WARN << "调度任务 " << jobKey << " 失败已记录";
                done();
            },
            [done, jobKey](const drogon::orm::DrogonDbException &e) {
                LOG_ERROR << "记录失败 tick 也失败了 (" << jobKey << "): " << e.base().what();
                done();
            },
            jobKey,
            lockKey,
            instanceId,
            detailJson);
    } catch (const std::exception &e) {
        LOG_ERROR << "记录失败 tick 抛出异常: " << e.what();
        done();
    }
}

}  // namespace usapp::scheduler
