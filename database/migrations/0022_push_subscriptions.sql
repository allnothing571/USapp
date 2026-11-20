-- =====================================================================
-- 0022_push_subscriptions: Phase 2 P2-5a device subscription lifecycle (no dispatch)
-- (docs/phase2-web-push.md revision 3, Sections 4 and 9).
--
-- push_subscriptions           immutable subscription generations (one per activation)
-- push_subscription_revocations one immutable revocation per generation
-- push_registration_commands   every accepted registration key and its ORIGINAL result,
--                              including already_active (no-op) registrations
-- A generation is active when it has no revocation and its session is authenticated, not
-- revoked and not expired. The VAPID key currency is NOT known to the database (it is
-- server configuration), so the database invariants are conservative: they count every
-- unrevoked generation of a valid session, whatever its vapid_key_id.
-- Locks (global order): session row FOR SHARE -> 33013 user (hashtext(user_id)) ->
-- 33014 endpoint (hashtext(endpoint_hash)). No path takes another user's user lock.
-- SQLSTATE: UY001 append-only; UY002 generation guard; UY003 revocation guard;
-- UY004 command guard; UY005 deferred links.
-- No delivery, dispatch or control tables; no UPDATE grants.
-- =====================================================================

BEGIN;

DO $push_preflight$
BEGIN
    IF to_regclass('public.task_reactions') IS NULL THEN
        RAISE EXCEPTION '0022_push_subscriptions requires 0021_task_reactions';
    END IF;
END
$push_preflight$;

