-- =====================================================================
-- database/tests/wallet_ledger_test.sql
--
-- 阶段 1 M2-A：钱包流水账数据库侧测试（只编写；由项目负责人手动执行）。
-- 需要已应用 0006_wallet_ledger 的测试库（usapp_test），绝不对主库 usapp 执行。
-- 脚本在任何夹具写入之前检查 current_database() = 'usapp_test'，不符则 RAISE EXCEPTION
-- （ON_ERROR_STOP 使 psql 以非零码退出；事务中止，不提交任何数据）。
--
-- 运行两次（psql 执行方式与 constraints_test.sql 相同）：
--   1) 以应用角色 usapp_api 运行：验证权限屏障。
--      只追加表上的 UPDATE（lock_token 除外）/ DELETE / TRUNCATE 期望 42501；
--      UPDATE lock_token 通过列级授权、被触发器拒绝，期望 UW001。
--   2) 以维护角色（超级用户）运行：权限不再是屏障，验证触发器本身。
--      UPDATE / DELETE 期望 UW001；单独 TRUNCATE 流水表、以及三表一起 TRUNCATE 期望 UW001
--      （单独 TRUNCATE 账户表或操作表会先因外键引用失败，不能证明触发器，所以不这样测）。
--      0007 应用后，三表 TRUNCATE 额外显式列入引用 wallet_operations 的 task_completions
--      （不用 CASCADE），流水表仍排第一，期望仍是 UW001。
--   其他身份一律拒绝运行。
--
-- 约定：
--   * 整个文件包在 BEGIN ... ROLLBACK 中，不提交任何数据。
--   * 每个“应当失败”的用例包在 BEGIN ... EXCEPTION 子块中（psql 会话内的子事务，
--     可以从错误中恢复；这与 Drogon 事务不同）。子块中的准备数据随子块回滚。
--   * 意外成功、或 SQLSTATE / 约束名不符时，RAISE EXCEPTION；配合 ON_ERROR_STOP，
--     psql 以非零码退出。此时事务已中止、连接关闭，同样不会提交任何数据。
--   * 全部通过时打印 ALL WALLET LEDGER ASSERTIONS PASSED 与运行身份。
--   * 不测试提交时的延迟检查（本脚本从不提交）；该项由 db_gated 的真实提交用例覆盖。
-- =====================================================================

\set ON_ERROR_STOP on

BEGIN;

DO $wallet_test$
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
    op1 uuid; op2 uuid; op3 uuid;
    i integer; bal bigint; amt bigint; tag text := substr(md5(random()::text), 1, 8);
    fp text := md5(random()::text) || md5(random()::text);
    k  text := 'psql-wallet-key-' || substr(md5(random()::text), 1, 8);
