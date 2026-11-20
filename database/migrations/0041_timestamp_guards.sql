-- =====================================================================
-- 0041_timestamp_guards: guard-assigned timestamps and Push protection (docs/phase3-timestamp-guards.md). 0000-0040 stay frozen.
--
-- A column default is read before a BEFORE trigger, then compared with a later clock reading and with its source. The wall clock can move backwards, so
-- a valid write could be refused. These eight guards now assign the time themselves when the caller gives none: omitted, DEFAULT and explicit NULL are one
-- request, because the eight defaults are dropped and NOT NULL is checked after BEFORE triggers. The rule is
--
--     T = GREATEST(clock_timestamp(), every source this row must follow), read after the guard's locks
--
-- A value a caller does supply meets the original predicates and errors, unchanged. No margin or tolerance exists anywhere.
--
-- What this guarantees: T is not earlier than any declared source or than the clock reading, T is exactly one of those values, and time does not decrease
-- along a declared chain. What it does not: T may lead a clock that moved backwards, with no fixed bound, until the clock passes it.
--
-- Eligibility, TTL and deadline rules are not changed. The one new fact is notice_ahead_skipped, an internal attempt outcome that spends no provider budget:
-- what the dispatcher records if a notice is ahead of the clock when an attempt that is already claimed reaches its pre-send check.
-- No new table, lock or SQLSTATE. Rows are not rewritten. The guards keep having no fixed search_path.
-- =====================================================================

BEGIN;

DO $timestamp_guards_preflight$
BEGIN
    IF to_regprocedure('public.usapp_memory_eraser_violations()') IS NULL THEN
        RAISE EXCEPTION '0041_timestamp_guards requires 0040_memory_eraser_safety';
    END IF;
END
$timestamp_guards_preflight$;

ALTER TABLE messages ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE message_replies ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE reward_redemptions ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE reward_redemption_events ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE reward_redemption_fulfillments ALTER COLUMN fulfilled_at DROP DEFAULT;
ALTER TABLE notifications ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE notification_intents ALTER COLUMN created_at DROP DEFAULT;
ALTER TABLE push_deliveries ALTER COLUMN created_at DROP DEFAULT;

-- ---------------------------------------------------------------- messages (0017 body)
CREATE OR REPLACE FUNCTION usapp_message_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $message_guard$
BEGIN
    IF NEW.created_at IS NULL THEN
        NEW.created_at := clock_timestamp();
    ELSIF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_TIME';
    END IF;
    RETURN NEW;
END
$message_guard$;

-- The early return is for the other columns' own constraints only.
CREATE OR REPLACE FUNCTION usapp_message_reply_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $message_reply_guard$
DECLARE
    m messages%ROWTYPE;
BEGIN
    IF NEW.message_id IS NULL OR NEW.couple_id IS NULL OR NEW.author_id IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO m FROM messages WHERE id = NEW.message_id;
    IF NOT FOUND OR m.couple_id IS DISTINCT FROM NEW.couple_id OR m.recipient_id IS DISTINCT FROM NEW.author_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_REPLY_ACTOR';
    END IF;
    IF NEW.created_at IS NULL THEN
        NEW.created_at := GREATEST(clock_timestamp(), m.created_at);
    ELSIF NEW.created_at > clock_timestamp() OR NEW.created_at < m.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_REPLY_TIME';
    END IF;
    RETURN NEW;
END
$message_reply_guard$;

