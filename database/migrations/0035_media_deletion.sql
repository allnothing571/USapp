-- =====================================================================
-- 0035_media_deletion: Phase 3 D6 first backend batch (docs/phase3-d6-media-deletion-design.md, docs/phase3-plan.md Section 4.8).
-- 0000-0034 stay frozen.
--
-- media_deletions           one deletion cycle of one READY photo: its uploader, its deadline, its key. Never changes
-- media_deletion_reversals  the original deleter's undo of that exact cycle, strictly before its deadline
--
-- A photo with an unreversed cycle is deleted now: no direct read, no new signed URL, no new attachment, no approval. Its binding
-- and its place in history stay. A purge plan is an ordinary 'everything' cleanup plan with the separate reason 'user_deleted',
-- which names the exact cycle: from its commit the cycle cannot be undone, and the existing sweep and late-object rules apply.
--
-- Adds one nullable column and a wider CHECK to media_cleanup_plans. Replaces five function bodies: the plan guard (0030), the
-- binding guard (0031), the memory revision and proposal photo guards (0030) and the album cover guard (0031). Replaces the release
-- clock (0030), so that a deleted photo is out of unattached cleanup and an undo restarts its period.
-- usapp_media_key_disposition, usapp_media_shared_now and every other guard are unchanged.
--
-- Lock order: member FOR SHARE -> 33025 request key -> 33018 media. Memories and albums keep their domain lock first, then sorted
-- 33018. The cleanup job holds only 33018. No path takes 33020, 33021 or 33025 after 33018.
-- SQLSTATE: UL001 append-only; UL002 deletion; UL003 reversal. The replaced guards keep UF005, UF007, UG003 and UH005.
-- =====================================================================

BEGIN;

DO $deletion_preflight$
BEGIN
    IF to_regclass('public.shared_plan_memory_links') IS NULL THEN
        RAISE EXCEPTION '0035_media_deletion requires 0034_shared_plan_memory';
    END IF;
END
$deletion_preflight$;

