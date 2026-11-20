-- =====================================================================
-- 0028_reminder_push_stale: P2-6 R5 (docs/phase2-reminders.md Sections 10 and 21). 0000-0027 stay frozen.
--
-- A pending reminder push whose source no longer warrants it is not sent. This migration adds the one definition of
-- that source validity, the internal attempt outcome source_stale_skipped and the final state source_stale. The inbox
-- notification is never touched, and every other notification keeps its behaviour: the function is true for it, and
-- the guards refuse both new values on it.
--
-- Neither guard asks the function whether the source is stale right now. The pre-send check decides from what it read;
-- by the time the skip is recorded the source may have changed back, and refusing that record would roll it back and
-- let the lease expire, which spends provider budget for a request that was never sent.
-- No row is changed, no lock is added and no privilege changes.
-- =====================================================================

BEGIN;

DO $stale_preflight$
BEGIN
    IF to_regproc('public.usapp_reminder_rule_needs_reset') IS NULL THEN
        RAISE EXCEPTION '0028_reminder_push_stale requires 0027_reminder_successor_rules';
    END IF;
END
$stale_preflight$;

-- True for anything that is not a reminder. p_at is the caller's single reading of the clock, so that this answer and
-- the expiry it sends agree. Everything below is derived from current rows, so a change and its reversal cancel out:
-- only a terminal delivery stays decided. STABLE: one consistent snapshot in the calling statement. Reads only.
CREATE FUNCTION usapp_reminder_occurrence_valid(p_notification uuid, p_at timestamptz DEFAULT clock_timestamp())
RETURNS boolean LANGUAGE plpgsql STABLE AS $occurrence_valid$
DECLARE
    o reminder_occurrences%ROWTYPE;
    r reminder_rules%ROWTYPE;
    t tasks%ROWTYPE;
    rev anniversary_revisions%ROWTYPE;
    year_gap integer;
BEGIN
    SELECT k.* INTO o FROM notifications n JOIN reminder_occurrences k ON k.id = n.reminder_occurrence_id
     WHERE n.id = p_notification;
    IF NOT FOUND THEN RETURN true; END IF;
    IF p_at >= o.stale_at THEN RETURN false; END IF;
    IF o.task_id IS NOT NULL THEN
        -- The rule's terms are not compared: a changed time is still the reminder that was due (no refire, O3).
        SELECT * INTO r FROM reminder_rules x WHERE x.owner_id = o.owner_id AND x.task_id = o.task_id ORDER BY x.seq DESC LIMIT 1;
        IF NOT FOUND OR NOT r.enabled THEN RETURN false; END IF;
        SELECT * INTO t FROM tasks WHERE id = o.task_id;
        -- Cancelled or superseded. A period's deadline is already inside stale_at, by the occurrence guard.
        IF t.assignee_id IS DISTINCT FROM o.owner_id OR EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = t.id) THEN
            RETURN false;
        END IF;
        IF o.period_date IS NULL THEN
            IF (t.deadline_at IS NOT NULL AND p_at >= t.deadline_at)
               OR EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) THEN
                RETURN false;
            END IF;
        -- Partial progress has no completion row, so it never suppresses.
        ELSIF EXISTS (SELECT 1 FROM tasks p JOIN task_completions c ON c.task_id = p.id
                       WHERE p.recurrence_id = t.id AND p.period_date = o.period_date) THEN
            RETURN false;
        END IF;
        RETURN true;
    END IF;
    SELECT * INTO r FROM reminder_rules x WHERE x.owner_id = o.owner_id AND x.anniversary_id = o.anniversary_id ORDER BY x.seq DESC LIMIT 1;
    -- A removed offset is the user saying not to remind then, not a changed time.
    IF NOT FOUND OR NOT r.enabled OR NOT (o.days_before::smallint = ANY (r.days_before_set)) THEN
        RETURN false;
    END IF;
    -- The current schedule, whichever revision fired it: title and note are not part of it. The occurrence guard's test.
    SELECT * INTO rev FROM anniversary_revisions x WHERE x.anniversary_id = o.anniversary_id ORDER BY x.version DESC LIMIT 1;
    year_gap := EXTRACT(YEAR FROM o.target_date)::integer - EXTRACT(YEAR FROM rev.event_date)::integer;
    RETURN COALESCE((rev.kind = 'countdown' AND o.target_date = rev.event_date)
                    OR (rev.kind = 'anniversary' AND year_gap >= 0
                        AND (rev.event_date + make_interval(years => year_gap))::date = o.target_date), false);
END
$occurrence_valid$;

