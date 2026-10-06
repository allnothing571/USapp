// =====================================================================
// services/api/tests/TestSupport.h
//
// 极简测试支撑：断言宏 + 失败计数。
//
// 为什么不用 GoogleTest/Catch2：仓库不引入新依赖（阶段 0 硬约束），
// 只用 C++17 标准库。用法：
//
//   #include "TestSupport.h"
//   int main() {
//       TEST_CASE("xxx") { CHECK(...); }
//       return usapptest::finish();
//   }
//
// 每个断言都打印 __FILE__:__LINE__；main 末尾 finish() 返回
// 0（全部通过）或 1（存在失败），供 ctest 判定。
//
// 重要：本文件只是“断言与计数”，不做任何 I/O 之外的事情，
// 因此纯单元测试不依赖数据库、网络或 Drogon 事件循环。
// =====================================================================
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace usapptest {

// ---- 全局计数 -------------------------------------------------------
inline int &totalChecks() {
    static int n = 0;
    return n;
}

inline int &failedChecks() {
    static int n = 0;
    return n;
}

inline const char *&currentCase() {
    static const char *c = "(no case)";
    return c;
}

// ---- 值的可打印化 ---------------------------------------------------
inline std::string render(const std::string &v) { return v; }
inline std::string render(const char *v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string render(bool v) { return v ? "true" : "false"; }

template <typename T>
std::string render(const T &v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// 把不可见字符转义出来，避免断言输出里“看起来一样其实不一样”。
inline std::string visualize(const std::string &v) {
    std::string out;
    out.reserve(v.size() + 2);
    for (unsigned char c : v) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c >= 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// ---- 失败记录 -------------------------------------------------------
inline void fail(const char *file, int line, const std::string &what) {
    ++failedChecks();
    std::printf("[FAIL] %s\n       %s:%d\n       %s\n", currentCase(), file, line, what.c_str());
}

// 显式声明“这是静态文本断言，不是运行时验证”，避免把文本代理当成
// 真实的运行时证据。写入测试时调用一次即可。
inline void noteStaticAssertion(const std::string &why) {
    std::printf("[STATIC] %s%s\n", currentCase(), why.c_str());
}

inline void note(const std::string &what) { std::printf("[NOTE]   %s\n", what.c_str()); }

inline int finish() {
    const int total = totalChecks();
    const int failed = failedChecks();
    std::printf("\n---- 断言 %d 个，失败 %d 个 ----\n", total, failed);
    if (failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d 个断言失败)\n", failed);
    return 1;
}

// 门控跳过出口：输出明确跳过原因，并返回独立退出码（默认 77，对应 CTest 的 SKIP_RETURN_CODE）。
inline int skip(const std::string &reason, int skipCode = 77) {
    std::printf("\n[SKIP] %s (exit code %d)\n", reason.c_str(), skipCode);
    std::printf("RESULT: SKIP\n");
    return skipCode;
}

}  // namespace usapptest

// ---- 用例开始 -------------------------------------------------------
#define TEST_CASE(name)                                        \
    usapptest::currentCase() = (name);                          \
    std::printf("[ RUN  ] %s\n", usapptest::currentCase());     \
    for (bool _usapp_case_once = true; _usapp_case_once;)        \
        for (; _usapp_case_once; _usapp_case_once = false)

// ---- 基础断言 -------------------------------------------------------
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++usapptest::totalChecks();                                            \
        if (!(cond)) {                                                         \
            usapptest::fail(__FILE__, __LINE__,                                \
                            std::string("CHECK 失败: ") + #cond);              \
        }                                                                      \
    } while (0)

#define CHECK_FALSE(cond) CHECK(!(cond))

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        const auto &_usapp_a = (a);                                             \
        const auto &_usapp_b = (b);                                             \
        if (!(_usapp_a == _usapp_b)) {                                          \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_EQ 失败: " #a " == " #b          \
                                        "\n         左值: ") +                  \
                                usapptest::visualize(usapptest::render(_usapp_a)) + \
                                "\n         右值: " +                           \
                                usapptest::visualize(usapptest::render(_usapp_b))); \
        }                                                                       \
    } while (0)

#define CHECK_NE(a, b)                                                          \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        const auto &_usapp_a = (a);                                             \
        const auto &_usapp_b = (b);                                             \
        if (_usapp_a == _usapp_b) {                                             \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_NE 失败: " #a " != " #b          \
                                        "\n         两者都是: ") +              \
                                usapptest::visualize(usapptest::render(_usapp_a))); \
        }                                                                       \
    } while (0)

// ---- 异常断言 -------------------------------------------------------
#define CHECK_THROWS_AS(expr, ExType)                                           \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        bool _usapp_thrown_expected = false;                                    \
        bool _usapp_wrong_exception = false;                                    \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const ExType &) {                                              \
            _usapp_thrown_expected = true;                                      \
        } catch (const std::exception &_usapp_other) {                          \
            _usapp_wrong_exception = true;                                      \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_THROWS_AS: " #expr              \
                                        " 抛出了非 " #ExType " 异常: ") +       \
                                _usapp_other.what());                           \
        } catch (...) {                                                         \
            _usapp_wrong_exception = true;                                      \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_THROWS_AS: " #expr              \
                                        " 抛出了非 " #ExType " 异常（未知类型）")); \
        }                                                                       \
        if (!_usapp_thrown_expected && !_usapp_wrong_exception) {               \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_THROWS_AS: " #expr              \
                                        " 未抛出 " #ExType));                   \
        }                                                                       \
    } while (0)

#define CHECK_NOTHROW(expr)                                                     \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const std::exception &_usapp_e) {                              \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_NOTHROW: " #expr " 抛出了: ") +  \
                                _usapp_e.what());                               \
        } catch (...) {                                                         \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_NOTHROW: " #expr " 抛出了未知异常")); \
        }                                                                       \
    } while (0)

// ---- 文本包含断言（静态断言用）--------------------------------------
#define CHECK_CONTAINS(haystack, needle)                                        \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        const std::string _usapp_hay = (haystack);                              \
        const std::string _usapp_need = (needle);                               \
        if (_usapp_hay.find(_usapp_need) == std::string::npos) {                \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_CONTAINS 失败，未找到: ") +      \
                                usapptest::visualize(_usapp_need));             \
        }                                                                       \
    } while (0)

#define CHECK_NOT_CONTAINS(haystack, needle)                                    \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        const std::string _usapp_hay = (haystack);                              \
        const std::string _usapp_need = (needle);                               \
        const auto _usapp_pos = _usapp_hay.find(_usapp_need);                   \
        if (_usapp_pos != std::string::npos) {                                  \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_NOT_CONTAINS 失败，不应出现: ") + \
                                usapptest::visualize(_usapp_need) +             \
                                " (offset " + std::to_string(_usapp_pos) + ")"); \
        }                                                                       \
    } while (0)

// 顺序断言：first 必须出现在 second 之前（同为 npos 视为失败）。
#define CHECK_ORDER(text, first, second)                                        \
    do {                                                                        \
        ++usapptest::totalChecks();                                             \
        const std::string _usapp_t = (text);                                    \
        const auto _usapp_p1 = _usapp_t.find(first);                            \
        const auto _usapp_p2 = _usapp_t.find(second);                           \
        if (_usapp_p1 == std::string::npos) {                                   \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_ORDER: 未找到第一个串: ") + #first); \
        } else if (_usapp_p2 == std::string::npos) {                            \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_ORDER: 未找到第二个串: ") + #second); \
        } else if (!(_usapp_p1 < _usapp_p2)) {                                  \
            usapptest::fail(__FILE__, __LINE__,                                 \
                            std::string("CHECK_ORDER 失败: " #first             \
                                        " 应出现在 " #second " 之前 (offset ") + \
                                std::to_string(_usapp_p1) + " vs " +            \
                                std::to_string(_usapp_p2) + ")");               \
        }                                                                       \
    } while (0)