-- ---------------------------------------------------------------- redemptions (0016 bodies)
CREATE OR REPLACE FUNCTION usapp_redemption_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE p reward_products%ROWTYPE; r reward_product_revisions%ROWTYPE; o wallet_operations%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33007::integer,hashtext(NEW.product_id::text));
    SELECT * INTO p FROM reward_products WHERE id=NEW.product_id;
    IF NOT FOUND OR ROW(p.couple_id,p.creator_id,p.owner_id) IS DISTINCT FROM
        ROW(NEW.couple_id,NEW.creator_id,NEW.redeemer_id) THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_ACTOR';
    END IF;
    SELECT * INTO r FROM reward_product_revisions WHERE product_id=NEW.product_id ORDER BY version DESC LIMIT 1;
    IF NOT FOUND OR r.status <> 'active' OR r.version IS DISTINCT FROM NEW.product_version
       OR ROW(NEW.title,NEW.description,NEW.price_flowers,NEW.redemption_mode) IS DISTINCT FROM
          ROW(r.title,r.description,r.price_flowers,r.redemption_mode) THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_SNAPSHOT';
    END IF;
    IF NEW.redemption_mode='direct' THEN
        SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id;
        IF NOT FOUND OR ROW(o.operation_type,o.source_type,o.source_id,o.actor_user_id,o.couple_id)
            IS DISTINCT FROM ROW('REWARD_REDEMPTION'::text,'reward_redemption'::text,NEW.id::text,NEW.redeemer_id,NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_OPERATION';
        END IF;
    ELSIF NEW.redemption_mode <> 'request' OR NEW.operation_id IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_OPERATION';
    END IF;
    IF NEW.created_at IS NULL THEN
        NEW.created_at := GREATEST(clock_timestamp(), r.created_at);
    ELSIF NEW.created_at > clock_timestamp() OR NEW.created_at < r.created_at THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_TIME';
    END IF;
    RETURN NEW;
END $guard$;

-- After lock 33008, so the time also follows whoever held it. The floor includes the proposal a decision answers.
CREATE OR REPLACE FUNCTION usapp_redemption_event_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE s reward_redemption_state%ROWTYPE; p reward_redemption_events%ROWTYPE; o wallet_operations%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33008::integer,hashtext(NEW.redemption_id::text));
    SELECT * INTO s FROM reward_redemption_state WHERE id=NEW.redemption_id;
    IF NOT FOUND OR s.couple_id IS DISTINCT FROM NEW.couple_id
       OR NEW.actor_id NOT IN (s.creator_id,s.redeemer_id) THEN
        RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_ACTOR';
    END IF;
    IF NEW.created_at IS NULL THEN
        NEW.created_at := GREATEST(clock_timestamp(), s.created_at,
                                   (SELECT e.created_at FROM reward_redemption_events e WHERE e.id=NEW.proposal_id));
    ELSIF NEW.created_at < s.created_at OR NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_TIME';
    END IF;
    IF NEW.action IN ('confirm','reject','withdraw') THEN
        IF s.status <> 'pending_confirmation' OR
           (NEW.action IN ('confirm','reject') AND NEW.actor_id <> s.creator_id) OR
           (NEW.action='withdraw' AND NEW.actor_id <> s.redeemer_id) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_STATE';
        END IF;
    ELSIF NEW.action='cancel_propose' THEN
        IF s.status <> 'pending_fulfillment' OR s.cancellation_id IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_CANCEL_PENDING';
        END IF;
    ELSE
        SELECT * INTO p FROM reward_redemption_events WHERE id=NEW.proposal_id;
        IF NOT FOUND OR s.status <> 'pending_fulfillment' OR s.cancellation_id IS DISTINCT FROM NEW.proposal_id
           OR p.redemption_id IS DISTINCT FROM NEW.redemption_id OR p.action <> 'cancel_propose'
           OR NEW.created_at < p.created_at
           OR (NEW.action='cancel_withdraw' AND NEW.actor_id <> p.actor_id)
           OR (NEW.action IN ('cancel_approve','cancel_reject') AND NEW.actor_id=p.actor_id) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_CANCEL_DECISION';
        END IF;
    END IF;
    IF NEW.operation_id IS NOT NULL THEN
        SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id;
        IF NOT FOUND OR o.couple_id IS DISTINCT FROM NEW.couple_id OR o.source_type <> 'reward_redemption'
           OR o.source_id IS DISTINCT FROM NEW.redemption_id::text OR o.actor_user_id IS DISTINCT FROM NEW.actor_id
           OR o.operation_type IS DISTINCT FROM (CASE WHEN NEW.action='confirm' THEN 'REWARD_REDEMPTION' ELSE 'REDEMPTION_REFUND' END) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_OPERATION';
        END IF;
    END IF;
    RETURN NEW;
END $guard$;

