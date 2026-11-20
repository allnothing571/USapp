-- P2-3a: rollback-only guards. No business service hooks are invoked by these SQL fixtures.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid(); item uuid := gen_random_uuid();
 proposal uuid := gen_random_uuid(); notice uuid; tag text := gen_random_uuid()::text;
 app boolean := current_user = 'usapp_api'; expected text; got text; statement text; cases integer := 0;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
 (a,'p23a-'||tag||'@example.com','p23a-'||tag||'@example.com'),
 (b,'p23b-'||tag||'@example.com','p23b-'||tag||'@example.com'),
 (c,'p23c-'||tag||'@example.com','p23c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p23',a),(other_space,'p23-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES(space,a,1),(space,b,2),(other_space,c,1);
 INSERT INTO anniversaries(id,couple_id,creator_id) VALUES(item,space,a);
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,
 idempotency_key,request_fingerprint) VALUES(item,space,1,a,'create','test',DATE '2020-01-01','anniversary','',tag,repeat('a',64));
 INSERT INTO anniversary_edit_proposals(id,couple_id,anniversary_id,proposer_id,base_version,title,event_date,kind,note,
 idempotency_key,request_fingerprint) VALUES(proposal,space,item,b,1,'changed',DATE '2020-01-02','anniversary','',tag,repeat('b',64));
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,anniversary_proposal_id,target_kind,target_id)
 SELECT couple_id,actor_id,recipient_id,event_type,proposal,target_kind,target_id
 FROM usapp_notification_source('anniversary_proposal',proposal) RETURNING id INTO notice;
 IF notice IS NULL OR NOT EXISTS(SELECT 1 FROM notifications WHERE id=notice AND recipient_id=a AND actor_id=b)
 THEN RAISE EXCEPTION 'recipient derivation failed'; END IF;
 cases := cases+1;
 -- Required dedup collision is visible, never swallowed by the writer.
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,anniversary_proposal_id,target_kind,target_id)
  SELECT couple_id,actor_id,recipient_id,event_type,proposal,target_kind,target_id
  FROM usapp_notification_source('anniversary_proposal',proposal);
  RAISE EXCEPTION 'duplicate accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,anniversary_proposal_id,target_kind,target_id)
  VALUES(space,b,c,'anniversary.edit_proposed',proposal,'anniversary',item);
  RAISE EXCEPTION 'wrong recipient accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,anniversary_proposal_id,target_kind,target_id)
  VALUES(other_space,b,c,'anniversary.edit_proposed',proposal,'anniversary',item);
  RAISE EXCEPTION 'wrong space accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,anniversary_proposal_id,target_kind,target_id)
  VALUES(space,b,a,'anniversary.edit_approved',proposal,'anniversary',item);
  RAISE EXCEPTION 'missing decision accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notification_reads(notification_id,couple_id,recipient_id) VALUES(notice,space,b);
  RAISE EXCEPTION 'wrong receipt recipient accepted';
 EXCEPTION WHEN SQLSTATE 'UN103' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notification_reads(notification_id,couple_id,recipient_id,read_at) VALUES(notice,space,a,clock_timestamp()+interval '1 day');
  RAISE EXCEPTION 'future receipt accepted';
 EXCEPTION WHEN SQLSTATE 'UN103' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notification_reads(notification_id,couple_id,recipient_id,read_at) VALUES(notice,space,a,DATE '1900-01-01');
  RAISE EXCEPTION 'early receipt accepted';
 EXCEPTION WHEN SQLSTATE 'UN103' THEN cases := cases+1; END;
 INSERT INTO notification_reads(notification_id,couple_id,recipient_id) VALUES(notice,space,a);
 INSERT INTO notification_reads(notification_id,couple_id,recipient_id) VALUES(notice,space,a) ON CONFLICT DO NOTHING;
 IF (SELECT count(*) FROM notification_reads WHERE notification_id=notice)<>1 THEN RAISE EXCEPTION 'read duplicate'; END IF;
 cases := cases+1;
 expected := CASE WHEN app THEN '42501' ELSE 'UN101' END;
 FOREACH statement IN ARRAY ARRAY['UPDATE notifications SET actor_id=actor_id', 'DELETE FROM notifications',
  'UPDATE notification_reads SET read_at=read_at', 'DELETE FROM notification_reads',
  'TRUNCATE notifications,notification_reads' || CASE WHEN to_regclass('public.notification_intents') IS NOT NULL
     THEN ',notification_intents' ELSE '' END
  || CASE WHEN to_regclass('public.push_deliveries') IS NOT NULL
     THEN ',push_deliveries,push_attempts,push_attempt_observations,push_attempt_expiries,push_delivery_outcomes,push_subscription_revocations' ELSE '' END] LOOP
  got := NULL;
  BEGIN EXECUTE statement; EXCEPTION WHEN OTHERS THEN got := SQLSTATE; END;
  IF got IS DISTINCT FROM expected THEN RAISE EXCEPTION 'mutation barrier: expected %, got %',expected,got; END IF;
  cases := cases+1;
 END LOOP;
 RAISE NOTICE 'ALL NOTIFICATION ASSERTIONS PASSED: role=% cases=%',current_user,cases;
END
$test$;
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
