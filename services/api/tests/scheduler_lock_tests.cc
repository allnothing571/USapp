// =====================================================================
// services/api/tests/scheduler_lock_tests.cc
//
// 要求 6：调度锁竞争与回滚释放。
//
// 两部分：
//   A) 运行时单元测试 —— usapp::scheduler::LockKey 的纯函数行为
//      （validateJobKey / validateKeyNamespace / make / value / describe）
//      以及 crypto::fnv1a64 的独立复算。全部不需要数据库。
//   B) 静态文本断言 —— 读 services/api/src/scheduler/Scheduler.cc 的**文本**，
//      断言"取锁 → 写 scheduler_ticks → commit"发生在同一个 TxScope 内、
//      未获锁分支 rollback 且不写行、afterCommit 在 commit 之后。
//
// !!! 重要声明 !!!
// B) 是**文本断言（textual/static assertion）**，不是运行时验证。
// 它读的是源码字符串，证明的是"源码里这几步写在同一段事务代码里"，
// 不能证明真实 PostgreSQL 下 pg_try_advisory_xact_lock 的行为。
// 没有数据库连接时这是能做到的最强代理证据；真实互斥必须在
// 集成测试（integration_tests.cc，需要 USAPP_TEST_DSN）里验证。
// 每一处这样的断言前都调用 usapptest::noteStaticAssertion()，输出里可见。
// =====================================================================

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "RepoText.h"

#include "core/Crypto.h"
#include "scheduler/LockKey.h"

namespace {

using usapp::scheduler::LockKey;

// 独立重实现的 FNV-1a 64：用来交叉验证 crypto::fnv1a64 的常量/字节序处理，
// 而不是用它自己的结果自证。
constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kPrime = 1099511628211ULL;

uint64_t refFnv1a64(const std::string &input) {
    uint64_t hash = kOffsetBasis;
    for (unsigned char c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kPrime;
    }
    return hash;
}

int64_t refMaskedValue(const std::string &keyNamespace, const std::string &jobKey) {
    int64_t value = static_cast<int64_t>(refFnv1a64(keyNamespace + ":" + jobKey) & 0x7FFFFFFFFFFFFFFFULL);
    if (value == 0) {
        value = 1;
    }
    return value;
}

// 合法 / 非法 job_key 用例表。
const std::vector<std::string> kLegalJobKeys = {
    "a",
    "0",
    "z",
    "heartbeat",
    "job-1",
    "job.1",
    "job_1",
    "a.b-c_d",
    "bootstrap",            // 注意：没有下划线前缀是合法的
    "scheduler-tick-0001",
    std::string(63, 'a'),   // 上界：63 字节
    "a" + std::string(62, '9'),
};

const std::vector<std::string> kIllegalJobKeys = {
    "",                          // 空
    std::string(64, 'a'),        // 超长（64 > 63）
    "__bootstrap__",             // 保留值：以下划线开头
    "_bootstrap",                // 下划线开头
    "-bootstrap",                // 连字符开头
    ".bootstrap",                // 点开头
    "Bootstrap",                 // 大写
    "BOOTSTRAP",                 // 全大写
    "job key",                   // 空格
    "job\tkey",                  // 制表符
    "job\nkey",                  // 换行
    "job/key",                   // 斜杠
    "job:key",                   // 冒号（拼接分隔符，必须禁止）
    "job+key",                   // 加号
    "job;key",                   // 分号
    "j\u00f6b",                  // UTF-8 非 ASCII（é/ö 之类，逐字节 > 0x7e）
    "\xe4\xbb\xbb\xe5\x8a\xa1",  // UTF-8 中文
};

const std::vector<std::string> kLegalNamespaces = {
    "usapp.scheduler",
    "a",
    "A-Z_9.x",
    "usapp",
    "!",
    "~",                    // 0x7e 上界
    std::string(64, 'a'),   // 上界：64 字节
};

const std::vector<std::string> kIllegalNamespaces = {
    "",                       // 空
    std::string(65, 'a'),     // 超长
    "ns:name",                // 冒号
    "ns name",                // 空格
    "ns\tname",               // 制表符
    "ns\nname",               // 换行
    "\x7f",                   // DEL（0x7f > 0x7e）
    "\x01",                   // 控制字符（< 0x21）
    "\xc3\xa9",               // 非 ASCII
    "usapp\xe8\xb0\x83",      // 非 ASCII 尾巴
};

}  // namespace