CREATE TABLE push_subscriptions (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    user_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    session_id uuid NOT NULL,
    endpoint text NOT NULL,
    endpoint_hash text NOT NULL,
    p256dh text NOT NULL,
    auth text NOT NULL,
    vapid_key_id text NOT NULL,
    activated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_subscriptions_pkey PRIMARY KEY (id),
    CONSTRAINT push_subscriptions_id_user_unique UNIQUE (id, user_id),
    CONSTRAINT push_subscriptions_member_fk FOREIGN KEY (couple_id, user_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT push_subscriptions_session_fk FOREIGN KEY (session_id)
        REFERENCES sessions (id) ON DELETE RESTRICT,
    -- Allowlisted push services only: exact FCM / Mozilla hosts, label-boundary Apple / Windows
    -- suffixes; https, default port, no userinfo, printable ASCII path.
    CONSTRAINT push_subscriptions_endpoint_shape CHECK (
        char_length(endpoint) <= 2048
        AND endpoint ~ '^https://(fcm\.googleapis\.com|updates\.push\.services\.mozilla\.com|([a-z0-9]([a-z0-9-]*[a-z0-9])?\.)+push\.apple\.com|([a-z0-9]([a-z0-9-]*[a-z0-9])?\.)+notify\.windows\.com)/[!-~]*$'
    ),
    CONSTRAINT push_subscriptions_endpoint_hash CHECK (endpoint_hash = encode(sha256(convert_to(endpoint, 'UTF8')), 'hex')),
    -- base64url without padding: 65-byte P-256 point and 16-byte auth secret.
    CONSTRAINT push_subscriptions_p256dh_shape CHECK (p256dh ~ '^[A-Za-z0-9_-]{87}$'),
    CONSTRAINT push_subscriptions_auth_shape CHECK (auth ~ '^[A-Za-z0-9_-]{22}$'),
    CONSTRAINT push_subscriptions_vapid_shape CHECK (vapid_key_id ~ '^[0-9a-f]{64}$')
);

CREATE INDEX push_subscriptions_user_idx ON push_subscriptions (user_id, activated_at DESC, id DESC);
CREATE INDEX push_subscriptions_endpoint_idx ON push_subscriptions (endpoint_hash);
CREATE INDEX push_subscriptions_session_idx ON push_subscriptions (session_id);

CREATE TABLE push_subscription_revocations (
    subscription_id uuid NOT NULL,
    user_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    revoked_by uuid NOT NULL,
    reason text NOT NULL,
    caused_by uuid,
    revoked_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_subscription_revocations_pkey PRIMARY KEY (subscription_id),
    CONSTRAINT push_subscription_revocations_owner_fk FOREIGN KEY (subscription_id, user_id)
        REFERENCES push_subscriptions (id, user_id) ON DELETE RESTRICT,
    CONSTRAINT push_subscription_revocations_actor_fk FOREIGN KEY (couple_id, revoked_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- The successor generation is inserted after this row in the same transaction.
    CONSTRAINT push_subscription_revocations_cause_fk FOREIGN KEY (caused_by)
        REFERENCES push_subscriptions (id) ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT push_subscription_revocations_reason_valid
        CHECK (reason IN ('user', 'superseded', 'replaced', 'reowned')),
    CONSTRAINT push_subscription_revocations_cause_presence CHECK ((reason = 'user') = (caused_by IS NULL)),
    CONSTRAINT push_subscription_revocations_actor_rule CHECK (
        (reason IN ('user', 'superseded', 'replaced')) = (revoked_by = user_id)
    ),
    CONSTRAINT push_subscription_revocations_not_self CHECK (caused_by IS DISTINCT FROM subscription_id)
);

CREATE TABLE push_registration_commands (
    user_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    couple_id uuid NOT NULL,
    session_id uuid NOT NULL,
    request_fingerprint text NOT NULL,
    result text NOT NULL,
    generation_id uuid NOT NULL,
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT push_registration_commands_pkey PRIMARY KEY (user_id, idempotency_key),
    CONSTRAINT push_registration_commands_generation_fk FOREIGN KEY (generation_id, user_id)
        REFERENCES push_subscriptions (id, user_id) ON DELETE RESTRICT,
    CONSTRAINT push_registration_commands_member_fk FOREIGN KEY (couple_id, user_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT push_registration_commands_session_fk FOREIGN KEY (session_id)
        REFERENCES sessions (id) ON DELETE RESTRICT,
    CONSTRAINT push_registration_commands_result_valid CHECK (result IN ('created', 'already_active')),
    CONSTRAINT push_registration_commands_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT push_registration_commands_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

-- Exactly one creating command per generation (the deferred link requires at least one).
CREATE UNIQUE INDEX push_registration_commands_created_unique
    ON push_registration_commands (generation_id) WHERE result = 'created';
CREATE INDEX push_registration_commands_generation_idx ON push_registration_commands (generation_id);

CREATE FUNCTION usapp_push_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $push_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UY001', MESSAGE = 'USAPP_PUSH_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$push_reject$;

CREATE TRIGGER push_subscriptions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON push_subscriptions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_reject_mutation();
CREATE TRIGGER push_subscription_revocations_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_subscription_revocations FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_reject_mutation();
CREATE TRIGGER push_registration_commands_append_only BEFORE UPDATE OR DELETE OR TRUNCATE
    ON push_registration_commands FOR EACH STATEMENT EXECUTE FUNCTION usapp_push_reject_mutation();

-- A session is valid for push when it is an authenticated session of that user, not revoked
-- and not expired at the time of the statement.
CREATE FUNCTION usapp_push_session_valid(p_session uuid, p_user uuid) RETURNS boolean
LANGUAGE sql STABLE AS $push_session$
    SELECT EXISTS (SELECT 1 FROM sessions s
                    WHERE s.id = p_session AND s.user_id = p_user AND s.is_authenticated
                      AND s.revoked_at IS NULL AND s.expires_at > clock_timestamp())
$push_session$;

-- VOLATILE: statements after the advisory-lock waits see the previous holder's commit.
CREATE FUNCTION usapp_push_generation_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $generation_guard$
DECLARE
    session_created timestamptz;
    active_count bigint;
BEGIN
    IF NEW.user_id IS NULL OR NEW.session_id IS NULL OR NEW.endpoint_hash IS NULL OR NEW.activated_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33013::integer, hashtext(NEW.user_id::text));
    PERFORM pg_advisory_xact_lock(33014::integer, hashtext(NEW.endpoint_hash));
    SELECT s.created_at INTO session_created FROM sessions s WHERE s.id = NEW.session_id;
    IF NOT usapp_push_session_valid(NEW.session_id, NEW.user_id)
       OR NEW.activated_at < session_created OR NEW.activated_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UY002', MESSAGE = 'USAPP_PUSH_GENERATION_SESSION';
    END IF;
    -- At most one unrevoked generation per endpoint (any user, any session state).
    IF EXISTS (SELECT 1 FROM push_subscriptions g
                WHERE g.endpoint_hash = NEW.endpoint_hash
                  AND NOT EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UY002', MESSAGE = 'USAPP_PUSH_ENDPOINT_OCCUPIED';
    END IF;
    -- Conservative device cap: unrevoked generations of the user's valid sessions.
    SELECT count(*) INTO active_count FROM push_subscriptions g
     WHERE g.user_id = NEW.user_id
       AND NOT EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id)
       AND usapp_push_session_valid(g.session_id, g.user_id);
    IF active_count >= 10 THEN
        RAISE EXCEPTION USING ERRCODE = 'UY002', MESSAGE = 'USAPP_PUSH_DEVICE_LIMIT';
    END IF;
    RETURN NEW;
END
$generation_guard$;

CREATE TRIGGER push_subscriptions_guard BEFORE INSERT ON push_subscriptions
    FOR EACH ROW EXECUTE FUNCTION usapp_push_generation_guard();

CREATE FUNCTION usapp_push_revocation_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $revocation_guard$
DECLARE
    g push_subscriptions%ROWTYPE;
BEGIN
    IF NEW.subscription_id IS NULL OR NEW.revoked_by IS NULL OR NEW.revoked_at IS NULL THEN
        RETURN NEW;
    END IF;
    -- The revoker's user lock: own revocations serialize with own registrations. A re-owning
    -- revoker never takes the previous owner's lock (it only lowers that user's count).
    PERFORM pg_advisory_xact_lock(33013::integer, hashtext(NEW.revoked_by::text));
    SELECT * INTO g FROM push_subscriptions WHERE id = NEW.subscription_id;
    IF NOT FOUND OR g.user_id IS DISTINCT FROM NEW.user_id
       OR NEW.revoked_at < g.activated_at OR NEW.revoked_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UY003', MESSAGE = 'USAPP_PUSH_REVOCATION_TARGET';
    END IF;
    -- A user revocation comes from the owner's space; a re-own is decided by a successor link.
    IF NEW.reason = 'user' AND NEW.couple_id IS DISTINCT FROM g.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UY003', MESSAGE = 'USAPP_PUSH_REVOCATION_SPACE';
    END IF;
    RETURN NEW;
END
$revocation_guard$;

CREATE TRIGGER push_subscription_revocations_guard BEFORE INSERT ON push_subscription_revocations
    FOR EACH ROW EXECUTE FUNCTION usapp_push_revocation_guard();

CREATE FUNCTION usapp_push_command_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $command_guard$
DECLARE
    g push_subscriptions%ROWTYPE;
BEGIN
    IF NEW.user_id IS NULL OR NEW.generation_id IS NULL OR NEW.session_id IS NULL OR NEW.recorded_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33013::integer, hashtext(NEW.user_id::text));
    SELECT * INTO g FROM push_subscriptions WHERE id = NEW.generation_id;
    IF NOT FOUND OR g.user_id IS DISTINCT FROM NEW.user_id OR g.couple_id IS DISTINCT FROM NEW.couple_id
       OR g.session_id IS DISTINCT FROM NEW.session_id
       OR NOT usapp_push_session_valid(NEW.session_id, NEW.user_id)
       OR NEW.recorded_at < g.activated_at OR NEW.recorded_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UY004', MESSAGE = 'USAPP_PUSH_COMMAND_TARGET';
    END IF;
    -- A no-op registration may only point at a generation that is still unrevoked.
    IF NEW.result = 'already_active'
       AND EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UY004', MESSAGE = 'USAPP_PUSH_COMMAND_REVOKED';
    END IF;
    RETURN NEW;
END
$command_guard$;

CREATE TRIGGER push_registration_commands_guard BEFORE INSERT ON push_registration_commands
    FOR EACH ROW EXECUTE FUNCTION usapp_push_command_guard();

-- Deferred links, checked at commit:
--   * every generation has its creating command;
--   * a successor revocation (superseded / replaced / reowned) points at a generation on the
--     same endpoint created by the revoker, with the reason matching the actual change.
CREATE FUNCTION usapp_push_link_check() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $push_link$
DECLARE
    old_g push_subscriptions%ROWTYPE;
    new_g push_subscriptions%ROWTYPE;
    same_keys boolean;
BEGIN
    IF TG_TABLE_NAME = 'push_subscriptions' THEN
        IF NOT EXISTS (SELECT 1 FROM push_registration_commands c
                        WHERE c.generation_id = NEW.id AND c.result = 'created') THEN
            RAISE EXCEPTION USING ERRCODE = 'UY005', MESSAGE = 'USAPP_PUSH_GENERATION_COMMAND';
        END IF;
        RETURN NULL;
    END IF;
    IF NEW.caused_by IS NULL THEN
        RETURN NULL;
    END IF;
    SELECT * INTO old_g FROM push_subscriptions WHERE id = NEW.subscription_id;
    SELECT * INTO new_g FROM push_subscriptions WHERE id = NEW.caused_by;
    IF new_g.id IS NULL OR new_g.endpoint_hash IS DISTINCT FROM old_g.endpoint_hash
       OR new_g.user_id IS DISTINCT FROM NEW.revoked_by OR new_g.activated_at < NEW.revoked_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UY005', MESSAGE = 'USAPP_PUSH_SUCCESSOR';
    END IF;
    same_keys := ROW(new_g.p256dh, new_g.auth, new_g.vapid_key_id)
                 IS NOT DISTINCT FROM ROW(old_g.p256dh, old_g.auth, old_g.vapid_key_id);
    IF (NEW.reason = 'reowned' AND new_g.user_id = old_g.user_id)
       OR (NEW.reason = 'replaced' AND (new_g.user_id <> old_g.user_id OR same_keys))
       OR (NEW.reason = 'superseded' AND (new_g.user_id <> old_g.user_id OR NOT same_keys
                                          OR new_g.session_id = old_g.session_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UY005', MESSAGE = 'USAPP_PUSH_SUCCESSOR_REASON';
    END IF;
    RETURN NULL;
END
$push_link$;

CREATE CONSTRAINT TRIGGER push_subscriptions_command_link AFTER INSERT ON push_subscriptions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_push_link_check();
CREATE CONSTRAINT TRIGGER push_subscription_revocations_successor_link AFTER INSERT
    ON push_subscription_revocations DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.caused_by IS NOT NULL)
    EXECUTE FUNCTION usapp_push_link_check();

REVOKE ALL ON push_subscriptions, push_subscription_revocations, push_registration_commands
    FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON push_subscriptions, push_subscription_revocations, push_registration_commands
    TO usapp_api;

COMMIT;
