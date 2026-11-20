-- =====================================================================
-- 0025_reminders: Phase 2 P2-6 R1 reminder rules and occurrence storage
-- (docs/phase2-reminders.md, owner defaults O1-O15). No generator exists in R1: nothing here runs on a
-- schedule, and reminder_occurrences stays empty until a later, separately approved batch.
--
-- reminder_rules        append-only per (owner, target); the highest seq is current
-- reminder_occurrences  append-only; one row per fired reminder, identity independent of the rule revision
-- notifications         gains the closed reminder family: reminder_occurrence_id, and a NULL actor_id for
--                       reminder.task / reminder.anniversary only. Every other family keeps a non-null actor.
--
-- Locks: 33004 (root task) or 33010 (anniversary), then 33017 per (owner, target).
-- SQLSTATE: UV001 append-only; UV002 rule guard; UV003 occurrence guard; UV004 deferred occurrence link.
-- The 0020 notification guard is replaced with its body unchanged plus one reminder branch.
-- =====================================================================

BEGIN;

DO $reminder_preflight$
BEGIN
    IF to_regclass('public.push_delivery_outcomes') IS NULL
       OR NOT EXISTS (SELECT 1 FROM information_schema.columns
                       WHERE table_name = 'push_attempts' AND column_name = 'control_revision') THEN
        RAISE EXCEPTION '0025_reminders requires 0024_push_dispatch_repairs';
    END IF;
END
$reminder_preflight$;

-- ---------------------------------------------------------------- pure helpers
CREATE FUNCTION usapp_reminder_offsets_valid(offsets smallint[]) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $offsets_valid$
    -- COALESCE: an empty array has NULL dimensions, and a NULL CHECK would pass.
    SELECT COALESCE(array_ndims(offsets) = 1 AND array_length(offsets, 1) BETWEEN 1 AND 3
       AND offsets <@ ARRAY[0, 1, 3, 7, 14, 30]::smallint[]
       -- Strictly ascending, so one set has exactly one spelling.
       AND offsets = (SELECT array_agg(DISTINCT x ORDER BY x) FROM unnest(offsets) x), false)
$offsets_valid$;

-- The one definition of a task reminder's instant. Asia/Shanghai, as every task guard computes.
CREATE FUNCTION usapp_reminder_task_fire_at(p_mode text, p_before integer, p_days integer, p_time time,
                                            p_date date, p_last date, p_deadline timestamptz)
RETURNS timestamptz LANGUAGE sql STABLE AS $task_fire$
    SELECT CASE p_mode
             WHEN 'before_deadline' THEN p_deadline - make_interval(mins => p_before)
             WHEN 'local_time' THEN ((p_last - p_days) + p_time) AT TIME ZONE 'Asia/Shanghai'
             WHEN 'absolute' THEN (p_date + p_time) AT TIME ZONE 'Asia/Shanghai'
           END
$task_fire$;

