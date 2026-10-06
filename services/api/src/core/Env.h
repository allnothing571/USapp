// =====================================================================
// services/api/src/core/Env.h
// 环境变量读取（12-factor）。所有密钥类配置只从环境读取，不进代码库。
// =====================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace usapp::env {

// 原始读取；不存在时返回 std::nullopt。
std::optional<std::string> read(const std::string &name);

// 带默认值读取（未设置或为空串时返回默认值）。
std::string get(const std::string &name, const std::string &fallback = "");

// 必填读取；缺失时抛出 std::runtime_error（启动即失败，避免"静默降级"）。
std::string require(const std::string &name);

int64_t getInt(const std::string &name, int64_t fallback);
bool getBool(const std::string &name, bool fallback);

// 逗号分隔列表，自动 trim 并丢弃空项。
std::vector<std::string> getList(const std::string &name);

}  // namespace usapp::env
