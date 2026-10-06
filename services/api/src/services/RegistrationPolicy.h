// =====================================================================
// services/api/src/services/RegistrationPolicy.h
//
// 注册白名单策略。
//
// 语义边界（必须保持不变）：
//   * 白名单只决定“是否允许创建一个新账号”。
//   * 白名单不决定任何业务权限：既不影响情侣空间内的角色，
//     也不影响管理员认证（管理员是独立表、独立认证）。
//   * 已存在的账号永远可以继续登录，即使它后来被移出白名单。
//     否则把某人从白名单删掉会变成“封禁已有账号”，与需求不符。
// =====================================================================
#pragma once

#include <string>
#include <unordered_set>

namespace usapp {

class RegistrationPolicy {
  public:
    static RegistrationPolicy &instance();

    void configure(const std::string &mode, const std::unordered_set<std::string> &allowedEmails);

    bool isWhitelistMode() const { return whitelist_; }
    bool isAllowedEmail(const std::string &canonicalEmail) const;

    // userExists 由调用方先查库得到。
    bool mayCreateAccount(const std::string &canonicalEmail, bool userExists) const;

  private:
    RegistrationPolicy() = default;
    bool whitelist_ = true;
    std::unordered_set<std::string> allowed_;
};

}  // namespace usapp