BEGIN
    -- ---------- 目标库守卫（必须先于任何夹具 DML） ----------
    -- 只允许在 usapp_test 上运行；按 current_database() 精确比较，不依赖连接参数。
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
    IF to_regclass('public.wallet_ledger_entries') IS NULL THEN
        RAISE EXCEPTION '缺少钱包表：请先在测试库应用 0006_wallet_ledger';
    END IF;

    -- ---------- 顶层夹具（随文件末尾的 ROLLBACK 丢弃） ----------
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ua, 'wallet_psql_a_' || tag || '@example.com', 'wallet_psql_a_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (ub, 'wallet_psql_b_' || tag || '@example.com', 'wallet_psql_b_' || tag || '@example.com');
    INSERT INTO users (id, email_canonical, email_display)
        VALUES (uc, 'wallet_psql_c_' || tag || '@example.com', 'wallet_psql_c_' || tag || '@example.com');
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c1, 'wallet-psql', ua);
    INSERT INTO couples (id, name, created_by_user_id) VALUES (c2, 'wallet-psql-2', uc);
    INSERT INTO couple_members (user_id, couple_id, slot) VALUES (ua, c1, 1), (ub, c1, 2), (uc, c2, 1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ua, 'flower', c1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ua, 'sunshine', c1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'flower', c1);
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (uc, 'flower', c2);
    -- ON CONFLICT DO NOTHING：已存在时不报错、不更新
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ua, 'flower', c1)
        ON CONFLICT (user_id, asset_type) DO NOTHING;

    -- ================================================================
    -- A. 正向链：多资产操作（两条语句）、同账户多条、扣减到零、单条上限
    -- ================================================================
    op1 := gen_random_uuid();
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op1, c1, ua, 'flower', 1, 5, 5);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op1, c1, ua, 'sunshine', 1, 40, 40);
    op2 := gen_random_uuid();
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (op2, c1, ua, 'TASK_REWARD', 'psql_test', op2::text);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op2, c1, ua, 'flower', 2, 3, 8);
    op3 := gen_random_uuid();
    -- 通用链式扣减（不是兑换）：0015 起每个 REWARD_REDEMPTION 操作都必须关联一条兑换记录
    -- （延迟校验 UM003），所以这里使用非任务来源的 TASK_REWARD 类型（0007 只校验 source_type = 'task'）。
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
        VALUES (op3, c1, ua, 'TASK_REWARD', 'psql_test', op3::text);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op3, c1, ua, 'flower', 3, -8, 0);
    IF (SELECT balance_after FROM wallet_ledger_entries
         WHERE user_id = ua AND asset_type = 'flower' ORDER BY seq DESC LIMIT 1) <> 0 THEN
        RAISE EXCEPTION 'A1 失败：扣减后小红花余额应为 0';
    END IF;
    PERFORM 1 FROM wallet_accounts WHERE user_id = ua AND asset_type = 'flower' FOR UPDATE;
    n_passed := n_passed + 1;

    -- ================================================================
    -- B. 链式篡改与起始状态（账户 ub/flower 尚无流水；ua/flower 当前 seq 3、余额 0）
    -- ================================================================
    BEGIN  -- B1 伪造起始余额
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'flower', 1, 1, 1000);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B1 失败：伪造起始余额意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW004' THEN RAISE EXCEPTION 'B1 失败：期望 UW004，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B2 首条 seq 不是 1
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'flower', 2, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW003' THEN RAISE EXCEPTION 'B2 失败：期望 UW003，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B3 跳号
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ua, 'flower', 5, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW003' THEN RAISE EXCEPTION 'B3 失败：期望 UW003，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B4 重复序号
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ua, 'flower', 3, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW003' THEN RAISE EXCEPTION 'B4 失败：期望 UW003，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B5 篡改的中间余额（以 500 而不是上一条的 0 为基数）
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ua, 'flower', 4, 3, 503);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW004' THEN RAISE EXCEPTION 'B5 失败：期望 UW004，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B6 一条语句写入两行（两行各自链式合法）
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ua, 'flower', 4, 1, 1), (op1, c1, ua, 'sunshine', 2, 1, 41);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B6 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW005' THEN RAISE EXCEPTION 'B6 失败：期望 UW005，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- B7 账户不存在
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'sunshine', 1, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'B7 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW002' THEN RAISE EXCEPTION 'B7 失败：期望 UW002，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    -- ================================================================
    -- C. 上限（D2：拒绝而非截断）
    -- ================================================================
    BEGIN  -- C1 小红花单条上限
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'flower', 1, 1000, 1000);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_amount_bound' THEN
                RAISE EXCEPTION 'C1 失败：期望 23514 amount_bound，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C2 余额不得为负
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'REWARD_REDEMPTION', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'flower', 1, -1, -1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_balance_bound' THEN
                RAISE EXCEPTION 'C2 失败：期望 23514 balance_bound，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C3 阳光值余额上限：恰好 1,000,000,000 通过，再加 1 拒绝（只在本脚本中测试）
        INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (ub, 'sunshine', c1);
        bal := 0;
        FOR i IN 1..10000 LOOP
            op1 := gen_random_uuid();
            INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
                VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
            bal := bal + 100000;
            INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
                VALUES (op1, c1, ub, 'sunshine', i, 100000, bal);
        END LOOP;
        IF bal <> 1000000000 THEN RAISE EXCEPTION 'C3 准备失败：余额 %', bal; END IF;
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'sunshine', 10001, 1, 1000000001);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C3 失败：超出阳光值余额上限意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_balance_bound' THEN
                RAISE EXCEPTION 'C3 失败：期望 23514 balance_bound，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- C4 小红花余额上限：恰好 1,000,000 通过，再加 1 拒绝
        bal := 0;
        FOR i IN 1..1002 LOOP
            amt := CASE WHEN i <= 1001 THEN 999 ELSE 1 END;
            op1 := gen_random_uuid();
            INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
                VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
            bal := bal + amt;
            INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
                VALUES (op1, c1, ub, 'flower', i, amt, bal);
        END LOOP;
        IF bal <> 1000000 THEN RAISE EXCEPTION 'C4 准备失败：余额 %', bal; END IF;
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, ub, 'flower', 1003, 1, 1000001);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'C4 失败：超出小红花余额上限意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23514' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_balance_bound' THEN
                RAISE EXCEPTION 'C4 失败：期望 23514 balance_bound，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- ================================================================
    -- D. 结构与成员一致性、延迟完整性、幂等
    -- ================================================================
    BEGIN  -- D1 流水的空间与操作的空间不同
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c2, uc, 'flower', 1, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23503' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_operation_fk' THEN
                RAISE EXCEPTION 'D1 失败：期望 23503 operation_fk，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D2 流水用户不是该空间成员
        op1 := gen_random_uuid();
        INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (op1, c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op1, c1, uc, 'flower', 1, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23503' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_member_fk' THEN
                RAISE EXCEPTION 'D2 失败：期望 23503 member_fk，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D3 操作发起者不是成员
        INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (c1, uc, 'TASK_REWARD', 'psql_test', gen_random_uuid()::text);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23503' OR v_constraint IS DISTINCT FROM 'wallet_operations_actor_member_fk' THEN
                RAISE EXCEPTION 'D3 失败：期望 23503 actor_member_fk，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D4 同一操作同一账户第二条流水
        INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
            VALUES (op3, c1, ua, 'flower', 4, 1, 1);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23505' OR v_constraint IS DISTINCT FROM 'wallet_ledger_entries_operation_account_unique' THEN
                RAISE EXCEPTION 'D4 失败：期望 23505 operation_account_unique，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D5 没有流水的操作：SET CONSTRAINTS ALL IMMEDIATE 时拒绝
        INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (c1, ua, 'TASK_REWARD', 'psql_test', gen_random_uuid()::text);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW006' THEN RAISE EXCEPTION 'D5 失败：期望 UW006，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;
    -- 不依赖子块回滚是否恢复约束模式：显式恢复为延迟检查，后续“先插操作、再插流水”才成立。
    SET CONSTRAINTS ALL DEFERRED;

    -- D6 幂等：同一作用域重复拒绝；不同发起者或不同类型允许
    op1 := gen_random_uuid();
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id,
                                   idempotency_key, request_fingerprint)
        VALUES (op1, c1, ub, 'TASK_REWARD', 'psql_test', op1::text, k, fp);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op1, c1, ub, 'flower', 1, 2, 2);
    op2 := gen_random_uuid();
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id,
                                   idempotency_key, request_fingerprint)
        VALUES (op2, c1, ua, 'TASK_REWARD', 'psql_test', op2::text, k, fp);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op2, c1, ua, 'flower', 4, 1, 1);
    BEGIN
        INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, source_type, source_id,
                                       idempotency_key, request_fingerprint)
            VALUES (c1, ub, 'TASK_REWARD', 'psql_test', gen_random_uuid()::text, k, fp);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D6 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23505' OR v_constraint IS DISTINCT FROM 'wallet_operations_idempotency_unique' THEN
                RAISE EXCEPTION 'D6 失败：期望 23505 idempotency_unique，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- D7 业务来源重复
        INSERT INTO wallet_operations (couple_id, actor_user_id, operation_type, source_type, source_id)
            VALUES (c1, ua, 'TASK_REWARD', 'psql_test', op1::text);
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'D7 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_constraint = CONSTRAINT_NAME;
            IF SQLSTATE <> '23505' OR v_constraint IS DISTINCT FROM 'wallet_operations_source_unique' THEN
                RAISE EXCEPTION 'D7 失败：期望 23505 source_unique，实际 % %', SQLSTATE, v_constraint;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- ================================================================
    -- E. 只追加：应用角色看到权限错误，维护角色看到触发器错误
    -- ================================================================
    BEGIN  -- E1 UPDATE lock_token：两种身份都由触发器拒绝
        UPDATE wallet_accounts SET lock_token = 0 WHERE user_id = ua AND asset_type = 'flower';
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E1 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> 'UW001' THEN RAISE EXCEPTION 'E1 失败：期望 UW001，实际 %', SQLSTATE; END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E2 UPDATE 账户其他列
        UPDATE wallet_accounts SET couple_id = couple_id WHERE user_id = ua AND asset_type = 'flower';
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E2 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E2 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E3 UPDATE 流水
        UPDATE wallet_ledger_entries SET amount = amount WHERE user_id = ua;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E3 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E3 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E4 UPDATE 操作
        UPDATE wallet_operations SET source_id = source_id WHERE actor_user_id = ua;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E4 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E4 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E5 DELETE 流水
        DELETE FROM wallet_ledger_entries WHERE user_id = ua;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E5 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E5 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E6 DELETE 操作
        DELETE FROM wallet_operations WHERE actor_user_id = ua;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E6 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E6 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    BEGIN  -- E7 DELETE 账户
        DELETE FROM wallet_accounts WHERE user_id = uc;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E7 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E7 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    -- 0007 起 wallet_ledger_entries 上有延迟约束触发器：D6 顶层插入的流水留有待触发事件，
    -- 维护角色直接 TRUNCATE 会先报 55006，无法验证只追加触发器的 UW001。
    -- 先让延迟检查立即执行。放在预期失败子块之外：夹具不合法必须直接使整个测试失败。
    -- 应用角色不需要：权限检查（42501）先于待触发事件检查。
    IF NOT is_app THEN
        SET CONSTRAINTS ALL IMMEDIATE;
    END IF;

    BEGIN  -- E8 TRUNCATE 流水
        TRUNCATE wallet_ledger_entries;
        RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E8 失败：意外成功';
    EXCEPTION
        WHEN SQLSTATE 'UWT00' THEN RAISE;
        WHEN OTHERS THEN
            IF SQLSTATE <> (CASE WHEN is_app THEN '42501' ELSE 'UW001' END) THEN
                RAISE EXCEPTION 'E8 失败：身份 % 实际 %', run_role, SQLSTATE;
            END IF;
    END;
    n_passed := n_passed + 1;

    IF is_app THEN
        BEGIN  -- E9a 应用角色：单独 TRUNCATE 账户表
            TRUNCATE wallet_accounts;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E9a 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> '42501' THEN RAISE EXCEPTION 'E9a 失败：期望 42501，实际 %', SQLSTATE; END IF;
        END;
        BEGIN  -- E9b 应用角色：单独 TRUNCATE 操作表
            TRUNCATE wallet_operations;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E9b 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> '42501' THEN RAISE EXCEPTION 'E9b 失败：期望 42501，实际 %', SQLSTATE; END IF;
        END;
    ELSE
        -- 先检查合法夹具的延迟约束，清空 wallet_operations 的待处理触发事件；
        -- 否则 TRUNCATE 会先报 55006，无法验证只追加触发器的 UW001。
        -- 放在预期失败子块之外：夹具检查失败必须直接使整个测试失败。
        SET CONSTRAINTS ALL IMMEDIATE;
        BEGIN  -- E9 维护角色：钱包三表一起 TRUNCATE（外键检查通过后由触发器拒绝）
            -- 0007 起 task_completions 有外键引用 wallet_operations：不把它列入同一条 TRUNCATE，
            -- 外键检查会先失败（0A000），无法证明只追加触发器。存在时显式列在钱包三表之后
            -- （不使用 CASCADE）；wallet_ledger_entries 仍排第一，期望的拒绝仍是 UW001。
            -- 只有执行到的分支才会被规划，所以 0007 未应用时不会引用不存在的表。
            -- 0015 起 reward_redemptions 也引用 wallet_operations：存在时同样显式列在后面。
            IF to_regclass('public.task_completions') IS NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts;
            -- 0045 adds avatar_purchases referencing wallet_operations: listed last, no CASCADE.
            ELSIF to_regclass('public.avatar_purchases') IS NOT NULL THEN
                IF to_regclass('public.room_furniture_purchases') IS NOT NULL THEN
                    TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events, reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads, notification_intents, task_reactions, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, avatar_purchases, admin_redemption_cancellations, room_furniture_purchases;
                ELSE
                    TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events, reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads, notification_intents, task_reactions, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, avatar_purchases, admin_redemption_cancellations;
                END IF;
            -- 0023 adds the push dispatch chain referencing notifications: listed last, no CASCADE.
            ELSIF to_regclass('public.push_deliveries') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events, reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads, notification_intents, task_reactions, push_deliveries, push_attempts, push_attempt_observations, push_attempt_expiries, push_delivery_outcomes, push_subscription_revocations, admin_redemption_cancellations;
            ELSIF to_regclass('public.task_reactions') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events, reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads, notification_intents, task_reactions;
            ELSIF to_regclass('public.notification_intents') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events,
                         reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads,
                         notification_intents;
            ELSIF to_regclass('public.notifications') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events,
                         reward_redemptions, reward_redemption_fulfillments, reward_redemption_events, notifications, notification_reads;
            ELSIF to_regclass('public.reward_redemption_events') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events,
                         reward_redemptions, reward_redemption_fulfillments, reward_redemption_events;
            ELSIF to_regclass('public.reward_redemption_fulfillments') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events,
                         reward_redemptions, reward_redemption_fulfillments;
            ELSIF to_regclass('public.task_progress_events') IS NOT NULL THEN
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions, task_progress_events;
            ELSE
                TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions;
            END IF;
            RAISE EXCEPTION USING ERRCODE = 'UWT00', MESSAGE = 'E9 失败：意外成功';
        EXCEPTION
            WHEN SQLSTATE 'UWT00' THEN RAISE;
            WHEN OTHERS THEN
                IF SQLSTATE <> 'UW001' THEN RAISE EXCEPTION 'E9 失败：期望 UW001，实际 %', SQLSTATE; END IF;
        END;
    END IF;
    n_passed := n_passed + 1;

    -- 顶层剩余数据仍满足全部不变量（未被上面的子块改动）
    IF (SELECT count(*) FROM wallet_ledger_entries WHERE user_id IN (ua, ub, uc)) <> 6 THEN
        RAISE EXCEPTION '收尾检查失败：顶层流水条数应为 6';
    END IF;

    RAISE NOTICE 'ALL WALLET LEDGER ASSERTIONS PASSED: role=% mode=% groups=%',
        run_role, CASE WHEN is_app THEN 'application' ELSE 'maintenance' END, n_passed;
END
$wallet_test$;

-- 顶层剩余操作都必须有流水（触发延迟检查）；随后整体回滚，不提交任何数据。
SET CONSTRAINTS ALL IMMEDIATE;

ROLLBACK;
