-- Registration-command terminal timestamp only; 0000-0047 remain frozen.
-- Like 0041, omitted/DEFAULT/NULL asks the guard to assign once after its lock and source read.
-- Supplied timestamps retain the original strict checks. No changes to subscription activation,
-- revocations, dispatch cutover/eligibility/TTL, keys, ownership, privileges or existing rows.
BEGIN;

DO $preflight$
BEGIN
    IF to_regclass('public.push_registration_commands') IS NULL
       OR to_regprocedure('public.usapp_push_command_guard()') IS NULL THEN
        RAISE EXCEPTION '0048_push_command_timestamp requires 0022_push_subscriptions';
    END IF;
END
$preflight$;

ALTER TABLE push_registration_commands ALTER COLUMN recorded_at DROP DEFAULT;

CREATE OR REPLACE FUNCTION usapp_push_command_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $command_guard$
DECLARE
    g push_subscriptions%ROWTYPE;
BEGIN
    IF NEW.user_id IS NULL OR NEW.generation_id IS NULL OR NEW.session_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33013::integer, hashtext(NEW.user_id::text));
    SELECT * INTO g FROM push_subscriptions WHERE id = NEW.generation_id;
    IF NOT FOUND OR g.user_id IS DISTINCT FROM NEW.user_id OR g.couple_id IS DISTINCT FROM NEW.couple_id
       OR g.session_id IS DISTINCT FROM NEW.session_id
       OR NOT usapp_push_session_valid(NEW.session_id, NEW.user_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UY004', MESSAGE = 'USAPP_PUSH_COMMAND_TARGET';
    END IF;
    IF NEW.recorded_at IS NULL THEN
        NEW.recorded_at := GREATEST(clock_timestamp(), g.activated_at);
    ELSIF NEW.recorded_at < g.activated_at OR NEW.recorded_at > clock_timestamp() THEN
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

COMMIT;
