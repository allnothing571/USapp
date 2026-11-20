-- P3-2b: rollback-only guards on album rename requests (0032). Run as usapp_api and as a superuser.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 alb uuid := gen_random_uuid(); other_alb uuid := gen_random_uuid();
 p1 uuid := gen_random_uuid(); p2 uuid := gen_random_uuid(); p3 uuid := gen_random_uuid(); p4 uuid := gen_random_uuid();
 t1 uuid; t2 uuid; t3 uuid; t4 uuid; t5 uuid; t_other uuid;
 got text; stmt text; def text;
 app boolean := current_user = 'usapp_api'; cases integer := 0;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'p32ra-'||tag||'@example.com','p32ra-'||tag||'@example.com'),
  (b,'p32rb-'||tag||'@example.com','p32rb-'||tag||'@example.com'),
  (c,'p32rc-'||tag||'@example.com','p32rc-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p32r',a),(other_space,'p32r-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);
 INSERT INTO album_texts(couple_id,name) VALUES (space,'one') RETURNING id INTO t1;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'two') RETURNING id INTO t2;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'three') RETURNING id INTO t3;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'four') RETURNING id INTO t4;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'five') RETURNING id INTO t5;
 INSERT INTO album_texts(couple_id,name) VALUES (other_space,'other') RETURNING id INTO t_other;
 INSERT INTO albums(id,couple_id,creator_id) VALUES (alb,space,a),(other_alb,other_space,c);
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,1,a,'create',t1,tag||'-r1',repeat('a',64));
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (other_alb,other_space,1,c,'create',t_other,tag||'-r2',repeat('a',64));

 -- ---- Revision shape, after 0032.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id) VALUES (%L,%L,2,%L,'edit',%L)$s$, alb, space, a, t2),
  format($s$INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key) VALUES (%L,%L,2,%L,'edit',%L,%L)$s$, alb, space, a, t2, tag||'-n1'),
  format($s$INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,proposal_id,idempotency_key,request_fingerprint) VALUES (%L,%L,2,%L,'edit',%L,%L,%L,repeat('a',64))$s$, alb, space, a, t2, gen_random_uuid(), tag||'-n2'),
  format($s$INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (%L,%L,2,%L,'rename',%L,%L,repeat('a',64))$s$, alb, space, a, t2, tag||'-n3')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad revision accepted: %', left(stmt, 120);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;

 -- ---- Proposal: partner only, the current version, the same couple.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,1,%L,%L,repeat('b',64))$s$, space, alb, a, t2, tag||'-x1'),
  format($s$INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,1,%L,%L,repeat('b',64))$s$, space, other_alb, b, t2, tag||'-x2'),
  format($s$INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,1,%L,%L,repeat('b',64))$s$, space, gen_random_uuid(), b, t2, tag||'-x3')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad proposer accepted: %', left(stmt, 120);
  EXCEPTION WHEN SQLSTATE 'UH007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_ALBUM_PROPOSER' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 END LOOP;
 BEGIN
  INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (space,alb,c,1,t2,tag||'-x4',repeat('b',64));
  RAISE EXCEPTION 'proposer of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (space,alb,b,1,t_other,tag||'-x5',repeat('b',64));
  RAISE EXCEPTION 'text of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (space,alb,b,2,t2,tag||'-x6',repeat('b',64));
  RAISE EXCEPTION 'future base accepted';
 EXCEPTION WHEN SQLSTATE 'UH007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_PROPOSAL_BASE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO album_edit_proposals(id,couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (p1,space,alb,b,1,t2,tag||'-p1',repeat('b',64));

 -- ---- Pending: a second request and a direct rename are both refused.
 BEGIN
  INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (space,alb,b,1,t3,tag||'-x7',repeat('b',64));
  RAISE EXCEPTION 'second pending accepted';
 EXCEPTION WHEN SQLSTATE 'UH007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_PROPOSAL_PENDING' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,2,a,'edit',t3,tag||'-x8',repeat('a',64));
  RAISE EXCEPTION 'direct rename while pending accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_PROPOSAL_PENDING' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;

 -- ---- Decision actors.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,'rejected',%L,repeat('c',64))$s$, p1, space, b, tag||'-d1'),
  format($s$INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,'approved',%L,repeat('c',64))$s$, p1, space, b, tag||'-d2'),
  format($s$INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,'withdrawn',%L,repeat('c',64))$s$, p1, space, a, tag||'-d3')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad decider accepted: %', left(stmt, 120);
  EXCEPTION WHEN SQLSTATE 'UH008' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_ALBUM_DECISION_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 END LOOP;
 BEGIN
  INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,other_space,c,'approved',tag||'-d4',repeat('c',64));
  RAISE EXCEPTION 'decision across couples accepted';
 EXCEPTION WHEN SQLSTATE 'UH008' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_DECISION_TARGET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (gen_random_uuid(),space,a,'rejected',tag||'-d5',repeat('c',64));
  RAISE EXCEPTION 'decision on nothing accepted';
 EXCEPTION WHEN SQLSTATE 'UH008' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'maybe',tag||'-d6',repeat('c',64));
  RAISE EXCEPTION 'bad outcome accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;

 -- ---- An approval without its decision, by the wrong actor, or with another text.
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,2,a,'approve',p1,t2);
  RAISE EXCEPTION 'approve revision with no decision accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_APPROVAL' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- An approved decision with no revision fails at the deferred check.
 BEGIN
  INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'approved',tag||'-d7',repeat('c',64));
  SET CONSTRAINTS ALL IMMEDIATE;
  RAISE EXCEPTION 'approval with no revision accepted';
 EXCEPTION WHEN SQLSTATE 'UH006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_APPROVAL_REVISION' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 SET CONSTRAINTS ALL DEFERRED;
 INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'approved',tag||'-d8',repeat('c',64));
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,2,a,'approve',p1,t3);
  RAISE EXCEPTION 'approval with another text accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_APPROVAL' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,2,b,'approve',p1,t2);
  RAISE EXCEPTION 'approval by the partner accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,2,a,'approve',p1,t2);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 cases := cases+1;
 -- One decision per proposal; one revision per approval.
 BEGIN
  INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'rejected',tag||'-d9',repeat('c',64));
  RAISE EXCEPTION 'second decision accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,3,a,'approve',p1,t2);
  RAISE EXCEPTION 'second approval revision accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN cases := cases+1; WHEN unique_violation THEN cases := cases+1; END;

 -- ---- After a decision: direct rename is free again; an old base is refused; reject and withdraw change nothing.
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,3,a,'edit',t3,tag||'-r3',repeat('a',64));
 BEGIN
  INSERT INTO album_edit_proposals(couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (space,alb,b,2,t4,tag||'-x9',repeat('b',64));
  RAISE EXCEPTION 'old base accepted';
 EXCEPTION WHEN SQLSTATE 'UH007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_PROPOSAL_BASE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO album_edit_proposals(id,couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (p2,space,alb,b,3,t4,tag||'-p2',repeat('b',64));
 INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p2,space,a,'rejected',tag||'-d10',repeat('c',64));
 INSERT INTO album_edit_proposals(id,couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (p3,space,alb,b,3,t4,tag||'-p3',repeat('b',64));
 INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p3,space,b,'withdrawn',tag||'-d11',repeat('c',64));
 IF (SELECT max(version) FROM album_revisions WHERE album_id = alb) <> 3 THEN RAISE EXCEPTION 'decision moved the version'; END IF;
 cases := cases+1;

 -- ---- The stale guards. Unreachable through any permitted write, so forced: superuser only, inside a block that undoes everything.
 INSERT INTO album_edit_proposals(id,couple_id,album_id,proposer_id,base_version,text_id,idempotency_key,request_fingerprint) VALUES (p4,space,alb,b,3,t5,tag||'-p4',repeat('b',64));
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 IF NOT app THEN
  BEGIN
   ALTER TABLE album_revisions DISABLE TRIGGER album_revisions_guard;
   INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,4,a,'edit',t3,tag||'-s1',repeat('a',64));
   ALTER TABLE album_revisions ENABLE TRIGGER album_revisions_guard;
   INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p4,space,a,'approved',tag||'-s2',repeat('c',64));
   RAISE EXCEPTION 'stale approval accepted';
  EXCEPTION WHEN SQLSTATE 'UH008' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_ALBUM_DECISION_STALE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
  BEGIN
   ALTER TABLE album_revisions DISABLE TRIGGER album_revisions_guard;
   INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,4,a,'edit',t3,tag||'-s3',repeat('a',64));
   ALTER TABLE album_revisions ENABLE TRIGGER album_revisions_guard;
   ALTER TABLE album_edit_decisions DISABLE TRIGGER USER;
   INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p4,space,a,'approved',tag||'-s4',repeat('c',64));
   INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,5,a,'approve',p4,t5);
   RAISE EXCEPTION 'approve revision on a stale base accepted';
  EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_ALBUM_APPROVAL' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 END IF;
 -- The forced state left nothing behind, and the same request approves normally.
 IF (SELECT max(version) FROM album_revisions WHERE album_id = alb) <> 3 THEN RAISE EXCEPTION 'forced stale state stayed'; END IF;
 INSERT INTO album_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p4,space,a,'approved',tag||'-d12',repeat('c',64));
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,proposal_id,text_id) VALUES (alb,space,4,a,'approve',p4,t5);
 cases := cases+1;

 -- The definition of the deferred check.
 SELECT pg_get_triggerdef(t.oid) INTO def FROM pg_trigger t WHERE t.tgrelid = 'public.album_edit_decisions'::regclass AND t.tgname = 'album_edit_decisions_approval_revision'
    AND t.tgdeferrable AND t.tginitdeferred AND t.tgconstraint <> 0;
 IF def IS NULL OR def NOT LIKE '%''approved''%' THEN RAISE EXCEPTION 'trigger definition: %', def; END IF;
 cases := cases+1;

 -- ---- Append-only, for each table.
 FOREACH stmt IN ARRAY ARRAY[
  'UPDATE album_edit_proposals SET proposer_id = proposer_id', 'DELETE FROM album_edit_proposals',
  'UPDATE album_edit_decisions SET outcome = outcome', 'DELETE FROM album_edit_decisions'
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'mutation accepted: %', stmt;
  EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
   WHEN SQLSTATE 'UH001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;
 END LOOP;
 IF has_table_privilege('usapp_push', 'public.album_edit_proposals', 'SELECT') OR has_table_privilege('usapp_push', 'public.album_edit_decisions', 'SELECT') THEN RAISE EXCEPTION 'usapp_push can read requests'; END IF;
 cases := cases+1;

 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'ALL ALBUM RENAME LEDGER ASSERTIONS PASSED: % cases as %', cases, current_user;
END
$test$;
ROLLBACK;
