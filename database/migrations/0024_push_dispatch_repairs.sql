-- =====================================================================
-- 0024_push_dispatch_repairs: Phase 2 P2-5b correctness repairs (docs/phase2-web-push.md Section 14).
-- DRAFT: not applied anywhere. 0022 and 0023 stay frozen; this only replaces function bodies and
-- adds to them.
--
-- 1. Claim provenance. push_attempts.control_revision records the latest control revision at claim
--    time, assigned by the guard and never trusted from the caller. An automatic pause is allowed
--    only for a result whose attempt was claimed under the CURRENT revision (see PushDispatch.cc);
--    while enabled the latest revision is always the owner's enable or resume, so a stale result
--    cannot pause a control state the owner restored after the claim. Older rows stay NULL: unknown
--    provenance, which never pauses.
-- 2. Stale pre-send attempts. New internal observation lease_lost_skipped: this claim lost its lease
--    or was superseded before anything was sent. No provider was involved, so it uses no budget.
-- 3. already_active versus gone. A gone revocation now takes the endpoint lock 33014, as a
--    registration does, so it cannot revoke the occupant between the registration's read and its
--    command. It still takes no user lock 33013. Registration order is session -> 33013 -> 33014; a
--    gone recording holds only 33014, so no cycle exists. Guards are not weakened.
-- =====================================================================

BEGIN;

DO $repairs_preflight$
BEGIN
    IF to_regclass('public.push_delivery_outcomes') IS NULL THEN
        RAISE EXCEPTION '0024_push_dispatch_repairs requires 0023_push_dispatch';
    END IF;
END
$repairs_preflight$;

ALTER TABLE push_attempts ADD COLUMN control_revision bigint;
ALTER TABLE push_attempts
    ADD CONSTRAINT push_attempts_control_revision_fk FOREIGN KEY (control_revision)
        REFERENCES push_dispatch_control (revision) ON DELETE RESTRICT;

ALTER TABLE push_attempt_observations DROP CONSTRAINT push_attempt_observations_class_rule;
ALTER TABLE push_attempt_observations
    ADD CONSTRAINT push_attempt_observations_class_rule CHECK (
        (class = 'provider' AND outcome IN ('accepted', 'gone', 'rejected', 'transient', 'unknown',
                                            'provider_auth_failed'))
        OR (class = 'internal' AND outcome IN ('worker_auth_failed', 'transport_unavailable', 'busy',
                                               'inactive_skipped', 'ttl_skipped', 'cutover_skipped',
                                               'control_skipped', 'terminal_skipped', 'lease_lost_skipped')));

-- The 0023 body plus the claim provenance. The revision is read BEFORE the state: a transition in
-- between leaves the claim with an older revision, which can only make its results stale.
CREATE OR REPLACE FUNCTION usapp_push_attempt_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $attempt_guard$
DECLARE
    ctl record;
    live bigint;
    notice_at timestamptz;
    latest bigint;
