-- M4: one-off counting tasks. Existing tasks remain single-completion tasks.
-- Progress is append-only; the final event and the existing M3 settlement must
-- commit together. No existing migration or wallet guard is changed.
-- Reserved advisory namespace: (33004, hashtext(canonical task UUID)). Hash
-- collisions only serialize unrelated tasks; they do not merge their data.
-- UP001 identity/mode/snapshot; UP002 sequence; UP003 completion link;
-- UP004 decision time; UP005 final operation. Mutation uses existing UT001.
BEGIN;

DO $count_preflight$
BEGIN
    IF to_regclass('public.tasks') IS NULL
       OR to_regclass('public.task_completions') IS NULL THEN
        RAISE EXCEPTION '0008_task_counting requires 0007_tasks_one_off';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_api' AND NOT rolsuper)
       OR NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_push' AND NOT rolsuper) THEN
        RAISE EXCEPTION '0008_task_counting requires non-superuser application roles';
    END IF;
END
$count_preflight$;

ALTER TABLE tasks
    ADD COLUMN completion_mode text NOT NULL DEFAULT 'single',
    ADD COLUMN target_count integer NOT NULL DEFAULT 1,
    ADD CONSTRAINT tasks_completion_mode_valid CHECK (completion_mode IN ('single', 'count')),
    ADD CONSTRAINT tasks_target_count_valid CHECK (
        target_count >= 1 AND (completion_mode = 'count' OR target_count = 1)
    );

