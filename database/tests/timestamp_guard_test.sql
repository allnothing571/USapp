-- =====================================================================
-- database/tests/timestamp_guard_test.sql
--
-- 0041 (docs/phase3-timestamp-guards.md): the eight guards, both shapes of a backward clock step, repeated steps, supplied values, the three NULL forms and
-- the complete source-to-consumer chains. Deterministic: no real clock step is needed. The guards call clock_timestamp() with no fixed search_path, so a
-- fake one ahead of pg_catalog on this session's search_path moves every guard's clock, by the offset in usapp.fake_offset.
-- Requires 0041's effect on usapp_test and refuses any other database. Connect as the maintenance superuser, which alone can build the control fixtures, and pass the mode:
--   psql -U postgres -d usapp_test -v mode=application -f timestamp_guard_test.sql
--   psql -U postgres -d usapp_test -v mode=maintenance -f timestamp_guard_test.sql
-- application: assertions run under SET LOCAL ROLE usapp_api. maintenance: as the superuser, plus the triggers-off cases.
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

SELECT set_config('usapp.ts_mode', :'mode', true);
SELECT set_config('usapp.fake_offset', '0', true);

DO $refuse$
BEGIN
    IF current_database() IS DISTINCT FROM 'usapp_test' THEN RAISE EXCEPTION 'usapp_test only: %', current_database(); END IF;
    IF NOT (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN RAISE EXCEPTION 'connect as the maintenance superuser'; END IF;
    IF current_setting('usapp.ts_mode') NOT IN ('application', 'maintenance') THEN RAISE EXCEPTION 'mode must be application or maintenance'; END IF;
    -- From the catalog, not the registry, so that this also runs inside the rollback-only prevalidation of the migration.
    IF (SELECT a.atthasdef FROM pg_attribute a WHERE a.attrelid = 'public.messages'::regclass AND a.attname = 'created_at') THEN RAISE EXCEPTION 'apply 0041_timestamp_guards first'; END IF;
END
$refuse$;

CREATE SCHEMA usapp_test_clock;
GRANT USAGE ON SCHEMA usapp_test_clock TO usapp_api;

CREATE FUNCTION usapp_test_clock.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE AS
$$ SELECT pg_catalog.clock_timestamp() + current_setting('usapp.fake_offset')::interval $$;

CREATE FUNCTION usapp_test_clock.set_clock(p_offset text) RETURNS void LANGUAGE plpgsql VOLATILE AS
$$ BEGIN PERFORM set_config('usapp.fake_offset', p_offset, true); END $$;

-- The statement must fail with exactly this SQLSTATE and message. Its subtransaction is gone either way.
CREATE FUNCTION usapp_test_clock.refused(p_label text, p_sql text, p_state text, p_message text) RETURNS void LANGUAGE plpgsql VOLATILE AS $refused$
DECLARE
    got_state text;
    got_message text;
BEGIN
    BEGIN
        EXECUTE p_sql;
        RAISE EXCEPTION USING ERRCODE = 'UTS01', MESSAGE = 'accepted';
    EXCEPTION WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS got_state = RETURNED_SQLSTATE, got_message = MESSAGE_TEXT;
    END;
    IF got_state IS DISTINCT FROM p_state OR got_message IS DISTINCT FROM p_message THEN
        RAISE EXCEPTION 'case %: expected % %, got % %', p_label, p_state, p_message, got_state, got_message;
    END IF;
END
$refused$;

CREATE TABLE usapp_test_clock.fx (couple uuid, a uuid, b uuid, other_couple uuid, session_b uuid, g uuid, p_request uuid, p_direct uuid);
GRANT SELECT ON usapp_test_clock.fx TO usapp_api;

CREATE FUNCTION usapp_test_clock.key() RETURNS text LANGUAGE sql VOLATILE AS $$ SELECT 'ts-' || replace(gen_random_uuid()::text, '-', '') $$;

-- Sources: each takes its time from its guard, under whatever the clock says now.
CREATE FUNCTION usapp_test_clock.src_message() RETURNS uuid LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE; m uuid := gen_random_uuid();
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
        VALUES (m, f.couple, f.a, f.b, 'ts', usapp_test_clock.key(), repeat('a', 64));
    RETURN m;
END $f$;

CREATE FUNCTION usapp_test_clock.src_redemption(p_mode text) RETURNS uuid LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE; r uuid := gen_random_uuid(); op uuid; pid uuid; x reward_product_revisions%ROWTYPE;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    pid := CASE p_mode WHEN 'direct' THEN f.p_direct ELSE f.p_request END;
    SELECT * INTO x FROM reward_product_revisions WHERE product_id = pid AND version = 1;
    IF p_mode = 'direct' THEN
        op := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op, f.couple, f.b, 'REWARD_REDEMPTION', 'reward_redemption', r::text);
    END IF;
    INSERT INTO reward_redemptions (id, couple_id, product_id, product_version, creator_id, redeemer_id, title, description, price_flowers, redemption_mode, operation_id, idempotency_key, request_fingerprint)
        VALUES (r, f.couple, pid, 1, f.a, f.b, x.title, x.description, x.price_flowers, x.redemption_mode, op, usapp_test_clock.key(), repeat('a', 64));
    RETURN r;