int main() {
    TEST_CASE("要求6 LockKey：validateJobKey 接受合法形状、拒绝非法形状") {
        for (const auto &key : kLegalJobKeys) {
            bool threw = false;
            try {
                usapp::scheduler::validateJobKey(key);
            } catch (const std::invalid_argument &) {
                threw = true;
            } catch (...) {
                threw = true;
            }
            if (threw) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("validateJobKey 不应拒绝合法 job_key: ") +
                                    usapptest::visualize(key));
            }
            ++usapptest::totalChecks();
        }

        // 逐一验证非法输入抛异常。
        for (const auto &key : kIllegalJobKeys) {
            bool threw = false;
            try {
                usapp::scheduler::validateJobKey(key);
            } catch (const std::invalid_argument &) {
                threw = true;
            } catch (...) {
                threw = true;
            }
            if (!threw) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("validateJobKey 必须拒绝非法 job_key: ") +
                                    usapptest::visualize(key));
            }
            ++usapptest::totalChecks();
        }

        // 显式点名几条要求的用例。
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey(""), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey(std::string(64, 'a')), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("__bootstrap__"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("_bootstrap"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("-bootstrap"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey(".bootstrap"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("Bootstrap"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("job key"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("job:key"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("\xe4\xbb\xbb\xe5\x8a\xa1"), std::invalid_argument);

        // 保留值 __bootstrap__ 必须被拒绝 —— 这是显式要求。
        CHECK_THROWS_AS(usapp::scheduler::validateJobKey("__bootstrap__"), std::invalid_argument);
    }

    TEST_CASE("要求6 LockKey：validateKeyNamespace 接受可打印 ASCII、拒绝冒号/空白/非 ASCII") {
        for (const auto &ns : kLegalNamespaces) {
            bool threw = false;
            try {
                usapp::scheduler::validateKeyNamespace(ns);
            } catch (...) {
                threw = true;
            }
            if (threw) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("validateKeyNamespace 不应拒绝合法键空间: ") +
                                    usapptest::visualize(ns));
            }
            ++usapptest::totalChecks();
        }

        for (const auto &ns : kIllegalNamespaces) {
            bool threw = false;
            try {
                usapp::scheduler::validateKeyNamespace(ns);
            } catch (const std::invalid_argument &) {
                threw = true;
            } catch (...) {
                threw = true;
            }
            if (!threw) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("validateKeyNamespace 必须拒绝非法键空间: ") +
                                    usapptest::visualize(ns));
            }
            ++usapptest::totalChecks();
        }

        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace(""), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace(std::string(65, 'a')),
                        std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace("ns:name"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace("ns name"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace("ns\tname"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace("\x7f"), std::invalid_argument);
        CHECK_THROWS_AS(usapp::scheduler::validateKeyNamespace("\xc3\xa9"), std::invalid_argument);

        // 默认键空间与 .env.example 的 SCHEDULER_KEY_NAMESPACE 默认值一致。
        CHECK_EQ(std::string(usapp::scheduler::defaultKeyNamespace()), std::string("usapp.scheduler"));
        CHECK_NOTHROW(usapp::scheduler::validateKeyNamespace(usapp::scheduler::defaultKeyNamespace()));
    }

    TEST_CASE("要求6 LockKey：且只由 (keyNamespace, jobKey) 决定；value() 恒 > 0 且 != 0") {
        const std::string ns = "usapp.scheduler";
        const std::string job = "heartbeat";

        const LockKey a = LockKey::make(ns, job);
        const LockKey b = LockKey::make(ns, job);

        // fullKey 形状：恰好 "<ns>:<job>"，且可以拆回来。
        CHECK_EQ(a.fullKey(), ns + ":" + job);
        CHECK_EQ(a.jobKey(), job);
        CHECK_EQ(a.fullKey().find(':'), ns.size());

        // 稳定性：同一输入任何时刻都是同一个值（跨实例一致的前提）。
        CHECK_EQ(a.value(), b.value());
        CHECK_EQ(a.fullKey(), b.fullKey());

        // value() 恒 > 0（掩掉符号位、0 映射为 1）：满足 scheduler_ticks.lock_key <> 0，
        // 也永不与 -1 的哨兵行冲突。
        CHECK(a.value() > 0);
        CHECK_NE(a.value(), static_cast<int64_t>(0));
        CHECK_NE(a.value(), static_cast<int64_t>(-1));

        // 与独立复算的掩码值一致。
        CHECK_EQ(a.value(), refMaskedValue(ns, job));

        // 不同 jobKey -> 不同键值；同 jobKey 不同 keyNamespace -> 不同键值。
        const LockKey other = LockKey::make(ns, "scheduler-tick-0001");
        CHECK_NE(a.value(), other.value());
        const LockKey otherNs = LockKey::make("usapp.scheduler.other", job);
        CHECK_NE(a.value(), otherNs.value());
        CHECK_NE(a.fullKey(), otherNs.fullKey());

        // describe() 形状。
        CHECK_EQ(a.describe(), a.fullKey() + " -> " + std::to_string(a.value()));
    }

    TEST_CASE("要求6 LockKey：value() 对大量键恒 > 0（掩掉符号位的回归证据）") {
        // crypto::fnv1a64 返回原始 64 位 bit pattern；advisoryLockKey() 允许负数。
        // LockKey::value() 必须把它掩成正值。这里先找到一个"原始值为负"的输入，
        // 再断言 LockKey 仍然是正数——这是真正能失败的回归断言，不是恒真式。
        std::string negativeSample;
        std::string negativeSampleNs = "usapp.scheduler";
        int64_t rawNegative = 0;
        for (int i = 0; i < 5000 && negativeSample.empty(); ++i) {
            const std::string job = "job-" + std::to_string(i);
            const int64_t raw = usapp::crypto::advisoryLockKey(negativeSampleNs, job);
            if (raw < 0) {
                negativeSample = job;
                rawNegative = raw;
            }
        }
        CHECK(!negativeSample.empty());
        if (!negativeSample.empty()) {
            const LockKey k = LockKey::make(negativeSampleNs, negativeSample);
            CHECK(rawNegative < 0);           // 原始 bit pattern 确实是负的
            CHECK(k.value() > 0);             // 掩码后必须是正的
            CHECK_NE(k.value(), static_cast<int64_t>(-1));
            CHECK_EQ(k.value(),
                     static_cast<int64_t>(usapp::crypto::fnv1a64(k.fullKey()) & 0x7FFFFFFFFFFFFFFFULL));
            CHECK_NE(static_cast<int64_t>(rawNegative), k.value());
            usapptest::note("crypto::advisoryLockKey 保留负数 bit pattern，而 LockKey::value() 掩成正值："
                            "两者对同一输入可以符号不同（Crypto.cc:266 vs LockKey.cc:78）");
        }

        // 批量抽样：任何输入都不得产生 <= 0 的键。
        int nonPositive = 0;
        for (int i = 0; i < 2000; ++i) {
            const std::string job = "tick-" + std::to_string(i);
            const LockKey k = LockKey::make("usapp.scheduler", job);
            if (k.value() <= 0) {
                ++nonPositive;
            }
            if (k.value() != refMaskedValue("usapp.scheduler", job)) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("LockKey::value() 与独立复算不一致: ") + job);
                break;
            }
        }
        CHECK_EQ(nonPositive, 0);
    }

    TEST_CASE("要求6 crypto::fnv1a64：与独立重实现逐字节一致") {
        const std::vector<std::string> samples = {
            "",
            "a",
            "usapp.scheduler:heartbeat",
            "usapp.scheduler:job-1",
            "\xe4\xbb\xbb\xe5\x8a\xa1",              // UTF-8 中文
            std::string("a\0b", 3),                  // 含嵌入 NUL，必须按字节走
            std::string(200, 'x'),
            "\xff\xfe\x00\x01",                      // 高位字节
        };
        for (const auto &s : samples) {
            CHECK_EQ(usapp::crypto::fnv1a64(s), refFnv1a64(s));
        }
        // 空串必须是 FNV offset basis。
        CHECK_EQ(usapp::crypto::fnv1a64(""), kOffsetBasis);
        // advisoryLockKey 就是 fnv1a64(ns + ":" + job) 的 bit pattern 语义。
        CHECK_EQ(usapp::crypto::advisoryLockKey("usapp.scheduler", "heartbeat"),
                 static_cast<int64_t>(usapp::crypto::fnv1a64("usapp.scheduler:heartbeat")));
    }

    TEST_CASE("要求6 静态断言：Scheduler.cc 里取锁/写 tick/commit 在同一事务范围内") {
        usapptest::noteStaticAssertion(
            "读 services/api/src/scheduler/Scheduler.cc 的文本，断言事务顺序。"
            "这是文本断言，不是运行时验证：它不能替代真实 PostgreSQL 下的互斥测试。");

        const std::string path = "services/api/src/scheduler/Scheduler.cc";
        const std::string text = usapptest::readRepoFile(path);
        if (text.empty()) {
            usapptest::fail(__FILE__, __LINE__,
                            std::string("读不到源码文本: ") + path + " (" +
                                usapptest::repoRootHint() + ")");
            return usapptest::finish();
        }
        const std::string flat = usapptest::collapseWs(text);

        // 1) 取锁用的正是事务级 advisory lock（xact 版本随事务结束自动释放）。
        CHECK_CONTAINS(flat, "pg_try_advisory_xact_lock");
        CHECK_CONTAINS(flat, "pg_try_advisory_xact_lock($1::bigint) AS acquired");
        CHECK_CONTAINS(flat, "runJob");

        // 2) 整个过程被一个 db::TxScope 包住。
        CHECK_CONTAINS(flat, "db::TxScope::begin(");

        // 3) 完成后写 scheduler_ticks（outcome='completed'）。
        CHECK_CONTAINS(flat, "INSERT INTO scheduler_ticks");
        CHECK_CONTAINS(flat, "'completed'");
        CHECK_CONTAINS(flat, "VALUES ($1, $2, $3, 'completed', $4::jsonb)");

        // 4) 顺序：TxScope 开始 -> 取锁 -> 写 tick -> commit -> afterCommit。
        CHECK_ORDER(flat, "db::TxScope::begin(", "pg_try_advisory_xact_lock");
        CHECK_ORDER(flat, "pg_try_advisory_xact_lock", "INSERT INTO scheduler_ticks");
        CHECK_ORDER(flat, "INSERT INTO scheduler_ticks", "scope->commit(");
        CHECK_ORDER(flat, "scope->commit(", "result.afterCommit()");

        // 5) afterCommit 的语义注释（锁释放后才做网络/IO）。
        CHECK_CONTAINS(flat, "锁已释放，才执行网络/IO 动作");

        // 6) 未获锁分支：rollback（释放连接）且不写任何行，且在写 tick 之前。
        CHECK_CONTAINS(flat, "scope->rollback(");
        CHECK_CONTAINS(flat, "未取得锁");
        CHECK_CONTAINS(flat, "另一个实例持有锁");
        CHECK_ORDER(flat, "scope->rollback(", "INSERT INTO scheduler_ticks");
        CHECK_ORDER(flat, "scope->rollback(", "scope->commit(");

        const std::string skipBranch = usapptest::sliceFromTo(text, "没拿到锁", "job->runLocked");
        CHECK(!skipBranch.empty());
        CHECK_CONTAINS(skipBranch, "rollback");
        CHECK_CONTAINS(skipBranch, "Skipped");
        CHECK_NOT_CONTAINS(skipBranch, "INSERT INTO scheduler_ticks");
        CHECK_CONTAINS(skipBranch, "不写任何行");

        // 7) 失败路径：先 rollback 释放锁，再用独立语句补写 failed 行；绝不在事务里写。
        CHECK_CONTAINS(flat, "recordFailedTick");
        CHECK_CONTAINS(flat, "'failed'");
        CHECK_ORDER(flat, "scope->rollback(", "recordFailedTick");
        // 从函数定义处切到文件末尾：失败行由 Pool::instance().client() 独立写入，
        // 完全不在 TxScope 之内（切片里不得出现 TxScope）。
        const std::string failedInsert =
            usapptest::sliceFromTo(text, "void Scheduler::recordFailedTick", "\x01-no-such-marker-");
        CHECK_CONTAINS(failedInsert, "INSERT INTO scheduler_ticks");
        CHECK_CONTAINS(failedInsert, "'failed'");
        CHECK_NOT_CONTAINS(failedInsert, "TxScope");

        // 8) 未就绪的池要被显式挡住（recordFailedTick 内部）。
        CHECK_CONTAINS(flat, "db::Pool::instance().ready()");
    }

    return usapptest::finish();
}
