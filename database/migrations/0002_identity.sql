-- =====================================================================
-- 0002_identity.sql
-- 身份、会话、邮箱验证码。
--
-- 硬约束：
--   * 会话分两类：预认证会话（user_id IS NULL）与正式会话（user_id NOT NULL）。
--     两类都必须带会话绑定的 CSRF 令牌哈希。
--   * 数据库只保存令牌与验证码的 SHA-256，不保存明文。
--   * 邮箱以 canonical（lower + btrim）作为唯一键；显示值单独保存，不改写用户输入。
-- =====================================================================

BEGIN;

-- ---------------------------------------------------------------- users
CREATE TABLE users (
    id                 uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    email_canonical    text        NOT NULL,
    email_display      text        NOT NULL,
    display_name       text        NOT NULL DEFAULT '',
    status             text        NOT NULL DEFAULT 'active',
    -- 为未来端到端加密预留（需求 40）。阶段 0 不写入、不实现。
    encryption_version smallint,
    key_version        integer,
    encrypted_payload  jsonb,
    created_at         timestamptz NOT NULL DEFAULT now(),
    updated_at         timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT users_email_canonical_lower  CHECK (email_canonical = lower(email_canonical)),
    CONSTRAINT users_email_canonical_shape
        CHECK (email_canonical ~ '^[^@[:space:]]+@[^@[:space:]]+\.[^@[:space:]]+$'),
    CONSTRAINT users_email_display_not_blank CHECK (length(btrim(email_display)) > 0),
    CONSTRAINT users_status_valid CHECK (status IN ('active', 'disabled'))
);

CREATE UNIQUE INDEX users_email_canonical_key ON users (email_canonical);

CREATE TRIGGER users_set_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION usapp_set_updated_at();

COMMENT ON TABLE users IS
    '账号。阶段 0 只有邮箱验证码登录；不存在密码列。管理员在独立表 admin_users 中，'
    '不通过本表任何列授予管理权限。';
COMMENT ON COLUMN users.email_canonical IS
    'lower(btrim(email))；唯一键，避免大小写导致的重复账号。';

-- ------------------------------------------------------------- sessions
CREATE TABLE sessions (
    id                       uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    token_hash               text        NOT NULL,
    csrf_token_hash          text        NOT NULL,
    user_id                  uuid        REFERENCES users (id) ON DELETE CASCADE,
    is_authenticated         boolean     NOT NULL DEFAULT false,
    user_agent_family        text,
    created_at               timestamptz NOT NULL DEFAULT now(),
    last_seen_at             timestamptz NOT NULL DEFAULT now(),
    expires_at               timestamptz NOT NULL,
    revoked_at               timestamptz,
    revoked_reason           text,
    rotated_from_session_id  uuid        REFERENCES sessions (id) ON DELETE SET NULL,

    CONSTRAINT sessions_token_hash_unique UNIQUE (token_hash),
    CONSTRAINT sessions_auth_consistency CHECK (
        (is_authenticated AND user_id IS NOT NULL)
        OR (NOT is_authenticated AND user_id IS NULL)
    ),
    CONSTRAINT sessions_expiry_after_creation CHECK (expires_at > created_at)
);

CREATE INDEX sessions_user_active_idx
    ON sessions (user_id) WHERE revoked_at IS NULL;
CREATE INDEX sessions_expiry_idx
    ON sessions (expires_at) WHERE revoked_at IS NULL;

COMMENT ON TABLE sessions IS
    '服务端会话。Cookie 只携带不透明随机令牌，库内仅存 SHA-256。'
    '登录成功后必须新建一行（轮换会话标识），并撤销原预认证行。';
COMMENT ON COLUMN sessions.is_authenticated IS
    'false = 登录前短期预认证会话；true = 正式会话。两者都绑定 CSRF 令牌。';

-- ------------------------------------------- email_verification_codes
CREATE TABLE email_verification_codes (
    id                       uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    email_canonical          text        NOT NULL,
    purpose                  text        NOT NULL DEFAULT 'login',
    code_hash                text        NOT NULL,
    requested_by_session_id  uuid        REFERENCES sessions (id) ON DELETE CASCADE,
    attempts                 smallint    NOT NULL DEFAULT 0,
    max_attempts             smallint    NOT NULL DEFAULT 5,
    consumed_at              timestamptz,
    -- 消费原因：consumed=登录成功用掉；superseded=重新发送时被新码作废；
    -- rejected=验证码正确但白名单拒绝注册。只记录原因，不记录明文验证码。
    consumed_reason          text,
    expires_at               timestamptz NOT NULL,
    created_at               timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT evc_purpose_valid CHECK (purpose IN ('login')),
    CONSTRAINT evc_consumed_reason_valid
        CHECK (consumed_reason IS NULL
               OR consumed_reason IN ('consumed', 'superseded', 'rejected')),
    CONSTRAINT evc_consumed_reason_requires_consumed_at
        CHECK (consumed_reason IS NULL OR consumed_at IS NOT NULL),
    CONSTRAINT evc_attempts_range
        CHECK (attempts >= 0 AND attempts <= max_attempts AND max_attempts > 0),
    CONSTRAINT evc_expiry_after_creation CHECK (expires_at > created_at),
    CONSTRAINT evc_email_canonical_lower CHECK (email_canonical = lower(email_canonical))
);

-- 同一预认证会话同一时刻只能有一张未消费的验证码；重新发送必须先作废旧码。
CREATE UNIQUE INDEX evc_single_active_per_session
    ON email_verification_codes (requested_by_session_id)
    WHERE consumed_at IS NULL AND requested_by_session_id IS NOT NULL;

CREATE INDEX evc_email_recent_idx
    ON email_verification_codes (email_canonical, created_at DESC);

COMMENT ON TABLE email_verification_codes IS
    '一次性邮箱验证码。只存 SHA-256；attempts 达上限后必须作废。'
    '白名单判定在“验证码校验”阶段进行，发送阶段不泄露邮箱是否在白名单。';

COMMIT;