END $f$;

-- A notice through the production statement's facts. p_at is omitted (OMIT), DEFAULT, NULL or a SQL expression.
CREATE FUNCTION usapp_test_clock.notice(p_family text, p_source uuid, p_at text DEFAULT 'OMIT', OUT id uuid, OUT t timestamptz) LANGUAGE plpgsql VOLATILE AS $f$
DECLARE
    x record;
    col text := CASE p_family WHEN 'message' THEN 'message_id' WHEN 'message_reply' THEN 'message_reply_id' WHEN 'redemption_event' THEN 'redemption_event_id' ELSE 'redemption_id' END;
BEGIN
    IF p_family IN ('message', 'message_reply') THEN
        SELECT s.event_type, s.couple_id, s.actor_id, s.recipient_id, s.target_kind, s.target_id INTO STRICT x FROM usapp_notification_optional_source(p_family, p_source, NULL) s;
    ELSE
        SELECT s.event_type, s.couple_id, s.actor_id, s.recipient_id, s.target_kind, s.target_id INTO STRICT x FROM usapp_notification_source(p_family, p_source) s;
    END IF;
    EXECUTE format('INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, %I%s) VALUES ($1, $2, $3, $4, $5, $6, $7%s) RETURNING id, created_at',
                   col, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', created_at' END, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END)
        INTO id, t USING x.couple_id, x.actor_id, x.recipient_id, x.event_type, x.target_kind, x.target_id, p_source;
END $f$;

CREATE FUNCTION usapp_test_clock.reply(p_message uuid, p_at text DEFAULT 'OMIT') RETURNS timestamptz LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE; t timestamptz;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    EXECUTE format('INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint%s) VALUES ($1, $2, $3, ''ts'', $4, $5%s) RETURNING created_at',
                   CASE WHEN p_at = 'OMIT' THEN '' ELSE ', created_at' END, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END)
        INTO t USING p_message, f.couple, f.b, usapp_test_clock.key(), repeat('a', 64);
    RETURN t;
END $f$;

CREATE FUNCTION usapp_test_clock.event(p_redemption uuid, p_actor uuid, p_action text, p_proposal uuid DEFAULT NULL, p_at text DEFAULT 'OMIT', OUT id uuid, OUT t timestamptz) LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    EXECUTE format('INSERT INTO reward_redemption_events (redemption_id, couple_id, actor_id, action, proposal_id, idempotency_key, request_fingerprint%s) VALUES ($1, $2, $3, $4, $5, $6, $7%s) RETURNING id, created_at',
                   CASE WHEN p_at = 'OMIT' THEN '' ELSE ', created_at' END, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END)
        INTO id, t USING p_redemption, f.couple, p_actor, p_action, p_proposal, usapp_test_clock.key(), repeat('a', 64);
END $f$;

CREATE FUNCTION usapp_test_clock.fulfill(p_redemption uuid, p_by uuid, p_at text DEFAULT 'OMIT') RETURNS timestamptz LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE; t timestamptz;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    EXECUTE format('INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key, request_fingerprint%s) VALUES ($1, $2, $3, $4, $5%s) RETURNING fulfilled_at',
                   CASE WHEN p_at = 'OMIT' THEN '' ELSE ', fulfilled_at' END, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END)
        INTO t USING p_redemption, f.couple, p_by, usapp_test_clock.key(), repeat('a', 64);
    RETURN t;
END $f$;

