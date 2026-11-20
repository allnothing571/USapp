-- M5-C: bounded custom date-range counting tasks (plan D5).
-- A custom task is one ordinary root task (no template, no periods):
-- start_local_date is the inclusive start date, deadline_local_date the inclusive
-- end date, and the existing 0007 rule makes deadline_at the midnight after the end
-- date, Asia/Shanghai. Progress accumulates across the range and settles once
-- through the existing M4 path. 0000-0010 are unchanged. No data is seeded.
-- UC001: progress/completion decision before the range starts.
BEGIN;

DO $custom_preflight$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint
                    WHERE conrelid = 'public.tasks'::regclass
                      AND conname = 'tasks_weekly_template_valid') THEN
        RAISE EXCEPTION '0011_custom_range_tasks requires 0010_weekly_tasks';
    END IF;
END
$custom_preflight$;

ALTER TABLE tasks
    ADD COLUMN start_local_date date,
    DROP CONSTRAINT tasks_schedule_valid,
    ADD CONSTRAINT tasks_schedule_valid CHECK (schedule_type IN ('one_off', 'daily', 'weekly', 'custom')),
    ADD CONSTRAINT tasks_custom_range_valid CHECK (
        (schedule_type = 'custom') = (start_local_date IS NOT NULL)
        AND (schedule_type <> 'custom' OR (
            recurrence_id IS NULL AND completion_mode = 'count'
            AND deadline_local_date IS NOT NULL AND deadline_local_time IS NULL
            AND start_local_date <= deadline_local_date))
    );

-- Before the start date no progress or completion is accepted, including direct SQL.
-- Expiry is already enforced by the 0007/0008 deadline guards.
CREATE FUNCTION usapp_custom_action_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $custom_action_guard$
DECLARE
    t tasks%ROWTYPE;
    decision timestamptz;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.schedule_type IS DISTINCT FROM 'custom' THEN RETURN NEW; END IF;
    IF TG_TABLE_NAME = 'task_progress_events' THEN
        decision := NEW.recorded_at;
    ELSE
        decision := NEW.completed_at;
    END IF;
    IF decision < (t.start_local_date::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
        RAISE EXCEPTION USING ERRCODE = 'UC001', MESSAGE = 'USAPP_CUSTOM_NOT_STARTED';
    END IF;
    RETURN NEW;
END
$custom_action_guard$;

CREATE TRIGGER task_progress_events_custom_guard BEFORE INSERT ON task_progress_events
    FOR EACH ROW EXECUTE FUNCTION usapp_custom_action_guard();
CREATE TRIGGER task_completions_custom_guard BEFORE INSERT ON task_completions
    FOR EACH ROW EXECUTE FUNCTION usapp_custom_action_guard();

-- Same 21 columns and rows as 0010 plus start_local_date appended last. Custom roots
-- are listed like one-off roots; recurring rows carry a NULL start date.
CREATE OR REPLACE VIEW task_read_model WITH (security_invoker = true) AS
WITH day AS (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d),
cur AS (
    SELECT r.id AS root_id, x.span,
           day.d - CASE WHEN x.span = 7 THEN EXTRACT(ISODOW FROM day.d)::integer - 1 ELSE 0 END AS ps
      FROM tasks r CROSS JOIN day
     CROSS JOIN LATERAL (SELECT CASE r.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END AS span) x
     WHERE r.schedule_type IN ('daily', 'weekly') AND r.recurrence_id IS NULL
)
SELECT t.id, t.couple_id, t.creator_id, t.assignee_id, t.title, t.description,
       t.reward_flowers, t.reward_sunshine, t.timezone,
       t.deadline_local_date, t.deadline_local_time, t.deadline_at,
       t.create_idempotency_key, t.create_fingerprint, t.created_at,
       t.completion_mode, t.target_count, t.schedule_type, t.recurrence_id, t.period_date,
       true AS current_period, t.start_local_date
  FROM tasks t
 WHERE t.schedule_type IN ('one_off', 'custom') AND t.recurrence_id IS NULL
UNION ALL
SELECT COALESCE(p.id, r.id), r.couple_id, r.creator_id, r.assignee_id, r.title, r.description,
       r.reward_flowers, r.reward_sunshine, r.timezone,
       c.ps + (c.span - 1), NULL::time, (c.ps + c.span)::timestamp AT TIME ZONE 'Asia/Shanghai',
       r.create_idempotency_key, r.create_fingerprint,
       GREATEST(r.created_at, c.ps::timestamp AT TIME ZONE 'Asia/Shanghai'),
       r.completion_mode, r.target_count, r.schedule_type, r.id, c.ps, true, NULL::date
  FROM cur c JOIN tasks r ON r.id = c.root_id
  LEFT JOIN tasks p ON p.recurrence_id = r.id AND p.period_date = c.ps
 WHERE r.created_at < (c.ps + c.span)::timestamp AT TIME ZONE 'Asia/Shanghai'
UNION ALL
SELECT p.id, p.couple_id, p.creator_id, p.assignee_id, p.title, p.description,
       p.reward_flowers, p.reward_sunshine, p.timezone,
       p.deadline_local_date, p.deadline_local_time, p.deadline_at,
       p.create_idempotency_key, p.create_fingerprint,
       GREATEST(r.created_at, p.period_date::timestamp AT TIME ZONE 'Asia/Shanghai'),
       p.completion_mode, p.target_count, p.schedule_type, p.recurrence_id, p.period_date, false, NULL::date
  FROM tasks p JOIN tasks r ON r.id = p.recurrence_id JOIN cur c ON c.root_id = r.id
 WHERE p.period_date < c.ps;

REVOKE ALL ON task_read_model FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON task_read_model TO usapp_api;

COMMIT;
