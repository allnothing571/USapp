-- =====================================================================
-- 0037_memory_delete_requests: D6 whole-memory deletion, step M1b (docs/phase3-d6-memory-deletion-design.md). 0000-0036 stay frozen.
--
-- memory_delete_requests   the partner asks the creator to delete a memory. It carries no text. Never changes
-- memory_delete_decisions  the one answer: approved or rejected by the creator, withdrawn by the requester. Never changes
--
-- At most one pending request of either kind per memory: an edit proposal and a delete request exclude each other, and the creator's direct
-- edit is refused while either is pending. Deleting a memory needs nothing pending, so the deciding transaction writes the decision first and
-- the cycle next. An approved decision names that cycle by a deferred foreign key, and a deferred check requires that the cycle was written
-- in this same transaction, by the decider. An undo restores the memory and changes no decision: an approved request stays approved, and the
-- partner may ask again. Nothing here is irreversible, and the creator-photo cascade (M1c) and text erasure (M1d) are still later steps.
--
-- Replaces three function bodies, each with one more refusal: usapp_memory_revision_guard and usapp_memory_proposal_guard, and
-- usapp_memory_deletion_guard (all from 0036).
--
-- Lock order: member FOR SHARE -> 33020 memory, as every memory write. This step takes no photo lock. No new lock class.
-- SQLSTATE: UE001 append-only (as 0036); UE004 request; UE005 decision. The replaced guards keep UE002, UG002 and UG004.
-- =====================================================================

BEGIN;

DO $memory_delete_requests_preflight$
BEGIN
    IF to_regclass('public.memory_deletion_reversals') IS NULL THEN
        RAISE EXCEPTION '0037_memory_delete_requests requires 0036_memory_deletion';
    END IF;
END
$memory_delete_requests_preflight$;

