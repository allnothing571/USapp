// =====================================================================
// services/api/tests/RepoText.h
//
// 静态文本断言的支撑：
//   * 定位仓库根目录（USAPP_REPO_ROOT 环境变量 -> CMake 注入的默认值
//     -> 从当前工作目录向上找 -> 从本文件所在目录向上找）
//   * 读文件、空白归一化
//   * 解析 contracts/openapi.yaml 的 responses 块（够用即可，不做通用 YAML）
//
// 为什么需要它：无数据库环境下，部分“运行时不变量”（成员数量上限、
// 同一连接同一事务、权限语义）只能通过读取源码/契约文本来做可执行代理
// 断言。这类断言必须在测试里显式标注为“静态文本断言”，不能冒充运行时验证。
// =====================================================================
#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace usapptest {

// ---- 路径工具 -------------------------------------------------------
inline std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) {
        return name;
    }
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') {
        return dir + name;
    }
    return dir + "/" + name;
}

inline std::string parentDir(const std::string &path) {
    std::string s = path;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
    const auto pos = s.find_last_of("/\\");
    if (pos == std::string::npos) {
        return std::string();
    }
    return s.substr(0, pos);
}

inline bool fileExists(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

inline std::string readFileOrEmpty(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::string();
    }
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

// ---- 仓库根目录 -----------------------------------------------------
// 标记文件：存在即认定该目录是仓库根。
inline const char *repoMarkerRelPath() {
    return "database/migrations/0003_couple_binding.sql";
}

inline bool looksLikeRepoRoot(const std::string &dir) {
    return !dir.empty() && fileExists(joinPath(dir, repoMarkerRelPath()));
}

// 向上最多 8 层寻找标记文件。
inline std::string searchUpwards(std::string dir) {
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (looksLikeRepoRoot(dir)) {
            return dir;
        }
        const std::string up = parentDir(dir);
        if (up == dir) {
            break;
        }
        dir = up;
    }
    return std::string();
}

inline std::string findRepoRoot() {
    // 1) 环境变量优先（也用于 CI 里把仓库放在别处的情况）。
    if (const char *env = std::getenv("USAPP_REPO_ROOT")) {
        const std::string root(env);
        if (looksLikeRepoRoot(root)) {
            return root;
        }
    }

    // 2) CMake 注入的默认值（tests/CMakeLists.txt 相对路径推导）。
#ifdef USAPP_REPO_ROOT_DEFAULT
    {
        const std::string injected = USAPP_REPO_ROOT_DEFAULT;
        if (looksLikeRepoRoot(injected)) {
            return injected;
        }
    }
#endif

    // 3) 从当前工作目录向上找。
    {
        const std::string fromCwd = searchUpwards(".");
        if (!fromCwd.empty()) {
            return fromCwd;
        }
    }

    // 4) 从本文件所在目录向上找（源文件路径由编译器提供）。
    {
        const std::string fromFile = searchUpwards(parentDir(std::string(__FILE__)));
        if (!fromFile.empty()) {
            return fromFile;
        }
    }

    return std::string();
}

// 读仓库内文件；失败时返回空串（调用方应显式断言非空并给出可读原因）。
inline std::string readRepoFile(const std::string &relPath) {
    return readFileOrEmpty(joinPath(findRepoRoot(), relPath));
}

// 找不到仓库根时给出可操作的提示，而不是静默跳过。
inline std::string repoRootHint() {
    return "无法定位仓库根目录（缺少 " + std::string(repoMarkerRelPath()) +
           "）。请设置环境变量 USAPP_REPO_ROOT 指向仓库根目录。";
}

// ---- 空白归一化 -----------------------------------------------------
inline bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 把所有空白折叠为单个空格并 trim。
inline std::string collapseWs(const std::string &s) {
    std::string out;
    bool pending = false;
    for (char c : s) {
        if (isSpaceChar(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
    }
    return out;
}

// 删除所有空白（用于“格式无关”的文本断言，例如 SQL 里的空格差异）。
inline std::string stripAllWs(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!isSpaceChar(c)) {
            out.push_back(c);
        }
    }
    return out;
}

// ---- 文本切片 -------------------------------------------------------
// 从 marker 开始（含）到 endMarker 之前（不含）。找不到 marker 返回空串。
inline std::string sliceFromTo(const std::string &text,
                               const std::string &startMarker,
                               const std::string &endMarker) {
    const auto start = text.find(startMarker);
    if (start == std::string::npos) {
        return std::string();
    }
    const auto end = text.find(endMarker, start + startMarker.size());
    if (end == std::string::npos) {
        return text.substr(start);
    }
    return text.substr(start, end - start);
}

