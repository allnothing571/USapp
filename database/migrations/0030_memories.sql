-- =====================================================================
-- 0030_memories: Phase 3 P3-1b photo memories (docs/phase3-plan.md Sections 7, 10 and 22). 0000-0029 stay frozen.
--
-- memories                 immutable identity
-- memory_texts             title and description, referenced by revisions and proposals. Held apart so that the deletion
--                          decision (D6) can act on text alone. Append-only like everything else here: no delete path yet
-- memory_revisions         create, creator edit, approved proposal; the highest version is current
-- memory_revision_photos   each revision's ordered photos; history keeps its references
-- memory_edit_proposals    a partner's full replacement against the current version (V1 section 30, as 0018)
-- memory_proposal_photos   the proposed photos: a reference, not a binding
-- memory_edit_decisions    one per proposal: approved, rejected, withdrawn
--
-- Photo references. A photo is BOUND (media_bindings) once any revision uses it, for good: a bound object is never eligible
-- for cleanup, so history stays readable. A pending proposal RESERVES its unbound photos: they cannot be planned for cleanup
-- or used elsewhere, but are not bound. Approval binds them, in the revision's transaction. A rejection or withdrawal
-- releases them simply by deciding the proposal, and the unattached period restarts from that decision.
--
-- Replaces two 0029 function bodies: the plan guard (pending references and the release clock) and the binding guard (the
-- target must be a memory of the same couple, and the actor entitled).
-- Lock order: member FOR SHARE -> 33020 memory -> sorted 33018 media. Completion and cleanup hold only 33018 and never 33020.
-- SQLSTATE: UG001 append-only; UG002 revision; UG003 photo; UG004 proposal; UG005 decision; UG006 deferred links.
-- =====================================================================

BEGIN;

DO $memory_preflight$
BEGIN
    IF to_regclass('public.media_bindings') IS NULL THEN
        RAISE EXCEPTION '0030_memories requires 0029_media';
    END IF;
END
$memory_preflight$;

