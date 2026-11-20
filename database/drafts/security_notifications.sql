-- Draft: owner-approved mandatory personal notices; validate rollback before numbering 0052.
BEGIN;
CREATE TABLE account_login_events(
 id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
 session_id uuid NOT NULL UNIQUE,
 user_id uuid NOT NULL,
 couple_id uuid,
 created_at timestamptz NOT NULL
);
-- No FK to mutable sessions: existing session cleanup/rotation must not delete immutable login evidence or be blocked.
-- The insert guard validates the live source. Retain UUID/time only, never token hashes, IP, agent text or fingerprints.
CREATE FUNCTION usapp_login_event_guard() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE s sessions%ROWTYPE; space uuid;
BEGIN
 SELECT * INTO s FROM sessions WHERE id=NEW.session_id AND is_authenticated AND user_id=NEW.user_id;
 IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UW201',MESSAGE='LOGIN_EVENT_SOURCE';END IF;
 SELECT couple_id INTO space FROM couple_members WHERE user_id=NEW.user_id;
 IF NEW.couple_id IS DISTINCT FROM space THEN RAISE EXCEPTION USING ERRCODE='UW201',MESSAGE='LOGIN_EVENT_SPACE';END IF;
 IF NEW.created_at IS NULL THEN NEW.created_at:=GREATEST(clock_timestamp(),s.created_at);
 ELSIF NEW.created_at<s.created_at OR NEW.created_at>clock_timestamp() THEN RAISE EXCEPTION USING ERRCODE='UW201',MESSAGE='LOGIN_EVENT_TIME';END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER account_login_events_guard BEFORE INSERT ON account_login_events FOR EACH ROW EXECUTE FUNCTION usapp_login_event_guard();
CREATE TRIGGER account_login_events_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON account_login_events
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_notification_reject_mutation();

ALTER TABLE notifications ADD COLUMN admin_adjustment_id uuid REFERENCES admin_wallet_adjustments(id) ON DELETE RESTRICT,
 ADD COLUMN admin_cancellation_id uuid REFERENCES admin_redemption_cancellations(id) ON DELETE RESTRICT,
 ADD COLUMN login_event_id uuid REFERENCES account_login_events(id) ON DELETE RESTRICT;
ALTER TABLE notifications DROP CONSTRAINT notifications_one_source;
ALTER TABLE notifications ADD CONSTRAINT notifications_one_source CHECK(num_nonnulls(
 redemption_id,redemption_event_id,task_change_proposal_id,anniversary_proposal_id,task_id,anniversary_id,message_id,
 message_reply_id,reminder_occurrence_id,memory_id,shared_plan_id,album_batch_id,admin_adjustment_id,admin_cancellation_id,login_event_id)=1);
ALTER TABLE notifications DROP CONSTRAINT notifications_target_kind_valid;
ALTER TABLE notifications ADD CONSTRAINT notifications_target_kind_valid CHECK(target_kind IN('task','reward','anniversary','message','memory','shared_plan','album','account'));
ALTER TABLE notifications DROP CONSTRAINT notifications_reminder_family;
ALTER TABLE notifications ADD CONSTRAINT notifications_reminder_family CHECK(
 (event_type IN('reminder.task','reminder.anniversary'))=(reminder_occurrence_id IS NOT NULL)
 AND (event_type IN('reminder.task','reminder.anniversary','security.wallet_adjusted','security.redemption_cancelled','security.session_created'))=(actor_id IS NULL));
ALTER TABLE notifications ALTER COLUMN couple_id DROP NOT NULL;
ALTER TABLE notifications ADD CONSTRAINT notifications_personal_space CHECK(couple_id IS NOT NULL OR login_event_id IS NOT NULL);
ALTER TABLE notifications ADD CONSTRAINT notifications_id_recipient_unique UNIQUE(id,recipient_id);
CREATE UNIQUE INDEX notifications_adjustment_unique ON notifications(admin_adjustment_id) WHERE admin_adjustment_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_admin_cancel_unique ON notifications(admin_cancellation_id) WHERE admin_cancellation_id IS NOT NULL;
CREATE UNIQUE INDEX notifications_login_unique ON notifications(login_event_id) WHERE login_event_id IS NOT NULL;
ALTER TABLE notification_reads ALTER COLUMN couple_id DROP NOT NULL;
ALTER TABLE notification_reads ADD CONSTRAINT notification_reads_recipient_fk FOREIGN KEY(notification_id,recipient_id) REFERENCES notifications(id,recipient_id) ON DELETE RESTRICT;

CREATE FUNCTION usapp_security_notice_source(kind text,source uuid)
RETURNS TABLE(event_type text,couple_id uuid,actor_id uuid,recipient_id uuid,target_kind text,target_id uuid,occurred_at timestamptz)
LANGUAGE sql VOLATILE AS $$
 SELECT 'security.wallet_adjusted',a.couple_id,NULL::uuid,a.user_id,'account',a.user_id,a.created_at
 FROM admin_wallet_adjustments a WHERE kind='security.wallet_adjusted' AND a.id=source
 UNION ALL SELECT 'security.redemption_cancelled',a.couple_id,NULL::uuid,a.redeemer_id,'account',a.redeemer_id,a.created_at
 FROM admin_redemption_cancellations a WHERE kind='security.redemption_cancelled' AND a.id=source
 UNION ALL SELECT 'security.session_created',e.couple_id,NULL::uuid,e.user_id,'account',e.user_id,e.created_at
 FROM account_login_events e WHERE kind='security.session_created' AND e.id=source