CREATE TABLE media_deletions (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    deleted_by uuid NOT NULL,
    cycle_no integer NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    deleted_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    purge_after timestamptz NOT NULL,
    CONSTRAINT media_deletions_pkey PRIMARY KEY (id),
    CONSTRAINT media_deletions_id_media_unique UNIQUE (id, media_id),
    CONSTRAINT media_deletions_cycle_unique UNIQUE (media_id, cycle_no),
    CONSTRAINT media_deletions_key_unique UNIQUE (deleted_by, idempotency_key),
    CONSTRAINT media_deletions_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT media_deletions_member_fk FOREIGN KEY (couple_id, deleted_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT media_deletions_cycle_valid CHECK (cycle_no >= 1),
    -- The one deadline: hours, so a calendar never moves it.
    CONSTRAINT media_deletions_window_valid CHECK (purge_after = deleted_at + interval '168 hours'),
    CONSTRAINT media_deletions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT media_deletions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX media_deletions_due_idx ON media_deletions (purge_after);

CREATE TABLE media_deletion_reversals (
    deletion_id uuid NOT NULL,
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    reversed_by uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    reversed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT media_deletion_reversals_pkey PRIMARY KEY (deletion_id),
    CONSTRAINT media_deletion_reversals_key_unique UNIQUE (reversed_by, idempotency_key),
    CONSTRAINT media_deletion_reversals_deletion_fk FOREIGN KEY (deletion_id, media_id)
        REFERENCES media_deletions (id, media_id) ON DELETE RESTRICT,
    CONSTRAINT media_deletion_reversals_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT media_deletion_reversals_member_fk FOREIGN KEY (couple_id, reversed_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT media_deletion_reversals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT media_deletion_reversals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX media_deletion_reversals_media_idx ON media_deletion_reversals (media_id);

CREATE FUNCTION usapp_media_deletion_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $deletion_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UL001', MESSAGE = 'USAPP_MEDIA_DELETION_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$deletion_reject$;
CREATE TRIGGER media_deletions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_deletions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_deletion_reject_mutation();
CREATE TRIGGER media_deletion_reversals_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON media_deletion_reversals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_media_deletion_reject_mutation();

-- ---------------------------------------------------------------- purge plans
ALTER TABLE media_cleanup_plans ADD COLUMN deletion_id uuid;
ALTER TABLE media_cleanup_plans
    ADD CONSTRAINT media_cleanup_plans_deletion_fk FOREIGN KEY (deletion_id, media_id)
        REFERENCES media_deletions (id, media_id) ON DELETE RESTRICT;
ALTER TABLE media_cleanup_plans DROP CONSTRAINT media_cleanup_plans_reason_valid;
ALTER TABLE media_cleanup_plans ADD CONSTRAINT media_cleanup_plans_reason_valid CHECK (
    (scope = 'leftovers' AND reason = 'ready' AND deletion_id IS NULL)
    OR (scope = 'everything' AND reason IN ('rejected', 'abandoned', 'unattached') AND deletion_id IS NULL)
    OR (scope = 'everything' AND reason = 'user_deleted' AND deletion_id IS NOT NULL));

-- ---------------------------------------------------------------- definitions
-- The one boundary. Undo is open strictly before the deadline; at the deadline it is closed and a purge may be planned.
-- Takes the time as an argument so that the exact instant can be tested. Every rule passes clock_timestamp().
CREATE FUNCTION usapp_media_undo_open(p_purge_after timestamptz, p_now timestamptz) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $undo_open$
    SELECT p_now < p_purge_after
$undo_open$;

-- The photo's unreversed cycle, if it has one. There is at most one: the deletion guard refuses a second.
CREATE FUNCTION usapp_media_active_deletion(p_media uuid) RETURNS uuid
LANGUAGE sql STABLE AS $active_deletion$
    SELECT d.id FROM media_deletions d
     WHERE d.media_id = p_media
       AND NOT EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = d.id)
     LIMIT 1
$active_deletion$;

CREATE FUNCTION usapp_media_deleted_now(p_media uuid) RETURNS boolean
LANGUAGE sql STABLE AS $deleted_now$
    SELECT usapp_media_active_deletion(p_media) IS NOT NULL
$deleted_now$;

-- The 0030 clock with two changes: NULL while deleted, and an undo restarts the period, so that a restored photo is not cleaned
-- up by the first tick after its undo. The job's own query reads this, so it needs no change to skip a deleted photo.
CREATE OR REPLACE FUNCTION usapp_media_released_at(p_media uuid) RETURNS timestamptz
LANGUAGE sql STABLE AS $released_at$
    SELECT GREATEST(t.decided_at,
                    (SELECT max(d.decided_at) FROM memory_proposal_photos x
                       JOIN memory_edit_decisions d ON d.proposal_id = x.proposal_id
                      WHERE x.media_id = p_media),
                    (SELECT max(r.reversed_at) FROM media_deletion_reversals r WHERE r.media_id = p_media))
      FROM media_upload_outcomes t
     WHERE t.media_id = p_media AND t.outcome = 'ready'
       AND NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = p_media)
       AND usapp_media_pending_reference(p_media) IS NULL
       AND NOT usapp_media_deleted_now(p_media)
$released_at$;

-- ---------------------------------------------------------------- deletion guards. VOLATILE: statements after the lock see its holder's commit.
-- The uploader's own READY photo, with nothing already taking it away. Binding and reservation do not matter here.
CREATE FUNCTION usapp_media_deletion_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $deletion_guard$
DECLARE
    m media_objects%ROWTYPE;
    t media_upload_outcomes%ROWTYPE;
BEGIN
    IF NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.deleted_by IS NULL OR NEW.cycle_no IS NULL OR NEW.deleted_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    IF NOT FOUND OR m.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_TARGET';
    END IF;
    IF m.uploader_id IS DISTINCT FROM NEW.deleted_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_ACTOR';
    END IF;
    SELECT * INTO t FROM media_upload_outcomes WHERE media_id = NEW.media_id AND outcome = 'ready';
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_NOT_READY';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_CLEANUP';
    END IF;
    IF usapp_media_deleted_now(NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_ACTIVE';
    END IF;
    IF NEW.cycle_no::bigint <> 1 + COALESCE((SELECT max(d.cycle_no) FROM media_deletions d WHERE d.media_id = NEW.media_id), 0)::bigint THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_CYCLE';
    END IF;
    IF NEW.deleted_at > clock_timestamp() OR NEW.deleted_at < t.decided_at
       OR NEW.deleted_at < COALESCE((SELECT max(r.reversed_at) FROM media_deletion_reversals r WHERE r.media_id = NEW.media_id), NEW.deleted_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UL002', MESSAGE = 'USAPP_MEDIA_DELETION_TIME';
    END IF;
    RETURN NEW;
END
$deletion_guard$;
CREATE TRIGGER media_deletions_guard BEFORE INSERT ON media_deletions
    FOR EACH ROW EXECUTE FUNCTION usapp_media_deletion_guard();

-- The original deleter, this exact cycle, on the database clock, and no purge plan.
CREATE FUNCTION usapp_media_reversal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $reversal_guard$
DECLARE
    d media_deletions%ROWTYPE;
BEGIN
    IF NEW.deletion_id IS NULL OR NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.reversed_by IS NULL OR NEW.reversed_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO d FROM media_deletions WHERE id = NEW.deletion_id;
    IF NOT FOUND OR d.media_id IS DISTINCT FROM NEW.media_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_TARGET';
    END IF;
    IF d.deleted_by IS DISTINCT FROM NEW.reversed_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_ACTOR';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_PURGE_PLANNED';
    END IF;
    IF NOT usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_WINDOW';
    END IF;
    IF NEW.reversed_at > clock_timestamp() OR NEW.reversed_at < d.deleted_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_TIME';
    END IF;
    RETURN NEW;
END
$reversal_guard$;
CREATE TRIGGER media_deletion_reversals_guard BEFORE INSERT ON media_deletion_reversals
    FOR EACH ROW EXECUTE FUNCTION usapp_media_reversal_guard();

-- ---------------------------------------------------------------- replaced guards
-- The 0030 body plus one branch, first. 'user_deleted' alone passes a bound or reserved photo, and only for the exact unreversed
-- cycle it names, once that cycle's deadline has been reached. Every other reason meets the unchanged rules: the release clock is
-- NULL while deleted, so 'unattached' refuses a deleted photo.
CREATE OR REPLACE FUNCTION usapp_media_plan_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $plan_guard$
DECLARE
    m media_objects%ROWTYPE;
    t media_upload_outcomes%ROWTYPE;
    d media_deletions%ROWTYPE;
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
    IF NEW.reason = 'user_deleted' THEN
        SELECT * INTO d FROM media_deletions WHERE id = NEW.deletion_id AND media_id = NEW.media_id;
        IF NOT FOUND OR t.outcome IS DISTINCT FROM 'ready' THEN
            RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_ELIGIBILITY';
        END IF;
        IF EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = d.id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_REVERSED';
        END IF;
        IF usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF005', MESSAGE = 'USAPP_MEDIA_PLAN_WINDOW';
        END IF;
        RETURN NEW;
    END IF;
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

-- The 0031 body plus one refusal, after the cleanup one: a deleted photo gains no new home.
CREATE OR REPLACE FUNCTION usapp_media_binding_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $binding_guard$
DECLARE
    m media_objects%ROWTYPE;
    reserved uuid;
BEGIN
    IF NEW.media_id IS NULL OR NEW.created_at IS NULL OR NEW.target_kind IS NULL OR NEW.target_id IS NULL THEN
        RETURN NEW;
    END IF;
    IF NEW.target_kind = 'album' THEN
        PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.target_id::text));
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    IF NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = NEW.media_id AND t.outcome = 'ready') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_NOT_READY';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_CLEANUP';
    END IF;
    IF usapp_media_deleted_now(NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_DELETED';
    END IF;
    IF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TIME';
    END IF;
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    reserved := usapp_media_pending_reference(NEW.media_id);
    IF NEW.target_kind = 'memory' THEN
        IF NOT EXISTS (SELECT 1 FROM memories x WHERE x.id = NEW.target_id AND x.couple_id = NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
        END IF;
        IF m.uploader_id IS DISTINCT FROM NEW.bound_by
           AND NOT EXISTS (SELECT 1 FROM memory_proposal_photos x
                             JOIN memory_edit_proposals p ON p.id = x.proposal_id
                             JOIN memory_edit_decisions d ON d.proposal_id = p.id
                            WHERE x.media_id = NEW.media_id AND p.memory_id = NEW.target_id
                              AND d.outcome = 'approved' AND d.decided_by = NEW.bound_by) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_ACTOR';
        END IF;
        IF reserved IS NOT NULL AND NOT EXISTS (SELECT 1 FROM memory_edit_proposals p WHERE p.id = reserved AND p.memory_id = NEW.target_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_RESERVED';
        END IF;
    ELSIF NEW.target_kind = 'album' THEN
        IF NOT EXISTS (SELECT 1 FROM albums x WHERE x.id = NEW.target_id AND x.couple_id = NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
        END IF;
        IF m.uploader_id IS DISTINCT FROM NEW.bound_by THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_ACTOR';
        END IF;
        IF reserved IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_RESERVED';
        END IF;
    ELSE
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
    END IF;
    RETURN NEW;
END
$binding_guard$;

-- The 0030 body plus one refusal. Covers a kept photo too: a new revision is an edit or an approval, and neither may name a deleted
-- photo. Rows already written are history and are never looked at again.
CREATE OR REPLACE FUNCTION usapp_memory_revision_photo_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_photo_guard$
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
    IF usapp_media_deleted_now(NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_DELETED';
    END IF;
    RETURN NEW;
END
$revision_photo_guard$;

-- The 0030 body plus one refusal, before the bound branch, which otherwise returns early.
CREATE OR REPLACE FUNCTION usapp_memory_proposal_photo_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $proposal_photo_guard$
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
    IF usapp_media_deleted_now(NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG003', MESSAGE = 'USAPP_MEMORY_PHOTO_DELETED';
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

-- The 0031 body plus the media lock, 33021 first as everywhere, and one refusal.
CREATE OR REPLACE FUNCTION usapp_album_cover_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $cover_guard$
DECLARE
    top integer;
BEGIN
    IF NEW.album_id IS NULL OR NEW.event_no IS NULL OR NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.actor_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    IF NOT EXISTS (SELECT 1 FROM album_items i WHERE i.album_id = NEW.album_id AND i.media_id = NEW.media_id AND i.couple_id = NEW.couple_id
                      AND NOT EXISTS (SELECT 1 FROM album_item_removals r WHERE r.album_id = i.album_id AND r.media_id = i.media_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH005', MESSAGE = 'USAPP_ALBUM_COVER_ITEM';
    END IF;
    IF usapp_media_deleted_now(NEW.media_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH005', MESSAGE = 'USAPP_ALBUM_COVER_DELETED';
    END IF;
    SELECT max(event_no) INTO top FROM album_cover_events WHERE album_id = NEW.album_id;
    IF NEW.event_no::bigint <> COALESCE(top, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH005', MESSAGE = 'USAPP_ALBUM_COVER_EVENT';
    END IF;
    RETURN NEW;
END
$cover_guard$;

REVOKE ALL ON media_deletions, media_deletion_reversals FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON media_deletions, media_deletion_reversals TO usapp_api;

COMMIT;
