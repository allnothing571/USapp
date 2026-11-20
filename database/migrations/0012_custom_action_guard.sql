-- M5-C repair: the 0010 usapp_daily_action_guard rejects progress/completion on
-- every non-one_off task (UD003), which also blocks custom range roots from 0011.
-- The function is replaced in place (same name, triggers and errors); the only
-- change is that custom roots pass through to the 0011 start guard and the
-- existing 0007/0008 deadline guards. Daily/weekly templates are still rejected
-- and period instances keep their 0010 date rules. 0000-0011 are unchanged; no new
-- table, column, grant or seeded data.
BEGIN;

DO $custom_guard_preflight$
BEGIN
    IF to_regprocedure('public.usapp_custom_action_guard()') IS NULL THEN
        RAISE EXCEPTION '0012_custom_action_guard requires 0011_custom_range_tasks';
    END IF;
END
$custom_guard_preflight$;

CREATE OR REPLACE FUNCTION usapp_daily_action_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $daily_action_guard$
DECLARE
    t tasks%ROWTYPE;
    decision timestamptz;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND THEN RETURN NEW; END IF;
    IF t.schedule_type NOT IN ('one_off', 'custom') THEN
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

COMMIT;
