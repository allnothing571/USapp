-- =====================================================================
-- 0029_media: Phase 3 P3-1a private media (docs/phase3-plan.md Sections 4 and 21). 0000-0028 stay frozen.
--
-- media_objects             one upload grant: the couple, uploader and exact staging key are fixed here
-- media_promotion_attempts  one server-side copy each, to a final key that is written once and never reused
-- media_upload_outcomes     READY names one attempt, and so one immutable object; REJECTED is terminal
-- media_cleanup_plans       once a plan exists no attempt can start or publish: this is the fence
-- media_cleanup_passes      a first delete, then a sweep after the last late write that can be expected
-- media_bindings            one media object belongs to one target
-- media_late_objects        objects found after their plan was swept, or unknown to the database
--
-- Bytes live only in object storage. Nothing here calls the store. Locks: 33018 per media object, 33019 per couple
-- for grant limits. All tables are append-only, for usapp_api SELECT and INSERT; usapp_push has no access.
-- SQLSTATE: UF001 append-only; UF002 grant; UF003 attempt; UF004 outcome; UF005 plan; UF006 pass; UF007 binding; UF008 late object.
-- =====================================================================

BEGIN;

DO $media_preflight$
BEGIN
    IF to_regproc('public.usapp_reminder_occurrence_valid') IS NULL THEN
        RAISE EXCEPTION '0029_media requires 0028_reminder_push_stale';
    END IF;
END
$media_preflight$;

