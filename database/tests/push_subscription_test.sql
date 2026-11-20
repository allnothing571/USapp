-- =====================================================================
-- database/tests/push_subscription_test.sql
--
-- P2-5a: database-side tests for push subscription generations, revocations and
-- registration commands (docs/phase2-web-push.md Section 4). Requires 0022 on usapp_test;
-- refuses any other database before writing fixtures. Run twice:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UY001 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Valid fixtures are
-- written with deferred links, then SET CONSTRAINTS ALL IMMEDIATE checks them and makes
-- every later statement check its deferred links at statement end.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $push_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_passed integer := 0;
    n_cases  integer := 0;
    initial_notices bigint;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    sa uuid := gen_random_uuid();   -- ua valid session
    sa2 uuid := gen_random_uuid();  -- ua second valid session
    sx uuid := gen_random_uuid();   -- ua expired session
    sb uuid := gen_random_uuid();   -- ub valid session
    sc uuid := gen_random_uuid();   -- uc valid session (other space)
    g1 uuid := gen_random_uuid();
    g3 uuid := gen_random_uuid();
    g4 uuid := gen_random_uuid();
    g5 uuid := gen_random_uuid();
    e1 text := 'https://fcm.googleapis.com/fcm/send/p25a-' || substr(md5(random()::text), 1, 12);
    e_free text := 'https://web.push.apple.com/p25a-' || substr(md5(random()::text), 1, 12);
    p1 text := 'B' || repeat('A', 86);
    p2 text := 'B' || repeat('C', 86);
    au text := repeat('A', 22);
    vk text := repeat('a', 64);
    tag text := substr(md5(random()::text), 1, 8);
    fp  text := md5(random()::text) || md5(random()::text);
    gb uuid;
    eb text;
    app_or text;
    rec record;
    i integer;
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
    IF to_regclass('public.push_registration_commands') IS NULL THEN
        RAISE EXCEPTION '缺少推送订阅表：请先在测试库应用 0022_push_subscriptions';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UY001' END;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'p25a_psql_a_' || tag || '@example.com', 'p25a_psql_a_' || tag || '@example.com'),
               (ub, 'p25a_psql_b_' || tag || '@example.com', 'p25a_psql_b_' || tag || '@example.com'),
               (uc, 'p25a_psql_c_' || tag || '@example.com', 'p25a_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'p25a-psql', ua), (c2, 'p25a-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO sessions (id, token_hash, csrf_token_hash, user_id, is_authenticated, expires_at)
        VALUES (sa, md5(random()::text) || tag, md5(random()::text), ua, true, now() + interval '1 day'),
               (sa2, md5(random()::text) || tag, md5(random()::text), ua, true, now() + interval '1 day'),
               (sb, md5(random()::text) || tag, md5(random()::text), ub, true, now() + interval '1 day'),
               (sc, md5(random()::text) || tag, md5(random()::text), uc, true, now() + interval '1 day');
    INSERT INTO sessions (id, token_hash, csrf_token_hash, user_id, is_authenticated, created_at, expires_at)
        VALUES (sx, md5(random()::text) || tag, md5(random()::text), ua, true, now() - interval '2 days',
                now() - interval '1 day');

    SELECT count(*) INTO initial_notices FROM notifications WHERE couple_id IN(c1,c2);

    -- A. K1 -> G1 created; K2 -> already_active G1; revoke G1; K3 -> G3; replacement G3 -> G4
    --    (new keys); re-own G4 -> G5 by uc (other space); ub fills the device cap (10).
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (g1, ua, c1, sa, e1, encode(sha256(convert_to(e1, 'UTF8')), 'hex'), p1, au, vk);
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (ua, 'psql-p25a-k1-' || tag, c1, sa, fp, 'created', g1),
               (ua, 'psql-p25a-k2-' || tag, c1, sa, fp, 'already_active', g1);
    INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason)
        VALUES (g1, ua, c1, ua, 'user');
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (g3, ua, c1, sa, e1, encode(sha256(convert_to(e1, 'UTF8')), 'hex'), p1, au, vk);
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (ua, 'psql-p25a-k3-' || tag, c1, sa, fp, 'created', g3);
    INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason, caused_by)
        VALUES (g3, ua, c1, ua, 'replaced', g4);
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (g4, ua, c1, sa, e1, encode(sha256(convert_to(e1, 'UTF8')), 'hex'), p2, au, vk);
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (ua, 'psql-p25a-k4-' || tag, c1, sa, fp, 'created', g4);
    INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason, caused_by)
        VALUES (g4, ua, c2, uc, 'reowned', g5);
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (g5, uc, c2, sc, e1, encode(sha256(convert_to(e1, 'UTF8')), 'hex'), p2, au, vk);
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (uc, 'psql-p25a-k5-' || tag, c2, sc, fp, 'created', g5);
    FOR i IN 1..10 LOOP
        gb := gen_random_uuid();
        eb := 'https://updates.push.services.mozilla.com/wpush/v2/p25a-' || tag || '-' || i;
        INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
            VALUES (gb, ub, c1, sb, eb, encode(sha256(convert_to(eb, 'UTF8')), 'hex'), p1, au, vk);
        INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
            VALUES (ub, 'psql-p25a-b' || i || '-' || tag, c1, sb, fp, 'created', gb);
    END LOOP;
    SET CONSTRAINTS ALL IMMEDIATE;  -- deferred command / successor links pass for the fixtures
    n_passed := n_passed + 1;

    FOR rec IN
        SELECT * FROM (VALUES
            -- B. Shapes: allowlist, label boundary, port, hash, key length.
            ('B1 not allowlisted', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, 'https://evil.example.com/x',
                encode(sha256(convert_to('https://evil.example.com/x', 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, p1, au, vk), '23514'),
            ('B2 suffix boundary', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, 'https://xpush.apple.com/x',
                encode(sha256(convert_to('https://xpush.apple.com/x', 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, p1, au, vk), '23514'),
            ('B3 explicit port', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, 'https://fcm.googleapis.com:8443/x',
                encode(sha256(convert_to('https://fcm.googleapis.com:8443/x', 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, p1, au, vk), '23514'),
            ('B4 hash mismatch', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L, %L, %L, %L, %L)$s$,
                ua, c1, sa2, e_free, repeat('0', 64), p1, au, vk), '23514'),
            ('B5 short p256dh', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, e_free, e_free, 'BAAA', au, vk), '23514'),
            -- C. Guards: endpoint occupancy, session validity/ownership, device cap, revocation
            --    actor rules, command rules and deferred links.
            ('C1 endpoint occupied', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, e1, e1, p1, au, vk), 'UY002'),
            ('C2 expired session', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sx, e_free, e_free, p1, au, vk), 'UY002'),
            ('C3 another user session', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id,
                endpoint, endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sb, e_free, e_free, p1, au, vk), 'UY002'),
            ('C4 device cap', format($s$INSERT INTO push_subscriptions (user_id, couple_id, session_id, endpoint,
                endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ub, c1, sb, e_free, e_free, p1, au, vk), 'UY002'),
            ('C5 cross-space user-revocation', format($s$INSERT INTO push_subscription_revocations (subscription_id,
                user_id, couple_id, revoked_by, reason) VALUES (%L, %L, %L, %L, 'user')$s$, g5, uc, c1, ub),
             'UY003'),
            ('C5b partner user-revocation', format($s$INSERT INTO push_subscription_revocations (subscription_id,
                user_id, couple_id, revoked_by, reason) VALUES (%L, %L, %L, %L, 'user')$s$, gb, ub, c1, ua),
             '23514'),
            ('C6 second revocation', format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id,
                couple_id, revoked_by, reason) VALUES (%L, %L, %L, %L, 'user')$s$, g1, ua, c1, ua), '23505'),
            ('C7 no-op on revoked generation', format($s$INSERT INTO push_registration_commands (user_id,
                idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id) VALUES (%L, %L,
                %L, %L, %L, 'already_active', %L)$s$, ua, 'psql-p25a-c7-' || tag, c1, sa, fp, g1), 'UY004'),
            ('C8 command session mismatch', format($s$INSERT INTO push_registration_commands (user_id,
                idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id) VALUES (%L, %L,
                %L, %L, %L, 'already_active', %L)$s$, uc, 'psql-p25a-c8-' || tag, c2, sc, fp, g1), 'UY004'),
            ('C9 successor elsewhere', format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id,
                couple_id, revoked_by, reason, caused_by) VALUES (%L, %L, %L, %L, 'replaced', %L)$s$,
                g5, uc, c2, uc, g1), 'UY005'),
            ('C10 generation without command', format($s$INSERT INTO push_subscriptions (user_id, couple_id,
                session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id) VALUES (%L, %L, %L, %L,
                encode(sha256(convert_to(%L, 'UTF8')), 'hex'), %L, %L, %L)$s$,
                ua, c1, sa2, e_free, e_free, p1, au, vk), 'UY005'),
            ('C11 duplicate key', format($s$INSERT INTO push_registration_commands (user_id, idempotency_key,
                couple_id, session_id, request_fingerprint, result, generation_id) VALUES (%L, %L, %L, %L, %L,
                'already_active', %L)$s$, uc, 'psql-p25a-k5-' || tag, c2, sc, fp, g5), '23505'),
            ('C12 reowned by owner', format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id,
                couple_id, revoked_by, reason, caused_by) VALUES (%L, %L, %L, %L, 'reowned', %L)$s$,
                g5, uc, c2, uc, g4), '23514'),
            -- D. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
            ('D1 update generation', format('UPDATE push_subscriptions SET vapid_key_id = vapid_key_id WHERE id = %L',
                g5), app_or),
            ('D2 delete revocation', format('DELETE FROM push_subscription_revocations WHERE subscription_id = %L',
                g1), app_or),
            ('D3 delete command', format('DELETE FROM push_registration_commands WHERE generation_id = %L', g1),
             app_or),
            -- 0023 adds dispatch tables referencing generations: listed last, no CASCADE.
            ('D4 truncate', 'TRUNCATE push_registration_commands, push_subscription_revocations, push_subscriptions'
                || CASE WHEN to_regclass('public.push_deliveries') IS NOT NULL
                    THEN ', push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, '
                         || 'push_delivery_outcomes' ELSE '' END, app_or)
        ) AS t(label, stmt, want)
    LOOP
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
    n_passed := n_passed + 1;

    -- E. Fixture facts: K2 still maps to G1 (revoked); G3 replaced; the endpoint belongs to uc
    --    only; ub holds exactly 10 active generations; subscription operations changed no wallets or notices beyond login fixture effects.
    IF (SELECT generation_id FROM push_registration_commands WHERE user_id = ua AND idempotency_key = 'psql-p25a-k2-' || tag)
           IS DISTINCT FROM g1
       OR (SELECT reason FROM push_subscription_revocations WHERE subscription_id = g3) IS DISTINCT FROM 'replaced'
       OR (SELECT count(*) FROM push_subscriptions g WHERE g.endpoint = e1
            AND NOT EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id)) <> 1
       OR (SELECT count(*) FROM push_subscriptions g WHERE g.user_id = ub
            AND NOT EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id)) <> 10
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id IN (c1, c2))
       OR (SELECT count(*) FROM notifications WHERE couple_id IN(c1,c2)) <> initial_notices THEN
        RAISE EXCEPTION 'E1 失败：推送订阅夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL PUSH SUBSCRIPTION ASSERTIONS PASSED: role=% mode=% groups=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed, n_cases;
END
$push_test$;

ROLLBACK;