-- The state check stays with the error it always had; only the time moved out of it.
CREATE OR REPLACE FUNCTION usapp_redemption_fulfillment_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE s reward_redemption_state%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33008::integer,hashtext(NEW.redemption_id::text));
    SELECT * INTO s FROM reward_redemption_state WHERE id=NEW.redemption_id;
    IF NOT FOUND OR s.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.fulfilled_by IS DISTINCT FROM s.creator_id THEN
        RAISE EXCEPTION USING ERRCODE='UM004',MESSAGE='USAPP_FULFILLMENT_ACTOR';
    END IF;
    IF s.status <> 'pending_fulfillment' THEN
        RAISE EXCEPTION USING ERRCODE='UM004',MESSAGE='USAPP_FULFILLMENT_STATE';
    END IF;
    IF NEW.fulfilled_at IS NULL THEN
        NEW.fulfilled_at := GREATEST(clock_timestamp(), s.created_at);
    ELSIF NEW.fulfilled_at < s.created_at OR NEW.fulfilled_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE='UM004',MESSAGE='USAPP_FULFILLMENT_STATE';
    END IF;
    RETURN NEW;
END $guard$;

-- ---------------------------------------------------------------- notifications (0025 body)
CREATE OR REPLACE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record; assigned boolean := NEW.created_at IS NULL; source_found boolean;
BEGIN
 IF NEW.event_type IN ('reminder.task', 'reminder.anniversary') THEN
   SELECT * INTO facts FROM usapp_notification_reminder_source(NEW.reminder_occurrence_id)
    WHERE event_type = NEW.event_type;
 ELSIF NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited', 'message.created',
                       'message.replied') THEN
   IF NEW.event_type = 'task.published' THEN
     family := 'task'; source := NEW.task_id;
   ELSIF NEW.event_type IN ('anniversary.created', 'anniversary.edited') THEN
     family := 'anniversary_revision'; source := NEW.anniversary_id;
   ELSIF NEW.event_type = 'message.created' THEN
     family := 'message'; source := NEW.message_id;
   ELSE
     family := 'message_reply'; source := NEW.message_reply_id;
   END IF;
   SELECT * INTO facts FROM usapp_notification_optional_source(family, source, NEW.anniversary_version);
 ELSE
   -- Event prefix determines the exact expected family; no untyped source or link is trusted.
   IF NEW.event_type IN ('redemption.created','redemption.requested') THEN
     family := 'redemption'; source := NEW.redemption_id;
   ELSIF NEW.event_type = 'redemption.fulfilled' THEN
     family := 'fulfillment'; source := NEW.redemption_id;
   ELSIF NEW.event_type IN ('redemption.confirm','redemption.reject','redemption.withdraw',
     'redemption.cancel_propose','redemption.cancel_approve','redemption.cancel_reject','redemption.cancel_withdraw') THEN
     family := 'redemption_event'; source := NEW.redemption_event_id;
   ELSIF NEW.event_type = 'task.change_proposed' THEN
     family := 'task_proposal'; source := NEW.task_change_proposal_id;
   ELSIF NEW.event_type IN ('task.change_approved','task.change_rejected','task.change_withdrawn') THEN
     family := 'task_decision'; source := NEW.task_change_proposal_id;
   ELSIF NEW.event_type = 'anniversary.edit_proposed' THEN
     family := 'anniversary_proposal'; source := NEW.anniversary_proposal_id;
   ELSIF NEW.event_type IN ('anniversary.edit_approved','anniversary.edit_rejected','anniversary.edit_withdrawn') THEN
     family := 'anniversary_decision'; source := NEW.anniversary_proposal_id;
   ELSE
     RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_EVENT';
   END IF;
   SELECT * INTO facts FROM usapp_notification_source(family, source);
 END IF;
 source_found := FOUND;
 IF assigned THEN
   NEW.created_at := GREATEST(clock_timestamp(), facts.occurred_at);
 END IF;
 IF NOT source_found OR ROW(NEW.event_type,NEW.couple_id,NEW.actor_id,NEW.recipient_id,NEW.target_kind,NEW.target_id)
   IS DISTINCT FROM ROW(facts.event_type,facts.couple_id,facts.actor_id,facts.recipient_id,facts.target_kind,facts.target_id)
   OR (NOT assigned AND (NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp())) THEN
   RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_SOURCE';
 END IF;
 RETURN NEW;
END
$guard$;

-- ---------------------------------------------------------------- notification intents (0020 body)
-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE OR REPLACE FUNCTION usapp_notification_intent_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $intent_guard$
DECLARE
    family text;
    source uuid;
    facts record;
    previous integer;
    assigned boolean := NEW.created_at IS NULL;
    source_found boolean;
