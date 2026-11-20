-- =====================================================================
-- database/tests/notification_choice_test.sql
--
-- P2-3b: database-side tests for optional notifications and notification intents
-- (docs/phase2-notifications.md Section 12). Requires 0020_notification_choices on
-- usapp_test; refuses any other database before writing fixtures. Run twice:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UN101 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. After the valid
-- fixtures, SET CONSTRAINTS ALL IMMEDIATE runs the deferred UN105 link per statement.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $choice_test$
DECLARE
    is_app   boolean;
    run_role text := current_user;
    n_passed integer := 0;
    n_cases  integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    t1 uuid := gen_random_uuid();
    a1 uuid := gen_random_uuid();   -- ua's, v1 create + v2 edit
    a2 uuid := gen_random_uuid();   -- uc's, partnerless
    m1 uuid := gen_random_uuid();   -- ua -> ub, replied by ub
    m2 uuid := gen_random_uuid();   -- ua -> ub, no intent yet
    n_task uuid;
    n_edit uuid;
    n_reply uuid;
    tag text := substr(md5(random()::text), 1, 8);
    fp  text := md5(random()::text) || md5(random()::text);
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
    IF to_regclass('public.notification_intents') IS NULL THEN
        RAISE EXCEPTION '缺少通知意图表：请先在测试库应用 0020_notification_choices';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UN101' END;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'p23b_psql_a_' || tag || '@example.com', 'p23b_psql_a_' || tag || '@example.com'),
               (ub, 'p23b_psql_b_' || tag || '@example.com', 'p23b_psql_b_' || tag || '@example.com'),
               (uc, 'p23b_psql_c_' || tag || '@example.com', 'p23b_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'p23b-psql', ua), (c2, 'p23b-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);

    -- A. Valid sources and intents for all five operations.
    INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                       create_idempotency_key, create_fingerprint)
        VALUES (t1, c1, ua, ub, 'p23b task', 1, 0, 'psql-p23b-task-' || tag, fp);
    INSERT INTO anniversaries (id, couple_id, creator_id) VALUES (a1, c1, ua), (a2, c2, uc);
    INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind,
                                       note, idempotency_key, request_fingerprint)
        VALUES (a1, c1, 1, ua, 'create', 'p23b', DATE '2020-01-01', 'anniversary', '', 'psql-p23b-a1-' || tag, fp),
               (a2, c2, 1, uc, 'create', 'p23b', DATE '2020-01-01', 'anniversary', '', 'psql-p23b-a2-' || tag, fp);
    INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind,
                                       note, idempotency_key, request_fingerprint)
        VALUES (a1, c1, 2, ua, 'edit', 'p23b v2', DATE '2020-01-02', 'anniversary', '', 'psql-p23b-a1e-' || tag, fp);
    INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint)
        VALUES (m1, c1, ua, ub, 'p23b', 'psql-p23b-m1-' || tag, fp),
               (m2, c1, ua, ub, 'p23b 2', 'psql-p23b-m2-' || tag, fp);
    INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint)
        VALUES (m1, c1, ub, 'ok', 'psql-p23b-r1-' || tag, fp);

    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, task_id)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, t1
          FROM usapp_notification_optional_source('task', t1, NULL) RETURNING id INTO n_task;
    INSERT INTO notification_intents (couple_id, actor_id, operation, task_id, choice_explicit, requested,
                                      recipient_id, notification_id, preference_seq)
        VALUES (c1, ua, 'task_publish', t1, true, true, ub, n_task, 1);
    INSERT INTO notification_intents (couple_id, actor_id, operation, anniversary_id, anniversary_version,
                                      choice_explicit, requested, recipient_id)
        VALUES (c1, ua, 'anniversary_create', a1, 1, false, false, ub);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id,
                               anniversary_id, anniversary_version)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, a1, 2
          FROM usapp_notification_optional_source('anniversary_revision', a1, 2) RETURNING id INTO n_edit;
    INSERT INTO notification_intents (couple_id, actor_id, operation, anniversary_id, anniversary_version,
                                      choice_explicit, requested, recipient_id, notification_id, preference_seq)
        VALUES (c1, ua, 'anniversary_edit', a1, 2, true, true, ub, n_edit, 1);
    -- Partnerless: requested, no recipient, no preference and no notice.
    INSERT INTO notification_intents (couple_id, actor_id, operation, anniversary_id, anniversary_version,
                                      choice_explicit, requested)
        VALUES (c2, uc, 'anniversary_create', a2, 1, true, true);
    INSERT INTO notification_intents (couple_id, actor_id, operation, message_id, choice_explicit, requested,
                                      recipient_id, preference_seq)
        VALUES (c1, ua, 'message_create', m1, true, false, ub, 1);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, message_reply_id)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, m1
          FROM usapp_notification_optional_source('message_reply', m1, NULL) RETURNING id INTO n_reply;
    INSERT INTO notification_intents (couple_id, actor_id, operation, message_reply_id, choice_explicit, requested,
                                      recipient_id, notification_id, preference_seq)
        VALUES (c1, ub, 'message_reply', m1, true, true, ua, n_reply, 1);
    SET CONSTRAINTS ALL IMMEDIATE;  -- the deferred UN105 links pass for the valid fixtures
    IF (SELECT recipient_id FROM notifications WHERE id = n_reply) IS DISTINCT FROM ua
       OR (SELECT recipient_id FROM notifications WHERE id = n_task) IS DISTINCT FROM ub THEN
        RAISE EXCEPTION 'A 失败：接收者推导不符';
    END IF;
    n_passed := n_passed + 1;

    FOR rec IN
        SELECT * FROM (VALUES
            -- B. Optional notices: link, recipient, family and revision kind.
            ('B1 notice without intent', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, message_id) VALUES (%L, %L, %L, 'message.created', 'message',
                %L, %L)$s$, c1, ua, ub, m2, m2), 'UN105'),
            ('B2 wrong recipient', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type,
                target_kind, target_id, task_id) VALUES (%L, %L, %L, 'task.published', 'task', %L, %L)$s$,
                c1, ua, uc, t1, t1), 'UN102'),
            ('B3 wrong family', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type,
                target_kind, target_id, message_reply_id) VALUES (%L, %L, %L, 'message.created', 'message', %L,
                %L)$s$, c1, ua, ub, m1, m1), 'UN102'),
            ('B4 edit revision as created', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, anniversary_id, anniversary_version) VALUES (%L, %L, %L,
                'anniversary.created', 'anniversary', %L, %L, 2)$s$, c1, ua, ub, a1, a1), 'UN102'),
            -- C. Intents: one per source, actor, recipient, sequences, links and exact families.
            ('C1 second intent', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id, preference_seq) VALUES (%L, %L,
                'message_create', %L, true, false, %L, 2)$s$, c1, ua, m1, ub), '23505'),
            ('C2 not the author', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'message_create', %L, false,
                false, %L)$s$, c1, ub, m2, ua), 'UN104'),
            ('C3 recipient hidden', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested) VALUES (%L, %L, 'message_create', %L, false, false)$s$,
                c1, ua, m2), 'UN104'),
            ('C4 requested without notice', format($s$INSERT INTO notification_intents (couple_id, actor_id,
                operation, message_id, choice_explicit, requested, recipient_id, preference_seq) VALUES (%L, %L,
                'message_create', %L, true, true, %L, 2)$s$, c1, ua, m2, ub), '23514'),
            ('C5 preference gap', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id, preference_seq) VALUES (%L, %L,
                'message_create', %L, true, false, %L, 3)$s$, c1, ua, m2, ub), 'UN104'),
            ('C6 omitted remembered', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id, preference_seq) VALUES (%L, %L,
                'message_create', %L, false, false, %L, 2)$s$, c1, ua, m2, ub), '23514'),
            ('C7 forged link', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id, notification_id, preference_seq) VALUES (%L,
                %L, 'message_create', %L, true, true, %L, %L, 2)$s$, c1, ua, m2, ub, n_task), 'UN104'),
            ('C8 edit revision as create', format($s$INSERT INTO notification_intents (couple_id, actor_id,
                operation, anniversary_id, anniversary_version, choice_explicit, requested, recipient_id) VALUES
                (%L, %L, 'anniversary_create', %L, 2, false, false, %L)$s$, c1, ua, a1, ub), 'UN104'),
            ('C9 family mismatch', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'task_publish', %L, false,
                false, %L)$s$, c1, ua, m2, ub), 'UN104'),
            ('C10 requested implicitly', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                message_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'message_create', %L, false,
                true, %L)$s$, c1, ua, m2, ub), '23514'),
            -- D. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
            ('D1 update intent', format($s$UPDATE notification_intents SET requested = true WHERE message_id = %L$s$,
                m1), app_or),
            ('D2 delete intent', format('DELETE FROM notification_intents WHERE task_id = %L', t1), app_or),
            ('D3 truncate', 'TRUNCATE notification_intents', app_or)
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

    -- E. Remembered defaults: only explicit choices with a recipient carry a sequence.
    IF (SELECT requested FROM notification_intents WHERE actor_id = ua AND operation = 'message_create'
         AND preference_seq IS NOT NULL ORDER BY preference_seq DESC LIMIT 1) IS DISTINCT FROM false
       OR EXISTS (SELECT 1 FROM notification_intents WHERE actor_id = uc AND preference_seq IS NOT NULL)
       OR (SELECT count(*) FROM notification_intents WHERE couple_id IN (c1, c2)) <> 6
       OR (SELECT count(*) FROM notifications WHERE couple_id IN (c1, c2)) <> 3
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id IN (c1, c2)) THEN
        RAISE EXCEPTION 'E1 失败：通知意图夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL NOTIFICATION CHOICE ASSERTIONS PASSED: role=% mode=% groups=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed, n_cases;
END
$choice_test$;

ROLLBACK;
