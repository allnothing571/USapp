-- =====================================================================
-- 0003_couple_binding.sql
-- 情侣空间、成员槽位、邀请。
--
-- 硬约束（不得放宽）：
--   * couple_members.user_id 非空且唯一 —— 阶段 0 每个用户最多属于一个 couple。
--   * couple_id 与 slot 组合唯一 —— 一个空间一个槽位最多一人。
--   * slot 只能为 1 或 2；slot 不代表性别、不代表权限。
--   * 不出现 boyfriend_id / girlfriend_id / my_user / her_user 之类硬编码列。
--   * 邀请的有效性校验、消费与加入必须在同一事务内完成。
--   * 容量由 slot 取值域 (1,2) 定义。刻意不引入 max_members 列，
--     避免出现“列允许 N>2 但 slot 约束只允许 2”的自相矛盾。
-- =====================================================================

BEGIN;

-- -------------------------------------------------------------- couples
CREATE TABLE couples (
    id                 uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    name               text        NOT NULL DEFAULT '',
    timezone           text        NOT NULL DEFAULT 'Asia/Shanghai',
    status             text        NOT NULL DEFAULT 'active',
    created_by_user_id uuid        NOT NULL REFERENCES users (id) ON DELETE RESTRICT,
    -- 为未来端到端加密预留（需求 40）。阶段 0 不写入、不实现。
    encryption_version smallint,
    key_version        integer,
    encrypted_payload  jsonb,
    created_at         timestamptz NOT NULL DEFAULT now(),
    updated_at         timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT couples_status_valid CHECK (status IN ('active', 'archived')),
    CONSTRAINT couples_timezone_not_blank CHECK (length(btrim(timezone)) > 0)
);

CREATE INDEX couples_status_idx ON couples (status);
CREATE INDEX couples_created_by_idx ON couples (created_by_user_id);

CREATE TRIGGER couples_set_updated_at
    BEFORE UPDATE ON couples
    FOR EACH ROW EXECUTE FUNCTION usapp_set_updated_at();

COMMENT ON COLUMN couples.timezone IS
    '空间时区。阶段 0 固定写入 Asia/Shanghai，但保留该列以便后续按空间调整；'
    '自然日结算必须以本列为准，禁止用 UTC 直接切日。';
COMMENT ON COLUMN couples.created_by_user_id IS
    '创建者仅用于审计与追溯，不代表任何管理权限。双方权限完全对称。';

-- -------------------------------------------------------- couple_members
CREATE TABLE couple_members (
    id         uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id    uuid        NOT NULL REFERENCES users (id) ON DELETE RESTRICT,
    couple_id  uuid        NOT NULL REFERENCES couples (id) ON DELETE RESTRICT,
    slot       smallint    NOT NULL,
    joined_at  timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT couple_members_slot_valid CHECK (slot IN (1, 2)),
    CONSTRAINT couple_members_user_unique UNIQUE (user_id),
    CONSTRAINT couple_members_couple_slot_unique UNIQUE (couple_id, slot)
);

CREATE INDEX couple_members_couple_idx ON couple_members (couple_id);

COMMENT ON TABLE couple_members IS
    '用户与情侣空间的绑定。user_id 唯一：阶段 0 每个用户只能进入一个 couple。'
    'couple_id + slot 唯一：每个空间每个槽位只能有一人。';
COMMENT ON COLUMN couple_members.slot IS
    '槽位编号，只能为 1 或 2。仅用于占位与展示，不代表性别、角色或权限。';

-- --------------------------------------------------------- space_invites
CREATE TABLE space_invites (
    id                  uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    couple_id           uuid        NOT NULL REFERENCES couples (id) ON DELETE CASCADE,
    code_hash           text        NOT NULL,
    code_prefix         text        NOT NULL,
    created_by_user_id  uuid        NOT NULL REFERENCES users (id) ON DELETE RESTRICT,
    max_uses            smallint    NOT NULL DEFAULT 1,
    used_count          smallint    NOT NULL DEFAULT 0,
    expires_at          timestamptz NOT NULL,
    revoked_at          timestamptz,
    consumed_by_user_id uuid        REFERENCES users (id) ON DELETE SET NULL,
    consumed_at         timestamptz,
    created_at          timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT space_invites_code_hash_unique UNIQUE (code_hash),
    CONSTRAINT space_invites_max_uses_positive CHECK (max_uses >= 1),
    CONSTRAINT space_invites_used_within_max CHECK (used_count >= 0 AND used_count <= max_uses),
    CONSTRAINT space_invites_expiry_after_creation CHECK (expires_at > created_at),
    CONSTRAINT space_invites_consumed_consistency CHECK (
        (consumed_at IS NULL AND consumed_by_user_id IS NULL)
        OR (consumed_at IS NOT NULL AND consumed_by_user_id IS NOT NULL)
    )
);

CREATE INDEX space_invites_couple_idx ON space_invites (couple_id);
CREATE INDEX space_invites_active_idx
    ON space_invites (couple_id)
    WHERE revoked_at IS NULL AND consumed_at IS NULL;

COMMENT ON TABLE space_invites IS
    '情侣空间邀请。明文邀请码只在创建响应中返回一次，库内仅存 SHA-256 与前缀。'
    '接受邀请时必须以 SELECT ... FOR UPDATE 锁定该行，在同一事务内完成'
    '有效性校验、成员插入、used_count 递增与 consumed_* 写入。';
COMMENT ON COLUMN space_invites.code_prefix IS
    '邀请码前缀，仅用于管理与审计展示，不足以还原邀请码。';

COMMIT;
