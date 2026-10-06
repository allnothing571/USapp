// =====================================================================
// services/api/src/scheduler/LockKey.h
//
// 调度锁键。
//
// 这是阶段 0 调度里最容易悄悄出错的地方，因此把不变量写在最前面：
//
//   1. 同一个 job_key 在【所有实例】【任何时刻】必须算出同一个 64 位整数。
//      只要不一致，pg_try_advisory_xact_lock 就不会互斥，多实例会同时
//      执行同一任务 —— 而且不会报任何错，只会重复执行。
//   2. 因此键值【只】由 (keyNamespace, job_key) 经确定性散列得出：
//      不含时间、不含随机数、不含实例名、不含进程 id、不含内存地址。
//   3. 因此 job_key 的取值形状被严格限制为 ASCII 小写：不同实例的
//      locale、终端编码、Unicode 规范化形式都不得影响结果。
//
// 键值给 PostgreSQL 的 advisory lock 使用，落库时写入
// scheduler_ticks.lock_key（约束 <> 0），便于事后核对。
// =====================================================================
#pragma once

#include <cstdint>
#include <string>

namespace usapp::scheduler {

// job_key 的允许形状：[a-z0-9][a-z0-9._-]{0,62}
// 不合法抛 std::invalid_argument（附可读原因）。宁可启动即失败，
// 也不要带着一个跨实例不一致的锁键静默运行。
void validateJobKey(const std::string &jobKey);

// 校验键空间：非空、长度 1..64、不含空白与冒号（冒号是拼接分隔符）。
void validateKeyNamespace(const std::string &keyNamespace);

// 未配置时使用的键空间。与 .env.example 的 SCHEDULER_KEY_NAMESPACE 默认值一致。
const char *defaultKeyNamespace();

class LockKey {
  public:
    // 用 AppConfig 的 SCHEDULER_KEY_NAMESPACE 构造（正常路径）。
    static LockKey forJob(const std::string &jobKey);

    // 显式给出键空间（测试用：同一 job_key、不同键空间必须得到不同键）。
    static LockKey make(const std::string &keyNamespace, const std::string &jobKey);

    // 恒 > 0（已掩掉符号位，并把 0 映射为 1），因此满足
    // scheduler_ticks.lock_key <> 0，也永不与 -1 的哨兵行冲突。
    int64_t value() const { return value_; }

    // "<keyNamespace>:<jobKey>"，即散列的实际输入。日志用。
    const std::string &fullKey() const { return fullKey_; }
    const std::string &jobKey() const { return jobKey_; }

    std::string describe() const;

  private:
    LockKey(std::string fullKey, std::string jobKey, int64_t value)
        : fullKey_(std::move(fullKey)), jobKey_(std::move(jobKey)), value_(value) {}

    std::string fullKey_;
    std::string jobKey_;
    int64_t value_;
};

}  // namespace usapp::scheduler
