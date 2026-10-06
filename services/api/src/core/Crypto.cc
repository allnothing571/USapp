// =====================================================================
// services/api/src/core/Crypto.cc
// =====================================================================
#include "core/Crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace usapp::crypto {

namespace {

constexpr char kHex[] = "0123456789abcdef";

std::string toHex(const unsigned char *data, size_t len) {
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

// 严格的 base64 解码（拒绝非字母表字符、拒绝长度非 4 的倍数）。
bool base64Decode(const std::string &in, std::vector<unsigned char> &out) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::array<int, 256> table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const std::string a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < a.size(); ++i) {
            t[static_cast<unsigned char>(a[i])] = static_cast<int>(i);
        }
        return t;
    }();
    (void)alphabet;

    if (in.empty() || in.size() % 4 != 0) {
        return false;
    }
    out.clear();
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int vals[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            const unsigned char c = static_cast<unsigned char>(in[i + j]);
            if (in[i + j] == '=') {
                if (j < 2 || i + 4 != in.size()) {
                    return false;
                }
                vals[j] = 0;
                ++pad;
            } else {
                vals[j] = table[c];
                if (vals[j] < 0 || pad > 0) {
                    return false;
                }
            }
        }
        const uint32_t triple = (static_cast<uint32_t>(vals[0]) << 18) |
                                (static_cast<uint32_t>(vals[1]) << 12) |
                                (static_cast<uint32_t>(vals[2]) << 6) |
                                static_cast<uint32_t>(vals[3]);
        out.push_back(static_cast<unsigned char>((triple >> 16) & 0xFF));
        if (pad < 2) {
            out.push_back(static_cast<unsigned char>((triple >> 8) & 0xFF));
        }
        if (pad < 1) {
            out.push_back(static_cast<unsigned char>(triple & 0xFF));
        }
    }
    return true;
}