CREATE FUNCTION usapp_test_clock.delivery(p_notice uuid, p_at text DEFAULT 'OMIT', p_generation uuid DEFAULT NULL) RETURNS timestamptz LANGUAGE plpgsql VOLATILE AS $f$
DECLARE f usapp_test_clock.fx%ROWTYPE; t timestamptz; recipient uuid;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    SELECT recipient_id INTO recipient FROM notifications WHERE id = p_notice;
    EXECUTE format('INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id%s) VALUES ($1, $2, $3, $4%s) RETURNING created_at',
                   CASE WHEN p_at = 'OMIT' THEN '' ELSE ', created_at' END, CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END)
        INTO t USING p_notice, COALESCE(p_generation, f.g), recipient, f.couple;
    RETURN t;
END $f$;

-- One write of one kind: the source under one clock, then the row under another. Returns the floor it must follow, and the time it got.
CREATE FUNCTION usapp_test_clock.pair(p_kind text, p_at text, p_source_offset text, p_row_offset text, OUT floor_at timestamptz, OUT t timestamptz) LANGUAGE plpgsql VOLATILE AS $f$
DECLARE
    f usapp_test_clock.fx%ROWTYPE;
    m uuid; r uuid; n uuid; pid uuid; x reward_product_revisions%ROWTYPE;
    tcol text := CASE WHEN p_at = 'OMIT' THEN '' ELSE ', created_at' END;
    tval text := CASE WHEN p_at = 'OMIT' THEN '' ELSE ', ' || p_at END;
BEGIN
    SELECT * INTO f FROM usapp_test_clock.fx;
    PERFORM usapp_test_clock.set_clock(p_source_offset);
    CASE p_kind
    WHEN 'message' THEN
        floor_at := NULL;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        EXECUTE format('INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint%s) VALUES ($1, $2, $3, ''ts'', $4, $5%s) RETURNING created_at', tcol, tval)
            INTO t USING f.couple, f.a, f.b, usapp_test_clock.key(), repeat('a', 64);
    WHEN 'reply' THEN
        m := usapp_test_clock.src_message();
        SELECT created_at INTO floor_at FROM messages WHERE id = m;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        t := usapp_test_clock.reply(m, p_at);
    WHEN 'redemption' THEN
        -- A revision of its own, timed by the source clock: that is what this guard follows.
        pid := gen_random_uuid();
        INSERT INTO reward_products (id, couple_id, creator_id, owner_id) VALUES (pid, f.couple, f.a, f.b);
        INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers, redemption_mode, status, idempotency_key, request_fingerprint, created_at)
            VALUES (pid, f.couple, 1, f.a, 'create', 'ts', 3, 'request', 'active', usapp_test_clock.key(), repeat('a', 64), clock_timestamp())
            RETURNING * INTO x;
        floor_at := x.created_at;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        EXECUTE format('INSERT INTO reward_redemptions (couple_id, product_id, product_version, creator_id, redeemer_id, title, description, price_flowers, redemption_mode, idempotency_key, request_fingerprint%s) VALUES ($1, $2, 1, $3, $4, $5, $6, $7, $8, $9, $10%s) RETURNING created_at', tcol, tval)
            INTO t USING f.couple, pid, f.a, f.b, x.title, x.description, x.price_flowers, x.redemption_mode, usapp_test_clock.key(), repeat('a', 64);
    WHEN 'event' THEN
        r := usapp_test_clock.src_redemption('request');
        SELECT created_at INTO floor_at FROM reward_redemptions WHERE id = r;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        SELECT e.t INTO t FROM usapp_test_clock.event(r, f.a, 'reject', NULL, p_at) e;
    WHEN 'fulfillment' THEN
        r := usapp_test_clock.src_redemption('direct');
        SELECT created_at INTO floor_at FROM reward_redemptions WHERE id = r;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        t := usapp_test_clock.fulfill(r, f.a, p_at);
    WHEN 'notification' THEN
        m := usapp_test_clock.src_message();
        SELECT created_at INTO floor_at FROM messages WHERE id = m;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        SELECT e.t INTO t FROM usapp_test_clock.notice('message', m, p_at) e;
    WHEN 'intent' THEN
        m := usapp_test_clock.src_message();
        SELECT created_at INTO floor_at FROM messages WHERE id = m;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        EXECUTE format('INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested, recipient_id%s) VALUES ($1, $2, ''message_create'', $3, false, false, $4%s) RETURNING created_at', tcol, tval)
            INTO t USING f.couple, f.a, m, f.b;
    WHEN 'delivery' THEN
        m := usapp_test_clock.src_message();
        SELECT e.id, e.t INTO n, floor_at FROM usapp_test_clock.notice('message', m) e;
        PERFORM usapp_test_clock.set_clock(p_row_offset);
        t := usapp_test_clock.delivery(n, p_at);
    END CASE;
