-- =====================================================================
-- 0020_notification_choices: Phase 2 P2-3b optional notifications and remembered choices
-- (docs/phase2-notifications.md Section 12).
--
-- Five ordinary operations may notify the partner: task publication, anniversary creation,
-- anniversary creator direct edit, message creation and message reply. Every NEW such
-- operation records one immutable notification_intents row with the original decision:
--   omitted            -> choice_explicit = false, requested = false, no preference, no notice;
--   explicit, partner  -> preference_seq allocated per (actor, operation); true links one notice;
--   explicit, none     -> (anniversaries only) no preference, no notice.
-- The remembered default is the intent with the highest preference_seq. No mutable table.
-- notifications gains typed optional sources; the 0019 guard is extended (not edited) so
-- optional notices resolve from immutable business rows and must link their intent.
-- Lock: 33012 per (actor, operation) for preference sequences, after all domain locks.
-- SQLSTATE: UN101 append-only (0019), UN102 notice source (0019), UN104 intent guard,
-- UN105 deferred optional notice -> intent link.
-- =====================================================================

BEGIN;

DO $choice_preflight$
BEGIN
    IF to_regclass('public.notification_reads') IS NULL THEN
        RAISE EXCEPTION '0020_notification_choices requires 0019_notifications';
    END IF;
END
$choice_preflight$;

-- 1. Optional typed sources on notifications. A revision is (anniversary_id, version).
ALTER TABLE notifications
    ADD COLUMN task_id uuid,
    ADD COLUMN anniversary_id uuid,
    ADD COLUMN anniversary_version integer,
    ADD COLUMN message_id uuid,
    ADD COLUMN message_reply_id uuid;