-- ---------------------------------------------------------------- rules
CREATE TABLE reminder_rules (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    owner_id uuid NOT NULL,
    seq integer NOT NULL,
    task_id uuid,
    anniversary_id uuid,
    enabled boolean NOT NULL,
    mode text,
    before_minutes integer,
    days_before integer,
    at_time time,
    at_local_date date,
    days_before_set smallint[],
    origin text NOT NULL DEFAULT 'user',
    idempotency_key text,
    request_fingerprint text,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reminder_rules_pkey PRIMARY KEY (id),
    CONSTRAINT reminder_rules_identity_unique UNIQUE (id, couple_id, owner_id),
    CONSTRAINT reminder_rules_key_unique UNIQUE (owner_id, idempotency_key),
    CONSTRAINT reminder_rules_owner_fk FOREIGN KEY (couple_id, owner_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_rules_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_rules_anniversary_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_rules_one_target CHECK (num_nonnulls(task_id, anniversary_id) = 1),
    CONSTRAINT reminder_rules_seq_valid CHECK (seq >= 1),
    CONSTRAINT reminder_rules_origin_valid CHECK (origin IN ('user', 'successor')),
    -- A disabled rule carries no schedule; an enabled one exactly its mode's fields.
    CONSTRAINT reminder_rules_shape CHECK (
        (NOT enabled AND num_nonnulls(mode, before_minutes, days_before, at_time, at_local_date, days_before_set) = 0)
        OR (enabled AND task_id IS NOT NULL AND days_before_set IS NULL AND (
            (mode = 'before_deadline' AND before_minutes IS NOT NULL
             AND num_nonnulls(days_before, at_time, at_local_date) = 0)
            OR (mode = 'local_time' AND days_before IS NOT NULL AND at_time IS NOT NULL
                AND num_nonnulls(before_minutes, at_local_date) = 0)
            OR (mode = 'absolute' AND at_local_date IS NOT NULL AND at_time IS NOT NULL
                AND num_nonnulls(before_minutes, days_before) = 0)))
        OR (enabled AND anniversary_id IS NOT NULL AND mode = 'anniversary_offsets' AND days_before_set IS NOT NULL
            AND at_time IS NOT NULL AND num_nonnulls(before_minutes, days_before, at_local_date) = 0)
    ),
    -- The widest values of any schedule; the guard applies the schedule-specific bound.
    CONSTRAINT reminder_rules_before_range CHECK (before_minutes IS NULL OR before_minutes BETWEEN 1 AND 10080),
    CONSTRAINT reminder_rules_days_range CHECK (days_before IS NULL OR days_before BETWEEN 0 AND 7),
    CONSTRAINT reminder_rules_time_minute CHECK (at_time IS NULL OR EXTRACT(SECOND FROM at_time) = 0),
    CONSTRAINT reminder_rules_date_range CHECK (
        at_local_date IS NULL OR at_local_date BETWEEN DATE '1970-01-01' AND DATE '9998-12-31'),
    CONSTRAINT reminder_rules_offsets_valid CHECK (
        days_before_set IS NULL OR usapp_reminder_offsets_valid(days_before_set)),
    CONSTRAINT reminder_rules_key_presence CHECK (
        (origin = 'user') = (idempotency_key IS NOT NULL) AND (idempotency_key IS NULL) = (request_fingerprint IS NULL)),
    CONSTRAINT reminder_rules_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT reminder_rules_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE UNIQUE INDEX reminder_rules_task_seq_unique ON reminder_rules (owner_id, task_id, seq) WHERE task_id IS NOT NULL;
CREATE UNIQUE INDEX reminder_rules_anniversary_seq_unique ON reminder_rules (owner_id, anniversary_id, seq)
    WHERE anniversary_id IS NOT NULL;
-- The list's immutable page order: each target's first revision.
CREATE INDEX reminder_rules_first_idx ON reminder_rules (owner_id, created_at DESC, id DESC) WHERE seq = 1;

-- NULL: acceptable. Otherwise one of: closed, invalid, passed. The single definition for the service's error
-- and the guard. p_at is the write's own time.
CREATE FUNCTION usapp_reminder_task_rule_problem(p_task uuid, p_enabled boolean, p_mode text, p_before integer,
                                                 p_days integer, p_time time, p_date date, p_at timestamptz)
RETURNS text LANGUAGE plpgsql VOLATILE AS $task_problem$
DECLARE
    t tasks%ROWTYPE;
    fire timestamptz;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = p_task;
    IF NOT FOUND OR t.recurrence_id IS NOT NULL THEN
        RETURN 'invalid';
    END IF;
    -- A cancelled or superseded root takes no more rule writes, disabling included (design Section 5.1).
    IF EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = t.id) THEN
        RETURN 'closed';
    END IF;
    -- Disabling stays possible on a completed or expired target and when the instant has passed.
    IF NOT p_enabled THEN
        RETURN NULL;
    END IF;
    IF t.schedule_type IN ('one_off', 'custom') AND EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) THEN
        RETURN 'closed';
    END IF;
    IF t.schedule_type = 'one_off' AND t.deadline_at IS NULL THEN
        IF p_mode IS DISTINCT FROM 'absolute' THEN RETURN 'invalid'; END IF;
    ELSIF p_mode IS NOT DISTINCT FROM 'absolute' OR p_mode IS NULL THEN
        RETURN 'invalid';
    END IF;
    -- Schedule bounds keep a fire instant inside its own period (design Section 5.2).
    IF t.schedule_type = 'daily' THEN
        IF (p_mode = 'before_deadline' AND p_before NOT BETWEEN 1 AND 1440) OR (p_mode = 'local_time' AND p_days <> 0) THEN
            RETURN 'invalid';
        END IF;
        RETURN NULL;
    END IF;
    IF t.schedule_type = 'weekly' THEN
        IF (p_mode = 'before_deadline' AND p_before NOT BETWEEN 1 AND 10080)
           OR (p_mode = 'local_time' AND p_days NOT BETWEEN 0 AND 6) THEN
            RETURN 'invalid';
        END IF;
        RETURN NULL;
    END IF;
    IF t.schedule_type = 'custom' AND p_mode = 'local_time' AND p_days NOT BETWEEN 0 AND 6 THEN
        RETURN 'invalid';
    END IF;
    fire := usapp_reminder_task_fire_at(p_mode, p_before, p_days, p_time, p_date, t.deadline_local_date, t.deadline_at);
    IF fire IS NULL OR (t.deadline_at IS NOT NULL AND fire >= t.deadline_at) THEN
        RETURN 'invalid';
    END IF;
    IF t.schedule_type = 'custom' AND fire < (t.start_local_date::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
        RETURN 'invalid';
    END IF;
    IF fire <= p_at THEN
        RETURN 'passed';
    END IF;
    RETURN NULL;
END
$task_problem$;

-- VOLATILE: statements after the advisory-lock waits see the previous holder's commit.
CREATE FUNCTION usapp_reminder_rule_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $rule_guard$
DECLARE
    t tasks%ROWTYPE;
    a anniversaries%ROWTYPE;
    problem text;
    prev reminder_rules%ROWTYPE;
    has_prev boolean;
    target_created timestamptz;
BEGIN
    IF NEW.couple_id IS NULL OR NEW.owner_id IS NULL OR NEW.seq IS NULL OR NEW.enabled IS NULL
       OR NEW.created_at IS NULL OR num_nonnulls(NEW.task_id, NEW.anniversary_id) <> 1 THEN
        RETURN NEW;
    END IF;
    -- R4 (rule copy on an approved edit) is not implemented; a later migration may allow it.
    IF NEW.origin IS DISTINCT FROM 'user' THEN
        RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_ORIGIN';
    END IF;
    IF NEW.task_id IS NOT NULL THEN
        PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.task_id::text));
        SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
        IF NOT FOUND OR t.couple_id IS DISTINCT FROM NEW.couple_id OR t.recurrence_id IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_TARGET';
        END IF;
        IF t.assignee_id IS DISTINCT FROM NEW.owner_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_OWNER';
        END IF;
        target_created := t.created_at;
        problem := usapp_reminder_task_rule_problem(NEW.task_id, NEW.enabled, NEW.mode, NEW.before_minutes,
                                                    NEW.days_before, NEW.at_time, NEW.at_local_date, NEW.created_at);
        IF problem IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_' || upper(problem);
        END IF;
        PERFORM pg_advisory_xact_lock(33017::integer, hashtext(NEW.owner_id::text || ':' || NEW.task_id::text));
        SELECT * INTO prev FROM reminder_rules
         WHERE owner_id = NEW.owner_id AND task_id = NEW.task_id ORDER BY seq DESC LIMIT 1;
        has_prev := FOUND;
    ELSE
        PERFORM pg_advisory_xact_lock(33010::integer, hashtext(NEW.anniversary_id::text));
        SELECT * INTO a FROM anniversaries WHERE id = NEW.anniversary_id;
        IF NOT FOUND OR a.couple_id IS DISTINCT FROM NEW.couple_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_TARGET';
        END IF;
        target_created := a.created_at;
        PERFORM pg_advisory_xact_lock(33017::integer, hashtext(NEW.owner_id::text || ':' || NEW.anniversary_id::text));
        SELECT * INTO prev FROM reminder_rules
         WHERE owner_id = NEW.owner_id AND anniversary_id = NEW.anniversary_id ORDER BY seq DESC LIMIT 1;
        has_prev := FOUND;
    END IF;
    IF NEW.seq::bigint <> COALESCE(prev.seq, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_SEQ';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < target_created
       OR (has_prev AND NEW.created_at < prev.created_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_TIME';
    END IF;
    RETURN NEW;
END
$rule_guard$;

CREATE TRIGGER reminder_rules_guard BEFORE INSERT ON reminder_rules
    FOR EACH ROW EXECUTE FUNCTION usapp_reminder_rule_guard();

-- ---------------------------------------------------------------- effective time (design Section 6.1)
-- The earliest revision of the latest contiguous run with the current schedule; NULL when disabled. An
-- identical re-save extends the run and a disable breaks it.
CREATE FUNCTION usapp_reminder_task_effective_at(p_owner uuid, p_task uuid) RETURNS timestamptz
LANGUAGE sql STABLE AS $task_effective$
    WITH cur AS (SELECT * FROM reminder_rules r WHERE r.owner_id = p_owner AND r.task_id = p_task
                  ORDER BY r.seq DESC LIMIT 1)
    SELECT (SELECT min(x.created_at) FROM reminder_rules x
             WHERE x.owner_id = p_owner AND x.task_id = p_task
               AND x.seq > COALESCE((SELECT max(b.seq) FROM reminder_rules b
                                      WHERE b.owner_id = p_owner AND b.task_id = p_task
                                        AND (NOT b.enabled
                                             OR ROW(b.mode, b.before_minutes, b.days_before, b.at_time, b.at_local_date)
                                                IS DISTINCT FROM ROW(cur.mode, cur.before_minutes, cur.days_before,
                                                                     cur.at_time, cur.at_local_date))), 0))
      FROM cur WHERE cur.enabled
$task_effective$;

-- Per offset: NULL when disabled or this offset is not in the current set. Changing other offsets leaves it.
CREATE FUNCTION usapp_reminder_offset_effective_at(p_owner uuid, p_anniversary uuid, p_offset integer)
RETURNS timestamptz LANGUAGE sql STABLE AS $offset_effective$
    WITH cur AS (SELECT * FROM reminder_rules r WHERE r.owner_id = p_owner AND r.anniversary_id = p_anniversary
                  ORDER BY r.seq DESC LIMIT 1)
    SELECT (SELECT min(x.created_at) FROM reminder_rules x
             WHERE x.owner_id = p_owner AND x.anniversary_id = p_anniversary
               AND x.seq > COALESCE((SELECT max(b.seq) FROM reminder_rules b
                                      WHERE b.owner_id = p_owner AND b.anniversary_id = p_anniversary
                                        AND (NOT b.enabled OR b.at_time IS DISTINCT FROM cur.at_time
                                             OR NOT (p_offset::smallint = ANY (b.days_before_set)))), 0))
      FROM cur WHERE cur.enabled AND p_offset::smallint = ANY (cur.days_before_set)
$offset_effective$;

-- Title and note are not part of the schedule; (kind, event_date) is.
CREATE FUNCTION usapp_anniversary_schedule_effective(p_anniversary uuid, OUT effective_at timestamptz,
                                                     OUT schedule_version integer, OUT latest_version integer)
LANGUAGE sql STABLE AS $schedule_effective$
    WITH cur AS (SELECT * FROM anniversary_revisions r WHERE r.anniversary_id = p_anniversary
                  ORDER BY r.version DESC LIMIT 1),
    run AS (SELECT x.version, x.created_at FROM anniversary_revisions x, cur
             WHERE x.anniversary_id = p_anniversary
               AND x.version > COALESCE((SELECT max(b.version) FROM anniversary_revisions b
                                          WHERE b.anniversary_id = p_anniversary
                                            AND ROW(b.kind, b.event_date) IS DISTINCT FROM ROW(cur.kind, cur.event_date)), 0))
    SELECT (SELECT r.created_at FROM run r ORDER BY r.version LIMIT 1),
           (SELECT min(r.version) FROM run r), (SELECT cur.version FROM cur)
$schedule_effective$;

-- ---------------------------------------------------------------- occurrences
CREATE TABLE reminder_occurrences (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    owner_id uuid NOT NULL,
    rule_id uuid NOT NULL,
    event_kind text NOT NULL,
    task_id uuid,
    period_date date,
    anniversary_id uuid,
    anniversary_version integer,
    schedule_version integer,
    target_date date,
    days_before integer,
    fire_at timestamptz NOT NULL,
    stale_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reminder_occurrences_pkey PRIMARY KEY (id),
    CONSTRAINT reminder_occurrences_identity_unique UNIQUE (id, couple_id, owner_id),
    CONSTRAINT reminder_occurrences_rule_fk FOREIGN KEY (rule_id, couple_id, owner_id)
        REFERENCES reminder_rules (id, couple_id, owner_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_occurrences_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_occurrences_anniversary_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reminder_occurrences_version_fk FOREIGN KEY (anniversary_id, anniversary_version)
        REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT,
    CONSTRAINT reminder_occurrences_schedule_version_fk FOREIGN KEY (anniversary_id, schedule_version)
        REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT,
    CONSTRAINT reminder_occurrences_family CHECK (
        (event_kind = 'reminder.task' AND task_id IS NOT NULL
         AND num_nonnulls(anniversary_id, anniversary_version, schedule_version, target_date, days_before) = 0)
        OR (event_kind = 'reminder.anniversary' AND task_id IS NULL AND period_date IS NULL
            AND num_nonnulls(anniversary_id, anniversary_version, schedule_version, target_date, days_before) = 5)),
    CONSTRAINT reminder_occurrences_window CHECK (stale_at > fire_at AND created_at >= fire_at AND created_at < stale_at)
);
-- Stable identity, independent of the rule revision: a changed time never refires a fired period (O3).
CREATE UNIQUE INDEX reminder_occurrences_once_unique ON reminder_occurrences (owner_id, task_id)
    WHERE task_id IS NOT NULL AND period_date IS NULL;
CREATE UNIQUE INDEX reminder_occurrences_period_unique ON reminder_occurrences (owner_id, task_id, period_date)
    WHERE period_date IS NOT NULL;
CREATE UNIQUE INDEX reminder_occurrences_target_unique ON reminder_occurrences (owner_id, anniversary_id, target_date, days_before)
    WHERE anniversary_id IS NOT NULL;

CREATE FUNCTION usapp_reminder_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $reminder_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UV001', MESSAGE = 'USAPP_REMINDER_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$reminder_reject$;
CREATE TRIGGER reminder_rules_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON reminder_rules
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_reminder_reject_mutation();
CREATE TRIGGER reminder_occurrences_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON reminder_occurrences
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_reminder_reject_mutation();

-- Every fact is recomputed; the writer supplies nothing it could get wrong (design Sections 5-7).
CREATE FUNCTION usapp_reminder_occurrence_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $occurrence_guard$
DECLARE
    r reminder_rules%ROWTYPE;
    latest integer;
    t tasks%ROWTYPE;
    span integer;
    period_last date;
    period_deadline timestamptz;
    fire timestamptz;
    stale timestamptz;
    rule_effective timestamptz;
    source_effective timestamptz;
    sched record;
    rev anniversary_revisions%ROWTYPE;
    year_gap integer;
BEGIN
    IF NEW.rule_id IS NULL OR NEW.event_kind IS NULL OR NEW.created_at IS NULL OR NEW.fire_at IS NULL OR NEW.stale_at IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO r FROM reminder_rules WHERE id = NEW.rule_id;
    IF NOT FOUND OR r.couple_id IS DISTINCT FROM NEW.couple_id OR r.owner_id IS DISTINCT FROM NEW.owner_id
       OR r.task_id IS DISTINCT FROM NEW.task_id OR r.anniversary_id IS DISTINCT FROM NEW.anniversary_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_RULE';
    END IF;
    IF r.task_id IS NOT NULL THEN
        PERFORM pg_advisory_xact_lock(33004::integer, hashtext(r.task_id::text));
        PERFORM pg_advisory_xact_lock(33017::integer, hashtext(r.owner_id::text || ':' || r.task_id::text));
        SELECT max(x.seq) INTO latest FROM reminder_rules x WHERE x.owner_id = r.owner_id AND x.task_id = r.task_id;
    ELSE
        PERFORM pg_advisory_xact_lock(33010::integer, hashtext(r.anniversary_id::text));
        PERFORM pg_advisory_xact_lock(33017::integer, hashtext(r.owner_id::text || ':' || r.anniversary_id::text));
        SELECT max(x.seq) INTO latest FROM reminder_rules x WHERE x.owner_id = r.owner_id AND x.anniversary_id = r.anniversary_id;
    END IF;
    IF latest IS DISTINCT FROM r.seq OR NOT r.enabled THEN
        RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_RULE_STALE';
    END IF;
    IF r.task_id IS NOT NULL THEN
        SELECT * INTO t FROM tasks WHERE id = r.task_id;
        IF t.assignee_id IS DISTINCT FROM r.owner_id OR t.recurrence_id IS NOT NULL
           OR EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = t.id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_TARGET';
        END IF;
        IF t.schedule_type IN ('daily', 'weekly') THEN
            span := CASE t.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END;
            IF NEW.period_date IS NULL OR (span = 7 AND EXTRACT(ISODOW FROM NEW.period_date) <> 1)
               OR EXISTS (SELECT 1 FROM tasks p JOIN task_completions c ON c.task_id = p.id
                           WHERE p.recurrence_id = t.id AND p.period_date = NEW.period_date) THEN
                RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_PERIOD';
            END IF;
            period_last := NEW.period_date + (span - 1);
            period_deadline := (NEW.period_date + span)::timestamp AT TIME ZONE 'Asia/Shanghai';
        ELSE
            IF NEW.period_date IS NOT NULL
               OR EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) THEN
                RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_PERIOD';
            END IF;
            period_last := t.deadline_local_date;
            period_deadline := t.deadline_at;
        END IF;
        fire := usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, r.at_local_date,
                                            period_last, period_deadline);
        stale := fire + interval '2 hours';
        IF period_deadline IS NOT NULL THEN stale := LEAST(period_deadline, stale); END IF;
        rule_effective := usapp_reminder_task_effective_at(r.owner_id, r.task_id);
        source_effective := t.created_at;
    ELSE
        SELECT * INTO sched FROM usapp_anniversary_schedule_effective(r.anniversary_id);
        SELECT * INTO rev FROM anniversary_revisions x WHERE x.anniversary_id = r.anniversary_id AND x.version = sched.latest_version;
        IF NOT (NEW.days_before::smallint = ANY (r.days_before_set))
           OR NEW.anniversary_version IS DISTINCT FROM sched.latest_version
           OR NEW.schedule_version IS DISTINCT FROM sched.schedule_version THEN
            RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_TARGET';
        END IF;
        year_gap := EXTRACT(YEAR FROM NEW.target_date)::integer - EXTRACT(YEAR FROM rev.event_date)::integer;
        IF NOT ((rev.kind = 'countdown' AND NEW.target_date = rev.event_date)
                OR (rev.kind = 'anniversary' AND year_gap >= 0
                    AND (rev.event_date + make_interval(years => year_gap))::date = NEW.target_date)) THEN
            RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_TARGET';
        END IF;
        fire := ((NEW.target_date - NEW.days_before) + r.at_time) AT TIME ZONE 'Asia/Shanghai';
        stale := ((NEW.target_date - NEW.days_before) + 1)::timestamp AT TIME ZONE 'Asia/Shanghai';
        rule_effective := usapp_reminder_offset_effective_at(r.owner_id, r.anniversary_id, NEW.days_before);
        source_effective := sched.effective_at;
    END IF;
    IF NEW.event_kind IS DISTINCT FROM (CASE WHEN r.task_id IS NOT NULL THEN 'reminder.task' ELSE 'reminder.anniversary' END)
       OR NEW.fire_at IS DISTINCT FROM fire OR NEW.stale_at IS DISTINCT FROM stale THEN
        RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_FACTS';
    END IF;
    -- The one eligibility rule: never retroactive, never past the window.
    IF rule_effective IS NULL OR GREATEST(rule_effective, source_effective) > fire
       OR NEW.created_at < fire OR NEW.created_at >= stale OR NEW.created_at > clock_timestamp()
       OR clock_timestamp() >= stale THEN
        RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_WINDOW';
    END IF;
    RETURN NEW;
END
$occurrence_guard$;

CREATE TRIGGER reminder_occurrences_guard BEFORE INSERT ON reminder_occurrences
    FOR EACH ROW EXECUTE FUNCTION usapp_reminder_occurrence_guard();

-- ---------------------------------------------------------------- notifications: the closed reminder family
ALTER TABLE notifications ADD COLUMN reminder_occurrence_id uuid;
ALTER TABLE notifications
    ADD CONSTRAINT notifications_reminder_fk FOREIGN KEY (reminder_occurrence_id, couple_id, recipient_id)
        REFERENCES reminder_occurrences (id, couple_id, owner_id) ON DELETE RESTRICT;
ALTER TABLE notifications DROP CONSTRAINT notifications_one_source;
ALTER TABLE notifications
    ADD CONSTRAINT notifications_one_source
        CHECK (num_nonnulls(redemption_id, redemption_event_id, task_change_proposal_id, anniversary_proposal_id,
                            task_id, anniversary_id, message_id, message_reply_id, reminder_occurrence_id) = 1);
ALTER TABLE notifications ALTER COLUMN actor_id DROP NOT NULL;
-- A NULL actor is the reminder family and nothing else; every other family keeps its previous invariant.
ALTER TABLE notifications
    ADD CONSTRAINT notifications_reminder_family
        CHECK ((event_type IN ('reminder.task', 'reminder.anniversary')) = (reminder_occurrence_id IS NOT NULL)
               AND (event_type IN ('reminder.task', 'reminder.anniversary')) = (actor_id IS NULL));
CREATE UNIQUE INDEX notifications_reminder_source_unique ON notifications (event_type, reminder_occurrence_id, recipient_id)
    WHERE reminder_occurrence_id IS NOT NULL;
-- One notification per occurrence, whatever its event type; also serves the deferred link check.
CREATE UNIQUE INDEX notifications_reminder_occurrence_unique ON notifications (reminder_occurrence_id)
    WHERE reminder_occurrence_id IS NOT NULL;

-- The 0020 body, unchanged, in the ELSE; only the first branch is new.
CREATE FUNCTION usapp_notification_reminder_source(source uuid)
RETURNS TABLE(event_type text, couple_id uuid, actor_id uuid, recipient_id uuid,
              target_kind text, target_id uuid, occurred_at timestamptz)
LANGUAGE sql STABLE AS $reminder_source$
 SELECT o.event_kind, o.couple_id, NULL::uuid, o.owner_id,
        CASE o.event_kind WHEN 'reminder.task' THEN 'task' ELSE 'anniversary' END,
        COALESCE(o.task_id, o.anniversary_id), o.created_at
   FROM reminder_occurrences o WHERE o.id = source
$reminder_source$;

CREATE OR REPLACE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record;
BEGIN
 IF NEW.event_type IN ('reminder.task', 'reminder.anniversary') THEN
   SELECT * INTO facts FROM usapp_notification_reminder_source(NEW.reminder_occurrence_id)
    WHERE event_type = NEW.event_type;
 ELSIF NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited', 'message.created',
                       'message.replied') THEN
   IF NEW.event_type = 'task.published' THEN
     family := 'task'; source := NEW.task_id;
   ELSIF NEW.event_type IN ('anniversary.created', 'anniversary.edited') THEN
     family := 'anniversary_revision'; source := NEW.anniversary_id;
   ELSIF NEW.event_type = 'message.created' THEN
     family := 'message'; source := NEW.message_id;
   ELSE
     family := 'message_reply'; source := NEW.message_reply_id;
   END IF;
   SELECT * INTO facts FROM usapp_notification_optional_source(family, source, NEW.anniversary_version);
 ELSE
   -- Event prefix determines the exact expected family; no untyped source or link is trusted.
   IF NEW.event_type IN ('redemption.created','redemption.requested') THEN
     family := 'redemption'; source := NEW.redemption_id;
   ELSIF NEW.event_type = 'redemption.fulfilled' THEN
     family := 'fulfillment'; source := NEW.redemption_id;
   ELSIF NEW.event_type IN ('redemption.confirm','redemption.reject','redemption.withdraw',
     'redemption.cancel_propose','redemption.cancel_approve','redemption.cancel_reject','redemption.cancel_withdraw') THEN
     family := 'redemption_event'; source := NEW.redemption_event_id;
   ELSIF NEW.event_type = 'task.change_proposed' THEN
     family := 'task_proposal'; source := NEW.task_change_proposal_id;
   ELSIF NEW.event_type IN ('task.change_approved','task.change_rejected','task.change_withdrawn') THEN
     family := 'task_decision'; source := NEW.task_change_proposal_id;
   ELSIF NEW.event_type = 'anniversary.edit_proposed' THEN
     family := 'anniversary_proposal'; source := NEW.anniversary_proposal_id;
   ELSIF NEW.event_type IN ('anniversary.edit_approved','anniversary.edit_rejected','anniversary.edit_withdrawn') THEN
     family := 'anniversary_decision'; source := NEW.anniversary_proposal_id;
   ELSE
     RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_EVENT';
   END IF;
   SELECT * INTO facts FROM usapp_notification_source(family, source);
 END IF;
 IF NOT FOUND OR ROW(NEW.event_type,NEW.couple_id,NEW.actor_id,NEW.recipient_id,NEW.target_kind,NEW.target_id)
   IS DISTINCT FROM ROW(facts.event_type,facts.couple_id,facts.actor_id,facts.recipient_id,facts.target_kind,facts.target_id)
   OR NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp() THEN
   RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_SOURCE';
 END IF;
 RETURN NEW;
