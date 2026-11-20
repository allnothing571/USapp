-- =====================================================================
-- database/tests/anniversary_ledger_test.sql
--
-- P2-2: database-side tests for anniversaries (docs/phase2-anniversaries.md).
-- Requires 0018_anniversaries on usapp_test; refuses any other database before writing
-- fixtures. Run twice, like message_ledger_test.sql:
--   1) as usapp_api: UPDATE / DELETE / TRUNCATE expect 42501 (privilege barrier);
--   2) as the maintenance superuser: they expect UN001 (append-only trigger).
-- The whole file runs inside BEGIN ... ROLLBACK and commits nothing. Every failing
-- case runs in a sub-block that is rolled back; an unexpected success or a wrong
-- SQLSTATE raises, and ON_ERROR_STOP makes psql exit non-zero. After the valid
-- fixtures, SET CONSTRAINTS ALL IMMEDIATE runs the deferred UN005 checks per statement.
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $anniversary_test$
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
    a1 uuid := gen_random_uuid();  -- ua's; approved P1 (v2), pending P2
    a2 uuid := gen_random_uuid();  -- ub's; pending P3 from ua
    a3 uuid := gen_random_uuid();  -- ua's; no proposals
    ax uuid := gen_random_uuid();  -- uc's, in c2
    p1 uuid := gen_random_uuid();
    p2 uuid := gen_random_uuid();
    p3 uuid := gen_random_uuid();
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
    IF to_regclass('public.anniversary_home_pins') IS NULL THEN
        RAISE EXCEPTION '缺少纪念日表：请先在测试库应用 0018_anniversaries';
    END IF;
    app_or := CASE WHEN is_app THEN '42501' ELSE 'UN001' END;

    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'anniv_psql_a_' || tag || '@example.com', 'anniv_psql_a_' || tag || '@example.com'),
               (ub, 'anniv_psql_b_' || tag || '@example.com', 'anniv_psql_b_' || tag || '@example.com'),
               (uc, 'anniv_psql_c_' || tag || '@example.com', 'anniv_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'anniv-psql', ua), (c2, 'anniv-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);

    -- A. Valid fixtures: create, creator edit, proposal approved, pending proposals, pins.
    INSERT INTO anniversaries (id, couple_id, creator_id) VALUES (a1, c1, ua), (a2, c1, ub), (a3, c1, ua), (ax, c2, uc);
    INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, title, event_date, kind,
                                       note, idempotency_key, request_fingerprint)
        VALUES (a1, c1, 1, ua, 'create', '在一起', DATE '2020-02-29', 'anniversary', '第一次' || chr(10) || '见面',
                'psql-ann-a1-' || tag, fp),
               (a2, c1, 1, ub, 'create', '考试', DATE '2199-12-31', 'countdown', '', 'psql-ann-a2-' || tag, fp),
               (a3, c1, 1, ua, 'create', '生日', DATE '1900-01-01', 'anniversary', '', 'psql-ann-a3-' || tag, fp),
               (ax, c2, 1, uc, 'create', '别人的', DATE '2021-01-01', 'anniversary', '', 'psql-ann-ax-' || tag, fp);
    INSERT INTO anniversary_edit_proposals (id, couple_id, anniversary_id, proposer_id, base_version, title,
                                            event_date, kind, note, idempotency_key, request_fingerprint)
        VALUES (p1, c1, a1, ub, 1, '在一起（改）', DATE '2020-03-01', 'anniversary', '', 'psql-ann-p1-' || tag, fp);
    INSERT INTO anniversary_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key,
                                            request_fingerprint)
        VALUES (p1, c1, ua, 'approved', 'psql-ann-d1-' || tag, fp);
    INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id, action, proposal_id, title,
                                       event_date, kind, note)
        VALUES (a1, c1, 2, ua, 'approve', p1, '在一起（改）', DATE '2020-03-01', 'anniversary', '');
    INSERT INTO anniversary_edit_proposals (id, couple_id, anniversary_id, proposer_id, base_version, title,
                                            event_date, kind, note, idempotency_key, request_fingerprint)
        VALUES (p2, c1, a1, ub, 2, '再改', DATE '2020-03-01', 'anniversary', '', 'psql-ann-p2-' || tag, fp),
               (p3, c1, a2, ua, 1, '考试（改）', DATE '2199-12-30', 'countdown', '', 'psql-ann-p3-' || tag, fp);
    INSERT INTO anniversary_home_pins (user_id, seq, couple_id, anniversary_id, idempotency_key, request_fingerprint)
        VALUES (ua, 1, c1, a2, 'psql-ann-pin1-' || tag, fp);
    INSERT INTO anniversary_home_pins (user_id, seq, couple_id, anniversary_id, idempotency_key, request_fingerprint)
        VALUES (ua, 2, c1, NULL, 'psql-ann-pin2-' || tag, fp);
    SET CONSTRAINTS ALL IMMEDIATE;  -- the deferred UN005 checks pass for the valid fixtures
    n_passed := n_passed + 1;

    -- B-G. Each statement must fail with exactly the expected SQLSTATE.
    FOR rec IN
        SELECT * FROM (VALUES
            -- B. Shapes and identity.
            ('B1 title 41', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id,
                action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L, 'edit',
                repeat('长', 41), DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ua, 'psql-ann-b1-' || tag, fp),
             '23514'),
            ('B2 title newline', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'edit', 'a' || chr(10) || 'b', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ua,
                'psql-ann-b2-' || tag, fp), '23514'),
            ('B3 note control', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, note, idempotency_key, request_fingerprint) VALUES (%L, %L,
                2, %L, 'edit', 'a', DATE '2020-01-01', 'anniversary', 'x' || chr(7), %L, %L)$s$, a3, c1, ua,
                'psql-ann-b3-' || tag, fp), '23514'),
            ('B4 date range', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'edit', 'a', DATE '1899-12-31', 'anniversary', %L, %L)$s$, a3, c1, ua, 'psql-ann-b4-' || tag, fp),
             '23514'),
            ('B5 kind', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id,
                action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L, 'edit',
                'a', DATE '2020-01-01', 'birthday', %L, %L)$s$, a3, c1, ua, 'psql-ann-b5-' || tag, fp), '23514'),
            ('B6 key shape', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version, actor_id,
                action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L, 'edit',
                'a', DATE '2020-01-01', 'anniversary', 'short', %L)$s$, a3, c1, ua, fp), '23514'),
            ('B7 create not first', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'create', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ua, 'psql-ann-b7-' || tag, fp),
             '23514'),
            ('B8 duplicate key', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ua, 'psql-ann-a3-' || tag, fp),
             '23505'),
            -- C. Revision guard (UN002).
            ('C1 partner revision', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ub, 'psql-ann-c1-' || tag, fp),
             'UN002'),
            ('C2 version gap', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 3, %L,
                'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a3, c1, ua, 'psql-ann-c2-' || tag, fp),
             'UN002'),
            ('C3 edit while pending', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 3, %L,
                'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, a1, c1, ua, 'psql-ann-c3-' || tag, fp),
             'UN002'),
            ('C4 approve without decision', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id,
                version, actor_id, action, proposal_id, title, event_date, kind, note) VALUES (%L, %L, 3, %L, 'approve',
                %L, '再改', DATE '2020-03-01', 'anniversary', '')$s$, a1, c1, ua, p2), 'UN002'),
            ('C5 future time', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint, created_at) VALUES (%L,
                %L, 2, %L, 'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L, clock_timestamp() + interval '1 hour')$s$,
                a3, c1, ua, 'psql-ann-c5-' || tag, fp), 'UN002'),
            ('C6 couple mismatch', format($s$INSERT INTO anniversary_revisions (anniversary_id, couple_id, version,
                actor_id, action, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L, 2, %L,
                'edit', 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, ax, c1, ua, 'psql-ann-c6-' || tag, fp),
             'UN002'),
            -- D. Proposal guard (UN003) and membership.
            ('D1 creator proposing', format($s$INSERT INTO anniversary_edit_proposals (couple_id, anniversary_id,
                proposer_id, base_version, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L,
                %L, 1, 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, c1, a3, ua, 'psql-ann-d1x-' || tag, fp),
             'UN003'),
            ('D2 stale base', format($s$INSERT INTO anniversary_edit_proposals (couple_id, anniversary_id,
                proposer_id, base_version, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L,
                %L, 2, 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, c1, a3, ub, 'psql-ann-d2-' || tag, fp),
             'UN003'),
            ('D3 second pending', format($s$INSERT INTO anniversary_edit_proposals (couple_id, anniversary_id,
                proposer_id, base_version, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L,
                %L, 2, 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, c1, a1, ub, 'psql-ann-d3-' || tag, fp),
             'UN003'),
            ('D4 non-member proposer', format($s$INSERT INTO anniversary_edit_proposals (couple_id, anniversary_id,
                proposer_id, base_version, title, event_date, kind, idempotency_key, request_fingerprint) VALUES (%L, %L,
                %L, 1, 'a', DATE '2020-01-01', 'anniversary', %L, %L)$s$, c1, a3, uc, 'psql-ann-d4-' || tag, fp),
             '23503'),
            -- E. Decision guard (UN004) and one decision per proposal.
            ('E1 partner approving', format($s$INSERT INTO anniversary_edit_decisions (proposal_id, couple_id,
                decided_by, outcome, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, 'approved', %L, %L)$s$,
                p2, c1, ub, 'psql-ann-e1-' || tag, fp), 'UN004'),
            ('E2 creator withdrawing', format($s$INSERT INTO anniversary_edit_decisions (proposal_id, couple_id,
                decided_by, outcome, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, 'withdrawn', %L, %L)$s$,
                p2, c1, ua, 'psql-ann-e2-' || tag, fp), 'UN004'),
            ('E3 other couple', format($s$INSERT INTO anniversary_edit_decisions (proposal_id, couple_id,
                decided_by, outcome, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, 'rejected', %L, %L)$s$,
                p2, c2, uc, 'psql-ann-e3-' || tag, fp), 'UN004'),
            ('E4 second decision', format($s$INSERT INTO anniversary_edit_decisions (proposal_id, couple_id,
                decided_by, outcome, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, 'rejected', %L, %L)$s$,
                p1, c1, ua, 'psql-ann-e4-' || tag, fp), '23505'),
            -- F. Pin guard (UN006), same-space target and key uniqueness.
            ('F1 pin seq gap', format($s$INSERT INTO anniversary_home_pins (user_id, seq, couple_id, anniversary_id,
                idempotency_key, request_fingerprint) VALUES (%L, 4, %L, NULL, %L, %L)$s$, ua, c1,
                'psql-ann-f1-' || tag, fp), 'UN006'),
            ('F2 pin other space item', format($s$INSERT INTO anniversary_home_pins (user_id, seq, couple_id,
                anniversary_id, idempotency_key, request_fingerprint) VALUES (%L, 3, %L, %L, %L, %L)$s$, ua, c1, ax,
                'psql-ann-f2-' || tag, fp), '23503'),
            ('F3 pin as non-member', format($s$INSERT INTO anniversary_home_pins (user_id, seq, couple_id,
                anniversary_id, idempotency_key, request_fingerprint) VALUES (%L, 3, %L, NULL, %L, %L)$s$, ua, c2,
                'psql-ann-f3-' || tag, fp), '23503'),
            ('F4 pin key reused', format($s$INSERT INTO anniversary_home_pins (user_id, seq, couple_id,
                anniversary_id, idempotency_key, request_fingerprint) VALUES (%L, 3, %L, NULL, %L, %L)$s$, ua, c1,
                'psql-ann-pin1-' || tag, fp), '23505'),
            -- G. Deferred links (UN005), checked per statement after SET CONSTRAINTS ALL IMMEDIATE.
            ('G1 identity without create', format($s$INSERT INTO anniversaries (couple_id, creator_id) VALUES (%L,
                %L)$s$, c1, ua), 'UN005'),
            ('G2 approval without revision', format($s$INSERT INTO anniversary_edit_decisions (proposal_id,
                couple_id, decided_by, outcome, idempotency_key, request_fingerprint) VALUES (%L, %L, %L, 'approved',
                %L, %L)$s$, p3, c1, ub, 'psql-ann-g2-' || tag, fp), 'UN005'),
            -- H. Append-only: privilege barrier for usapp_api, trigger for the maintenance role.
            ('H1 update identity', format('UPDATE anniversaries SET creator_id = creator_id WHERE id = %L', a1), app_or),
            ('H2 update revision', format($s$UPDATE anniversary_revisions SET title = 'x' WHERE anniversary_id = %L$s$,
                a1), app_or),
            ('H3 delete proposal', format('DELETE FROM anniversary_edit_proposals WHERE id = %L', p2), app_or),
            ('H4 delete decision', format('DELETE FROM anniversary_edit_decisions WHERE proposal_id = %L', p1), app_or),
            ('H5 update pin', format('UPDATE anniversary_home_pins SET anniversary_id = NULL WHERE user_id = %L', ua),
             app_or),
            ('H6 truncate', 'TRUNCATE anniversary_home_pins, anniversary_edit_decisions, anniversary_edit_proposals, '
                || 'anniversary_revisions, anniversaries' || CASE WHEN to_regclass('public.notifications') IS NOT NULL
                    THEN ', notifications, notification_reads' ELSE '' END
                || CASE WHEN to_regclass('public.notification_intents') IS NOT NULL
                    THEN ', notification_intents' ELSE '' END
                || CASE WHEN to_regclass('public.push_deliveries') IS NOT NULL
                    THEN ', push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations' ELSE '' END
                -- 0025 references anniversaries and their revisions: listed last, no CASCADE.
                || CASE WHEN to_regclass('public.reminder_occurrences') IS NOT NULL
                    THEN ', reminder_occurrences, reminder_rules' ELSE '' END, app_or)
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

    -- I. The valid fixture is intact and wrote no wallet data.
    IF (SELECT max(version) FROM anniversary_revisions WHERE anniversary_id = a1) <> 2
       OR (SELECT title FROM anniversary_revisions WHERE anniversary_id = a1 AND version = 2) <> '在一起（改）'
       OR (SELECT count(*) FROM anniversary_edit_decisions WHERE couple_id = c1) <> 1
       OR (SELECT count(*) FROM anniversary_home_pins WHERE user_id = ua) <> 2
       OR (SELECT count(*) FROM anniversaries WHERE couple_id = c1) <> 3
       OR EXISTS (SELECT 1 FROM wallet_operations WHERE couple_id IN (c1, c2)) THEN
        RAISE EXCEPTION 'I1 失败：纪念日夹具状态不符';
    END IF;
    n_passed := n_passed + 1;

    RAISE NOTICE 'ALL ANNIVERSARY ASSERTIONS PASSED: role=% mode=% groups=% cases=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed, n_cases;
END
$anniversary_test$;

ROLLBACK;
