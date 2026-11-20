-- =====================================================================
-- database/tests/avatar_purchase_test.sql
--
-- Phase 4 B2: database-side tests for 0045_avatar_purchases (docs/phase4-avatar-shop-design.md Section 6). Requires 0045 on
-- usapp_test; refuses any other database before writing fixtures. Run twice:
--   1) as usapp_api: purchases cannot be updated or deleted (42501), an unknown posting type is refused (23514), and the
--      purchase function refuses an unknown item and a caller outside a couple. Funded fixtures need the maintenance role.
--   2) as the maintenance superuser: one sunshine and one flower credit are written with triggers off (a fixture only, so no task is needed);
--      then the purchase function's answers, every guard (UA005), every deferred link (UA006: wrong amount, wrong asset,
--      extra entry, missing purchase, wrong source) and append-only (UA001).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $avatar_purchase_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_cases  integer := 0;
    tag      text := substr(md5(random()::text), 1, 8);
    fp       text := repeat('a', 64);
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    loner uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    adm uuid;
    item uuid;
    item2 uuid;
    starter_item uuid;
    credit uuid := gen_random_uuid();
    r jsonb;
    app_or text;
    rec record;
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
    IF to_regclass('public.avatar_purchases') IS NULL THEN
        RAISE EXCEPTION '缺少 0045_avatar_purchases';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UA001' END;

    INSERT INTO users (id, email_canonical, email_display) VALUES
        (ua, 'b2_a_' || tag || '@example.com', 'b2_a_' || tag || '@example.com'),
        (ub, 'b2_b_' || tag || '@example.com', 'b2_b_' || tag || '@example.com'),
        (loner, 'b2_l_' || tag || '@example.com', 'b2_l_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'b2-psql', ua);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2);

    -- A. Both roles: refusals that need no catalog fixture, and the closed set of posting types.
    r := usapp_avatar_purchase(ub, 'b2-ledger-nf-' || tag, fp, gen_random_uuid(), 1);
    IF r->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'A1 失败: %', r; END IF;
    r := usapp_avatar_purchase(loner, 'b2-ledger-ns-' || tag, fp, gen_random_uuid(), 1);
    IF r->>'error' IS DISTINCT FROM 'no_space' THEN RAISE EXCEPTION 'A2 失败: %', r; END IF;
    n_cases := n_cases + 2;
    FOR rec IN SELECT * FROM (VALUES
        ('A3 unknown posting type', format($s$INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, source_type, source_id)
             VALUES (%L, %L, 'AVATAR_GIFT', 'avatar_purchase', 'x')$s$, c1, ub), '23514'),
        ('A4 update purchases', 'UPDATE avatar_purchases SET price_sunshine = 1', app_or),
        ('A5 delete purchases', 'DELETE FROM avatar_purchases', app_or),
        ('A6 truncate purchases', 'TRUNCATE avatar_purchases', app_or)
    ) AS t(label, stmt, want) LOOP
        BEGIN
            EXECUTE rec.stmt;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = rec.label || ' 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> rec.want THEN
                    RAISE EXCEPTION '% 失败：身份 % 期望 %，实际 %', rec.label, run_role, rec.want, SQLSTATE;
                END IF;
        END;
        n_cases := n_cases + 1;
    END LOOP;

    IF NOT is_app THEN
        -- B. Catalog fixtures through the B1 functions; credits of 500 sunshine and 100 flowers with triggers off (fixture only).
        INSERT INTO admin_users (username, password_hash, status) VALUES ('t_b2_' || tag, 'x', 'active') RETURNING id INTO adm;
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES
            ('hat.b2a_' || tag || '.v1', 'hat', 'hat.b2a_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true),
            ('hat.b2b_' || tag || '.v1', 'hat', 'hat.b2b_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true),
            ('hat.b2s_' || tag || '.v1', 'hat', 'hat.b2s_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true);
        item := (usapp_avatar_item_create(adm, 'b2-ledger-i1-' || tag, fp, 'hat', false, '帽一', 'hat.b2a_' || tag || '.v1', 120, true)->>'item_id')::uuid;
        item2 := (usapp_avatar_item_create(adm, 'b2-ledger-i2-' || tag, fp, 'hat', false, '帽二', 'hat.b2b_' || tag || '.v1', 400, true)->>'item_id')::uuid;
        starter_item := (usapp_avatar_item_create(adm, 'b2-ledger-i3-' || tag, fp, 'hat', true, '帽三', 'hat.b2s_' || tag || '.v1', NULL, true)->>'item_id')::uuid;
        IF item IS NULL OR item2 IS NULL OR starter_item IS NULL THEN RAISE EXCEPTION 'B0 夹具失败'; END IF;
        PERFORM set_config('session_replication_role', 'replica', true);
        INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'sunshine', c1);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (credit, c1, ub, 'TASK_REWARD', 'task', 'b2-ledger-' || tag);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (credit, c1, ub, 'sunshine', 1, 500, 500);
        INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'flower', c1);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (credit, c1, ub, 'flower', 1, 100, 100);
        PERFORM set_config('session_replication_role', 'origin', true);

        -- C. The function: purchase, replay, key reuse and each refusal; exact balance after.
        r := usapp_avatar_purchase(ub, 'b2-ledger-p1-' || tag, fp, item, 1);
        IF r->>'result' IS DISTINCT FROM 'created' THEN RAISE EXCEPTION 'C1 失败: %', r; END IF;
        IF (SELECT balance_after FROM wallet_ledger_entries WHERE user_id = ub AND asset_type = 'sunshine' ORDER BY seq DESC LIMIT 1) <> 380 THEN
            RAISE EXCEPTION 'C2 余额不符';
        END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p1-' || tag, fp, item, 1);
        IF r->>'result' IS DISTINCT FROM 'replay' THEN RAISE EXCEPTION 'C3 失败: %', r; END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p1-' || tag, repeat('b', 64), item, 1);
        IF r->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'C4 失败: %', r; END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p2-' || tag, fp, item, 1);
        IF r->>'error' IS DISTINCT FROM 'already_owned' THEN RAISE EXCEPTION 'C5 失败: %', r; END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p3-' || tag, fp, item2, 1);
        IF r->>'error' IS DISTINCT FROM 'insufficient' THEN RAISE EXCEPTION 'C6 失败: %', r; END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p4-' || tag, fp, item2, 2);
        IF r->>'error' IS DISTINCT FROM 'conflict' OR (r->>'current_version')::int <> 1 THEN RAISE EXCEPTION 'C7 失败: %', r; END IF;
        r := usapp_avatar_purchase(ub, 'b2-ledger-p5-' || tag, fp, starter_item, 1);
        IF r->>'error' IS DISTINCT FROM 'starter' THEN RAISE EXCEPTION 'C8 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'b2-ledger-rv-' || tag, fp, item2, 1, '帽二', 'hat.b2b_' || tag || '.v1', 400, false);
        r := usapp_avatar_purchase(ub, 'b2-ledger-p6-' || tag, fp, item2, 2);
        IF r->>'error' IS DISTINCT FROM 'unlisted' THEN RAISE EXCEPTION 'C9 失败: %', r; END IF;
        IF (SELECT count(*) FROM avatar_purchases WHERE user_id = ub) <> 1
           OR (SELECT count(*) FROM wallet_operations WHERE actor_user_id = ub AND operation_type = 'AVATAR_PURCHASE') <> 1
           OR (SELECT count(*) FROM wallet_ledger_entries WHERE user_id = ub AND asset_type = 'flower') <> 1
           OR (SELECT count(*) FROM audit_logs WHERE actor_id = ub AND action = 'avatar.purchased') <> 1 THEN
            RAISE EXCEPTION 'C10 写入数量不符';
        END IF;
        n_cases := n_cases + 10;

        -- D. Forged rows: every guard and deferred link refuses. Each case builds its own operation and entries with a correct
        -- ledger chain, then checks deferred constraints immediately inside its own block.
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES
            ('hat.b2c_' || tag || '.v1', 'hat', 'hat.b2c_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true);
        item := (usapp_avatar_item_create(adm, 'b2-ledger-i4-' || tag, fp, 'hat', false, '帽四', 'hat.b2c_' || tag || '.v1', 50, true)->>'item_id')::uuid;
        FOR rec IN SELECT * FROM (VALUES
            ('D1 wrong amount', -1, 50, 'sunshine', true, 'avatar_purchase', 50, false, 'UA006'),
            ('D2 wrong asset', -50, 50, 'flower', true, 'avatar_purchase', 50, false, 'UA006'),
            ('D3 extra entry', -50, 50, 'sunshine', true, 'avatar_purchase', 50, true, 'UA006'),
            ('D4 no purchase', -50, 50, 'sunshine', false, 'avatar_purchase', 50, false, 'UA006'),
            ('D5 snapshot price', -50, 50, 'sunshine', true, 'avatar_purchase', 49, false, 'UA005'),
            ('D6 wrong source type', -50, 50, 'sunshine', false, 'other', 50, false, 'UA006')
        ) AS t(label, amount, price, asset, with_purchase, source_type, snapshot_price, extra, want) LOOP
            BEGIN
                EXECUTE format($d$DO $x$
                DECLARE op uuid := gen_random_uuid(); pid uuid := gen_random_uuid(); s bigint; b bigint;
                BEGIN
                    SET CONSTRAINTS ALL DEFERRED;
                    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint)
                        VALUES (op, %1$L, %2$L, 'AVATAR_PURCHASE', %3$L, pid::text, 'b2-forged-' || pid, %4$L);
                    IF %5$L::boolean THEN
                        INSERT INTO avatar_purchases (id, couple_id, user_id, item_id, item_version, name, category, asset_key, price_sunshine,
                                                      operation_id, idempotency_key, request_fingerprint)
                            VALUES (pid, %1$L, %2$L, %6$L, 1, '帽四', 'hat', %7$L, %8$L, op, 'b2-forged-' || pid, %4$L);
                    END IF;
                    SELECT COALESCE(max(seq), 0), COALESCE((SELECT balance_after FROM wallet_ledger_entries WHERE user_id = %2$L AND asset_type = %9$L
                           ORDER BY seq DESC LIMIT 1), 0) INTO s, b FROM wallet_ledger_entries WHERE user_id = %2$L AND asset_type = %9$L;
                    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
                        VALUES (op, %1$L, %2$L, %9$L, s + 1, %10$s, b + %10$s);
                    IF %11$L::boolean THEN
                        -- One more entry on the same operation: the buyer's flower debit of 1, on its own account chain.
                        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
                            SELECT op, %1$L, %2$L, 'flower', max(seq) + 1, -1,
                                   (SELECT balance_after FROM wallet_ledger_entries WHERE user_id = %2$L AND asset_type = 'flower' ORDER BY seq DESC LIMIT 1) - 1
                              FROM wallet_ledger_entries WHERE user_id = %2$L AND asset_type = 'flower';
                    END IF;
                    SET CONSTRAINTS ALL IMMEDIATE;
                END $x$$d$, c1, ub, rec.source_type, fp, rec.with_purchase, item, 'hat.b2c_' || tag || '.v1', rec.snapshot_price,
                    rec.asset, rec.amount, rec.extra);
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = rec.label || ' 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> rec.want THEN RAISE EXCEPTION '% 失败：期望 %，实际 % (%)', rec.label, rec.want, SQLSTATE, SQLERRM; END IF;
            END;
            n_cases := n_cases + 1;
        END LOOP;
        SET CONSTRAINTS ALL DEFERRED;

        -- E. A starter item is never purchased, even by a forged row; the snapshot must be the current listed revision.
        BEGIN
            EXECUTE format($d$DO $x$
            DECLARE op uuid := gen_random_uuid(); pid uuid := gen_random_uuid();
            BEGIN
                INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint)
                    VALUES (op, %1$L, %2$L, 'AVATAR_PURCHASE', 'avatar_purchase', pid::text, 'b2-forged-' || pid, %3$L);
                INSERT INTO avatar_purchases (id, couple_id, user_id, item_id, item_version, name, category, asset_key, price_sunshine,
                                              operation_id, idempotency_key, request_fingerprint)
                    VALUES (pid, %1$L, %2$L, %4$L, 1, '帽三', 'hat', %5$L, 1, op, 'b2-forged-' || pid, %3$L);
            END $x$$d$, c1, ub, fp, starter_item, 'hat.b2s_' || tag || '.v1');
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E1 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UA005' THEN RAISE EXCEPTION 'E1 失败：期望 UA005，实际 %', SQLSTATE; END IF;
        END;
        n_cases := n_cases + 1;
        SET CONSTRAINTS ALL IMMEDIATE;  -- every valid row of this pass still satisfies every deferred link
        SET CONSTRAINTS ALL DEFERRED;
    END IF;

    RAISE NOTICE 'ALL AVATAR PURCHASE ASSERTIONS PASSED: role=% mode=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_cases;
END
$avatar_purchase_test$;

ROLLBACK;
