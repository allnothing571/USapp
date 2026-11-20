-- =====================================================================
-- 0026_reminder_next_fire: P2-6 R1 review repair (docs/phase2-reminders.md Section 17.8). 0025 stays frozen.
--
-- next_fire_at must not name an occurrence identity that has already fired: the no-refire rule (O3) means
-- generation would never create it. These two read functions now exclude fired identities, using exactly the
-- keys of the 0025 partial UNIQUE indexes. No table, constraint, guard, privilege or generation rule changes.
-- =====================================================================

BEGIN;

DO $next_fire_preflight$
BEGIN
    IF to_regclass('public.reminder_occurrences') IS NULL THEN
        RAISE EXCEPTION '0026_reminder_next_fire requires 0025_reminders';
    END IF;
END
$next_fire_preflight$;

-- The next instant under the current rule, or NULL. Reads only.
CREATE OR REPLACE FUNCTION usapp_reminder_task_next_fire(p_owner uuid, p_task uuid) RETURNS timestamptz
LANGUAGE plpgsql STABLE AS $task_next$
DECLARE
    r reminder_rules%ROWTYPE;
    t tasks%ROWTYPE;
    floor_at timestamptz;
    now_at timestamptz := clock_timestamp();
    today date := (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
    span integer;
    period_start date;
    fire timestamptz;
BEGIN
    SELECT * INTO r FROM reminder_rules x WHERE x.owner_id = p_owner AND x.task_id = p_task ORDER BY x.seq DESC LIMIT 1;
    IF NOT FOUND OR NOT r.enabled THEN RETURN NULL; END IF;
    SELECT * INTO t FROM tasks WHERE id = p_task;
    IF EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = t.id) THEN RETURN NULL; END IF;
    floor_at := GREATEST(usapp_reminder_task_effective_at(p_owner, p_task), t.created_at);
    IF t.schedule_type IN ('one_off', 'custom') THEN
        IF EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) THEN RETURN NULL; END IF;
        -- Identity: owner + task. One reminder per target, whichever rule revision fired it.
        IF EXISTS (SELECT 1 FROM reminder_occurrences o
                    WHERE o.owner_id = p_owner AND o.task_id = p_task AND o.period_date IS NULL) THEN
            RETURN NULL;
        END IF;
        fire := usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, r.at_local_date,
                                            t.deadline_local_date, t.deadline_at);
        RETURN CASE WHEN fire > now_at AND fire >= floor_at THEN fire END;
    END IF;
    span := CASE t.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END;
    period_start := today - CASE WHEN span = 7 THEN EXTRACT(ISODOW FROM today)::integer - 1 ELSE 0 END;
    -- Each instant lies inside its own period, and an occurrence exists only once its instant has passed, so the
    -- next period is never fired: this period or the next always holds the answer.
    FOR i IN 0..1 LOOP
        fire := usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, r.at_local_date,
                                            period_start + (span - 1), (period_start + span)::timestamp AT TIME ZONE 'Asia/Shanghai');
        -- Identity: owner + root + period_date.
        IF fire > now_at AND fire >= floor_at
           AND NOT EXISTS (SELECT 1 FROM tasks p JOIN task_completions c ON c.task_id = p.id
                            WHERE p.recurrence_id = t.id AND p.period_date = period_start)
           AND NOT EXISTS (SELECT 1 FROM reminder_occurrences o
                            WHERE o.owner_id = p_owner AND o.task_id = p_task AND o.period_date = period_start) THEN
            RETURN fire;
        END IF;
        period_start := period_start + span;
    END LOOP;
    RETURN NULL;
END
$task_next$;

CREATE OR REPLACE FUNCTION usapp_reminder_anniversary_next_fire(p_owner uuid, p_anniversary uuid) RETURNS timestamptz
LANGUAGE plpgsql STABLE AS $anniversary_next$
DECLARE
    r reminder_rules%ROWTYPE;
    rev anniversary_revisions%ROWTYPE;
    sched record;
    now_at timestamptz := clock_timestamp();
    today_year integer := EXTRACT(YEAR FROM (clock_timestamp() AT TIME ZONE 'Asia/Shanghai'))::integer;
    offset_days smallint;
    floor_at timestamptz;
    first_k integer;
    target_day date;
    fire timestamptz;
    best timestamptz;
BEGIN
    SELECT * INTO r FROM reminder_rules x WHERE x.owner_id = p_owner AND x.anniversary_id = p_anniversary ORDER BY x.seq DESC LIMIT 1;
    IF NOT FOUND OR NOT r.enabled THEN RETURN NULL; END IF;
    SELECT * INTO sched FROM usapp_anniversary_schedule_effective(p_anniversary);
    SELECT * INTO rev FROM anniversary_revisions x WHERE x.anniversary_id = p_anniversary AND x.version = sched.latest_version;
    first_k := GREATEST(0, today_year - EXTRACT(YEAR FROM rev.event_date)::integer - 1);
    FOREACH offset_days IN ARRAY r.days_before_set LOOP
        floor_at := GREATEST(usapp_reminder_offset_effective_at(p_owner, p_anniversary, offset_days), sched.effective_at);
        FOR k IN first_k..first_k + 3 LOOP
            EXIT WHEN rev.kind = 'countdown' AND k > 0;
            target_day := (rev.event_date + make_interval(years => k))::date;
            fire := ((target_day - offset_days) + r.at_time) AT TIME ZONE 'Asia/Shanghai';
            -- Identity: owner + anniversary + target_date + days_before. A fired target moves this offset on to
            -- a later year; the other offsets are evaluated on their own.
            IF fire > now_at AND fire >= floor_at
               AND NOT EXISTS (SELECT 1 FROM reminder_occurrences o
                                WHERE o.owner_id = p_owner AND o.anniversary_id = p_anniversary
                                  AND o.target_date = target_day AND o.days_before = offset_days) THEN
                IF best IS NULL OR fire < best THEN best := fire; END IF;
                EXIT;
            END IF;
        END LOOP;
    END LOOP;
    RETURN best;
END
$anniversary_next$;

COMMIT;
