-- =====================================================================
-- database/tests/avatar_loadout_test.sql
--
-- Phase 4 B3: database-side tests for 0046_avatar_loadouts (docs/phase4-avatar-shop-design.md Section 7). Requires 0046 on
-- usapp_test; refuses any other database before writing fixtures. Run twice:
--   1) as usapp_api: when no starter configuration exists yet, a null required slot fails closed (defaults_unavailable) with no
--      event; malformed requests and callers outside a couple are refused; loadouts cannot be updated or deleted (42501).
--   2) as the maintenance superuser: the same first check, then catalog fixtures, one purchase (funded by a fixture credit with
--      triggers off), the save function's answers, every guard refusal (UA007) and append-only (UA001).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $avatar_loadout_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_cases  integer := 0;
    defaults_case text := 'skipped';
    tag      text := substr(md5(random()::text), 1, 8);
    fp       text := repeat('a', 64);
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    loner uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    adm uuid;
    hair1 uuid; hair2 uuid; tee uuid; smile uuid; hat uuid; hat2 uuid;
    credit uuid := gen_random_uuid();
    cfg integer;
    wallet_before bigint;
    r jsonb;
    app_or text;
    rec record;
    all_null jsonb := '{"hair":null,"clothes":null,"expression":null,"hat":null,"handheld":null,"accessory":null}';
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
    IF to_regclass('public.avatar_loadout_events') IS NULL THEN
        RAISE EXCEPTION '缺少 0046_avatar_loadouts';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UA001' END;

    INSERT INTO users (id, email_canonical, email_display) VALUES
        (ua, 'b3_a_' || tag || '@example.com', 'b3_a_' || tag || '@example.com'),
        (ub, 'b3_b_' || tag || '@example.com', 'b3_b_' || tag || '@example.com'),
        (loner, 'b3_l_' || tag || '@example.com', 'b3_l_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'b3-psql', ua);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2);

    -- A. No configuration yet: fail closed, nothing saved. Only meaningful before any configuration exists.
    IF NOT EXISTS (SELECT 1 FROM avatar_starter_configs) THEN
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-nd-' || tag, fp, 0, all_null);
        IF r->>'error' IS DISTINCT FROM 'defaults_unavailable' THEN RAISE EXCEPTION 'A1 失败: %', r; END IF;
        IF EXISTS (SELECT 1 FROM avatar_loadout_events WHERE user_id = ub) THEN RAISE EXCEPTION 'A1 写入了形象'; END IF;
        defaults_case := 'covered';
        n_cases := n_cases + 1;
    END IF;
    r := usapp_avatar_loadout_save(ub, 'b3-ledger-iv-' || tag, fp, 0, '{"hair":null,"clothes":null,"expression":null,"hat":null,"handheld":null}');
    IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'A2 失败: %', r; END IF;
    r := usapp_avatar_loadout_save(ub, 'b3-ledger-iv2-' || tag, fp, 0, all_null || '{"wings":null}');
    IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'A3 失败: %', r; END IF;
    r := usapp_avatar_loadout_save(ub, 'b3-ledger-iv3-' || tag, fp, 0, all_null || '{"hat":"x"}');
    IF r->>'error' IS DISTINCT FROM 'invalid' THEN RAISE EXCEPTION 'A4 失败: %', r; END IF;
    r := usapp_avatar_loadout_save(loner, 'b3-ledger-ns-' || tag, fp, 0, all_null);
    IF r->>'error' IS DISTINCT FROM 'no_space' THEN RAISE EXCEPTION 'A5 失败: %', r; END IF;
    n_cases := n_cases + 4;
    FOR rec IN SELECT * FROM (VALUES
        ('A6 update loadouts', 'UPDATE avatar_loadout_events SET hat_item_id = NULL', app_or),
        ('A7 delete loadouts', 'DELETE FROM avatar_loadout_events', app_or),
        ('A8 truncate loadouts', 'TRUNCATE avatar_loadout_events', app_or)
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
        -- B. Fixtures: starters, a configuration, two paid hats; ub buys one (funded with triggers off, fixture only).
        INSERT INTO admin_users (username, password_hash, status) VALUES ('t_b3_' || tag, 'x', 'active') RETURNING id INTO adm;
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers) VALUES
            ('hair.b3a_' || tag || '.v1', 'hair', 'hair.b3a_' || tag, 1, 'svg', jsonb_build_object('back', repeat('a', 64), 'front', repeat('b', 64), 'front_under_hat', repeat('c', 64))),
            ('hair.b3b_' || tag || '.v1', 'hair', 'hair.b3b_' || tag, 1, 'svg', jsonb_build_object('back', repeat('a', 64), 'front', repeat('b', 64), 'front_under_hat', repeat('c', 64))),
            ('clothes.b3_' || tag || '.v1', 'clothes', 'clothes.b3_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64))),
            ('expression.b3_' || tag || '.v1', 'expression', 'expression.b3_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)));
        INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES
            ('hat.b3a_' || tag || '.v1', 'hat', 'hat.b3a_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true),
            ('hat.b3b_' || tag || '.v1', 'hat', 'hat.b3b_' || tag, 1, 'svg', jsonb_build_object('main', repeat('d', 64)), true);
        hair1 := (usapp_avatar_item_create(adm, 'b3-ledger-h1-' || tag, fp, 'hair', true, '短发', 'hair.b3a_' || tag || '.v1', NULL, true)->>'item_id')::uuid;
        hair2 := (usapp_avatar_item_create(adm, 'b3-ledger-h2-' || tag, fp, 'hair', true, '长发', 'hair.b3b_' || tag || '.v1', NULL, false)->>'item_id')::uuid;
        tee := (usapp_avatar_item_create(adm, 'b3-ledger-t-' || tag, fp, 'clothes', true, 'T恤', 'clothes.b3_' || tag || '.v1', NULL, true)->>'item_id')::uuid;
        smile := (usapp_avatar_item_create(adm, 'b3-ledger-s-' || tag, fp, 'expression', true, '笑', 'expression.b3_' || tag || '.v1', NULL, true)->>'item_id')::uuid;
        hat := (usapp_avatar_item_create(adm, 'b3-ledger-p1-' || tag, fp, 'hat', false, '帽', 'hat.b3a_' || tag || '.v1', 30, true)->>'item_id')::uuid;
        hat2 := (usapp_avatar_item_create(adm, 'b3-ledger-p2-' || tag, fp, 'hat', false, '帽二', 'hat.b3b_' || tag || '.v1', 30, true)->>'item_id')::uuid;
        r := usapp_avatar_starter_set(adm, 'b3-ledger-cfg-' || tag, fp, (SELECT COALESCE(max(version), 0) FROM avatar_starter_configs), hair1, tee, smile);
        cfg := (r->>'version')::int;
        IF cfg IS NULL THEN RAISE EXCEPTION 'B0 夹具失败: %', r; END IF;
        PERFORM set_config('session_replication_role', 'replica', true);
        INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'sunshine', c1);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id) VALUES (credit, c1, ub, 'TASK_REWARD', 'task', 'b3-' || tag);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after) VALUES (credit, c1, ub, 'sunshine', 1, 100, 100);
        PERFORM set_config('session_replication_role', 'origin', true);
        r := usapp_avatar_purchase(ub, 'b3-ledger-buy-' || tag, fp, hat, 1);
        IF r->>'result' IS DISTINCT FROM 'created' THEN RAISE EXCEPTION 'B1 购买失败: %', r; END IF;

        -- C. The function: defaults resolved from one configuration; no wallet row; replay; key reuse; refusals.
        wallet_before := (SELECT count(*) FROM wallet_operations) + (SELECT count(*) FROM wallet_ledger_entries);
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv1-' || tag, fp, 0, all_null || jsonb_build_object('hat', hat::text));
        IF r->>'result' IS DISTINCT FROM 'created' THEN RAISE EXCEPTION 'C1 失败: %', r; END IF;
        IF NOT EXISTS (SELECT 1 FROM avatar_loadout_events WHERE user_id = ub AND version = 1 AND hair_item_id = hair1 AND clothes_item_id = tee
                         AND expression_item_id = smile AND hat_item_id = hat AND handheld_item_id IS NULL AND starter_version = cfg) THEN
            RAISE EXCEPTION 'C2 解析结果不符';
        END IF;
        IF (SELECT count(*) FROM wallet_operations) + (SELECT count(*) FROM wallet_ledger_entries) <> wallet_before THEN
            RAISE EXCEPTION 'C3 保存形象写入了钱包';
        END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv1-' || tag, fp, 0, all_null || jsonb_build_object('hat', hat::text));
        IF r->>'result' IS DISTINCT FROM 'replay' THEN RAISE EXCEPTION 'C4 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv1-' || tag, repeat('b', 64), 0, all_null);
        IF r->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'C5 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv2-' || tag, fp, 0, all_null);
        IF r->>'error' IS DISTINCT FROM 'conflict' OR (r->>'current_version')::int <> 1 THEN RAISE EXCEPTION 'C6 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv3-' || tag, fp, 1, all_null || jsonb_build_object('hat', tee::text));
        IF r->>'error' IS DISTINCT FROM 'slot_invalid' THEN RAISE EXCEPTION 'C7 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv4-' || tag, fp, 1, all_null || jsonb_build_object('hat', gen_random_uuid()::text));
        IF r->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'C8 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv5-' || tag, fp, 1, all_null || jsonb_build_object('hat', hat2::text));
        IF r->>'error' IS DISTINCT FROM 'not_owned' THEN RAISE EXCEPTION 'C9 失败: %', r; END IF;
        r := usapp_avatar_loadout_save(ua, 'b3-ledger-sv6-' || tag, fp, 0, all_null || jsonb_build_object('hat', hat::text));
        IF r->>'error' IS DISTINCT FROM 'not_owned' THEN RAISE EXCEPTION 'C10 伴侣的购买 失败: %', r; END IF;
        -- An unlisted starter stays equippable; naming every required slot records no configuration.
        r := usapp_avatar_loadout_save(ub, 'b3-ledger-sv7-' || tag, fp, 1,
                 jsonb_build_object('hair', hair2::text, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL));
        IF r->>'result' IS DISTINCT FROM 'created'
           OR (SELECT starter_version FROM avatar_loadout_events WHERE user_id = ub AND version = 2) IS NOT NULL THEN
            RAISE EXCEPTION 'C11 失败: %', r;
        END IF;
        IF (SELECT count(*) FROM avatar_loadout_events WHERE user_id IN (ua, ub)) <> 2
           OR (SELECT count(*) FROM audit_logs WHERE action = 'avatar.loadout_saved' AND actor_id = ub) <> 2 THEN
            RAISE EXCEPTION 'C12 事件或审计条数不符';
        END IF;
        n_cases := n_cases + 12;

        -- D. Forged rows: the guard refuses each.
        FOR rec IN SELECT * FROM (VALUES
            ('D1 version gap', ub, 5, hair1, NULL::uuid, jsonb_build_object('hair', hair1::text, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL), NULL::integer),
            ('D2 partner purchase', ua, 1, hair1, hat, jsonb_build_object('hair', hair1::text, 'clothes', tee::text, 'expression', smile::text, 'hat', hat::text, 'handheld', NULL, 'accessory', NULL), NULL),
            ('D3 wrong default', ub, 3, hair2, NULL, jsonb_build_object('hair', NULL, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL), cfg),
            ('D4 default without configuration', ub, 3, hair1, NULL, jsonb_build_object('hair', NULL, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL), NULL),
            ('D5 configuration without default', ub, 3, hair1, NULL, jsonb_build_object('hair', hair1::text, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL), cfg),
            ('D6 stored differs from requested', ub, 3, hair1, NULL, jsonb_build_object('hair', hair1::text, 'clothes', tee::text, 'expression', smile::text, 'hat', hat::text, 'handheld', NULL, 'accessory', NULL), NULL),
            ('D7 wrong category', ub, 3, tee, NULL, jsonb_build_object('hair', tee::text, 'clothes', tee::text, 'expression', smile::text, 'hat', NULL, 'handheld', NULL, 'accessory', NULL), NULL)
        ) AS t(label, who, version, hair, hat_item, requested, starter) LOOP
            BEGIN
                INSERT INTO avatar_loadout_events (user_id, couple_id, version, hair_item_id, clothes_item_id, expression_item_id, hat_item_id,
                                                   requested, starter_version, idempotency_key, request_fingerprint)
                    VALUES (rec.who, c1, rec.version, rec.hair, tee, smile, rec.hat_item, rec.requested, rec.starter,
                            'b3-forged-' || md5(rec.label), fp);
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = rec.label || ' 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> 'UA007' THEN RAISE EXCEPTION '% 失败：期望 UA007，实际 % (%)', rec.label, SQLSTATE, SQLERRM; END IF;
            END;
            n_cases := n_cases + 1;
        END LOOP;
    END IF;

    RAISE NOTICE 'ALL AVATAR LOADOUT ASSERTIONS PASSED: role=% mode=% cases=% defaults_case=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_cases, defaults_case;
END
$avatar_loadout_test$;

ROLLBACK;
