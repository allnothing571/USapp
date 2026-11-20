-- =====================================================================
-- 0027_reminder_successor_rules: P2-6 R4 (docs/phase2-reminders.md Sections 5.1 and 20). 0000-0026 stay frozen.
--
-- An approved edit copies the assignee's latest reminder rule to the successor, in the approval's transaction.
-- This migration adds verifiable provenance, enforces it in the guard, and adds one derived fact: an enabled inherited
-- rule whose settings do not fit the new terms, or whose instant had passed when it was copied, "needs reset". Such a
-- rule keeps the user's choice, has no next instant and produces no occurrence. No row is changed. User-origin
-- validation is unchanged, and so is occurrence eligibility apart from this one refusal.
-- =====================================================================

BEGIN;

DO $successor_preflight$
BEGIN
    IF to_regclass('public.reminder_occurrences') IS NULL OR NOT EXISTS (
        SELECT 1 FROM pg_proc p WHERE p.proname = 'usapp_reminder_task_next_fire' AND p.prosrc LIKE '%reminder_occurrences%') THEN
        RAISE EXCEPTION '0027_reminder_successor_rules requires 0026_reminder_next_fire';
    END IF;
END
$successor_preflight$;

ALTER TABLE reminder_rules ADD COLUMN source_rule_id uuid;
ALTER TABLE reminder_rules ADD COLUMN source_proposal_id uuid;
ALTER TABLE reminder_rules
    ADD CONSTRAINT reminder_rules_source_rule_fk FOREIGN KEY (source_rule_id, couple_id, owner_id)
        REFERENCES reminder_rules (id, couple_id, owner_id) ON DELETE RESTRICT;
ALTER TABLE reminder_rules
    ADD CONSTRAINT reminder_rules_source_decision_fk FOREIGN KEY (source_proposal_id)
        REFERENCES task_change_decisions (proposal_id) ON DELETE RESTRICT;
-- Provenance is exactly the successor origin's, on a task's first revision.
ALTER TABLE reminder_rules
    ADD CONSTRAINT reminder_rules_source_presence CHECK (
        (origin = 'successor') = (source_rule_id IS NOT NULL)
        AND (origin = 'successor') = (source_proposal_id IS NOT NULL)
        AND (origin <> 'successor' OR (task_id IS NOT NULL AND seq = 1)));
-- One decision has one successor, and that successor one assignee.
CREATE UNIQUE INDEX reminder_rules_source_decision_unique ON reminder_rules (source_proposal_id)
    WHERE source_proposal_id IS NOT NULL;

-- The one definition, used by the read, the generator and the occurrence guard. Measured at the copy's own time, so a
-- usable copy does not later become "needs reset" merely because its instant arrived. A closed or completed target
-- reports its own state instead.
CREATE FUNCTION usapp_reminder_rule_needs_reset(p_rule uuid) RETURNS boolean
LANGUAGE sql STABLE AS $needs_reset$
    SELECT COALESCE((SELECT r.origin = 'successor' AND r.enabled
                            AND usapp_reminder_task_rule_problem(r.task_id, true, r.mode, r.before_minutes, r.days_before,
                                                                 r.at_time, r.at_local_date, r.created_at) IN ('invalid', 'passed')
                       FROM reminder_rules r WHERE r.id = p_rule), false)
$needs_reset$;

-- VOLATILE: statements after the advisory-lock waits see the previous holder's commit.
CREATE OR REPLACE FUNCTION usapp_reminder_rule_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $rule_guard$
DECLARE
    t tasks%ROWTYPE;
    a anniversaries%ROWTYPE;
    problem text;
    prev reminder_rules%ROWTYPE;
    has_prev boolean;
    target_created timestamptz;
    decision task_change_decisions%ROWTYPE;
    proposal task_change_proposals%ROWTYPE;
    source reminder_rules%ROWTYPE;
    predecessor tasks%ROWTYPE;