CREATE TABLE memories (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    creator_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memories_pkey PRIMARY KEY (id),
    CONSTRAINT memories_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT memories_creator_fk FOREIGN KEY (couple_id, creator_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT
);
CREATE INDEX memories_couple_idx ON memories (couple_id, created_at DESC, id DESC);

CREATE TABLE memory_texts (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    title text NOT NULL,
    description text NOT NULL DEFAULT '',
    CONSTRAINT memory_texts_pkey PRIMARY KEY (id),
    CONSTRAINT memory_texts_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT memory_texts_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 60
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT memory_texts_description_shape CHECK (
        char_length(description) <= 2000
        AND description = btrim(description, E' \n')
        AND description !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    )
);

CREATE TABLE memory_revisions (
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    version integer NOT NULL,
    actor_id uuid NOT NULL,
    action text NOT NULL,
    proposal_id uuid,
    memory_date date NOT NULL,
    text_id uuid NOT NULL,
    idempotency_key text,
    request_fingerprint text,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_revisions_pkey PRIMARY KEY (memory_id, version),
    CONSTRAINT memory_revisions_key_unique UNIQUE (actor_id, idempotency_key),
    CONSTRAINT memory_revisions_proposal_unique UNIQUE (proposal_id),
    CONSTRAINT memory_revisions_identity_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_revisions_member_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_revisions_text_fk FOREIGN KEY (text_id, couple_id)
        REFERENCES memory_texts (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_revisions_version_valid CHECK (version >= 1),
    CONSTRAINT memory_revisions_action_valid CHECK (action IN ('create', 'edit', 'approve')),
    CONSTRAINT memory_revisions_create_first CHECK ((action = 'create') = (version = 1)),
    CONSTRAINT memory_revisions_approve_link CHECK ((action = 'approve') = (proposal_id IS NOT NULL)),
    CONSTRAINT memory_revisions_key_presence CHECK (
        (action = 'approve') = (idempotency_key IS NULL)
        AND (idempotency_key IS NULL) = (request_fingerprint IS NULL)
    ),
    CONSTRAINT memory_revisions_date_range CHECK (memory_date >= DATE '1900-01-01'),
    CONSTRAINT memory_revisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_revisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_revisions_timeline_idx ON memory_revisions (couple_id, memory_date DESC);

CREATE TABLE memory_revision_photos (
    memory_id uuid NOT NULL,
    version integer NOT NULL,
    position integer NOT NULL,
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    CONSTRAINT memory_revision_photos_pkey PRIMARY KEY (memory_id, version, position),
    CONSTRAINT memory_revision_photos_once UNIQUE (memory_id, version, media_id),
    CONSTRAINT memory_revision_photos_revision_fk FOREIGN KEY (memory_id, version)
        REFERENCES memory_revisions (memory_id, version) ON DELETE RESTRICT,
    CONSTRAINT memory_revision_photos_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    -- Database-level same couple.
    CONSTRAINT memory_revision_photos_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_revision_photos_position_valid CHECK (position BETWEEN 1 AND 9)
);
CREATE INDEX memory_revision_photos_media_idx ON memory_revision_photos (media_id);

CREATE TABLE memory_edit_proposals (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    proposer_id uuid NOT NULL,
    base_version integer NOT NULL,
    memory_date date NOT NULL,
    text_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_edit_proposals_pkey PRIMARY KEY (id),
    CONSTRAINT memory_edit_proposals_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT memory_edit_proposals_key_unique UNIQUE (proposer_id, idempotency_key),
    -- V1 section 30 "original content": the immutable base revision.
    CONSTRAINT memory_edit_proposals_base_fk FOREIGN KEY (memory_id, base_version)
        REFERENCES memory_revisions (memory_id, version) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_proposals_identity_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_proposals_member_fk FOREIGN KEY (couple_id, proposer_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_proposals_text_fk FOREIGN KEY (text_id, couple_id)
        REFERENCES memory_texts (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_proposals_date_range CHECK (memory_date >= DATE '1900-01-01'),
    CONSTRAINT memory_edit_proposals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_edit_proposals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_edit_proposals_memory_idx ON memory_edit_proposals (memory_id, created_at DESC, id DESC);

ALTER TABLE memory_revisions
    ADD CONSTRAINT memory_revisions_proposal_fk FOREIGN KEY (proposal_id)
        REFERENCES memory_edit_proposals (id) ON DELETE RESTRICT;

CREATE TABLE memory_proposal_photos (
    proposal_id uuid NOT NULL,
    position integer NOT NULL,
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    CONSTRAINT memory_proposal_photos_pkey PRIMARY KEY (proposal_id, position),
    CONSTRAINT memory_proposal_photos_once UNIQUE (proposal_id, media_id),
    CONSTRAINT memory_proposal_photos_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES memory_edit_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_proposal_photos_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_proposal_photos_position_valid CHECK (position BETWEEN 1 AND 9)
);
CREATE INDEX memory_proposal_photos_media_idx ON memory_proposal_photos (media_id);

CREATE TABLE memory_edit_decisions (
    proposal_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    decided_by uuid NOT NULL,
    outcome text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- One decision per proposal: approve, reject and withdraw race on this key.
    CONSTRAINT memory_edit_decisions_pkey PRIMARY KEY (proposal_id),
    CONSTRAINT memory_edit_decisions_key_unique UNIQUE (decided_by, idempotency_key),
    CONSTRAINT memory_edit_decisions_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES memory_edit_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_decisions_member_fk FOREIGN KEY (couple_id, decided_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_edit_decisions_outcome_valid CHECK (outcome IN ('approved', 'rejected', 'withdrawn')),
    CONSTRAINT memory_edit_decisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_edit_decisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

-- ---------------------------------------------------------------- append-only
CREATE FUNCTION usapp_memory_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $memory_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UG001', MESSAGE = 'USAPP_MEMORY_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$memory_reject$;
CREATE TRIGGER memories_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memories
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_texts_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_texts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_revisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_revision_photos_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_revision_photos
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_edit_proposals_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_edit_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_proposal_photos_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_proposal_photos
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_edit_decisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_edit_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();

-- ---------------------------------------------------------------- media reference definitions
CREATE FUNCTION usapp_media_pending_reference(p_media uuid) RETURNS uuid
LANGUAGE sql STABLE AS $pending_reference$
    SELECT x.proposal_id FROM memory_proposal_photos x
     WHERE x.media_id = p_media
       AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = x.proposal_id)
     LIMIT 1
$pending_reference$;

-- When a READY object became unreferenced: its own READY time, or the decision that last released it. NULL while it is
-- bound, reserved or not READY, which is what keeps it out of unattached cleanup. The one definition for the guard and the job.
CREATE FUNCTION usapp_media_released_at(p_media uuid) RETURNS timestamptz
LANGUAGE sql STABLE AS $released_at$
    SELECT GREATEST(t.decided_at, (SELECT max(d.decided_at) FROM memory_proposal_photos x
                                     JOIN memory_edit_decisions d ON d.proposal_id = x.proposal_id
                                    WHERE x.media_id = p_media))
      FROM media_upload_outcomes t
     WHERE t.media_id = p_media AND t.outcome = 'ready'
       AND NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = p_media)
       AND usapp_media_pending_reference(p_media) IS NULL
$released_at$;

-- The 0029 body with two changes: a reserved object cannot be planned, and unattached counts from its release.
CREATE OR REPLACE FUNCTION usapp_media_plan_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $plan_guard$
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
    IF usapp_media_pending_reference(NEW.media_id) IS NOT NULL AND NEW.scope = 'everything' THEN
        RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_RESERVED';
    END IF;
    IF (NEW.reason = 'ready' AND t.outcome IS DISTINCT FROM 'ready')
       OR (NEW.reason = 'rejected' AND t.outcome IS DISTINCT FROM 'rejected')
       OR (NEW.reason = 'abandoned' AND (decided OR clock_timestamp() < m.expires_at + interval '15 minutes'))
       OR (NEW.reason = 'unattached' AND (t.outcome IS DISTINCT FROM 'ready'
           OR COALESCE(clock_timestamp() < usapp_media_released_at(NEW.media_id) + interval '7 days', true))) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_ELIGIBILITY';
    END IF;
    RETURN NEW;
END
$plan_guard$;

-- The 0029 body plus: the target is a memory of this couple; the actor uploaded the object or approved it onto this memory;
-- and it is not reserved by a pending proposal on another memory.
CREATE OR REPLACE FUNCTION usapp_media_binding_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $binding_guard$
DECLARE
    m media_objects%ROWTYPE;
    reserved uuid;
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
    IF NOT EXISTS (SELECT 1 FROM memories x WHERE x.id = NEW.target_id AND x.couple_id = NEW.couple_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
    END IF;
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    IF m.uploader_id IS DISTINCT FROM NEW.bound_by
       AND NOT EXISTS (SELECT 1 FROM memory_proposal_photos x
                         JOIN memory_edit_proposals p ON p.id = x.proposal_id
                         JOIN memory_edit_decisions d ON d.proposal_id = p.id
                        WHERE x.media_id = NEW.media_id AND p.memory_id = NEW.target_id
                          AND d.outcome = 'approved' AND d.decided_by = NEW.bound_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_ACTOR';
    END IF;
    reserved := usapp_media_pending_reference(NEW.media_id);
    IF reserved IS NOT NULL AND NOT EXISTS (SELECT 1 FROM memory_edit_proposals p WHERE p.id = reserved AND p.memory_id = NEW.target_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_RESERVED';
    END IF;
    RETURN NEW;
END
$binding_guard$;

-- ---------------------------------------------------------------- memory guards
CREATE FUNCTION usapp_memory_revision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_guard$
DECLARE
    x memories%ROWTYPE;
    prev memory_revisions%ROWTYPE;
    has_prev boolean;
    p memory_edit_proposals%ROWTYPE;
BEGIN
    IF NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.version IS NULL OR NEW.actor_id IS NULL
       OR NEW.action IS NULL OR NEW.created_at IS NULL OR NEW.memory_date IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO x FROM memories WHERE id = NEW.memory_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.actor_id IS DISTINCT FROM x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_ACTOR';
    END IF;
    SELECT * INTO prev FROM memory_revisions WHERE memory_id = NEW.memory_id ORDER BY version DESC LIMIT 1;
    has_prev := FOUND;
    IF NEW.version::bigint <> COALESCE(prev.version, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_VERSION';
    END IF;
    IF NEW.action = 'edit' AND EXISTS (
        SELECT 1 FROM memory_edit_proposals q WHERE q.memory_id = NEW.memory_id
           AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_PROPOSAL_PENDING';
    END IF;
    IF NEW.action = 'approve' THEN
        SELECT * INTO p FROM memory_edit_proposals WHERE id = NEW.proposal_id;
        IF NOT FOUND OR p.memory_id IS DISTINCT FROM NEW.memory_id OR p.base_version IS DISTINCT FROM prev.version
           OR NOT EXISTS (SELECT 1 FROM memory_edit_decisions d
                           WHERE d.proposal_id = p.id AND d.outcome = 'approved' AND d.decided_by = NEW.actor_id)
           OR ROW(NEW.memory_date, NEW.text_id) IS DISTINCT FROM ROW(p.memory_date, p.text_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_APPROVAL';
        END IF;
    ELSIF NEW.memory_date > (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_DATE';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < x.created_at
       OR (has_prev AND NEW.created_at < prev.created_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_TIME';
    END IF;
    RETURN NEW;
END
$revision_guard$;
CREATE TRIGGER memory_revisions_guard BEFORE INSERT ON memory_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_revision_guard();

-- A revision's photos arrive in its own transaction, and each is bound to this memory: READY, unplanned and the couple's own
-- are all established by the binding guard. So a revision cannot reference an object cleanup could take.
CREATE FUNCTION usapp_memory_revision_photo_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_photo_guard$
BEGIN
    IF NEW.memory_id IS NULL OR NEW.version IS NULL OR NEW.media_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    IF NOT EXISTS (SELECT 1 FROM memory_revisions r WHERE r.memory_id = NEW.memory_id AND r.version = NEW.version
                      AND r.xmin = pg_current_xact_id()::xid) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_REVISION';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = NEW.media_id AND b.couple_id = NEW.couple_id
                      AND b.target_kind = 'memory' AND b.target_id = NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_UNBOUND';
    END IF;
    RETURN NEW;
END
$revision_photo_guard$;
CREATE TRIGGER memory_revision_photos_guard BEFORE INSERT ON memory_revision_photos
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_revision_photo_guard();

-- Partner only, against the current version, at most one pending proposal per memory.
CREATE FUNCTION usapp_memory_proposal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $proposal_guard$
DECLARE
    x memories%ROWTYPE;
    current_version integer;
BEGIN
    IF NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.proposer_id IS NULL
       OR NEW.base_version IS NULL OR NEW.created_at IS NULL OR NEW.memory_date IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO x FROM memories WHERE id = NEW.memory_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.proposer_id = x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_PROPOSER';
    END IF;
    SELECT max(version) INTO current_version FROM memory_revisions WHERE memory_id = NEW.memory_id;
    IF NEW.base_version IS DISTINCT FROM current_version THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_PROPOSAL_BASE';
    END IF;
    IF EXISTS (SELECT 1 FROM memory_edit_proposals q WHERE q.memory_id = NEW.memory_id
                  AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_PROPOSAL_PENDING';
    END IF;
    IF NEW.memory_date > (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_DATE';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < x.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_PROPOSAL_TIME';
    END IF;
    RETURN NEW;
END
$proposal_guard$;
CREATE TRIGGER memory_edit_proposals_guard BEFORE INSERT ON memory_edit_proposals
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_proposal_guard();

-- A proposed photo is already this memory's, or is the proposer's own, READY, unplanned, unbound and not reserved elsewhere.
CREATE FUNCTION usapp_memory_proposal_photo_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $proposal_photo_guard$
DECLARE
    p memory_edit_proposals%ROWTYPE;
    m media_objects%ROWTYPE;
    b media_bindings%ROWTYPE;
    reserved uuid;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.media_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO p FROM memory_edit_proposals WHERE id = NEW.proposal_id AND xmin = pg_current_xact_id()::xid;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_PROPOSAL';
    END IF;
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    SELECT * INTO b FROM media_bindings WHERE media_id = NEW.media_id;
    IF FOUND THEN
        IF b.target_id IS DISTINCT FROM p.memory_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_ELSEWHERE';
        END IF;
        RETURN NEW;
    END IF;
    IF m.uploader_id IS DISTINCT FROM p.proposer_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_OWNER';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = NEW.media_id AND t.outcome = 'ready')
       OR EXISTS (SELECT 1 FROM media_cleanup_plans c WHERE c.media_id = NEW.media_id AND c.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_UNAVAILABLE';
    END IF;
    SELECT x.proposal_id INTO reserved FROM memory_proposal_photos x
     WHERE x.media_id = NEW.media_id AND x.proposal_id <> NEW.proposal_id
       AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = x.proposal_id) LIMIT 1;
    IF FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_RESERVED';
    END IF;
    RETURN NEW;
END
$proposal_photo_guard$;
CREATE TRIGGER memory_proposal_photos_guard BEFORE INSERT ON memory_proposal_photos
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_proposal_photo_guard();

-- approved/rejected: the creator; withdrawn: the proposer; approval only against the current version.
CREATE FUNCTION usapp_memory_decision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $decision_guard$
DECLARE
    p memory_edit_proposals%ROWTYPE;
    x memories%ROWTYPE;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL
       OR NEW.outcome IS NULL OR NEW.decided_at IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO p FROM memory_edit_proposals WHERE id = NEW.proposal_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DECISION_TARGET';
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(p.memory_id::text));
    SELECT * INTO x FROM memories WHERE id = p.memory_id;
    IF (NEW.outcome IN ('approved', 'rejected') AND NEW.decided_by IS DISTINCT FROM x.creator_id)
       OR (NEW.outcome = 'withdrawn' AND NEW.decided_by IS DISTINCT FROM p.proposer_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DECISION_ACTOR';
    END IF;
    IF NEW.outcome = 'approved'
       AND p.base_version IS DISTINCT FROM (SELECT max(r.version) FROM memory_revisions r WHERE r.memory_id = p.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DECISION_STALE';
    END IF;
    IF NEW.decided_at > clock_timestamp() OR NEW.decided_at < p.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DECISION_TIME';
    END IF;
    RETURN NEW;
END
$decision_guard$;
CREATE TRIGGER memory_edit_decisions_guard BEFORE INSERT ON memory_edit_decisions
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_decision_guard();

-- ---------------------------------------------------------------- deferred links, checked at commit
-- 1 to 9 photos at contiguous positions.
CREATE FUNCTION usapp_memory_photos_valid(n bigint, top integer) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $photos_valid$
    SELECT n BETWEEN 1 AND 9 AND top = n
$photos_valid$;

CREATE FUNCTION usapp_memory_link_check() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $memory_link$
BEGIN
    IF TG_TABLE_NAME = 'memories' THEN
        IF NOT EXISTS (SELECT 1 FROM memory_revisions r WHERE r.memory_id = NEW.id AND r.version = 1 AND r.action = 'create') THEN
            RAISE EXCEPTION USING ERRCODE = 'UG006', MESSAGE = 'USAPP_MEMORY_CREATE_REVISION';
        END IF;
    ELSIF TG_TABLE_NAME = 'memory_revisions' THEN
        IF NOT (SELECT usapp_memory_photos_valid(count(*), max(x.position)) FROM memory_revision_photos x
                 WHERE x.memory_id = NEW.memory_id AND x.version = NEW.version) THEN
            RAISE EXCEPTION USING ERRCODE = 'UG006', MESSAGE = 'USAPP_MEMORY_REVISION_PHOTOS';
        END IF;
    ELSIF TG_TABLE_NAME = 'memory_edit_proposals' THEN
        IF NOT (SELECT usapp_memory_photos_valid(count(*), max(x.position)) FROM memory_proposal_photos x
                 WHERE x.proposal_id = NEW.id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UG006', MESSAGE = 'USAPP_MEMORY_PROPOSAL_PHOTOS';
        END IF;
    ELSIF NEW.outcome = 'approved' THEN
        IF (SELECT count(*) FROM memory_revisions r WHERE r.proposal_id = NEW.proposal_id AND r.action = 'approve') <> 1 THEN
            RAISE EXCEPTION USING ERRCODE = 'UG006', MESSAGE = 'USAPP_MEMORY_APPROVAL_REVISION';
        END IF;
    END IF;
    RETURN NULL;
END
$memory_link$;
CREATE CONSTRAINT TRIGGER memories_create_revision AFTER INSERT ON memories
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_memory_link_check();
CREATE CONSTRAINT TRIGGER memory_revisions_photos AFTER INSERT ON memory_revisions
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_memory_link_check();
CREATE CONSTRAINT TRIGGER memory_edit_proposals_photos AFTER INSERT ON memory_edit_proposals
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_memory_link_check();
CREATE CONSTRAINT TRIGGER memory_edit_decisions_approval_revision AFTER INSERT ON memory_edit_decisions
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN (NEW.outcome = 'approved')
    EXECUTE FUNCTION usapp_memory_link_check();

REVOKE ALL ON memories, memory_texts, memory_revisions, memory_revision_photos, memory_edit_proposals,
    memory_proposal_photos, memory_edit_decisions FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON memories, memory_texts, memory_revisions, memory_revision_photos, memory_edit_proposals,
    memory_proposal_photos, memory_edit_decisions TO usapp_api;

COMMIT;
