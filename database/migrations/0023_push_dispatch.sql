-- =====================================================================
-- 0023_push_dispatch: Phase 2 P2-5b durable push dispatch (docs/phase2-web-push.md Sections 5, 6
-- and 13). Committed notifications remain the durable source; nothing here writes to wallets,
-- tasks, notifications or audit logs, and no business writer changes.
--
-- push_dispatch_control      append-only control log; latest revision is the effective state
-- push_deliveries            one row per (notification, generation), inserted by fan-out
-- push_attempts              contiguous claims with a lease
-- push_attempt_observations  the ACTUAL result of an attempt (at most one per attempt)
-- push_attempt_expiries      the SYNTHETIC lease expiry (kept apart; never occupies the slot above)
-- push_delivery_outcomes     terminal scheduling state, first writer wins
--
-- 0022 compatibility (additive; 0022 itself is frozen): provider invalidation is a new revocation
-- reason `gone`. It has NO human actor (revoked_by IS NULL), no successor, and links to the exact
-- provider observation that reported it, so it can never be mistaken for a user action.
--
-- Roles: enable / disable / resume only by a superuser maintenance session with actor = owner;
-- usapp_api may append only an automatic dispatcher pause. usapp_push has no access.
-- Locks: 33015 control log (serializes transitions); 33016 claims (global live-lease bound), after
-- the scheduler lock. The gone revocation takes neither the user nor the endpoint lock (it only
-- lowers counts, 0022 / Section 4.3). Recording takes no advisory lock except 33015 for a pause.
-- SQLSTATE: UZ001 append-only; UZ002 control role/transition; UZ003 delivery guard; UZ004 attempt
-- guard; UZ005 observation / expiry guard; UZ006 outcome guard; UZ007 gone revocation guard.
-- =====================================================================

BEGIN;

DO $dispatch_preflight$
BEGIN
    IF to_regclass('public.push_registration_commands') IS NULL THEN
        RAISE EXCEPTION '0023_push_dispatch requires 0022_push_subscriptions';
    END IF;
END
$dispatch_preflight$;

-- ---------------------------------------------------------------- control
CREATE TABLE push_dispatch_control (
    revision bigint NOT NULL,
    kind text NOT NULL,
    actor text NOT NULL,
    reason_code text NOT NULL,
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_dispatch_control_pkey PRIMARY KEY (revision),
    CONSTRAINT push_dispatch_control_revision_positive CHECK (revision >= 1),
    CONSTRAINT push_dispatch_control_kind_valid CHECK (kind IN ('enable', 'disable', 'pause', 'resume')),
    CONSTRAINT push_dispatch_control_actor_valid CHECK (actor IN ('owner', 'dispatcher')),
    CONSTRAINT push_dispatch_control_reason_shape CHECK (reason_code ~ '^[a-z0-9_]{1,64}$')
);

-- Effective state and cutover. Cutover = recorded_at of the latest enable; resume never moves it.
CREATE FUNCTION usapp_push_control_state(OUT state text, OUT cutover timestamptz)
LANGUAGE sql STABLE AS $control_state$
    SELECT COALESCE((SELECT CASE c.kind WHEN 'pause' THEN 'paused' WHEN 'disable' THEN 'disabled' ELSE 'enabled' END
                       FROM push_dispatch_control c ORDER BY c.revision DESC LIMIT 1), 'disabled'),
           (SELECT max(c.recorded_at) FROM push_dispatch_control c WHERE c.kind = 'enable')
$control_state$;

CREATE FUNCTION usapp_push_control_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $control_guard$
DECLARE
    prev_state text;
    next_revision bigint;
