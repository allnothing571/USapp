-- =====================================================================
-- database/tests/task_ledger_test.sql
--
-- 阶段 1 M3-A：任务表与任务结算一致性的数据库侧测试（只编写；由项目负责人手动执行）。
-- 需要已应用 0006_wallet_ledger 与 0007_tasks_one_off 的测试库（usapp_test），绝不对主库 usapp 执行。
-- 脚本在任何夹具写入之前检查 current_database() = 'usapp_test'，不符则 RAISE EXCEPTION。
--
-- 运行两次（与 wallet_ledger_test.sql 相同方式）：
--   1) 以应用角色 usapp_api：任务表上的 UPDATE / DELETE / TRUNCATE 期望 42501（权限屏障）。
--   2) 以维护超级用户：权限不再是屏障，UPDATE / DELETE / TRUNCATE 期望 UT001（触发器）。
--      TRUNCATE 前先 SET CONSTRAINTS ALL IMMEDIATE（有待触发的延迟事件时 PostgreSQL 拒绝 TRUNCATE），
--      并把 tasks 与 task_completions 一起截断（单独截断 tasks 会先因外键引用失败，不能证明触发器）。
--   其他身份一律拒绝运行。
--
-- 约定：
--   * 整个文件包在 BEGIN ... ROLLBACK 中，不提交任何数据。
--   * 每个“应当失败”的用例包在 BEGIN ... EXCEPTION 子块中；子块中的准备数据随子块回滚。
--   * 意外成功、或 SQLSTATE / 约束名不符时 RAISE EXCEPTION；配合 ON_ERROR_STOP，psql 以非零码退出，
--     事务中止，不提交任何数据。
--   * IF 条件中的 CASE 一律加括号（PL/pgSQL 以第一个顶层 THEN 结束 IF 条件）。
--   * 不测试真实 COMMIT 时的延迟校验（本脚本从不提交）；该项由 db_gated 的真实提交尝试覆盖。
--   * 跨事务追加到真正已提交的结算属于 M3-C；这里只在同一事务内、合法结算通过 IMMEDIATE 之后追加。
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $task_test$
DECLARE
    is_app       boolean;
    run_role     text := current_user;
    v_constraint text;
    n_passed     integer := 0;
    ua uuid := gen_random_uuid();
    ub uuid := gen_random_uuid();
    uc uuid := gen_random_uuid();
    c1 uuid := gen_random_uuid();
    c2 uuid := gen_random_uuid();
    t1 uuid := gen_random_uuid();
    t2 uuid;
    op1 uuid := gen_random_uuid();
    op2 uuid;
    tag text := substr(md5(random()::text), 1, 8);
    fp  text := md5(random()::text) || md5(random()::text);
    t3 uuid := gen_random_uuid();  -- M6: open one-off task (A -> B)
    t4 uuid := gen_random_uuid();  -- M6: daily template (A -> B)
    pr uuid;
    sx uuid;