-- ---------------------------------------------------------------- grants
CREATE TABLE media_objects (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    uploader_id uuid NOT NULL,
    staging_key text NOT NULL,
    declared_type text NOT NULL,
    declared_size bigint NOT NULL,
    expires_at timestamptz NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_objects_pkey PRIMARY KEY (id),
    CONSTRAINT media_objects_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT media_objects_staging_unique UNIQUE (staging_key),
    CONSTRAINT media_objects_key_unique UNIQUE (uploader_id, idempotency_key),
    CONSTRAINT media_objects_uploader_fk FOREIGN KEY (couple_id, uploader_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- Built by the server: no client value reaches an object key.
    CONSTRAINT media_objects_staging_shape CHECK (staging_key = 'staging/' || couple_id::text || '/' || id::text || '/upload'),
    CONSTRAINT media_objects_type_valid CHECK (declared_type IN ('image/jpeg', 'image/png', 'image/webp')),
    CONSTRAINT media_objects_size_valid CHECK (declared_size BETWEEN 1 AND 20000000),
    CONSTRAINT media_objects_expiry_valid CHECK (expires_at > created_at AND expires_at <= created_at + interval '1 hour'),
    CONSTRAINT media_objects_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT media_objects_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX media_objects_couple_idx ON media_objects (couple_id, created_at DESC);
CREATE INDEX media_objects_expiry_idx ON media_objects (expires_at);

-- ---------------------------------------------------------------- promotion
CREATE TABLE media_promotion_attempts (
    media_id uuid NOT NULL,
    attempt_no integer NOT NULL,
    couple_id uuid NOT NULL,
    final_key text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_promotion_attempts_pkey PRIMARY KEY (media_id, attempt_no),
    CONSTRAINT media_promotion_attempts_key_unique UNIQUE (final_key),
    CONSTRAINT media_promotion_attempts_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT media_promotion_attempts_no_valid CHECK (attempt_no BETWEEN 1 AND 5),
    -- Never under staging/, and unique per attempt: nothing else can ever write this key.
    CONSTRAINT media_promotion_attempts_key_shape CHECK (
        final_key ~ ('^media/' || couple_id::text || '/' || media_id::text || '/[0-9a-f-]{36}/original$'))
);

CREATE TABLE media_upload_outcomes (
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    outcome text NOT NULL,
    reason text,
    attempt_no integer,
    real_format text,
    width integer,
    height integer,
    stored_size bigint,
    etag text,
    crc64 text,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_upload_outcomes_pkey PRIMARY KEY (media_id),
    CONSTRAINT media_upload_outcomes_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT media_upload_outcomes_attempt_fk FOREIGN KEY (media_id, attempt_no)
        REFERENCES media_promotion_attempts (media_id, attempt_no) ON DELETE RESTRICT,
    CONSTRAINT media_upload_outcomes_shape CHECK (
        (outcome = 'ready' AND reason IS NULL AND num_nonnulls(attempt_no, real_format, width, height, stored_size, etag) = 6
         AND real_format IN ('image/jpeg', 'image/png', 'image/webp')
         AND width BETWEEN 1 AND 30000 AND height BETWEEN 1 AND 30000 AND width::bigint * height::bigint <= 250000000)
        OR (outcome = 'rejected' AND reason IN ('size_mismatch', 'not_image', 'format_mismatch', 'dimensions', 'attempts_exhausted')))
);

-- ---------------------------------------------------------------- cleanup and bindings
CREATE TABLE media_cleanup_plans (
    media_id uuid NOT NULL,
    scope text NOT NULL,
    reason text NOT NULL,
    sweep_not_before timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_cleanup_plans_pkey PRIMARY KEY (media_id, scope),
    CONSTRAINT media_cleanup_plans_media_fk FOREIGN KEY (media_id) REFERENCES media_objects (id) ON DELETE RESTRICT,
    CONSTRAINT media_cleanup_plans_reason_valid CHECK (
        (scope = 'leftovers' AND reason = 'ready')
        OR (scope = 'everything' AND reason IN ('rejected', 'abandoned', 'unattached')))
);
CREATE INDEX media_cleanup_plans_created_idx ON media_cleanup_plans (created_at);

CREATE TABLE media_cleanup_passes (
    media_id uuid NOT NULL,
    scope text NOT NULL,
    kind text NOT NULL,
    completed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_cleanup_passes_pkey PRIMARY KEY (media_id, scope, kind),
    CONSTRAINT media_cleanup_passes_plan_fk FOREIGN KEY (media_id, scope)
        REFERENCES media_cleanup_plans (media_id, scope) ON DELETE RESTRICT,
    CONSTRAINT media_cleanup_passes_kind_valid CHECK (kind IN ('first', 'sweep'))
);

CREATE TABLE media_bindings (
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    target_kind text NOT NULL,
    target_id uuid NOT NULL,
    bound_by uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_bindings_pkey PRIMARY KEY (media_id),
    CONSTRAINT media_bindings_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT media_bindings_member_fk FOREIGN KEY (couple_id, bound_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT media_bindings_kind_valid CHECK (target_kind IN ('memory'))
);

CREATE TABLE media_late_objects (
    object_key text NOT NULL,
    disposition text NOT NULL,
    source text NOT NULL,
    found_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_late_objects_pkey PRIMARY KEY (object_key, disposition),
    CONSTRAINT media_late_objects_disposition_valid CHECK (disposition IN ('removed', 'report_only')),
    CONSTRAINT media_late_objects_source_valid CHECK (source IN ('attempt_resume', 'reconcile')),
    CONSTRAINT media_late_objects_key_length CHECK (char_length(object_key) BETWEEN 1 AND 1024)
);

-- ---------------------------------------------------------------- append-only
CREATE FUNCTION usapp_media_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $media_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UF001', MESSAGE = 'USAPP_MEDIA_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$media_reject$;
CREATE TRIGGER media_objects_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_objects
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_promotion_attempts_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_promotion_attempts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_upload_outcomes_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_upload_outcomes
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_cleanup_plans_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_cleanup_plans
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_cleanup_passes_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_cleanup_passes
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_bindings_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_bindings
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();
CREATE TRIGGER media_late_objects_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_late_objects
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_reject_mutation();

-- ---------------------------------------------------------------- definitions
-- NULL: acceptable. The one definition for the service's error and the guard. Reads after the caller's 33019 wait.
CREATE FUNCTION usapp_media_grant_problem(p_user uuid, p_couple uuid, p_size bigint) RETURNS text
LANGUAGE plpgsql VOLATILE AS $grant_problem$
BEGIN
    IF (SELECT count(*) FROM media_objects o
         WHERE o.uploader_id = p_user AND o.expires_at > clock_timestamp()
           AND NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = o.id)
           AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id)) >= 20 THEN
        RETURN 'open_grants';
    END IF;
    IF (SELECT count(*) FROM media_objects o
         WHERE o.uploader_id = p_user AND o.created_at > clock_timestamp() - interval '1 day') >= 200 THEN
        RETURN 'daily_grants';
    END IF;
    IF COALESCE((SELECT sum(o.declared_size) FROM media_objects o
                  WHERE o.couple_id = p_couple
                    AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything')), 0)
       + p_size > 21474836480 THEN
        RETURN 'storage';
    END IF;
    RETURN NULL;
END
$grant_problem$;

-- What a key found in the store means. The one definition for the reconciler and a resumed attempt.
--   keep           the READY object: never removed
--   live           no outcome and no plan: an attempt or upload may still be running
--   pending_sweep  dead, and the plan's sweep will remove it
--   late           dead, and the plan's sweep has already run: it appeared afterwards
--   unknown        no row names it: reported, never removed
-- Dead is final: a plan blocks every later attempt and outcome, and nothing removes a plan.
CREATE FUNCTION usapp_media_key_disposition(p_key text) RETURNS text
LANGUAGE plpgsql STABLE AS $key_disposition$
DECLARE
    v_media uuid;
    v_attempt integer;
    v_winner integer;
    v_scope text;
BEGIN
    SELECT a.media_id, a.attempt_no INTO v_media, v_attempt FROM media_promotion_attempts a WHERE a.final_key = p_key;
    IF NOT FOUND THEN
        SELECT o.id INTO v_media FROM media_objects o WHERE o.staging_key = p_key;
        IF NOT FOUND THEN RETURN 'unknown'; END IF;
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = v_media AND p.scope = 'everything') THEN
        v_scope := 'everything';
    ELSE
        SELECT t.attempt_no INTO v_winner FROM media_upload_outcomes t WHERE t.media_id = v_media AND t.outcome = 'ready';
        IF v_attempt IS NOT NULL AND v_attempt = v_winner THEN RETURN 'keep'; END IF;
        IF NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = v_media AND p.scope = 'leftovers') THEN
            RETURN 'live';
        END IF;
        v_scope := 'leftovers';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_passes x WHERE x.media_id = v_media AND x.scope = v_scope AND x.kind = 'sweep') THEN
        RETURN 'late';
    END IF;
    RETURN 'pending_sweep';