END $f$;

SET LOCAL search_path = usapp_test_clock, pg_catalog, public;

DO $timestamp_test$
DECLARE
    mode text := current_setting('usapp.ts_mode');
    n_cases integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    sa uuid := gen_random_uuid();
    sb uuid := gen_random_uuid();
    ga uuid := gen_random_uuid();
    gb uuid := gen_random_uuid();
    glate uuid := gen_random_uuid();
    p_req uuid := gen_random_uuid();
    p_dir uuid := gen_random_uuid();
    tag text := replace(gen_random_uuid()::text, '-', '');
    pk text := 'B' || repeat('A', 86);
    au text := repeat('A', 22);
    vk text := repeat('a', 64);
    kinds text[] := ARRAY['message', 'reply', 'redemption', 'event', 'fulfillment', 'notification', 'intent', 'delivery'];
    kind text;
    form text;
    bad record;
    res record;
    m uuid; r uuid; n uuid; p uuid; d uuid;
    t0 timestamptz; t1 timestamptz; t2 timestamptz; t3 timestamptz;
    lead_at interval;
    errors jsonb := jsonb_build_object(
        'message', ARRAY['UB002', 'USAPP_MESSAGE_TIME'],
        'reply', ARRAY['UB002', 'USAPP_MESSAGE_REPLY_TIME'],
        'redemption', ARRAY['UM002', 'USAPP_REDEMPTION_TIME'],
        'event', ARRAY['UN001', 'USAPP_REDEMPTION_EVENT_TIME'],
        'fulfillment', ARRAY['UM004', 'USAPP_FULFILLMENT_STATE'],
        'notification', ARRAY['UN102', 'USAPP_NOTIFICATION_SOURCE'],
        'intent', ARRAY['UN104', 'USAPP_NOTIFICATION_INTENT_SOURCE'],
        'delivery', ARRAY['UZ003', 'USAPP_PUSH_DELIVERY_ELIGIBILITY']);