BEGIN
    -- Serializes every transition; the state below is read after the wait.
    PERFORM pg_advisory_xact_lock(33015::integer, 0);
    IF current_user = 'usapp_api' THEN
        IF NEW.kind <> 'pause' OR NEW.actor <> 'dispatcher'
           OR NEW.reason_code NOT IN ('worker_auth_failed', 'provider_auth_failed') THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ002', MESSAGE = 'USAPP_PUSH_CONTROL_ROLE';
        END IF;
    ELSIF current_setting('is_superuser') = 'on' THEN
        IF NEW.actor <> 'owner' THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ002', MESSAGE = 'USAPP_PUSH_CONTROL_ACTOR';
        END IF;
    ELSE
        RAISE EXCEPTION USING ERRCODE = 'UZ002', MESSAGE = 'USAPP_PUSH_CONTROL_ROLE';
    END IF;
    SELECT s.state INTO prev_state FROM usapp_push_control_state() s;
    IF NOT ((NEW.kind = 'enable' AND prev_state = 'disabled')
            OR (NEW.kind = 'disable' AND prev_state IN ('enabled', 'paused'))
            OR (NEW.kind = 'pause' AND prev_state = 'enabled')
            OR (NEW.kind = 'resume' AND prev_state = 'paused')) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ002', MESSAGE = 'USAPP_PUSH_CONTROL_TRANSITION',
            DETAIL = NEW.kind || ' from ' || prev_state;
    END IF;
    SELECT COALESCE(max(c.revision), 0) + 1 INTO next_revision FROM push_dispatch_control c;
    IF NEW.revision IS NOT NULL AND NEW.revision <> next_revision THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ002', MESSAGE = 'USAPP_PUSH_CONTROL_REVISION';
    END IF;
    NEW.revision := next_revision;
    -- Taken under the lock, so revision order and time order agree; never caller-supplied.
    NEW.recorded_at := clock_timestamp();
    RETURN NEW;
END
$control_guard$;

CREATE TRIGGER push_dispatch_control_guard BEFORE INSERT ON push_dispatch_control
    FOR EACH ROW EXECUTE FUNCTION usapp_push_control_guard();

-- ---------------------------------------------------------------- dispatch facts
CREATE TABLE push_deliveries (
    notification_id uuid NOT NULL,
    generation_id uuid NOT NULL,
    recipient_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_deliveries_pkey PRIMARY KEY (notification_id, generation_id),
    -- Same recipient on both sides, by typed FKs.
    CONSTRAINT push_deliveries_notification_fk FOREIGN KEY (notification_id, couple_id, recipient_id)
        REFERENCES notifications (id, couple_id, recipient_id) ON DELETE RESTRICT,
    CONSTRAINT push_deliveries_generation_fk FOREIGN KEY (generation_id, recipient_id)
        REFERENCES push_subscriptions (id, user_id) ON DELETE RESTRICT
);
CREATE INDEX push_deliveries_generation_idx ON push_deliveries (generation_id);
CREATE INDEX push_deliveries_recipient_idx ON push_deliveries (recipient_id, created_at DESC);

CREATE TABLE push_attempts (
    notification_id uuid NOT NULL,
    generation_id uuid NOT NULL,
    attempt_no integer NOT NULL,
    instance_id text NOT NULL,
    claimed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    lease_until timestamptz NOT NULL,
    CONSTRAINT push_attempts_pkey PRIMARY KEY (notification_id, generation_id, attempt_no),
    CONSTRAINT push_attempts_delivery_fk FOREIGN KEY (notification_id, generation_id)
        REFERENCES push_deliveries (notification_id, generation_id) ON DELETE RESTRICT,
    CONSTRAINT push_attempts_no_positive CHECK (attempt_no >= 1),
    CONSTRAINT push_attempts_instance_shape CHECK (char_length(instance_id) BETWEEN 1 AND 128),
    -- The lease is at most the 60 s of Section 6.3 and cannot be extended (append-only).
    CONSTRAINT push_attempts_lease_bound CHECK (lease_until > claimed_at
                                                AND lease_until <= claimed_at + interval '60 seconds')
);
CREATE INDEX push_attempts_lease_idx ON push_attempts (lease_until);

CREATE TABLE push_attempt_observations (
    notification_id uuid NOT NULL,
    generation_id uuid NOT NULL,
    attempt_no integer NOT NULL,
    outcome text NOT NULL,
    class text NOT NULL,
    http_status integer,
    retry_after_s integer,
    observed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_attempt_observations_pkey PRIMARY KEY (notification_id, generation_id, attempt_no),
    CONSTRAINT push_attempt_observations_attempt_fk FOREIGN KEY (notification_id, generation_id, attempt_no)
        REFERENCES push_attempts (notification_id, generation_id, attempt_no) ON DELETE RESTRICT,
    -- Provider results consume the budget. Internal failures and pre-send skips never reached a
    -- provider. provider_auth and worker_auth stay distinct outcomes.
    CONSTRAINT push_attempt_observations_class_rule CHECK (
        (class = 'provider' AND outcome IN ('accepted', 'gone', 'rejected', 'transient', 'unknown',
                                            'provider_auth_failed'))
        OR (class = 'internal' AND outcome IN ('worker_auth_failed', 'transport_unavailable', 'busy',
                                               'inactive_skipped', 'ttl_skipped', 'cutover_skipped',
                                               'control_skipped', 'terminal_skipped'))),
    CONSTRAINT push_attempt_observations_status_range CHECK (http_status IS NULL OR http_status BETWEEN 100 AND 599),
    CONSTRAINT push_attempt_observations_retry_range CHECK (retry_after_s IS NULL OR retry_after_s BETWEEN 0 AND 86400)
);