BEGIN
    family := CASE NEW.operation
                WHEN 'task_publish' THEN 'task'
                WHEN 'message_create' THEN 'message'
                WHEN 'message_reply' THEN 'message_reply'
                ELSE 'anniversary_revision' END;
    source := COALESCE(NEW.task_id, NEW.anniversary_id, NEW.message_id, NEW.message_reply_id);
    SELECT * INTO facts FROM usapp_notification_optional_source(family, source, NEW.anniversary_version);
    source_found := FOUND;
    IF assigned THEN
        NEW.created_at := GREATEST(clock_timestamp(), facts.occurred_at);
    END IF;
    IF NOT source_found OR facts.operation IS DISTINCT FROM NEW.operation
       OR facts.couple_id IS DISTINCT FROM NEW.couple_id OR facts.actor_id IS DISTINCT FROM NEW.actor_id
       OR facts.recipient_id IS DISTINCT FROM NEW.recipient_id
       OR (NOT assigned AND (NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp())) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN104', MESSAGE = 'USAPP_NOTIFICATION_INTENT_SOURCE';
    END IF;
    IF NEW.preference_seq IS NOT NULL THEN
        PERFORM pg_advisory_xact_lock(33012::integer, hashtext(NEW.actor_id::text || ':' || NEW.operation));
        SELECT max(preference_seq) INTO previous FROM notification_intents
         WHERE actor_id = NEW.actor_id AND operation = NEW.operation AND preference_seq IS NOT NULL;
        IF NEW.preference_seq::bigint <> COALESCE(previous, 0)::bigint + 1 THEN
            RAISE EXCEPTION USING ERRCODE = 'UN104', MESSAGE = 'USAPP_NOTIFICATION_INTENT_PREFERENCE';
        END IF;
    END IF;
    IF NEW.notification_id IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM notifications n
         WHERE n.id = NEW.notification_id AND n.event_type = facts.event_type AND n.actor_id = NEW.actor_id
           AND n.task_id IS NOT DISTINCT FROM NEW.task_id
           AND n.anniversary_id IS NOT DISTINCT FROM NEW.anniversary_id
           AND n.anniversary_version IS NOT DISTINCT FROM NEW.anniversary_version
           AND n.message_id IS NOT DISTINCT FROM NEW.message_id
           AND n.message_reply_id IS NOT DISTINCT FROM NEW.message_reply_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN104', MESSAGE = 'USAPP_NOTIFICATION_INTENT_LINK';
    END IF;
    RETURN NEW;
END
$intent_guard$;

-- ---------------------------------------------------------------- push deliveries (0023 body)
-- Eligibility and the 24 h TTL first, exactly as before. Only the delivery's own time follows the common rule.
CREATE OR REPLACE FUNCTION usapp_push_delivery_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
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
       OR notice_at <= clock_timestamp() - interval '24 hours' THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ003', MESSAGE = 'USAPP_PUSH_DELIVERY_ELIGIBILITY';
    END IF;
    IF NEW.created_at IS NULL THEN
        NEW.created_at := GREATEST(clock_timestamp(), notice_at);
    ELSIF NEW.created_at > clock_timestamp() OR NEW.created_at < notice_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UZ003', MESSAGE = 'USAPP_PUSH_DELIVERY_ELIGIBILITY';
    END IF;
    RETURN NEW;
END
$delivery_guard$;

-- ---------------------------------------------------------------- the one new closed value
-- Internal: nothing reached a provider, so it can never spend the budget, and it decides no terminal state.
ALTER TABLE push_attempt_observations DROP CONSTRAINT push_attempt_observations_class_rule;
ALTER TABLE push_attempt_observations
    ADD CONSTRAINT push_attempt_observations_class_rule CHECK (
        (class = 'provider' AND outcome IN ('accepted', 'gone', 'rejected', 'transient', 'unknown',
                                            'provider_auth_failed'))
        OR (class = 'internal' AND outcome IN ('worker_auth_failed', 'transport_unavailable', 'busy',
                                               'inactive_skipped', 'ttl_skipped', 'cutover_skipped',
                                               'control_skipped', 'terminal_skipped', 'lease_lost_skipped',
                                               'source_stale_skipped', 'notice_ahead_skipped')));

COMMIT;