BEGIN
    -- ------------------------------------------------------------ fixtures (maintenance, clock real)
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'ts_a_' || tag || '@example.com', 'ts_a_' || tag || '@example.com'),
               (ub, 'ts_b_' || tag || '@example.com', 'ts_b_' || tag || '@example.com'),
               (uc, 'ts_c_' || tag || '@example.com', 'ts_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'ts', ua), (c2, 'ts-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO sessions (id, token_hash, csrf_token_hash, user_id, is_authenticated, expires_at)
        VALUES (sa, md5(random()::text) || tag, md5(random()::text), ua, true, now() + interval '1 day'),
               (sb, md5(random()::text) || tag, md5(random()::text), ub, true, now() + interval '1 day');
    INSERT INTO reward_products (id, couple_id, creator_id, owner_id) VALUES (p_req, c1, ua, ub), (p_dir, c1, ua, ub);
    INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, price_flowers, redemption_mode, status, idempotency_key, request_fingerprint)
        VALUES (p_req, c1, 1, ua, 'create', 'ts', 3, 'request', 'active', 'ts-req-' || tag, repeat('a', 64)),
               (p_dir, c1, 1, ua, 'create', 'ts', 3, 'direct', 'active', 'ts-dir-' || tag, repeat('a', 64));
    IF (SELECT s.state FROM usapp_push_control_state() s) <> 'disabled' THEN
        INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'test_fixture');
    END IF;
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (gb, ub, c1, sb, 'https://fcm.googleapis.com/fcm/send/ts-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/ts-' || tag, 'UTF8')), 'hex'), pk, au, vk),
               (ga, ua, c1, sa, 'https://fcm.googleapis.com/fcm/send/ts-a-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/ts-a-' || tag, 'UTF8')), 'hex'), pk, au, vk);
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'test_fixture');
    INSERT INTO usapp_test_clock.fx VALUES (c1, ua, ub, c2, sb, gb, p_req, p_dir);
    IF mode = 'application' THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;

    -- ------------------------------------------------------------ A. the eight guards
    FOREACH kind IN ARRAY kinds LOOP
        -- The three forms are one request: the database assigns the time.
        FOREACH form IN ARRAY ARRAY['OMIT', 'DEFAULT', 'NULL'] LOOP
            SELECT * INTO res FROM pair(kind, form, '0', '0');
            IF res.t IS NULL OR (res.floor_at IS NOT NULL AND res.t < res.floor_at) THEN
                RAISE EXCEPTION 'A1 % %: t=% floor=%', kind, form, res.t, res.floor_at;
            END IF;
            n_cases := n_cases + 1;
        END LOOP;
        -- Shape 1: the clock moved back after the source. The row takes exactly the floor, and is accepted.
        SELECT * INTO res FROM pair(kind, 'OMIT', '0', '-2 seconds');
        IF kind = 'message' THEN
            IF res.t > pg_catalog.clock_timestamp() - interval '1 second' THEN RAISE EXCEPTION 'A2 message: t is not the clock reading'; END IF;
        ELSIF res.t IS DISTINCT FROM res.floor_at THEN
            RAISE EXCEPTION 'A2 %: t=% floor=%', kind, res.t, res.floor_at;
        END IF;
        -- Shape 2: the source was written while the clock read ahead. The same rule answers.
        SELECT * INTO res FROM pair(kind, 'NULL', '2 seconds', '0');
        IF kind <> 'message' AND res.t IS DISTINCT FROM res.floor_at THEN
            RAISE EXCEPTION 'A3 %: t=% floor=%', kind, res.t, res.floor_at;
        END IF;
        -- The clock reading itself, when it is the later one.
        SELECT * INTO res FROM pair(kind, 'OMIT', '0', '5 seconds');
        IF res.t < pg_catalog.clock_timestamp() + interval '4 seconds' THEN RAISE EXCEPTION 'A4 %: t=% is not the later clock reading', kind, res.t; END IF;
        n_cases := n_cases + 3;
        -- A supplied value meets the original predicate and error, however close.
        PERFORM set_clock('0');
        SELECT * INTO res FROM pair(kind, 'clock_timestamp()', '0', '0');
        IF res.t IS NULL THEN RAISE EXCEPTION 'A5 %: a valid supplied time was not stored', kind; END IF;
        PERFORM refused('future ' || kind, format('SELECT * FROM usapp_test_clock.pair(%L, %L, ''0'', ''0'')', kind, 'clock_timestamp() + interval ''1 hour'''), errors->kind->>0, errors->kind->>1);
        PERFORM refused('near future ' || kind, format('SELECT * FROM usapp_test_clock.pair(%L, %L, ''0'', ''0'')', kind, 'clock_timestamp() + interval ''1 second'''), errors->kind->>0, errors->kind->>1);
        IF kind <> 'message' THEN
            PERFORM refused('past ' || kind, format('SELECT * FROM usapp_test_clock.pair(%L, %L, ''0'', ''0'')', kind, 'clock_timestamp() - interval ''1 hour'''), errors->kind->>0, errors->kind->>1);
            -- Half a second before its source. The new rule never helps a supplied value.
            PERFORM refused('just before ' || kind, format('SELECT * FROM usapp_test_clock.pair(%L, %L, ''0'', ''0'')', kind, '(clock_timestamp() - interval ''500 milliseconds'')'), errors->kind->>0, errors->kind->>1);
            n_cases := n_cases + 2;
        END IF;
        n_cases := n_cases + 3;
    END LOOP;

    -- ------------------------------------------------------------ B. identity, authorization and state: refused with their errors when the time is NULL
    PERFORM set_clock('0');
    m := src_message();
    PERFORM refused('reply by the author', format('INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, %L, ''ts'', ''%s'', repeat(''a'', 64), NULL)', m, c1, ua, key()), 'UB002', 'USAPP_MESSAGE_REPLY_ACTOR');
    PERFORM refused('redemption by the creator', format('INSERT INTO reward_redemptions (couple_id, product_id, product_version, creator_id, redeemer_id, title, price_flowers, redemption_mode, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, 1, %L, %L, ''ts'', 3, ''request'', ''%s'', repeat(''a'', 64), NULL)', c1, p_req, ub, ua, key()), 'UM002', 'USAPP_REDEMPTION_ACTOR');
    r := src_redemption('request');
    PERFORM refused('reject by the redeemer', format('INSERT INTO reward_redemption_events (redemption_id, couple_id, actor_id, action, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, %L, ''reject'', ''%s'', repeat(''a'', 64), NULL)', r, c1, ub, key()), 'UN001', 'USAPP_REDEMPTION_EVENT_STATE');
    PERFORM refused('event by a stranger', format('INSERT INTO reward_redemption_events (redemption_id, couple_id, actor_id, action, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, %L, ''reject'', ''%s'', repeat(''a'', 64), NULL)', r, c1, uc, key()), 'UN001', 'USAPP_REDEMPTION_EVENT_ACTOR');
    PERFORM refused('fulfil by the redeemer', format('INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key, request_fingerprint, fulfilled_at) VALUES (%L, %L, %L, ''%s'', repeat(''a'', 64), NULL)', r, c1, ub, key()), 'UM004', 'USAPP_FULFILLMENT_ACTOR');
    PERFORM refused('fulfil before it is due', format('INSERT INTO reward_redemption_fulfillments (redemption_id, couple_id, fulfilled_by, idempotency_key, request_fingerprint, fulfilled_at) VALUES (%L, %L, %L, ''%s'', repeat(''a'', 64), NULL)', r, c1, ua, key()), 'UM004', 'USAPP_FULFILLMENT_STATE');
    PERFORM refused('notice with the wrong recipient', format('INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id, created_at) VALUES (%L, %L, %L, ''message.created'', ''message'', %L, %L, NULL)', c1, ub, ua, m, m), 'UN102', 'USAPP_NOTIFICATION_SOURCE');
    PERFORM refused('notice with no source', format('INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id, created_at) VALUES (%L, %L, %L, ''message.created'', ''message'', %L, %L, NULL)', c1, ua, ub, gen_random_uuid(), gen_random_uuid()), 'UN102', 'USAPP_NOTIFICATION_SOURCE');
    PERFORM refused('intent by the recipient', format('INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested, recipient_id, created_at) VALUES (%L, %L, ''message_create'', %L, false, false, %L, NULL)', c1, ub, m, ua), 'UN104', 'USAPP_NOTIFICATION_INTENT_SOURCE');
    SELECT e.id INTO n FROM notice('message', m) e;
    PERFORM refused('delivery in another space', format('INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id, created_at) VALUES (%L, %L, %L, %L, NULL)', n, gb, ub, c2), 'UZ003', 'USAPP_PUSH_DELIVERY_SOURCE');
    n_cases := n_cases + 10;

    -- ------------------------------------------------------------ C. whole chains under repeated backward steps
    -- Message, reply, notice, intent, delivery. Every step goes further back. Time never decreases along the chain, and the lead over the clock GROWS past any
    -- one step: there is no fixed bound, and this pins that down.
    PERFORM set_clock('0');
    m := src_message();
    SELECT created_at INTO t0 FROM messages WHERE id = m;
    PERFORM set_clock('-1 second');
    t1 := reply(m);
    PERFORM set_clock('-2 seconds');
    SELECT e.id, e.t INTO n, t2 FROM notice('message_reply', m) e;
    PERFORM set_clock('-3 seconds');
    INSERT INTO notification_intents (couple_id, actor_id, operation, message_reply_id, choice_explicit, requested, recipient_id)
        VALUES (c1, ub, 'message_reply', m, false, false, ua) RETURNING created_at INTO t3;
    IF t1 IS DISTINCT FROM t0 OR t2 IS DISTINCT FROM t0 OR t3 IS DISTINCT FROM t0 THEN RAISE EXCEPTION 'C1: % % % %', t0, t1, t2, t3; END IF;
    PERFORM set_clock('-4 seconds');
    t1 := delivery(n, 'OMIT', ga);
    IF t1 IS DISTINCT FROM t0 THEN RAISE EXCEPTION 'C1 delivery: % %', t0, t1; END IF;
    lead_at := t1 - clock_timestamp();
    IF lead_at < interval '3 seconds' THEN RAISE EXCEPTION 'C1: the lead did not accumulate: %', lead_at; END IF;
    n_cases := n_cases + 1;

    -- Direct redemption, proposal, decision, notice, delivery. A decision follows its proposal, not only the redemption.
    PERFORM set_clock('0');
    r := src_redemption('direct');
    SELECT created_at INTO t0 FROM reward_redemptions WHERE id = r;
    PERFORM set_clock('3 seconds');
    SELECT e.id, e.t INTO p, t1 FROM event(r, ub, 'cancel_propose') e;
    IF t1 <= t0 THEN RAISE EXCEPTION 'C2 proposal: % %', t0, t1; END IF;
    PERFORM set_clock('-1 second');
    SELECT e.id, e.t INTO d, t2 FROM event(r, ua, 'cancel_reject', p) e;
    IF t2 IS DISTINCT FROM t1 THEN RAISE EXCEPTION 'C2 decision does not follow its proposal: % %', t1, t2; END IF;
    PERFORM set_clock('-2 seconds');
    SELECT e.id, e.t INTO n, t3 FROM notice('redemption_event', d) e;
    IF t3 IS DISTINCT FROM t1 THEN RAISE EXCEPTION 'C2 notice: % %', t1, t3; END IF;
    PERFORM set_clock('-3 seconds');
    t3 := delivery(n);
    IF t3 IS DISTINCT FROM t1 THEN RAISE EXCEPTION 'C2 delivery: % %', t1, t3; END IF;
    n_cases := n_cases + 1;

    -- Direct redemption, fulfillment, notice, delivery.
    PERFORM set_clock('2 seconds');
    r := src_redemption('direct');
    SELECT created_at INTO t0 FROM reward_redemptions WHERE id = r;
    PERFORM set_clock('-1 second');
    t1 := fulfill(r, ua);
    PERFORM set_clock('-2 seconds');
    SELECT e.id, e.t INTO n, t2 FROM notice('fulfillment', r) e;
    PERFORM set_clock('-3 seconds');
    t3 := delivery(n);
    IF t1 IS DISTINCT FROM t0 OR t2 IS DISTINCT FROM t0 OR t3 IS DISTINCT FROM t0 THEN RAISE EXCEPTION 'C3: % % % %', t0, t1, t2, t3; END IF;
    n_cases := n_cases + 1;

    -- Request redemption, then its notice, on a clock that moved back.
    PERFORM set_clock('1 second');
    r := src_redemption('request');
    SELECT created_at INTO t0 FROM reward_redemptions WHERE id = r;
    PERFORM set_clock('-1 second');
    SELECT e.t INTO t1 FROM notice('redemption', r) e;
    IF t1 IS DISTINCT FROM t0 THEN RAISE EXCEPTION 'C4: % %', t0, t1; END IF;
    n_cases := n_cases + 1;

    -- ------------------------------------------------------------ D. eligibility and TTL do not move: a NULL time gets no way round them
    -- TTL: the same 24 h boundary, on either side of it.
    PERFORM set_clock('0');
    m := src_message();
    SELECT e.id INTO n FROM notice('message', m) e;
    PERFORM set_clock('25 hours');
    PERFORM refused('past the TTL', format('SELECT usapp_test_clock.delivery(%L, ''NULL'')', n), 'UZ003', 'USAPP_PUSH_DELIVERY_ELIGIBILITY');
    PERFORM set_clock('23 hours');
    t1 := delivery(n, 'NULL');
    IF t1 < pg_catalog.clock_timestamp() + interval '22 hours' THEN RAISE EXCEPTION 'D1: %', t1; END IF;
    -- Before the device existed.
    PERFORM set_clock('0');
    m := src_message();
    SELECT e.id INTO n FROM notice('message', m) e;
    EXECUTE 'RESET ROLE';
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (glate, ub, c1, sb, 'https://fcm.googleapis.com/fcm/send/ts-l-' || tag,
                encode(sha256(convert_to('https://fcm.googleapis.com/fcm/send/ts-l-' || tag, 'UTF8')), 'hex'), pk, au, vk);
    IF mode = 'application' THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    PERFORM refused('before the device', format('SELECT usapp_test_clock.delivery(%L, ''NULL'', %L)', n, glate), 'UZ003', 'USAPP_PUSH_DELIVERY_ELIGIBILITY');
    -- Before the cutover, and while disabled.
    EXECUTE 'RESET ROLE';
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'test_fixture');
    IF mode = 'application' THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    PERFORM refused('disabled', format('SELECT usapp_test_clock.delivery(%L, ''NULL'')', n), 'UZ003', 'USAPP_PUSH_DELIVERY_DISABLED');
    EXECUTE 'RESET ROLE';
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'test_fixture');
    IF mode = 'application' THEN EXECUTE 'SET LOCAL ROLE usapp_api'; END IF;
    PERFORM refused('before the cutover', format('SELECT usapp_test_clock.delivery(%L, ''NULL'')', n), 'UZ003', 'USAPP_PUSH_DELIVERY_ELIGIBILITY');
    n_cases := n_cases + 5;

    -- ------------------------------------------------------------ E. the known exposure: read and outcome keep their predicates
    -- A notice ahead of the clock can't yet be read, and an outcome can't yet precede its delivery. Each is accepted once the clock passes.
    PERFORM set_clock('2 seconds');
    m := src_message();
    SELECT e.id, e.t INTO n, t1 FROM notice('message', m) e;
    t2 := delivery(n);
    PERFORM set_clock('0');
    PERFORM refused('read before the notice', format('INSERT INTO notification_reads (notification_id, couple_id, recipient_id) VALUES (%L, %L, %L)', n, c1, ub), 'UN103', 'USAPP_NOTIFICATION_READ');
    PERFORM refused('outcome before the delivery', format('INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) VALUES (%L, %L, ''inactive'')', n, gb), 'UZ006', 'USAPP_PUSH_OUTCOME_TIME');
    PERFORM set_clock('5 seconds');
    INSERT INTO notification_reads (notification_id, couple_id, recipient_id, read_at) VALUES (n, c1, ub, clock_timestamp());
    INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, decided_at) VALUES (n, gb, 'inactive', clock_timestamp());
    n_cases := n_cases + 4;

    -- ------------------------------------------------------------ F. triggers off: nothing assigns, so all three forms fail closed; a supplied value is stored
    IF mode = 'maintenance' THEN
        PERFORM set_clock('0');
        PERFORM set_config('session_replication_role', 'replica', true);
        PERFORM refused('replica omitted', format('INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, ''ts'', ''%s'', repeat(''a'', 64))', c1, ua, ub, key()), '23502',
                        'null value in column "created_at" of relation "messages" violates not-null constraint');
        PERFORM refused('replica NULL', format('INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, %L, ''ts'', ''%s'', repeat(''a'', 64), NULL)', c1, ua, ub, key()), '23502',
                        'null value in column "created_at" of relation "messages" violates not-null constraint');
        PERFORM refused('replica DEFAULT', format('INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at) VALUES (%L, %L, %L, ''ts'', ''%s'', repeat(''a'', 64), DEFAULT)', c1, ua, ub, key()), '23502',
                        'null value in column "created_at" of relation "messages" violates not-null constraint');
        INSERT INTO messages (couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint, created_at)
            VALUES (c1, ua, ub, 'ts', key(), repeat('a', 64), TIMESTAMPTZ '2000-01-01 00:00:00+00') RETURNING created_at INTO t1;
        IF t1 IS DISTINCT FROM TIMESTAMPTZ '2000-01-01 00:00:00+00' THEN RAISE EXCEPTION 'F: supplied value not stored: %', t1; END IF;
        PERFORM set_config('session_replication_role', 'origin', true);
        n_cases := n_cases + 4;
    END IF;

    -- ------------------------------------------------------------ G. the closed outcome value is internal only
    EXECUTE 'RESET ROLE';
    PERFORM set_clock('0');
    IF pg_get_constraintdef((SELECT c.oid FROM pg_constraint c WHERE c.conname = 'push_attempt_observations_class_rule')) !~ 'internal.*notice_ahead_skipped' THEN
        RAISE EXCEPTION 'G: notice_ahead_skipped is not a closed internal value';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_attribute a WHERE a.atthasdef AND a.attname IN ('created_at', 'fulfilled_at')
                  AND a.attrelid IN (to_regclass('public.messages'), to_regclass('public.message_replies'), to_regclass('public.reward_redemptions'), to_regclass('public.reward_redemption_events'),
                                     to_regclass('public.reward_redemption_fulfillments'), to_regclass('public.notifications'), to_regclass('public.notification_intents'), to_regclass('public.push_deliveries'))) THEN
        RAISE EXCEPTION 'G: a default remains on one of the eight columns';
    END IF;
    n_cases := n_cases + 2;

    RAISE NOTICE 'ALL TIMESTAMP GUARD ASSERTIONS PASSED: mode=% cases=%', mode, n_cases;
END
$timestamp_test$;

ROLLBACK;
