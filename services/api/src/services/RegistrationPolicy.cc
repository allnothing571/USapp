// =====================================================================
// services/api/src/services/RegistrationPolicy.cc
// =====================================================================
#include "services/RegistrationPolicy.h"

namespace usapp {

RegistrationPolicy &RegistrationPolicy::instance() {
    static RegistrationPolicy policy;
    return policy;
}

void RegistrationPolicy::configure(const std::string &mode,
                                   const std::unordered_set<std::string> &allowedEmails) {
    whitelist_ = (mode == "whitelist");
    allowed_ = allowedEmails;
}

bool RegistrationPolicy::isAllowedEmail(const std::string &canonicalEmail) const {
    return allowed_.count(canonicalEmail) > 0;
}

bool RegistrationPolicy::mayCreateAccount(const std::string &canonicalEmail, bool userExists) const {
    if (userExists) {
        // 白名单不是封禁名单：已有账号不受影响。
        return true;
    }
    if (!whitelist_) {
        return true;
    }
    return isAllowedEmail(canonicalEmail);
}

}  // namespace usapp
