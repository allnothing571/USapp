-- =====================================================================
-- database/tests/avatar_catalog_test.sql
--
-- Phase 4 B1: database-side tests for 0044_avatar_catalog (docs/phase4-avatar-shop-design.md). Requires 0044 on
-- usapp_test; refuses any other database before writing fixtures. Run twice:
--   1) as usapp_api: the registry cannot be written (42501); UPDATE / DELETE / TRUNCATE on every catalog table
--      expect 42501; write functions refuse unknown assets and starters. Registry fixtures need the maintenance
--      role, so the guard cases run only in pass 2.
--   2) as the maintenance superuser: append-only expects UA001, and every guard (UA002-UA004) and function answer.
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $avatar_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_cases  integer := 0;
    tag      text := substr(md5(random()::text), 1, 8);
    adm      uuid;
    off_adm  uuid;
    k        text;
    fp       text := repeat('a', 64);
    fp2      text := repeat('b', 64);
    r        jsonb;
    hair_id  uuid;
    tee_id   uuid;
    smile_id uuid;
    hat_id   uuid;
    app_or   text;
    rec      record;
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
    IF to_regclass('public.avatar_starter_configs') IS NULL THEN
        RAISE EXCEPTION '缺少 0044_avatar_catalog';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UA001' END;

    INSERT INTO admin_users (username, password_hash, status) VALUES ('t_av_' || tag, 'x', 'active') RETURNING id INTO adm;
    INSERT INTO admin_users (username, password_hash, status) VALUES ('t_avd_' || tag, 'x', 'disabled') RETURNING id INTO off_adm;

    -- A. Both roles: unknown assets and starters are answers, not errors; the registry is not the application's.
    r := usapp_avatar_item_create(adm, 'ledger-key-a-' || tag, fp, 'hat', false, 'x', 'hat.none_' || tag || '.v1', 5, true);
    IF r->>'error' IS DISTINCT FROM 'asset_invalid' THEN RAISE EXCEPTION 'A1 失败: %', r; END IF;
    r := usapp_avatar_item_create(adm, 'ledger-key-b-' || tag, fp, 'hat', true, 'x', 'hat.none_' || tag || '.v1', 5, true);
    IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'A2 失败: %', r; END IF;
    r := usapp_avatar_starter_set(adm, 'ledger-key-c-' || tag, fp, (SELECT COALESCE(max(version), 0) FROM avatar_starter_configs),
                                  gen_random_uuid(), gen_random_uuid(), gen_random_uuid());
    IF r->>'error' IS DISTINCT FROM 'starter_invalid' THEN RAISE EXCEPTION 'A3 失败: %', r; END IF;
    r := usapp_avatar_item_revise(adm, 'ledger-key-d-' || tag, fp, gen_random_uuid(), 1, 'x', 'hat.none_' || tag || '.v1', 5, true);
    IF r->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'A4 失败: %', r; END IF;
    n_cases := n_cases + 4;

    FOR rec IN SELECT * FROM (VALUES
        ('A5 registry insert', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top)
             VALUES (%L, 'hat', %L, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true)$s$,
             'hat.app_' || tag || '.v1', 'hat.app_' || tag), CASE WHEN is_app THEN '42501' ELSE NULL END),
        ('A6 update items', 'UPDATE avatar_items SET starter = true', app_or),
        ('A7 delete revisions', 'DELETE FROM avatar_item_revisions', app_or),
        -- 0046 adds avatar_loadout_events referencing the configurations: listed after them, no CASCADE, so the expected
        -- refusal is still the append-only trigger (or the privilege barrier).
        ('A8 truncate configs', CASE WHEN to_regclass('public.avatar_loadout_events') IS NULL THEN 'TRUNCATE avatar_starter_configs'
                                     ELSE 'TRUNCATE avatar_starter_configs, avatar_loadout_events' END, app_or),
        ('A9 delete assets', 'DELETE FROM avatar_assets', app_or)
    ) AS t(label, stmt, want) LOOP
        IF rec.want IS NULL THEN CONTINUE; END IF;
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
        -- B. Registry fixtures (maintenance only), then registry guards.
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers) VALUES
            ('hair.l_' || tag || '.v1', 'hair', 'hair.l_' || tag, 1, 'svg',
             jsonb_build_object('back', repeat('a', 64), 'front', repeat('b', 64), 'front_under_hat', repeat('c', 64))),
            ('clothes.l_' || tag || '.v1', 'clothes', 'clothes.l_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64))),
            ('expression.l_' || tag || '.v1', 'expression', 'expression.l_' || tag, 1, 'webp', jsonb_build_object('main', repeat('d', 64)));
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES
            ('hat.l_' || tag || '.v1', 'hat', 'hat.l_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true),
            ('hat.l_' || tag || '.v2', 'hat', 'hat.l_' || tag, 2, 'svg', jsonb_build_object('main', repeat('e', 64)), true);
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES
            ('hat.m_' || tag || '.v1', 'hat', 'hat.m_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), false);

        FOR rec IN SELECT * FROM (VALUES
            ('B1 revision gap', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top)
                 VALUES (%L, 'hat', %L, 4, 'svg', jsonb_build_object('main', repeat('d', 64)), true)$s$, 'hat.l_' || tag || '.v4', 'hat.l_' || tag), 'UA002'),
            ('B2 hint change', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top)
                 VALUES (%L, 'hat', %L, 3, 'svg', jsonb_build_object('main', repeat('d', 64)), false)$s$, 'hat.l_' || tag || '.v3', 'hat.l_' || tag), 'UA002'),
            ('B3 format change', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top)
                 VALUES (%L, 'hat', %L, 3, 'webp', jsonb_build_object('main', repeat('d', 64)), true)$s$, 'hat.l_' || tag || '.v3', 'hat.l_' || tag), 'UA002'),
            ('B4 hair layers', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES (%L, 'hair', %L, 1, 'svg', jsonb_build_object('main', repeat('d', 64)))$s$, 'hair.x_' || tag || '.v1', 'hair.x_' || tag), '23514'),
            ('B5 bad hash', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES (%L, 'clothes', %L, 1, 'svg', jsonb_build_object('main', 'abc'))$s$, 'clothes.x_' || tag || '.v1', 'clothes.x_' || tag), '23514'),
            ('B6 url key', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES ('https://cdn/x.v1', 'clothes', %L, 1, 'svg', jsonb_build_object('main', repeat('d', 64)))$s$, 'clothes.y_' || tag), '23514'),
            ('B7 path family', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES (%L, 'clothes', %L, 1, 'svg', jsonb_build_object('main', repeat('d', 64)))$s$, 'clothes/../x.v1', 'clothes/../x'), '23514'),
            ('B8 hat without hint', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES (%L, 'hat', %L, 1, 'svg', jsonb_build_object('main', repeat('d', 64)))$s$, 'hat.n_' || tag || '.v1', 'hat.n_' || tag), '23514'),
            ('B9 png', format($s$INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers)
                 VALUES (%L, 'clothes', %L, 1, 'png', jsonb_build_object('main', repeat('d', 64)))$s$, 'clothes.z_' || tag || '.v1', 'clothes.z_' || tag), '23514')
        ) AS t(label, stmt, want) LOOP
            BEGIN
                EXECUTE rec.stmt;
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = rec.label || ' 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> rec.want THEN RAISE EXCEPTION '% 失败：期望 %，实际 %', rec.label, rec.want, SQLSTATE; END IF;
            END;
            n_cases := n_cases + 1;
        END LOOP;

        -- C. Functions: create, replay, key reuse, family uniqueness, revise rules, starter rules, audit.
        r := usapp_avatar_item_create(adm, 'ledger-hat-' || tag, fp, 'hat', false, '帽子', 'hat.l_' || tag || '.v1', 300, true);
        IF r->>'result' IS DISTINCT FROM 'created' THEN RAISE EXCEPTION 'C1 失败: %', r; END IF;
        hat_id := (r->>'item_id')::uuid;
        r := usapp_avatar_item_create(adm, 'ledger-hat-' || tag, fp, 'hat', false, '帽子', 'hat.l_' || tag || '.v1', 300, true);
        IF r->>'result' IS DISTINCT FROM 'replay' OR (r->>'item_id')::uuid IS DISTINCT FROM hat_id THEN RAISE EXCEPTION 'C2 失败: %', r; END IF;
        r := usapp_avatar_item_create(adm, 'ledger-hat-' || tag, fp2, 'hat', false, '帽子', 'hat.l_' || tag || '.v1', 301, true);
        IF r->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'C3 失败: %', r; END IF;
        r := usapp_avatar_item_create(adm, 'ledger-hat2-' || tag, fp, 'hat', false, '帽子', 'hat.l_' || tag || '.v2', 300, true);
        IF r->>'error' IS DISTINCT FROM 'asset_invalid' THEN RAISE EXCEPTION 'C4 同族第二件 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev-' || tag, fp, hat_id, 2, '帽子', 'hat.l_' || tag || '.v2', 300, true);
        IF r->>'error' IS DISTINCT FROM 'conflict' OR (r->>'current_version')::int <> 1 THEN RAISE EXCEPTION 'C5 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev-' || tag, fp, hat_id, 1, '帽子', 'hat.m_' || tag || '.v1', 300, true);
        IF r->>'error' IS DISTINCT FROM 'asset_invalid' THEN RAISE EXCEPTION 'C6 他族 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev-' || tag, fp, hat_id, 1, '帽子', 'hat.l_' || tag || '.v1', 300, true);
        IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'C7 无变化 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev-' || tag, fp, hat_id, 1, '帽子', 'hat.l_' || tag || '.v2', NULL, true);
        IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'C8 付费改免费 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev-' || tag, fp, hat_id, 1, '新帽子', 'hat.l_' || tag || '.v2', 350, false);
        IF r->>'result' IS DISTINCT FROM 'created' OR (r->>'version')::int <> 2 THEN RAISE EXCEPTION 'C9 失败: %', r; END IF;
        r := usapp_avatar_item_revise(adm, 'ledger-rev2-' || tag, fp, hat_id, 2, '新帽子', 'hat.l_' || tag || '.v1', 350, true);
        IF r->>'error' IS DISTINCT FROM 'asset_invalid' THEN RAISE EXCEPTION 'C10 退回旧版 失败: %', r; END IF;
        r := usapp_avatar_starter_set(adm, 'ledger-rev-' || tag, fp, 0, hat_id, hat_id, hat_id);
        IF r->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'C11 失败: %', r; END IF;

        r := usapp_avatar_item_create(adm, 'ledger-hair-' || tag, fp, 'hair', true, '短发', 'hair.l_' || tag || '.v1', NULL, true);
        hair_id := (r->>'item_id')::uuid;
        r := usapp_avatar_item_create(adm, 'ledger-tee-' || tag, fp, 'clothes', true, 'T恤', 'clothes.l_' || tag || '.v1', NULL, true);
        tee_id := (r->>'item_id')::uuid;
        r := usapp_avatar_item_create(adm, 'ledger-smile-' || tag, fp, 'expression', true, '微笑', 'expression.l_' || tag || '.v1', NULL, false);
        smile_id := (r->>'item_id')::uuid;
        IF hair_id IS NULL OR tee_id IS NULL OR smile_id IS NULL THEN RAISE EXCEPTION 'C12 初始装扮夹具失败'; END IF;
        r := usapp_avatar_starter_set(adm, 'ledger-st1-' || tag, fp, (SELECT COALESCE(max(version), 0) FROM avatar_starter_configs),
                                      hat_id, tee_id, smile_id);
        IF r->>'error' IS DISTINCT FROM 'starter_invalid' THEN RAISE EXCEPTION 'C13 失败: %', r; END IF;
        r := usapp_avatar_starter_set(adm, 'ledger-st2-' || tag, fp, (SELECT COALESCE(max(version), 0) FROM avatar_starter_configs),
                                      hair_id, tee_id, smile_id);
        IF r->>'result' IS DISTINCT FROM 'created' THEN RAISE EXCEPTION 'C14 失败: %', r; END IF;
        IF (SELECT count(*) FROM audit_logs WHERE actor_type = 'admin' AND actor_id = adm) <> 6 THEN
            RAISE EXCEPTION 'C15 审计条数不符: %', (SELECT count(*) FROM audit_logs WHERE actor_id = adm);
        END IF;
        n_cases := n_cases + 15;

        -- D. Direct inserts the functions never make: every guard refuses.
        FOR rec IN SELECT * FROM (VALUES
            ('D1 version gap', format($s$INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                 idempotency_key, request_fingerprint) VALUES (%L, 5, 'x', %L, 5, true, %L, %L, %L)$s$, hat_id, 'hat.l_' || tag || '.v2', adm, 'ledger-d1-' || tag, fp), 'UA003'),
            ('D2 foreign family', format($s$INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                 idempotency_key, request_fingerprint) VALUES (%L, 3, 'x', %L, 5, true, %L, %L, %L)$s$, hat_id, 'hat.m_' || tag || '.v1', adm, 'ledger-d2-' || tag, fp), 'UA003'),
            ('D3 starter priced', format($s$INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                 idempotency_key, request_fingerprint) VALUES (%L, 2, 'x', %L, 5, true, %L, %L, %L)$s$, hair_id, 'hair.l_' || tag || '.v1', adm, 'ledger-d3-' || tag, fp), 'UA003'),
            ('D4 disabled admin', format($s$INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                 idempotency_key, request_fingerprint) VALUES (%L, 3, 'x', %L, 5, true, %L, %L, %L)$s$, hat_id, 'hat.l_' || tag || '.v2', off_adm, 'ledger-d4-' || tag, fp), 'UA003'),
            ('D5 price range', format($s$INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                 idempotency_key, request_fingerprint) VALUES (%L, 3, 'x', %L, 10000, true, %L, %L, %L)$s$, hat_id, 'hat.l_' || tag || '.v2', adm, 'ledger-d5-' || tag, fp), '23514'),
            ('D6 starter non-starter', format($s$INSERT INTO avatar_starter_configs (version, hair_item_id, clothes_item_id, expression_item_id,
                 admin_user_id, idempotency_key, request_fingerprint) VALUES ((SELECT max(version) + 1 FROM avatar_starter_configs), %L, %L, %L, %L, %L, %L)$s$,
                 hat_id, tee_id, smile_id, adm, 'ledger-d6-' || tag, fp), 'UA004'),
            ('D7 starter gap', format($s$INSERT INTO avatar_starter_configs (version, hair_item_id, clothes_item_id, expression_item_id,
                 admin_user_id, idempotency_key, request_fingerprint) VALUES ((SELECT max(version) + 2 FROM avatar_starter_configs), %L, %L, %L, %L, %L, %L)$s$,
                 hair_id, tee_id, smile_id, adm, 'ledger-d7-' || tag, fp), 'UA004'),
            ('D8 disabled creator', format($s$INSERT INTO avatar_items (category, starter, family, created_by) VALUES ('hat', false, %L, %L)$s$,
                 'hat.zz_' || tag, off_adm), 'UA003'),
            ('D9 item without revision', format($s$DO $x$ BEGIN INSERT INTO avatar_items (category, starter, family, created_by)
                 VALUES ('hat', false, %L, %L); SET CONSTRAINTS avatar_items_require_revision IMMEDIATE; END $x$$s$, 'hat.m_' || tag, adm), 'UA003'),
            ('D10 append-only items', format('UPDATE avatar_items SET category = %L WHERE id = %L', 'clothes', hat_id), 'UA001'),
            ('D11 append-only registry', format('UPDATE avatar_assets SET layers = layers WHERE family = %L', 'hat.l_' || tag), 'UA001')
        ) AS t(label, stmt, want) LOOP
            BEGIN
                EXECUTE rec.stmt;
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = rec.label || ' 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> rec.want THEN RAISE EXCEPTION '% 失败：期望 %，实际 %', rec.label, rec.want, SQLSTATE; END IF;
            END;
            n_cases := n_cases + 1;
        END LOOP;
        SET CONSTRAINTS avatar_items_require_revision IMMEDIATE;
        SET CONSTRAINTS avatar_items_require_revision DEFERRED;
    END IF;

    IF EXISTS (SELECT 1 FROM wallet_operations WHERE source_type LIKE 'avatar%') THEN
        RAISE EXCEPTION 'E1 失败：目录写入不得触及钱包';
    END IF;

    RAISE NOTICE 'ALL AVATAR CATALOG ASSERTIONS PASSED: role=% mode=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_cases;
END
$avatar_test$;

ROLLBACK;
