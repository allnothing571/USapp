-- D6 M1b: rollback-only guards on memory delete requests (0037). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 photo uuid := gen_random_uuid(); foreign_photo uuid := gen_random_uuid();
 m uuid := gen_random_uuid(); fm uuid := gen_random_uuid(); txt uuid; ftxt uuid;
 r1 uuid; r2 uuid; r3 uuid; r4 uuid; prop uuid := gen_random_uuid(); d1 uuid := gen_random_uuid(); d2 uuid;
 fp text := repeat('a', 64); cases integer := 0; t timestamptz;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'m1ra-'||tag||'@example.com','m1ra-'||tag||'@example.com'),
  (b,'m1rb-'||tag||'@example.com','m1rb-'||tag||'@example.com'),
  (c,'m1rc-'||tag||'@example.com','m1rc-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'m1r',a),(other_space,'m1r-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 SELECT x.id, x.sp, x.who, 'staging/'||x.sp||'/'||x.id||'/upload', 'image/png', 1000, clock_timestamp()-interval '30 days', tag||'-'||x.tagpart, fp, clock_timestamp()-interval '30 days 00:10:00'
   FROM (VALUES (photo,space,a,'p'),(foreign_photo,other_space,c,'f')) x(id,sp,who,tagpart);
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 SELECT x.id,1,x.sp,'media/'||x.sp||'/'||x.id||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '30 days'
   FROM (VALUES (photo,space),(foreign_photo,other_space)) x(id,sp);
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 SELECT x.id,x.sp,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '29 days'
   FROM (VALUES (photo,space),(foreign_photo,other_space)) x(id,sp);
 INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES (m,space,a,clock_timestamp()-interval '20 days'),(fm,other_space,c,clock_timestamp()-interval '20 days');
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'m1b title','m1b words') RETURNING id INTO txt;
 INSERT INTO memory_texts(couple_id,title) VALUES (other_space,'m1b foreign') RETURNING id INTO ftxt;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint,created_at) VALUES
  (m,space,1,a,'create',DATE '2026-01-01',txt,tag||'-r1',fp,clock_timestamp()-interval '19 days'),
  (fm,other_space,1,c,'create',DATE '2026-01-01',ftxt,tag||'-r2',fp,clock_timestamp()-interval '19 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photo,space,'memory',m,a),(foreign_photo,other_space,'memory',fm,c);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (m,1,1,photo,space),(fm,1,1,foreign_photo,other_space);

 -- ---- A request: actor, target, time, and shape.
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,a,tag||'-bad1',fp);
  RAISE EXCEPTION 'creator request accepted';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_ACTOR' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (fm,space,b,tag||'-bad2',fp);
  RAISE EXCEPTION 'cross-space request accepted';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_TARGET' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint,created_at) VALUES (m,space,b,tag||'-bad3',fp,clock_timestamp()+interval '1 hour');
  RAISE EXCEPTION 'future request accepted';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_TIME' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,'short',fp);
  RAISE EXCEPTION 'bad key accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;

 -- ---- Pending: it excludes a second request, an edit proposal, a direct edit, and a cycle. The memory is otherwise unchanged.
 INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q1',fp) RETURNING id INTO r1;
 IF usapp_memory_pending_delete_request(m) IS DISTINCT FROM r1 OR usapp_memory_deleted_now(m) THEN RAISE EXCEPTION 'pending state'; END IF;
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q2',fp);
  RAISE EXCEPTION 'second request accepted';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_PENDING' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,m,b,1,DATE '2026-01-02',txt,tag||'-p1',fp);
  RAISE EXCEPTION 'proposal accepted while a delete request is pending';
 EXCEPTION WHEN SQLSTATE 'UG004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_PENDING' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (m,space,2,a,'edit',DATE '2026-01-02',txt,tag||'-e1',fp);
  RAISE EXCEPTION 'direct edit accepted while a delete request is pending';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_REQUEST_PENDING' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,1,tag||'-x1',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'cycle accepted while a delete request is pending';
 EXCEPTION WHEN SQLSTATE 'UE002' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETION_REQUEST_PENDING' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- A decision: actor, target, shape and uniqueness.
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,b,'rejected',tag||'-bad4',fp);
  RAISE EXCEPTION 'requester rejection accepted';
 EXCEPTION WHEN SQLSTATE 'UE005' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_DECISION_ACTOR' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'withdrawn',tag||'-bad5',fp);
  RAISE EXCEPTION 'creator withdrawal accepted';
 EXCEPTION WHEN SQLSTATE 'UE005' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_DECISION_ACTOR' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,fm,other_space,c,'rejected',tag||'-bad6',fp);
  RAISE EXCEPTION 'cross-space decision accepted';
 EXCEPTION WHEN SQLSTATE 'UE005' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETE_DECISION_TARGET' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'approved',tag||'-bad7',fp);
  RAISE EXCEPTION 'approval with no cycle accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,deletion_id,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'rejected',gen_random_uuid(),tag||'-bad8',fp);
  RAISE EXCEPTION 'rejection naming a cycle accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'maybe',tag||'-bad9',fp);
  RAISE EXCEPTION 'bad outcome accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;

 -- ---- Rejected: free again, and final.
 INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'rejected',tag||'-d1',fp);
 IF usapp_memory_pending_delete_request(m) IS NOT NULL THEN RAISE EXCEPTION 'still pending'; END IF;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,b,'withdrawn',tag||'-d2',fp);
  RAISE EXCEPTION 'second decision accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;

 -- ---- Pending edit proposal: it excludes a request.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (prop,space,m,b,1,DATE '2026-01-02',txt,tag||'-p2',fp);
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,photo,space);
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q3',fp);
  RAISE EXCEPTION 'request accepted while an edit proposal is pending';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_PROPOSAL_PENDING' THEN RAISE; END IF; cases := cases+1; END;
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (prop,space,b,'withdrawn',tag||'-p3',fp);

 -- ---- Withdrawn by the requester.
 INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q4',fp) RETURNING id INTO r2;
 INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r2,m,space,b,'withdrawn',tag||'-d3',fp);

 -- ---- A request that was rejected or withdrawn has deleted no photo, and neither has a pending one. Only a cycle does, since M1c.
 IF EXISTS (SELECT 1 FROM media_deletions WHERE media_id = photo) THEN RAISE EXCEPTION 'a request alone deleted a photo'; END IF;

 -- ---- Approved: the decision names a cycle not written yet, the cycle follows, and the commit-time checks hold. All at the top level: they read xmin.
 INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q5',fp) RETURNING id INTO r3;
 INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,deletion_id,idempotency_key,request_fingerprint) VALUES (r3,m,space,a,'approved',d1,tag||'-d4',fp);
 t := clock_timestamp();
 INSERT INTO memory_deletions(id,memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (d1,m,space,a,1,tag||'-d4',fp,t,t+interval '168 hours');
 PERFORM usapp_memory_cascade_photos(d1);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 IF NOT usapp_memory_deleted_now(m) THEN RAISE EXCEPTION 'approved but not deleted'; END IF;
 IF usapp_media_shared_now(photo) THEN RAISE EXCEPTION 'still shared after approval'; END IF;

 -- ---- Deleted: no request and no decision, of a settled request or otherwise.
 BEGIN
  INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q6',fp);
  RAISE EXCEPTION 'request on a deleted memory accepted';
 EXCEPTION WHEN SQLSTATE 'UE004' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (r1,m,space,a,'rejected',tag||'-d5',fp);
  RAISE EXCEPTION 'decision on a deleted memory accepted';
 EXCEPTION WHEN SQLSTATE 'UE005' THEN IF SQLERRM <> 'USAPP_MEMORY_DELETED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- An undo changes no decision: it stays approved, nothing is pending, and a new request and cycle follow.
 INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint) VALUES (d1,m,space,a,tag||'-u1',fp);
 PERFORM usapp_memory_cascade_undo(d1);
 IF usapp_memory_deleted_now(m) OR usapp_memory_pending_delete_request(m) IS NOT NULL THEN RAISE EXCEPTION 'undo state'; END IF;
 IF (SELECT outcome FROM memory_delete_decisions WHERE request_id = r3) IS DISTINCT FROM 'approved' THEN RAISE EXCEPTION 'decision changed'; END IF;
 INSERT INTO memory_delete_requests(memory_id,couple_id,requester_id,idempotency_key,request_fingerprint) VALUES (m,space,b,tag||'-q7',fp) RETURNING id INTO r4;
 d2 := gen_random_uuid();
 -- One cycle names one decision.
 BEGIN
  INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,deletion_id,idempotency_key,request_fingerprint) VALUES (r4,m,space,a,'approved',d1,tag||'-d6',fp);
  RAISE EXCEPTION 'a cycle named twice';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 INSERT INTO memory_delete_decisions(request_id,memory_id,couple_id,decided_by,outcome,deletion_id,idempotency_key,request_fingerprint) VALUES (r4,m,space,a,'approved',d2,tag||'-d7',fp);
 t := clock_timestamp();
 INSERT INTO memory_deletions(id,memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (d2,m,space,a,2,tag||'-d7',fp,t,t+interval '168 hours');
 PERFORM usapp_memory_cascade_photos(d2);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 IF usapp_memory_active_deletion(m) IS DISTINCT FROM d2 THEN RAISE EXCEPTION 'second cycle'; END IF;

 -- ---- Append-only: a statement trigger refuses even a statement that touches no row.
 BEGIN UPDATE memory_delete_requests SET request_fingerprint = fp WHERE false; RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN DELETE FROM memory_delete_requests WHERE false; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN UPDATE memory_delete_decisions SET request_fingerprint = fp WHERE false; RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN DELETE FROM memory_delete_decisions WHERE false; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;

 -- ---- Nothing in M1b touches text or photos.
 IF (SELECT description FROM memory_texts WHERE id = txt) IS DISTINCT FROM 'm1b words' THEN RAISE EXCEPTION 'description changed'; END IF;
 IF (SELECT count(*) FROM media_bindings WHERE target_id = m) <> 1 THEN RAISE EXCEPTION 'bindings changed'; END IF;

 RAISE NOTICE 'ALL MEMORY DELETE REQUEST LEDGER ASSERTIONS PASSED: % cases', cases;
END
$test$;
ROLLBACK;
