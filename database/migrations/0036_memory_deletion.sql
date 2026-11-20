-- =====================================================================
-- 0036_memory_deletion: D6 whole-memory deletion, step M1a (docs/phase3-d6-memory-deletion-design.md). 0000-0035 stay frozen.
--
-- memory_deletions           one deletion cycle of one memory, by its creator. Never changes
-- memory_deletion_reversals  the creator's undo of that exact cycle, strictly before its deadline
--
-- A memory with an unreversed cycle is deleted now: every read answers as if it were absent, and nothing can edit it, propose to it,
-- decide on it, bind to it or newly link a plan to it. All text and bytes stay. Nothing here is irreversible: the deadline only closes
-- undo. Deletion requests are M1b, the creator-photo cascade and the kept-photo list M1c, and text erasure M1d.
--
-- A cycle may not start while an edit proposal is pending: the service decides it first, in the same transaction, so its reservations are
-- released at commit and an undo does not bring it back.
--
-- Replaces five function bodies: usapp_memory_revision_guard, usapp_memory_proposal_guard and usapp_memory_decision_guard (0030), the
-- binding guard (0035) and usapp_media_shared_now (0031), so that a photo is no longer shared through a deleted memory. Its uploader keeps
-- access, as ever. Also replaces the plan link guard and attach function (0034). A plan's existing link is left alone.
--
-- Lock order: member FOR SHARE -> 33020 memory -> sorted 33018 media, as every memory write. New plan links take 33024 and 33022, then
-- 33020; nothing takes 33022 or 33024 after 33020. The binding guard's memory branch now takes 33020 before 33018, as its album branch
-- does with 33021. No new lock class.
-- SQLSTATE: UE001 append-only; UE002 deletion; UE003 reversal. The replaced guards keep UF007, UG002, UG004, UG005 and UK006.
-- =====================================================================

BEGIN;

DO $memory_deletion_preflight$
BEGIN
    IF to_regclass('public.media_deletion_reversals') IS NULL THEN
        RAISE EXCEPTION '0036_memory_deletion requires 0035_media_deletion';
    END IF;
END
$memory_deletion_preflight$;