BEGIN
    IF NEW.couple_id IS NULL OR NEW.owner_id IS NULL OR NEW.seq IS NULL OR NEW.enabled IS NULL
       OR NEW.created_at IS NULL OR num_nonnulls(NEW.task_id, NEW.anniversary_id) <> 1 THEN
        RETURN NEW;
    END IF;
    IF NEW.origin IS DISTINCT FROM 'user' AND NEW.origin IS DISTINCT FROM 'successor' THEN
        RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_ORIGIN';
    END IF;
    IF NEW.origin = 'successor' THEN
        IF NEW.task_id IS NULL OR NEW.source_rule_id IS NULL OR NEW.source_proposal_id IS NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_SOURCE';
        END IF;
        -- The approved edit that created exactly this successor.
        SELECT * INTO decision FROM task_change_decisions WHERE proposal_id = NEW.source_proposal_id;
        SELECT * INTO proposal FROM task_change_proposals WHERE id = NEW.source_proposal_id;
        IF decision.outcome IS DISTINCT FROM 'approved' OR proposal.kind IS DISTINCT FROM 'edit'
           OR decision.successor_task_id IS DISTINCT FROM NEW.task_id OR decision.couple_id IS DISTINCT FROM NEW.couple_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_SOURCE';
        END IF;
        -- Predecessor first: every rule write takes this lock, so the latest revision cannot move underneath.
        PERFORM pg_advisory_xact_lock(33004::integer, hashtext(proposal.task_id::text));
        SELECT * INTO predecessor FROM tasks WHERE id = proposal.task_id;
        SELECT * INTO source FROM reminder_rules WHERE id = NEW.source_rule_id;
        -- The assignee's own latest revision, and its terms exactly: nobody can choose or change them.
        IF source.task_id IS DISTINCT FROM proposal.task_id OR source.owner_id IS DISTINCT FROM NEW.owner_id
           OR source.couple_id IS DISTINCT FROM NEW.couple_id OR predecessor.assignee_id IS DISTINCT FROM NEW.owner_id
           OR source.seq IS DISTINCT FROM (SELECT max(x.seq) FROM reminder_rules x
                                            WHERE x.owner_id = NEW.owner_id AND x.task_id = proposal.task_id)
           OR ROW(source.enabled, source.mode, source.before_minutes, source.days_before, source.at_time, source.at_local_date, source.days_before_set)
              IS DISTINCT FROM ROW(NEW.enabled, NEW.mode, NEW.before_minutes, NEW.days_before, NEW.at_time, NEW.at_local_date, NEW.days_before_set) THEN
            RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_SOURCE';
        END IF;
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
        -- A copy keeps the user's choice even when it no longer fits; usapp_reminder_rule_needs_reset reports that.
        IF NEW.origin = 'user' THEN
            problem := usapp_reminder_task_rule_problem(NEW.task_id, NEW.enabled, NEW.mode, NEW.before_minutes,
                                                        NEW.days_before, NEW.at_time, NEW.at_local_date, NEW.created_at);
            IF problem IS NOT NULL THEN
                RAISE EXCEPTION USING ERRCODE = 'UV002', MESSAGE = 'USAPP_REMINDER_' || upper(problem);
            END IF;
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

-- Every fact is recomputed; the writer supplies nothing it could get wrong (design Sections 5-7).
CREATE OR REPLACE FUNCTION usapp_reminder_occurrence_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $occurrence_guard$
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
    -- An inherited rule that no longer fits generates nothing until the user sets it again.
    IF usapp_reminder_rule_needs_reset(r.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UV003', MESSAGE = 'USAPP_REMINDER_OCCURRENCE_NEEDS_RESET';
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
    -- No instant is fabricated for an inherited rule that no longer fits.
    IF usapp_reminder_rule_needs_reset(r.id) THEN RETURN NULL; END IF;
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

-- Deferred: a supersession whose predecessor has a rule for its assignee commits only with the copy of the latest one.
CREATE FUNCTION usapp_reminder_successor_link() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $successor_link$
DECLARE
    latest uuid;
BEGIN
    SELECT r.id INTO latest
      FROM tasks t JOIN reminder_rules r ON r.task_id = t.id AND r.owner_id = t.assignee_id
     WHERE t.id = NEW.task_id ORDER BY r.seq DESC LIMIT 1;
    IF FOUND AND NOT EXISTS (
        SELECT 1 FROM reminder_rules c
         WHERE c.origin = 'successor' AND c.source_rule_id = latest AND c.source_proposal_id = NEW.proposal_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UV005', MESSAGE = 'USAPP_REMINDER_SUCCESSOR_UNCOPIED';
    END IF;
    RETURN NULL;
END
$successor_link$;
CREATE CONSTRAINT TRIGGER task_closures_reminder_copied AFTER INSERT ON task_closures
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.reason = 'superseded')
    EXECUTE FUNCTION usapp_reminder_successor_link();

COMMIT;