-- ---------------------------------------------------------------- the two new closed values
-- Internal: nothing reached a provider, so it can never spend the budget (usapp_push_provider_attempts counts provider
-- observations and expiries with no observation).
ALTER TABLE push_attempt_observations DROP CONSTRAINT push_attempt_observations_class_rule;
ALTER TABLE push_attempt_observations
    ADD CONSTRAINT push_attempt_observations_class_rule CHECK (
        (class = 'provider' AND outcome IN ('accepted', 'gone', 'rejected', 'transient', 'unknown',
                                            'provider_auth_failed'))
        OR (class = 'internal' AND outcome IN ('worker_auth_failed', 'transport_unavailable', 'busy',
                                               'inactive_skipped', 'ttl_skipped', 'cutover_skipped',
                                               'control_skipped', 'terminal_skipped', 'lease_lost_skipped',
                                               'source_stale_skipped')));

ALTER TABLE push_delivery_outcomes DROP CONSTRAINT push_delivery_outcomes_state_valid;
ALTER TABLE push_delivery_outcomes
    ADD CONSTRAINT push_delivery_outcomes_state_valid CHECK (final_state IN ('accepted', 'gone', 'rejected', 'inactive',
                                                                             'expired', 'exhausted', 'cutover_skipped',
                                                                             'source_stale'));
-- source_stale names the attempt whose pre-send check decided it.
ALTER TABLE push_delivery_outcomes DROP CONSTRAINT push_delivery_outcomes_attempt_rule;
ALTER TABLE push_delivery_outcomes
    ADD CONSTRAINT push_delivery_outcomes_attempt_rule CHECK (
        final_state NOT IN ('accepted', 'gone', 'rejected', 'exhausted', 'source_stale') OR attempt_no IS NOT NULL);

-- The 0023 body plus the reminder family of source_stale_skipped.
CREATE OR REPLACE FUNCTION usapp_push_observation_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $observation_guard$
DECLARE
    claimed timestamptz;
BEGIN
    SELECT a.claimed_at INTO claimed FROM push_attempts a
     WHERE a.notification_id = NEW.notification_id AND a.generation_id = NEW.generation_id
       AND a.attempt_no = NEW.attempt_no;
    IF claimed IS NOT NULL AND (NEW.observed_at < claimed OR NEW.observed_at > clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ005', MESSAGE = 'USAPP_PUSH_OBSERVATION_TIME';
    END IF;
    IF NEW.outcome = 'source_stale_skipped' AND NOT EXISTS (
           SELECT 1 FROM notifications n WHERE n.id = NEW.notification_id AND n.reminder_occurrence_id IS NOT NULL) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ005', MESSAGE = 'USAPP_PUSH_OBSERVATION_SOURCE';
    END IF;
    RETURN NEW;
END
$observation_guard$;

-- The 0023 body plus source_stale, which only that exact attempt's own observation can decide.
CREATE OR REPLACE FUNCTION usapp_push_outcome_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $outcome_guard$
DECLARE
    observed text;
    created timestamptz;
    notice_at timestamptz;
    ctl record;
BEGIN
    SELECT d.created_at INTO created FROM push_deliveries d
     WHERE d.notification_id = NEW.notification_id AND d.generation_id = NEW.generation_id;
    IF created IS NOT NULL AND (NEW.decided_at < created OR NEW.decided_at > clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_TIME';
    END IF;
    IF NEW.final_state IN ('accepted', 'gone', 'rejected') THEN
        SELECT o.outcome INTO observed FROM push_attempt_observations o
         WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id
           AND o.attempt_no = NEW.attempt_no;
        IF observed IS DISTINCT FROM NEW.final_state THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_OBSERVATION';
        END IF;
    ELSIF NEW.final_state = 'source_stale' THEN
        SELECT o.outcome INTO observed FROM push_attempt_observations o
         WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id
           AND o.attempt_no = NEW.attempt_no;
        IF observed IS DISTINCT FROM 'source_stale_skipped' THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_OBSERVATION';
        END IF;
    ELSIF NEW.final_state = 'exhausted' THEN
        IF usapp_push_provider_attempts(NEW.notification_id, NEW.generation_id) < 5 THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_BUDGET';
        END IF;
    ELSIF NEW.final_state = 'cutover_skipped' THEN
        SELECT n.created_at INTO notice_at FROM notifications n WHERE n.id = NEW.notification_id;
        SELECT * INTO ctl FROM usapp_push_control_state();
        IF ctl.cutover IS NULL OR notice_at >= ctl.cutover THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_CUTOVER';
        END IF;
    END IF;
    RETURN NEW;
END
$outcome_guard$;

COMMIT;
