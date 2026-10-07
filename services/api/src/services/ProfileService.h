// =====================================================================
// services/api/src/services/ProfileService.h
//
// 个人资料（阶段 1 M1：昵称）。
//
// 规则（负责人 2026-10-06 批准）：
//   * 先去掉首尾空白，再要求 1..20 个 Unicode 码点（不是 UTF-8 字节，
//     也不是 JavaScript 的 UTF-16 码元）。
//   * 非法 UTF-8、控制字符（C0 / DEL / C1）与换行（含 U+2028 / U+2029）
//     出现在任何位置都拒绝；它们不会被当作“首尾空白”裁掉。
//   * 昵称允许重复；库中已有的空昵称仍是合法存量状态，本服务只约束新写入。
//
// 身份：userId 只能来自服务端会话（控制器从 AuthFilter 写入的请求属性取得），
//   本服务不接受、也不存在“替别人改昵称”的入口。
//
// 事务不变量：同一事务内 UPDATE users -> INSERT audit_logs -> 提交；
//   只有提交回调确认成功后才回调 Ok。审计 detail 不含昵称值、令牌或任何密钥；
//   Status::detail 只写日志，同样不含昵称值。
// =====================================================================
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace usapp {

class ProfileService {
  public:
    static ProfileService &instance();

    static constexpr size_t kDisplayNameMaxCodePoints = 20;
    // 超过该字节数直接判为过长，不做逐字符解码（20 个码点最多 80 字节，
    // 再给首尾空白留足余量）。
    static constexpr size_t kDisplayNameMaxRawBytes = 1024;

    enum class DisplayNameError {
        None,
        InvalidUtf8,       // 字节序列不是合法 UTF-8（含过长编码、代理项、超出 U+10FFFF）
        ControlCharacter,  // 控制字符或换行
        Empty,             // 去掉首尾空白后为空
        TooLong,           // 去掉首尾空白后超过 20 个码点
    };

    // 校验并规范化。成功时 out 为去掉首尾空白后的昵称并返回 None；
    // 失败时 out 被清空。纯函数，不访问数据库，供控制器与单元测试共用。
    static DisplayNameError normalizeDisplayName(const std::string &raw, std::string &out);

    // 面向用户的中文提示（不回显输入）。
    static const char *displayNameErrorMessage(DisplayNameError error);

    enum class Outcome {
        Ok,
        Validation,  // 昵称不合法，未修改任何数据
        NotFound,    // 会话用户在库中不存在（竞态删除），未修改任何数据
        Error,       // 内部错误，细节只写日志
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        DisplayNameError validation = DisplayNameError::None;
        std::string detail;  // 内部描述，只写日志；永不包含昵称值
        bool ok() const { return outcome == Outcome::Ok; }
    };

    using StatusCallback = std::function<void(Status)>;

    // 更新调用者自己的昵称。userId 必须来自服务端会话。
    void updateDisplayName(const std::string &userId, const std::string &rawName, StatusCallback cb);

  private:
    ProfileService() = default;
};

}  // namespace usapp
