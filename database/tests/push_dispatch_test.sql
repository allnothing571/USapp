-- =====================================================================
-- database/tests/push_dispatch_test.sql
--
-- P2-5b: database-side tests for push dispatch (docs/phase2-web-push.md Sections 5, 6, 13).
-- Requires 0023 on usapp_test; refuses any other database before writing fixtures.
-- Always connect as the maintenance superuser, because only that role can enable control and so
-- build the fixtures; pass the mode:
--   psql -U postgres -d usapp_test -v mode=application -f push_dispatch_test.sql
--   psql -U postgres -d usapp_test -v mode=maintenance -f push_dispatch_test.sql
-- application: assertions run under SET LOCAL ROLE usapp_api (mutations expect 42501);
-- maintenance: assertions run as the superuser (mutations expect UZ001).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing, including control rows.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

SELECT set_config('usapp.p25b_mode', :'mode', true);

DO $dispatch_test$
DECLARE
    mode text := current_setting('usapp.p25b_mode');
    is_app boolean;
    n_groups integer := 0;
    n_cases integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    sa uuid := gen_random_uuid();
    sb uuid := gen_random_uuid();
    sc uuid := gen_random_uuid();
    ga uuid := gen_random_uuid();   -- ua's device
    gb uuid := gen_random_uuid();   -- ub's device, activated before the notices
    gc uuid := gen_random_uuid();   -- uc's device, other space
    glate uuid := gen_random_uuid();-- ub's device, activated after the notices
    r5_task uuid := gen_random_uuid();
    r5_rule uuid;
    r5_occ uuid;
    r5_notice uuid;
    r5_plain uuid;
    r5_stale timestamptz;
    n_old uuid;                     -- notice inserted before the enable
    n uuid[] := ARRAY[]::uuid[];    -- notices ua -> ub after the enable
    m uuid;
    tag text := substr(md5(random()::text), 1, 8);
    fp text := md5(random()::text) || md5(random()::text);
    pk text := 'B' || repeat('A', 86);
    au text := repeat('A', 22);
    vk text := repeat('a', 64);
    app_or text;
    c text[];
    i integer;
    got text;
    cut1 timestamptz;
    cut2 timestamptz;
    rec record;
