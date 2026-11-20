-- M5-A: immutable daily templates and lazily materialized counting periods.
-- No applied migration is changed. No task, progress or wallet data is seeded.
-- UD001 period snapshot; UD002 publication/date; UD003 template mutation path.
BEGIN;

DO $daily_preflight$
BEGIN
    IF to_regclass('public.task_progress_events') IS NULL THEN
        RAISE EXCEPTION '0009_daily_tasks requires 0008_task_counting';
    END IF;
END
$daily_preflight$;

ALTER TABLE tasks
    ADD COLUMN schedule_type text NOT NULL DEFAULT 'one_off',
    ADD COLUMN recurrence_id uuid,
    ADD COLUMN period_date date,
    ADD CONSTRAINT tasks_schedule_valid CHECK (schedule_type IN ('one_off', 'daily')),
    ADD CONSTRAINT tasks_daily_template_valid CHECK (
        schedule_type <> 'daily' OR (recurrence_id IS NULL AND completion_mode = 'count' AND deadline_at IS NULL)
    ),
    ADD CONSTRAINT tasks_period_shape CHECK (
        (recurrence_id IS NULL AND period_date IS NULL) OR
        (recurrence_id IS NOT NULL AND period_date IS NOT NULL AND schedule_type = 'one_off')
    ),
    ADD CONSTRAINT tasks_period_date_valid CHECK (
        period_date IS NULL OR period_date BETWEEN DATE '1970-01-01' AND DATE '9998-12-31'
    ),
    ADD CONSTRAINT tasks_recurrence_fk FOREIGN KEY (recurrence_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT tasks_daily_period_unique UNIQUE (recurrence_id, period_date);

CREATE FUNCTION usapp_daily_period_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $daily_period_guard$
DECLARE
    parent tasks%ROWTYPE;
BEGIN
    IF NEW.recurrence_id IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.recurrence_id::text));
    SELECT * INTO parent FROM tasks WHERE id = NEW.recurrence_id;
    IF NOT FOUND OR parent.schedule_type IS DISTINCT FROM 'daily'
       OR parent.recurrence_id IS NOT NULL
       OR ROW(NEW.couple_id, NEW.creator_id, NEW.assignee_id, NEW.title, NEW.description,
              NEW.reward_flowers, NEW.reward_sunshine, NEW.completion_mode, NEW.target_count, NEW.timezone)
          IS DISTINCT FROM
          ROW(parent.couple_id, parent.creator_id, parent.assignee_id, parent.title, parent.description,
              parent.reward_flowers, parent.reward_sunshine, parent.completion_mode, parent.target_count, parent.timezone)
       OR NEW.deadline_local_date IS DISTINCT FROM NEW.period_date
       OR NEW.deadline_local_time IS NOT NULL
       OR NEW.deadline_at IS DISTINCT FROM
          ((NEW.period_date + 1)::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
        RAISE EXCEPTION USING ERRCODE = 'UD001', MESSAGE = 'USAPP_DAILY_SNAPSHOT';
    END IF;
    IF NEW.period_date < (parent.created_at AT TIME ZONE 'Asia/Shanghai')::date
       OR NEW.period_date > (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date
       OR NEW.created_at < parent.created_at
       OR NEW.created_at < (NEW.period_date::timestamp AT TIME ZONE 'Asia/Shanghai')
       OR NEW.created_at >= NEW.deadline_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UD002', MESSAGE = 'USAPP_DAILY_PERIOD_DATE';
    END IF;
    RETURN NEW;
END
$daily_period_guard$;

CREATE TRIGGER tasks_daily_period_guard BEFORE INSERT ON tasks
    FOR EACH ROW EXECUTE FUNCTION usapp_daily_period_guard();

CREATE FUNCTION usapp_daily_action_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $daily_action_guard$
DECLARE
    t tasks%ROWTYPE;
    decision timestamptz;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND THEN RETURN NEW; END IF;
    IF t.schedule_type = 'daily' THEN
        RAISE EXCEPTION USING ERRCODE = 'UD003', MESSAGE = 'USAPP_DAILY_TEMPLATE_NOT_PERIOD';
    END IF;
    IF t.recurrence_id IS NOT NULL THEN
        IF TG_TABLE_NAME = 'task_progress_events' THEN
            -- Same root->instance lock order as the service, including direct SQL.
            PERFORM pg_advisory_xact_lock(33004::integer, hashtext(t.recurrence_id::text));
            decision := NEW.recorded_at;
        ELSE
            decision := NEW.completed_at;
        END IF;
        IF decision < (t.period_date::timestamp AT TIME ZONE 'Asia/Shanghai')
           OR decision >= t.deadline_at THEN
            RAISE EXCEPTION USING ERRCODE = 'UD002', MESSAGE = 'USAPP_DAILY_ACTION_DATE';
        END IF;
    END IF;
    RETURN NEW;
END
$daily_action_guard$;

CREATE TRIGGER task_progress_events_daily_guard BEFORE INSERT ON task_progress_events
    FOR EACH ROW EXECUTE FUNCTION usapp_daily_action_guard();
CREATE TRIGGER task_completions_daily_guard BEFORE INSERT ON task_completions
    FOR EACH ROW EXECUTE FUNCTION usapp_daily_action_guard();

-- A read never creates a stored period. Today's virtual row and a materialized
-- row have the same public (template id, date) identity and stable sort time.
-- Past untouched days have no stored row and do not appear in history.
CREATE VIEW task_read_model WITH (security_invoker = true) AS
WITH day AS (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d)
SELECT t.*, true AS current_period FROM tasks t
 WHERE t.schedule_type = 'one_off' AND t.recurrence_id IS NULL
UNION ALL
SELECT COALESCE(p.id, r.id), r.couple_id, r.creator_id, r.assignee_id, r.title, r.description,
       r.reward_flowers, r.reward_sunshine, r.timezone,
       day.d, NULL::time, (day.d + 1)::timestamp AT TIME ZONE 'Asia/Shanghai',
       r.create_idempotency_key, r.create_fingerprint,
       GREATEST(r.created_at, day.d::timestamp AT TIME ZONE 'Asia/Shanghai'),
       r.completion_mode, r.target_count, 'daily'::text, r.id, day.d, true
  FROM tasks r CROSS JOIN day
  LEFT JOIN tasks p ON p.recurrence_id = r.id AND p.period_date = day.d
 WHERE r.schedule_type = 'daily' AND r.recurrence_id IS NULL
   AND r.created_at < (day.d + 1)::timestamp AT TIME ZONE 'Asia/Shanghai'
UNION ALL
SELECT p.id, p.couple_id, p.creator_id, p.assignee_id, p.title, p.description,
       p.reward_flowers, p.reward_sunshine, p.timezone,
       p.deadline_local_date, p.deadline_local_time, p.deadline_at,
       p.create_idempotency_key, p.create_fingerprint,
       GREATEST(r.created_at, p.period_date::timestamp AT TIME ZONE 'Asia/Shanghai'),
       p.completion_mode, p.target_count, p.schedule_type, p.recurrence_id, p.period_date, false
  FROM tasks p JOIN tasks r ON r.id = p.recurrence_id CROSS JOIN day
 WHERE p.recurrence_id IS NOT NULL AND p.period_date < day.d;

REVOKE ALL ON task_read_model FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON task_read_model TO usapp_api;

COMMIT;
