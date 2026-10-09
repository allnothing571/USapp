-- =====================================================================
-- 0007_tasks_one_off.sql
-- 阶段 1 M3-A：一次性任务与单次完成的数据库基础（docs/phase1-m3-design.md 修订 3 第 3.1 节）。
--
-- 已批准的产品决定（2026-10-09）：
--   * D22/D22a：整数奖励，小红花 0-999、阳光值 0-100000，且至少一项为正数。
--   * D23：标题去首尾空白后 1-40 个码点、无控制字符与换行；描述最多 500 个码点，
--          允许 LF，不允许 CR（服务端归一化 CRLF）与其他控制字符。
--   * D5：可选的 Asia/Shanghai 本地日期与可选时间；只有日期时截止于次日 0 点（不含）。
--   * D5a：完成的决策时间必须严格早于截止时间，并保存为 task_completions.completed_at。
--   * D26：任务表只授予 usapp_api SELECT, INSERT；PUBLIC / usapp_push 没有权限。
--   * D34：完成记录、任务奖励操作与其流水条目的整体一致性由数据库触发器保证，
--          并在之后任何向任务操作追加流水的事务中重新校验。
--
-- 刻意不做：
--   * 不设“发布时截止时间必须在未来”的 CHECK 或触发器。新发布的时间校验由服务在
--     “先抢占幂等键、再分类”之后进行（设计第 4.2 节），否则重放会被误拒。
--   * 不修改 0000-0006；0006 的授权与触发器保持不变，本迁移只在钱包表上增加
--     仅对 source_type = 'task' 的任务操作生效的延迟校验。
--   * 不写入任何数据。
--
-- 自定义 SQLSTATE（测试据此断言，不依赖本地化的错误文本）：
--   UT001 只追加表上的 UPDATE / DELETE / TRUNCATE
--   UT002 完成记录引用的任务不存在，或任务与完成记录不在同一空间
--   UT003 完成者不是任务的接收者
--   UT004 完成记录的奖励快照与任务不一致
--   UT005 完成记录引用的钱包操作不存在、类型/来源/发起者/空间不符
--   UT006 完成时间不合法（晚于当前时间、早于任务创建、或不早于截止时间）
--   UT007 任务结算整体不一致（缺完成记录、重复、条目集合与快照不符）
-- =====================================================================

BEGIN;

-- ---------------------------------------------------------------- 角色前置检查
DO $task_roles$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_api') THEN
        RAISE EXCEPTION '0007_tasks_one_off: 角色 usapp_api 不存在';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_push') THEN
        RAISE EXCEPTION '0007_tasks_one_off: 角色 usapp_push 不存在';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_roles
                WHERE rolname IN ('usapp_api', 'usapp_push') AND rolsuper) THEN
        RAISE EXCEPTION '0007_tasks_one_off: usapp_api / usapp_push 不得是超级用户';
    END IF;
    IF to_regclass('public.wallet_operations') IS NULL
       OR to_regclass('public.wallet_ledger_entries') IS NULL THEN
        RAISE EXCEPTION '0007_tasks_one_off: 需要先应用 0006_wallet_ledger';
    END IF;
END
$task_roles$;

