-- =====================================================================
-- 0018_anniversaries: Phase 2 P2-2 anniversaries and countdowns
-- (docs/phase2-anniversaries.md, owner-approved defaults O1-O10).
--
-- anniversaries (immutable identity) + anniversary_revisions (create / creator edit /
-- approved proposal; current state = highest version) + anniversary_edit_proposals and
-- anniversary_edit_decisions (V1 §30 edit requests, one pending per anniversary, creator
-- edits blocked while pending: option A) + anniversary_home_pins (per-user Home pin
-- history; current pin = highest seq; NULL anniversary = cleared).
-- All five are append-only; usapp_api gets SELECT, INSERT only. No deletion, archive,
-- recurrence, image, reminder or notification columns. Civil dates are `date` values.
-- Locks: per anniversary (33010, hashtext(anniversary id)); per user pin (33011, hashtext(user id)).
-- UN001 append-only; UN002 revision guard; UN003 proposal guard; UN004 decision guard;
-- UN005 deferred links; UN006 pin guard.
-- =====================================================================

BEGIN;

DO $anniversary_preflight$
BEGIN
    IF to_regclass('public.message_replies') IS NULL THEN
        RAISE EXCEPTION '0018_anniversaries requires 0017_messages';
    END IF;
END
$anniversary_preflight$;

