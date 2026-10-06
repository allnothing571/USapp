// =====================================================================
// services/api/src/scheduler/LockKey.cc
// =====================================================================
#include "scheduler/LockKey.h"

#include <cctype>
#include <stdexcept>

#include "config/AppConfig.h"
#include "core/Crypto.h"

namespace usapp::scheduler {

namespace {

bool isLowerAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isJobKeyChar(char c) {
    return isLowerAlnum(c) || c == '.' || c == '_' || c == '-';
}

}  // namespace

const char *defaultKeyNamespace() { return "usapp.scheduler"; }

void validateJobKey(const std::string &jobKey) {
    if (jobKey.empty()) {
        throw std::invalid_argument("调度 job_key 不能为空");
    }
    if (jobKey.size() > 63) {
        throw std::invalid_argument("调度 job_key 过长（最多 63 字节）: " + jobKey);
    }
    for (char c : jobKey) {
        if (!isJobKeyChar(c)) {
            throw std::invalid_argument(
                "调度 job_key 只允许 ASCII 小写 [a-z0-9._-]（跨实例必须逐字节一致）: " + jobKey);
        }
    }
    if (!isLowerAlnum(jobKey.front())) {
        throw std::invalid_argument("调度 job_key 必须以小写字母或数字开头（既有的 __bootstrap__ 是保留值）: " +
                                   jobKey);
    }
}

void validateKeyNamespace(const std::string &keyNamespace) {
    if (keyNamespace.empty()) {
        throw std::invalid_argument("调度键空间不能为空");
    }
    if (keyNamespace.size() > 64) {
        throw std::invalid_argument("调度键空间过长（最多 64 字节）: " + keyNamespace);
    }
    for (char c : keyNamespace) {
        if (c == ':') {
            throw std::invalid_argument("调度键空间不能含冒号（冒号是拼接分隔符）: " + keyNamespace);
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) != 0 || uc < 0x21 || uc > 0x7e) {
            throw std::invalid_argument("调度键空间只能是可打印 ASCII 且不含空白: " + keyNamespace);
        }
    }
}

LockKey LockKey::forJob(const std::string &jobKey) {
    // AppConfig 在 main 里已经 load() 过；这里不吞异常：拿不到配置就没有可靠的锁。
    return make(AppConfig::instance().schedulerKeyNamespace, jobKey);
}

LockKey LockKey::make(const std::string &keyNamespace, const std::string &jobKey) {
    validateKeyNamespace(keyNamespace);
    validateJobKey(jobKey);

    const std::string fullKey = keyNamespace + ":" + jobKey;
    const uint64_t hashed = crypto::fnv1a64(fullKey);

    // 掩掉最高位：键值恒为正，C++17 下不存在实现定义的负数转换。
    int64_t value = static_cast<int64_t>(hashed & 0x7FFFFFFFFFFFFFFFULL);
    if (value == 0) {
        // 0 会与 scheduler_ticks.lock_key <> 0 冲突。映射为 1 仍然是确定性的。
        value = 1;
    }
    return LockKey(fullKey, jobKey, value);
}

std::string LockKey::describe() const {
    return fullKey_ + " -> " + std::to_string(value_);
}

}  // namespace usapp::scheduler