BEGIN
    IF NEW.notification_id IS NULL OR NEW.generation_id IS NULL OR NEW.attempt_no IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33016::integer, 0);
    SELECT max(c.revision) INTO latest FROM push_dispatch_control c;
    IF NEW.control_revision IS NOT NULL AND NEW.control_revision IS DISTINCT FROM latest THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_REVISION';
    END IF;
    NEW.control_revision := latest;
    SELECT * INTO ctl FROM usapp_push_control_state();
    SELECT n.created_at INTO notice_at FROM notifications n WHERE n.id = NEW.notification_id;
    IF ctl.state <> 'enabled' OR notice_at < ctl.cutover OR notice_at <= clock_timestamp() - interval '24 hours' THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_CONTROL';
    END IF;
    IF EXISTS (SELECT 1 FROM push_delivery_outcomes o
                WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_TERMINAL';
    END IF;
    IF NEW.attempt_no <> 1 + (SELECT COALESCE(max(a.attempt_no), 0) FROM push_attempts a
                               WHERE a.notification_id = NEW.notification_id AND a.generation_id = NEW.generation_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_ORDER';
    END IF;
    IF NEW.attempt_no > 1 AND NOT EXISTS (
           SELECT 1 FROM push_attempt_observations o
            WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id
              AND o.attempt_no = NEW.attempt_no - 1)
       AND NOT EXISTS (
           SELECT 1 FROM push_attempt_expiries x
            WHERE x.notification_id = NEW.notification_id AND x.generation_id = NEW.generation_id
              AND x.attempt_no = NEW.attempt_no - 1) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_UNRESOLVED';
    END IF;
    IF usapp_push_provider_attempts(NEW.notification_id, NEW.generation_id) >= 5 THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_BUDGET';
    END IF;
    SELECT count(*) INTO live FROM push_attempts a
     WHERE a.lease_until > clock_timestamp()
       AND NOT EXISTS (SELECT 1 FROM push_attempt_observations o
                        WHERE o.notification_id = a.notification_id AND o.generation_id = a.generation_id
                          AND o.attempt_no = a.attempt_no)
       AND NOT EXISTS (SELECT 1 FROM push_attempt_expiries x
                        WHERE x.notification_id = a.notification_id AND x.generation_id = a.generation_id
                          AND x.attempt_no = a.attempt_no);
    IF live >= 4 THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_CAPACITY';
    END IF;
    IF NEW.claimed_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ004', MESSAGE = 'USAPP_PUSH_ATTEMPT_TIME';
    END IF;
    RETURN NEW;
END
$attempt_guard$;

-- The 0023 body plus the endpoint lock for gone. After this wait, a registration that read this
-- generation as the occupant has committed, and any later registration will read it as revoked.
CREATE OR REPLACE FUNCTION usapp_push_revocation_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $revocation_guard$
DECLARE
    g push_subscriptions%ROWTYPE;
    observed text;
BEGIN
    IF NEW.subscription_id IS NULL OR NEW.revoked_at IS NULL THEN
        RETURN NEW;
    END IF;
    IF NEW.reason <> 'gone' THEN
        IF NEW.revoked_by IS NULL THEN
            RETURN NEW;  -- the actor rule CHECK rejects it
        END IF;
        -- The revoker's user lock: own revocations serialize with own registrations. A re-owning
        -- revoker never takes the previous owner's lock (it only lowers that user's count).
        PERFORM pg_advisory_xact_lock(33013::integer, hashtext(NEW.revoked_by::text));
    END IF;
    SELECT * INTO g FROM push_subscriptions WHERE id = NEW.subscription_id;
    IF NEW.reason = 'gone' AND FOUND THEN
        PERFORM pg_advisory_xact_lock(33014::integer, hashtext(g.endpoint_hash));
    END IF;
    IF g.id IS NULL OR g.user_id IS DISTINCT FROM NEW.user_id
       OR NEW.revoked_at < g.activated_at OR NEW.revoked_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UY003', MESSAGE = 'USAPP_PUSH_REVOCATION_TARGET';
    END IF;
    -- A user revocation comes from the owner's space; a re-own is decided by a successor link.
    IF NEW.reason = 'user' AND NEW.couple_id IS DISTINCT FROM g.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UY003', MESSAGE = 'USAPP_PUSH_REVOCATION_SPACE';
    END IF;
    IF NEW.reason = 'gone' THEN
        SELECT o.outcome INTO observed FROM push_attempt_observations o
         WHERE o.notification_id = NEW.gone_notification_id AND o.generation_id = NEW.subscription_id
           AND o.attempt_no = NEW.gone_attempt_no;
        IF observed IS DISTINCT FROM 'gone' OR NEW.couple_id IS DISTINCT FROM g.couple_id THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ007', MESSAGE = 'USAPP_PUSH_GONE_SOURCE';
        END IF;
    END IF;
    RETURN NEW;
END
$revocation_guard$;

COMMIT;
