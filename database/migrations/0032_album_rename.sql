-- =====================================================================
-- 0032_album_rename: Phase 3 P3-2b album rename and rename requests (docs/phase3-p32b-album-rename-design.md, D18 Option A).
-- 0000-0031 stay frozen.
--
-- album_edit_proposals   a partner's requested name against the current version (V1 section 30, as 0030)
-- album_edit_decisions   one per proposal: approved, rejected, withdrawn
--
-- Extends album_revisions additively: an 'approve' revision names its proposal, and its key lives on the decision. Replaces one
-- 0031 function body, the revision guard: a direct rename is refused while a request is pending, which is what keeps a pending
-- request's base current, and an approval is exactly the proposal's text as base_version + 1. Existing rows are not touched.
-- Lock order: member FOR SHARE -> 33021 album. Nothing here takes 33018 or 33020. Photos and covers do not change the version.
-- SQLSTATE: UH002 revision; UH006 deferred links; UH007 proposal; UH008 decision.
-- =====================================================================

BEGIN;

DO $album_rename_preflight$
BEGIN
    IF to_regclass('public.album_revisions') IS NULL THEN
        RAISE EXCEPTION '0032_album_rename requires 0031_albums';
    END IF;
END
$album_rename_preflight$;

CREATE TABLE album_edit_proposals (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    album_id uuid NOT NULL,
    proposer_id uuid NOT NULL,
    base_version integer NOT NULL,
    text_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_edit_proposals_pkey PRIMARY KEY (id),
    CONSTRAINT album_edit_proposals_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT album_edit_proposals_key_unique UNIQUE (proposer_id, idempotency_key),
    -- V1 section 30 "original content": the immutable base revision.
    CONSTRAINT album_edit_proposals_base_fk FOREIGN KEY (album_id, base_version)
        REFERENCES album_revisions (album_id, version) ON DELETE RESTRICT,
    CONSTRAINT album_edit_proposals_album_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_edit_proposals_member_fk FOREIGN KEY (couple_id, proposer_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_edit_proposals_text_fk FOREIGN KEY (text_id, couple_id)
        REFERENCES album_texts (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_edit_proposals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_edit_proposals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX album_edit_proposals_album_idx ON album_edit_proposals (album_id, created_at DESC, id DESC);

CREATE TABLE album_edit_decisions (
    proposal_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    decided_by uuid NOT NULL,
    outcome text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- One decision per proposal: approve, reject and withdraw race on this key.
    CONSTRAINT album_edit_decisions_pkey PRIMARY KEY (proposal_id),
    CONSTRAINT album_edit_decisions_key_unique UNIQUE (decided_by, idempotency_key),
    CONSTRAINT album_edit_decisions_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES album_edit_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_edit_decisions_member_fk FOREIGN KEY (couple_id, decided_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_edit_decisions_outcome_valid CHECK (outcome IN ('approved', 'rejected', 'withdrawn')),
    CONSTRAINT album_edit_decisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_edit_decisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TRIGGER album_edit_proposals_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_edit_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_edit_decisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_edit_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();

-- ---------------------------------------------------------------- album_revisions, additively
ALTER TABLE album_revisions
    ADD COLUMN proposal_id uuid,
    ALTER COLUMN idempotency_key DROP NOT NULL,
    ALTER COLUMN request_fingerprint DROP NOT NULL,
    DROP CONSTRAINT album_revisions_action_valid,
    ADD CONSTRAINT album_revisions_action_valid CHECK (action IN ('create', 'edit', 'approve')),
    ADD CONSTRAINT album_revisions_proposal_unique UNIQUE (proposal_id),
    ADD CONSTRAINT album_revisions_proposal_fk FOREIGN KEY (proposal_id)
        REFERENCES album_edit_proposals (id) ON DELETE RESTRICT,
    ADD CONSTRAINT album_revisions_approve_link CHECK ((action = 'approve') = (proposal_id IS NOT NULL)),
    ADD CONSTRAINT album_revisions_key_presence CHECK (
        (action = 'approve') = (idempotency_key IS NULL)
        AND (idempotency_key IS NULL) = (request_fingerprint IS NULL)
    );

-- The 0031 body plus: no direct rename while a request is pending; an approval is the approved proposal's own text, on its base.
CREATE OR REPLACE FUNCTION usapp_album_revision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_guard$
DECLARE
    x albums%ROWTYPE;
    top integer;
    p album_edit_proposals%ROWTYPE;
BEGIN
    IF NEW.album_id IS NULL OR NEW.couple_id IS NULL OR NEW.version IS NULL OR NEW.actor_id IS NULL OR NEW.action IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    SELECT * INTO x FROM albums WHERE id = NEW.album_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.actor_id IS DISTINCT FROM x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_ACTOR';
    END IF;
    SELECT max(version) INTO top FROM album_revisions WHERE album_id = NEW.album_id;
    IF NEW.version::bigint <> COALESCE(top, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_VERSION';
    END IF;
    IF NEW.action = 'edit' AND EXISTS (
        SELECT 1 FROM album_edit_proposals q WHERE q.album_id = NEW.album_id
           AND NOT EXISTS (SELECT 1 FROM album_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_PROPOSAL_PENDING';
    END IF;
    IF NEW.action = 'approve' THEN
        SELECT * INTO p FROM album_edit_proposals WHERE id = NEW.proposal_id;
        IF NOT FOUND OR p.album_id IS DISTINCT FROM NEW.album_id OR p.base_version IS DISTINCT FROM top
           OR p.text_id IS DISTINCT FROM NEW.text_id
           OR NOT EXISTS (SELECT 1 FROM album_edit_decisions d
                           WHERE d.proposal_id = p.id AND d.outcome = 'approved' AND d.decided_by = NEW.actor_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_APPROVAL';
        END IF;
    END IF;
    RETURN NEW;
END
$revision_guard$;

-- ---------------------------------------------------------------- request guards
-- Partner only, against the current version, at most one pending request per album.
CREATE FUNCTION usapp_album_proposal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $proposal_guard$
DECLARE
    x albums%ROWTYPE;
BEGIN
    IF NEW.album_id IS NULL OR NEW.couple_id IS NULL OR NEW.proposer_id IS NULL OR NEW.base_version IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    SELECT * INTO x FROM albums WHERE id = NEW.album_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.proposer_id = x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UH007', MESSAGE = 'USAPP_ALBUM_PROPOSER';
    END IF;
    IF NEW.base_version IS DISTINCT FROM (SELECT max(r.version) FROM album_revisions r WHERE r.album_id = NEW.album_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH007', MESSAGE = 'USAPP_ALBUM_PROPOSAL_BASE';
    END IF;
    IF EXISTS (SELECT 1 FROM album_edit_proposals q WHERE q.album_id = NEW.album_id
                  AND NOT EXISTS (SELECT 1 FROM album_edit_decisions d WHERE d.proposal_id = q.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH007', MESSAGE = 'USAPP_ALBUM_PROPOSAL_PENDING';
    END IF;
    RETURN NEW;
END
$proposal_guard$;
CREATE TRIGGER album_edit_proposals_guard BEFORE INSERT ON album_edit_proposals
    FOR EACH ROW EXECUTE FUNCTION usapp_album_proposal_guard();

-- approved/rejected: the creator; withdrawn: the proposer; approval only against the current version.
CREATE FUNCTION usapp_album_decision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $decision_guard$
DECLARE
    p album_edit_proposals%ROWTYPE;
    x albums%ROWTYPE;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL OR NEW.outcome IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO p FROM album_edit_proposals WHERE id = NEW.proposal_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UH008', MESSAGE = 'USAPP_ALBUM_DECISION_TARGET';
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(p.album_id::text));
    SELECT * INTO x FROM albums WHERE id = p.album_id;
    IF (NEW.outcome IN ('approved', 'rejected') AND NEW.decided_by IS DISTINCT FROM x.creator_id)
       OR (NEW.outcome = 'withdrawn' AND NEW.decided_by IS DISTINCT FROM p.proposer_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH008', MESSAGE = 'USAPP_ALBUM_DECISION_ACTOR';
    END IF;
    IF NEW.outcome = 'approved'
       AND p.base_version IS DISTINCT FROM (SELECT max(r.version) FROM album_revisions r WHERE r.album_id = p.album_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH008', MESSAGE = 'USAPP_ALBUM_DECISION_STALE';
    END IF;
    RETURN NEW;
END
$decision_guard$;
CREATE TRIGGER album_edit_decisions_guard BEFORE INSERT ON album_edit_decisions
    FOR EACH ROW EXECUTE FUNCTION usapp_album_decision_guard();

-- ---------------------------------------------------------------- deferred link, checked at commit
CREATE FUNCTION usapp_album_approval_check() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $approval_check$
BEGIN
    IF (SELECT count(*) FROM album_revisions r WHERE r.proposal_id = NEW.proposal_id AND r.action = 'approve') <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH006', MESSAGE = 'USAPP_ALBUM_APPROVAL_REVISION';
    END IF;
    RETURN NULL;
END
$approval_check$;
CREATE CONSTRAINT TRIGGER album_edit_decisions_approval_revision AFTER INSERT ON album_edit_decisions
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN (NEW.outcome = 'approved')
    EXECUTE FUNCTION usapp_album_approval_check();

REVOKE ALL ON album_edit_proposals, album_edit_decisions FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON album_edit_proposals, album_edit_decisions TO usapp_api;

COMMIT;
