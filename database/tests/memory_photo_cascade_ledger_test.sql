-- D6 M1c: rollback-only guards on the memory photo cascade (0038). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid();
 space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 p1 uuid := gen_random_uuid(); p2 uuid := gen_random_uuid(); own uuid := gen_random_uuid(); partner uuid := gen_random_uuid(); spare uuid := gen_random_uuid();
 m uuid := gen_random_uuid(); other uuid := gen_random_uuid(); txt uuid; prop uuid := gen_random_uuid();
 d1 uuid; d2 uuid; own_cycle uuid; linked uuid; n integer; fp text := repeat('a', 64); cases integer := 0; t timestamptz;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'m1ca-'||tag||'@example.com','m1ca-'||tag||'@example.com'),
  (b,'m1cb-'||tag||'@example.com','m1cb-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'m1c',a);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2);
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 SELECT x.id, space, x.who, 'staging/'||space||'/'||x.id||'/upload', 'image/png', 1000, clock_timestamp()-interval '30 days', tag||'-'||x.tagpart, fp, clock_timestamp()-interval '30 days 00:10:00'
   FROM (VALUES (p1,a,'1'),(p2,a,'2'),(own,a,'3'),(partner,b,'4'),(spare,a,'5')) x(id,who,tagpart);
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 SELECT x, 1, space, 'media/'||space||'/'||x||'/'||gen_random_uuid()||'/original', clock_timestamp()-interval '30 days' FROM unnest(ARRAY[p1,p2,own,partner,spare]) x;
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 SELECT x, space, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp()-interval '29 days' FROM unnest(ARRAY[p1,p2,own,partner,spare]) x;
 INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES (m,space,a,clock_timestamp()-interval '20 days'),(other,space,a,clock_timestamp()-interval '20 days');
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'m1c title') RETURNING id INTO txt;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint,created_at) VALUES
  (m,space,1,a,'create',DATE '2026-01-01',txt,tag||'-r1',fp,clock_timestamp()-interval '19 days'),
  (other,space,1,a,'create',DATE '2026-01-01',txt,tag||'-r2',fp,clock_timestamp()-interval '19 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (p1,space,'memory',m,a),(p2,space,'memory',m,a),(own,space,'memory',m,a),(spare,space,'memory',other,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (m,1,1,p1,space),(m,1,2,p2,space),(m,1,3,own,space),(other,1,1,spare,space);
 -- The partner's photo, by an approved request; p2 stays only in history after it.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
 VALUES (prop,space,m,b,1,DATE '2026-01-02',txt,tag||'-e1',fp,clock_timestamp()-interval '18 days');
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,p1,space),(prop,2,own,space),(prop,3,partner,space);
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint,decided_at)
 VALUES (prop,space,a,'approved',tag||'-e2',fp,clock_timestamp()-interval '17 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (partner,space,'memory',m,a);
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id,created_at)
 VALUES (m,space,2,a,'approve',prop,DATE '2026-01-02',txt,clock_timestamp()-interval '17 days');
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (m,2,1,p1,space),(m,2,2,own,space),(m,2,3,partner,space);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;

 -- ---- An independent cycle first: it is never linked.
 t := clock_timestamp();
 INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (own,space,a,1,tag||'-i1',fp,t,t+interval '168 hours') RETURNING id INTO own_cycle;

 -- ---- A cycle with no cascade fails at commit.
 t := clock_timestamp();
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (other,space,a,1,tag||'-o1',fp,t,t+interval '168 hours') RETURNING id INTO d2;
 BEGIN
  SET CONSTRAINTS memory_deletions_cascade_complete IMMEDIATE;
  RAISE EXCEPTION 'a cycle with no cascade passed';
 EXCEPTION WHEN SQLSTATE 'UE007' THEN IF SQLERRM <> 'USAPP_MEMORY_CASCADE_INCOMPLETE' THEN RAISE; END IF; cases := cases+1; END;
 SET CONSTRAINTS ALL DEFERRED;
 n := usapp_memory_cascade_photos(d2);
 IF n <> 1 THEN RAISE EXCEPTION 'other cascade: %', n; END IF;

 -- ---- The cascade: the creator's current and history photos, not the independent one, not the partner's. One time, one deadline.
 t := clock_timestamp();
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,1,tag||'-d1',fp,t,t+interval '168 hours') RETURNING id INTO d1;
 n := usapp_memory_cascade_photos(d1);
 IF n <> 2 THEN RAISE EXCEPTION 'cascade count: %', n; END IF;
 IF NOT usapp_media_deleted_now(p1) OR NOT usapp_media_deleted_now(p2) THEN RAISE EXCEPTION 'not deleted'; END IF;
 IF usapp_media_deleted_now(partner) THEN RAISE EXCEPTION 'partner photo deleted'; END IF;
 IF EXISTS (SELECT 1 FROM memory_deletion_photo_cycles WHERE memory_deletion_id = d1 AND media_id IN (own, partner)) THEN RAISE EXCEPTION 'linked too much'; END IF;
 IF (SELECT count(*) FROM memory_deletion_photo_cycles l JOIN media_deletions x ON x.id = l.media_deletion_id
      WHERE l.memory_deletion_id = d1 AND x.deleted_at = t AND x.purge_after = t + interval '168 hours' AND x.deleted_by = a) <> 2 THEN RAISE EXCEPTION 'times'; END IF;
 SELECT media_deletion_id INTO linked FROM memory_deletion_photo_cycles WHERE memory_deletion_id = d1 AND media_id = p1;
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 -- A repeat finds nothing new.
 IF usapp_memory_cascade_photos(d1) <> 0 THEN RAISE EXCEPTION 'cascaded twice'; END IF;

 -- ---- Links: shape and target.
 BEGIN
  INSERT INTO memory_deletion_photo_cycles(media_deletion_id,media_id,memory_deletion_id,memory_id,couple_id) VALUES (own_cycle,own,d1,other,space);
  RAISE EXCEPTION 'a link to another memory accepted';
 EXCEPTION WHEN SQLSTATE 'UE007' THEN IF SQLERRM <> 'USAPP_MEMORY_CASCADE_MEMORY_CYCLE' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_deletion_photo_cycles(media_deletion_id,media_id,memory_deletion_id,memory_id,couple_id) VALUES (gen_random_uuid(),own,d1,m,space);
  RAISE EXCEPTION 'a link to an absent cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE007' THEN IF SQLERRM <> 'USAPP_MEMORY_CASCADE_MEDIA_CYCLE' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_deletion_photo_cycles(media_deletion_id,media_id,memory_deletion_id,memory_id,couple_id) VALUES (own_cycle,own,d1,m,space);
  RAISE EXCEPTION 'an independent cycle linked';
 EXCEPTION WHEN SQLSTATE 'UE007' THEN IF SQLERRM <> 'USAPP_MEMORY_CASCADE_MISMATCH' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- A linked cycle does not undo by itself.
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint) VALUES (linked,p1,space,a,tag||'-u1',fp);
  RAISE EXCEPTION 'a standalone linked undo accepted';
 EXCEPTION WHEN SQLSTATE 'UL003' THEN IF SQLERRM <> 'USAPP_MEDIA_REVERSAL_LINKED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- An undo leaves none behind, at commit.
 INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint) VALUES (d1,m,space,a,tag||'-u2',fp);
 BEGIN
  SET CONSTRAINTS memory_deletion_reversals_cascade_complete IMMEDIATE;
  RAISE EXCEPTION 'an undo with its photos left behind passed';
 EXCEPTION WHEN SQLSTATE 'UE007' THEN IF SQLERRM <> 'USAPP_MEMORY_CASCADE_UNDO_INCOMPLETE' THEN RAISE; END IF; cases := cases+1; END;
 SET CONSTRAINTS ALL DEFERRED;
 n := usapp_memory_cascade_undo(d1);
 IF n <> 2 THEN RAISE EXCEPTION 'undo count: %', n; END IF;
 IF usapp_media_deleted_now(p1) OR usapp_media_deleted_now(p2) THEN RAISE EXCEPTION 'not restored'; END IF;
 IF NOT usapp_media_deleted_now(own) THEN RAISE EXCEPTION 'the independent one was restored'; END IF;
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;

 -- ---- Append-only.
 BEGIN UPDATE memory_deletion_photo_cycles SET couple_id = space WHERE false; RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;
 BEGIN DELETE FROM memory_deletion_photo_cycles WHERE false; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UE001' THEN cases := cases+1; WHEN insufficient_privilege THEN cases := cases+1; END;

 -- ---- Text stays: M1c erases nothing.
 IF (SELECT title FROM memory_texts WHERE id = txt) IS DISTINCT FROM 'm1c title' THEN RAISE EXCEPTION 'text changed'; END IF;

 RAISE NOTICE 'ALL MEMORY PHOTO CASCADE LEDGER ASSERTIONS PASSED: % cases', cases;
END
$test$;
ROLLBACK;
