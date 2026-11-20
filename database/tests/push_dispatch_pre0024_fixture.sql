-- =====================================================================
-- database/tests/push_dispatch_pre0024_fixture.sql
--
-- P2-5b repair verification: COMMITTED fixture for a disposable instance at exactly 0000-0023, run by
-- the maintenance role BEFORE 0024. It leaves real attempts claimed under 0023, so that after 0024
-- their control_revision is NULL (unknown provenance) and the upgrade meets existing rows.
-- Refuses to run unless 0024 is absent and the database is usapp_test. Ends with control disabled.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $pre0024$
DECLARE
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    sb uuid := gen_random_uuid();
    gb uuid := gen_random_uuid();
    m uuid;
    n uuid;
    e text := 'https://fcm.googleapis.com/fcm/send/p25b-pre0024-' || substr(md5(random()::text), 1, 12);
    tag text := substr(md5(random()::text), 1, 8);
    fp text := md5(random()::text) || md5(random()::text);
    ts timestamptz;
    i integer;
BEGIN
    IF current_database() IS DISTINCT FROM 'usapp_test' THEN
        RAISE EXCEPTION 'refusing: database is %', current_database();
    END IF;
    IF EXISTS (SELECT 1 FROM schema_migrations WHERE version = '0024_push_dispatch_repairs') THEN
        RAISE EXCEPTION 'refusing: this fixture must run before 0024';
    END IF;
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'p25b_pre0024_a_' || tag || '@m3-fixture.test', 'p25b_pre0024_a_' || tag || '@m3-fixture.test'),
               (ub, 'p25b_pre0024_b_' || tag || '@m3-fixture.test', 'p25b_pre0024_b_' || tag || '@m3-fixture.test');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'p25b-pre0024', ua);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2);
    INSERT INTO sessions (id, token_hash, csrf_token_hash, user_id, is_authenticated, expires_at)
        VALUES (sb, md5(random()::text) || tag, md5(random()::text), ub, true, now() + interval '1 day');
    INSERT INTO push_subscriptions (id, user_id, couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id)
        VALUES (gb, ub, c1, sb, e, encode(sha256(convert_to(e, 'UTF8')), 'hex'), 'B' || repeat('A', 86), repeat('A', 22), repeat('a', 64));
    INSERT INTO push_registration_commands (user_id, idempotency_key, couple_id, session_id, request_fingerprint, result, generation_id)
        VALUES (ub, 'psql-p25b-pre0024-' || tag, c1, sb, fp, 'created', gb);
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable', 'owner', 'pre0024_fixture');
    FOR i IN 1..3 LOOP
        m := gen_random_uuid();
        INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
            VALUES (m, c1, ua, ub, 'p25b pre0024', 'psql-p25b-pre0024-' || i || '-' || tag, fp);
        INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_id)
            VALUES (c1, ua, ub, 'message.created', 'message', m, m) RETURNING id INTO n;
        INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested,
                                          recipient_id, notification_id, preference_seq)
            VALUES (c1, ua, 'message_create', m, true, true, ub, n, i);
        INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) VALUES (n, gb, ub, c1);
        -- The lease has already ended by the time anything runs next, so it holds no capacity.
        ts := clock_timestamp();
        INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, claimed_at, lease_until)
            VALUES (n, gb, 1, 'p25b-pre-0024', ts, ts + interval '1 millisecond');
        -- One attempt already has a result, so the replaced class rule validates an existing row.
        IF i = 3 THEN
            INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, http_status)
                VALUES (n, gb, 1, 'busy', 'internal', 503);
        END IF;
    END LOOP;
    INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'pre0024_fixture');
    RAISE NOTICE 'PRE-0024 FIXTURE COMMITTED: attempts=3';
END
$pre0024$;

COMMIT;