END
$key_disposition$;

-- ---------------------------------------------------------------- guards
-- VOLATILE: statements after the advisory-lock waits see the previous holder's commit.
CREATE FUNCTION usapp_media_object_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $object_guard$
DECLARE
    problem text;
BEGIN
    IF NEW.couple_id IS NULL OR NEW.uploader_id IS NULL OR NEW.declared_size IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33019::integer, hashtext(NEW.couple_id::text));
    problem := usapp_media_grant_problem(NEW.uploader_id, NEW.couple_id, NEW.declared_size);
    IF problem IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UF002', MESSAGE = 'USAPP_MEDIA_LIMIT_' || upper(problem);
    END IF;
    IF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UF002', MESSAGE = 'USAPP_MEDIA_TIME';
    END IF;
    RETURN NEW;
END
$object_guard$;
CREATE TRIGGER media_objects_guard BEFORE INSERT ON media_objects
    FOR EACH ROW EXECUTE FUNCTION usapp_media_object_guard();

-- No attempt after an outcome or a plan, or past the completion window. The attempt row commits before the copy, so
-- every key that could exist is known before it can exist.
CREATE FUNCTION usapp_media_attempt_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $attempt_guard$
DECLARE
    m media_objects%ROWTYPE;
BEGIN
    IF NEW.media_id IS NULL OR NEW.attempt_no IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    IF NOT FOUND OR m.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UF003', MESSAGE = 'USAPP_MEDIA_ATTEMPT_TARGET';
    END IF;
    IF EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF003', MESSAGE = 'USAPP_MEDIA_ATTEMPT_DECIDED';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF003', MESSAGE = 'USAPP_MEDIA_ATTEMPT_CLEANUP';
    END IF;
    IF NEW.attempt_no <> 1 + (SELECT COALESCE(max(a.attempt_no), 0) FROM media_promotion_attempts a WHERE a.media_id = NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF003', MESSAGE = 'USAPP_MEDIA_ATTEMPT_ORDER';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < m.created_at
       OR NEW.created_at > m.expires_at + interval '15 minutes' THEN
        RAISE EXCEPTION USING ERRCODE = 'UF003', MESSAGE = 'USAPP_MEDIA_ATTEMPT_WINDOW';
    END IF;
    RETURN NEW;
END
$attempt_guard$;
CREATE TRIGGER media_promotion_attempts_guard BEFORE INSERT ON media_promotion_attempts
    FOR EACH ROW EXECUTE FUNCTION usapp_media_attempt_guard();

-- The fence: once cleanup has started, no attempt can publish anything. READY agrees with the grant.
CREATE FUNCTION usapp_media_outcome_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $outcome_guard$
DECLARE
    m media_objects%ROWTYPE;
BEGIN
    IF NEW.media_id IS NULL OR NEW.decided_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    IF NOT FOUND OR m.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UF004', MESSAGE = 'USAPP_MEDIA_OUTCOME_TARGET';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF004', MESSAGE = 'USAPP_MEDIA_OUTCOME_CLEANUP';
    END IF;
    IF NEW.outcome = 'ready' AND (NEW.real_format IS DISTINCT FROM m.declared_type OR NEW.stored_size IS DISTINCT FROM m.declared_size) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF004', MESSAGE = 'USAPP_MEDIA_OUTCOME_FACTS';
    END IF;
    IF NEW.decided_at > clock_timestamp() OR NEW.decided_at < m.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UF004', MESSAGE = 'USAPP_MEDIA_OUTCOME_TIME';
    END IF;
    RETURN NEW;
END
$outcome_guard$;
CREATE TRIGGER media_upload_outcomes_guard BEFORE INSERT ON media_upload_outcomes
    FOR EACH ROW EXECUTE FUNCTION usapp_media_outcome_guard();

-- Eligibility, repeated here for direct SQL. A bound object can never be planned for deletion.
CREATE FUNCTION usapp_media_plan_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $plan_guard$
DECLARE
    m media_objects%ROWTYPE;
    t media_upload_outcomes%ROWTYPE;
    decided boolean;
BEGIN
    IF NEW.media_id IS NULL OR NEW.scope IS NULL OR NEW.reason IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_TARGET';
    END IF;
    SELECT * INTO t FROM media_upload_outcomes WHERE media_id = NEW.media_id;
    decided := FOUND;
    IF EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = NEW.media_id) AND NEW.scope = 'everything' THEN
        RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_BOUND';
    END IF;
    IF (NEW.reason = 'ready' AND t.outcome IS DISTINCT FROM 'ready')
       OR (NEW.reason = 'rejected' AND t.outcome IS DISTINCT FROM 'rejected')
       OR (NEW.reason = 'abandoned' AND (decided OR clock_timestamp() < m.expires_at + interval '15 minutes'))
       OR (NEW.reason = 'unattached' AND (t.outcome IS DISTINCT FROM 'ready' OR clock_timestamp() < t.decided_at + interval '7 days')) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_ELIGIBILITY';
    END IF;
    RETURN NEW;
