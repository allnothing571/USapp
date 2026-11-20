-- P2-3a: mandatory in-app notifications. No backfill, optional choices or Push.
BEGIN;

DO $preflight$
BEGIN
  IF to_regclass('public.anniversary_edit_decisions') IS NULL THEN
    RAISE EXCEPTION '0019_notifications requires 0018_anniversaries';
  END IF;
END
$preflight$;

-- Closed registry. Sources are immutable domain rows, not client-authored payloads.
-- No SECURITY DEFINER: the existing application SELECT grants are required.
CREATE FUNCTION usapp_notification_source(family text, source uuid)
RETURNS TABLE(event_type text, couple_id uuid, actor_id uuid, recipient_id uuid,
              target_kind text, target_id uuid, occurred_at timestamptz)
LANGUAGE sql STABLE AS $source$
 SELECT CASE r.redemption_mode WHEN 'request' THEN 'redemption.requested' ELSE 'redemption.created' END,
        r.couple_id, r.redeemer_id, r.creator_id, 'reward', r.id, r.created_at
   FROM reward_redemptions r WHERE family = 'redemption' AND r.id = source
 UNION ALL
 SELECT 'redemption.' || e.action, e.couple_id, e.actor_id,
        CASE WHEN e.actor_id = r.creator_id THEN r.redeemer_id ELSE r.creator_id END,
        'reward', r.id, e.created_at
   FROM reward_redemption_events e JOIN reward_redemptions r ON r.id = e.redemption_id
  WHERE family = 'redemption_event' AND e.id = source
 UNION ALL
 SELECT 'redemption.fulfilled', r.couple_id, f.fulfilled_by, r.redeemer_id,
        'reward', r.id, f.fulfilled_at
   FROM reward_redemption_fulfillments f JOIN reward_redemptions r ON r.id = f.redemption_id
  WHERE family = 'fulfillment' AND f.redemption_id = source
 UNION ALL
 SELECT 'task.change_proposed', p.couple_id, p.proposer_id,
        CASE WHEN p.proposer_id = t.creator_id THEN t.assignee_id ELSE t.creator_id END,
        'task', t.id, p.created_at
   FROM task_change_proposals p JOIN tasks t ON t.id = p.task_id
  WHERE family = 'task_proposal' AND p.id = source
 UNION ALL
 SELECT 'task.change_' || d.outcome, p.couple_id, d.decided_by,
        CASE WHEN d.decided_by = t.creator_id THEN t.assignee_id ELSE t.creator_id END,
        'task', t.id, d.decided_at
   FROM task_change_decisions d JOIN task_change_proposals p ON p.id = d.proposal_id
   JOIN tasks t ON t.id = p.task_id
  WHERE family = 'task_decision' AND d.proposal_id = source
 UNION ALL
 SELECT 'anniversary.edit_proposed', p.couple_id, p.proposer_id, a.creator_id,
        'anniversary', a.id, p.created_at
   FROM anniversary_edit_proposals p JOIN anniversaries a ON a.id = p.anniversary_id
  WHERE family = 'anniversary_proposal' AND p.id = source
 UNION ALL
 SELECT 'anniversary.edit_' || d.outcome, p.couple_id, d.decided_by,
        CASE WHEN d.decided_by = a.creator_id THEN p.proposer_id ELSE a.creator_id END,
        'anniversary', a.id, d.decided_at
   FROM anniversary_edit_decisions d JOIN anniversary_edit_proposals p ON p.id = d.proposal_id
   JOIN anniversaries a ON a.id = p.anniversary_id
  WHERE family = 'anniversary_decision' AND d.proposal_id = source
$source$;