$$;

CREATE OR REPLACE FUNCTION usapp_notification_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE family text; source uuid; facts record; assigned boolean := NEW.created_at IS NULL; source_found boolean;
BEGIN
 IF NEW.event_type IN ('security.wallet_adjusted','security.redemption_cancelled','security.session_created') THEN
   SELECT * INTO facts FROM usapp_security_notice_source(NEW.event_type,COALESCE(NEW.admin_adjustment_id,NEW.admin_cancellation_id,NEW.login_event_id));
 ELSIF NEW.event_type IN ('reminder.task', 'reminder.anniversary') THEN
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



CREATE OR REPLACE FUNCTION usapp_notification_read_guard() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE written_at timestamptz;
BEGIN
 SELECT created_at INTO written_at FROM notifications WHERE id=NEW.notification_id
  AND couple_id IS NOT DISTINCT FROM NEW.couple_id AND recipient_id=NEW.recipient_id;
 IF NOT FOUND OR NEW.read_at<written_at OR NEW.read_at>clock_timestamp() THEN
  RAISE EXCEPTION USING ERRCODE='UN103',MESSAGE='USAPP_NOTIFICATION_READ';END IF;
 RETURN NEW;
END $$;

CREATE FUNCTION usapp_security_notice_emit() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE kind text; facts record;
BEGIN
 kind:=CASE TG_TABLE_NAME WHEN 'admin_wallet_adjustments' THEN 'security.wallet_adjusted'
  WHEN 'admin_redemption_cancellations' THEN 'security.redemption_cancelled' ELSE 'security.session_created' END;
 SELECT * INTO facts FROM usapp_security_notice_source(kind,NEW.id);
 IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UW201',MESSAGE='SECURITY_NOTICE_SOURCE';END IF;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,admin_adjustment_id,admin_cancellation_id,login_event_id)
 VALUES(facts.couple_id,NULL,facts.recipient_id,kind,'account',facts.target_id,
 CASE WHEN TG_TABLE_NAME='admin_wallet_adjustments' THEN NEW.id END,
 CASE WHEN TG_TABLE_NAME='admin_redemption_cancellations' THEN NEW.id END,
 CASE WHEN TG_TABLE_NAME='account_login_events' THEN NEW.id END);
 RETURN NEW;
END $$;
CREATE TRIGGER admin_adjustment_notice AFTER INSERT ON admin_wallet_adjustments FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_emit();
CREATE TRIGGER admin_cancel_notice AFTER INSERT ON admin_redemption_cancellations FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_emit();
CREATE TRIGGER login_event_notice AFTER INSERT ON account_login_events FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_emit();
CREATE FUNCTION usapp_session_login_capture() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF TG_OP='UPDATE' AND OLD.is_authenticated THEN RETURN NEW;END IF;
 INSERT INTO account_login_events(session_id,user_id,couple_id)
 SELECT NEW.id,NEW.user_id,(SELECT couple_id FROM couple_members WHERE user_id=NEW.user_id)
 ON CONFLICT(session_id) DO NOTHING;
 RETURN NEW;
END $$;
CREATE TRIGGER sessions_login_capture AFTER INSERT OR UPDATE OF is_authenticated ON sessions
 FOR EACH ROW WHEN(NEW.is_authenticated) EXECUTE FUNCTION usapp_session_login_capture();
CREATE FUNCTION usapp_security_notice_required() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
 IF TG_TABLE_NAME='sessions' THEN
  SELECT count(*) INTO n FROM account_login_events e JOIN notifications x ON x.login_event_id=e.id
   WHERE e.session_id=NEW.id AND e.user_id=NEW.user_id AND x.recipient_id=NEW.user_id AND x.event_type='security.session_created';
 ELSE
  SELECT count(*) INTO n FROM notifications WHERE
   (TG_TABLE_NAME='admin_wallet_adjustments' AND admin_adjustment_id=NEW.id)
   OR (TG_TABLE_NAME='admin_redemption_cancellations' AND admin_cancellation_id=NEW.id)
   OR (TG_TABLE_NAME='account_login_events' AND login_event_id=NEW.id);
 END IF;
 IF n<>1 THEN RAISE EXCEPTION USING ERRCODE='UW201',MESSAGE='SECURITY_NOTICE_REQUIRED';END IF;
 RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER sessions_login_notice_required AFTER INSERT OR UPDATE OF is_authenticated ON sessions
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN(NEW.is_authenticated) EXECUTE FUNCTION usapp_security_notice_required();
CREATE CONSTRAINT TRIGGER adjustments_notice_required AFTER INSERT ON admin_wallet_adjustments
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_required();
CREATE CONSTRAINT TRIGGER cancellations_notice_required AFTER INSERT ON admin_redemption_cancellations
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_required();
CREATE CONSTRAINT TRIGGER logins_notice_required AFTER INSERT ON account_login_events
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_security_notice_required();
REVOKE ALL ON account_login_events FROM PUBLIC,usapp_api,usapp_push;
GRANT SELECT,INSERT ON account_login_events TO usapp_api;
REVOKE ALL ON FUNCTION usapp_login_event_guard(),usapp_security_notice_source(text,uuid),usapp_security_notice_emit(),usapp_session_login_capture(),usapp_security_notice_required() FROM PUBLIC,usapp_push;
GRANT EXECUTE ON FUNCTION usapp_security_notice_source(text,uuid) TO usapp_api;
COMMIT;
