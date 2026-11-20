-- Automatic decision time is assigned after the existing lock and semantic checks.
-- Supplied times retain the strict old predicates. No historical row or deadline changes.
BEGIN;
ALTER TABLE memory_edit_decisions ALTER COLUMN decided_at DROP DEFAULT;

CREATE OR REPLACE FUNCTION usapp_memory_decision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $decision_guard$
DECLARE
    p memory_edit_proposals%ROWTYPE;
    x memories%ROWTYPE;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL
       OR NEW.outcome IS NULL THEN
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
    IF NEW.decided_at IS NULL THEN
        NEW.decided_at := GREATEST(clock_timestamp(), p.created_at);
    ELSIF NEW.decided_at > clock_timestamp() OR NEW.decided_at < p.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UG005', MESSAGE = 'USAPP_MEMORY_DECISION_TIME';
    END IF;
    RETURN NEW;
END
$decision_guard$;

COMMIT;