CREATE TABLE memory_delete_requests (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    requester_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_delete_requests_pkey PRIMARY KEY (id),
    CONSTRAINT memory_delete_requests_id_memory_unique UNIQUE (id, memory_id),
    CONSTRAINT memory_delete_requests_key_unique UNIQUE (requester_id, idempotency_key),
    CONSTRAINT memory_delete_requests_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_delete_requests_member_fk FOREIGN KEY (couple_id, requester_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT memory_delete_requests_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_delete_requests_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_delete_requests_memory_idx ON memory_delete_requests (memory_id, created_at DESC);

CREATE TABLE memory_delete_decisions (
    request_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    decided_by uuid NOT NULL,
    outcome text NOT NULL,
    deletion_id uuid,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_delete_decisions_pkey PRIMARY KEY (request_id),
    CONSTRAINT memory_delete_decisions_key_unique UNIQUE (decided_by, idempotency_key),
    CONSTRAINT memory_delete_decisions_deletion_unique UNIQUE (deletion_id),
    CONSTRAINT memory_delete_decisions_request_fk FOREIGN KEY (request_id, memory_id)
        REFERENCES memory_delete_requests (id, memory_id) ON DELETE RESTRICT,
    CONSTRAINT memory_delete_decisions_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT memory_delete_decisions_member_fk FOREIGN KEY (couple_id, decided_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- Deferred: the decision comes first, because a cycle cannot start while the request is pending.
    CONSTRAINT memory_delete_decisions_deletion_fk FOREIGN KEY (deletion_id, memory_id)
        REFERENCES memory_deletions (id, memory_id) ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT memory_delete_decisions_outcome_valid CHECK (outcome IN ('approved', 'rejected', 'withdrawn')),
    CONSTRAINT memory_delete_decisions_cycle_presence CHECK ((outcome = 'approved') = (deletion_id IS NOT NULL)),
    CONSTRAINT memory_delete_decisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT memory_delete_decisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX memory_delete_decisions_memory_idx ON memory_delete_decisions (memory_id);

CREATE TRIGGER memory_delete_requests_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_delete_requests
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();
CREATE TRIGGER memory_delete_decisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_delete_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();

-- ---------------------------------------------------------------- definitions
-- The memory's undecided delete request, if it has one. There is at most one: the request guard refuses a second.
CREATE FUNCTION usapp_memory_pending_delete_request(p_memory uuid) RETURNS uuid
LANGUAGE sql STABLE AS $pending_delete_request$
    SELECT q.id FROM memory_delete_requests q
     WHERE q.memory_id = p_memory
       AND NOT EXISTS (SELECT 1 FROM memory_delete_decisions d WHERE d.request_id = q.id)
     LIMIT 1
$pending_delete_request$;

-- ---------------------------------------------------------------- request and decision guards. VOLATILE: statements after the lock see its holder's commit.
CREATE FUNCTION usapp_memory_delete_request_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $request_guard$
DECLARE
    x memories%ROWTYPE;
BEGIN
    IF NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.requester_id IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO x FROM memories WHERE id = NEW.memory_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_TARGET';
    END IF;
    IF NEW.requester_id = x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_ACTOR';
    END IF;
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_DELETED';
    END IF;
    IF usapp_memory_pending_delete_request(NEW.memory_id) IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_PENDING';
    END IF;
    IF EXISTS (SELECT 1 FROM memory_edit_proposals q WHERE q.memory_id = NEW.memory_id
                  AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_PROPOSAL_PENDING';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < x.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UE004', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_TIME';
    END IF;
    RETURN NEW;
END
$request_guard$;
CREATE TRIGGER memory_delete_requests_guard BEFORE INSERT ON memory_delete_requests
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_delete_request_guard();

-- Approved and rejected by the creator, withdrawn by the requester, and never on a deleted memory.
CREATE FUNCTION usapp_memory_delete_decision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $decision_guard$
DECLARE
    q memory_delete_requests%ROWTYPE;
    x memories%ROWTYPE;
BEGIN
    IF NEW.request_id IS NULL OR NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL
       OR NEW.outcome IS NULL OR NEW.decided_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO q FROM memory_delete_requests WHERE id = NEW.request_id;
    IF NOT FOUND OR q.memory_id IS DISTINCT FROM NEW.memory_id OR q.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE005', MESSAGE = 'USAPP_MEMORY_DELETE_DECISION_TARGET';
    END IF;
    IF usapp_memory_deleted_now(NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE005', MESSAGE = 'USAPP_MEMORY_DELETED';
    END IF;
    SELECT * INTO x FROM memories WHERE id = NEW.memory_id;
    IF (NEW.outcome IN ('approved', 'rejected') AND NEW.decided_by IS DISTINCT FROM x.creator_id)
       OR (NEW.outcome = 'withdrawn' AND NEW.decided_by IS DISTINCT FROM q.requester_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE005', MESSAGE = 'USAPP_MEMORY_DELETE_DECISION_ACTOR';
    END IF;
    IF NEW.decided_at > clock_timestamp() OR NEW.decided_at < q.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UE005', MESSAGE = 'USAPP_MEMORY_DELETE_DECISION_TIME';
    END IF;
    RETURN NEW;
END
$decision_guard$;
CREATE TRIGGER memory_delete_decisions_guard BEFORE INSERT ON memory_delete_decisions
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_delete_decision_guard();

-- At commit: an approval's cycle was written by this transaction, by the decider. An older cycle of the same memory does not count.
CREATE FUNCTION usapp_memory_delete_decision_cycle_check() RETURNS trigger LANGUAGE plpgsql AS $cycle_check$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM memory_deletions d
                    WHERE d.id = NEW.deletion_id AND d.memory_id = NEW.memory_id AND d.deleted_by = NEW.decided_by
                      AND d.xmin = pg_current_xact_id()::xid) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE005', MESSAGE = 'USAPP_MEMORY_DELETE_DECISION_CYCLE';
    END IF;
    RETURN NULL;
END
$cycle_check$;
CREATE CONSTRAINT TRIGGER memory_delete_decisions_cycle AFTER INSERT ON memory_delete_decisions
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN (NEW.outcome = 'approved')
    EXECUTE FUNCTION usapp_memory_delete_decision_cycle_check();

-- ---------------------------------------------------------------- replaced guards
-- The 0036 body plus one refusal: a cycle needs the delete request settled first, as it needs an edit proposal settled.
CREATE OR REPLACE FUNCTION usapp_memory_deletion_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $deletion_guard$
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
    IF usapp_memory_pending_delete_request(NEW.memory_id) IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UE002', MESSAGE = 'USAPP_MEMORY_DELETION_REQUEST_PENDING';
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

-- The 0036 body plus one refusal: the creator's direct edit waits for a delete request, as it waits for an edit proposal.
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
    IF NEW.action = 'edit' AND usapp_memory_pending_delete_request(NEW.memory_id) IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UG002', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_PENDING';
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

-- The 0036 body plus one refusal: an edit proposal waits for a delete request.
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
    IF usapp_memory_pending_delete_request(NEW.memory_id) IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UG004', MESSAGE = 'USAPP_MEMORY_DELETE_REQUEST_PENDING';
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

REVOKE ALL ON memory_delete_requests, memory_delete_decisions FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON memory_delete_requests, memory_delete_decisions TO usapp_api;

COMMIT;