BEGIN
    IF current_database() IS DISTINCT FROM 'usapp_test' THEN
        RAISE EXCEPTION '拒绝运行：当前数据库为 %，只允许 usapp_test', current_database();
    END IF;
    IF NOT (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        RAISE EXCEPTION '必须以维护超级用户连接，当前身份: %', current_user;
    END IF;
    IF mode NOT IN ('application', 'maintenance') THEN
        RAISE EXCEPTION 'mode 只能是 application 或 maintenance: %', mode;
    END IF;
    IF to_regclass('public.push_dispatch_control') IS NULL THEN
        RAISE EXCEPTION '缺少推送派发表：请先在测试库应用 0023_push_dispatch';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema = 'public' AND table_name = 'push_attempts'
                      AND column_name = 'control_revision') THEN
        RAISE EXCEPTION '缺少 control_revision：请先在测试库应用 0024_push_dispatch_repairs';
    END IF;
    is_app := mode = 'application';
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UZ001' END;

    -- ------------------------------------------------------------ fixtures (maintenance)
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'p25b_psql_a_' || tag || '@example.com', 'p25b_psql_a_' || tag || '@example.com'),
               (ub, 'p25b_psql_b_' || tag || '@example.com', 'p25b_psql_b_' || tag || '@example.com'),
               (uc, 'p25b_psql_c_' || tag || '@example.com', 'p25b_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'p25b-psql', ua), (c2, 'p25b-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO sessions (id, token_hash, csrf_token_hash, user_id, is_authenticated, expires_at)
        VALUES (sa, md5(random()::text) || tag, md5(random()::text), ua, true, now() + interval '1 day'),
               (sb, md5(random()::text) || tag, md5(random()::text), ub, true, now() + interval '1 day'),
               (sc, md5(random()::text) || tag, md5(random()::text), uc, true, now() + interval '1 day');

    IF (SELECT s.state FROM usapp_push_control_state() s) <> 'disabled' THEN
        INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'test_fixture');
    END IF;

    m := gen_random_uuid();
    INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
        VALUES (m, c1, ua, ub, 'p25b old', 'psql-p25b-old-' || tag, fp);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id)
        VALUES (c1, ua, ub, 'message.created', 'message', m, m) RETURNING id INTO n_old;
    INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested,
                                      recipient_id, notification_id, preference_seq)
        VALUES (c1, ua, 'message_create', m, true, true, ub, n_old, 1);

    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (ga, ua, c1, sa, 'https://fcm.googleapis.com/fcm/send/p25b-a-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/p25b-a-' || tag, 'UTF8')), 'hex'), pk, au, vk),
               (gb, ub, c1, sb, 'https://fcm.googleapis.com/fcm/send/p25b-b-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/p25b-b-' || tag, 'UTF8')), 'hex'), pk, au, vk),
               (gc, uc, c2, sc, 'https://fcm.googleapis.com/fcm/send/p25b-c-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/p25b-c-' || tag, 'UTF8')), 'hex'), pk, au, vk);
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'test_fixture');
    SELECT s.cutover INTO cut1 FROM usapp_push_control_state() s;

    FOR i IN 1..8 LOOP
        m := gen_random_uuid();
        INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (m, c1, ua, ub, 'p25b ' || i, 'psql-p25b-' || i || '-' || tag, fp);
        INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id)
            VALUES (c1, ua, ub, 'message.created', 'message', m, m) RETURNING id INTO rec;
        INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested,
                                          recipient_id, notification_id, preference_seq)
            VALUES (c1, ua, 'message_create', m, true, true, ub, rec.id, i + 1);
        n := n || rec.id;
    END LOOP;

    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (glate, ub, c1, sb, 'https://fcm.googleapis.com/fcm/send/p25b-l-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/p25b-l-' || tag, 'UTF8')), 'hex'), pk, au, vk);

    -- Complete fixtures: each generation has its creating command, so the deferred links can be
    -- made immediate before the append-only group below (TRUNCATE refuses pending trigger events).
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (ua, 'psql-p25b-ga-' || tag, c1, sa, fp, 'created', ga),
               (ub, 'psql-p25b-gb-' || tag, c1, sb, fp, 'created', gb),
               (uc, 'psql-p25b-gc-' || tag, c2, sc, fp, 'created', gc),
               (ub, 'psql-p25b-gl-' || tag, c1, sb, fp, 'created', glate);

    -- ------------------------------------------------------------ A. control role and ordering
    -- Always as the maintenance role: actor attribution and the transition table.
    c := ARRAY[
        format($s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'dispatcher', 'x')$s$), 'UZ002',
        format($s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'again')$s$), 'UZ002',
        format($s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('resume', 'owner', 'not_paused')$s$), 'UZ002',
        format($s$INSERT INTO push_dispatch_control (revision, kind, actor, reason_code) VALUES (999999, 'pause', 'owner', 'bad_rev')$s$), 'UZ002',
        format($s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('pause', 'owner', 'BAD REASON')$s$), '23514'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'A maint case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;

    -- The application role may only pause automatically, only with the two auth reasons.
    EXECUTE 'SET LOCAL ROLE usapp_api';
    c := ARRAY[
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'x')$s$, 'UZ002',
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'x')$s$, 'UZ002',
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('resume', 'owner', 'x')$s$, 'UZ002',
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'dispatcher', 'worker_auth_failed')$s$, 'UZ002',
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('pause', 'owner', 'worker_auth_failed')$s$, 'UZ002',
        $s$INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('pause', 'dispatcher', 'other_reason')$s$, 'UZ002'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'A app case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    -- Pause succeeds from enabled; a second pause is an invalid transition.
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('pause', 'dispatcher', 'worker_auth_failed');
    IF (SELECT s.state FROM usapp_push_control_state() s) <> 'paused' THEN RAISE EXCEPTION 'pause state'; END IF;
    got := 'none';
    BEGIN
        INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('pause', 'dispatcher', 'provider_auth_failed');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ002' THEN RAISE EXCEPTION 'second pause: %', got; END IF;
    n_cases := n_cases + 1;
    EXECUTE 'RESET ROLE';
    -- usapp_push has no access at all.
    FOR rec IN SELECT t FROM unnest(ARRAY['push_dispatch_control', 'push_deliveries', 'push_attempts',
                                          'push_attempt_observations', 'push_attempt_expiries',
                                          'push_delivery_outcomes']) t LOOP
        IF has_table_privilege('usapp_push', 'public.' || rec.t, 'SELECT')
           OR has_table_privilege('usapp_push', 'public.' || rec.t, 'INSERT') THEN
            RAISE EXCEPTION 'usapp_push can reach %', rec.t;
        END IF;
    END LOOP;
    n_cases := n_cases + 1;
    -- Resume does not move the cutover.
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('resume', 'owner', 'fixed');
    SELECT s.cutover INTO cut2 FROM usapp_push_control_state() s;
    IF cut2 IS DISTINCT FROM cut1 OR (SELECT s.state FROM usapp_push_control_state() s) <> 'enabled' THEN
        RAISE EXCEPTION 'resume moved the cutover or did not enable';
    END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    IF is_app THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;

    -- ------------------------------------------------------------ B. delivery eligibility
    c := ARRAY[
        -- before the cutover
        format($s$INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (%L, %L, %L, %L)$s$, n_old, gb, ub, c1), 'UZ003',
        -- before the generation's activation
        format($s$INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (%L, %L, %L, %L)$s$, n[1], glate, ub, c1), 'UZ003',
        -- generation of another space
        format($s$INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (%L, %L, %L, %L)$s$, n[1], gc, ub, c1), 'UZ003',
        -- generation of the partner, not the recipient (recipient on the generation FK differs)
        format($s$INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (%L, %L, %L, %L)$s$, n[1], ga, ub, c1), '23503',
        -- the notice's actor is not its recipient
        format($s$INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (%L, %L, %L, %L)$s$, n[1], ga, ua, c1), '23503'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'B case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    FOR i IN 1..8 LOOP
        INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (n[i], gb, ub, c1);
    END LOOP;
    got := 'none';
    BEGIN
        INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (n[1], gb, ub, c1);
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> '23505' THEN RAISE EXCEPTION 'duplicate delivery: %', got; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ C. claims
    c := ARRAY[
        -- not contiguous
        format($s$INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until) VALUES (%L, %L, 2, 'i', clock_timestamp() + interval '30 seconds')$s$, n[1], gb), 'UZ004',
        -- a lease longer than 60 s
        format($s$INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until) VALUES (%L, %L, 1, 'i', clock_timestamp() + interval '61 seconds')$s$, n[1], gb), '23514',
        -- no delivery row
        format($s$INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until) VALUES (%L, %L, 1, 'i', clock_timestamp() + interval '30 seconds')$s$, n[1], glate), '23503'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'C case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    -- Four live leases globally. Earlier committed live attempts on usapp_test would only lower
    -- this, so the fifth rejected claim below still proves the bound.
    FOR i IN 1..8 LOOP
        BEGIN
            INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
                VALUES (n[i], gb, 1, 'i', clock_timestamp() + interval '30 seconds');
        EXCEPTION WHEN SQLSTATE 'UZ004' THEN
            n_cases := n_cases + 1;
            EXIT;
        END;
        IF i = 8 THEN RAISE EXCEPTION 'capacity bound was not enforced'; END IF;
    END LOOP;
    IF (SELECT count(*) FROM push_attempts a WHERE a.generation_id = gb) > 4 THEN
        RAISE EXCEPTION 'more than four live claims';
    END IF;
    -- 0024: the guard assigns the claim's control revision; the caller cannot choose it.
    IF EXISTS (SELECT 1 FROM push_attempts a WHERE a.generation_id = gb
                AND a.control_revision IS DISTINCT FROM (SELECT max(r.revision) FROM push_dispatch_control r)) THEN
        RAISE EXCEPTION 'claim without the current control revision';
    END IF;
    n_cases := n_cases + 1;
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until, control_revision)
            VALUES (n[8], gb, 1, 'i', clock_timestamp() + interval '30 seconds', 1);
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'caller-supplied control revision: %', got; END IF;
    n_cases := n_cases + 1;
    -- A second attempt while the first is unresolved.
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
            VALUES (n[1], gb, 2, 'i', clock_timestamp() + interval '30 seconds');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'unresolved previous attempt: %', got; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ D. observations and expiries
    c := ARRAY[
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) VALUES (%L, %L, 1, 'accepted', 'internal')$s$, n[1], gb), '23514',
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) VALUES (%L, %L, 1, 'worker_auth_failed', 'provider')$s$, n[1], gb), '23514',
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, retry_after_s) VALUES (%L, %L, 1, 'transient', 'provider', -1)$s$, n[1], gb), '23514',
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, http_status) VALUES (%L, %L, 1, 'transient', 'provider', 700)$s$, n[1], gb), '23514',
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) VALUES (%L, %L, 9, 'accepted', 'provider')$s$, n[1], gb), '23503',
        -- an expiry before the lease ended
        format($s$INSERT INTO push_attempt_expiries (notification_id, generation_id, attempt_no) VALUES (%L, %L, 1)$s$, n[1], gb), 'UZ005'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'D case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    -- 0024: a lost lease is an internal fact and never spends the budget.
    got := 'none';
    BEGIN
        INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class)
            VALUES (n[3], gb, 1, 'lease_lost_skipped', 'provider');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> '23514' THEN RAISE EXCEPTION 'lease_lost_skipped as provider: %', got; END IF;
    n_cases := n_cases + 1;
    INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class)
        VALUES (n[3], gb, 1, 'lease_lost_skipped', 'internal');
    IF usapp_push_provider_attempts(n[3], gb) <> 0 THEN RAISE EXCEPTION 'lease_lost_skipped spent budget'; END IF;
    n_cases := n_cases + 1;
    -- First observation stands; a duplicate conflicts.
    INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, http_status)
        VALUES (n[1], gb, 1, 'accepted', 'provider', 201);
    got := 'none';
    BEGIN
        INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class)
            VALUES (n[1], gb, 1, 'transient', 'provider');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> '23505' THEN RAISE EXCEPTION 'duplicate observation: %', got; END IF;
    n_cases := n_cases + 1;
    -- A synthetic expiry cannot be written once an actual result exists.
    got := 'none';
    BEGIN
        INSERT INTO push_attempt_expiries (notification_id, generation_id, attempt_no, expired_at)
            VALUES (n[1], gb, 1, clock_timestamp());
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got NOT IN ('UZ005') THEN RAISE EXCEPTION 'expiry after observation: %', got; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ E. outcomes
    c := ARRAY[
        -- a provider verdict without its attempt (the guard fires before the CHECK)
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (%L, %L, 'accepted')$s$, n[2], gb), 'UZ006',
        -- no such observation on this attempt
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) VALUES (%L, %L, 'accepted', 1)$s$, n[2], gb), 'UZ006',
        -- the observation says accepted, not rejected
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) VALUES (%L, %L, 'rejected', 1)$s$, n[1], gb), 'UZ006',
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) VALUES (%L, %L, 'exhausted', 1)$s$, n[1], gb), 'UZ006',
        -- the notice is not before the cutover
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (%L, %L, 'cutover_skipped')$s$, n[2], gb), 'UZ006',
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (%L, %L, 'bogus')$s$, n[2], gb), '23514'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'E case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no)
        VALUES (n[1], gb, 'accepted', 1);
    got := 'none';
    BEGIN
        INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (n[1], gb, 'expired');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> '23505' THEN RAISE EXCEPTION 'terminal outcome rewritten: %', got; END IF;
    n_cases := n_cases + 1;
    -- No claim after a terminal outcome.
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
            VALUES (n[1], gb, 2, 'i', clock_timestamp() + interval '30 seconds');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'claim after terminal: %', got; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ F. gone attribution
    INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, http_status)
        VALUES (n[2], gb, 1, 'gone', 'provider', 410);
    c := ARRAY[
        -- gone with a human actor
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason, gone_notification_id, gone_attempt_no) VALUES (%L, %L, %L, %L, 'gone', %L, 1)$s$, gb, ub, c1, ub, n[2]), '23514',
        -- gone without its source
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason) VALUES (%L, %L, %L, 'gone')$s$, gb, ub, c1), 'UZ007',
        -- the source observation is not gone
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason, gone_notification_id, gone_attempt_no) VALUES (%L, %L, %L, 'gone', %L, 1)$s$, gb, ub, c1, n[1]), 'UZ007',
        -- the gone of G cannot revoke another generation (no observation on that generation)
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason, gone_notification_id, gone_attempt_no) VALUES (%L, %L, %L, 'gone', %L, 1)$s$, glate, ub, c1, n[2]), 'UZ007',
        -- a user revocation without an actor is not silently accepted
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason) VALUES (%L, %L, %L, 'user')$s$, gb, ub, c1), '23514',
        -- a user revocation cannot borrow provider attribution
        format($s$INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, revoked_by, reason, gone_notification_id, gone_attempt_no) VALUES (%L, %L, %L, %L, 'user', %L, 1)$s$, gb, ub, c1, ub, n[2]), '23514'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'F case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason, gone_notification_id, gone_attempt_no)
        VALUES (gb, ub, c1, 'gone', n[2], 1);
    IF (SELECT r.revoked_by FROM push_subscription_revocations r WHERE r.subscription_id = gb) IS NOT NULL THEN
        RAISE EXCEPTION 'gone recorded a human actor';
    END IF;
    IF EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = glate) THEN
        RAISE EXCEPTION 'gone reached another generation of the recipient';
    END IF;
    INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no)
        VALUES (n[2], gb, 'gone', 1);
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ G. budget function
    IF usapp_push_provider_attempts(n[1], gb) <> 1 OR usapp_push_provider_attempts(n[5], gb) <> 0 THEN
        RAISE EXCEPTION 'provider budget count';
    END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ H. append-only
    EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
    FOR rec IN SELECT * FROM (VALUES
        ('UPDATE push_dispatch_control SET reason_code = reason_code'),
        ('DELETE FROM push_dispatch_control'),
        ('TRUNCATE push_dispatch_control CASCADE'),
        ('UPDATE push_deliveries SET couple_id = couple_id'),
        ('DELETE FROM push_deliveries'),
        ('TRUNCATE push_deliveries CASCADE'),
        ('UPDATE push_attempts SET instance_id = instance_id'),
        ('DELETE FROM push_attempts'),
        ('TRUNCATE push_attempts CASCADE'),
        ('UPDATE push_attempt_observations SET outcome = outcome'),
        ('DELETE FROM push_attempt_observations'),
        ('TRUNCATE push_attempt_observations CASCADE'),
        ('UPDATE push_attempt_expiries SET expired_at = expired_at'),
        ('DELETE FROM push_attempt_expiries'),
        ('TRUNCATE push_attempt_expiries CASCADE'),
        ('UPDATE push_delivery_outcomes SET final_state = final_state'),
        ('DELETE FROM push_delivery_outcomes'),
        ('TRUNCATE push_delivery_outcomes CASCADE')) v(stmt) LOOP
        got := 'none';
        BEGIN EXECUTE rec.stmt; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM app_or THEN RAISE EXCEPTION 'H %: got %, want %', rec.stmt, got, app_or; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    n_groups := n_groups + 1;

    -- ------------------------------------------------------------ I. disable and a new cutover
    IF is_app THEN EXECUTE 'RESET ROLE'; END IF;
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'test');
    IF is_app THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    got := 'none';
    BEGIN
        INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (n[3], glate, ub, c1);
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ003' THEN RAISE EXCEPTION 'delivery while disabled: %', got; END IF;
    n_cases := n_cases + 1;
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
            VALUES (n[8], gb, 1, 'i', clock_timestamp() + interval '30 seconds');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'claim while disabled: %', got; END IF;
    n_cases := n_cases + 1;
    IF is_app THEN EXECUTE 'RESET ROLE'; END IF;
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'test');
    SELECT s.cutover INTO cut2 FROM usapp_push_control_state() s;
    IF cut2 <= cut1 THEN RAISE EXCEPTION 'enable after disable must move the cutover'; END IF;
    n_cases := n_cases + 1;
    IF is_app THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    -- A notice from before the new cutover is now skippable and not claimable.
    INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (n[8], gb, 'cutover_skipped');
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
            VALUES (n[7], gb, 1, 'i', clock_timestamp() + interval '30 seconds');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'claim before the new cutover: %', got; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;
    IF is_app THEN EXECUTE 'RESET ROLE'; END IF;

    -- ------------------------------------------------------------ J. R5: source_stale belongs to reminders, by its own observation
    IF to_regproc('public.usapp_reminder_occurrence_valid') IS NULL THEN
        RAISE EXCEPTION 'missing usapp_reminder_occurrence_valid: apply 0028_reminder_push_stale first';
    END IF;
    -- Group H left constraints immediate; an occurrence and its notice are inserted in two statements.
    EXECUTE 'SET CONSTRAINTS ALL DEFERRED';
    r5_stale := date_trunc('second', clock_timestamp() + interval '10 minutes');
    INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine, create_idempotency_key, create_fingerprint,
                       deadline_local_date, deadline_local_time, deadline_at, created_at)
        VALUES (r5_task, c1, ua, ub, 'p26r5', 1, 1, 'psql-p26r5-' || tag, fp, (r5_stale AT TIME ZONE 'Asia/Shanghai')::date,
                (r5_stale AT TIME ZONE 'Asia/Shanghai')::time, r5_stale, clock_timestamp() - interval '2 days');
    INSERT INTO reminder_rules (couple_id, owner_id, seq, task_id, enabled, mode, before_minutes, idempotency_key, request_fingerprint, created_at)
        VALUES (c1, ub, 1, r5_task, true, 'before_deadline', 30, 'psql-p26r5-rule-' || tag, fp, clock_timestamp() - interval '1 day') RETURNING id INTO r5_rule;
    INSERT INTO reminder_occurrences (couple_id, owner_id, rule_id, event_kind, task_id, fire_at, stale_at)
        VALUES (c1, ub, r5_rule, 'reminder.task', r5_task, r5_stale - interval '30 minutes', r5_stale) RETURNING id INTO r5_occ;
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, reminder_occurrence_id)
        VALUES (c1, NULL, ub, 'reminder.task', 'task', r5_task, r5_occ) RETURNING id INTO r5_notice;
    m := gen_random_uuid();
    INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
        VALUES (m, c1, ua, ub, 'p26r5', 'psql-p26r5-msg-' || tag, fp);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id)
        VALUES (c1, ua, ub, 'message.created', 'message', m, m) RETURNING id INTO r5_plain;
    INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested,
                                      recipient_id, notification_id, preference_seq)
        VALUES (c1, ua, 'message_create', m, true, true, ub, r5_plain, 10);
    IF is_app THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id)
        VALUES (r5_notice, gb, ub, c1), (r5_plain, gb, ub, c1);
    INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
        VALUES (r5_notice, gb, 1, 'i', clock_timestamp() + interval '30 seconds'),
               (r5_plain, gb, 1, 'i', clock_timestamp() + interval '30 seconds');
    IF NOT usapp_reminder_occurrence_valid(r5_plain) OR NOT usapp_reminder_occurrence_valid(r5_notice) THEN
        RAISE EXCEPTION 'source validity of a live reminder or a message';
    END IF;
    n_cases := n_cases + 1;
    c := ARRAY[
        -- never a provider fact
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) VALUES (%L, %L, 1, 'source_stale_skipped', 'provider')$s$, r5_notice, gb), '23514',
        -- only a reminder has a source to be stale
        format($s$INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class) VALUES (%L, %L, 1, 'source_stale_skipped', 'internal')$s$, r5_plain, gb), 'UZ005',
        -- no attempt named (the guard fires before the CHECK)
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (%L, %L, 'source_stale')$s$, r5_notice, gb), 'UZ006',
        -- no such observation yet
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) VALUES (%L, %L, 'source_stale', 1)$s$, r5_notice, gb), 'UZ006',
        format($s$INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) VALUES (%L, %L, 'source_stale', 1)$s$, r5_plain, gb), 'UZ006'
    ];
    FOR i IN 1..array_length(c, 1) BY 2 LOOP
        got := 'none';
        BEGIN EXECUTE c[i]; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
        IF got IS DISTINCT FROM c[i + 1] THEN RAISE EXCEPTION 'J case %: got %, want %', i, got, c[i + 1]; END IF;
        n_cases := n_cases + 1;
    END LOOP;
    INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class)
        VALUES (r5_notice, gb, 1, 'source_stale_skipped', 'internal');
    IF usapp_push_provider_attempts(r5_notice, gb) <> 0 THEN RAISE EXCEPTION 'source_stale_skipped spent budget'; END IF;
    n_cases := n_cases + 1;
    INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no)
        VALUES (r5_notice, gb, 'source_stale', 1);
    -- Terminal: not rewritten, and no later claim.
    got := 'none';
    BEGIN
        INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (r5_notice, gb, 'expired');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> '23505' THEN RAISE EXCEPTION 'source_stale rewritten: %', got; END IF;
    n_cases := n_cases + 1;
    got := 'none';
    BEGIN
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, lease_until)
            VALUES (r5_notice, gb, 2, 'i', clock_timestamp() + interval '30 seconds');
    EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
    IF got <> 'UZ004' THEN RAISE EXCEPTION 'claim after source_stale: %', got; END IF;
    n_cases := n_cases + 1;
    -- Only the source_stale values changed: a claim's budget and control state are what they were.
    IF (SELECT s.state FROM usapp_push_control_state() s) <> 'enabled' THEN RAISE EXCEPTION 'a stale skip moved control'; END IF;
    n_cases := n_cases + 1;
    n_groups := n_groups + 1;
    IF is_app THEN EXECUTE 'RESET ROLE'; END IF;

    RAISE NOTICE 'ALL PUSH DISPATCH ASSERTIONS PASSED: role=% mode=% groups=% cases=%',
        CASE WHEN is_app THEN 'usapp_api' ELSE current_user END, mode, n_groups, n_cases;
END
$dispatch_test$;

ROLLBACK;
