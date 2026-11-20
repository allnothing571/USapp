-- =====================================================================
-- 0043_phase3_notifications: D20 opt-in partner notices for three Phase 3 operations (docs/phase3-plan.md Section 30).
--
-- Owner-approved D20 (2026-11-09): creating a photo memory, creating a shared plan, and adding one batch of photos to an album may
-- each notify the partner, default off. At most one notice per operation (one per album batch, never per photo). No other Phase 3
-- operation notifies. Notices carry fixed generic text only; the source ids below stay internal.
--
-- This extends the 0020 optional-notification pipeline by addition only:
--   * notifications and notification_intents gain typed sources: memory_id, shared_plan_id, album_batch_id;
--   * the closed operation, family, target-kind and one-source checks are replaced with supersets;
--   * usapp_notification_optional_source gains three branches; a memory created by plan conversion never resolves;
--   * usapp_notification_guard (0041 body) and usapp_notification_intent_guard (0041 body) are replaced with those bodies
--     unchanged plus the three branches; their time rule is exactly the 0041 rule, nothing else is changed;
--   * the deferred optional-notice link trigger covers the three new events.
-- The three new operations never allocate a remembered preference (preference_seq stays NULL): D20 makes each form default off,
-- so nothing is remembered for them. The five 0020 operations keep their rule unchanged.
-- No new lock, table or SQLSTATE. 0000-0042 stay frozen.
-- =====================================================================

BEGIN;

DO $phase3_notifications_preflight$
BEGIN
    IF to_regclass('public.deletion_recovery_attempts') IS NULL THEN
        RAISE EXCEPTION '0043_phase3_notifications requires 0042_deletion_recovery';
    END IF;
END
$phase3_notifications_preflight$;

-- ---------------------------------------------------------------- notifications: three typed sources
ALTER TABLE notifications
    ADD COLUMN memory_id uuid,
    ADD COLUMN shared_plan_id uuid,
    ADD COLUMN album_batch_id uuid;

ALTER TABLE notifications
    ADD CONSTRAINT notifications_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notifications_shared_plan_fk FOREIGN KEY (shared_plan_id, couple_id)
        REFERENCES shared_plans (id, couple_id) ON DELETE RESTRICT,
    -- The batch's space is checked by the guard against the batch row, as 0020 does for message replies.
    ADD CONSTRAINT notifications_album_batch_fk FOREIGN KEY (album_batch_id)
        REFERENCES album_photo_batches (id) ON DELETE RESTRICT;

ALTER TABLE notifications DROP CONSTRAINT notifications_one_source;
ALTER TABLE notifications
    ADD CONSTRAINT notifications_one_source
        CHECK (num_nonnulls(redemption_id, redemption_event_id, task_change_proposal_id, anniversary_proposal_id,
                            task_id, anniversary_id, message_id, message_reply_id, reminder_occurrence_id,
                            memory_id, shared_plan_id, album_batch_id) = 1);
ALTER TABLE notifications DROP CONSTRAINT notifications_target_kind_valid;
ALTER TABLE notifications
    ADD CONSTRAINT notifications_target_kind_valid
        CHECK (target_kind IN ('task', 'reward', 'anniversary', 'message', 'memory', 'shared_plan', 'album'));

CREATE UNIQUE INDEX notifications_memory_source_unique ON notifications (event_type, memory_id, recipient_id)
    WHERE memory_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_shared_plan_source_unique ON notifications (event_type, shared_plan_id, recipient_id)
    WHERE shared_plan_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_album_batch_source_unique ON notifications (event_type, album_batch_id, recipient_id)
    WHERE album_batch_id IS NOT NULL;

-- ---------------------------------------------------------------- notification_intents: the same three sources
ALTER TABLE notification_intents
    ADD COLUMN memory_id uuid,
    ADD COLUMN shared_plan_id uuid,
    ADD COLUMN album_batch_id uuid;