CREATE TABLE push_attempt_expiries (
    notification_id uuid NOT NULL,
    generation_id uuid NOT NULL,
    attempt_no integer NOT NULL,
    expired_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_attempt_expiries_pkey PRIMARY KEY (notification_id, generation_id, attempt_no),
    CONSTRAINT push_attempt_expiries_attempt_fk FOREIGN KEY (notification_id, generation_id, attempt_no)
        REFERENCES push_attempts (notification_id, generation_id, attempt_no) ON DELETE RESTRICT
);

CREATE TABLE push_delivery_outcomes (
    notification_id uuid NOT NULL,
    generation_id uuid NOT NULL,
    final_state text NOT NULL,
    attempt_no integer,
    decided_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_delivery_outcomes_pkey PRIMARY KEY (notification_id, generation_id),
    CONSTRAINT push_delivery_outcomes_delivery_fk FOREIGN KEY (notification_id, generation_id)
        REFERENCES push_deliveries (notification_id, generation_id) ON DELETE RESTRICT,
    CONSTRAINT push_delivery_outcomes_attempt_fk FOREIGN KEY (notification_id, generation_id, attempt_no)
        REFERENCES push_attempts (notification_id, generation_id, attempt_no) ON DELETE RESTRICT,
    CONSTRAINT push_delivery_outcomes_state_valid CHECK (final_state IN ('accepted', 'gone', 'rejected', 'inactive',
                                                                         'expired', 'exhausted', 'cutover_skipped')),
    -- A provider verdict names the attempt that produced it.
    CONSTRAINT push_delivery_outcomes_attempt_rule CHECK (
        final_state NOT IN ('accepted', 'gone', 'rejected', 'exhausted') OR attempt_no IS NOT NULL)
);
CREATE INDEX push_delivery_outcomes_state_idx ON push_delivery_outcomes (final_state, decided_at DESC);

-- The window scan of fan-out (no watermark, so late commits are found).
CREATE INDEX notifications_created_idx ON notifications (created_at);

-- ---------------------------------------------------------------- budget
-- Attempts that consumed the provider budget: a provider-class observation, or a synthetic expiry
-- with no observation (unknown result). Internal observations never count.
CREATE FUNCTION usapp_push_provider_attempts(p_notification uuid, p_generation uuid) RETURNS integer
LANGUAGE sql STABLE AS $provider_attempts$
    SELECT count(*)::integer
      FROM push_attempts a
      LEFT JOIN push_attempt_observations o USING (notification_id, generation_id, attempt_no)
      LEFT JOIN push_attempt_expiries x USING (notification_id, generation_id, attempt_no)
     WHERE a.notification_id = p_notification AND a.generation_id = p_generation
       AND (o.class = 'provider' OR (o.attempt_no IS NULL AND x.attempt_no IS NOT NULL))
$provider_attempts$;

-- ---------------------------------------------------------------- append-only
CREATE FUNCTION usapp_push_dispatch_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $dispatch_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UZ001', MESSAGE = 'USAPP_PUSH_DISPATCH_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$dispatch_reject$;

CREATE TRIGGER push_dispatch_control_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_dispatch_control FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();
CREATE TRIGGER push_deliveries_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_deliveries FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();
CREATE TRIGGER push_attempts_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_attempts FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();
CREATE TRIGGER push_attempt_observations_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_attempt_observations FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();
CREATE TRIGGER push_attempt_expiries_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_attempt_expiries FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();
CREATE TRIGGER push_delivery_outcomes_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_delivery_outcomes FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_dispatch_reject_mutation();

-- ---------------------------------------------------------------- guards
-- Fan-out, Section 5.2 rules 1-4 at insert time: the FKs give rule 1, the rest is checked here.
-- Control must have been enabled (paused still fans out); the 24 h bound is the documented TTL.
CREATE FUNCTION usapp_push_delivery_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $delivery_guard$
DECLARE
    notice_at timestamptz;
    activated timestamptz;
    g_couple uuid;
    ctl record;
