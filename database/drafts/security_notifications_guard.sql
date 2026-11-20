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