END
$guard$;

-- An occurrence exists only with the one notification that delivers it (checked at commit).
CREATE FUNCTION usapp_reminder_occurrence_link() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $occurrence_link$
BEGIN
    IF (SELECT count(*) FROM notifications n WHERE n.reminder_occurrence_id = NEW.id) <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UV004', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_UNLINKED';
    END IF;
    RETURN NULL;
END
$occurrence_link$;
CREATE CONSTRAINT TRIGGER reminder_occurrences_notification_link AFTER INSERT ON reminder_occurrences
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_reminder_occurrence_link();

-- ---------------------------------------------------------------- read helpers for the API
-- The next instant under the current rule, or NULL. Reads only.
CREATE FUNCTION usapp_reminder_task_next_fire(p_owner uuid, p_task uuid) RETURNS timestamptz
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
        fire := usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, r.at_local_date,
                                            t.deadline_local_date, t.deadline_at);
        RETURN CASE WHEN fire > now_at AND fire >= floor_at THEN fire END;
    END IF;
    span := CASE t.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END;
    period_start := today - CASE WHEN span = 7 THEN EXTRACT(ISODOW FROM today)::integer - 1 ELSE 0 END;
    -- Each instant lies inside its own period, so this period or the next always holds the answer.
    FOR i IN 0..1 LOOP
        fire := usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, r.at_local_date,
                                            period_start + (span - 1), (period_start + span)::timestamp AT TIME ZONE 'Asia/Shanghai');
        IF fire > now_at AND fire >= floor_at
           AND NOT EXISTS (SELECT 1 FROM tasks p JOIN task_completions c ON c.task_id = p.id
                            WHERE p.recurrence_id = t.id AND p.period_date = period_start) THEN
            RETURN fire;
        END IF;
        period_start := period_start + span;
    END LOOP;
    RETURN NULL;
END
$task_next$;

CREATE FUNCTION usapp_reminder_anniversary_next_fire(p_owner uuid, p_anniversary uuid) RETURNS timestamptz
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
            IF fire > now_at AND fire >= floor_at THEN
                IF best IS NULL OR fire < best THEN best := fire; END IF;
                EXIT;
            END IF;
        END LOOP;
    END LOOP;
    RETURN best;
END
$anniversary_next$;

REVOKE ALL ON reminder_rules, reminder_occurrences FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON reminder_rules, reminder_occurrences TO usapp_api;

COMMIT;