// 取从 marker 起到其后的第一个分号（含）——用于抓取 C 字符串拼接的 SQL 常量。
inline std::string sliceToSemicolon(const std::string &text, const std::string &startMarker) {
    const auto start = text.find(startMarker);
    if (start == std::string::npos) {
        return std::string();
    }
    const auto end = text.find(';', start);
    if (end == std::string::npos) {
        return text.substr(start);
    }
    return text.substr(start, end - start + 1);
}

inline std::string trimCopy(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isSpaceChar(s[b])) {
        ++b;
    }
    while (e > b && isSpaceChar(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

inline int indentOf(const std::string &line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') {
            ++n;
        } else if (c == '\t') {
            n += 2;
        } else {
            break;
        }
    }
    return n;
}

inline std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') {
                cur.pop_back();
            }
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

// ---- OpenAPI responses 解析 ----------------------------------------
struct OpenApiResponse {
    std::string path;
    std::string method;
    std::string status;
    bool isRef = false;      // 该响应只有 $ref: '#/components/responses/...'
    bool hasNoStore = false; // 块内出现 NoStore 引用
};

// 只支持本仓库 contracts/openapi.yaml 的实际排版：
//   `  /path:`（2 空格） -> `    get:`（4） -> `      responses:`（6） -> `        '200':`（8）
// 响应体字段缩进 >= 10。
inline std::vector<OpenApiResponse> parseOpenApiResponses(const std::string &text) {
    const std::vector<std::string> lines = splitLines(text);
    std::vector<OpenApiResponse> out;

    std::string path;
    std::string method;
    bool inResponses = false;

    struct Pending {
        size_t lineIndex = 0;
        std::string path;
        std::string method;
        std::string status;
    };
    std::vector<Pending> pending;

    const auto statusOf = [](const std::string &trimmed) -> std::string {
        // 形如 `'200':` 或 `200:`
        std::string s = trimmed;
        if (!s.empty() && s.back() == ':') {
            s.pop_back();
        }
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
            s = s.substr(1, s.size() - 2);
        }
        if (s.size() == 3 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' &&
            s[2] >= '0' && s[2] <= '9') {
            return s;
        }
        return std::string();
    };

    const auto methodOf = [](const std::string &trimmed) -> std::string {
        static const char *kMethods[] = {"get", "post", "put", "patch", "delete", "head", "options"};
        std::string s = trimmed;
        if (!s.empty() && s.back() == ':') {
            s.pop_back();
        }
        for (const char *m : kMethods) {
            if (s == m) {
                return s;
            }
        }
        return std::string();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            continue;
        }
        const int indent = indentOf(line);

        if (indent == 0) {
            // 顶层键：components / servers / paths ... 只有 paths 段内的东西才关心。
            if (trimmed == "components:") {
                break;
            }
            inResponses = false;
            method.clear();
            continue;
        }

        if (indent == 2 && !trimmed.empty() && trimmed[0] == '/') {
            path = trimmed;
            if (!path.empty() && path.back() == ':') {
                path.pop_back();
            }
            method.clear();
            inResponses = false;
            continue;
        }

        if (indent == 4) {
            const std::string m = methodOf(trimmed);
            if (!m.empty()) {
                method = m;
                inResponses = false;
            }
            continue;
        }

        if (indent == 6 && trimmed == "responses:") {
            inResponses = true;
            continue;
        }

        if (inResponses && indent == 8) {
            const std::string st = statusOf(trimmed);
            if (!st.empty() && !path.empty()) {
                pending.push_back(Pending{i, path, method, st});
            }
        }
    }

    out.reserve(pending.size());
    for (size_t p = 0; p < pending.size(); ++p) {
        const Pending &item = pending[p];
        // 响应体字段缩进 >= 10；遇到下一个 `        'NNN':`（缩进 8）即结束。
        std::string body;
        for (size_t j = item.lineIndex + 1; j < lines.size(); ++j) {
            const std::string &line = lines[j];
            const std::string trimmed = trimCopy(line);
            if (trimmed.empty()) {
                continue;
            }
            if (indentOf(line) <= 8) {
                break;
            }
            body += line;
            body.push_back('\n');
        }
        OpenApiResponse r;
        r.path = item.path;
        r.method = item.method;
        r.status = item.status;
        r.hasNoStore = body.find("NoStore") != std::string::npos;
        r.isRef = body.find("$ref: '#/components/responses/") != std::string::npos ||
                  body.find("$ref: \"#/components/responses/") != std::string::npos;
        out.push_back(r);
    }
    return out;
}

}  // namespace usapptest