ALTER TABLE notifications
    ADD CONSTRAINT notifications_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notifications_anniversary_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notifications_anniversary_revision_fk FOREIGN KEY (anniversary_id, anniversary_version)
        REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT,
    ADD CONSTRAINT notifications_message_fk FOREIGN KEY (message_id, couple_id)
        REFERENCES messages (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notifications_message_reply_fk FOREIGN KEY (message_reply_id)
        REFERENCES message_replies (message_id) ON DELETE RESTRICT;

-- 0019 declared its one-source and target-kind checks without names; replace exactly those two.
DO $choice_replace_checks$
DECLARE
    c record;
    dropped integer := 0;
BEGIN
    FOR c IN SELECT conname FROM pg_constraint
              WHERE conrelid = 'public.notifications'::regclass AND contype = 'c'
                AND (pg_get_constraintdef(oid) LIKE '%num_nonnulls(%'
                     OR pg_get_constraintdef(oid) LIKE '%target_kind%')
    LOOP
        EXECUTE format('ALTER TABLE notifications DROP CONSTRAINT %I', c.conname);
        dropped := dropped + 1;
    END LOOP;
    IF dropped <> 2 THEN
        RAISE EXCEPTION '0020_notification_choices expected 2 replaceable notification checks, found %', dropped;
    END IF;
END
$choice_replace_checks$;

ALTER TABLE notifications
    ADD CONSTRAINT notifications_target_kind_valid
        CHECK (target_kind IN ('task', 'reward', 'anniversary', 'message')),
    ADD CONSTRAINT notifications_one_source
        CHECK (num_nonnulls(redemption_id, redemption_event_id, task_change_proposal_id, anniversary_proposal_id,
                            task_id, anniversary_id, message_id, message_reply_id) = 1),
    ADD CONSTRAINT notifications_anniversary_version_pair
        CHECK ((anniversary_id IS NULL) = (anniversary_version IS NULL));

CREATE UNIQUE INDEX notifications_task_source_unique ON notifications (event_type, task_id, recipient_id)
    WHERE task_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_revision_source_unique
    ON notifications (event_type, anniversary_id, anniversary_version, recipient_id)
    WHERE anniversary_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_message_source_unique ON notifications (event_type, message_id, recipient_id)
    WHERE message_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_reply_source_unique ON notifications (event_type, message_reply_id, recipient_id)
    WHERE message_reply_id IS NOT NULL;

-- 2. Closed resolver for the five ordinary operations. recipient_id is the partner at the time
-- of the operation; NULL only for an anniversary revision in a space without a partner.
-- Task: a real root publication only (no lazy period, no M6 successor).
CREATE FUNCTION usapp_notification_optional_source(family text, source uuid, source_version integer)
RETURNS TABLE(event_type text, operation text, couple_id uuid, actor_id uuid, recipient_id uuid,
              target_kind text, target_id uuid, occurred_at timestamptz)
LANGUAGE sql STABLE AS $optional_source$
 SELECT 'task.published', 'task_publish', t.couple_id, t.creator_id, t.assignee_id, 'task', t.id, t.created_at
   FROM tasks t
  WHERE family = 'task' AND t.id = source AND t.recurrence_id IS NULL
    AND NOT EXISTS (SELECT 1 FROM task_change_decisions d WHERE d.successor_task_id = t.id)
 UNION ALL
 SELECT CASE r.action WHEN 'create' THEN 'anniversary.created' ELSE 'anniversary.edited' END,
        CASE r.action WHEN 'create' THEN 'anniversary_create' ELSE 'anniversary_edit' END,
        r.couple_id, r.actor_id,
        (SELECT m.user_id FROM couple_members m
          WHERE m.couple_id = r.couple_id AND m.user_id <> r.actor_id ORDER BY m.slot LIMIT 1),
        'anniversary', r.anniversary_id, r.created_at
   FROM anniversary_revisions r
  WHERE family = 'anniversary_revision' AND r.anniversary_id = source AND r.version = source_version
    AND r.action IN ('create', 'edit')
 UNION ALL
 SELECT 'message.created', 'message_create', d.couple_id, d.author_id, d.recipient_id, 'message', d.id, d.created_at
   FROM messages d
  WHERE family = 'message' AND d.id = source
 UNION ALL
 SELECT 'message.replied', 'message_reply', r.couple_id, r.author_id, d.author_id, 'message', d.id, r.created_at
   FROM message_replies r JOIN messages d ON d.id = r.message_id
  WHERE family = 'message_reply' AND r.message_id = source
$optional_source$;

-- 3. Notification guard: the 0019 mandatory registry unchanged, plus the five optional events.
CREATE OR REPLACE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record;
BEGIN
 IF NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited', 'message.created',
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
 IF NOT FOUND OR ROW(NEW.event_type,NEW.couple_id,NEW.actor_id,NEW.recipient_id,NEW.target_kind,NEW.target_id)
   IS DISTINCT FROM ROW(facts.event_type,facts.couple_id,facts.actor_id,facts.recipient_id,facts.target_kind,facts.target_id)
   OR NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp() THEN
   RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_SOURCE';
 END IF;
 RETURN NEW;
END
$guard$;

-- 4. Immutable per-operation decision evidence and remembered choices.
CREATE TABLE notification_intents (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    actor_id uuid NOT NULL,
    operation text NOT NULL,
    task_id uuid,
    anniversary_id uuid,
    anniversary_version integer,
    message_id uuid,
    message_reply_id uuid,
    choice_explicit boolean NOT NULL,
    requested boolean NOT NULL,
    recipient_id uuid,
    notification_id uuid,
    preference_seq integer,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT notification_intents_pkey PRIMARY KEY (id),
    CONSTRAINT notification_intents_notification_unique UNIQUE (notification_id),
    CONSTRAINT notification_intents_operation_valid CHECK (operation IN
        ('task_publish', 'anniversary_create', 'anniversary_edit', 'message_create', 'message_reply')),
    -- Exactly the reference family of the operation.
    CONSTRAINT notification_intents_family CHECK (
        (operation = 'task_publish' AND task_id IS NOT NULL
            AND num_nonnulls(anniversary_id, anniversary_version, message_id, message_reply_id) = 0)
        OR (operation IN ('anniversary_create', 'anniversary_edit') AND anniversary_id IS NOT NULL
            AND anniversary_version IS NOT NULL AND num_nonnulls(task_id, message_id, message_reply_id) = 0)
        OR (operation = 'message_create' AND message_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_reply_id) = 0)
        OR (operation = 'message_reply' AND message_reply_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_id) = 0)),
    CONSTRAINT notification_intents_revision_kind CHECK (
        (operation <> 'anniversary_create' OR anniversary_version = 1)
        AND (operation <> 'anniversary_edit' OR anniversary_version > 1)),
    -- requested only by an explicit true; a notice exactly when requested with a recipient;
    -- a preference sequence exactly for an explicit choice with a recipient.
    CONSTRAINT notification_intents_requested_explicit CHECK (NOT requested OR choice_explicit),
    CONSTRAINT notification_intents_notice_presence CHECK (
        (notification_id IS NOT NULL) = (requested AND recipient_id IS NOT NULL)),
    CONSTRAINT notification_intents_preference_presence CHECK (
        (preference_seq IS NOT NULL) = (choice_explicit AND recipient_id IS NOT NULL)),
    CONSTRAINT notification_intents_preference_seq_valid CHECK (preference_seq IS NULL OR preference_seq >= 1),
    CONSTRAINT notification_intents_not_self CHECK (recipient_id IS NULL OR recipient_id <> actor_id),
    CONSTRAINT notification_intents_actor_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_recipient_fk FOREIGN KEY (couple_id, recipient_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_task_fk FOREIGN KEY (task_id, couple_id)
        REFERENCES tasks (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_anniversary_fk FOREIGN KEY (anniversary_id, couple_id)
        REFERENCES anniversaries (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_revision_fk FOREIGN KEY (anniversary_id, anniversary_version)
        REFERENCES anniversary_revisions (anniversary_id, version) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_message_fk FOREIGN KEY (message_id, couple_id)
        REFERENCES messages (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_reply_fk FOREIGN KEY (message_reply_id)
        REFERENCES message_replies (message_id) ON DELETE RESTRICT,
    CONSTRAINT notification_intents_notice_fk FOREIGN KEY (notification_id, couple_id, recipient_id)
        REFERENCES notifications (id, couple_id, recipient_id) ON DELETE RESTRICT
);

-- One intent per operation source.
CREATE UNIQUE INDEX notification_intents_task_unique ON notification_intents (task_id)
    WHERE task_id IS NOT NULL;
CREATE UNIQUE INDEX notification_intents_revision_unique ON notification_intents (anniversary_id, anniversary_version)
    WHERE anniversary_id IS NOT NULL;
CREATE UNIQUE INDEX notification_intents_message_unique ON notification_intents (message_id)
    WHERE message_id IS NOT NULL;
CREATE UNIQUE INDEX notification_intents_reply_unique ON notification_intents (message_reply_id)
    WHERE message_reply_id IS NOT NULL;
-- Contiguous preference sequence per (actor, operation); also serves the latest-choice lookup.
CREATE UNIQUE INDEX notification_intents_preference_unique
    ON notification_intents (actor_id, operation, preference_seq)
    WHERE preference_seq IS NOT NULL;

CREATE TRIGGER notification_intents_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON notification_intents
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_notification_reject_mutation();

-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE FUNCTION usapp_notification_intent_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $intent_guard$
DECLARE
    family text;
    source uuid;
    facts record;
    previous integer;
BEGIN
    family := CASE NEW.operation
                WHEN 'task_publish' THEN 'task'
                WHEN 'message_create' THEN 'message'
                WHEN 'message_reply' THEN 'message_reply'
                ELSE 'anniversary_revision' END;
    source := COALESCE(NEW.task_id, NEW.anniversary_id, NEW.message_id, NEW.message_reply_id);
    SELECT * INTO facts FROM usapp_notification_optional_source(family, source, NEW.anniversary_version);
    IF NOT FOUND OR facts.operation IS DISTINCT FROM NEW.operation
       OR facts.couple_id IS DISTINCT FROM NEW.couple_id OR facts.actor_id IS DISTINCT FROM NEW.actor_id
       OR facts.recipient_id IS DISTINCT FROM NEW.recipient_id
       OR NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp() THEN
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

CREATE TRIGGER notification_intents_guard BEFORE INSERT ON notification_intents
    FOR EACH ROW EXECUTE FUNCTION usapp_notification_intent_guard();

-- 5. An optional notice exists only with the requesting intent that links it (checked at commit).
CREATE FUNCTION usapp_notification_intent_link() RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $intent_link$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM notification_intents i WHERE i.notification_id = NEW.id AND i.requested) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN105', MESSAGE = 'USAPP_NOTIFICATION_OPTIONAL_UNLINKED';
    END IF;
    RETURN NULL;
END
$intent_link$;

CREATE CONSTRAINT TRIGGER notifications_optional_intent AFTER INSERT ON notifications
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited',
                                         'message.created', 'message.replied'))
    EXECUTE FUNCTION usapp_notification_intent_link();

REVOKE ALL ON notification_intents FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON notification_intents TO usapp_api;

COMMIT;
