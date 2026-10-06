// =====================================================================
// services/api/src/core/Env.cc
// =====================================================================
#include "core/Env.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace usapp::env {

namespace {

bool isBlank(const std::string &s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::optional<std::string> read(const std::string &name) {
    const char *raw = std::getenv(name.c_str());
    if (raw == nullptr) {
        return std::nullopt;
    }
    return std::string(raw);
}

std::string get(const std::string &name, const std::string &fallback) {
    const auto v = read(name);
    if (!v.has_value() || isBlank(*v)) {
        return fallback;
    }
    return trim(*v);
}

std::string require(const std::string &name) {
    const auto v = read(name);
    if (!v.has_value() || isBlank(*v)) {
        throw std::runtime_error("缺少必需的环境变量: " + name);
    }
    return trim(*v);
}

int64_t getInt(const std::string &name, int64_t fallback) {
    const auto v = read(name);
    if (!v.has_value() || isBlank(*v)) {
        return fallback;
    }
    try {
        return std::stoll(trim(*v));
    } catch (const std::exception &) {
        throw std::runtime_error("环境变量 " + name + " 不是合法整数: " + trim(*v));
    }
}

bool getBool(const std::string &name, bool fallback) {
    const auto v = get(name, "");
    if (v.empty()) {
        return fallback;
    }
    std::string lower = v;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        return false;
    }
    throw std::runtime_error("环境变量 " + name + " 不是合法布尔值: " + v);
}

std::vector<std::string> getList(const std::string &name) {
    std::vector<std::string> out;
    const auto raw = get(name, "");
    if (raw.empty()) {
        return out;
    }
    size_t start = 0;
    while (start <= raw.size()) {
        const auto comma = raw.find(',', start);
        const auto piece = trim(raw.substr(start, comma == std::string::npos ? std::string::npos
                                                                             : comma - start));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

}  // namespace usapp::env
