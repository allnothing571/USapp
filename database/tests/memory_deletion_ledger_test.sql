-- D6 M1a: rollback-only guards on whole-memory deletion (0036). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 photo uuid := gen_random_uuid(); partner_photo uuid := gen_random_uuid(); loose uuid := gen_random_uuid(); foreign_photo uuid := gen_random_uuid(); spare uuid := gen_random_uuid();
 m uuid := gen_random_uuid(); pend uuid := gen_random_uuid(); fm uuid := gen_random_uuid(); txt uuid; ptxt uuid; ftxt uuid;
 prop uuid := gen_random_uuid(); pend_prop uuid := gen_random_uuid();
 d1 uuid; d2 uuid; d3 uuid; d4 uuid; plan uuid; result jsonb;
 fp text := repeat('a', 64); cases integer := 0; t timestamptz;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'m1a-'||tag||'@example.com','m1a-'||tag||'@example.com'),
  (b,'m1b-'||tag||'@example.com','m1b-'||tag||'@example.com'),
  (c,'m1c-'||tag||'@example.com','m1c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'m1',a),(other_space,'m1-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 SELECT x.id, x.sp, x.who, 'staging/'||x.sp||'/'||x.id||'/upload', 'image/png', 1000, clock_timestamp()-interval '30 days', tag||'-'||x.tagpart, fp, clock_timestamp()-interval '30 days 00:10:00'
   FROM (VALUES (photo,space,a,'p'),(partner_photo,space,b,'q'),(loose,space,a,'l'),(foreign_photo,other_space,c,'f'),(spare,space,a,'s')) x(id,sp,who,tagpart);
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 SELECT x.id,1,x.sp,'media/'||x.sp||'/'||x.id||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '30 days'
   FROM (VALUES (photo,space),(partner_photo,space),(loose,space),(foreign_photo,other_space),(spare,space)) x(id,sp);
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 SELECT x.id,x.sp,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '29 days'
   FROM (VALUES (photo,space),(partner_photo,space),(loose,space),(foreign_photo,other_space),(spare,space)) x(id,sp);

 -- Memories by a, old enough to choose any age below: one with the partner's approved photo, one with a pending request, one in another space.
 INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES
  (m,space,a,clock_timestamp()-interval '20 days'),(pend,space,a,clock_timestamp()-interval '20 days'),(fm,other_space,c,clock_timestamp()-interval '20 days');
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'m1a title','m1a words') RETURNING id INTO txt;
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'m1a pending') RETURNING id INTO ptxt;
 INSERT INTO memory_texts(couple_id,title) VALUES (other_space,'m1a foreign') RETURNING id INTO ftxt;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint,created_at) VALUES
  (m,space,1,a,'create',DATE '2026-01-01',txt,tag||'-r1',fp,clock_timestamp()-interval '19 days'),
  (pend,space,1,a,'create',DATE '2026-01-01',ptxt,tag||'-r2',fp,clock_timestamp()-interval '19 days'),
  (fm,other_space,1,c,'create',DATE '2026-01-01',ftxt,tag||'-r3',fp,clock_timestamp()-interval '19 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES
  (photo,space,'memory',m,a),(foreign_photo,other_space,'memory',fm,c);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES
  (m,1,1,photo,space),(fm,1,1,foreign_photo,other_space);
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (loose,space,'memory',pend,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (pend,1,1,loose,space);
 -- The partner's photo, by an approved request: a binding of the partner's uploaded photo that is shared through m.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
 VALUES (prop,space,m,b,1,DATE '2026-01-02',txt,tag||'-e1',fp,clock_timestamp()-interval '18 days');
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,partner_photo,space);
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint,decided_at)
 VALUES (prop,space,a,'approved',tag||'-e2',fp,clock_timestamp()-interval '17 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (partner_photo,space,'memory',m,a);
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
 VALUES (m,space,2,a,'approve',prop,DATE '2026-01-02',txt,NULL,NULL,clock_timestamp()-interval '17 days');
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (m,2,1,partner_photo,space);
 -- A request still pending on pend.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
 VALUES (pend_prop,space,pend,b,1,DATE '2026-01-03',ptxt,tag||'-e3',fp,clock_timestamp()-interval '10 days');
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (pend_prop,1,loose,space);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;

 IF usapp_memory_deleted_now(m) OR usapp_memory_active_deletion(m) IS NOT NULL OR NOT usapp_media_shared_now(partner_photo) THEN RAISE EXCEPTION 'fixture state'; END IF;

 -- ---- Deletion: shape, actor, target, state, cycle and time.
 BEGIN
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,purge_after)
  VALUES (m,space,a,1,tag||'-bad1',fp,clock_timestamp()+interval '1 day');
  RAISE EXCEPTION 'a shorter window accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,b,1,tag||'-bad2',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'partner deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_ACTOR' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (fm,space,a,1,tag||'-bad3',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'cross-space deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_TARGET' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (pend,space,a,1,tag||'-bad4',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'deletion with a pending request accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_PROPOSAL_PENDING' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-bad5',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'cycle out of order accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_CYCLE' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp()+interval '1 hour';
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,1,tag||'-bad6',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'future deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_TIME' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp()-interval '21 days';
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,1,tag||'-bad7',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'deletion before the memory accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_TIME' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,1,'short',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'bad key accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;

 -- ---- A first cycle, aged so that its deadline is still ahead.
 t := clock_timestamp()-interval '1 day';
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,1,tag||'-d1',fp,t,t+interval '168 hours') RETURNING id INTO d1;
 PERFORM usapp_memory_cascade_photos(d1);
 IF NOT usapp_memory_deleted_now(m) OR usapp_memory_active_deletion(m) IS DISTINCT FROM d1 THEN RAISE EXCEPTION 'not deleted'; END IF;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-d1b',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a second open cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_ACTIVE' THEN RAISE; END IF; cases := cases+1; END;
 -- A key belongs to its actor: the same key by another actor is allowed.
 t := clock_timestamp();
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (fm,other_space,c,1,tag||'-d1',fp,t,t+interval '168 hours') RETURNING id INTO d4;
 PERFORM usapp_memory_cascade_photos(d4);
 cases := cases+1;

 -- ---- Sharing: the partner's photo stops being shared through the deleted memory. Its binding and bytes rows stay.
 IF usapp_media_shared_now(partner_photo) OR usapp_media_shared_now(photo) THEN RAISE EXCEPTION 'still shared'; END IF;
 IF (SELECT count(*) FROM media_bindings WHERE target_id = m) <> 2 THEN RAISE EXCEPTION 'bindings changed'; END IF;
 IF (SELECT count(*) FROM media_cleanup_plans WHERE media_id IN (photo, partner_photo)) <> 0 THEN RAISE EXCEPTION 'a plan appeared'; END IF;

 -- ---- Nothing can change a deleted memory, at the guard.
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (m,space,3,a,'edit',DATE '2026-01-04',txt,tag||'-x1',fp);
  RAISE EXCEPTION 'revision on a deleted memory accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,m,b,2,DATE '2026-01-04',txt,tag||'-x2',fp);
  RAISE EXCEPTION 'proposal on a deleted memory accepted';
 EXCEPTION WHEN SQLSTATE 'UG004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (spare,space,'memory',m,a);
  RAISE EXCEPTION 'binding to a deleted memory accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN IF SQLERRM <> 'USAPP_MEDIA_BINDING_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- Reversal: target, actor and window. Each failure leaves it deleted.
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (gen_random_uuid(),m,space,a,tag||'-u1',fp);
  RAISE EXCEPTION 'reversal of an absent cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_TARGET' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,pend,space,a,tag||'-u2',fp);
  RAISE EXCEPTION 'reversal through another memory accepted';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_TARGET' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,m,space,b,tag||'-u3',fp);
  RAISE EXCEPTION 'partner reversal accepted';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_ACTOR' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint,reversed_at)
  VALUES (d1,m,space,a,tag||'-u4',fp,clock_timestamp()+interval '1 hour');
  RAISE EXCEPTION 'future reversal accepted';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_TIME' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- Append-only: a statement trigger refuses even a statement that touches no row.
 BEGIN UPDATE memory_deletions SET request_fingerprint = fp WHERE false; RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN DELETE FROM memory_deletions WHERE false; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN UPDATE memory_deletion_reversals SET request_fingerprint = fp WHERE false; RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN DELETE FROM memory_deletion_reversals WHERE false; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;

 -- ---- The undo restores sharing; a second reversal of the same cycle is refused; a new cycle is numbered on.
 INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
 VALUES (d1,m,space,a,tag||'-u5',fp);
 PERFORM usapp_memory_cascade_undo(d1);
 IF usapp_memory_deleted_now(m) OR NOT usapp_media_shared_now(partner_photo) THEN RAISE EXCEPTION 'undo did not restore'; END IF;
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,m,space,a,tag||'-u6',fp);
  RAISE EXCEPTION 'a second reversal accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-d1',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a reused key accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 t := clock_timestamp();
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,2,tag||'-d2',fp,t,t+interval '168 hours') RETURNING id INTO d2;
 PERFORM usapp_memory_cascade_photos(d2);
 IF usapp_memory_active_deletion(m) IS DISTINCT FROM d2 THEN RAISE EXCEPTION 'second cycle'; END IF;
 INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
 VALUES (d2,m,space,a,tag||'-u7',fp);
 PERFORM usapp_memory_cascade_undo(d2);

 -- ---- The deadline: a cycle past it cannot be reversed, by anyone, and stays deleted.
 t := clock_timestamp()-interval '168 hours 1 second';
 BEGIN
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,3,tag||'-old',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a cycle before the last undo accepted';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_TIME' THEN RAISE; END IF; cases := cases+1; END;
 -- Another memory has no later undo, so it can hold an expired cycle. Its pending request is decided first, as the service does.
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint)
 VALUES (pend_prop,space,a,'rejected',tag||'-e4',fp);
 IF usapp_media_pending_reference(loose) IS NOT NULL THEN RAISE EXCEPTION 'reservation not released'; END IF;
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (pend,space,a,1,tag||'-d3',fp,t,t+interval '168 hours') RETURNING id INTO d3;
 PERFORM usapp_memory_cascade_photos(d3);
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d3,pend,space,a,tag||'-u8',fp);
  RAISE EXCEPTION 'a reversal past the deadline accepted';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_WINDOW' THEN RAISE; END IF; cases := cases+1; END;
 IF NOT usapp_memory_deleted_now(pend) THEN RAISE EXCEPTION 'expired cycle restored'; END IF;
 -- A decision on a request of a deleted memory: the deletion answers before the decision's own uniqueness.
 BEGIN
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint)
  VALUES (pend_prop,space,a,'approved',tag||'-e6',fp);
  RAISE EXCEPTION 'decision accepted';
 EXCEPTION WHEN SQLSTATE 'UG005' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- Plan links: refused while deleted, by the function and by the guard; the plan's own rows are not touched.
 result := usapp_shared_plan_write(a,NULL,'create',gen_random_uuid()::text,'{"body":{"title":"M1a plan","date":null,"time":null,"place":null,"note":null}}');
 plan := (result #>> '{plan,id}')::uuid;
 result := usapp_shared_plan_write(a,plan,'advance',gen_random_uuid()::text,'{"expected_status":"want"}');
 result := usapp_shared_plan_write(a,plan,'advance',gen_random_uuid()::text,'{"expected_status":"plan"}');
 result := usapp_plan_memory_attach(a,plan,gen_random_uuid()::text,pend);
 IF result->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'link to a deleted memory: %', result; END IF;
 BEGIN
  INSERT INTO shared_plan_memory_links(plan_id,couple_id,memory_id,actor_id,mode,request_key) VALUES (plan,space,pend,a,'existing',gen_random_uuid()::text);
  RAISE EXCEPTION 'guard let a link through';
 EXCEPTION WHEN SQLSTATE 'UK006' THEN IF SQLERRM <> 'USAPP_PLAN_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;
 IF EXISTS (SELECT 1 FROM shared_plan_memory_links WHERE plan_id = plan) THEN RAISE EXCEPTION 'a link appeared'; END IF;
 result := usapp_plan_memory_attach(a,plan,tag||'-link',m);
 IF result->>'memory_id' IS DISTINCT FROM m::text THEN RAISE EXCEPTION 'link to a restored memory: %', result; END IF;

 -- ---- The text is what it was: nothing in M1a erases.
 IF (SELECT count(*) FROM memory_texts WHERE id IN (txt,ptxt) AND title LIKE 'm1a %') <> 2 THEN RAISE EXCEPTION 'text changed'; END IF;
 IF (SELECT description FROM memory_texts WHERE id = txt) IS DISTINCT FROM 'm1a words' THEN RAISE EXCEPTION 'description changed'; END IF;

 RAISE NOTICE 'ALL MEMORY DELETION LEDGER ASSERTIONS PASSED: % cases', cases;
END
$test$;
ROLLBACK;
