-- =====================================================================
-- database/tests/constraints_test.sql
--
-- 数据模型约束测试（只编写，不执行）。
--   运行： psql -h 127.0.0.1 -U usapp_api -d usapp -v ON_ERROR_STOP=1 \
--               -f database/tests/constraints_test.sql
--
-- 约定：
--   * 整个文件包在 BEGIN ... ROLLBACK 中，测试数据全部丢弃。
--   * 每个“应当失败”的插入都包在 BEGIN...EXCEPTION 中；如果没有抛异常，
--     就主动 RAISE EXCEPTION 让 psql 以非零码退出。
--   * 全部通过时打印 ALL CONSTRAINT ASSERTIONS PASSED。
-- =====================================================================

BEGIN;

DO $test$
DECLARE
    u1 uuid; u2 uuid; u3 uuid;
    c1 uuid; c2 uuid;
    s_pre uuid;
    n_catalog integer;
BEGIN
    -- ---------- 夹具 ----------
    INSERT INTO users (email_canonical, email_display)
        VALUES ('alice@example.com', 'Alice@example.com') RETURNING id INTO u1;
    INSERT INTO users (email_canonical, email_display)
        VALUES ('bob@example.com', 'Bob@example.com') RETURNING id INTO u2;
    INSERT INTO users (email_canonical, email_display)
        VALUES ('carol@example.com', 'Carol@example.com') RETURNING id INTO u3;

    INSERT INTO couples (name, created_by_user_id) VALUES ('c1', u1) RETURNING id INTO c1;
    INSERT INTO couples (name, created_by_user_id) VALUES ('c2', u3) RETURNING id INTO c2;

    -- ================================================================
    -- A. 邮箱规范化
    -- ================================================================
    BEGIN
        INSERT INTO users (email_canonical, email_display)
            VALUES ('ALICE@example.com', 'ALICE@example.com');
        RAISE EXCEPTION 'A1 失败：email_canonical 未小写却未被拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO users (email_canonical, email_display)
            VALUES ('alice@example.com', 'Alice2@example.com');
        RAISE EXCEPTION 'A2 失败：重复 email_canonical 未被 UNIQUE 拒绝';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    -- ================================================================
    -- B. couple_members：非空 / slot 取值域 / 唯一性
    -- ================================================================
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u1, c1, 1);

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, c1, 3);
        RAISE EXCEPTION 'B1 失败：slot=3 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, c1, 0);
        RAISE EXCEPTION 'B2 失败：slot=0 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, c1, NULL);
        RAISE EXCEPTION 'B3 失败：slot IS NULL 未被 NOT NULL 拒绝';
    EXCEPTION WHEN not_null_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (NULL, c1, 2);
        RAISE EXCEPTION 'B4 失败：user_id IS NULL 未被 NOT NULL 拒绝';
    EXCEPTION WHEN not_null_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, NULL, 2);
        RAISE EXCEPTION 'B5 失败：couple_id IS NULL 未被 NOT NULL 拒绝';
    EXCEPTION WHEN not_null_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u1, c2, 1);
        RAISE EXCEPTION 'B6 失败：同一 user_id 加入第二个 couple 未被 UNIQUE(user_id) 拒绝';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, c1, 1);
        RAISE EXCEPTION 'B7 失败：同一 (couple_id, slot) 被两人占用未被 UNIQUE 拒绝';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    -- 槽位 2 可用，且两个槽位填满后第三人被拒绝（阶段 0 容量恰好为 2）
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u2, c1, 2);

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u3, c1, 1);
        RAISE EXCEPTION 'B8 失败：slot 1 已占用仍可插入';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO couple_members (user_id, couple_id, slot) VALUES (u3, c1, 2);
        RAISE EXCEPTION 'B9 失败：slot 2 已占用仍可插入';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    -- ================================================================
    -- C. sessions：认证一致性
    -- ================================================================
    INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, expires_at)
        VALUES ('pre-hash-1', 'csrf-hash-1', false, NULL, now() + interval '30 minutes')
        RETURNING id INTO s_pre;

    BEGIN
        INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, expires_at)
            VALUES ('bad-hash-1', 'csrf', true, NULL, now() + interval '1 hour');
        RAISE EXCEPTION 'C1 失败：正式会话缺 user_id 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, expires_at)
            VALUES ('bad-hash-2', 'csrf', false, u1, now() + interval '1 hour');
        RAISE EXCEPTION 'C2 失败：预认证会话带 user_id 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, expires_at)
            VALUES ('pre-hash-1', 'csrf', false, NULL, now() + interval '1 hour');
        RAISE EXCEPTION 'C3 失败：重复 token_hash 未被 UNIQUE 拒绝';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO sessions (token_hash, csrf_token_hash, is_authenticated, user_id, expires_at)
            VALUES ('bad-hash-3', 'csrf', false, NULL, now() - interval '1 minute');
        RAISE EXCEPTION 'C4 失败：expires_at 早于 created_at 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    -- ================================================================
    -- D. email_verification_codes
    -- ================================================================
    INSERT INTO email_verification_codes (email_canonical, code_hash, requested_by_session_id, expires_at)
        VALUES ('alice@example.com', 'code-hash-1', s_pre, now() + interval '10 minutes');

    BEGIN
        INSERT INTO email_verification_codes (email_canonical, code_hash, requested_by_session_id, expires_at)
            VALUES ('alice@example.com', 'code-hash-2', s_pre, now() + interval '10 minutes');
        RAISE EXCEPTION 'D1 失败：同一预认证会话存在两张未消费验证码';
    EXCEPTION WHEN unique_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO email_verification_codes (email_canonical, code_hash, attempts, max_attempts, expires_at)
            VALUES ('bob@example.com', 'code-hash-3', 6, 5, now() + interval '10 minutes');
        RAISE EXCEPTION 'D2 失败：attempts > max_attempts 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    -- ================================================================
    -- E. space_invites
    -- ================================================================
    BEGIN
        INSERT INTO space_invites (couple_id, code_hash, code_prefix, created_by_user_id,
                                   max_uses, used_count, expires_at)
            VALUES (c1, 'inv-hash-bad', 'ABCDEF', u1, 1, 2, now() + interval '1 day');
        RAISE EXCEPTION 'E1 失败：used_count > max_uses 未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    BEGIN
        INSERT INTO space_invites (couple_id, code_hash, code_prefix, created_by_user_id,
                                   expires_at, consumed_at, consumed_by_user_id)
            VALUES (c1, 'inv-hash-bad2', 'ABCDEF', u1, now() + interval '1 day', now(), NULL);
        RAISE EXCEPTION 'E2 失败：consumed_at 有值但 consumed_by_user_id 为空未被 CHECK 拒绝';
    EXCEPTION WHEN check_violation THEN
        NULL;
    END;

    -- ================================================================
    -- F. 管理员与用户必须完全分离（不存在任何外键或列耦合）
    -- ================================================================
    SELECT count(*) INTO n_catalog
    FROM information_schema.columns col
    JOIN information_schema.tables tbl
      ON tbl.table_schema = col.table_schema AND tbl.table_name = col.table_name
    WHERE col.table_name = 'admin_users'
      AND col.column_name IN ('user_id', 'couple_id', 'slot', 'is_admin');
    IF n_catalog <> 0 THEN
        RAISE EXCEPTION 'F1 失败：admin_users 出现与情侣模型耦合的列';
    END IF;

    SELECT count(*) INTO n_catalog
    FROM information_schema.table_constraints tc
    JOIN information_schema.constraint_column_usage ccu
      ON ccu.constraint_name = tc.constraint_name AND ccu.constraint_schema = tc.constraint_schema
    WHERE tc.table_name = 'admin_users'
      AND tc.constraint_type = 'FOREIGN KEY';
    IF n_catalog <> 0 THEN
        RAISE EXCEPTION 'F2 失败：admin_users 存在外键，管理员认证不独立';
    END IF;

    SELECT count(*) INTO n_catalog
    FROM information_schema.columns
    WHERE table_name = 'couples' AND column_name = 'timezone';
    IF n_catalog <> 1 THEN
        RAISE EXCEPTION 'F3 失败：couples.timezone 列缺失（阶段 0 要求保留时区字段）';
    END IF;

    SELECT count(*) INTO n_catalog
    FROM information_schema.columns
    WHERE table_name = 'couples' AND column_name = 'max_members';
    IF n_catalog <> 0 THEN
        RAISE EXCEPTION 'F4 失败：couples.max_members 不应存在（容量由 slot 取值域定义）';
    END IF;

    RAISE NOTICE 'ALL CONSTRAINT ASSERTIONS PASSED';
END
$test$;

\echo 'ALL CONSTRAINT ASSERTIONS PASSED'

ROLLBACK;