-- ------------------------------------------------------------------ tasks
-- M3 中任务发布后不可修改（没有编辑路径）。
CREATE TABLE tasks (
    id                     uuid        NOT NULL DEFAULT gen_random_uuid(),
    couple_id              uuid        NOT NULL,
    creator_id             uuid        NOT NULL,
    assignee_id            uuid        NOT NULL,
    title                  text        NOT NULL,
    description            text        NOT NULL DEFAULT '',
    reward_flowers         integer     NOT NULL,
    reward_sunshine        integer     NOT NULL,
    timezone               text        NOT NULL DEFAULT 'Asia/Shanghai',
    deadline_local_date    date,
    deadline_local_time    time,
    deadline_at            timestamptz,
    create_idempotency_key text        NOT NULL,
    create_fingerprint     text        NOT NULL,
    created_at             timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT tasks_pkey PRIMARY KEY (id),
    CONSTRAINT tasks_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT tasks_create_key_unique UNIQUE (creator_id, create_idempotency_key),
    CONSTRAINT tasks_creator_member_fk
        FOREIGN KEY (couple_id, creator_id) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT tasks_assignee_member_fk
        FOREIGN KEY (couple_id, assignee_id) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT tasks_creator_not_assignee CHECK (assignee_id <> creator_id),
    CONSTRAINT tasks_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 40
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT tasks_description_shape CHECK (
        char_length(description) <= 500
        AND description !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT tasks_reward_flowers_range CHECK (reward_flowers BETWEEN 0 AND 999),
    CONSTRAINT tasks_reward_sunshine_range CHECK (reward_sunshine BETWEEN 0 AND 100000),
    CONSTRAINT tasks_reward_positive CHECK (reward_flowers > 0 OR reward_sunshine > 0),
    CONSTRAINT tasks_timezone_supported CHECK (timezone = 'Asia/Shanghai'),
    CONSTRAINT tasks_deadline_time_requires_date
        CHECK (deadline_local_time IS NULL OR deadline_local_date IS NOT NULL),
    CONSTRAINT tasks_deadline_consistency
        CHECK ((deadline_at IS NULL) = (deadline_local_date IS NULL)),
    -- deadline_at 必须恰好等于本地日期/时间在任务时区下的时刻；只有日期时为次日 0 点。
    CONSTRAINT tasks_deadline_at_matches_local CHECK (
        deadline_at IS NULL
        OR deadline_at = (
            CASE WHEN deadline_local_time IS NULL
                 THEN (deadline_local_date + 1)::timestamp
                 ELSE deadline_local_date + deadline_local_time
            END
        ) AT TIME ZONE timezone
    ),
    CONSTRAINT tasks_create_key_shape
        CHECK (char_length(create_idempotency_key) BETWEEN 16 AND 128),
    CONSTRAINT tasks_create_fingerprint_shape CHECK (create_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE INDEX tasks_couple_deadline_idx ON tasks (couple_id, deadline_at, created_at, id);

COMMENT ON TABLE tasks IS
    '一次性任务（M3）。发布后不可修改；接收者由服务端取为同一空间的另一成员。'
    '状态不存储：有完成记录为已完成；否则 deadline_at <= now() 为已过期；否则为进行中。';
COMMENT ON COLUMN tasks.deadline_at IS
    '由本地日期/时间按 timezone 计算的截止时刻（不含）。发布时不在数据库中要求其晚于当前时间。';

-- ------------------------------------------------------- task_completions
CREATE TABLE task_completions (
    task_id         uuid        NOT NULL,
    couple_id       uuid        NOT NULL,
    completed_by    uuid        NOT NULL,
    operation_id    uuid        NOT NULL,
    reward_flowers  integer     NOT NULL,
    reward_sunshine integer     NOT NULL,
    completed_at    timestamptz NOT NULL,

    CONSTRAINT task_completions_pkey PRIMARY KEY (task_id),
    CONSTRAINT task_completions_operation_unique UNIQUE (operation_id),
    CONSTRAINT task_completions_task_fk
        FOREIGN KEY (task_id, couple_id) REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_completions_member_fk
        FOREIGN KEY (couple_id, completed_by) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT task_completions_operation_fk
        FOREIGN KEY (operation_id, couple_id) REFERENCES wallet_operations (id, couple_id)
        ON DELETE RESTRICT,
    CONSTRAINT task_completions_reward_flowers_range CHECK (reward_flowers BETWEEN 0 AND 999),
    CONSTRAINT task_completions_reward_sunshine_range CHECK (reward_sunshine BETWEEN 0 AND 100000),
    CONSTRAINT task_completions_reward_positive CHECK (reward_flowers > 0 OR reward_sunshine > 0)
);

CREATE INDEX task_completions_couple_completed_idx
    ON task_completions (couple_id, completed_at DESC, task_id);

COMMENT ON TABLE task_completions IS
    '任务完成记录（只追加，每个任务至多一条）。completed_at 是服务的决策时间（D5a），'
    'reward_* 是实际入账金额的快照；operation_id 指向唯一对应的 TASK_REWARD 钱包操作。';

-- ------------------------------------------------------------- 只追加保护
CREATE FUNCTION usapp_task_reject_mutation()
RETURNS trigger
LANGUAGE plpgsql
AS $task_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UT001',
        MESSAGE = 'USAPP_TASK_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$task_reject$;

CREATE TRIGGER tasks_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON tasks
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();
CREATE TRIGGER task_completions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON task_completions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();

-- ------------------------------------------------ 完成记录的立即校验（D34 第 1 项）
-- 关键列为 NULL 时放行，交给 NOT NULL 约束报 23502；其余检查一律 NULL 安全（IS DISTINCT FROM）。
CREATE FUNCTION usapp_task_completion_guard()
RETURNS trigger
LANGUAGE plpgsql
AS $task_guard$
DECLARE
    t tasks%ROWTYPE;
    o wallet_operations%ROWTYPE;
BEGIN
    IF NEW.task_id IS NULL OR NEW.couple_id IS NULL OR NEW.completed_by IS NULL
       OR NEW.operation_id IS NULL OR NEW.reward_flowers IS NULL
       OR NEW.reward_sunshine IS NULL OR NEW.completed_at IS NULL THEN
        RETURN NEW;
    END IF;

    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UT002', MESSAGE = 'USAPP_TASK_COMPLETION_TASK';
    END IF;
    IF NEW.completed_by IS DISTINCT FROM t.assignee_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UT003', MESSAGE = 'USAPP_TASK_COMPLETION_ASSIGNEE';
    END IF;
    IF NEW.reward_flowers IS DISTINCT FROM t.reward_flowers
       OR NEW.reward_sunshine IS DISTINCT FROM t.reward_sunshine THEN
        RAISE EXCEPTION USING ERRCODE = 'UT004', MESSAGE = 'USAPP_TASK_COMPLETION_SNAPSHOT';
    END IF;

    SELECT * INTO o FROM wallet_operations WHERE id = NEW.operation_id;
    IF NOT FOUND
       OR o.operation_type IS DISTINCT FROM 'TASK_REWARD'
       OR o.source_type IS DISTINCT FROM 'task'
       OR o.source_id IS DISTINCT FROM NEW.task_id::text
       OR o.actor_user_id IS DISTINCT FROM NEW.completed_by
       OR o.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UT005', MESSAGE = 'USAPP_TASK_COMPLETION_OPERATION';
    END IF;

    IF NEW.completed_at > clock_timestamp()
       OR NEW.completed_at < t.created_at
       OR (t.deadline_at IS NOT NULL AND NEW.completed_at >= t.deadline_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UT006', MESSAGE = 'USAPP_TASK_COMPLETION_TIME';
    END IF;
    RETURN NEW;
END
$task_guard$;

CREATE TRIGGER task_completions_guard
    BEFORE INSERT ON task_completions
    FOR EACH ROW EXECUTE FUNCTION usapp_task_completion_guard();

-- ------------------------------------------- 共享结算校验器（D34 第 2 项）
-- 一个任务奖励操作合法，当且仅当：
--   * 操作存在，类型为 TASK_REWARD，来源为 'task'；
--   * 恰好一条完成记录引用它，且该完成记录的 task_id 与操作来源一致；
--   * 它的流水恰好是“每个正奖励资产一条”，全部记给完成者，金额等于快照，没有其他流水。
-- 所有计数用 count(*)（不会为 NULL），金额合计显式 COALESCE；缺行一律报错，不会静默通过。
CREATE FUNCTION usapp_task_settlement_validate(p_operation_id uuid)
RETURNS void
LANGUAGE plpgsql
AS $task_validate$
DECLARE
    o            wallet_operations%ROWTYPE;
    c            task_completions%ROWTYPE;
    n_completion bigint;
    n_total      bigint;
    n_flower     bigint;
    n_sunshine   bigint;
    n_wrong_user bigint;
    sum_flower   bigint;
    sum_sunshine bigint;
BEGIN
    SELECT * INTO o FROM wallet_operations WHERE id = p_operation_id;
    IF NOT FOUND
       OR o.operation_type IS DISTINCT FROM 'TASK_REWARD'
       OR o.source_type IS DISTINCT FROM 'task' THEN
        RAISE EXCEPTION USING ERRCODE = 'UT007', MESSAGE = 'USAPP_TASK_SETTLEMENT_OPERATION';
    END IF;

    SELECT count(*) INTO n_completion FROM task_completions WHERE operation_id = p_operation_id;
    IF n_completion <> 1 THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UT007',
            MESSAGE = 'USAPP_TASK_SETTLEMENT_COMPLETION',
            DETAIL  = 'completions referencing the operation: ' || n_completion;
    END IF;
    SELECT * INTO c FROM task_completions WHERE operation_id = p_operation_id;
    IF c.task_id::text IS DISTINCT FROM o.source_id
       OR c.couple_id IS DISTINCT FROM o.couple_id
       OR c.completed_by IS DISTINCT FROM o.actor_user_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UT007', MESSAGE = 'USAPP_TASK_SETTLEMENT_LINK';
    END IF;

    SELECT count(*),
           count(*) FILTER (WHERE e.asset_type = 'flower'),
           count(*) FILTER (WHERE e.asset_type = 'sunshine'),
           count(*) FILTER (WHERE e.user_id IS DISTINCT FROM c.completed_by),
           COALESCE(sum(e.amount) FILTER (WHERE e.asset_type = 'flower'), 0),
           COALESCE(sum(e.amount) FILTER (WHERE e.asset_type = 'sunshine'), 0)
      INTO n_total, n_flower, n_sunshine, n_wrong_user, sum_flower, sum_sunshine
      FROM wallet_ledger_entries e
     WHERE e.operation_id = p_operation_id;

    IF n_wrong_user <> 0
       OR n_flower <> (CASE WHEN c.reward_flowers > 0 THEN 1 ELSE 0 END)
       OR n_sunshine <> (CASE WHEN c.reward_sunshine > 0 THEN 1 ELSE 0 END)
       OR n_total <> n_flower + n_sunshine
       OR sum_flower <> c.reward_flowers
       OR sum_sunshine <> c.reward_sunshine THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UT007',
            MESSAGE = 'USAPP_TASK_SETTLEMENT_ENTRIES',
            DETAIL  = 'entries=' || n_total || ' flower=' || n_flower || '/' || sum_flower
                      || ' sunshine=' || n_sunshine || '/' || sum_sunshine
                      || ' other_user=' || n_wrong_user;
    END IF;
END
$task_validate$;

-- 三个延迟触发器共用同一校验器。延迟是必须的：首次结算先插入完成记录，再插入流水。
CREATE FUNCTION usapp_task_completion_settlement_check()
RETURNS trigger
LANGUAGE plpgsql
AS $task_completion_check$
BEGIN
    PERFORM usapp_task_settlement_validate(NEW.operation_id);
    RETURN NULL;
END
$task_completion_check$;

CREATE CONSTRAINT TRIGGER task_completions_settlement_valid
    AFTER INSERT ON task_completions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_completion_settlement_check();

CREATE FUNCTION usapp_task_operation_settlement_check()
RETURNS trigger
LANGUAGE plpgsql
AS $task_operation_check$
BEGIN
    PERFORM usapp_task_settlement_validate(NEW.id);
    RETURN NULL;
END
$task_operation_check$;

-- 只对任务来源的操作生效；钱包测试使用的 wallet_test / psql_test 等来源不受影响。
CREATE CONSTRAINT TRIGGER wallet_operations_task_settlement_valid
    AFTER INSERT ON wallet_operations
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW
    WHEN (NEW.source_type = 'task')
    EXECUTE FUNCTION usapp_task_operation_settlement_check();

-- 任何新流水：只有其操作来源为 'task' 时才重新校验整个结算（包括追加到早已提交的结算）。
-- WHEN 子句不能查询其他表，所以在函数内判断来源。
CREATE FUNCTION usapp_task_entry_settlement_check()
RETURNS trigger
LANGUAGE plpgsql
AS $task_entry_check$
DECLARE
    v_source_type text;
BEGIN
    SELECT o.source_type INTO v_source_type FROM wallet_operations o WHERE o.id = NEW.operation_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UT007', MESSAGE = 'USAPP_TASK_SETTLEMENT_OPERATION';
    END IF;
    IF v_source_type IS DISTINCT FROM 'task' THEN
        RETURN NULL;
    END IF;
    PERFORM usapp_task_settlement_validate(NEW.operation_id);
    RETURN NULL;
END
$task_entry_check$;

CREATE CONSTRAINT TRIGGER wallet_ledger_entries_task_settlement_valid
    AFTER INSERT ON wallet_ledger_entries
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_entry_settlement_check();

-- ------------------------------------------------------------- 权限收窄
-- 默认权限会给新表授予 usapp_api 全部 DML，这里显式收回。init-roles.sql 第 8 节在
-- ALL TABLES 授权之后重复同样的收窄，保证重新执行 init-roles 不会放宽。
REVOKE ALL ON tasks, task_completions FROM PUBLIC;
REVOKE ALL ON tasks, task_completions FROM usapp_api;
REVOKE ALL ON tasks, task_completions FROM usapp_push;
GRANT SELECT, INSERT ON tasks, task_completions TO usapp_api;

COMMIT;
