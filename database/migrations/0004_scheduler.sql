-- =====================================================================
-- 0004_scheduler.sql
-- 进程内调度基础（阶段 0 只做基础，不做任何任务结算）。
--
-- 设计约束：
--   * 跨实例互斥由 pg_try_advisory_xact_lock 提供，不引入 Redis、不引入额外调度服务。
--   * 每次 tick 使用固定、跨实例一致的键（由 SCHEDULER_KEY_NAMESPACE + job_key 派生）。
--   * 未获得锁立即跳过，不写任何行、不做任何等待。
--   * scheduler_ticks 的写入必须与 advisory lock 处于同一连接、同一事务：
--     这正是“调度数据库操作不脱离持锁事务”的可验证证据。
--   * 网络操作（邮件、推送等）不得占用持锁事务，必须在提交后执行。
-- =====================================================================

BEGIN;

CREATE TABLE scheduler_ticks (
    id          bigserial   PRIMARY KEY,
    job_key     text        NOT NULL,
    lock_key    bigint      NOT NULL,
    instance_id text        NOT NULL,
    outcome     text        NOT NULL,
    detail      jsonb       NOT NULL DEFAULT '{}'::jsonb,
    started_at  timestamptz NOT NULL DEFAULT now(),
    finished_at timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT scheduler_ticks_outcome_valid
        CHECK (outcome IN ('completed', 'failed')),
    CONSTRAINT scheduler_ticks_job_key_not_blank CHECK (length(btrim(job_key)) > 0),
    CONSTRAINT scheduler_ticks_instance_id_not_blank CHECK (length(btrim(instance_id)) > 0),
    CONSTRAINT scheduler_ticks_lock_key_nonzero CHECK (lock_key <> 0)
);

CREATE INDEX scheduler_ticks_job_started_idx
    ON scheduler_ticks (job_key, started_at DESC);

COMMENT ON TABLE scheduler_ticks IS
    '调度 tick 的持锁事务内记录。只在真正持有 advisory lock 的事务中写入；'
    '因锁竞争被跳过的 tick 不产生任何行（只写进程日志），'
    '因此本表可以证明“调度数据库操作与锁同事务、同连接”。';
COMMENT ON COLUMN scheduler_ticks.lock_key IS
    '由命名空间与任务 key 派生的 64 位有符号键；跨实例必须完全一致。'
    '改动派生算法等同于更换锁，会导致双实例同时执行，必须配套迁移。';

-- 阶段 0 只有一个“调度心跳”占位任务，用于验证锁与事务边界。
-- 它不做任何业务结算。后续阶段新增的任务周期结算会使用新的 job_key 与新的锁键。
INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome, detail)
VALUES ('__bootstrap__', -1, 'migration', 'completed',
        '{"note":"阶段 0 调度基础占位记录，可在首次部署后删除"}'::jsonb);

COMMIT;