CREATE TABLE anniversaries (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    creator_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT anniversaries_pkey PRIMARY KEY (id),
    CONSTRAINT anniversaries_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT anniversaries_creator_fk FOREIGN KEY (couple_id, creator_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT
);

CREATE INDEX anniversaries_couple_idx ON anniversaries (couple_id, created_at DESC, id DESC);

CREATE TABLE anniversary_revisions (
    anniversary_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    version integer NOT NULL,
    actor_id uuid NOT NULL,
    action text NOT NULL,
    proposal_id uuid,
    title text NOT NULL,
    event_date date NOT NULL,
    kind text NOT NULL,
    note text NOT NULL DEFAULT '',
    idempotency_key text,
    request_fingerprint text,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT anniversary_revisions_pkey PRIMARY KEY (anniversary_id, version),
    CONSTRAINT anniversary_revisions_key_unique UNIQUE (actor_id, idempotency_key),
    CONSTRAINT anniversary_revisions_proposal_unique UNIQUE (proposal_id),
    CONSTRAINT anniversary_revisions_identity_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_revisions_member_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_revisions_version_valid CHECK (version >= 1),
    CONSTRAINT anniversary_revisions_action_valid CHECK (action IN ('create', 'edit', 'approve')),
    CONSTRAINT anniversary_revisions_create_first CHECK ((action = 'create') = (version = 1)),
    -- An approval revision links its proposal; its request key lives on the decision row.
    CONSTRAINT anniversary_revisions_approve_link CHECK ((action = 'approve') = (proposal_id IS NOT NULL)),
    CONSTRAINT anniversary_revisions_key_presence CHECK (
        (action = 'approve') = (idempotency_key IS NULL)
        AND (idempotency_key IS NULL) = (request_fingerprint IS NULL)
    ),
    CONSTRAINT anniversary_revisions_kind_valid CHECK (kind IN ('anniversary', 'countdown')),
    CONSTRAINT anniversary_revisions_date_range CHECK (event_date BETWEEN DATE '1900-01-01' AND DATE '2199-12-31'),
    CONSTRAINT anniversary_revisions_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 40
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT anniversary_revisions_note_shape CHECK (
        char_length(note) <= 500
        AND note = btrim(note, E' \n')
        AND note !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT anniversary_revisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT anniversary_revisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE anniversary_edit_proposals (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    anniversary_id uuid NOT NULL,
    proposer_id uuid NOT NULL,
    base_version integer NOT NULL,
    title text NOT NULL,
    event_date date NOT NULL,
    kind text NOT NULL,
    note text NOT NULL DEFAULT '',
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT anniversary_edit_proposals_pkey PRIMARY KEY (id),
    CONSTRAINT anniversary_edit_proposals_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT anniversary_edit_proposals_key_unique UNIQUE (proposer_id, idempotency_key),
    -- V1 §30 "原内容": the immutable base revision.
    CONSTRAINT anniversary_edit_proposals_base_fk FOREIGN KEY (anniversary_id, base_version)
        REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT,
    CONSTRAINT anniversary_edit_proposals_identity_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_edit_proposals_member_fk FOREIGN KEY (couple_id, proposer_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_edit_proposals_kind_valid CHECK (kind IN ('anniversary', 'countdown')),
    CONSTRAINT anniversary_edit_proposals_date_range CHECK (event_date BETWEEN DATE '1900-01-01' AND DATE '2199-12-31'),
    CONSTRAINT anniversary_edit_proposals_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 40
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT anniversary_edit_proposals_note_shape CHECK (
        char_length(note) <= 500
        AND note = btrim(note, E' \n')
        AND note !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT anniversary_edit_proposals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT anniversary_edit_proposals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE INDEX anniversary_edit_proposals_anniversary_idx
    ON anniversary_edit_proposals (anniversary_id, created_at DESC, id DESC);

ALTER TABLE anniversary_revisions
    ADD CONSTRAINT anniversary_revisions_proposal_fk FOREIGN KEY (proposal_id)
        REFERENCES anniversary_edit_proposals (id) ON DELETE RESTRICT;

CREATE TABLE anniversary_edit_decisions (
    proposal_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    decided_by uuid NOT NULL,
    outcome text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- One decision per proposal: approve, reject and withdraw race on this key.
    CONSTRAINT anniversary_edit_decisions_pkey PRIMARY KEY (proposal_id),
    CONSTRAINT anniversary_edit_decisions_key_unique UNIQUE (decided_by, idempotency_key),
    CONSTRAINT anniversary_edit_decisions_proposal_fk FOREIGN KEY (proposal_id, couple_id)
        REFERENCES anniversary_edit_proposals (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_edit_decisions_member_fk FOREIGN KEY (couple_id, decided_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_edit_decisions_outcome_valid CHECK (outcome IN ('approved', 'rejected', 'withdrawn')),
    CONSTRAINT anniversary_edit_decisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT anniversary_edit_decisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE anniversary_home_pins (
    user_id uuid NOT NULL,
    seq integer NOT NULL,
    couple_id uuid NOT NULL,
    anniversary_id uuid,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT anniversary_home_pins_pkey PRIMARY KEY (user_id, seq),
    CONSTRAINT anniversary_home_pins_key_unique UNIQUE (user_id, idempotency_key),
    CONSTRAINT anniversary_home_pins_member_fk FOREIGN KEY (couple_id, user_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- A pin can only reference an anniversary of the pinning member's own space.
    CONSTRAINT anniversary_home_pins_target_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT anniversary_home_pins_seq_valid CHECK (seq >= 1),
    CONSTRAINT anniversary_home_pins_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT anniversary_home_pins_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_anniversary_reject_mutation()
RETURNS trigger LANGUAGE plpgsql
AS $anniversary_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UN001',
        MESSAGE = 'USAPP_ANNIVERSARY_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$anniversary_reject$;

CREATE TRIGGER anniversaries_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON anniversaries
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_anniversary_reject_mutation();
CREATE TRIGGER anniversary_revisions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON anniversary_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_anniversary_reject_mutation();
CREATE TRIGGER anniversary_edit_proposals_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON anniversary_edit_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_anniversary_reject_mutation();
CREATE TRIGGER anniversary_edit_decisions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON anniversary_edit_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_anniversary_reject_mutation();
CREATE TRIGGER anniversary_home_pins_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON anniversary_home_pins
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_anniversary_reject_mutation();

-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE FUNCTION usapp_anniversary_revision_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $revision_guard$
DECLARE
    a anniversaries%ROWTYPE;
    prev anniversary_revisions%ROWTYPE;
    has_prev boolean;
    p anniversary_edit_proposals%ROWTYPE;
BEGIN
    IF NEW.anniversary_id IS NULL OR NEW.couple_id IS NULL OR NEW.version IS NULL OR NEW.actor_id IS NULL
       OR NEW.action IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33010::integer, hashtext(NEW.anniversary_id::text));
    SELECT * INTO a FROM anniversaries WHERE id = NEW.anniversary_id;
    IF NOT FOUND OR a.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.actor_id IS DISTINCT FROM a.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UN002', MESSAGE = 'USAPP_ANNIVERSARY_ACTOR';
    END IF;
    SELECT * INTO prev FROM anniversary_revisions
     WHERE anniversary_id = NEW.anniversary_id ORDER BY version DESC LIMIT 1;
    has_prev := FOUND;
    IF NEW.version::bigint <> COALESCE(prev.version, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UN002', MESSAGE = 'USAPP_ANNIVERSARY_VERSION';
    END IF;
    IF NEW.action = 'edit' AND EXISTS (
        SELECT 1 FROM anniversary_edit_proposals x
         WHERE x.anniversary_id = NEW.anniversary_id
           AND NOT EXISTS (SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id = x.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN002', MESSAGE = 'USAPP_ANNIVERSARY_PROPOSAL_PENDING';
    END IF;
    IF NEW.action = 'approve' THEN
        SELECT * INTO p FROM anniversary_edit_proposals WHERE id = NEW.proposal_id;
        IF NOT FOUND OR p.anniversary_id IS DISTINCT FROM NEW.anniversary_id
           OR p.base_version IS DISTINCT FROM prev.version
           OR NOT EXISTS (SELECT 1 FROM anniversary_edit_decisions d
                           WHERE d.proposal_id = p.id AND d.outcome = 'approved' AND d.decided_by = NEW.actor_id)
           OR ROW(NEW.title, NEW.event_date, NEW.kind, NEW.note)
              IS DISTINCT FROM ROW(p.title, p.event_date, p.kind, p.note) THEN
            RAISE EXCEPTION USING ERRCODE = 'UN002', MESSAGE = 'USAPP_ANNIVERSARY_APPROVAL';
        END IF;
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < a.created_at
       OR (has_prev AND NEW.created_at < prev.created_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN002', MESSAGE = 'USAPP_ANNIVERSARY_TIME';
    END IF;
    RETURN NEW;
END
$revision_guard$;

CREATE TRIGGER anniversary_revisions_guard BEFORE INSERT ON anniversary_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_anniversary_revision_guard();

-- Partner only, against the current version, at most one pending proposal per anniversary.
CREATE FUNCTION usapp_anniversary_proposal_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $proposal_guard$
DECLARE
    a anniversaries%ROWTYPE;
    current_version integer;
BEGIN
    IF NEW.anniversary_id IS NULL OR NEW.couple_id IS NULL OR NEW.proposer_id IS NULL
       OR NEW.base_version IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33010::integer, hashtext(NEW.anniversary_id::text));
    SELECT * INTO a FROM anniversaries WHERE id = NEW.anniversary_id;
    IF NOT FOUND OR a.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.proposer_id = a.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UN003', MESSAGE = 'USAPP_ANNIVERSARY_PROPOSER';
    END IF;
    SELECT max(version) INTO current_version FROM anniversary_revisions WHERE anniversary_id = NEW.anniversary_id;
    IF NEW.base_version IS DISTINCT FROM current_version THEN
        RAISE EXCEPTION USING ERRCODE = 'UN003', MESSAGE = 'USAPP_ANNIVERSARY_PROPOSAL_BASE';
    END IF;
    IF EXISTS (SELECT 1 FROM anniversary_edit_proposals x
                WHERE x.anniversary_id = NEW.anniversary_id
                  AND NOT EXISTS (SELECT 1 FROM anniversary_edit_decisions d WHERE d.proposal_id = x.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN003', MESSAGE = 'USAPP_ANNIVERSARY_PROPOSAL_PENDING';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < a.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UN003', MESSAGE = 'USAPP_ANNIVERSARY_PROPOSAL_TIME';
    END IF;
    RETURN NEW;
END
$proposal_guard$;

CREATE TRIGGER anniversary_edit_proposals_guard BEFORE INSERT ON anniversary_edit_proposals
    FOR EACH ROW EXECUTE FUNCTION usapp_anniversary_proposal_guard();

-- approved/rejected: the creator; withdrawn: the proposer; approval only against the current version.
CREATE FUNCTION usapp_anniversary_decision_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $decision_guard$
DECLARE
    p anniversary_edit_proposals%ROWTYPE;
    a anniversaries%ROWTYPE;
    current_version integer;
BEGIN
    IF NEW.proposal_id IS NULL OR NEW.couple_id IS NULL OR NEW.decided_by IS NULL
       OR NEW.outcome IS NULL OR NEW.decided_at IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO p FROM anniversary_edit_proposals WHERE id = NEW.proposal_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UN004', MESSAGE = 'USAPP_ANNIVERSARY_DECISION_TARGET';
    END IF;
    PERFORM pg_advisory_xact_lock(33010::integer, hashtext(p.anniversary_id::text));
    SELECT * INTO a FROM anniversaries WHERE id = p.anniversary_id;
    IF (NEW.outcome IN ('approved', 'rejected') AND NEW.decided_by IS DISTINCT FROM a.creator_id)
       OR (NEW.outcome = 'withdrawn' AND NEW.decided_by IS DISTINCT FROM p.proposer_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN004', MESSAGE = 'USAPP_ANNIVERSARY_DECISION_ACTOR';
    END IF;
    IF NEW.outcome = 'approved' THEN
        SELECT max(version) INTO current_version FROM anniversary_revisions WHERE anniversary_id = p.anniversary_id;
        IF p.base_version IS DISTINCT FROM current_version THEN
            RAISE EXCEPTION USING ERRCODE = 'UN004', MESSAGE = 'USAPP_ANNIVERSARY_DECISION_STALE';
        END IF;
    END IF;
    IF NEW.decided_at > clock_timestamp() OR NEW.decided_at < p.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UN004', MESSAGE = 'USAPP_ANNIVERSARY_DECISION_TIME';
    END IF;
    RETURN NEW;
END
$decision_guard$;

CREATE TRIGGER anniversary_edit_decisions_guard BEFORE INSERT ON anniversary_edit_decisions
    FOR EACH ROW EXECUTE FUNCTION usapp_anniversary_decision_guard();

-- Deferred links: an identity has its version-1 create revision; an approval has exactly one
-- approve revision (and an approve revision always has its approved decision, checked in UN002).
CREATE FUNCTION usapp_anniversary_link_check()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $anniversary_link$
BEGIN
    IF TG_TABLE_NAME = 'anniversaries' THEN
        IF NOT EXISTS (SELECT 1 FROM anniversary_revisions r
                        WHERE r.anniversary_id = NEW.id AND r.version = 1 AND r.action = 'create') THEN
            RAISE EXCEPTION USING ERRCODE = 'UN005', MESSAGE = 'USAPP_ANNIVERSARY_CREATE_REVISION';
        END IF;
    ELSIF NEW.outcome = 'approved' THEN
        IF (SELECT count(*) FROM anniversary_revisions r
             WHERE r.proposal_id = NEW.proposal_id AND r.action = 'approve') <> 1 THEN
            RAISE EXCEPTION USING ERRCODE = 'UN005', MESSAGE = 'USAPP_ANNIVERSARY_APPROVAL_REVISION';
        END IF;
    END IF;
    RETURN NULL;
END
$anniversary_link$;

CREATE CONSTRAINT TRIGGER anniversaries_create_revision AFTER INSERT ON anniversaries
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_anniversary_link_check();
CREATE CONSTRAINT TRIGGER anniversary_edit_decisions_approval_revision AFTER INSERT ON anniversary_edit_decisions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.outcome = 'approved')
    EXECUTE FUNCTION usapp_anniversary_link_check();

-- Per-user pin history: contiguous seq, serialized per user.
CREATE FUNCTION usapp_anniversary_pin_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $pin_guard$
DECLARE
    prev anniversary_home_pins%ROWTYPE;
    has_prev boolean;
BEGIN
    IF NEW.user_id IS NULL OR NEW.seq IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33011::integer, hashtext(NEW.user_id::text));
    SELECT * INTO prev FROM anniversary_home_pins WHERE user_id = NEW.user_id ORDER BY seq DESC LIMIT 1;
    has_prev := FOUND;
    IF NEW.seq::bigint <> COALESCE(prev.seq, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UN006', MESSAGE = 'USAPP_ANNIVERSARY_PIN_SEQ';
    END IF;
    IF NEW.created_at > clock_timestamp() OR (has_prev AND NEW.created_at < prev.created_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN006', MESSAGE = 'USAPP_ANNIVERSARY_PIN_TIME';
    END IF;
    RETURN NEW;
END
$pin_guard$;

CREATE TRIGGER anniversary_home_pins_guard BEFORE INSERT ON anniversary_home_pins
    FOR EACH ROW EXECUTE FUNCTION usapp_anniversary_pin_guard();

REVOKE ALL ON anniversaries, anniversary_revisions, anniversary_edit_proposals, anniversary_edit_decisions,
    anniversary_home_pins FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON anniversaries, anniversary_revisions, anniversary_edit_proposals, anniversary_edit_decisions,
    anniversary_home_pins TO usapp_api;

COMMIT;