BEGIN
    SELECT n.created_at INTO notice_at FROM notifications n WHERE n.id = NEW.notification_id;
    SELECT g.activated_at, g.couple_id INTO activated, g_couple FROM push_subscriptions g WHERE g.id = NEW.generation_id;
    SELECT * INTO ctl FROM usapp_push_control_state();
    IF notice_at IS NULL OR activated IS NULL OR g_couple IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ003', MESSAGE = 'USAPP_PUSH_DELIVERY_SOURCE';
    END IF;
    IF ctl.state = 'disabled' OR ctl.cutover IS NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ003', MESSAGE = 'USAPP_PUSH_DELIVERY_DISABLED';
    END IF;
    IF notice_at < activated OR notice_at < ctl.cutover
       OR notice_at <= clock_timestamp() - interval '24 hours'
       OR NEW.created_at > clock_timestamp() OR NEW.created_at < notice_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ003', MESSAGE = 'USAPP_PUSH_DELIVERY_ELIGIBILITY';
    END IF;
    RETURN NEW;
END
$delivery_guard$;

CREATE TRIGGER push_deliveries_guard BEFORE INSERT ON push_deliveries
    FOR EACH ROW EXECUTE FUNCTION usapp_push_delivery_guard();

-- A claim: enabled control, no terminal outcome, contiguous, previous attempt resolved, budget
-- left, at most four live leases globally (lock 33016, taken inside the scheduler transaction).
CREATE FUNCTION usapp_push_attempt_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $attempt_guard$
DECLARE
    ctl record;
    live bigint;
    notice_at timestamptz;
BEGIN
    IF NEW.notification_id IS NULL OR NEW.generation_id IS NULL OR NEW.attempt_no IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33016::integer, 0);
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

CREATE TRIGGER push_attempts_guard BEFORE INSERT ON push_attempts
    FOR EACH ROW EXECUTE FUNCTION usapp_push_attempt_guard();

-- An observation may arrive at any later time, even after an expiry or a terminal outcome.
CREATE FUNCTION usapp_push_observation_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $observation_guard$
DECLARE
    claimed timestamptz;
