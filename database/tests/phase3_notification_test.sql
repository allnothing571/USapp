-- =====================================================================
-- database/tests/phase3_notification_test.sql
--
-- D20: database-side tests for the three opt-in Phase 3 notices (0043_phase3_notifications;
-- docs/phase3-plan.md Section 30). Requires 0043 on usapp_test; refuses any other database
-- before writing fixtures. Run twice:
--   1) as usapp_api: UPDATE / DELETE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UN101 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Fixtures are bare rows: only
-- the optional-intent link is made immediate (after the valid fixtures), so the memory and album link checks never run.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $phase3_test$
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
    mem1 uuid := gen_random_uuid();   -- ua's direct memory, notified
    mem2 uuid := gen_random_uuid();   -- ua's memory made by plan conversion
    mem3 uuid := gen_random_uuid();   -- uc's memory, no partner
    mem4 uuid := gen_random_uuid();   -- ua's direct memory, no intent yet
    alb1 uuid := gen_random_uuid();
    alb2 uuid := gen_random_uuid();   -- uc's
    b1 uuid := gen_random_uuid();     -- notified batch
    b2 uuid := gen_random_uuid();     -- quiet batch
    b3 uuid := gen_random_uuid();     -- uc's batch
    b4 uuid := gen_random_uuid();     -- no intent yet
    p_done uuid;
    p_new uuid;
    n_mem uuid;
    n_plan uuid;
    n_batch uuid;
    tag text := substr(md5(random()::text), 1, 8);
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
    IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = 'public.notifications'::regclass
                    AND attname = 'album_batch_id' AND NOT attisdropped) THEN
        RAISE EXCEPTION '缺少 D20 通知来源列：请先在测试库应用 0043_phase3_notifications';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UN101' END;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'd20_psql_a_' || tag || '@example.com', 'd20_psql_a_' || tag || '@example.com'),
               (ub, 'd20_psql_b_' || tag || '@example.com', 'd20_psql_b_' || tag || '@example.com'),
               (uc, 'd20_psql_c_' || tag || '@example.com', 'd20_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'd20-psql', ua), (c2, 'd20-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO memories (id, couple_id, creator_id) VALUES (mem1, c1, ua), (mem2, c1, ua), (mem3, c2, uc), (mem4, c1, ua);
    -- mem2 becomes a plan-created memory: a finished plan, linked in this transaction.
    p_done := (usapp_shared_plan_write(ua, NULL, 'create', 'd20-psql-p1-' || tag,
        '{"body":{"title":"D20 done","date":null,"time":null,"place":null,"note":null}}') #>> '{plan,id}')::uuid;
    PERFORM usapp_shared_plan_write(ua, p_done, 'advance', 'd20-psql-p1a-' || tag, '{"expected_status":"want"}');
    PERFORM usapp_shared_plan_write(ua, p_done, 'advance', 'd20-psql-p1b-' || tag, '{"expected_status":"plan"}');
    INSERT INTO shared_plan_memory_links (plan_id, couple_id, memory_id, actor_id, mode, request_key)
        VALUES (p_done, c1, mem2, ua, 'create', 'd20-psql-link-' || tag);
    p_new := (usapp_shared_plan_write(ua, NULL, 'create', 'd20-psql-p2-' || tag,
        '{"body":{"title":"D20 new","date":null,"time":null,"place":null,"note":null}}') #>> '{plan,id}')::uuid;
    IF p_done IS NULL OR p_new IS NULL THEN RAISE EXCEPTION '夹具：计划创建失败'; END IF;
    INSERT INTO albums (id, couple_id, creator_id) VALUES (alb1, c1, ua), (alb2, c2, uc);
    INSERT INTO album_photo_batches (id, album_id, couple_id, added_by, idempotency_key, request_fingerprint)
        VALUES (b1, alb1, c1, ua, 'd20-psql-b1-' || tag, repeat('a', 64)),
               (b2, alb1, c1, ub, 'd20-psql-b2-' || tag, repeat('a', 64)),
               (b3, alb2, c2, uc, 'd20-psql-b3-' || tag, repeat('a', 64)),
               (b4, alb1, c1, ua, 'd20-psql-b4-' || tag, repeat('a', 64));

    -- A. The resolver: the other member by slot, none for a partnerless space, none for a plan-created memory.
    IF (SELECT recipient_id FROM usapp_notification_optional_source('memory', mem1, NULL)) IS DISTINCT FROM ub
       OR (SELECT count(*) FROM usapp_notification_optional_source('memory', mem2, NULL)) <> 0
       OR (SELECT recipient_id FROM usapp_notification_optional_source('memory', mem3, NULL)) IS NOT NULL
       OR (SELECT count(*) FROM usapp_notification_optional_source('memory', mem3, NULL)) <> 1
       OR (SELECT recipient_id FROM usapp_notification_optional_source('album_batch', b2, NULL)) IS DISTINCT FROM ua
       OR (SELECT target_id FROM usapp_notification_optional_source('album_batch', b1, NULL)) IS DISTINCT FROM alb1
       OR (SELECT operation FROM usapp_notification_optional_source('shared_plan', p_new, NULL)) IS DISTINCT FROM 'shared_plan_create' THEN
        RAISE EXCEPTION 'A 失败：来源解析不符';
    END IF;
    n_passed := n_passed + 1;

    -- B. Valid notices and intents for the three operations; none remembers a choice.
    SET CONSTRAINTS notifications_optional_intent DEFERRED;  -- as the service writes: notice, then its intent
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, memory_id)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, mem1
          FROM usapp_notification_optional_source('memory', mem1, NULL) RETURNING id INTO n_mem;
    INSERT INTO notification_intents (couple_id, actor_id, operation, memory_id, choice_explicit, requested,
                                      recipient_id, notification_id)
        VALUES (c1, ua, 'memory_create', mem1, true, true, ub, n_mem);
    INSERT INTO notification_intents (couple_id, actor_id, operation, memory_id, choice_explicit, requested)
        VALUES (c2, uc, 'memory_create', mem3, true, true);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, shared_plan_id)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, p_new
          FROM usapp_notification_optional_source('shared_plan', p_new, NULL) RETURNING id INTO n_plan;
    INSERT INTO notification_intents (couple_id, actor_id, operation, shared_plan_id, choice_explicit, requested,
                                      recipient_id, notification_id)
        VALUES (c1, ua, 'shared_plan_create', p_new, true, true, ub, n_plan);
    INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, album_batch_id)
        SELECT couple_id, actor_id, recipient_id, event_type, target_kind, target_id, b1
          FROM usapp_notification_optional_source('album_batch', b1, NULL) RETURNING id INTO n_batch;
    INSERT INTO notification_intents (couple_id, actor_id, operation, album_batch_id, choice_explicit, requested,
                                      recipient_id, notification_id)
        VALUES (c1, ua, 'album_photos_add', b1, true, true, ub, n_batch);
    INSERT INTO notification_intents (couple_id, actor_id, operation, album_batch_id, choice_explicit, requested,
                                      recipient_id)
        VALUES (c1, ub, 'album_photos_add', b2, false, false, ua);
    -- The deferred link passes for every valid notice, then runs per statement for the refusals below.
    SET CONSTRAINTS notifications_optional_intent IMMEDIATE;
    IF (SELECT target_kind || ':' || target_id FROM notifications WHERE id = n_batch) IS DISTINCT FROM 'album:' || alb1
       OR (SELECT recipient_id FROM notifications WHERE id = n_plan) IS DISTINCT FROM ub
       OR EXISTS (SELECT 1 FROM notification_intents WHERE couple_id IN (c1, c2) AND preference_seq IS NOT NULL) THEN
        RAISE EXCEPTION 'B 失败：有效夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    FOR rec IN
        SELECT * FROM (VALUES
            -- C. Notices: plan conversion, recipient, family, couple, uniqueness, single source and the intent link.
            ('C1 plan-created memory', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, memory_id) VALUES (%L, %L, %L, 'memory.created', 'memory', %L, %L)$s$,
                c1, ua, ub, mem2, mem2), 'UN102'),
            ('C2 wrong recipient', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, memory_id) VALUES (%L, %L, %L, 'memory.created', 'memory', %L, %L)$s$,
                c1, ua, uc, mem4, mem4), 'UN102'),
            ('C3 actor as recipient', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, shared_plan_id) VALUES (%L, %L, %L, 'shared_plan.created',
                'shared_plan', %L, %L)$s$, c1, ua, ua, p_done, p_done), 'UN102'),
            ('C4 wrong family', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, memory_id) VALUES (%L, %L, %L, 'album.photos_added', 'album', %L, %L)$s$,
                c1, ua, ub, mem4, mem4), 'UN102'),
            ('C5 batch of another couple', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, album_batch_id) VALUES (%L, %L, %L, 'album.photos_added', 'album',
                %L, %L)$s$, c1, ua, ub, alb2, b3), 'UN102'),
            ('C6 batch as another kind', format($s$INSERT INTO notifications (couple_id,
                actor_id, recipient_id, event_type, target_kind, target_id, album_batch_id) VALUES (%L, %L, %L,
                'album.photos_added', 'memory', %L, %L)$s$, c1, ua, ub, alb1, b4), 'UN102'),
            ('C7 second notice per batch', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, album_batch_id) VALUES (%L, %L, %L, 'album.photos_added', 'album',
                %L, %L)$s$, c1, ua, ub, alb1, b1), '23505'),
            ('C8 two sources', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, memory_id, album_batch_id) VALUES (%L, %L, %L, 'memory.created',
                'memory', %L, %L, %L)$s$, c1, ua, ub, mem4, mem4, b4), '23514'),
            ('C9 notice without intent', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, album_batch_id) VALUES (%L, %L, %L, 'album.photos_added', 'album',
                %L, %L)$s$, c1, ua, ub, alb1, b4), 'UN105'),
            ('C10 memory of another couple', format($s$INSERT INTO notifications (couple_id, actor_id, recipient_id,
                event_type, target_kind, target_id, memory_id) VALUES (%L, %L, %L, 'memory.created', 'memory', %L, %L)$s$,
                c1, ua, ub, mem3, mem3), 'UN102'),
            -- D. Intents: never remembered, one per source, the creator only, no plan conversion, exact family and link.
            ('D1 remembered choice', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                memory_id, choice_explicit, requested, recipient_id, preference_seq) VALUES (%L, %L, 'memory_create',
                %L, true, false, %L, 1)$s$, c1, ua, mem4, ub), '23514'),
            ('D2 second intent', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                memory_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'memory_create', %L, false, false,
                %L)$s$, c1, ua, mem1, ub), '23505'),
            ('D3 not the creator', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                shared_plan_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'shared_plan_create', %L,
                false, false, %L)$s$, c1, ub, p_done, ua), 'UN104'),
            ('D4 plan conversion', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                memory_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'memory_create', %L, false, false,
                %L)$s$, c1, ua, mem2, ub), 'UN104'),
            ('D5 family mismatch', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                album_batch_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'memory_create', %L, false,
                false, %L)$s$, c1, ua, b4, ub), 'UN104'),
            ('D6 forged link', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                memory_id, choice_explicit, requested, recipient_id, notification_id) VALUES (%L, %L, 'memory_create',
                %L, true, true, %L, %L)$s$, c1, ua, mem4, ub, n_batch), 'UN104'),
            ('D7 requested without notice', format($s$INSERT INTO notification_intents (couple_id, actor_id,
                operation, album_batch_id, choice_explicit, requested, recipient_id) VALUES (%L, %L, 'album_photos_add',
                %L, true, true, %L)$s$, c1, ua, b4, ub), '23514'),
            ('D8 recipient hidden', format($s$INSERT INTO notification_intents (couple_id, actor_id, operation,
                album_batch_id, choice_explicit, requested) VALUES (%L, %L, 'album_photos_add', %L, false, false)$s$,
                c1, ua, b4), 'UN104'),
            -- E. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
            ('E1 update intent', format($s$UPDATE notification_intents SET requested = false WHERE memory_id = %L$s$,
                mem1), app_or),
            ('E2 delete intent', format('DELETE FROM notification_intents WHERE album_batch_id = %L', b1), app_or)
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

    -- F. Final state: exactly the valid rows, no remembered preference, no wallet effect.
    IF (SELECT count(*) FROM notification_intents WHERE couple_id IN (c1, c2)) <> 5
       OR (SELECT count(*) FROM notifications WHERE couple_id IN (c1, c2)) <> 3
       OR EXISTS (SELECT 1 FROM notification_intents WHERE couple_id IN (c1, c2) AND preference_seq IS NOT NULL)
       OR EXISTS (SELECT 1 FROM notification_intents WHERE memory_id = mem2)
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id IN (c1, c2)) THEN
        RAISE EXCEPTION 'F 失败：D20 夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL PHASE3 NOTIFICATION ASSERTIONS PASSED: role=% mode=% groups=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed, n_cases;
END
$phase3_test$;

ROLLBACK;
