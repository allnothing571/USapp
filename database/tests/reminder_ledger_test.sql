-- P2-6 R1: rollback-only guards on reminder rules, occurrences and the notification family.
-- Run as usapp_api and as a superuser. No generator exists: occurrences here are inserted directly.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := gen_random_uuid()::text;
 open_task uuid := gen_random_uuid(); due_task uuid := gen_random_uuid(); daily uuid := gen_random_uuid();
 weekly uuid := gen_random_uuid(); custom uuid := gen_random_uuid(); item uuid := gen_random_uuid();
 rule uuid; occ uuid; notice uuid;
 daily2 uuid := gen_random_uuid(); weekly2 uuid := gen_random_uuid(); custom2 uuid := gen_random_uuid();
 yearly uuid := gen_random_uuid(); countdown uuid := gen_random_uuid(); nowtime time; yearly_target date;
 r5_task uuid := gen_random_uuid(); r5_item uuid := gen_random_uuid(); r5_stale timestamptz; r5_notice uuid; r5_time time;
 scenario text; pred uuid; succ uuid; prop uuid; prop_other uuid; src uuid; older uuid; decided timestamptz; new_deadline timestamptz;
 today date := (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
 monday date;
 due_deadline timestamptz; fire timestamptz; minutes integer; got text;
 app boolean := current_user = 'usapp_api'; cases integer := 0;

BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 monday := today - (EXTRACT(ISODOW FROM today)::integer - 1);
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'p26a-'||tag||'@example.com','p26a-'||tag||'@example.com'),
  (b,'p26b-'||tag||'@example.com','p26b-'||tag||'@example.com'),
  (c,'p26c-'||tag||'@example.com','p26c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p26',a),(other_space,'p26-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,created_at)
  VALUES (open_task,space,a,b,'open',1,1,tag||'-open',repeat('a',64),clock_timestamp()-interval '3 days');
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,completion_mode,target_count,schedule_type,created_at)
  VALUES (daily,space,a,b,'daily',1,1,tag||'-daily',repeat('a',64),'count',2,'daily',clock_timestamp()-interval '3 days'),
         (weekly,space,a,b,'weekly',1,1,tag||'-weekly',repeat('a',64),'count',2,'weekly',clock_timestamp()-interval '3 days');
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,completion_mode,target_count,schedule_type,start_local_date,deadline_local_date,deadline_at,created_at)
  VALUES (custom,space,a,b,'custom',1,1,tag||'-custom',repeat('a',64),'count',2,'custom',today+2,today+4,
          ((today+5)::timestamp AT TIME ZONE 'Asia/Shanghai'),clock_timestamp()-interval '3 days');
 -- The deadline is at least 26 hours away, so a 2-hour window around "an hour ago" lies before it.
 due_deadline := ((today+2)::timestamp AT TIME ZONE 'Asia/Shanghai');
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,deadline_local_date,deadline_at,created_at)
  VALUES (due_task,space,a,b,'due',1,1,tag||'-due',repeat('a',64),today+1,due_deadline,clock_timestamp()-interval '3 days');
 INSERT INTO anniversaries(id,couple_id,creator_id,created_at) VALUES (item,space,a,clock_timestamp()-interval '3 days');
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint,created_at)
  VALUES (item,space,1,a,'create','item',today+3,'countdown','',tag||'-rev',repeat('a',64),clock_timestamp()-interval '3 days');

 -- ---- Shapes, closed values and the empty-array hole (CHECK violations in both roles).
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,item,true,'anniversary_offsets','{}','09:00',tag||'-empty',repeat('a',64));
  RAISE EXCEPTION 'empty offsets accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,item,true,'anniversary_offsets','{3,1}','09:00',tag||'-order',repeat('a',64));
  RAISE EXCEPTION 'unordered offsets accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,item,true,'anniversary_offsets','{2}','09:00',tag||'-closed',repeat('a',64));
  RAISE EXCEPTION 'unknown offset accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,item,false,'anniversary_offsets','09:00',tag||'-offshape',repeat('a',64));
  RAISE EXCEPTION 'disabled rule with a schedule accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,anniversary_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,1,daily,item,false,tag||'-both',repeat('a',64));
  RAISE EXCEPTION 'two targets accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,daily,true,'local_time',0,'09:00:30',tag||'-seconds',repeat('a',64));
  RAISE EXCEPTION 'seconds accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;

 -- ---- Guards: owner, space, origin, sequence and time.
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,a,1,daily,false,tag||'-owner',repeat('a',64));
  RAISE EXCEPTION 'creator rule accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_REMINDER_OWNER' THEN RAISE EXCEPTION 'owner: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (other_space,c,1,daily,false,tag||'-space',repeat('a',64));
  RAISE EXCEPTION 'other space accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,idempotency_key,request_fingerprint)
  VALUES (other_space,c,1,item,false,tag||'-space2',repeat('a',64));
  RAISE EXCEPTION 'other space anniversary accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,origin)
  VALUES (space,b,1,daily,false,'successor');
  RAISE EXCEPTION 'successor origin accepted without provenance';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,2,daily,false,tag||'-gap',repeat('a',64));
  RAISE EXCEPTION 'sequence gap accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,daily,false,tag||'-future',repeat('a',64),clock_timestamp()+interval '1 hour');
  RAISE EXCEPTION 'future time accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;

 -- ---- Schedule bounds, by the guard itself.
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,daily,true,'before_deadline',1441,tag||'-d1441',repeat('a',64));
  RAISE EXCEPTION 'daily 1441 accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_REMINDER_INVALID' THEN RAISE EXCEPTION 'daily: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,weekly,true,'local_time',7,'09:00',tag||'-w7',repeat('a',64));
  RAISE EXCEPTION 'weekly 7 days accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,custom,true,'before_deadline',4321,tag||'-c4321',repeat('a',64));
  RAISE EXCEPTION 'custom before the range accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,open_task,true,'before_deadline',30,tag||'-open-b',repeat('a',64));
  RAISE EXCEPTION 'before_deadline without a deadline accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,1,open_task,true,'absolute',today-1,'09:00',tag||'-open-p',repeat('a',64));
  RAISE EXCEPTION 'passed instant accepted';
 EXCEPTION WHEN SQLSTATE 'UV002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_REMINDER_PASSED' THEN RAISE EXCEPTION 'passed: %', got; END IF; cases := cases+1; END;
 -- The bounds are inclusive.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,daily,true,'before_deadline',1440,tag||'-d1440',repeat('a',64));
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,weekly,true,'before_deadline',10080,tag||'-w10080',repeat('a',64));
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,1,custom,true,'before_deadline',4320,tag||'-c4320',repeat('a',64));
 cases := cases+3;
 -- Disabling needs no valid instant.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,1,open_task,false,tag||'-open-off',repeat('a',64));
 cases := cases+1;

 -- ---- Append-only: the trigger for a superuser, no privilege for the application.
 BEGIN
  UPDATE reminder_rules SET enabled = enabled WHERE owner_id = b;
  RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UV001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  DELETE FROM reminder_occurrences WHERE owner_id = b;
  RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UV001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;

 -- ---- Effective time and the anniversary calendar.
 IF usapp_reminder_task_effective_at(b, open_task) IS NOT NULL THEN RAISE EXCEPTION 'disabled rule effective'; END IF;
 IF (DATE '2024-02-29' + make_interval(years => 1))::date <> DATE '2025-02-28'
    OR (DATE '2024-02-29' + make_interval(years => 4))::date <> DATE '2028-02-29' THEN
  RAISE EXCEPTION 'leap day'; END IF;
 IF usapp_reminder_task_next_fire(b, open_task) IS NOT NULL THEN RAISE EXCEPTION 'next fire when disabled'; END IF;
 cases := cases+3;

 -- ---- A rule whose instant was an hour ago, then occurrence guards.
 minutes := (extract(epoch FROM due_deadline - clock_timestamp()) / 60)::integer + 60;
 fire := due_deadline - make_interval(mins => minutes);
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,due_task,true,'before_deadline',minutes,tag||'-due-r',repeat('a',64),clock_timestamp()-interval '1 day')
  RETURNING id INTO rule;
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',due_task,fire+interval '1 minute',fire+interval '2 hours');
  RAISE EXCEPTION 'wrong instant accepted';
 EXCEPTION WHEN SQLSTATE 'UV003' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',due_task,fire,fire+interval '3 hours');
  RAISE EXCEPTION 'wrong window accepted';
 EXCEPTION WHEN SQLSTATE 'UV003' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,period_date,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',due_task,today,fire,fire+interval '2 hours');
  RAISE EXCEPTION 'period on a one-off accepted';
 EXCEPTION WHEN SQLSTATE 'UV003' THEN cases := cases+1; END;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',due_task,fire,fire+interval '2 hours') RETURNING id INTO occ;
 -- Without a notification the deferred link refuses it.
 BEGIN
  SET CONSTRAINTS reminder_occurrences_notification_link IMMEDIATE;
  RAISE EXCEPTION 'unlinked occurrence accepted';
 EXCEPTION WHEN SQLSTATE 'UV004' THEN cases := cases+1; END;
 SET CONSTRAINTS reminder_occurrences_notification_link DEFERRED;
 -- Identity: one per target, whichever rule revision.
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',due_task,fire,fire+interval '2 hours');
  RAISE EXCEPTION 'duplicate occurrence accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;

 -- ---- The notification family.
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,a,b,'reminder.task','task',due_task,occ);
  RAISE EXCEPTION 'reminder with an actor accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.anniversary','task',due_task,occ);
  RAISE EXCEPTION 'wrong reminder event accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,task_id)
  VALUES (space,NULL,b,'task.published','task',due_task,due_task);
  RAISE EXCEPTION 'null actor on another family accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id)
  VALUES (space,NULL,b,'reminder.task','task',due_task);
  RAISE EXCEPTION 'reminder without an occurrence accepted';
 EXCEPTION WHEN SQLSTATE 'UN102' THEN cases := cases+1; END;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',due_task,occ) RETURNING id INTO notice;
 SET CONSTRAINTS reminder_occurrences_notification_link IMMEDIATE;
 SET CONSTRAINTS reminder_occurrences_notification_link DEFERRED;
 cases := cases+1;
 BEGIN
  INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',due_task,occ);
  RAISE EXCEPTION 'second notice accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 -- A disabled or changed rule fences later generation.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,2,due_task,false,tag||'-due-off',repeat('a',64));
 cases := cases+1;

 -- ---- Anniversary occurrence: only a real target, per offset.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,item,true,'anniversary_offsets','{3}','00:00',tag||'-item-r',repeat('a',64),clock_timestamp()-interval '1 day')
  RETURNING id INTO rule;
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',item,1,1,today+4,3,((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai'),((today+2)::timestamp AT TIME ZONE 'Asia/Shanghai'));
  RAISE EXCEPTION 'not a target accepted';
 EXCEPTION WHEN SQLSTATE 'UV003' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',item,1,1,today+3,1,((today+2)::timestamp AT TIME ZONE 'Asia/Shanghai'),((today+3)::timestamp AT TIME ZONE 'Asia/Shanghai'));
  RAISE EXCEPTION 'offset outside the rule accepted';
 EXCEPTION WHEN SQLSTATE 'UV003' THEN cases := cases+1; END;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',item,1,1,today+3,3,(today::timestamp AT TIME ZONE 'Asia/Shanghai'),((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai'))
  RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.anniversary','anniversary',item,occ);
 cases := cases+1;

 -- ---- The read functions and effective time, as the API uses them.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,open_task,true,'absolute',today+3,'09:00',tag||'-open-on',repeat('a',64));
 IF usapp_reminder_task_next_fire(b, open_task) IS DISTINCT FROM (((today+3)+TIME '09:00') AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'one-off next fire'; END IF;
 IF usapp_reminder_task_effective_at(b, open_task) IS NULL THEN RAISE EXCEPTION 'one-off effective'; END IF;
 -- 1440 before midnight is 00:00 of the same period, already passed today, so the next is tomorrow's.
 IF usapp_reminder_task_next_fire(b, daily) IS DISTINCT FROM ((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'daily next fire'; END IF;
 IF usapp_reminder_task_next_fire(b, weekly) IS DISTINCT FROM ((monday+7)::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'weekly next fire'; END IF;
 IF usapp_reminder_task_next_fire(b, custom) IS DISTINCT FROM ((today+2)::timestamp AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'custom next fire'; END IF;
 IF usapp_reminder_task_next_fire(b, due_task) IS NOT NULL THEN RAISE EXCEPTION 'next fire on a disabled task rule'; END IF;
 -- Offset 3 at 00:00 for today+3 is today 00:00: passed, and a countdown has no later target.
 IF usapp_reminder_anniversary_next_fire(b, item) IS NOT NULL THEN RAISE EXCEPTION 'passed countdown next fire'; END IF;
 IF (usapp_anniversary_schedule_effective(item)).schedule_version <> 1 THEN RAISE EXCEPTION 'schedule version'; END IF;
 IF usapp_reminder_offset_effective_at(b, item, 3) IS NULL OR usapp_reminder_offset_effective_at(b, item, 1) IS NOT NULL THEN
  RAISE EXCEPTION 'offset effective'; END IF;
 -- The rule run: an identical re-save keeps it, a change does not, and a disable breaks it.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,item,true,'anniversary_offsets','{3,7}','00:00',tag||'-item-2',repeat('a',64));
 IF usapp_reminder_offset_effective_at(b, item, 3) IS DISTINCT FROM
    (SELECT created_at FROM reminder_rules WHERE anniversary_id = item AND owner_id = b AND seq = 1) THEN
  RAISE EXCEPTION 'added offset moved another'; END IF;
 IF usapp_reminder_offset_effective_at(b, item, 7) IS DISTINCT FROM
    (SELECT created_at FROM reminder_rules WHERE anniversary_id = item AND owner_id = b AND seq = 2) THEN
  RAISE EXCEPTION 'added offset time'; END IF;
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,3,item,false,tag||'-item-3',repeat('a',64));
 IF usapp_reminder_offset_effective_at(b, item, 3) IS NOT NULL THEN RAISE EXCEPTION 'disabled offset effective'; END IF;
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,4,item,true,'anniversary_offsets','{3}','00:00',tag||'-item-4',repeat('a',64));
 IF usapp_reminder_offset_effective_at(b, item, 3) IS DISTINCT FROM
    (SELECT created_at FROM reminder_rules WHERE anniversary_id = item AND owner_id = b AND seq = 4) THEN
  RAISE EXCEPTION 'disable did not break the run'; END IF;
 -- Yearly next fire, with a member's own rule on a Feb 29 base: always one of the two legal dates, in the future.
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint)
  VALUES (item,space,2,a,'edit','item',DATE '2024-02-29','anniversary','',tag||'-rev2',repeat('a',64));
 IF (usapp_anniversary_schedule_effective(item)).schedule_version <> 2 THEN RAISE EXCEPTION 'date change run'; END IF;
 fire := usapp_reminder_anniversary_next_fire(b, item);
 IF fire IS NULL OR fire <= clock_timestamp() OR to_char(((fire AT TIME ZONE 'Asia/Shanghai')::date + 3), 'MM-DD') NOT IN ('02-28', '02-29') THEN
  RAISE EXCEPTION 'leap next fire %', fire; END IF;
 cases := cases+12;

 -- ---- next_fire never names an identity that already fired (0026).
 -- The minute just begun: an instant that has passed, on today's date, whatever the hour.
 nowtime := date_trunc('minute', clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::time;
 -- One-off: due_task fired above and was disabled; a new time must not offer it again.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,3,due_task,true,'before_deadline',30,tag||'-due-on',repeat('a',64));
 IF usapp_reminder_task_next_fire(b, due_task) IS NOT NULL THEN RAISE EXCEPTION 'fired one-off offered again'; END IF;
 -- Custom: the same identity rule.
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,completion_mode,target_count,schedule_type,start_local_date,deadline_local_date,deadline_at,created_at)
  VALUES (custom2,space,a,b,'custom2',1,1,tag||'-custom2',repeat('a',64),'count',2,'custom',today,today+2,
          ((today+3)::timestamp AT TIME ZONE 'Asia/Shanghai'),clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,custom2,true,'local_time',2,nowtime,tag||'-custom2-r',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 fire := ((today + nowtime) AT TIME ZONE 'Asia/Shanghai');
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',custom2,fire,fire+interval '2 hours') RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',custom2,occ);
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,custom2,true,'local_time',0,'20:00',tag||'-custom2-2',repeat('a',64));
 IF usapp_reminder_task_next_fire(b, custom2) IS NOT NULL THEN RAISE EXCEPTION 'fired custom offered again'; END IF;
 -- Daily: this period fired, so a later time today moves to the next period.
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,completion_mode,target_count,schedule_type,created_at)
  VALUES (daily2,space,a,b,'daily2',1,1,tag||'-daily2',repeat('a',64),'count',2,'daily',clock_timestamp()-interval '3 days'),
         (weekly2,space,a,b,'weekly2',1,1,tag||'-weekly2',repeat('a',64),'count',2,'weekly',clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,daily2,true,'local_time',0,nowtime,tag||'-daily2-r',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,period_date,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',daily2,today,fire,LEAST(fire+interval '2 hours',((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai'))) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',daily2,occ);
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,daily2,true,'local_time',0,'23:59',tag||'-daily2-2',repeat('a',64));
 IF usapp_reminder_task_next_fire(b, daily2) IS DISTINCT FROM (((today+1)+TIME '23:59') AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'fired daily period offered again: %', usapp_reminder_task_next_fire(b, daily2); END IF;
 -- Weekly: this week's period fired today, so its Sunday is not offered; next week's is.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,weekly2,true,'local_time',7-EXTRACT(ISODOW FROM today)::integer,nowtime,tag||'-weekly2-r',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,period_date,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',weekly2,monday,fire,LEAST(fire+interval '2 hours',((monday+7)::timestamp AT TIME ZONE 'Asia/Shanghai'))) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',weekly2,occ);
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,days_before,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,weekly2,true,'local_time',0,'23:59',tag||'-weekly2-2',repeat('a',64));
 IF usapp_reminder_task_next_fire(b, weekly2) IS DISTINCT FROM (((monday+13)+TIME '23:59') AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'fired weekly period offered again: %', usapp_reminder_task_next_fire(b, weekly2); END IF;
 -- Anniversary: offset 1 of tomorrow's target fired today.
 yearly_target := today+1;
 INSERT INTO anniversaries(id,couple_id,creator_id,created_at) VALUES (yearly,space,a,clock_timestamp()-interval '3 days');
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint,created_at)
  VALUES (yearly,space,1,a,'create','yearly',(yearly_target - interval '4 years')::date,'anniversary','',tag||'-y1',repeat('a',64),clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,yearly,true,'anniversary_offsets','{1}',nowtime,tag||'-y-r',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',yearly,1,1,yearly_target,1,fire,((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai')) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.anniversary','anniversary',yearly,occ);
 -- The unfired offset 0 of the same target is preserved.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,yearly,true,'anniversary_offsets','{0,1}','23:59',tag||'-y-2',repeat('a',64));
 IF usapp_reminder_anniversary_next_fire(b, yearly) IS DISTINCT FROM ((yearly_target+TIME '23:59') AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'unfired offset lost: %', usapp_reminder_anniversary_next_fire(b, yearly); END IF;
 -- With only the fired offset, its later yearly target is next, never today.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,3,yearly,true,'anniversary_offsets','{1}','23:59',tag||'-y-3',repeat('a',64));
 IF usapp_reminder_anniversary_next_fire(b, yearly) IS DISTINCT FROM
    ((((yearly_target - interval '4 years' + interval '5 years')::date - 1)+TIME '23:59') AT TIME ZONE 'Asia/Shanghai') THEN
  RAISE EXCEPTION 'fired target offered again: %', usapp_reminder_anniversary_next_fire(b, yearly); END IF;
 -- Countdown: nothing later exists, so a fired one has no next.
 INSERT INTO anniversaries(id,couple_id,creator_id,created_at) VALUES (countdown,space,a,clock_timestamp()-interval '3 days');
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint,created_at)
  VALUES (countdown,space,1,a,'create','countdown',yearly_target,'countdown','',tag||'-c1',repeat('a',64),clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,countdown,true,'anniversary_offsets','{1}',nowtime,tag||'-c-r',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',countdown,1,1,yearly_target,1,fire,((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai')) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.anniversary','anniversary',countdown,occ);
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,countdown,true,'anniversary_offsets','{1}','23:59',tag||'-c-2',repeat('a',64));
 IF usapp_reminder_anniversary_next_fire(b, countdown) IS NOT NULL THEN RAISE EXCEPTION 'fired countdown offered again'; END IF;
 -- Everything above has its notification.
 SET CONSTRAINTS reminder_occurrences_notification_link IMMEDIATE;
 SET CONSTRAINTS reminder_occurrences_notification_link DEFERRED;
 cases := cases+7;

 -- ---- R4: provenance of an inherited rule (0027). Each scenario is a complete approved edit, backdated a day.
 FOREACH scenario IN ARRAY ARRAY['invalid', 'disabled', 'usable'] LOOP
  pred := gen_random_uuid(); succ := gen_random_uuid();
  decided := clock_timestamp() - interval '1 day';
  new_deadline := ((today+4)::timestamp AT TIME ZONE 'Asia/Shanghai');
  IF scenario = 'usable' THEN
   INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,deadline_local_date,deadline_at,created_at)
    VALUES (pred,space,a,b,'pred',1,1,tag||'-p-'||scenario,repeat('a',64),today+2,((today+3)::timestamp AT TIME ZONE 'Asia/Shanghai'),clock_timestamp()-interval '5 days');
   INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint,created_at)
    VALUES (space,b,1,pred,true,'before_deadline',30,tag||'-r-'||scenario,repeat('a',64),clock_timestamp()-interval '4 days') RETURNING id INTO src;
  ELSE
   -- Absolute needs a task with no deadline, and the instant below is today's: the guard is all that refuses it.
   INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,created_at)
    VALUES (pred,space,a,b,'pred',1,1,tag||'-p-'||scenario,repeat('a',64),clock_timestamp()-interval '5 days');
   INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,idempotency_key,request_fingerprint,created_at)
    VALUES (space,b,1,pred,true,'absolute',today,nowtime,tag||'-r-'||scenario,repeat('a',64),clock_timestamp()-interval '4 days') RETURNING id INTO src;
   IF scenario = 'disabled' THEN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint,created_at)
     VALUES (space,b,2,pred,false,tag||'-r2-'||scenario,repeat('a',64),clock_timestamp()-interval '3 days') RETURNING id INTO src;
   ELSE
    older := src;
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,idempotency_key,request_fingerprint,created_at)
     VALUES (space,b,2,pred,true,'absolute',today,nowtime,tag||'-r2-'||scenario,repeat('a',64),clock_timestamp()-interval '3 days') RETURNING id INTO src;
   END IF;
  END IF;
  -- A rejected edit on the same root, usable only as forged provenance.
  INSERT INTO task_change_proposals(couple_id,task_id,proposer_id,kind,title,description,reward_flowers,reward_sunshine,completion_mode,target_count,deadline_local_date,idempotency_key,request_fingerprint,created_at)
   VALUES (space,pred,a,'edit','edited','',1,1,'single',1,today+3,tag||'-px-'||scenario,repeat('a',64),clock_timestamp()-interval '2 days 2 hours') RETURNING id INTO prop_other;
  INSERT INTO task_change_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint,decided_at)
   VALUES (prop_other,space,b,'rejected',tag||'-dx-'||scenario,repeat('a',64),clock_timestamp()-interval '2 days');
  INSERT INTO task_change_proposals(couple_id,task_id,proposer_id,kind,title,description,reward_flowers,reward_sunshine,completion_mode,target_count,deadline_local_date,idempotency_key,request_fingerprint,created_at)
   VALUES (space,pred,a,'edit','edited','',1,1,'single',1,today+3,tag||'-pa-'||scenario,repeat('a',64),clock_timestamp()-interval '1 day 2 hours') RETURNING id INTO prop;
  INSERT INTO task_change_decisions(proposal_id,couple_id,decided_by,outcome,successor_task_id,idempotency_key,request_fingerprint,decided_at)
   VALUES (prop,space,b,'approved',succ,tag||'-da-'||scenario,repeat('a',64),decided);
  INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,description,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,deadline_local_date,deadline_at,created_at)
   VALUES (succ,space,a,b,'edited','',1,1,'chg:'||prop::text,repeat('a',64),today+3,new_deadline,decided);
  INSERT INTO task_closures(task_id,couple_id,reason,proposal_id,closed_at) VALUES (pred,space,'superseded',prop,decided);

  IF scenario = 'invalid' THEN
   -- Forged provenance, wrong owner, wrong terms, stale source, wrong revision, invalid origin.
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,1,succ,true,'absolute',today,nowtime,'successor',src,prop_other,decided);
    RAISE EXCEPTION 'a rejected decision accepted as provenance';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,a,1,succ,true,'absolute',today,nowtime,'successor',src,prop,decided);
    RAISE EXCEPTION 'another owner accepted';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,1,succ,true,'absolute',today+1,nowtime,'successor',src,prop,decided);
    RAISE EXCEPTION 'changed terms accepted';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,1,succ,false,'successor',src,prop,decided);
    RAISE EXCEPTION 'changed enabled state accepted';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,1,succ,true,'absolute',today,nowtime,'successor',older,prop,decided);
    RAISE EXCEPTION 'an older revision accepted as the source';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,1,daily,true,'absolute',today,nowtime,'successor',src,prop,decided);
    RAISE EXCEPTION 'another target accepted';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,at_local_date,at_time,origin,source_rule_id,source_proposal_id,created_at)
    VALUES (space,b,2,succ,true,'absolute',today,nowtime,'successor',src,prop,decided);
    RAISE EXCEPTION 'a later revision accepted as a copy';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,origin,source_rule_id,source_proposal_id)
    VALUES (space,b,1,item,false,'successor',src,prop);
    RAISE EXCEPTION 'an anniversary accepted as a successor';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,origin,idempotency_key,request_fingerprint)
    VALUES (space,b,1,succ,false,'imported',tag||'-origin',repeat('a',64));
    RAISE EXCEPTION 'an unknown origin accepted';
   EXCEPTION WHEN SQLSTATE 'UV002' THEN cases := cases+1; END;
   -- User origin with provenance: past the guard, refused by the CHECK.
   BEGIN
    INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint,source_rule_id,source_proposal_id)
    VALUES (space,b,1,succ,false,tag||'-forged',repeat('a',64),src,prop);
    RAISE EXCEPTION 'provenance accepted on a user rule';
   EXCEPTION WHEN check_violation THEN cases := cases+1; END;
   -- A supersession cannot commit without the copy.
   BEGIN
    SET CONSTRAINTS task_closures_reminder_copied IMMEDIATE;
    RAISE EXCEPTION 'a supersession without the copy accepted';
   EXCEPTION WHEN SQLSTATE 'UV005' THEN cases := cases+1; END;
   SET CONSTRAINTS task_closures_reminder_copied DEFERRED;
  END IF;

  -- The genuine copy of the latest revision, whatever its state.
  INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,days_before,at_time,at_local_date,days_before_set,origin,source_rule_id,source_proposal_id,created_at)
  SELECT r.couple_id,r.owner_id,1,succ,r.enabled,r.mode,r.before_minutes,r.days_before,r.at_time,r.at_local_date,r.days_before_set,'successor',r.id,prop,decided
    FROM reminder_rules r WHERE r.id = src RETURNING id INTO rule;
  SET CONSTRAINTS task_closures_reminder_copied IMMEDIATE;
  SET CONSTRAINTS task_closures_reminder_copied DEFERRED;
  BEGIN
   INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,days_before,at_time,at_local_date,days_before_set,origin,source_rule_id,source_proposal_id)
   SELECT r.couple_id,r.owner_id,2,succ,r.enabled,r.mode,r.before_minutes,r.days_before,r.at_time,r.at_local_date,r.days_before_set,'successor',r.id,prop
     FROM reminder_rules r WHERE r.id = src;
   RAISE EXCEPTION 'a second copy accepted';
  -- The guard sees a valid next seq; the CHECK allows a copy only as the first revision.
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;

  IF scenario = 'invalid' THEN
   IF NOT usapp_reminder_rule_needs_reset(rule) THEN RAISE EXCEPTION 'absolute on a deadline not reported'; END IF;
   IF usapp_reminder_task_next_fire(b, succ) IS NOT NULL THEN RAISE EXCEPTION 'next fire fabricated'; END IF;
   -- The instant is today's and inside its window, so only the needs-reset check refuses it.
   fire := ((today + nowtime) AT TIME ZONE 'Asia/Shanghai');
   BEGIN
    INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
    VALUES (space,b,rule,'reminder.task',succ,fire,LEAST(new_deadline,fire+interval '2 hours'));
    RAISE EXCEPTION 'an occurrence accepted on a rule needing reset';
   EXCEPTION WHEN SQLSTATE 'UV003' THEN
    GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
    IF got <> 'USAPP_REMINDER_OCCURRENCE_NEEDS_RESET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF;
    cases := cases+1;
   END;
   -- The user's own later valid save clears it.
   INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
   VALUES (space,b,2,succ,true,'before_deadline',30,tag||'-reset',repeat('a',64));
   IF usapp_reminder_task_next_fire(b, succ) IS DISTINCT FROM (new_deadline - interval '30 minutes') THEN RAISE EXCEPTION 'reset not cleared'; END IF;
  ELSIF scenario = 'disabled' THEN
   IF usapp_reminder_rule_needs_reset(rule) OR usapp_reminder_task_next_fire(b, succ) IS NOT NULL THEN RAISE EXCEPTION 'disabled copy misreported'; END IF;
  ELSE
   IF usapp_reminder_rule_needs_reset(rule) THEN RAISE EXCEPTION 'usable copy reported'; END IF;
   IF usapp_reminder_task_next_fire(b, succ) IS DISTINCT FROM (new_deadline - interval '30 minutes') THEN RAISE EXCEPTION 'usable copy has no next fire'; END IF;
  END IF;
  IF (SELECT count(*) FROM reminder_rules x WHERE x.task_id = pred) <> (CASE scenario WHEN 'usable' THEN 1 ELSE 2 END) THEN RAISE EXCEPTION 'predecessor rules changed'; END IF;
  SET CONSTRAINTS ALL IMMEDIATE;
  SET CONSTRAINTS ALL DEFERRED;
  cases := cases+1;
 END LOOP;

 -- ---- R5: the source of a pending push (0028). A one-off whose deadline, and so stale_at, is to the second.
 r5_stale := date_trunc('second', clock_timestamp() + interval '10 minutes');
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint,deadline_local_date,deadline_local_time,deadline_at,created_at)
  VALUES (r5_task,space,a,b,'r5',1,1,tag||'-r5-t',repeat('a',64),(r5_stale AT TIME ZONE 'Asia/Shanghai')::date,(r5_stale AT TIME ZONE 'Asia/Shanghai')::time,r5_stale,clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,r5_task,true,'before_deadline',30,tag||'-r5-1',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,task_id,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.task',r5_task,r5_stale-interval '30 minutes',r5_stale) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.task','task',r5_task,occ) RETURNING id INTO r5_notice;
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'a due reminder reported stale'; END IF;
 -- The window ends at stale_at exactly, and the deadline is that same instant.
 IF NOT usapp_reminder_occurrence_valid(r5_notice, r5_stale - interval '1 microsecond') THEN RAISE EXCEPTION 'stale before stale_at'; END IF;
 IF usapp_reminder_occurrence_valid(r5_notice, r5_stale) THEN RAISE EXCEPTION 'valid at stale_at'; END IF;
 IF usapp_reminder_occurrence_valid(r5_notice, r5_stale + interval '1 second') THEN RAISE EXCEPTION 'valid after stale_at'; END IF;
 -- A time-only edit is still the reminder that was due, and the instant is not recomputed.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,2,r5_task,true,'before_deadline',5,tag||'-r5-2',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'a time-only edit invalidated it'; END IF;
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,3,r5_task,false,tag||'-r5-3',repeat('a',64));
 IF usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'valid after disable'; END IF;
 -- Derived from the current rule: enabling again restores what no terminal state has decided.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,task_id,enabled,mode,before_minutes,idempotency_key,request_fingerprint)
  VALUES (space,b,4,r5_task,true,'before_deadline',5,tag||'-r5-4',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'not valid after enabling again'; END IF;
 -- A notification that is no reminder, and an unknown id.
 SELECT n.id INTO notice FROM notifications n WHERE n.reminder_occurrence_id IS NULL LIMIT 1;
 IF notice IS NOT NULL AND NOT usapp_reminder_occurrence_valid(notice) THEN RAISE EXCEPTION 'a non-reminder reported stale'; END IF;
 IF NOT usapp_reminder_occurrence_valid(gen_random_uuid()) THEN RAISE EXCEPTION 'an unknown id reported stale'; END IF;
 -- A read-only function: nothing in the inbox changes.
 IF (SELECT count(*) FROM notification_reads x WHERE x.notification_id = r5_notice) <> 0 THEN RAISE EXCEPTION 'a read appeared'; END IF;
 cases := cases+10;

 -- An anniversary due today, target tomorrow. The window ends at the midnight after the fire date.
 r5_time := date_trunc('minute', (clock_timestamp() AT TIME ZONE 'Asia/Shanghai'))::time;
 today := (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
 INSERT INTO anniversaries(id,couple_id,creator_id,created_at) VALUES (r5_item,space,a,clock_timestamp()-interval '3 days');
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint,created_at)
  VALUES (r5_item,space,1,a,'create','r5',today+1,'countdown','',tag||'-r5-a',repeat('a',64),clock_timestamp()-interval '3 days');
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint,created_at)
  VALUES (space,b,1,r5_item,true,'anniversary_offsets','{1,3}',r5_time,tag||'-r5-a1',repeat('a',64),clock_timestamp()-interval '1 day') RETURNING id INTO rule;
 r5_stale := ((today+1)::timestamp AT TIME ZONE 'Asia/Shanghai');
 INSERT INTO reminder_occurrences(couple_id,owner_id,rule_id,event_kind,anniversary_id,anniversary_version,schedule_version,target_date,days_before,fire_at,stale_at)
  VALUES (space,b,rule,'reminder.anniversary',r5_item,1,1,today+1,1,((today + r5_time) AT TIME ZONE 'Asia/Shanghai'),r5_stale) RETURNING id INTO occ;
 INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,reminder_occurrence_id)
  VALUES (space,NULL,b,'reminder.anniversary','anniversary',r5_item,occ) RETURNING id INTO r5_notice;
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'a due anniversary reported stale'; END IF;
 IF NOT usapp_reminder_occurrence_valid(r5_notice, r5_stale - interval '1 microsecond') THEN RAISE EXCEPTION 'anniversary stale before midnight'; END IF;
 IF usapp_reminder_occurrence_valid(r5_notice, r5_stale) THEN RAISE EXCEPTION 'anniversary valid at midnight'; END IF;
 -- Title and note are not part of the schedule.
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint)
  VALUES (r5_item,space,2,a,'edit','renamed',today+1,'countdown','noted',tag||'-r5-a2',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'a cosmetic edit invalidated it'; END IF;
 -- Away and back: judged only by the current schedule.
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint)
  VALUES (r5_item,space,3,a,'edit','renamed',today+9,'countdown','noted',tag||'-r5-a3',repeat('a',64));
 IF usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'valid with the date moved away'; END IF;
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint)
  VALUES (r5_item,space,4,a,'edit','renamed',today+1,'countdown','noted',tag||'-r5-a4',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'not valid with the date moved back'; END IF;
 -- To a yearly schedule on the same day of an earlier year: the target still falls on it.
 INSERT INTO anniversary_revisions(anniversary_id,couple_id,version,actor_id,action,title,event_date,kind,note,idempotency_key,request_fingerprint)
  VALUES (r5_item,space,5,a,'edit','renamed',((today+1) - interval '4 years')::date,'anniversary','noted',tag||'-r5-a5',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'not valid on the same yearly date'; END IF;
 -- A changed time keeps it; removing this offset, or disabling, does not.
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,2,r5_item,true,'anniversary_offsets','{1,3}','23:59',tag||'-r5-a6',repeat('a',64));
 IF NOT usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'a changed time invalidated it'; END IF;
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,mode,days_before_set,at_time,idempotency_key,request_fingerprint)
  VALUES (space,b,3,r5_item,true,'anniversary_offsets','{3}','23:59',tag||'-r5-a7',repeat('a',64));
 IF usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'valid with its offset removed'; END IF;
 INSERT INTO reminder_rules(couple_id,owner_id,seq,anniversary_id,enabled,idempotency_key,request_fingerprint)
  VALUES (space,b,4,r5_item,false,tag||'-r5-a8',repeat('a',64));
 IF usapp_reminder_occurrence_valid(r5_notice) THEN RAISE EXCEPTION 'anniversary valid after disable'; END IF;
 cases := cases+10;

 RAISE NOTICE 'ALL REMINDER LEDGER ASSERTIONS PASSED: % cases as %', cases, current_user;
END
$test$;
ROLLBACK;