ALTER TABLE notification_intents
    ADD CONSTRAINT notification_intents_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notification_intents_shared_plan_fk FOREIGN KEY (shared_plan_id, couple_id)
        REFERENCES shared_plans (id, couple_id) ON DELETE RESTRICT,
    ADD CONSTRAINT notification_intents_album_batch_fk FOREIGN KEY (album_batch_id)
        REFERENCES album_photo_batches (id) ON DELETE RESTRICT;

ALTER TABLE notification_intents DROP CONSTRAINT notification_intents_operation_valid;
ALTER TABLE notification_intents
    ADD CONSTRAINT notification_intents_operation_valid CHECK (operation IN
        ('task_publish', 'anniversary_create', 'anniversary_edit', 'message_create', 'message_reply',
         'memory_create', 'shared_plan_create', 'album_photos_add'));

ALTER TABLE notification_intents DROP CONSTRAINT notification_intents_family;
ALTER TABLE notification_intents
    ADD CONSTRAINT notification_intents_family CHECK (
        (operation = 'task_publish' AND task_id IS NOT NULL
            AND num_nonnulls(anniversary_id, anniversary_version, message_id, message_reply_id,
                             memory_id, shared_plan_id, album_batch_id) = 0)
        OR (operation IN ('anniversary_create', 'anniversary_edit') AND anniversary_id IS NOT NULL
            AND anniversary_version IS NOT NULL
            AND num_nonnulls(task_id, message_id, message_reply_id, memory_id, shared_plan_id, album_batch_id) = 0)
        OR (operation = 'message_create' AND message_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_reply_id,
                             memory_id, shared_plan_id, album_batch_id) = 0)
        OR (operation = 'message_reply' AND message_reply_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_id,
                             memory_id, shared_plan_id, album_batch_id) = 0)
        OR (operation = 'memory_create' AND memory_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_id, message_reply_id,
                             shared_plan_id, album_batch_id) = 0)
        OR (operation = 'shared_plan_create' AND shared_plan_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_id, message_reply_id,
                             memory_id, album_batch_id) = 0)
        OR (operation = 'album_photos_add' AND album_batch_id IS NOT NULL
            AND num_nonnulls(task_id, anniversary_id, anniversary_version, message_id, message_reply_id,
                             memory_id, shared_plan_id) = 0));

-- The five 0020 operations keep their rule; the three D20 operations never remember a choice.
ALTER TABLE notification_intents DROP CONSTRAINT notification_intents_preference_presence;
ALTER TABLE notification_intents
    ADD CONSTRAINT notification_intents_preference_presence CHECK (
        (preference_seq IS NOT NULL) = (choice_explicit AND recipient_id IS NOT NULL
            AND operation IN ('task_publish', 'anniversary_create', 'anniversary_edit', 'message_create', 'message_reply')));

CREATE UNIQUE INDEX notification_intents_memory_unique ON notification_intents (memory_id)
    WHERE memory_id IS NOT NULL;
CREATE UNIQUE INDEX notification_intents_shared_plan_unique ON notification_intents (shared_plan_id)
    WHERE shared_plan_id IS NOT NULL;
CREATE UNIQUE INDEX notification_intents_album_batch_unique ON notification_intents (album_batch_id)
    WHERE album_batch_id IS NOT NULL;

