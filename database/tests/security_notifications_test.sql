\set ON_ERROR_STOP on
BEGIN;
-- Appended inside rollback-only draft, under both application and maintenance roles.
DO $test$
DECLARE a uuid:=gen_random_uuid();b uuid:=gen_random_uuid();c uuid:=gen_random_uuid();adm uuid:=gen_random_uuid();
 session uuid:=gen_random_uuid();pre uuid:=gen_random_uuid();tag text:=md5(random()::text);r jsonb;source uuid;n bigint;notice uuid;
 product uuid:=gen_random_uuid();redemption uuid:=gen_random_uuid();err text;mode text;
BEGIN
 IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'disposable only';END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES(a,'sec_a_'||tag||'@example.com','sec_a_'||tag||'@example.com'),(b,'sec_b_'||tag||'@example.com','sec_b_'||tag||'@example.com');
 INSERT INTO sessions(id,token_hash,csrf_token_hash,is_authenticated,expires_at) VALUES(pre,md5(tag||'pre'),md5(tag||'csrf'),false,now()+interval '1 hour');
 IF EXISTS(SELECT 1 FROM account_login_events WHERE session_id=pre) THEN RAISE EXCEPTION 'preauth notified';END IF;
 INSERT INTO sessions(id,token_hash,csrf_token_hash,is_authenticated,user_id,expires_at) VALUES(session,md5(tag||'real'),md5(tag||'csrf2'),true,a,now()+interval '1 hour');
 SELECT id INTO source FROM account_login_events WHERE session_id=session AND user_id=a;
 IF source IS NULL THEN RAISE EXCEPTION 'no-space login lost';END IF;
 SELECT id INTO notice FROM notifications WHERE login_event_id=source AND recipient_id=a AND couple_id IS NULL AND actor_id IS NULL AND target_kind='account' AND target_id=a;
 IF notice IS NULL THEN RAISE EXCEPTION 'private login facts';END IF;
 INSERT INTO notification_reads(notification_id,couple_id,recipient_id) VALUES(notice,NULL,a);
 UPDATE sessions SET csrf_token_hash=md5(tag||'rotated'),last_seen_at=clock_timestamp() WHERE id=session;
 IF (SELECT count(*) FROM account_login_events WHERE session_id=session)<>1 THEN RAISE EXCEPTION 'touch renotified';END IF;
 DELETE FROM sessions WHERE id=session;
 IF NOT EXISTS(SELECT 1 FROM account_login_events WHERE id=source) THEN RAISE EXCEPTION 'session cleanup erased evidence';END IF;
 FOREACH mode IN ARRAY ARRAY['recipient','target','actor','space','source','time'] LOOP
  err:=NULL;
  BEGIN
   INSERT INTO notifications(event_type,recipient_id,actor_id,couple_id,target_kind,target_id,login_event_id,created_at)
   VALUES('security.session_created',CASE WHEN mode='recipient' THEN b ELSE a END,CASE WHEN mode='actor' THEN b END,
    CASE WHEN mode='space' THEN gen_random_uuid() END,'account',CASE WHEN mode='target' THEN b ELSE a END,
    CASE WHEN mode='source' THEN gen_random_uuid() ELSE source END,CASE WHEN mode='time' THEN now()+interval '1 hour' END);
  EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE;END;
  IF err NOT IN('UN102','23505') OR err IS NULL THEN RAISE EXCEPTION 'forged notice % accepted/wrong error: %',mode,err;END IF;
 END LOOP;
 err:=NULL;BEGIN INSERT INTO notification_reads(notification_id,couple_id,recipient_id) VALUES(notice,NULL,b);
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE;END;
 IF err IS DISTINCT FROM 'UN103' THEN RAISE EXCEPTION 'other account read receipt';END IF;
 UPDATE sessions SET is_authenticated=true,user_id=a WHERE id=pre;
 IF (SELECT count(*) FROM account_login_events WHERE session_id=pre)<>1 THEN RAISE EXCEPTION 'formal promotion missing';END IF;
 UPDATE sessions SET is_authenticated=true WHERE id=pre;
 IF (SELECT count(*) FROM account_login_events WHERE session_id=pre)<>1 THEN RAISE EXCEPTION 'same formal identity renotified';END IF;
 INSERT INTO couples(id,name,created_by_user_id) VALUES(c,'Security fixture',a);
 INSERT INTO couple_members(user_id,couple_id,slot) VALUES(a,c,1),(b,c,2);
 INSERT INTO admin_users(id,username,password_hash) VALUES(adm,'sec_'||tag,'synthetic-only-hash');
 r:=usapp_admin_wallet_adjust(adm,b,'flower',4,0,'Private operator reason',tag||'adjust');
 IF r ? 'error' THEN RAISE EXCEPTION 'adjustment refused';END IF;source:=(r#>>'{adjustment,id}')::uuid;
 SELECT count(*) INTO n FROM notifications WHERE admin_adjustment_id=source AND recipient_id=b AND actor_id IS NULL AND couple_id=c;
 IF n<>1 THEN RAISE EXCEPTION 'correction notice';END IF;
 PERFORM usapp_admin_wallet_adjust(adm,b,'flower',4,0,'Private operator reason',tag||'adjust');
 IF (SELECT count(*) FROM notifications WHERE admin_adjustment_id=source)<>1 THEN RAISE EXCEPTION 'replay renotified';END IF;
 INSERT INTO reward_products(id,couple_id,creator_id,owner_id) VALUES(product,c,a,b);
 INSERT INTO reward_product_revisions(product_id,couple_id,version,actor_id,action,title,price_flowers,redemption_mode,status,idempotency_key,request_fingerprint)
  VALUES(product,c,1,a,'create','Security fixture',3,'request','active',tag||'product',repeat('a',64));
 INSERT INTO reward_redemptions(id,couple_id,product_id,product_version,creator_id,redeemer_id,title,price_flowers,redemption_mode,idempotency_key,request_fingerprint)
 VALUES(redemption,c,product,1,a,b,'Security fixture',3,'request',tag||'request',repeat('a',64));
 r:=usapp_admin_redemption_cancel(adm,c,redemption,'pending_confirmation','Private cancellation reason',tag||'cancel');
 IF r ? 'error' THEN RAISE EXCEPTION 'cancel refused';END IF;source:=(r#>>'{cancellation,id}')::uuid;
 IF (SELECT count(*) FROM notifications WHERE admin_cancellation_id=source AND recipient_id=b AND actor_id IS NULL)<>1 THEN RAISE EXCEPTION 'unpaid cancel notice';END IF;
 SELECT count(*) INTO n FROM notifications WHERE recipient_id=b;
 BEGIN
  PERFORM usapp_admin_wallet_adjust(adm,b,'sunshine',3,0,'Rolled back private reason',tag||'rollback');
  RAISE EXCEPTION USING ERRCODE='UW299',MESSAGE='test rollback';
 EXCEPTION WHEN SQLSTATE 'UW299' THEN NULL;END;
 IF (SELECT count(*) FROM notifications WHERE recipient_id=b)<>n THEN RAISE EXCEPTION 'rollback left notice';END IF;
 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'SECURITY_NOTICES_DRAFT_PASSED role=%',current_user;
END $test$;

ROLLBACK;
