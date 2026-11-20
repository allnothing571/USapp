-- M6: confirmed task edits and cancellations (docs/phase1-m6-design.md).
-- Append-only proposals, decisions and closures plus immutable successor tasks.
-- No existing row changes: an approved edit closes the old root task as
-- 'superseded' and inserts a successor root with the new terms in the same
-- transaction; an approved cancellation closes the root as 'cancelled'.
-- Recurring instances are never closed individually: an unfinished stored period
-- whose deadline is after its template's closed_at reads as closed; a completed
-- period keeps its completion. 0000-0012 are unchanged. No data is seeded.
-- Lock: the existing per-root advisory lock (33004, hashtext(root uuid)).
-- UX002 proposal; UX003 decision; UX004 closure; UX005 deferred link;
-- UX006 action on a closed task; UX007 period of a closed template.
-- Mutation of the new tables uses the existing UT001.
BEGIN;

DO $change_preflight$
BEGIN
    IF to_regprocedure('public.usapp_custom_action_guard()') IS NULL
       OR position('''custom''' IN pg_get_functiondef('public.usapp_daily_action_guard()'::regprocedure)) = 0 THEN
        RAISE EXCEPTION '0013_task_changes requires 0012_custom_action_guard';
    END IF;
END
$change_preflight$;

CREATE TABLE task_change_proposals (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    task_id uuid NOT NULL,
    proposer_id uuid NOT NULL,
    kind text NOT NULL,
    title text,
    description text,
    reward_flowers integer,
    reward_sunshine integer,
    completion_mode text,
    target_count integer,
    deadline_local_date date,
    deadline_local_time time,
    start_local_date date,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT task_change_proposals_pkey PRIMARY KEY (id),
    CONSTRAINT task_change_proposals_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT task_change_proposals_key_unique UNIQUE (proposer_id, idempotency_key),
    CONSTRAINT task_change_proposals_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_change_proposals_member_fk FOREIGN KEY (couple_id, proposer_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT task_change_proposals_kind_valid CHECK (kind IN ('edit', 'cancel')),
    CONSTRAINT task_change_proposals_terms_shape CHECK (
        (kind = 'cancel' AND title IS NULL AND description IS NULL AND reward_flowers IS NULL
         AND reward_sunshine IS NULL AND completion_mode IS NULL AND target_count IS NULL
         AND deadline_local_date IS NULL AND deadline_local_time IS NULL AND start_local_date IS NULL)
        OR (kind = 'edit' AND title IS NOT NULL AND description IS NOT NULL AND reward_flowers IS NOT NULL
            AND reward_sunshine IS NOT NULL AND completion_mode IS NOT NULL AND target_count IS NOT NULL)
    ),
    -- Same shapes as the 0007/0008/0011 task CHECKs, so an approved successor is insertable.
    CONSTRAINT task_change_proposals_title_shape CHECK (
        title IS NULL OR (char_length(title) BETWEEN 1 AND 40 AND title = btrim(title)
                          AND title !~ '[\x01-\x1f\x7f-\x9f  ]')
    ),
    CONSTRAINT task_change_proposals_description_shape CHECK (
        description IS NULL OR (char_length(description) <= 500
                                AND description !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]')
    ),
    CONSTRAINT task_change_proposals_reward_range CHECK (
        (reward_flowers IS NULL OR reward_flowers BETWEEN 0 AND 999)
        AND (reward_sunshine IS NULL OR reward_sunshine BETWEEN 0 AND 100000)
        AND (kind = 'cancel' OR reward_flowers > 0 OR reward_sunshine > 0)
    ),
    CONSTRAINT task_change_proposals_mode_valid CHECK (
        (completion_mode IS NULL OR completion_mode IN ('single', 'count'))
        AND (target_count IS NULL OR (target_count >= 1 AND (completion_mode = 'count' OR target_count = 1)))
    ),
    CONSTRAINT task_change_proposals_dates_valid CHECK (
        (deadline_local_time IS NULL OR deadline_local_date IS NOT NULL)
        AND (start_local_date IS NULL OR (deadline_local_date IS NOT NULL AND start_local_date <= deadline_local_date))
        AND (deadline_local_date IS NULL OR deadline_local_date BETWEEN DATE '1970-01-01' AND DATE '9998-12-31')
        AND (start_local_date IS NULL OR start_local_date BETWEEN DATE '1970-01-01' AND DATE '9998-12-31')
    ),
    CONSTRAINT task_change_proposals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT task_change_proposals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE INDEX task_change_proposals_task_idx ON task_change_proposals (task_id, created_at);

CREATE TABLE task_change_decisions (
    proposal_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    decided_by uuid NOT NULL,
    outcome text NOT NULL,
    successor_task_id uuid,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT task_change_decisions_pkey PRIMARY KEY (proposal_id),
    CONSTRAINT task_change_decisions_key_unique UNIQUE (decided_by, idempotency_key),
    CONSTRAINT task_change_decisions_successor_unique UNIQUE (successor_task_id),
    CONSTRAINT task_change_decisions_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES task_change_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_change_decisions_member_fk FOREIGN KEY (couple_id, decided_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- The successor is inserted after its decision row in the same transaction.
    CONSTRAINT task_change_decisions_successor_fk FOREIGN KEY (successor_task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT task_change_decisions_outcome_valid CHECK (outcome IN ('approved', 'rejected', 'withdrawn')),
    CONSTRAINT task_change_decisions_successor_approved CHECK (successor_task_id IS NULL OR outcome = 'approved'),
    CONSTRAINT task_change_decisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT task_change_decisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE task_closures (
    task_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    reason text NOT NULL,
    proposal_id uuid NOT NULL,
    closed_at timestamptz NOT NULL,
    CONSTRAINT task_closures_pkey PRIMARY KEY (task_id),
    CONSTRAINT task_closures_proposal_unique UNIQUE (proposal_id),
    CONSTRAINT task_closures_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_closures_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES task_change_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_closures_reason_valid CHECK (reason IN ('cancelled', 'superseded'))
);

CREATE TRIGGER task_change_proposals_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON task_change_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();
CREATE TRIGGER task_change_decisions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON task_change_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();
CREATE TRIGGER task_closures_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON task_closures
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();

-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE FUNCTION usapp_task_change_proposal_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $proposal_guard$
DECLARE
    t tasks%ROWTYPE;
BEGIN
    IF NEW.task_id IS NULL OR NEW.couple_id IS NULL OR NEW.proposer_id IS NULL
       OR NEW.kind IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.task_id::text));
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.couple_id IS DISTINCT FROM NEW.couple_id OR t.recurrence_id IS NOT NULL
       OR NEW.proposer_id NOT IN (t.creator_id, t.assignee_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_TARGET';
    END IF;
    IF NEW.kind = 'edit' AND NEW.proposer_id IS DISTINCT FROM t.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_PROPOSER';
    END IF;
    IF NEW.kind = 'edit' AND NOT (CASE t.schedule_type
            WHEN 'one_off' THEN NEW.start_local_date IS NULL
            WHEN 'custom' THEN NEW.completion_mode = 'count' AND NEW.start_local_date IS NOT NULL
                               AND NEW.deadline_local_time IS NULL
            ELSE NEW.completion_mode = 'count' AND NEW.deadline_local_date IS NULL
                 AND NEW.start_local_date IS NULL
         END) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_TERMS';
    END IF;
    IF EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = t.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_CLOSED';
    END IF;
    IF t.schedule_type IN ('one_off', 'custom')
       AND EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_COMPLETED';
    END IF;
    IF EXISTS (SELECT 1 FROM task_change_proposals p
                WHERE p.task_id = t.id
                  AND NOT EXISTS (SELECT 1 FROM task_change_decisions d WHERE d.proposal_id = p.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_PENDING';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < t.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UX002', MESSAGE = 'USAPP_CHANGE_TIME';
    END IF;
    RETURN NEW;
END
$proposal_guard$;

CREATE TRIGGER task_change_proposals_guard BEFORE INSERT ON task_change_proposals
    FOR EACH ROW EXECUTE FUNCTION usapp_task_change_proposal_guard();

CREATE FUNCTION usapp_task_change_decision_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $decision_guard$
DECLARE
    p task_change_proposals%ROWTYPE;
    t tasks%ROWTYPE;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL
       OR NEW.outcome IS NULL OR NEW.decided_at IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO p FROM task_change_proposals WHERE id = NEW.proposal_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UX003', MESSAGE = 'USAPP_DECISION_PROPOSAL';
    END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(p.task_id::text));
    SELECT * INTO t FROM tasks WHERE id = p.task_id;
    IF (NEW.outcome = 'withdrawn') IS DISTINCT FROM (NEW.decided_by = p.proposer_id)
       OR NEW.decided_by NOT IN (t.creator_id, t.assignee_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX003', MESSAGE = 'USAPP_DECISION_ACTOR';
    END IF;
    IF (NEW.successor_task_id IS NOT NULL) IS DISTINCT FROM (NEW.outcome = 'approved' AND p.kind = 'edit') THEN
        RAISE EXCEPTION USING ERRCODE = 'UX003', MESSAGE = 'USAPP_DECISION_SUCCESSOR';
    END IF;
    IF NEW.decided_at > clock_timestamp() OR NEW.decided_at < p.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UX003', MESSAGE = 'USAPP_DECISION_TIME';
    END IF;
    RETURN NEW;
END
$decision_guard$;

CREATE TRIGGER task_change_decisions_guard BEFORE INSERT ON task_change_decisions
    FOR EACH ROW EXECUTE FUNCTION usapp_task_change_decision_guard();

CREATE FUNCTION usapp_task_closure_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $closure_guard$
DECLARE
    d task_change_decisions%ROWTYPE;
    p task_change_proposals%ROWTYPE;
    t tasks%ROWTYPE;
BEGIN
    IF NEW.task_id IS NULL OR NEW.couple_id IS NULL OR NEW.reason IS NULL
       OR NEW.proposal_id IS NULL OR NEW.closed_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.task_id::text));
    SELECT * INTO d FROM task_change_decisions WHERE proposal_id = NEW.proposal_id;
    SELECT * INTO p FROM task_change_proposals WHERE id = NEW.proposal_id;
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF d.outcome IS DISTINCT FROM 'approved' OR p.task_id IS DISTINCT FROM NEW.task_id
       OR p.couple_id IS DISTINCT FROM NEW.couple_id OR t.recurrence_id IS NOT NULL
       OR (NEW.reason = 'cancelled') IS DISTINCT FROM (p.kind = 'cancel')
       OR NEW.closed_at IS DISTINCT FROM d.decided_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UX004', MESSAGE = 'USAPP_CLOSURE_DECISION';
    END IF;
    IF t.schedule_type IN ('one_off', 'custom') THEN
        IF EXISTS (SELECT 1 FROM task_completions c WHERE c.task_id = t.id)
           OR (t.deadline_at IS NOT NULL AND NEW.closed_at >= t.deadline_at) THEN
            RAISE EXCEPTION USING ERRCODE = 'UX004', MESSAGE = 'USAPP_CLOSURE_STATE';
        END IF;
        IF p.kind = 'edit' AND EXISTS (SELECT 1 FROM task_progress_events e WHERE e.task_id = t.id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UX004', MESSAGE = 'USAPP_CLOSURE_PROGRESS';
        END IF;
    ELSIF p.kind = 'edit' AND EXISTS (
            -- A stored period is materialized together with its first +1.
            SELECT 1 FROM tasks i
             WHERE i.recurrence_id = t.id
               AND NEW.closed_at >= (i.period_date::timestamp AT TIME ZONE 'Asia/Shanghai')
               AND NEW.closed_at < i.deadline_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX004', MESSAGE = 'USAPP_CLOSURE_PROGRESS';
    END IF;
    RETURN NEW;
END
$closure_guard$;

CREATE TRIGGER task_closures_guard BEFORE INSERT ON task_closures
    FOR EACH ROW EXECUTE FUNCTION usapp_task_closure_guard();

-- Deferred: approved <=> one closure; approved edit <=> one exact successor root.
CREATE FUNCTION usapp_task_change_validate(p_proposal uuid)
RETURNS void LANGUAGE plpgsql VOLATILE
AS $change_validate$
DECLARE
    p task_change_proposals%ROWTYPE;
    d task_change_decisions%ROWTYPE;
    t tasks%ROWTYPE;
    s tasks%ROWTYPE;
    n_closures bigint;
BEGIN
    SELECT * INTO p FROM task_change_proposals WHERE id = p_proposal;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UX005', MESSAGE = 'USAPP_CHANGE_LINK_PROPOSAL';
    END IF;
    SELECT count(*) INTO n_closures FROM task_closures WHERE proposal_id = p_proposal;
    SELECT * INTO d FROM task_change_decisions WHERE proposal_id = p_proposal;
    IF (FOUND AND d.outcome = 'approved') IS DISTINCT FROM (n_closures = 1)
       OR n_closures > 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UX005', MESSAGE = 'USAPP_CHANGE_LINK_CLOSURE';
    END IF;
    IF d.successor_task_id IS NULL THEN
        IF d.outcome = 'approved' AND p.kind = 'edit' THEN
            RAISE EXCEPTION USING ERRCODE = 'UX005', MESSAGE = 'USAPP_CHANGE_LINK_SUCCESSOR';
        END IF;
        RETURN;
    END IF;
    SELECT * INTO t FROM tasks WHERE id = p.task_id;
    SELECT * INTO s FROM tasks WHERE id = d.successor_task_id;
    IF NOT FOUND OR s.recurrence_id IS NOT NULL OR s.period_date IS NOT NULL
       OR ROW(s.couple_id, s.creator_id, s.assignee_id, s.schedule_type, s.timezone)
          IS DISTINCT FROM ROW(t.couple_id, t.creator_id, t.assignee_id, t.schedule_type, t.timezone)
       OR ROW(s.title, s.description, s.reward_flowers, s.reward_sunshine, s.completion_mode, s.target_count,
              s.deadline_local_date, s.deadline_local_time, s.start_local_date)
          IS DISTINCT FROM
          ROW(p.title, p.description, p.reward_flowers, p.reward_sunshine, p.completion_mode, p.target_count,
              p.deadline_local_date, p.deadline_local_time, p.start_local_date)
       OR s.created_at IS DISTINCT FROM d.decided_at
       OR s.create_idempotency_key IS DISTINCT FROM ('chg:' || p.id::text)
       OR (s.deadline_at IS NOT NULL AND s.deadline_at <= d.decided_at)
       -- A custom successor keeps the original start, or starts on/after the decision date.
       OR (s.start_local_date IS DISTINCT FROM t.start_local_date
           AND s.start_local_date < (d.decided_at AT TIME ZONE 'Asia/Shanghai')::date) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX005', MESSAGE = 'USAPP_CHANGE_LINK_SUCCESSOR';
    END IF;
END
$change_validate$;

CREATE FUNCTION usapp_task_change_link_check()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $change_link$
DECLARE
    v_proposal uuid;
BEGIN
    IF TG_TABLE_NAME = 'tasks' THEN
        SELECT d.proposal_id INTO v_proposal FROM task_change_decisions d WHERE d.successor_task_id = NEW.id;
        IF NOT FOUND THEN
            RAISE EXCEPTION USING ERRCODE = 'UX005', MESSAGE = 'USAPP_CHANGE_LINK_ORPHAN';
        END IF;
    ELSE
        v_proposal := NEW.proposal_id;
    END IF;
    PERFORM usapp_task_change_validate(v_proposal);
    RETURN NULL;
END
$change_link$;

CREATE CONSTRAINT TRIGGER task_change_decisions_link_valid AFTER INSERT ON task_change_decisions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_change_link_check();
CREATE CONSTRAINT TRIGGER task_closures_link_valid AFTER INSERT ON task_closures
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_change_link_check();
CREATE CONSTRAINT TRIGGER tasks_successor_link_valid AFTER INSERT ON tasks
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.create_idempotency_key LIKE 'chg:%')
    EXECUTE FUNCTION usapp_task_change_link_check();

-- Named to fire before the existing progress/completion guards: root lock first.
CREATE FUNCTION usapp_task_closed_action_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $closed_action$
DECLARE
    v_root uuid;
BEGIN
    IF NEW.task_id IS NULL THEN RETURN NEW; END IF;
    SELECT COALESCE(t.recurrence_id, t.id) INTO v_root FROM tasks t WHERE t.id = NEW.task_id;
    IF NOT FOUND THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(v_root::text));
    IF EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = v_root) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX006', MESSAGE = 'USAPP_TASK_CLOSED';
    END IF;
    RETURN NEW;
END
$closed_action$;

CREATE TRIGGER task_progress_events_closure_guard BEFORE INSERT ON task_progress_events
    FOR EACH ROW EXECUTE FUNCTION usapp_task_closed_action_guard();
CREATE TRIGGER task_completions_closure_guard BEFORE INSERT ON task_completions
    FOR EACH ROW EXECUTE FUNCTION usapp_task_closed_action_guard();

CREATE FUNCTION usapp_task_closed_period_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $closed_period$
BEGIN
    IF NEW.recurrence_id IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.recurrence_id::text));
    IF EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = NEW.recurrence_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UX007', MESSAGE = 'USAPP_TASK_CLOSED_PERIOD';
    END IF;
    RETURN NEW;
END
$closed_period$;

CREATE TRIGGER tasks_period_closure_guard BEFORE INSERT ON tasks
    FOR EACH ROW EXECUTE FUNCTION usapp_task_closed_period_guard();

-- Same 22 columns as 0011. Live templates are unchanged. A closed template has
-- no virtual current row; all its stored periods stay readable, and its closure
-- period is its current_period row (a virtual one only when nothing was stored).
CREATE OR REPLACE VIEW task_read_model WITH (security_invoker = true) AS
WITH day AS (SELECT (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS d),
cur AS (
    SELECT r.id AS root_id, x.span,
           day.d - CASE WHEN x.span = 7 THEN EXTRACT(ISODOW FROM day.d)::integer - 1 ELSE 0 END AS ps
      FROM tasks r CROSS JOIN day
     CROSS JOIN LATERAL (SELECT CASE r.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END AS span) x
     WHERE r.schedule_type IN ('daily', 'weekly') AND r.recurrence_id IS NULL
       AND NOT EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = r.id)
),
fin AS (
    SELECT r.id AS root_id, x.span,
           x.cd - CASE WHEN x.span = 7 THEN EXTRACT(ISODOW FROM x.cd)::integer - 1 ELSE 0 END AS ps
      FROM tasks r JOIN task_closures k ON k.task_id = r.id
     CROSS JOIN LATERAL (SELECT CASE r.schedule_type WHEN 'weekly' THEN 7 ELSE 1 END AS span,
                                (k.closed_at AT TIME ZONE 'Asia/Shanghai')::date AS cd) x
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
 WHERE p.period_date < c.ps
UNION ALL
SELECT p.id, p.couple_id, p.creator_id, p.assignee_id, p.title, p.description,
       p.reward_flowers, p.reward_sunshine, p.timezone,
       p.deadline_local_date, p.deadline_local_time, p.deadline_at,
       p.create_idempotency_key, p.create_fingerprint,
       GREATEST(r.created_at, p.period_date::timestamp AT TIME ZONE 'Asia/Shanghai'),
       p.completion_mode, p.target_count, p.schedule_type, p.recurrence_id, p.period_date,
       p.period_date = f.ps, NULL::date
  FROM tasks p JOIN tasks r ON r.id = p.recurrence_id JOIN fin f ON f.root_id = r.id
UNION ALL
SELECT r.id, r.couple_id, r.creator_id, r.assignee_id, r.title, r.description,
       r.reward_flowers, r.reward_sunshine, r.timezone,
       f.ps + (f.span - 1), NULL::time, (f.ps + f.span)::timestamp AT TIME ZONE 'Asia/Shanghai',
       r.create_idempotency_key, r.create_fingerprint,
       GREATEST(r.created_at, f.ps::timestamp AT TIME ZONE 'Asia/Shanghai'),
       r.completion_mode, r.target_count, r.schedule_type, r.id, f.ps, true, NULL::date
  FROM fin f JOIN tasks r ON r.id = f.root_id
 WHERE NOT EXISTS (SELECT 1 FROM tasks p WHERE p.recurrence_id = r.id AND p.period_date = f.ps);

REVOKE ALL ON task_read_model FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON task_read_model TO usapp_api;

REVOKE ALL ON task_change_proposals, task_change_decisions, task_closures FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON task_change_proposals, task_change_decisions, task_closures TO usapp_api;

COMMIT;