BEGIN
    -- ---------- 目标库守卫（必须先于任何夹具 DML） ----------
    IF current_database() IS DISTINCT FROM 'usapp_test' THEN
        RAISE EXCEPTION '拒绝运行：当前数据库为 %，只允许 usapp_test', current_database();
    END IF;

    -- ---------- 运行身份 ----------
    IF current_user = 'usapp_api'
       AND NOT (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        is_app := true;
    ELSIF (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        is_app := false;
    ELSE
        RAISE EXCEPTION '只支持以 usapp_api 或维护超级用户运行，当前身份: %', current_user;
    END IF;
    IF to_regclass('public.tasks') IS NULL OR to_regclass('public.task_completions') IS NULL THEN
        RAISE EXCEPTION '缺少任务表：请先在测试库应用 0007_tasks_one_off';
    END IF;

    -- ---------- 顶层夹具（随文件末尾的 ROLLBACK 丢弃） ----------
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'task_psql_a_' || tag || '@example.com', 'task_psql_a_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ub, 'task_psql_b_' || tag || '@example.com', 'task_psql_b_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (uc, 'task_psql_c_' || tag || '@example.com', 'task_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'task-psql', ua);
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c2, 'task-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'flower', c1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'sunshine', c1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ua, 'flower', c1);

    -- ================================================================
    -- A. 合法结算（A 发布、B 完成：5 小红花 + 40 阳光值），描述含 LF
    -- ================================================================
    INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers,
                       reward_sunshine, create_idempotency_key, create_fingerprint)
        VALUES (t1, c1, ua, ub, 'Read one chapter', 'line one' || chr(10) || 'line two', 5, 40,
                'psql-task-key-' || tag, fp);
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (op1, c1, ub, 'TASK_REWARD', 'task', t1::text);
    INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                  reward_sunshine, completed_at)
        VALUES (t1, c1, ub, op1, 5, 40, clock_timestamp());
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op1, c1, ub, 'flower', 1, 5, 5);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op1, c1, ub, 'sunshine', 1, 40, 40);
    SET CONSTRAINTS ALL IMMEDIATE;  -- 合法结算必须通过全部延迟校验
    SET CONSTRAINTS ALL DEFERRED;
    n_passed := n_passed + 1;

    -- 只有日期的截止时间：次日本地 0 点；发布时不要求截止时间在未来。
    t2 := gen_random_uuid();
    INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                       deadline_local_date, deadline_at, create_idempotency_key, create_fingerprint)
        VALUES (t2, c1, ub, ua, 'Past date-only', 1, 0, date '2026-10-01',
                (date '2026-10-02')::timestamp AT TIME ZONE 'Asia/Shanghai', 'psql-task-key-2-' || tag, fp);
    IF (SELECT deadline_at FROM tasks WHERE id = t2) IS DISTINCT FROM timestamptz '2026-10-02 00:00:00+08' THEN
        RAISE EXCEPTION 'A2 失败：只有日期的截止时刻不正确';
    END IF;
    n_passed := n_passed + 1;

    -- ================================================================
    -- B. 任务约束
    -- ================================================================
    BEGIN  -- B1 两项奖励都为 0
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'Zero', 0, 0, 'psql-task-key-b1-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_reward_positive' THEN
                RAISE EXCEPTION 'B1 失败：期望 23514 tasks_reward_positive，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B2 小红花超过 999
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'Too many', 1000, 0, 'psql-task-key-b2-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_reward_flowers_range' THEN
                RAISE EXCEPTION 'B2 失败：期望 23514 tasks_reward_flowers_range，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B3 阳光值超过 100000
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'Too sunny', 0, 100001, 'psql-task-key-b3-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_reward_sunshine_range' THEN
                RAISE EXCEPTION 'B3 失败：期望 23514 tasks_reward_sunshine_range，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B4 标题 41 个码点
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, repeat(chr(21517), 41), 1, 0, 'psql-task-key-b4-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_title_shape' THEN
                RAISE EXCEPTION 'B4 失败：期望 23514 tasks_title_shape，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B5 标题含换行
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'a' || chr(10) || 'b', 1, 0, 'psql-task-key-b5-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_title_shape' THEN
                RAISE EXCEPTION 'B5 失败：期望 23514 tasks_title_shape，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B6 描述含 CR
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, description, reward_flowers,
                           reward_sunshine, create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'CR', 'a' || chr(13) || chr(10) || 'b', 1, 0, 'psql-task-key-b6-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B6 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_description_shape' THEN
                RAISE EXCEPTION 'B6 失败：期望 23514 tasks_description_shape，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B7 发布给自己
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ua, 'Self', 1, 0, 'psql-task-key-b7-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B7 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_creator_not_assignee' THEN
                RAISE EXCEPTION 'B7 失败：期望 23514 tasks_creator_not_assignee，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B8 接收者不是本空间成员
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, uc, 'Outsider', 1, 0, 'psql-task-key-b8-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B8 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23503' OR v_constraint IS DISTINCT FROM 'tasks_assignee_member_fk' THEN
                RAISE EXCEPTION 'B8 失败：期望 23503 tasks_assignee_member_fk，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B9 截止时刻与本地日期/时间不符
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           deadline_local_date, deadline_local_time, deadline_at,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'Mismatch', 1, 0, date '2026-10-20', time '23:00',
                    timestamptz '2026-10-20 23:00:00+00', 'psql-task-key-b9-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B9 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_deadline_at_matches_local' THEN
                RAISE EXCEPTION 'B9 失败：期望 23514 tasks_deadline_at_matches_local，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B10 时区不受支持
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           timezone, create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'UTC', 1, 0, 'UTC', 'psql-task-key-b10-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B10 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'tasks_timezone_supported' THEN
                RAISE EXCEPTION 'B10 失败：期望 23514 tasks_timezone_supported，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B11 同一发布者重复幂等键
        INSERT INTO tasks (couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (c1, ua, ub, 'Duplicate key', 1, 0, 'psql-task-key-' || tag, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B11 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23505' OR v_constraint IS DISTINCT FROM 'tasks_create_key_unique' THEN
                RAISE EXCEPTION 'B11 失败：期望 23505 tasks_create_key_unique，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- ================================================================
    -- C. 完成记录立即校验（每个子块自建任务与操作，随子块回滚）
    -- ================================================================
    BEGIN  -- C1 发布者完成
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'C1', 2, 0, 'psql-task-key-c1-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ua, 'TASK_REWARD', 'task', t2::text);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ua, op2, 2, 0, clock_timestamp());
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT003' THEN RAISE EXCEPTION 'C1 失败：期望 UT003，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C2 奖励快照不符
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'C2', 2, 0, 'psql-task-key-c2-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ub, 'TASK_REWARD', 'task', t2::text);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ub, op2, 3, 0, clock_timestamp());
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT004' THEN RAISE EXCEPTION 'C2 失败：期望 UT004，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C3 操作类型错误
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'C3', 2, 0, 'psql-task-key-c3-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ub, 'REWARD_REDEMPTION', 'task', t2::text);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ub, op2, 2, 0, clock_timestamp());
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT005' THEN RAISE EXCEPTION 'C3 失败：期望 UT005，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C4 借用另一个任务的操作（同一空间）
        t2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'C4', 5, 40, 'psql-task-key-c4-' || tag, fp);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ub, op1, 5, 40, clock_timestamp());
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT005' THEN RAISE EXCEPTION 'C4 失败：期望 UT005，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C5 完成时间不早于截止时间
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           deadline_local_date, deadline_at, create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'C5', 2, 0, date '2026-10-01',
                    (date '2026-10-02')::timestamp AT TIME ZONE 'Asia/Shanghai', 'psql-task-key-c5-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ub, 'TASK_REWARD', 'task', t2::text);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ub, op2, 2, 0, clock_timestamp());
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT006' THEN RAISE EXCEPTION 'C5 失败：期望 UT006，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- ================================================================
    -- D. 共享结算校验器（延迟；以 SET CONSTRAINTS ALL IMMEDIATE 触发）
    -- ================================================================
    BEGIN  -- D1 任务操作没有完成记录
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'D1', 2, 0, 'psql-task-key-d1-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ub, 'TASK_REWARD', 'task', t2::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op2, c1, ub, 'flower', 2, 2, 7);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT007' THEN RAISE EXCEPTION 'D1 失败：期望 UT007，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;
    SET CONSTRAINTS ALL DEFERRED;

    BEGIN  -- D2 已通过校验的结算之后，追加另一成员的流水
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ua, 'flower', 1, 1, 1);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT007' THEN RAISE EXCEPTION 'D2 失败：期望 UT007，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;
    SET CONSTRAINTS ALL DEFERRED;

    BEGIN  -- D3 已通过校验的结算之后，追加未奖励资产的流水（另建只奖励小红花的结算）
        t2 := gen_random_uuid(); op2 := gen_random_uuid();
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t2, c1, ua, ub, 'D3', 2, 0, 'psql-task-key-d3-' || tag, fp);
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ub, 'TASK_REWARD', 'task', t2::text);
        INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                      reward_sunshine, completed_at)
            VALUES (t2, c1, ub, op2, 2, 0, clock_timestamp());
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op2, c1, ub, 'flower', 2, 2, 7);
        SET CONSTRAINTS ALL IMMEDIATE;  -- 此时合法
        SET CONSTRAINTS ALL DEFERRED;
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op2, c1, ub, 'sunshine', 2, 1, 41);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UT007' THEN RAISE EXCEPTION 'D3 失败：期望 UT007，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;
    SET CONSTRAINTS ALL DEFERRED;

    BEGIN  -- D4 非任务操作不受影响（source_type = 'psql_test'，记给任一成员都可以）
        op2 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op2, c1, ua, 'TASK_REWARD', 'psql_test', op2::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op2, c1, ua, 'flower', 1, 3, 3);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D4 正常结束（子块回滚）';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN NULL;  -- 预期：非任务操作通过全部校验
        WHEN OTHERS THEN
            RAISE EXCEPTION 'D4 失败：非任务操作被拒绝 %', SQLSTATE;
    END;
    n_passed := n_passed + 1;
    SET CONSTRAINTS ALL DEFERRED;

    -- ================================================================
    -- E. 只追加：应用角色看到权限错误，维护角色看到触发器错误
    -- ================================================================
    BEGIN  -- E1 UPDATE tasks
        UPDATE tasks SET title = title WHERE id = t1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                RAISE EXCEPTION 'E1 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E2 DELETE tasks
        DELETE FROM tasks WHERE id = t1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                RAISE EXCEPTION 'E2 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E3 UPDATE task_completions
        UPDATE task_completions SET reward_flowers = reward_flowers WHERE task_id = t1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                RAISE EXCEPTION 'E3 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E4 DELETE task_completions
        DELETE FROM task_completions WHERE task_id = t1;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                RAISE EXCEPTION 'E4 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    IF is_app THEN
        BEGIN  -- E5a 应用角色：TRUNCATE tasks
            TRUNCATE tasks;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E5a 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> '42501' THEN RAISE EXCEPTION 'E5a 失败：期望 42501，实际 %', SQLSTATE; END IF;
        END;
        BEGIN  -- E5b 应用角色：TRUNCATE task_completions
            TRUNCATE task_completions;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E5b 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> '42501' THEN RAISE EXCEPTION 'E5b 失败：期望 42501，实际 %', SQLSTATE; END IF;
        END;
    ELSE
        -- 有待触发的延迟事件时 PostgreSQL 拒绝 TRUNCATE（55006），先让它们立即执行（此时顶层数据合法）。
        SET CONSTRAINTS ALL IMMEDIATE;
        BEGIN  -- E5 维护角色：两张任务表一起 TRUNCATE（外键检查通过后由触发器拒绝）
            IF to_regclass('public.task_progress_events') IS NULL THEN
                TRUNCATE tasks, task_completions;
            ELSIF to_regclass('public.task_closures') IS NULL THEN
                TRUNCATE tasks, task_completions, task_progress_events;
            ELSE
                -- M6 tables reference tasks; listing them keeps the FK check from masking UT001.
                -- 0023 adds the push dispatch chain referencing notifications: listed last, no CASCADE.
                -- 0025 adds reminder rules and occurrences referencing tasks: listed last, no CASCADE.
                IF to_regclass('public.reminder_occurrences') IS NOT NULL THEN
                    TRUNCATE tasks, task_completions, task_progress_events, task_change_proposals, task_change_decisions, task_closures, notifications, notification_reads, notification_intents, task_reactions, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, reminder_occurrences, reminder_rules;
                ELSIF to_regclass('public.push_deliveries') IS NOT NULL THEN
                    TRUNCATE tasks, task_completions, task_progress_events, task_change_proposals, task_change_decisions, task_closures, notifications, notification_reads, notification_intents, task_reactions, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations;
                ELSIF to_regclass('public.task_reactions') IS NOT NULL THEN
                    TRUNCATE tasks, task_completions, task_progress_events, task_change_proposals, task_change_decisions, task_closures, notifications, notification_reads, notification_intents, task_reactions;
                ELSIF to_regclass('public.notification_intents') IS NOT NULL THEN
                    TRUNCATE tasks, task_completions, task_progress_events,
                             task_change_proposals, task_change_decisions, task_closures,
                             notifications, notification_reads, notification_intents;
                ELSIF to_regclass('public.notifications') IS NOT NULL THEN
                    TRUNCATE tasks, task_completions, task_progress_events,
                             task_change_proposals, task_change_decisions, task_closures,
                             notifications, notification_reads;
                ELSE
                    TRUNCATE tasks, task_completions, task_progress_events,
                             task_change_proposals, task_change_decisions, task_closures;
                END IF;
            END IF;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E5 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UT001' THEN RAISE EXCEPTION 'E5 失败：期望 UT001，实际 %', SQLSTATE; END IF;
        END;
        SET CONSTRAINTS ALL DEFERRED;
    END IF;
    n_passed := n_passed + 1;

    -- ================================================================
    -- F. M6 change proposals, decisions and closures (only when 0013 is applied)
    --    Every case runs in a sub-block that is rolled back; direct SQL as either role.
    -- ================================================================
    IF to_regclass('public.task_closures') IS NOT NULL THEN
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint)
            VALUES (t3, c1, ua, ub, 'Change me', 2, 0, 'psql-task-key-f-' || tag, fp);
        INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, reward_flowers, reward_sunshine,
                           create_idempotency_key, create_fingerprint, completion_mode, target_count, schedule_type)
            VALUES (t4, c1, ua, ub, 'Daily change', 1, 0, 'psql-task-key-d-' || tag, fp, 'count', 1, 'daily');

        BEGIN  -- F1 the new tables are append-only
            UPDATE task_change_proposals SET kind = kind WHERE task_id = t3;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F1 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                    RAISE EXCEPTION 'F1 失败：身份 % 实际 %', run_role, SQLSTATE;
                END IF;
        END;
        BEGIN
            DELETE FROM task_closures WHERE task_id = t3;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F1b 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UT001' END) THEN
                    RAISE EXCEPTION 'F1b 失败：身份 % 实际 %', run_role, SQLSTATE;
                END IF;
        END;
        n_passed := n_passed + 1;

        BEGIN  -- F2 only the creator proposes an edit (UX002)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, title, description,
                reward_flowers, reward_sunshine, completion_mode, target_count, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ub, 'edit', 'Changed', '', 3, 0, 'single', 1, 'psql-change-f2-' || tag, fp);
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F2 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UX002' THEN RAISE EXCEPTION 'F2 失败：期望 UX002，实际 %', SQLSTATE; END IF;
        END;
        n_passed := n_passed + 1;

        BEGIN  -- F3 one undecided proposal per root (UX002); F4 the proposer cannot approve (UX003)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ub, 'cancel', 'psql-change-f3-' || tag, fp) RETURNING id INTO pr;
            BEGIN
                INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                    VALUES (c1, t3, ua, 'cancel', 'psql-change-f3b-' || tag, fp);
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F3 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> 'UX002' THEN RAISE EXCEPTION 'F3 失败：期望 UX002，实际 %', SQLSTATE; END IF;
            END;
            BEGIN
                INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint)
                    VALUES (pr, c1, ub, 'approved', 'psql-change-f4-' || tag, fp);
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F4 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> 'UX003' THEN RAISE EXCEPTION 'F4 失败：期望 UX003，实际 %', SQLSTATE; END IF;
            END;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F3/F4 正常结束（子块回滚）';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN
                IF SQLERRM <> 'F3/F4 正常结束（子块回滚）' THEN RAISE; END IF;
        END;
        n_passed := n_passed + 1;

        BEGIN  -- F5 a rejected decision cannot close the task (UX004)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ub, 'cancel', 'psql-change-f5-' || tag, fp) RETURNING id INTO pr;
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint)
                VALUES (pr, c1, ua, 'rejected', 'psql-change-f5d-' || tag, fp);
            INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at)
                SELECT t3, c1, 'cancelled', pr, d.decided_at FROM task_change_decisions d WHERE d.proposal_id = pr;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F5 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UX004' THEN RAISE EXCEPTION 'F5 失败：期望 UX004，实际 %', SQLSTATE; END IF;
        END;
        n_passed := n_passed + 1;

        BEGIN  -- F6 an approved decision without its closure fails the deferred link check (UX005)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ub, 'cancel', 'psql-change-f6-' || tag, fp) RETURNING id INTO pr;
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint)
                VALUES (pr, c1, ua, 'approved', 'psql-change-f6d-' || tag, fp);
            SET CONSTRAINTS ALL IMMEDIATE;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F6 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UX005' THEN RAISE EXCEPTION 'F6 失败：期望 UX005，实际 %', SQLSTATE; END IF;
        END;
        SET CONSTRAINTS ALL DEFERRED;
        n_passed := n_passed + 1;

        BEGIN  -- F7 a valid cancellation; afterwards progress/completion are rejected (UX006)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ub, 'cancel', 'psql-change-f7-' || tag, fp) RETURNING id INTO pr;
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint)
                VALUES (pr, c1, ua, 'approved', 'psql-change-f7d-' || tag, fp);
            INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at)
                SELECT t3, c1, 'cancelled', pr, d.decided_at FROM task_change_decisions d WHERE d.proposal_id = pr;
            SET CONSTRAINTS ALL IMMEDIATE;
            SET CONSTRAINTS ALL DEFERRED;
            BEGIN
                op2 := gen_random_uuid();
                INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
                    VALUES (op2, c1, ub, 'TASK_REWARD', 'task', t3::text);
                INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers,
                                              reward_sunshine, completed_at)
                    VALUES (t3, c1, ub, op2, 2, 0, clock_timestamp());
                RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F7 失败：意外成功';
            EXCEPTION
                WHEN SQLSTATE 'UWT00' THEN RAISE;
                WHEN OTHERS THEN
                    IF SQLSTATE <> 'UX006' THEN RAISE EXCEPTION 'F7 失败：期望 UX006，实际 %', SQLSTATE; END IF;
            END;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F7 正常结束（子块回滚）';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN
                IF SQLERRM <> 'F7 正常结束（子块回滚）' THEN RAISE; END IF;
        END;
        SET CONSTRAINTS ALL DEFERRED;
        n_passed := n_passed + 1;

        BEGIN  -- F8 a cancelled daily template cannot materialize a period (UX007)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, idempotency_key, request_fingerprint)
                VALUES (c1, t4, ua, 'cancel', 'psql-change-f8-' || tag, fp) RETURNING id INTO pr;
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint)
                VALUES (pr, c1, ub, 'approved', 'psql-change-f8d-' || tag, fp);
            INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at)
                SELECT t4, c1, 'cancelled', pr, d.decided_at FROM task_change_decisions d WHERE d.proposal_id = pr;
            INSERT INTO tasks (couple_id, creator_id, assignee_id, title, description, reward_flowers, reward_sunshine,
                               timezone, deadline_local_date, deadline_at, create_idempotency_key, create_fingerprint,
                               created_at, completion_mode, target_count, recurrence_id, period_date)
                SELECT r.couple_id, r.creator_id, r.assignee_id, r.title, r.description, r.reward_flowers,
                       r.reward_sunshine, r.timezone, x.d, (x.d + 1)::timestamp AT TIME ZONE 'Asia/Shanghai',
                       'psql-task-key-p-' || tag, r.create_fingerprint, clock_timestamp(), r.completion_mode,
                       r.target_count, r.id, x.d
                  FROM tasks r CROSS JOIN (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d) x
                 WHERE r.id = t4;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F8 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UX007' THEN RAISE EXCEPTION 'F8 失败：期望 UX007，实际 %', SQLSTATE; END IF;
        END;
        n_passed := n_passed + 1;

        BEGIN  -- F9 an approved edit with an exact successor passes; a mismatched successor fails (UX005)
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, title, description,
                reward_flowers, reward_sunshine, completion_mode, target_count, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ua, 'edit', 'Changed', '', 3, 0, 'single', 1, 'psql-change-f9-' || tag, fp)
                RETURNING id INTO pr;
            sx := gen_random_uuid();
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, successor_task_id,
                                               idempotency_key, request_fingerprint)
                VALUES (pr, c1, ub, 'approved', sx, 'psql-change-f9d-' || tag, fp);
            INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers,
                               reward_sunshine, create_idempotency_key, create_fingerprint, created_at)
                SELECT sx, c1, ua, ub, 'Changed', '', 3, 0, 'chg:' || pr::text, fp, d.decided_at
                  FROM task_change_decisions d WHERE d.proposal_id = pr;
            INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at)
                SELECT t3, c1, 'superseded', pr, d.decided_at FROM task_change_decisions d WHERE d.proposal_id = pr;
            SET CONSTRAINTS ALL IMMEDIATE;
            SET CONSTRAINTS ALL DEFERRED;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F9 正常结束（子块回滚）';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN
                IF SQLERRM <> 'F9 正常结束（子块回滚）' THEN RAISE; END IF;
            WHEN OTHERS THEN
                RAISE EXCEPTION 'F9 失败：合法替换被拒绝 %', SQLSTATE;
        END;
        SET CONSTRAINTS ALL DEFERRED;
        BEGIN
            INSERT INTO task_change_proposals (couple_id, task_id, proposer_id, kind, title, description,
                reward_flowers, reward_sunshine, completion_mode, target_count, idempotency_key, request_fingerprint)
                VALUES (c1, t3, ua, 'edit', 'Changed', '', 3, 0, 'single', 1, 'psql-change-f9b-' || tag, fp)
                RETURNING id INTO pr;
            sx := gen_random_uuid();
            INSERT INTO task_change_decisions (proposal_id, couple_id, decided_by, outcome, successor_task_id,
                                               idempotency_key, request_fingerprint)
                VALUES (pr, c1, ub, 'approved', sx, 'psql-change-f9e-' || tag, fp);
            INSERT INTO tasks (id, couple_id, creator_id, assignee_id, title, description, reward_flowers,
                               reward_sunshine, create_idempotency_key, create_fingerprint, created_at)
                SELECT sx, c1, ua, ub, 'Changed', '', 4, 0, 'chg:' || pr::text, fp, d.decided_at
                  FROM task_change_decisions d WHERE d.proposal_id = pr;
            INSERT INTO task_closures (task_id, couple_id, reason, proposal_id, closed_at)
                SELECT t3, c1, 'superseded', pr, d.decided_at FROM task_change_decisions d WHERE d.proposal_id = pr;
            SET CONSTRAINTS ALL IMMEDIATE;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'F9b 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UX005' THEN RAISE EXCEPTION 'F9b 失败：期望 UX005，实际 %', SQLSTATE; END IF;
        END;
        SET CONSTRAINTS ALL DEFERRED;
        n_passed := n_passed + 1;
    END IF;

    -- 顶层数据未被任何子块改动：合法结算仍是两条流水。
    IF (SELECT count(*) FROM wallet_ledger_entries WHERE operation_id = op1) <> 2 THEN
        RAISE EXCEPTION '收尾检查失败：合法结算应恰好两条流水';
    END IF;

    RAISE NOTICE 'ALL TASK LEDGER ASSERTIONS PASSED: role=% mode=% groups=%',
        run_role, (CASE WHEN is_app THEN 'application' ELSE 'maintenance' END), n_passed;
END
$task_test$;

-- 顶层剩余操作都必须通过全部延迟校验；随后整体回滚，不提交任何数据。
SET CONSTRAINTS ALL IMMEDIATE;

ROLLBACK;
