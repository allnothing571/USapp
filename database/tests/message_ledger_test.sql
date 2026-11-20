-- =====================================================================
-- database/tests/message_ledger_test.sql
--
-- P2-1: database-side tests for messages and replies (docs/phase2-messages.md).
-- Requires 0017_messages on usapp_test; refuses any other database before writing
-- fixtures. Run twice, like product_ledger_test.sql:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UB001 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Every failing
-- case runs in a sub-block that is rolled back; an unexpected success or a wrong
-- SQLSTATE raises, and ON_ERROR_STOP makes psql exit non-zero.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $message_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_passed integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    m1 uuid := gen_random_uuid();
    m2 uuid := gen_random_uuid();
    tag text := substr(md5(random()::text), 1, 8);
    fp  text := md5(random()::text) || md5(random()::text);
BEGIN
    IF current_database() IS DISTINCT FROM 'usapp_test' THEN
        RAISE EXCEPTION '拒绝运行：当前数据库为 %，只允许 usapp_test', current_database();
    END IF;
    IF current_user = 'usapp_api' AND NOT (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        is_app := true;
    ELSIF (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        is_app := false;
    ELSE
        RAISE EXCEPTION '只支持以 usapp_api 或维护超级用户运行，当前身份: %', current_user;
    END IF;
    IF to_regclass('public.message_replies') IS NULL THEN
        RAISE EXCEPTION '缺少留言表：请先在测试库应用 0017_messages';
    END IF;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'message_psql_a_' || tag || '@example.com', 'message_psql_a_' || tag || '@example.com'),
               (ub, 'message_psql_b_' || tag || '@example.com', 'message_psql_b_' || tag || '@example.com'),
               (uc, 'message_psql_c_' || tag || '@example.com', 'message_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'message-psql', ua), (c2, 'message-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);

    -- A. Valid messages both ways, LF kept, and the recipient's single reply.
    INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
        VALUES (m1, c1, ua, ub, '今天面试加油' || chr(10) || '不许紧张', 'psql-message-1-' || tag, fp),
               (m2, c1, ub, ua, '收到啦', 'psql-message-2-' || tag, fp);
    INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
        VALUES (m1, c1, ub, '今天也辛苦啦。', 'psql-reply-1-' || tag, fp);
    n_passed := n_passed + 1;

    -- B. Shape and identity constraints.
    BEGIN  -- B1 empty, too long, trailing blank line, control character
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (c1, ua, ub, repeat('长', 501), 'psql-message-b1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'B1 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    BEGIN
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (c1, ua, ub, 'x' || chr(7), 'psql-message-b1b-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1b 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'B1b 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B2 self-addressed
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (c1, ua, ua, 'x', 'psql-message-b2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'B2 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B3 recipient outside the space
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (c1, ua, uc, 'x', 'psql-message-b3-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23503' THEN RAISE EXCEPTION 'B3 失败：期望 23503，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B4 duplicate key for the same author
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (c1, ua, ub, 'x', 'psql-message-1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23505' THEN RAISE EXCEPTION 'B4 失败：期望 23505，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B5 future time
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at)
            VALUES (c1, ua, ub, 'x', 'psql-message-b5-' || tag, fp, clock_timestamp() + interval '1 hour');
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> 'UB002' THEN RAISE EXCEPTION 'B5 失败：期望 UB002，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- C. Reply guard: recipient only, same space, one reply, one line.
    BEGIN  -- C1 the author replying
        INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
            VALUES (m2, c1, ub, 'x', 'psql-reply-c1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> 'UB002' THEN RAISE EXCEPTION 'C1 失败：期望 UB002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- C2 a second reply
        INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
            VALUES (m1, c1, ub, 'again', 'psql-reply-c2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23505' THEN RAISE EXCEPTION 'C2 失败：期望 23505，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- C3 a reply claiming another space
        INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
            VALUES (m2, c2, uc, 'x', 'psql-reply-c3-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> 'UB002' THEN RAISE EXCEPTION 'C3 失败：期望 UB002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- C4 multi-line reply
        INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
            VALUES (m2, c1, ua, 'a' || chr(10) || 'b', 'psql-reply-c4-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'C4 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- D. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
    BEGIN
        UPDATE messages SET body = 'changed' WHERE id = m1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UB001' END) THEN
                RAISE EXCEPTION 'D1 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    BEGIN
        DELETE FROM message_replies WHERE message_id = m1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UB001' END) THEN
                RAISE EXCEPTION 'D2 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    BEGIN
        -- 0020 adds notifications / notification_intents referencing both tables: list them when
        -- present (no CASCADE) so the FK check does not mask UB001.
        -- 0023 adds the push dispatch chain referencing notifications: listed last, no CASCADE.
        IF to_regclass('public.push_deliveries') IS NOT NULL THEN
            TRUNCATE message_replies, messages, notifications, notification_reads, notification_intents, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations;
        ELSIF to_regclass('public.notification_intents') IS NOT NULL THEN
            TRUNCATE message_replies, messages, notifications, notification_reads, notification_intents;
        ELSE
            TRUNCATE message_replies, messages;
        END IF;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UB001' END) THEN
                RAISE EXCEPTION 'D3 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- E. The valid fixture is intact and wrote no wallet data.
    IF (SELECT count(*) FROM messages WHERE couple_id = c1) <> 2
       OR (SELECT count(*) FROM message_replies WHERE couple_id = c1) <> 1
       OR (SELECT body FROM message_replies WHERE message_id = m1) <> '今天也辛苦啦。'
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id = c1) THEN
        RAISE EXCEPTION 'E1 失败：留言夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL MESSAGE ASSERTIONS PASSED: role=% mode=% groups=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed;
END
$message_test$;

ROLLBACK;
