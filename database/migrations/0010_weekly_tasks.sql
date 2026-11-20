-- M5-B: immutable weekly counting templates with lazily materialized weeks.
-- A week is Monday 00:00 (inclusive) to the next Monday 00:00 (exclusive),
-- Asia/Shanghai (plan D4). A stored week uses period_date = its Monday,
-- deadline_local_date = its Sunday and deadline_at = the next Monday 00:00.
-- 0000-0009 are unchanged. The two 0009 trigger functions are replaced in place
-- (same names and triggers): daily rows keep exactly their 0009 rules, weekly rows
-- get the same snapshot/date/template protections. task_read_model keeps its
-- columns, security_invoker option and SELECT-only grant. No new table, column,
-- grant or seeded data. UD001 snapshot; UD002 date; UD003 template mutation path.
BEGIN;

DO $weekly_preflight$
BEGIN
    IF to_regclass('public.task_read_model') IS NULL THEN
        RAISE EXCEPTION '0010_weekly_tasks requires 0009_daily_tasks';
    END IF;
END
$weekly_preflight$;

ALTER TABLE tasks
    DROP CONSTRAINT tasks_schedule_valid,
    ADD CONSTRAINT tasks_schedule_valid CHECK (schedule_type IN ('one_off', 'daily', 'weekly')),
    ADD CONSTRAINT tasks_weekly_template_valid CHECK (
        schedule_type <> 'weekly' OR (recurrence_id IS NULL AND completion_mode = 'count' AND deadline_at IS NULL)
    );

CREATE OR REPLACE FUNCTION usapp_daily_period_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $daily_period_guard$
DECLARE
    parent tasks%ROWTYPE;
    span integer;
    first_period date;
    last_period date;
BEGIN
    IF NEW.recurrence_id IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.recurrence_id::text));
    SELECT * INTO parent FROM tasks WHERE id = NEW.recurrence_id;
    span := CASE parent.schedule_type WHEN 'daily' THEN 1 WHEN 'weekly' THEN 7 END;
    IF parent.id IS NULL OR span IS NULL
       OR parent.recurrence_id IS NOT NULL
       OR ROW(NEW.couple_id, NEW.creator_id, NEW.assignee_id, NEW.title, NEW.description,
              NEW.reward_flowers, NEW.reward_sunshine, NEW.completion_mode, NEW.target_count, NEW.timezone)
          IS DISTINCT FROM
          ROW(parent.couple_id, parent.creator_id, parent.assignee_id, parent.title, parent.description,
              parent.reward_flowers, parent.reward_sunshine, parent.completion_mode, parent.target_count, parent.timezone)
       OR (span = 7 AND EXTRACT(ISODOW FROM NEW.period_date) <> 1)
       OR NEW.deadline_local_date IS DISTINCT FROM NEW.period_date + (span - 1)
       OR NEW.deadline_local_time IS NOT NULL
       OR NEW.deadline_at IS DISTINCT FROM
          ((NEW.period_date + span)::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
        RAISE EXCEPTION USING ERRCODE = 'UD001', MESSAGE = 'USAPP_DAILY_SNAPSHOT';
    END IF;
    first_period := (parent.created_at AT TIME ZONE 'Asia/Shanghai')::date;
    last_period := (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
    IF span = 7 THEN
        first_period := first_period - (EXTRACT(ISODOW FROM first_period)::integer - 1);
        last_period := last_period - (EXTRACT(ISODOW FROM last_period)::integer - 1);
    END IF;
    IF NEW.period_date < first_period
       OR NEW.period_date > last_period
       OR NEW.created_at < parent.created_at
       OR NEW.created_at < (NEW.period_date::timestamp AT TIME ZONE 'Asia/Shanghai')
       OR NEW.created_at >= NEW.deadline_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UD002', MESSAGE = 'USAPP_DAILY_PERIOD_DATE';
    END IF;
    RETURN NEW;
END
$daily_period_guard$;

-- Any recurring template (daily or weekly) is never a progress/completion target.
CREATE OR REPLACE FUNCTION usapp_daily_action_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $daily_action_guard$
DECLARE
    t tasks%ROWTYPE;
    decision timestamptz;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND THEN RETURN NEW; END IF;
    IF t.schedule_type <> 'one_off' THEN
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

-- Same columns and order as 0009. Each recurring root projects its current
-- period (today, or this week's Monday) without writing; stored earlier periods
-- appear as history. Daily rows are identical to 0009.
CREATE OR REPLACE VIEW task_read_model WITH (security_invoker = true) AS
WITH day AS (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d),
cur AS (
    SELECT r.id AS root_id, x.span,
           day.d - CASE WHEN x.span = 7 THEN EXTRACT(ISODOW FROM day.d)::integer - 1 ELSE 0 END AS ps
      FROM tasks r CROSS JOIN day
     CROSS JOIN LATERAL (SELECT CASE r.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END AS span) x
     WHERE r.schedule_type IN ('daily', 'weekly') AND r.recurrence_id IS NULL
)
SELECT t.*, true AS current_period FROM tasks t
 WHERE t.schedule_type = 'one_off' AND t.recurrence_id IS NULL
UNION ALL
SELECT COALESCE(p.id, r.id), r.couple_id, r.creator_id, r.assignee_id, r.title, r.description,
       r.reward_flowers, r.reward_sunshine, r.timezone,
       c.ps + (c.span - 1), NULL::time, (c.ps + c.span)::timestamp AT TIME ZONE 'Asia/Shanghai',
       r.create_idempotency_key, r.create_fingerprint,
       GREATEST(r.created_at, c.ps::timestamp AT TIME ZONE 'Asia/Shanghai'),
       r.completion_mode, r.target_count, r.schedule_type, r.id, c.ps, true
  FROM cur c JOIN tasks r ON r.id = c.root_id
  LEFT JOIN tasks p ON p.recurrence_id = r.id AND p.period_date = c.ps
 WHERE r.created_at < (c.ps + c.span)::timestamp AT TIME ZONE 'Asia/Shanghai'
UNION ALL
SELECT p.id, p.couple_id, p.creator_id, p.assignee_id, p.title, p.description,
       p.reward_flowers, p.reward_sunshine, p.timezone,
       p.deadline_local_date, p.deadline_local_time, p.deadline_at,
       p.create_idempotency_key, p.create_fingerprint,
       GREATEST(r.created_at, p.period_date::timestamp AT TIME ZONE 'Asia/Shanghai'),
       p.completion_mode, p.target_count, p.schedule_type, p.recurrence_id, p.period_date, false
  FROM tasks p JOIN tasks r ON r.id = p.recurrence_id JOIN cur c ON c.root_id = r.id
 WHERE p.period_date < c.ps;

REVOKE ALL ON task_read_model FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON task_read_model TO usapp_api;

COMMIT;
