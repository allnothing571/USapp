// =====================================================================
// services/api/src/services/AuditLog.h
//
// 审计日志的尽力而为写入器。
//
// 分工：
//   * 【强一致事件】空间创建、邀请创建/撤销/接受 —— 由 SpaceService 在
//     自己的业务事务里直接 INSERT，保证"业务成功 <=> 审计存在"。
//   * 【尽力而为事件】会话建立/注销、管理员登录/注销 —— 用本类异步写入。
//     审计写失败只打日志，绝不回滚用户已经成功的操作（可用性优先，
//     且这些事件没有资产语义）。
//
// 硬约束：
//   * detail 里禁止出现验证码、会话令牌、CSRF 令牌、邀请码明文或任何密钥。
//   * 本类永远不抛异常（函数标 noexcept，内部全 try/catch）。
// =====================================================================
#pragma once

#include <json/json.h>

#include <string>

namespace usapp {

class AuditLog {
  public:
    // 异步写入 audit_logs。actorType 只允许 user/admin/system（与迁移的 CHECK 一致）。
    // actorId / coupleId / targetId 允许为空串，空串写 NULL。
    static void record(const std::string &actorType,
                       const std::string &actorId,
                       const std::string &coupleId,
                       const std::string &action,
                       const std::string &targetType,
                       const std::string &targetId,
                       const Json::Value &detail = Json::Value(Json::objectValue)) noexcept;

  private:
    AuditLog() = delete;
};

}  // namespace usapp