CREATE TABLE notifications (
 id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
 couple_id uuid NOT NULL,
 actor_id uuid NOT NULL,
 recipient_id uuid NOT NULL,
 event_type text NOT NULL,
 redemption_id uuid,
 redemption_event_id uuid REFERENCES reward_redemption_events(id) ON DELETE RESTRICT,
 task_change_proposal_id uuid,
 anniversary_proposal_id uuid,
 target_kind text NOT NULL CHECK (target_kind IN ('task','reward','anniversary')),
 target_id uuid NOT NULL,
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 UNIQUE(id, couple_id, recipient_id),
 FOREIGN KEY(couple_id, actor_id) REFERENCES couple_members(couple_id, user_id) ON DELETE RESTRICT,
 FOREIGN KEY(couple_id, recipient_id) REFERENCES couple_members(couple_id, user_id) ON DELETE RESTRICT,
 FOREIGN KEY(redemption_id, couple_id) REFERENCES reward_redemptions(id, couple_id) ON DELETE RESTRICT,
 FOREIGN KEY(task_change_proposal_id, couple_id) REFERENCES task_change_proposals(id, couple_id) ON DELETE RESTRICT,
 FOREIGN KEY(anniversary_proposal_id, couple_id) REFERENCES anniversary_edit_proposals(id, couple_id) ON DELETE RESTRICT,
 CHECK (actor_id <> recipient_id),
 CHECK (num_nonnulls(redemption_id, redemption_event_id, task_change_proposal_id, anniversary_proposal_id) = 1)
);
CREATE INDEX notifications_inbox_idx ON notifications(recipient_id, created_at DESC, id DESC);
CREATE UNIQUE INDEX notifications_redemption_unique ON notifications(event_type, redemption_id, recipient_id)
 WHERE redemption_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_event_unique ON notifications(event_type, redemption_event_id, recipient_id)
 WHERE redemption_event_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_task_unique ON notifications(event_type, task_change_proposal_id, recipient_id)
 WHERE task_change_proposal_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_anniversary_unique ON notifications(event_type, anniversary_proposal_id, recipient_id)
 WHERE anniversary_proposal_id IS NOT NULL;

CREATE TABLE notification_reads (
 notification_id uuid PRIMARY KEY,
 couple_id uuid NOT NULL,
 recipient_id uuid NOT NULL,
 read_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 FOREIGN KEY(notification_id, couple_id, recipient_id)
   REFERENCES notifications(id, couple_id, recipient_id) ON DELETE RESTRICT
);
CREATE INDEX notification_reads_recipient_idx ON notification_reads(recipient_id);

CREATE FUNCTION usapp_notification_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $guard$
BEGIN
 RAISE EXCEPTION USING ERRCODE = 'UN101', MESSAGE = 'USAPP_NOTIFICATION_APPEND_ONLY';
END
$guard$;
CREATE TRIGGER notifications_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON notifications
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_notification_reject_mutation();
CREATE TRIGGER notification_reads_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON notification_reads
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_notification_reject_mutation();

CREATE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record;
BEGIN
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
 IF NOT FOUND OR ROW(NEW.event_type,NEW.couple_id,NEW.actor_id,NEW.recipient_id,NEW.target_kind,NEW.target_id)
   IS DISTINCT FROM ROW(facts.event_type,facts.couple_id,facts.actor_id,facts.recipient_id,facts.target_kind,facts.target_id)
   OR NEW.created_at < facts.occurred_at OR NEW.created_at > clock_timestamp() THEN
   RAISE EXCEPTION USING ERRCODE = 'UN102', MESSAGE = 'USAPP_NOTIFICATION_SOURCE';
 END IF;
 RETURN NEW;
END
$guard$;
CREATE TRIGGER notifications_guard BEFORE INSERT ON notifications
 FOR EACH ROW EXECUTE FUNCTION usapp_notification_guard();

CREATE FUNCTION usapp_notification_read_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE written_at timestamptz;
BEGIN
 SELECT created_at INTO written_at FROM notifications WHERE id = NEW.notification_id
   AND couple_id = NEW.couple_id AND recipient_id = NEW.recipient_id;
 IF NOT FOUND OR NEW.read_at < written_at OR NEW.read_at > clock_timestamp() THEN
   RAISE EXCEPTION USING ERRCODE = 'UN103', MESSAGE = 'USAPP_NOTIFICATION_READ';
 END IF;
 RETURN NEW;
END
$guard$;
CREATE TRIGGER notification_reads_guard BEFORE INSERT ON notification_reads
 FOR EACH ROW EXECUTE FUNCTION usapp_notification_read_guard();

REVOKE ALL ON notifications, notification_reads FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON notifications, notification_reads TO usapp_api;
COMMIT;