CREATE TABLE memory_deletions (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    deleted_by uuid NOT NULL,
    cycle_no integer NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    deleted_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    purge_after timestamptz NOT NULL,
    CONSTRAINT memory_deletions_pkey PRIMARY KEY (id),
    CONSTRAINT memory_deletions_id_memory_unique UNIQUE (id, memory_id),
    CONSTRAINT memory_deletions_cycle_unique UNIQUE (memory_id, cycle_no),
    CONSTRAINT memory_deletions_key_unique UNIQUE (deleted_by, idempotency_key),
    CONSTRAINT memory_deletions_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletions_member_fk FOREIGN KEY (couple_id, deleted_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletions_cycle_valid CHECK (cycle_no >= 1),
    -- The one deadline: hours, so a calendar never moves it.
    CONSTRAINT memory_deletions_window_valid CHECK (purge_after = deleted_at + interval '168 hours'),
    CONSTRAINT memory_deletions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_deletions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_deletions_owner_idx ON memory_deletions (deleted_by, deleted_at DESC, id DESC);

CREATE TABLE memory_deletion_reversals (
    deletion_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    reversed_by uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    reversed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_deletion_reversals_pkey PRIMARY KEY (deletion_id),
    CONSTRAINT memory_deletion_reversals_key_unique UNIQUE (reversed_by, idempotency_key),
    CONSTRAINT memory_deletion_reversals_deletion_fk FOREIGN KEY (deletion_id, memory_id)
        REFERENCES memory_deletions (id, memory_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletion_reversals_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletion_reversals_member_fk FOREIGN KEY (couple_id, reversed_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletion_reversals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_deletion_reversals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_deletion_reversals_memory_idx ON memory_deletion_reversals (memory_id);

CREATE FUNCTION usapp_memory_deletion_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $memory_deletion_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UE001', MESSAGE = 'USAPP_MEMORY_DELETION_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$memory_deletion_reject$;
CREATE TRIGGER memory_deletions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_deletions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();
CREATE TRIGGER memory_deletion_reversals_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_deletion_reversals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();

-- ---------------------------------------------------------------- definitions
-- The memory's unreversed cycle, if it has one. There is at most one: the deletion guard refuses a second.
CREATE FUNCTION usapp_memory_active_deletion(p_memory uuid) RETURNS uuid
LANGUAGE sql STABLE AS $memory_active_deletion$
    SELECT d.id FROM memory_deletions d
     WHERE d.memory_id = p_memory
       AND NOT EXISTS (SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id = d.id)
     LIMIT 1
$memory_active_deletion$;

CREATE FUNCTION usapp_memory_deleted_now(p_memory uuid) RETURNS boolean
LANGUAGE sql STABLE AS $memory_deleted_now$
    SELECT usapp_memory_active_deletion(p_memory) IS NOT NULL
$memory_deleted_now$;

-- The 0031 body with one change: a photo is not shared through a deleted memory. An album item is its own rule, unchanged.
CREATE OR REPLACE FUNCTION usapp_media_shared_now(p_media uuid) RETURNS boolean
LANGUAGE sql STABLE AS $shared_now$
    SELECT EXISTS (SELECT 1 FROM media_bindings b
                    WHERE b.media_id = p_media AND b.target_kind = 'memory'
                      AND NOT usapp_memory_deleted_now(b.target_id))
        OR EXISTS (SELECT 1 FROM album_items i
                     JOIN media_bindings b ON b.media_id = i.media_id
                                          AND b.target_kind = 'album'
                                          AND b.target_id = i.album_id
                    WHERE i.media_id = p_media
                      AND NOT EXISTS (SELECT 1 FROM album_item_removals r
                                       WHERE r.album_id = i.album_id AND r.media_id = i.media_id))
$shared_now$;

-- ---------------------------------------------------------------- deletion guards. VOLATILE: statements after the lock see its holder's commit.
-- The creator's own memory, with no open cycle and nothing pending.
CREATE FUNCTION usapp_memory_deletion_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $deletion_guard$
DECLARE
    x memories%ROWTYPE;
BEGIN
    IF NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.deleted_by IS NULL OR NEW.cycle_no IS NULL OR NEW.deleted_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO x FROM memories WHERE id = NEW.memory_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_TARGET';
    END IF;
    IF x.creator_id IS DISTINCT FROM NEW.deleted_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_ACTOR';
    END IF;
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_ACTIVE';
    END IF;
    IF EXISTS (SELECT 1 FROM memory_edit_proposals q WHERE q.memory_id = NEW.memory_id
                  AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_PROPOSAL_PENDING';
    END IF;
    IF NEW.cycle_no::bigint <> 1 + COALESCE((SELECT max(d.cycle_no) FROM memory_deletions d WHERE d.memory_id = NEW.memory_id), 0)::bigint THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_CYCLE';
    END IF;
    IF NEW.deleted_at > clock_timestamp() OR NEW.deleted_at < x.created_at
       OR NEW.deleted_at < COALESCE((SELECT max(r.reversed_at) FROM memory_deletion_reversals r WHERE r.memory_id = NEW.memory_id), NEW.deleted_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_TIME';
    END IF;
    RETURN NEW;
END
$deletion_guard$;
CREATE TRIGGER memory_deletions_guard BEFORE INSERT ON memory_deletions
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_deletion_guard();

-- The original deleter, this exact cycle, on the database clock. The boundary is usapp_media_undo_open, the one definition.
CREATE FUNCTION usapp_memory_reversal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $reversal_guard$
DECLARE
    d memory_deletions%ROWTYPE;
BEGIN
    IF NEW.deletion_id IS NULL OR NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.reversed_by IS NULL OR NEW.reversed_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO d FROM memory_deletions WHERE id = NEW.deletion_id;
    IF NOT FOUND OR d.memory_id IS DISTINCT FROM NEW.memory_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_TARGET';
    END IF;
    IF d.deleted_by IS DISTINCT FROM NEW.reversed_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_ACTOR';
    END IF;
    IF NOT usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_WINDOW';
    END IF;
    IF NEW.reversed_at > clock_timestamp() OR NEW.reversed_at < d.deleted_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_TIME';
    END IF;
    RETURN NEW;
END
$reversal_guard$;
CREATE TRIGGER memory_deletion_reversals_guard BEFORE INSERT ON memory_deletion_reversals
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_reversal_guard();

-- ---------------------------------------------------------------- replaced memory guards
-- The 0030 body plus one refusal, after the lock.
CREATE OR REPLACE FUNCTION usapp_memory_revision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_guard$
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
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_DELETED';
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

-- The 0030 body plus one refusal, after the lock.
CREATE OR REPLACE FUNCTION usapp_memory_proposal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $proposal_guard$
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
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_DELETED';
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

-- The 0030 body plus one refusal, after the lock. Every outcome: a cycle can only start with nothing pending, so none is ever due.
CREATE OR REPLACE FUNCTION usapp_memory_decision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $decision_guard$
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
    IF usapp_memory_deleted_now(p.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DELETED';
    END IF;
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

-- The 0035 body plus the memory branch's lock, 33020 before 33018 as everywhere, and one refusal.
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
    ELSIF NEW.target_kind = 'memory' THEN
        PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.target_id::text));
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
        IF usapp_memory_deleted_now(NEW.target_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_MEMORY_DELETED';
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

-- ---------------------------------------------------------------- plan links
-- The 0034 body plus 33020 on the memory and one refusal. A brand new memory, in create mode, has no cycle.
CREATE OR REPLACE FUNCTION usapp_plan_memory_link_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path = pg_catalog, public, pg_temp AS $$
BEGIN
    IF usapp_plan_memory_preflight(NEW.actor_id, NEW.plan_id, NEW.mode, NEW.request_key) <> 'new' THEN
        RAISE EXCEPTION USING ERRCODE = 'UK006', MESSAGE = 'USAPP_PLAN_MEMORY_LINK_REFUSED';
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UK006', MESSAGE = 'USAPP_PLAN_MEMORY_DELETED';
    END IF;
    IF NEW.mode = 'create' AND NOT EXISTS (
        SELECT 1 FROM memories m WHERE m.id = NEW.memory_id AND m.couple_id = NEW.couple_id
            AND m.creator_id = NEW.actor_id AND m.xmin = pg_current_xact_id()::xid
    ) THEN
        RAISE EXCEPTION USING ERRCODE = 'UK006', MESSAGE = 'USAPP_PLAN_MEMORY_CREATION_REQUIRED';
    END IF;
    RETURN NEW;
END
$$;

-- The 0034 body with a new link refused for a deleted memory, as for an absent one, after its lock. A replay reports only what this
-- caller already sent, and is not evidence that the memory can be read now.
CREATE OR REPLACE FUNCTION usapp_plan_memory_attach(p_actor uuid, p_plan uuid, p_key text, p_memory uuid)
RETURNS jsonb LANGUAGE plpgsql SET search_path = pg_catalog, public, pg_temp AS $$
DECLARE
    check_result text;
    link shared_plan_memory_links%ROWTYPE;
BEGIN
    check_result := usapp_plan_memory_preflight(p_actor, p_plan, 'existing', p_key);
    IF check_result NOT IN ('new','replay') THEN RETURN jsonb_build_object('error', check_result); END IF;
    IF check_result = 'replay' THEN
        SELECT * INTO link FROM shared_plan_memory_links WHERE plan_id = p_plan;
        IF link.memory_id <> p_memory THEN RETURN '{"error":"key_reused"}'; END IF;
    ELSE
        PERFORM pg_advisory_xact_lock(33020::integer, hashtext(p_memory::text));
        IF usapp_memory_deleted_now(p_memory) THEN RETURN '{"error":"not_found"}'; END IF;
        INSERT INTO shared_plan_memory_links(plan_id,couple_id,memory_id,actor_id,mode,request_key)
        SELECT p.id,p.couple_id,m.id,p_actor,'existing',p_key
        FROM shared_plans p JOIN memories m ON m.couple_id = p.couple_id
        WHERE p.id = p_plan AND m.id = p_memory
        RETURNING * INTO link;
        IF NOT FOUND THEN RETURN '{"error":"not_found"}'; END IF;
        INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
        VALUES ('user',p_actor,link.couple_id,'shared_plan.memory_linked','shared_plan',p_plan::text,
                jsonb_build_object('memory_id',p_memory));
    END IF;
    RETURN jsonb_build_object('plan_id',p_plan,'memory_id',link.memory_id,'replayed',check_result = 'replay');
END
$$;

REVOKE ALL ON memory_deletions, memory_deletion_reversals FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON memory_deletions, memory_deletion_reversals TO usapp_api;

COMMIT;
