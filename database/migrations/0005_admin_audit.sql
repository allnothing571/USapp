-- =====================================================================
-- 0005_admin_audit.sql
-- 管理员认证与审计日志。
--
-- 硬约束：
--   * 管理员完全独立于情侣账号体系：管理员不是 users 行，也不通过
--     couple/slot/任何情侣角色获得管理权限。
--   * 管理入口只在回环地址（或受信同机代理）可达，网络层与应用层双重限制。
--   * 关键资产操作、兑换、管理员手动修改必须写审计日志（阶段 0 只建表 + 接入登录/空间事件）。
-- =====================================================================

BEGIN;

-- ------------------------------------------------------------ admin_users
CREATE TABLE admin_users (
    id            uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    username      text        NOT NULL,
    password_hash text        NOT NULL,
    status        text        NOT NULL DEFAULT 'active',
    last_login_at timestamptz,
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT admin_users_username_unique UNIQUE (username),
    CONSTRAINT admin_users_username_shape CHECK (username ~ '^[A-Za-z0-9._-]{3,64}$'),
    CONSTRAINT admin_users_status_valid CHECK (status IN ('active', 'disabled')),
    CONSTRAINT admin_users_password_hash_not_blank CHECK (length(btrim(password_hash)) > 0)
);

CREATE TRIGGER admin_users_set_updated_at
    BEFORE UPDATE ON admin_users
    FOR EACH ROW EXECUTE FUNCTION usapp_set_updated_at();

COMMENT ON TABLE admin_users IS
    '管理员账号。与 users 表无外键关系；管理员身份不来自情侣角色。';
COMMENT ON COLUMN admin_users.password_hash IS
    '格式 pbkdf2-sha256$<iterations>$<salt-b64>$<hash-b64>。'
    '明文口令只存在于负责人本地环境变量，永不入库、永不入仓库。';

-- --------------------------------------------------------- admin_sessions
CREATE TABLE admin_sessions (
    id              uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    token_hash      text        NOT NULL,
    csrf_token_hash text        NOT NULL,
    admin_user_id   uuid        NOT NULL REFERENCES admin_users (id) ON DELETE CASCADE,
    created_at      timestamptz NOT NULL DEFAULT now(),
    last_seen_at    timestamptz NOT NULL DEFAULT now(),
    expires_at      timestamptz NOT NULL,
    revoked_at      timestamptz,
    revoked_reason  text,

    CONSTRAINT admin_sessions_token_hash_unique UNIQUE (token_hash),
    CONSTRAINT admin_sessions_expiry_after_creation CHECK (expires_at > created_at)
);

CREATE INDEX admin_sessions_active_idx
    ON admin_sessions (admin_user_id) WHERE revoked_at IS NULL;

COMMENT ON TABLE admin_sessions IS
    '管理员会话，Cookie 名 __Host-usapp-admin（与情侣会话完全分离）。';

-- ------------------------------------------------------------- audit_logs
CREATE TABLE audit_logs (
    id          bigserial   PRIMARY KEY,
    actor_type  text        NOT NULL,
    actor_id    uuid,
    couple_id   uuid        REFERENCES couples (id) ON DELETE SET NULL,
    action      text        NOT NULL,
    target_type text,
    target_id   text,
    detail      jsonb       NOT NULL DEFAULT '{}'::jsonb,
    created_at  timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT audit_logs_actor_type_valid
        CHECK (actor_type IN ('user', 'admin', 'system')),
    CONSTRAINT audit_logs_action_not_blank CHECK (length(btrim(action)) > 0)
);

CREATE INDEX audit_logs_created_idx ON audit_logs (created_at DESC);
CREATE INDEX audit_logs_actor_idx ON audit_logs (actor_type, actor_id, created_at DESC);
CREATE INDEX audit_logs_couple_idx ON audit_logs (couple_id, created_at DESC);
CREATE INDEX audit_logs_action_idx ON audit_logs (action, created_at DESC);

COMMENT ON TABLE audit_logs IS
    '审计日志。阶段 0 记录：会话建立/注销、空间创建、邀请创建/撤销、邀请接受、'
    '管理员登录/注销。阶段 1+ 的关键资产操作必须继续写入本表。';
COMMENT ON COLUMN audit_logs.detail IS
    '禁止写入验证码、会话令牌、CSRF 令牌、邀请码明文或任何密钥。';

-- --------------------------------------------------- 管理端只读视图骨架
-- 阶段 0 管理后台只需要只读浏览；这里给出两组视图，避免后台直接拼复杂 SQL。
CREATE OR REPLACE VIEW admin_couple_overview AS
SELECT
    c.id                                   AS couple_id,
    c.name                                 AS name,
    c.timezone                             AS timezone,
    c.status                               AS status,
    count(m.id)                            AS member_count,
    count(m.id) FILTER (WHERE m.slot = 1)  AS slot_1_count,
    count(m.id) FILTER (WHERE m.slot = 2)  AS slot_2_count,
    c.created_at                           AS created_at,
    c.created_by_user_id                   AS created_by_user_id
FROM couples c
LEFT JOIN couple_members m ON m.couple_id = c.id
GROUP BY c.id;

CREATE OR REPLACE VIEW admin_user_overview AS
SELECT
    u.id                                  AS user_id,
    u.email_display                       AS email,
    u.display_name                        AS display_name,
    u.status                              AS status,
    m.couple_id                           AS couple_id,
    m.slot                                AS slot,
    m.joined_at                           AS joined_at,
    u.created_at                          AS created_at
FROM users u
LEFT JOIN couple_members m ON m.user_id = u.id;

COMMENT ON VIEW admin_couple_overview IS '管理后台只读视图：空间 + 槽位占用情况。';
COMMENT ON VIEW admin_user_overview   IS '管理后台只读视图：用户 + 所属空间与槽位。';

COMMIT;
