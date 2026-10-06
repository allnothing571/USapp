// =====================================================================
// services/api/src/core/Crypto.h
//
// 1. 随机标识生成（CSPRNG）
// 2. 令牌散列（SHA-256 hex）—— 数据库只保存散列，绝不保存明文
// 3. 邮箱规范化与验证码生成
// 4. PBKDF2-SHA256 口令校验（管理员口令）
// 5. FNV-1a 64 位（调度锁键派生，跨实例一致）
// =====================================================================
#pragma once

#include <cstdint>
#include <string>

namespace usapp::crypto {

// 32 字节 CSPRNG 随机数的 hex 表示（64 字符）。失败即抛异常。
std::string randomHex(size_t bytes);

// 定长数字验证码（默认 6 位）。首位可为 0，逐字符均匀取样。
std::string randomNumericCode(int digits);

// 从给定字母表均匀取样生成定长字符串（拒绝采样，无模偏斜）。
// 用于邀请码等需要"人可抄写"的随机串：字母表应去掉易混字符。
// alphabet 为空或含重复字符、length<=0、length>128 时抛 std::runtime_error。
std::string randomFromAlphabet(const std::string &alphabet, size_t length);

// SHA-256 hex（小写，64 字符）。
std::string sha256Hex(const std::string &input);

// 恒定时间比较（长度不同直接返回 false，但仍走完定长时间比较）。
bool constantTimeEquals(const std::string &a, const std::string &b);

// 邮箱规范化：trim -> 小写 -> 收窄到 ASCII 范围内做形状校验。
// 返回 std::nullopt 表示不是合法邮箱。
// 注意："合法" 只做结构性校验，不解析 DNS，也不做本地部分去点（避免误合并账号）。
struct CanonicalEmail {
    std::string canonical;  // 全部小写、已 trim
    std::string display;    // 用户原始输入（trim 后），仅用于展示
};
bool canonicalizeEmail(const std::string &raw, CanonicalEmail &out);

// ---- FNV-1a 64 ----
// 调度锁键派生。必须与 database/tests/advisory_lock_test.ps1 中记录的
// 键一致：  lockKey = fnv1a64(KEY_NAMESPACE + ":" + jobKey)
// 使用无符号 64 位并按 bit pattern 转成 int64（PostgreSQL bigint 接受负数）。
uint64_t fnv1a64(const std::string &input);
int64_t advisoryLockKey(const std::string &keyNamespace, const std::string &jobKey);

// ---- PBKDF2 ----
// 口令散列格式（database/migrations/0005_admin_audit.sql 已固化）：
//   pbkdf2-sha256$<iterations>$<salt-base64>$<hash-base64>
// 用户不存在时也要执行一次等价开销的假校验，避免时序侧信道。
bool pbkdf2Verify(const std::string &password, const std::string &encoded);
std::string pbkdf2Hash(const std::string &password, int iterations);

// 校验固定格式串是否合法（不依赖 OpenSSL 的 base64 宽松行为）。
bool looksLikePbkdf2Hash(const std::string &encoded);

}  // namespace usapp::crypto
