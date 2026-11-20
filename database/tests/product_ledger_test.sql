-- =====================================================================
-- database/tests/product_ledger_test.sql
--
-- M7: database-side tests for reward products (docs/phase1-m7-rewards.md).
-- Requires 0014_reward_products on usapp_test; refuses any other database before
-- writing fixtures. Run twice, like task_ledger_test.sql:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UR001 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Every failing
-- case runs in a sub-block that is rolled back; an unexpected success or a wrong
-- SQLSTATE raises, and ON_ERROR_STOP makes psql exit non-zero.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $product_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_passed integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    p1 uuid := gen_random_uuid();
    p2 uuid;
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
    IF to_regclass('public.reward_product_revisions') IS NULL THEN
        RAISE EXCEPTION '缺少奖励商品表：请先在测试库应用 0014_reward_products';
    END IF;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'product_psql_a_' || tag || '@example.com', 'product_psql_a_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ub, 'product_psql_b_' || tag || '@example.com', 'product_psql_b_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'product-psql', ua);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2);

    -- A. A valid product (A for B) with create, edit and archive revisions.
    INSERT INTO reward_products (id, couple_id, creator_id, owner_id) VALUES (p1, c1, ua, ub);
    INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, description,
        price_flowers, redemption_mode, status, idempotency_key, request_fingerprint)
        VALUES (p1, c1, 1, ua, 'create', 'Movie night', 'line one' || chr(10) || 'line two', 5, 'direct', 'active',
                'psql-product-1-' || tag, fp);
    INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
        redemption_mode, status, idempotency_key, request_fingerprint)
        VALUES (p1, c1, 2, ua, 'edit', 'Movie night', 999, 'request', 'active', 'psql-product-2-' || tag, fp);
    SET CONSTRAINTS ALL IMMEDIATE;
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    BEGIN  -- B1 price outside 1-999 (0 and 1000)
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 3, ua, 'edit', 'Free', 0, 'direct', 'active', 'psql-product-b1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'B1 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    BEGIN
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 3, ua, 'edit', 'Too much', 1000, 'direct', 'active', 'psql-product-b1b-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1b 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'B1b 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B2 the owner cannot write a revision (UR002)
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 3, ub, 'edit', 'Owner edit', 5, 'direct', 'active', 'psql-product-b2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UR002' THEN RAISE EXCEPTION 'B2 失败：期望 UR002，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B3 version gap (UR002); B4 archive must keep the current terms (UR002)
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 4, ua, 'edit', 'Gap', 5, 'direct', 'active', 'psql-product-b3-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UR002' THEN RAISE EXCEPTION 'B3 失败：期望 UR002，实际 %', SQLSTATE; END IF;
    END;
    BEGIN
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 3, ua, 'archive', 'Renamed on archive', 999, 'request', 'archived',
                    'psql-product-b4-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UR002' THEN RAISE EXCEPTION 'B4 失败：期望 UR002，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- C. Archive is one-way: after a valid archive no further revision is accepted.
    INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
        redemption_mode, status, idempotency_key, request_fingerprint)
        VALUES (p1, c1, 3, ua, 'archive', 'Movie night', 999, 'request', 'archived', 'psql-product-3-' || tag, fp);
    BEGIN
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers,
            redemption_mode, status, idempotency_key, request_fingerprint)
            VALUES (p1, c1, 4, ua, 'edit', 'Revived', 5, 'direct', 'active', 'psql-product-c1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UR002' THEN RAISE EXCEPTION 'C1 失败：期望 UR002，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D1 a member cannot create a product for themselves
        INSERT INTO reward_products (couple_id, creator_id, owner_id) VALUES (c1, ua, ua);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> '23514' THEN RAISE EXCEPTION 'D1 失败：期望 23514，实际 %', SQLSTATE; END IF;
    END;
    BEGIN  -- D2 a product without its create revision fails the deferred check (UR003)
        p2 := gen_random_uuid();
        INSERT INTO reward_products (id, couple_id, creator_id, owner_id) VALUES (p2, c1, ub, ua);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UR003' THEN RAISE EXCEPTION 'D2 失败：期望 UR003，实际 %', SQLSTATE; END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    -- E. Append-only: the application role sees permission errors, maintenance sees UR001.
    BEGIN
        UPDATE reward_product_revisions SET price_flowers = price_flowers WHERE product_id = p1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UR001' END) THEN
                RAISE EXCEPTION 'E1 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    BEGIN
        DELETE FROM reward_products WHERE id = p1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UR001' END) THEN
                RAISE EXCEPTION 'E2 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    SET CONSTRAINTS ALL IMMEDIATE;  -- TRUNCATE refuses pending deferred events (55006)
    BEGIN
        -- 0015 adds reward_redemptions referencing both tables: list it (and its fulfillments)
        -- explicitly when present, without CASCADE, so the FK check passes and UR001 is proven.
        -- 0023 adds the push dispatch chain referencing notifications: listed last, no CASCADE.
        IF to_regclass('public.push_deliveries') IS NOT NULL THEN
                TRUNCATE reward_product_revisions,reward_products,reward_redemptions,reward_redemption_fulfillments,reward_redemption_events, notifications, notification_reads, notification_intents, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, admin_redemption_cancellations;
        ELSIF to_regclass('public.notification_intents') IS NOT NULL THEN
                TRUNCATE reward_product_revisions,reward_products,reward_redemptions,reward_redemption_fulfillments,reward_redemption_events, notifications, notification_reads, notification_intents;
        ELSIF to_regclass('public.notifications') IS NOT NULL THEN
                TRUNCATE reward_product_revisions,reward_products,reward_redemptions,reward_redemption_fulfillments,reward_redemption_events, notifications, notification_reads;
        ELSIF to_regclass('public.reward_redemption_events') IS NOT NULL THEN
            TRUNCATE reward_product_revisions,reward_products,reward_redemptions,reward_redemption_fulfillments,reward_redemption_events;
        ELSIF to_regclass('public.reward_redemption_fulfillments') IS NULL THEN
            TRUNCATE reward_product_revisions, reward_products;
        ELSE
            TRUNCATE reward_product_revisions, reward_products, reward_redemptions, reward_redemption_fulfillments;
        END IF;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UR001' END) THEN
                RAISE EXCEPTION 'E3 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    -- F. No wallet side effects for this space.
    IF EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id = c1)
       OR EXISTS (SELECT 1 FROM wallet_accounts WHERE couple_id = c1) THEN
        RAISE EXCEPTION 'F1 失败：奖励商品写入了钱包数据';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL PRODUCT LEDGER ASSERTIONS PASSED: role=% mode=% groups=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed;
END
$product_test$;

SET CONSTRAINTS ALL IMMEDIATE;

ROLLBACK;
