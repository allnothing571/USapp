-- =====================================================================
-- database/tests/redemption_ledger_test.sql
--
-- M8: database-side tests for direct redemptions (docs/phase1-m8-redemption.md).
-- Requires 0015_reward_redemptions on usapp_test; refuses any other database before
-- writing fixtures. Run twice, like product_ledger_test.sql:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UM001 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Every failing
-- case runs in a sub-block that is rolled back; an unexpected success or a wrong
-- SQLSTATE raises, and ON_ERROR_STOP makes psql exit non-zero. Deferred link checks
-- are triggered with SET CONSTRAINTS ALL IMMEDIATE (real commits are db_gated's job).
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $redemption_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_passed integer := 0;
    ua uuid := gen_random_uuid();  -- creator
    ub uuid := gen_random_uuid();  -- owner / redeemer
    c1 uuid := gen_random_uuid();
    p1 uuid := gen_random_uuid();  -- direct, price 3
    p2 uuid := gen_random_uuid();  -- request mode
    fund uuid := gen_random_uuid();
    r1 uuid := gen_random_uuid();
    o1 uuid := gen_random_uuid();
    rx uuid;
    ox uuid;
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
    IF to_regclass('public.reward_redemption_fulfillments') IS NULL THEN
        RAISE EXCEPTION '缺少兑换表：请先在测试库应用 0015_reward_redemptions';
    END IF;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'redeem_psql_a_' || tag || '@example.com', 'redeem_psql_a_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ub, 'redeem_psql_b_' || tag || '@example.com', 'redeem_psql_b_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'redeem-psql', ua);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2);
    INSERT INTO reward_products (id, couple_id, creator_id, owner_id) VALUES (p1, c1, ua, ub), (p2, c1, ua, ub);
    INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
        redemption_mode, status, idempotency_key, request_fingerprint)
        VALUES (p1, c1, 1, ua, 'create', 'Movie night', 3, 'direct', 'active', 'psql-redeem-p1-' || tag, fp),
               (p2, c1, 1, ua, 'create', 'Ask first', 2, 'request', 'active', 'psql-redeem-p2-' || tag, fp);
    -- Generic funding of 10 flowers (not a task settlement: 0007 checks source_type = 'task' only).
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'flower', c1);
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (fund, c1, ub, 'TASK_REWARD', 'psql_test', fund::text);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (fund, c1, ub, 'flower', 1, 10, 10);

    -- A. A valid redemption: operation, snapshot row, one -price debit; then a creator fulfilment.
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (o1, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', r1::text);
    INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
        price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
        VALUES (r1, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', o1, 'psql-redeem-r1-' || tag, fp);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (o1, c1, ub, 'flower', 2, -3, 7);
    SET CONSTRAINTS ALL IMMEDIATE;  -- a valid redemption passes every deferred link check
    SET CONSTRAINTS ALL DEFERRED;
    INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key,
        request_fingerprint) VALUES (r1, c1, ua, 'psql-fulfil-r1-' || tag, fp);
    n_passed := n_passed + 1;

    -- B. Insert guard (UM002): actor, stale/tampered snapshot, archived, request mode, wrong operation.
    BEGIN  -- B1 the creator cannot be the redeemer
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ua, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ub, ua, 'Movie night', 3, 'direct', ox, 'psql-redeem-b1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B1 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B2 a stale version after an edit
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 2, ua, 'edit', 'Movie night', 9, 'direct', 'active', 'psql-redeem-b2e-' || tag, fp);
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-b2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B2 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B3 a snapshot that differs from the revision (price)
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 1, 'direct', ox, 'psql-redeem-b3-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B3 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B4 an archived product
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 2, ua, 'archive', 'Movie night', 3, 'direct', 'archived', 'psql-redeem-b4a-' || tag, fp);
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 2, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-b4-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B4 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B5 request mode is not redeemable in M8 (guard before the direct-only CHECK)
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p2, 1, ua, ub, 'Ask first', 2, 'direct', ox, 'psql-redeem-b5-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B5 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B6 the operation must be this redemption's REWARD_REDEMPTION operation
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', gen_random_uuid()::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-b6-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B6 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM002' THEN RAISE EXCEPTION 'B6 失败：期望 UM002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- B7 one redemption per (redeemer, idempotency key)
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-r1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B7 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> '23505' THEN RAISE EXCEPTION 'B7 失败：期望 23505，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- C. Deferred debit link (UM003): every redemption operation has exactly one -price flower debit.
    BEGIN  -- C1 a redemption-type debit without a redemption row
        ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', gen_random_uuid()::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (ox, c1, ub, 'flower', 3, -3, 4);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM003' THEN RAISE EXCEPTION 'C1 失败：期望 UM003，实际 %', SQLSTATE; END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    BEGIN  -- C2 a debit that does not match the snapshot price
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-c2-' || tag, fp);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (ox, c1, ub, 'flower', 3, -1, 6);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM003' THEN RAISE EXCEPTION 'C2 失败：期望 UM003，实际 %', SQLSTATE; END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    BEGIN  -- C3 a redemption whose operation has no debit (UM003 or the 0006 UW006, whichever fires first)
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-c3-' || tag, fp);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE NOT IN ('UM003', 'UW006') THEN
                RAISE EXCEPTION 'C3 失败：期望 UM003/UW006，实际 %', SQLSTATE;
            END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    -- D. Fulfilment guard and finality. M9 rejects a terminal state before the PK.
    BEGIN  -- D1 the redeemer cannot fulfil
        rx := gen_random_uuid(); ox := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (ox, c1, ub, 'REWARD_REDEMPTION', 'reward_redemption', rx::text);
        INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title,
            price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
            VALUES (rx, c1, p1, 1, ua, ub, 'Movie night', 3, 'direct', ox, 'psql-redeem-d1-' || tag, fp);
        INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key,
            request_fingerprint) VALUES (rx, c1, ub, 'psql-fulfil-d1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UM004' THEN RAISE EXCEPTION 'D1 失败：期望 UM004，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- D2 a fulfilment is final
        INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key,
            request_fingerprint) VALUES (r1, c1, ua, 'psql-fulfil-d2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN to_regclass('public.reward_redemption_events') IS NOT NULL
                                THEN 'UM004' ELSE '23505' END) THEN
                RAISE EXCEPTION 'D2 失败：重复履约未被预期守卫拒绝，实际 %', SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- E. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
    BEGIN
        UPDATE reward_redemptions SET price_flowers = 1 WHERE id = r1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UM001' END) THEN
                RAISE EXCEPTION 'E1 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    BEGIN
        DELETE FROM reward_redemption_fulfillments WHERE redemption_id = r1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UM001' END) THEN
                RAISE EXCEPTION 'E2 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    SET CONSTRAINTS ALL IMMEDIATE;  -- TRUNCATE refuses pending deferred events (55006)
    BEGIN
        -- 0023 adds the push dispatch chain referencing notifications: listed last, no CASCADE.
        IF to_regclass('public.push_deliveries') IS NOT NULL THEN
                TRUNCATE reward_redemption_events,reward_redemption_fulfillments,reward_redemptions, notifications, notification_reads, notification_intents, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, admin_redemption_cancellations;
        ELSIF to_regclass('public.notification_intents') IS NOT NULL THEN
                TRUNCATE reward_redemption_events,reward_redemption_fulfillments,reward_redemptions, notifications, notification_reads, notification_intents;
        ELSIF to_regclass('public.notifications') IS NOT NULL THEN
                TRUNCATE reward_redemption_events,reward_redemption_fulfillments,reward_redemptions, notifications, notification_reads;
        ELSIF to_regclass('public.reward_redemption_events') IS NOT NULL THEN
            TRUNCATE reward_redemption_events,reward_redemption_fulfillments,reward_redemptions;
        ELSE
            TRUNCATE reward_redemption_fulfillments, reward_redemptions;
        END IF;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UM001' END) THEN
                RAISE EXCEPTION 'E3 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    -- F. The valid fixture is intact: one debit, balance 7, no sunshine, no refunds.
    IF (SELECT balance_after FROM wallet_ledger_entries WHERE user_id = ub AND asset_type = 'flower'
         ORDER BY seq DESC LIMIT 1) <> 7
       OR (SELECT count(*) FROM reward_redemptions WHERE couple_id = c1) <> 1
       OR (SELECT count(*) FROM reward_redemption_fulfillments WHERE couple_id = c1) <> 1
       OR EXISTS (SELECT 1 FROM wallet_ledger_entries WHERE couple_id = c1 AND asset_type = 'sunshine')
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id = c1 AND operation_type = 'REDEMPTION_REFUND') THEN
        RAISE EXCEPTION 'F1 失败：兑换夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL REDEMPTION LEDGER ASSERTIONS PASSED: role=% mode=% groups=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed;
END
$redemption_test$;

SET CONSTRAINTS ALL IMMEDIATE;

ROLLBACK;