-- ---------------------------------------------------------------- the closed resolver: 0020 body plus three branches
-- recipient_id is the other member at the time of the operation, or NULL in a space without a partner.
-- A memory made by plan conversion (shared_plan_memory_links.mode = 'create') never resolves: conversion does not notify.
CREATE OR REPLACE FUNCTION usapp_notification_optional_source(family text, source uuid, source_version integer)
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
 UNION ALL
 SELECT 'memory.created', 'memory_create', mm.couple_id, mm.creator_id,
        (SELECT m.user_id FROM couple_members m
          WHERE m.couple_id = mm.couple_id AND m.user_id <> mm.creator_id ORDER BY m.slot LIMIT 1),
        'memory', mm.id, mm.created_at
   FROM memories mm
  WHERE family = 'memory' AND mm.id = source
    AND NOT EXISTS (SELECT 1 FROM shared_plan_memory_links l WHERE l.memory_id = mm.id AND l.mode = 'create')
 UNION ALL
 SELECT 'shared_plan.created', 'shared_plan_create', p.couple_id, p.creator_id,
        (SELECT m.user_id FROM couple_members m
          WHERE m.couple_id = p.couple_id AND m.user_id <> p.creator_id ORDER BY m.slot LIMIT 1),
        'shared_plan', p.id, p.created_at
   FROM shared_plans p
  WHERE family = 'shared_plan' AND p.id = source
 UNION ALL
 SELECT 'album.photos_added', 'album_photos_add', b.couple_id, b.added_by,
        (SELECT m.user_id FROM couple_members m
          WHERE m.couple_id = b.couple_id AND m.user_id <> b.added_by ORDER BY m.slot LIMIT 1),
        'album', b.album_id, b.created_at
   FROM album_photo_batches b
  WHERE family = 'album_batch' AND b.id = source
$optional_source$;

-- ---------------------------------------------------------------- notifications (0041 body plus three optional events)
CREATE OR REPLACE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record; assigned boolean := NEW.created_at IS NULL; source_found boolean;
BEGIN
 IF NEW.event_type IN ('reminder.task', 'reminder.anniversary') THEN
   SELECT * INTO facts FROM usapp_notification_reminder_source(NEW.reminder_occurrence_id)
    WHERE event_type = NEW.event_type;
 ELSIF NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited', 'message.created',
                       'message.replied', 'memory.created', 'shared_plan.created', 'album.photos_added') THEN
   IF NEW.event_type = 'task.published' THEN
     family := 'task'; source := NEW.task_id;
   ELSIF NEW.event_type IN ('anniversary.created', 'anniversary.edited') THEN
     family := 'anniversary_revision'; source := NEW.anniversary_id;
   ELSIF NEW.event_type = 'message.created' THEN
     family := 'message'; source := NEW.message_id;
   ELSIF NEW.event_type = 'memory.created' THEN
     family := 'memory'; source := NEW.memory_id;
   ELSIF NEW.event_type = 'shared_plan.created' THEN
     family := 'shared_plan'; source := NEW.shared_plan_id;
   ELSIF NEW.event_type = 'album.photos_added' THEN
     family := 'album_batch'; source := NEW.album_batch_id;
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

-- ---------------------------------------------------------------- notification intents (0041 body plus three families)
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
                WHEN 'memory_create' THEN 'memory'
                WHEN 'shared_plan_create' THEN 'shared_plan'
                WHEN 'album_photos_add' THEN 'album_batch'
                ELSE 'anniversary_revision' END;
    source := COALESCE(NEW.task_id, NEW.anniversary_id, NEW.message_id, NEW.message_reply_id,
                       NEW.memory_id, NEW.shared_plan_id, NEW.album_batch_id);
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
           AND n.message_reply_id IS NOT DISTINCT FROM NEW.message_reply_id
           AND n.memory_id IS NOT DISTINCT FROM NEW.memory_id
           AND n.shared_plan_id IS NOT DISTINCT FROM NEW.shared_plan_id
           AND n.album_batch_id IS NOT DISTINCT FROM NEW.album_batch_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UN104', MESSAGE = 'USAPP_NOTIFICATION_INTENT_LINK';
    END IF;
    RETURN NEW;
END
$intent_guard$;

-- ---------------------------------------------------------------- an optional notice exists only with its requesting intent
DROP TRIGGER notifications_optional_intent ON notifications;
CREATE CONSTRAINT TRIGGER notifications_optional_intent AFTER INSERT ON notifications
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.event_type IN ('task.published', 'anniversary.created', 'anniversary.edited',
                                         'message.created', 'message.replied', 'memory.created',
                                         'shared_plan.created', 'album.photos_added'))
    EXECUTE FUNCTION usapp_notification_intent_link();

COMMIT;
