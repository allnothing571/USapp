// =====================================================================
// services/api/src/services/SpaceService.h
//
// 情侣空间（couples / couple_members）与邀请（space_invites）。
//
// 语言约定（贯穿全仓，禁止出现 boyfriend_id/girlfriend_id 之类）：
//   * 不区分性别、不区分"谁邀请谁"的语义角色。成员用 slot ∈ {1,2}
//     表示"空间内的第几个位置"，slot 只表达容量与去重，不代表性别、
//     权限或创建者身份。创建者只是 created_by_user_id 与先占一个 slot。
//   * 数据模型不写死"两名用户"：容量由 slot 的取值域 (1,2) 与
//     UNIQUE(couple_id, slot) 决定，代码里只按"找到第一个空 slot"处理。
//
// 事务不变量（与 database/migrations/0003_couple_binding.sql 的表注释一致）：
//   * 创建空间：同一事务内 users 行 FOR UPDATE -> 确认无成员 -> 插 couples
//     -> 插 couple_members(slot=1) -> 写审计。
//   * 接受邀请：同一事务内 users 行 FOR UPDATE -> space_invites 行 FOR UPDATE
//     -> 校验撤销/过期/次数 -> 确认接受者无空间 -> couples 行 FOR UPDATE
//     -> 选空 slot -> 插成员 -> 递增 used_count 并写 consumed_* -> 写审计。
//     先锁 users 行让"同一用户的并发加入"串行化，再锁 couples 行让
//     "同一空间的并发加入"串行化；没有这两个锁，两个并发加入者会算出
//     同一个空 slot，靠唯一约束报 500 而不是干净的 SpaceFull。
// =====================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace usapp {

struct SpaceMemberInfo {
    std::string userId;
    std::string displayName;
    std::string emailDisplay;  // 仅展示用；不参与任何判定
    int slot = 0;
    std::string joinedAt;  // ISO8601 UTC
};

struct SpaceInfo {
    std::string spaceId;
    std::string name;
    std::string timezone;  // 阶段 0 固定 Asia/Shanghai，字段保留
    std::string status;    // active | archived
    std::string createdAt;
    std::vector<SpaceMemberInfo> members;
};

class SpaceService {
  public:
    static SpaceService &instance();

    // 邀请有效期与可用次数。刻意用常量而不是环境变量：阶段 0 没有运营需求，
    // 少一个可被配错的旋钮。测试与文档都引用这两个常量。
    static constexpr int kInviteTtlSeconds = 7 * 24 * 60 * 60;  // 7 天
    static constexpr int kInviteMaxUses = 1;

    enum class Outcome {
        Ok,
        AlreadyInSpace,          // 创建空间时：该用户已在某个空间
        SpaceNotFound,           // 查询/发邀请时：该用户不在任何空间
        SpaceFull,               // 接受邀请时：两个 slot 都被占用
        InviteInvalidOrExpired,  // 邀请码不存在、已过期或已用尽
        InviteRevoked,           // 邀请已被创建者撤销
        Validation,              // 入参不合法（形状/空值）
        Error,                   // 内部错误，细节只写日志
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string detail;  // 内部描述，只写日志，不回给客户端
        bool ok() const { return outcome == Outcome::Ok; }
    };

    // 邀请元数据。**不含邀请码明文**：库里只有 SHA-256，明文只在创建响应里出现一次。
    // 注意：必须声明在 InviteListCallback 之前，否则别名引用的是未声明的名字。
    struct InviteInfo {
        std::string id;
        std::string expiresAt;
        std::string createdAt;
        int maxUses = 0;
        int usedCount = 0;
        bool revoked = false;
    };

    // 第二个参数在非 Ok 时是默认构造的空 SpaceInfo。
    using SpaceCallback = std::function<void(Status, SpaceInfo)>;
    // rawCode 是明文邀请码，只在创建时返回一次；数据库只存 SHA-256。
    using InviteCreatedCallback = std::function<void(Status,
                                                     const std::string &rawCode,
                                                     const std::string &inviteId,
                                                     const std::string &expiresAt)>;
    using StatusCallback = std::function<void(Status)>;
    using InviteListCallback = std::function<void(Status, std::vector<InviteInfo>)>;

    // ---- 空间 ----
    void createSpaceForUser(const std::string &userId, SpaceCallback cb);
    void getCurrentSpace(const std::string &userId, SpaceCallback cb);
    // 空间详情。非成员一律 SpaceNotFound（不区分"不存在"与"无权查看"，
    // 避免据此探测他人空间是否存在）。
    void getSpaceForMember(const std::string &userId, const std::string &spaceId, SpaceCallback cb);

    // ---- 邀请 ----
    void createInvite(const std::string &userId, InviteCreatedCallback cb);
    // 列出当前空间的邀请（只含元数据）。用户不在任何空间时 SpaceNotFound。
    void listInvites(const std::string &userId, InviteListCallback cb);
    void revokeInvite(const std::string &userId, const std::string &inviteId, StatusCallback cb);
    void acceptInvite(const std::string &userId, const std::string &rawCode, SpaceCallback cb);

    // ---- 工具（控制器与测试共用）----
    // 归一化：去掉空白与连字符、转大写。返回空串表示形状不合法。
    // 目标字母表去掉易混字符（0/O、1/I/L、U/V 视作可区分保留）。
    static std::string normalizeInviteCode(const std::string &raw);
    // 生成 12 字符、约 59.5 bit 的邀请码，形如 "XXXX-XXXX-XXXX"。
    static std::string generateInviteCode();

    static const char *outcomeMessage(Outcome outcome);

  private:
    SpaceService() = default;
};

}  // namespace usapp
