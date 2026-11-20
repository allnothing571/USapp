-- P2-4 rollback-only, explicit test-database guard; never run against main.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid:=gen_random_uuid(); b uuid:=gen_random_uuid(); c uuid:=gen_random_uuid();
 space uuid:=gen_random_uuid(); other_space uuid:=gen_random_uuid(); task uuid:=gen_random_uuid();
 op uuid:=gen_random_uuid(); reaction uuid:=gen_random_uuid(); tag text:=gen_random_uuid()::text;
 got text; expected text; statement text; cases integer:=0;
BEGIN
 IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
 (a,'p24-a-'||tag||'@m3-fixture.test','p24-a-'||tag||'@m3-fixture.test'),
 (b,'p24-b-'||tag||'@m3-fixture.test','p24-b-'||tag||'@m3-fixture.test'),
 (c,'p24-c-'||tag||'@m3-fixture.test','p24-c-'||tag||'@m3-fixture.test');
 INSERT INTO couples(id,name,created_by_user_id) VALUES(space,'reaction',a),(other_space,'other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES(space,a,1),(space,b,2),(other_space,c,1);
 INSERT INTO wallet_accounts(user_id,asset_type,couple_id) VALUES(b,'flower',space);
 INSERT INTO tasks(id,couple_id,creator_id,assignee_id,title,reward_flowers,reward_sunshine,create_idempotency_key,create_fingerprint)
 VALUES(task,space,a,b,'reaction',1,0,'reaction-task-'||tag,repeat('a',64));
 INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
 VALUES(op,space,b,'TASK_REWARD','task',task::text);
 INSERT INTO task_completions(task_id,couple_id,completed_by,operation_id,reward_flowers,reward_sunshine,completed_at)
 VALUES(task,space,b,op,1,0,clock_timestamp());
 INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
 VALUES(op,space,b,'flower',1,1,1);
 -- Invalid source/author/time must hit the source guard before duplicate constraints.
 FOREACH statement IN ARRAY ARRAY[
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint) VALUES(%L,%L,%L,'emoji','heart',%L,%L)$s$,task,space,b,'reaction-self-'||tag,repeat('b',64)),
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint) VALUES(%L,%L,%L,'emoji','heart',%L,%L)$s$,task,other_space,c,'reaction-space-'||tag,repeat('b',64)),
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint,created_at) VALUES(%L,%L,%L,'emoji','heart',%L,%L,clock_timestamp()+interval '1 day')$s$,task,space,a,'reaction-future-'||tag,repeat('b',64)),
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint,created_at) VALUES(%L,%L,%L,'emoji','heart',%L,%L,DATE '1900-01-01')$s$,task,space,a,'reaction-early-'||tag,repeat('b',64))
 ] LOOP
  got:=NULL;BEGIN EXECUTE statement;EXCEPTION WHEN OTHERS THEN got:=SQLSTATE;END;
  IF got IS DISTINCT FROM 'UQ002' THEN RAISE EXCEPTION 'source guard got %',got;END IF;cases:=cases+1;
 END LOOP;
 FOREACH statement IN ARRAY ARRAY[
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint) VALUES(%L,%L,%L,'emoji','bad',%L,%L)$s$,task,space,a,'reaction-bad-'||tag,repeat('b',64)),
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,body,idempotency_key,request_fingerprint) VALUES(%L,%L,%L,'text',%L,%L,%L)$s$,task,space,a,repeat('花',41),'reaction-long-'||tag,repeat('b',64)),
 format($s$INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,body,idempotency_key,request_fingerprint) VALUES(%L,%L,%L,'text',%L,%L,%L)$s$,task,space,a,E'one\ntwo','reaction-line-'||tag,repeat('b',64))
 ] LOOP
  got:=NULL;BEGIN EXECUTE statement;EXCEPTION WHEN OTHERS THEN got:=SQLSTATE;END;
  IF got IS DISTINCT FROM '23514' THEN RAISE EXCEPTION 'content guard got %',got;END IF;cases:=cases+1;
 END LOOP;
 BEGIN
  INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint)
  VALUES(task,space,a,'emoji','heart','reaction-rollback-'||tag,repeat('d',64));
  INSERT INTO audit_logs(actor_type,action) VALUES(NULL,'task.reacted');
  RAISE EXCEPTION 'invalid audit accepted';
 EXCEPTION WHEN not_null_violation THEN cases:=cases+1; END;
 IF EXISTS(SELECT 1 FROM task_reactions WHERE completion_task_id=task) THEN RAISE EXCEPTION 'failed audit left a reaction';END IF;
 INSERT INTO task_reactions(id,completion_task_id,couple_id,author_id,kind,emoji_code,idempotency_key,request_fingerprint)
 VALUES(reaction,task,space,a,'emoji','heart','reaction-valid-'||tag,repeat('b',64));cases:=cases+1;
 BEGIN
 INSERT INTO task_reactions(completion_task_id,couple_id,author_id,kind,body,idempotency_key,request_fingerprint)
 VALUES(task,space,a,'text','谢谢','reaction-second-'||tag,repeat('c',64));
 RAISE EXCEPTION 'duplicate accepted';EXCEPTION WHEN unique_violation THEN cases:=cases+1;END;
 SET CONSTRAINTS ALL IMMEDIATE;
 expected:=CASE WHEN current_user='usapp_api' THEN '42501' ELSE 'UQ001' END;
 FOREACH statement IN ARRAY ARRAY['UPDATE task_reactions SET body=body','DELETE FROM task_reactions','TRUNCATE task_reactions'] LOOP
  got:=NULL;BEGIN EXECUTE statement;EXCEPTION WHEN OTHERS THEN got:=SQLSTATE;END;
  IF got IS DISTINCT FROM expected THEN RAISE EXCEPTION 'append-only expected %, got %',expected,got;END IF;cases:=cases+1;
 END LOOP;
 IF (SELECT count(*) FROM task_reactions WHERE completion_task_id=task)<>1 OR
 (SELECT balance_after FROM wallet_ledger_entries WHERE operation_id=op)<>1 THEN RAISE EXCEPTION 'unexpected side effect';END IF;
 RAISE NOTICE 'ALL TASK REACTION ASSERTIONS PASSED: role=% cases=%',current_user,cases;
END
$test$;
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