std::string base64Encode(const unsigned char *data, size_t len) {
    static const char *a =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t t = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(a[(t >> 18) & 0x3F]);
        out.push_back(a[(t >> 12) & 0x3F]);
        out.push_back(a[(t >> 6) & 0x3F]);
        out.push_back(a[t & 0x3F]);
    }
    if (i + 1 == len) {
        const uint32_t t = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(a[(t >> 18) & 0x3F]);
        out.push_back(a[(t >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        const uint32_t t = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(a[(t >> 18) & 0x3F]);
        out.push_back(a[(t >> 12) & 0x3F]);
        out.push_back(a[(t >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string trimCopy(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string randomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("CSPRNG 失败：无法生成随机字节");
    }
    return toHex(buf.data(), buf.size());
}

std::string randomNumericCode(int digits) {
    if (digits <= 0 || digits > 12) {
        throw std::runtime_error("验证码位数不合法");
    }
    std::string out;
    out.reserve(static_cast<size_t>(digits));
    for (int i = 0; i < digits; ++i) {
        unsigned char b = 0;
        do {
            if (RAND_bytes(&b, 1) != 1) {
                throw std::runtime_error("CSPRNG 失败：无法生成验证码");
            }
            // 拒绝采样，避免 256 % 10 造成的偏斜
        } while (b >= 250);
        out.push_back(static_cast<char>('0' + (b % 10)));
    }
    return out;
}

std::string randomFromAlphabet(const std::string &alphabet, size_t length) {
    if (alphabet.empty() || length == 0 || length > 128) {
        throw std::runtime_error("随机串参数不合法");
    }
    {
        bool seen[256] = {false};
        for (unsigned char c : alphabet) {
            if (seen[c]) {
                throw std::runtime_error("随机串字母表存在重复字符");
            }
            seen[c] = true;
        }
    }
    const size_t n = alphabet.size();
    // 拒绝采样上界：只接受 [0, limit) 内的字节，使每个字母表下标等概率。
    const unsigned int limit = 256u - (256u % static_cast<unsigned int>(n));
    std::string out;
    out.reserve(length);
    while (out.size() < length) {
        unsigned char b = 0;
        do {
            if (RAND_bytes(&b, 1) != 1) {
                throw std::runtime_error("CSPRNG 失败：无法生成随机串");
            }
        } while (b >= limit);
        out.push_back(alphabet[b % n]);
    }
    return out;
}

std::string sha256Hex(const std::string &input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
    return toHex(digest, SHA256_DIGEST_LENGTH);
}

bool constantTimeEquals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool canonicalizeEmail(const std::string &raw, CanonicalEmail &out) {
    const auto trimmed = trimCopy(raw);
    if (trimmed.empty() || trimmed.size() > 254) {
        return false;
    }
    std::string lower;
    lower.reserve(trimmed.size());
    for (char ch : trimmed) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    const auto at = lower.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= lower.size()) {
        return false;
    }
    if (lower.find('@', at + 1) != std::string::npos) {
        return false;
    }
    const std::string local = lower.substr(0, at);
    const std::string domain = lower.substr(at + 1);

    for (char ch : local) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' ||
                        ch == '_' || ch == '%' || ch == '+' || ch == '-' || ch == '!';
        if (!ok) {
            return false;
        }
    }
    if (local.front() == '.' || local.back() == '.') {
        return false;
    }
    if (domain.find('.') == std::string::npos) {
        return false;
    }
    for (char ch : domain) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    if (domain.front() == '.' || domain.back() == '.' || domain.front() == '-' ||
        domain.back() == '-') {
        return false;
    }

    out.canonical = lower;
    out.display = trimmed;
    return true;
}

uint64_t fnv1a64(const std::string &input) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffsetBasis;
    for (unsigned char c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kPrime;
    }
    // 与 PostgreSQL bigint 互操作时保留负数（bit pattern 语义）
    return hash;
}

int64_t advisoryLockKey(const std::string &keyNamespace, const std::string &jobKey) {
    return static_cast<int64_t>(fnv1a64(keyNamespace + ":" + jobKey));
}

namespace {

constexpr const char *kPbkdf2Prefix = "pbkdf2-sha256$";
constexpr int kDefaultIterations = 600000;
constexpr int kMaxIterations = 4000000;

bool pbkdf2Raw(const std::string &password,
               const std::vector<unsigned char> &salt,
               int iterations,
               std::vector<unsigned char> &out) {
    out.assign(32, 0);
    if (PKCS5_PBKDF2_HMAC(password.data(),
                          static_cast<int>(password.size()),
                          salt.data(),
                          static_cast<int>(salt.size()),
                          iterations,
                          EVP_sha256(),
                          static_cast<int>(out.size()),
                          out.data()) != 1) {
        return false;
    }
    return true;
}

bool parseEncoded(const std::string &encoded,
                  int &iterations,
                  std::vector<unsigned char> &salt,
                  std::vector<unsigned char> &hash) {
    const std::string prefix(kPbkdf2Prefix);
    if (encoded.rfind(prefix, 0) != 0) {
        return false;
    }
    const auto rest = encoded.substr(prefix.size());
    const auto first = rest.find('$');
    if (first == std::string::npos) {
        return false;
    }
    const auto second = rest.find('$', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    const std::string iterStr = rest.substr(0, first);
    const std::string saltB64 = rest.substr(first + 1, second - first - 1);
    const std::string hashB64 = rest.substr(second + 1);
    if (iterStr.empty() || iterStr.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        iterations = std::stoi(iterStr);
    } catch (const std::exception &) {
        return false;
    }
    if (iterations < 10000 || iterations > kMaxIterations) {
        return false;
    }
    return base64Decode(saltB64, salt) && base64Decode(hashB64, hash) && !salt.empty() &&
           !hash.empty();
}

}  // namespace

bool looksLikePbkdf2Hash(const std::string &encoded) {
    int iterations = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
    return parseEncoded(encoded, iterations, salt, hash);
}

bool pbkdf2Verify(const std::string &password, const std::string &encoded) {
    int iterations = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!parseEncoded(encoded, iterations, salt, expected)) {
        // 格式不合法也走一次等价开销，避免格式差异导致的时序区分。
        std::vector<unsigned char> throwaway;
        static const std::vector<unsigned char> kDummySalt(16, 0x5A);
        (void)pbkdf2Raw(password, kDummySalt, kDefaultIterations, throwaway);
        return false;
    }
    std::vector<unsigned char> actual;
    if (!pbkdf2Raw(password, salt, iterations, actual)) {
        return false;
    }
    if (actual.size() != expected.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        diff |= static_cast<unsigned char>(actual[i] ^ expected[i]);
    }
    return diff == 0;
}

std::string pbkdf2Hash(const std::string &password, int iterations) {
    if (iterations <= 0) {
        iterations = kDefaultIterations;
    }
    std::vector<unsigned char> salt(16);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("CSPRNG 失败：无法生成口令盐");
    }
    std::vector<unsigned char> hash;
    if (!pbkdf2Raw(password, salt, iterations, hash)) {
        throw std::runtime_error("PBKDF2 计算失败");
    }
    return std::string(kPbkdf2Prefix) + std::to_string(iterations) + "$" +
           base64Encode(salt.data(), salt.size()) + "$" + base64Encode(hash.data(), hash.size());
}

}  // namespace usapp::crypto