BEGIN
    SELECT a.claimed_at INTO claimed FROM push_attempts a
     WHERE a.notification_id = NEW.notification_id AND a.generation_id = NEW.generation_id
       AND a.attempt_no = NEW.attempt_no;
    IF claimed IS NOT NULL AND (NEW.observed_at < claimed OR NEW.observed_at > clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ005', MESSAGE = 'USAPP_PUSH_OBSERVATION_TIME';
    END IF;
    RETURN NEW;
END
$observation_guard$;

CREATE TRIGGER push_attempt_observations_guard BEFORE INSERT ON push_attempt_observations
    FOR EACH ROW EXECUTE FUNCTION usapp_push_observation_guard();

-- A synthetic expiry only after the lease ended and only while no actual result exists.
CREATE FUNCTION usapp_push_expiry_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $expiry_guard$
DECLARE
    lease timestamptz;
BEGIN
    SELECT a.lease_until INTO lease FROM push_attempts a
     WHERE a.notification_id = NEW.notification_id AND a.generation_id = NEW.generation_id
       AND a.attempt_no = NEW.attempt_no;
    IF lease IS NOT NULL AND (NEW.expired_at < lease OR NEW.expired_at > clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ005', MESSAGE = 'USAPP_PUSH_EXPIRY_TIME';
    END IF;
    IF EXISTS (SELECT 1 FROM push_attempt_observations o
                WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id
                  AND o.attempt_no = NEW.attempt_no) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ005', MESSAGE = 'USAPP_PUSH_EXPIRY_OBSERVED';
    END IF;
    RETURN NEW;
END
$expiry_guard$;

CREATE TRIGGER push_attempt_expiries_guard BEFORE INSERT ON push_attempt_expiries
    FOR EACH ROW EXECUTE FUNCTION usapp_push_expiry_guard();

-- Terminal facts that the database can verify. `expired` and `inactive` are decided by the
-- dispatcher's policy and session checks at that moment and are only time-checked here.
CREATE FUNCTION usapp_push_outcome_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $outcome_guard$
DECLARE
    observed text;
    created timestamptz;
    notice_at timestamptz;
    ctl record;
BEGIN
    SELECT d.created_at INTO created FROM push_deliveries d
     WHERE d.notification_id = NEW.notification_id AND d.generation_id = NEW.generation_id;
    IF created IS NOT NULL AND (NEW.decided_at < created OR NEW.decided_at > clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_TIME';
    END IF;
    IF NEW.final_state IN ('accepted', 'gone', 'rejected') THEN
        SELECT o.outcome INTO observed FROM push_attempt_observations o
         WHERE o.notification_id = NEW.notification_id AND o.generation_id = NEW.generation_id
           AND o.attempt_no = NEW.attempt_no;
        IF observed IS DISTINCT FROM NEW.final_state THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_OBSERVATION';
        END IF;
    ELSIF NEW.final_state = 'exhausted' THEN
        IF usapp_push_provider_attempts(NEW.notification_id, NEW.generation_id) < 5 THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_BUDGET';
        END IF;
    ELSIF NEW.final_state = 'cutover_skipped' THEN
        SELECT n.created_at INTO notice_at FROM notifications n WHERE n.id = NEW.notification_id;
        SELECT * INTO ctl FROM usapp_push_control_state();
        IF ctl.cutover IS NULL OR notice_at >= ctl.cutover THEN
            RAISE EXCEPTION USING ERRCODE = 'UZ006', MESSAGE = 'USAPP_PUSH_OUTCOME_CUTOVER';
        END IF;
    END IF;
    RETURN NEW;
END
$outcome_guard$;

CREATE TRIGGER push_delivery_outcomes_guard BEFORE INSERT ON push_delivery_outcomes
    FOR EACH ROW EXECUTE FUNCTION usapp_push_outcome_guard();

-- ---------------------------------------------------------------- 0022 compatibility: gone
ALTER TABLE push_subscription_revocations
    ALTER COLUMN revoked_by DROP NOT NULL,
    ADD COLUMN gone_notification_id uuid,
    ADD COLUMN gone_attempt_no integer;

ALTER TABLE push_subscription_revocations
    DROP CONSTRAINT push_subscription_revocations_reason_valid,
    DROP CONSTRAINT push_subscription_revocations_cause_presence,
    DROP CONSTRAINT push_subscription_revocations_actor_rule;

ALTER TABLE push_subscription_revocations
    ADD CONSTRAINT push_subscription_revocations_reason_valid
        CHECK (reason IN ('user', 'superseded', 'replaced', 'reowned', 'gone')),
    ADD CONSTRAINT push_subscription_revocations_cause_presence
        CHECK ((reason IN ('user', 'gone')) = (caused_by IS NULL)),
    -- `gone` has no human actor. Every other reason keeps its 0022 actor rule, written so a NULL
    -- actor fails instead of passing as unknown.
    ADD CONSTRAINT push_subscription_revocations_actor_rule CHECK (
        (reason = 'gone' AND revoked_by IS NULL)
        OR (reason = 'reowned' AND revoked_by IS NOT NULL AND revoked_by <> user_id)
        OR (reason IN ('user', 'superseded', 'replaced') AND revoked_by IS NOT NULL AND revoked_by = user_id)),
    -- Provider attribution: a gone revocation names the observation that reported it, on this very
    -- generation (subscription_id), so it can never reach a replacement on the same endpoint.
    ADD CONSTRAINT push_subscription_revocations_gone_source
        CHECK ((reason = 'gone') = (gone_notification_id IS NOT NULL)
               AND (reason = 'gone') = (gone_attempt_no IS NOT NULL)),
    ADD CONSTRAINT push_subscription_revocations_gone_fk
        FOREIGN KEY (gone_notification_id, subscription_id, gone_attempt_no)
        REFERENCES push_attempt_observations (notification_id, generation_id, attempt_no) ON DELETE RESTRICT;

-- Replaces the 0022 function body (the trigger stays). Non-gone behaviour is unchanged.
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
    IF NOT FOUND OR g.user_id IS DISTINCT FROM NEW.user_id
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

-- ---------------------------------------------------------------- grants
REVOKE ALL ON push_dispatch_control, push_deliveries, push_attempts, push_attempt_observations,
    push_attempt_expiries, push_delivery_outcomes FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON push_dispatch_control, push_deliveries, push_attempts, push_attempt_observations,
    push_attempt_expiries, push_delivery_outcomes TO usapp_api;

COMMIT;