END
$plan_guard$;
CREATE TRIGGER media_cleanup_plans_guard BEFORE INSERT ON media_cleanup_plans
    FOR EACH ROW EXECUTE FUNCTION usapp_media_plan_guard();

CREATE FUNCTION usapp_media_pass_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $pass_guard$
DECLARE
    due timestamptz;
BEGIN
    IF NEW.media_id IS NULL OR NEW.scope IS NULL OR NEW.kind IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT p.sweep_not_before INTO due FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = NEW.scope;
    IF NEW.kind = 'sweep' AND (due IS NULL OR clock_timestamp() < due) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF006', MESSAGE = 'USAPP_MEDIA_PASS_EARLY';
    END IF;
    RETURN NEW;
END
$pass_guard$;
CREATE TRIGGER media_cleanup_passes_guard BEFORE INSERT ON media_cleanup_passes
    FOR EACH ROW EXECUTE FUNCTION usapp_media_pass_guard();

-- Attachment takes the same lock as a plan: it either sees the plan and refuses, or commits first and the plan refuses.
CREATE FUNCTION usapp_media_binding_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $binding_guard$
BEGIN
    IF NEW.media_id IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    IF NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = NEW.media_id AND t.outcome = 'ready') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_NOT_READY';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_CLEANUP';
    END IF;
    IF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TIME';
    END IF;
    RETURN NEW;
END
$binding_guard$;
CREATE TRIGGER media_bindings_guard BEFORE INSERT ON media_bindings
    FOR EACH ROW EXECUTE FUNCTION usapp_media_binding_guard();

-- Only a dead key can be recorded as removed, and only a key nothing names as report-only.
CREATE FUNCTION usapp_media_late_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $late_guard$
DECLARE
    state text;
BEGIN
    IF NEW.object_key IS NULL OR NEW.disposition IS NULL THEN
        RETURN NEW;
    END IF;
    state := usapp_media_key_disposition(NEW.object_key);
    IF (NEW.disposition = 'removed' AND state NOT IN ('late', 'pending_sweep'))
       OR (NEW.disposition = 'report_only' AND state <> 'unknown') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF008', MESSAGE = 'USAPP_MEDIA_LATE_STATE';
    END IF;
    RETURN NEW;
END
$late_guard$;
CREATE TRIGGER media_late_objects_guard BEFORE INSERT ON media_late_objects
    FOR EACH ROW EXECUTE FUNCTION usapp_media_late_guard();

REVOKE ALL ON media_objects, media_promotion_attempts, media_upload_outcomes, media_cleanup_plans, media_cleanup_passes,
    media_bindings, media_late_objects FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON media_objects, media_promotion_attempts, media_upload_outcomes, media_cleanup_plans,
    media_cleanup_passes, media_bindings, media_late_objects TO usapp_api;

COMMIT;