CREATE TABLE task_progress_events (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    task_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    user_id uuid NOT NULL,
    progress_count integer NOT NULL,
    target_count integer NOT NULL,
    idempotency_key text NOT NULL,
    operation_id uuid,
    recorded_at timestamptz NOT NULL,
    CONSTRAINT task_progress_events_pkey PRIMARY KEY (id),
    CONSTRAINT task_progress_events_sequence_unique UNIQUE (task_id, progress_count),
    CONSTRAINT task_progress_events_key_unique UNIQUE (user_id, idempotency_key),
    CONSTRAINT task_progress_events_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_progress_events_member_fk FOREIGN KEY (couple_id, user_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT task_progress_events_operation_fk FOREIGN KEY (operation_id, couple_id)
        REFERENCES wallet_operations (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT task_progress_events_bounds CHECK (progress_count BETWEEN 1 AND target_count),
    CONSTRAINT task_progress_events_key_shape CHECK (
        idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'
    ),
    CONSTRAINT task_progress_events_final_operation CHECK (
        (operation_id IS NOT NULL) = (progress_count = target_count)
    )
);

CREATE TRIGGER task_progress_events_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON task_progress_events
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reject_mutation();

-- VOLATILE is intentional: queries after the advisory-lock wait get fresh
-- READ COMMITTED snapshots, including a preceding initializer's committed event.
CREATE FUNCTION usapp_task_progress_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $progress_guard$
DECLARE
    t tasks%ROWTYPE;
    o wallet_operations%ROWTYPE;
    last_count integer;
    last_time timestamptz;
BEGIN
    IF NEW.task_id IS NULL OR NEW.couple_id IS NULL OR NEW.user_id IS NULL
       OR NEW.progress_count IS NULL OR NEW.target_count IS NULL
       OR NEW.recorded_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(NEW.task_id::text));
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.completion_mode IS DISTINCT FROM 'count'
       OR t.couple_id IS DISTINCT FROM NEW.couple_id
       OR t.assignee_id IS DISTINCT FROM NEW.user_id
       OR t.target_count IS DISTINCT FROM NEW.target_count THEN
        RAISE EXCEPTION USING ERRCODE = 'UP001', MESSAGE = 'USAPP_PROGRESS_IDENTITY';
    END IF;
    SELECT p.progress_count, p.recorded_at INTO last_count, last_time
      FROM task_progress_events p WHERE p.task_id = NEW.task_id
      ORDER BY p.progress_count DESC LIMIT 1;
    IF NEW.progress_count::bigint <> COALESCE(last_count, 0)::bigint + 1
       OR NEW.progress_count > t.target_count
       OR EXISTS (SELECT 1 FROM task_completions WHERE task_id = NEW.task_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UP002', MESSAGE = 'USAPP_PROGRESS_SEQUENCE';
    END IF;
    IF NEW.recorded_at > clock_timestamp() OR NEW.recorded_at < t.created_at
       OR (last_time IS NOT NULL AND NEW.recorded_at < last_time)
       OR (t.deadline_at IS NOT NULL AND NEW.recorded_at >= t.deadline_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UP004', MESSAGE = 'USAPP_PROGRESS_TIME';
    END IF;
    IF NEW.operation_id IS NOT NULL THEN
        SELECT * INTO o FROM wallet_operations WHERE id = NEW.operation_id;
        IF NOT FOUND OR o.operation_type IS DISTINCT FROM 'TASK_REWARD'
           OR o.source_type IS DISTINCT FROM 'task' OR o.source_id IS DISTINCT FROM t.id::text
           OR o.actor_user_id IS DISTINCT FROM NEW.user_id
           OR o.couple_id IS DISTINCT FROM NEW.couple_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UP005', MESSAGE = 'USAPP_PROGRESS_OPERATION';
        END IF;
    END IF;
    RETURN NEW;
END
$progress_guard$;

CREATE TRIGGER task_progress_events_guard BEFORE INSERT ON task_progress_events
    FOR EACH ROW EXECUTE FUNCTION usapp_task_progress_guard();

-- The old completion route and direct SQL cannot bypass a counting target.
CREATE FUNCTION usapp_task_count_completion_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $count_completion_guard$
DECLARE
    t tasks%ROWTYPE;
    p task_progress_events%ROWTYPE;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.completion_mode <> 'count' THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33004::integer, hashtext(t.id::text));
    SELECT * INTO p FROM task_progress_events WHERE task_id = t.id
        ORDER BY progress_count DESC LIMIT 1;
    IF NOT FOUND OR p.progress_count IS DISTINCT FROM t.target_count
       OR p.operation_id IS DISTINCT FROM NEW.operation_id
       OR p.recorded_at IS DISTINCT FROM NEW.completed_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UP003', MESSAGE = 'USAPP_COUNT_COMPLETION_LINK';
    END IF;
    RETURN NEW;
END
$count_completion_guard$;

CREATE TRIGGER task_completions_count_guard BEFORE INSERT ON task_completions
    FOR EACH ROW EXECUTE FUNCTION usapp_task_count_completion_guard();

CREATE FUNCTION usapp_task_count_consistency_check()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $count_consistency$
DECLARE
    t tasks%ROWTYPE;
    p task_progress_events%ROWTYPE;
    c task_completions%ROWTYPE;
    has_completion boolean;
BEGIN
    SELECT * INTO t FROM tasks WHERE id = NEW.task_id;
    IF NOT FOUND OR t.completion_mode <> 'count' THEN RETURN NULL; END IF;
    SELECT * INTO p FROM task_progress_events WHERE task_id = t.id
        ORDER BY progress_count DESC LIMIT 1;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UP003', MESSAGE = 'USAPP_COUNT_PROGRESS_MISSING';
    END IF;
    SELECT * INTO c FROM task_completions WHERE task_id = t.id;
    has_completion := FOUND;
    IF (p.progress_count = t.target_count) IS DISTINCT FROM has_completion
       OR (has_completion AND (p.operation_id IS DISTINCT FROM c.operation_id
                            OR p.recorded_at IS DISTINCT FROM c.completed_at)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UP003', MESSAGE = 'USAPP_COUNT_SETTLEMENT_REQUIRED';
    END IF;
    RETURN NULL;
END
$count_consistency$;

CREATE CONSTRAINT TRIGGER task_progress_events_count_valid AFTER INSERT ON task_progress_events
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_count_consistency_check();
CREATE CONSTRAINT TRIGGER task_completions_count_valid AFTER INSERT ON task_completions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_task_count_consistency_check();

REVOKE ALL ON task_progress_events FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON task_progress_events TO usapp_api;

COMMIT;
