-- D6: rollback-only guards on media deletion (0035). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 m uuid := gen_random_uuid(); n uuid := gen_random_uuid(); unready uuid := gen_random_uuid(); foreign_media uuid := gen_random_uuid();
 home uuid := gen_random_uuid(); home_text uuid; home_photo uuid := gen_random_uuid(); spare uuid := gen_random_uuid();
 album uuid := gen_random_uuid(); album_text uuid; batch uuid := gen_random_uuid();
 d1 uuid; d2 uuid; d3 uuid; prop uuid := gen_random_uuid();
 fp text := repeat('a', 64); cases integer := 0; t timestamptz;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'d6a-'||tag||'@example.com','d6a-'||tag||'@example.com'),
  (b,'d6b-'||tag||'@example.com','d6b-'||tag||'@example.com'),
  (c,'d6c-'||tag||'@example.com','d6c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'d6',a),(other_space,'d6-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

 -- READY photos, old enough to choose any age below.
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 SELECT x.id, x.sp, x.who, 'staging/'||x.sp||'/'||x.id||'/upload', 'image/png', 1000, clock_timestamp()-interval '30 days', tag||'-'||x.tagpart, fp, clock_timestamp()-interval '30 days 00:10:00'
   FROM (VALUES (m,space,a,'m'),(n,space,a,'n'),(unready,space,a,'u'),(foreign_media,other_space,c,'f'),(home_photo,space,a,'h'),(spare,space,a,'s')) x(id,sp,who,tagpart);
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 SELECT x.id,1,x.sp,'media/'||x.sp||'/'||x.id||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '30 days'
   FROM (VALUES (m,space),(n,space),(foreign_media,other_space),(home_photo,space),(spare,space)) x(id,sp);
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 SELECT x.id,x.sp,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '29 days'
   FROM (VALUES (m,space),(n,space),(foreign_media,other_space),(home_photo,space),(spare,space)) x(id,sp);

 -- ---- The boundary, at the exact instant.
 t := timestamptz '2026-06-01 12:00:00+00';
 IF NOT usapp_media_undo_open(t, t - interval '1 microsecond') OR usapp_media_undo_open(t, t) OR usapp_media_undo_open(t, t + interval '1 microsecond') THEN
  RAISE EXCEPTION 'boundary';
 END IF;
 cases := cases+1;

 -- ---- Deletion: shape, actor, state, cycle and time.
 BEGIN
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,purge_after)
  VALUES (m,space,a,1,tag||'-bad1',fp,clock_timestamp()+interval '1 day');
  RAISE EXCEPTION 'a shorter window accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,b,1,tag||'-bad2',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'partner deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (foreign_media,space,a,1,tag||'-bad3',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'cross-space deletion accepted';
 EXCEPTION WHEN foreign_key_violation OR SQLSTATE 'UL002' THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (unready,space,a,1,tag||'-bad4',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'unready deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-bad5',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'cycle out of order accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 BEGIN
  t := clock_timestamp() + interval '1 hour';
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,1,tag||'-bad6',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'future deletion accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;

 t := clock_timestamp();
 INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,1,tag||'-d1',fp,t,t+interval '168 hours') RETURNING id INTO d1;
 IF NOT usapp_media_deleted_now(m) OR usapp_media_active_deletion(m) <> d1 OR usapp_media_deleted_now(n) THEN RAISE EXCEPTION 'deleted_now'; END IF;
 cases := cases+1;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-bad7',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a second open cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 BEGIN
  UPDATE media_deletions SET purge_after = purge_after + interval '1 day' WHERE id = d1;
  RAISE EXCEPTION 'update accepted';
 EXCEPTION WHEN SQLSTATE 'UL001' OR insufficient_privilege THEN cases := cases+1; END;
 BEGIN
  DELETE FROM media_deletions WHERE id = d1;
  RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN SQLSTATE 'UL001' OR insufficient_privilege THEN cases := cases+1; END;

 -- ---- A deleted photo gains no new home, in any table.
 INSERT INTO albums(id,couple_id,creator_id) VALUES (album,space,a);
 INSERT INTO album_texts(couple_id,name) VALUES (space,'d6') RETURNING id INTO album_text;
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint)
 VALUES (album,space,1,a,'create',album_text,tag||'-ar',fp);
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m,space,'album',album,a);
  RAISE EXCEPTION 'album binding of a deleted photo accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN
  IF SQLERRM <> 'USAPP_MEDIA_BINDING_DELETED' THEN RAISE; END IF;
  cases := cases+1;
 END;

 INSERT INTO memories(id,couple_id,creator_id) VALUES (home,space,b);
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'d6') RETURNING id INTO home_text;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (home,space,1,b,'create',DATE '2026-01-01',home_text,tag||'-mr',fp);
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (n,space,'memory',home,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (home,1,1,n,space);
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m,space,'memory',home,a);
  RAISE EXCEPTION 'memory binding of a deleted photo accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN
  IF SQLERRM <> 'USAPP_MEDIA_BINDING_DELETED' THEN RAISE; END IF;
  cases := cases+1;
 END;

 -- A kept photo deleted by its uploader: the bound row stays history, and no new revision may name it.
 t := clock_timestamp();
 INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (n,space,a,1,tag||'-d3',fp,t,t+interval '168 hours') RETURNING id INTO d3;
 IF (SELECT count(*) FROM memory_revision_photos WHERE memory_id = home AND media_id = n) <> 1
    OR (SELECT count(*) FROM media_bindings WHERE media_id = n) <> 1 THEN RAISE EXCEPTION 'history changed'; END IF;
 cases := cases+1;
 -- The parents are written outside any exception block, because the photo guards' xmin rule fails closed inside a savepoint. A spare
 -- photo then satisfies the deferred checks, so a later script in one prevalidation transaction can still ask for them.
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (home,space,2,b,'edit',DATE '2026-01-01',home_text,tag||'-mr2',fp);
 BEGIN
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (home,2,1,n,space);
  RAISE EXCEPTION 'a new revision naming a deleted photo accepted';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN
  IF SQLERRM <> 'USAPP_MEMORY_PHOTO_DELETED' THEN RAISE; END IF;
  cases := cases+1;
 END;
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (spare,space,'memory',home,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (home,2,1,spare,space);

 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (prop,space,home,a,2,DATE '2026-01-02',home_text,tag||'-prop',fp);
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,m,space);
  RAISE EXCEPTION 'proposal naming a deleted photo accepted';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN
  IF SQLERRM <> 'USAPP_MEMORY_PHOTO_DELETED' THEN RAISE; END IF;
  cases := cases+1;
 END;
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,spare,space);

 -- ---- Plans: ordinary reasons never pass a deleted, bound or reserved photo. user_deleted only at the deadline, for the exact cycle.
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'unattached plan on a deleted photo accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (m,'everything','unattached',clock_timestamp(),d1);
  RAISE EXCEPTION 'a cycle on an ordinary reason accepted';
 EXCEPTION WHEN check_violation OR SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'everything','user_deleted',clock_timestamp());
  RAISE EXCEPTION 'user_deleted without a cycle accepted';
 EXCEPTION WHEN check_violation OR SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (m,'leftovers','user_deleted',clock_timestamp(),d1);
  RAISE EXCEPTION 'user_deleted leftovers accepted';
 EXCEPTION WHEN check_violation OR SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (m,'everything','user_deleted',clock_timestamp(),d1);
  RAISE EXCEPTION 'plan before the deadline accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN
  IF SQLERRM <> 'USAPP_MEDIA_PLAN_WINDOW' THEN RAISE; END IF;
  cases := cases+1;
 END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (m,'everything','user_deleted',clock_timestamp(),d3);
  RAISE EXCEPTION 'another photo''s cycle accepted';
 EXCEPTION WHEN foreign_key_violation OR SQLSTATE 'UF005' THEN cases := cases+1; END;

 -- ---- Reversal: only the deleter, only this photo. Then a new cycle.
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,m,space,b,tag||'-r-bad1',fp);
  RAISE EXCEPTION 'partner undo accepted';
 EXCEPTION WHEN SQLSTATE 'UL003' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,n,space,a,tag||'-r-bad2',fp);
  RAISE EXCEPTION 'undo under another photo accepted';
 EXCEPTION WHEN foreign_key_violation OR SQLSTATE 'UL003' THEN cases := cases+1; END;
 INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
 VALUES (d1,m,space,a,tag||'-r1',fp);
 IF usapp_media_deleted_now(m) THEN RAISE EXCEPTION 'undo did not restore'; END IF;
 cases := cases+1;
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d1,m,space,a,tag||'-r2',fp);
  RAISE EXCEPTION 'second undo of one cycle accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (m,'everything','user_deleted',clock_timestamp(),d1);
  RAISE EXCEPTION 'plan on a reversed cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN
  IF SQLERRM <> 'USAPP_MEDIA_PLAN_REVERSED' THEN RAISE; END IF;
  cases := cases+1;
 END;
 BEGIN
  t := clock_timestamp() - interval '1 hour';
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (m,space,a,2,tag||'-bad8',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a cycle older than the last undo accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 t := clock_timestamp();
 INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,2,tag||'-d2',fp,t,t+interval '168 hours') RETURNING id INTO d2;
 IF usapp_media_active_deletion(m) <> d2 THEN RAISE EXCEPTION 'cycle 2'; END IF;
 cases := cases+1;

 -- ---- An expired cycle of the bound, deleted home photo: undo closed by the clock alone, then the exact plan, then the fence.
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (home_photo,space,'memory',home,a);
 t := clock_timestamp() - interval '168 hours 1 second';
 INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (home_photo,space,a,1,tag||'-dh',fp,t,t+interval '168 hours') RETURNING id INTO d3;
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d3,home_photo,space,a,tag||'-r-late',fp);
  RAISE EXCEPTION 'undo at the deadline accepted';
 EXCEPTION WHEN SQLSTATE 'UL003' THEN
  IF SQLERRM <> 'USAPP_MEDIA_REVERSAL_WINDOW' THEN RAISE; END IF;
  cases := cases+1;
 END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (home_photo,'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'ordinary plan on a bound photo accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN
  IF SQLERRM <> 'USAPP_MEDIA_PLAN_BOUND' THEN RAISE; END IF;
  cases := cases+1;
 END;
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before,deletion_id) VALUES (home_photo,'everything','user_deleted',clock_timestamp(),d3);
 IF usapp_media_key_disposition((SELECT final_key FROM media_promotion_attempts WHERE media_id = home_photo AND attempt_no = 1)) <> 'pending_sweep' THEN
  RAISE EXCEPTION 'purge did not reach the READY key';
 END IF;
 cases := cases+1;
 BEGIN
  INSERT INTO media_deletion_reversals(deletion_id,media_id,couple_id,reversed_by,idempotency_key,request_fingerprint)
  VALUES (d3,home_photo,space,a,tag||'-r-plan',fp);
  RAISE EXCEPTION 'undo after the plan accepted';
 EXCEPTION WHEN SQLSTATE 'UL003' THEN
  IF SQLERRM <> 'USAPP_MEDIA_REVERSAL_PURGE_PLANNED' THEN RAISE; END IF;
  cases := cases+1;
 END;
 BEGIN
  t := clock_timestamp();
  INSERT INTO media_deletions(media_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
  VALUES (home_photo,space,a,2,tag||'-dh2',fp,t,t+interval '168 hours');
  RAISE EXCEPTION 'a new cycle after a plan accepted';
 EXCEPTION WHEN SQLSTATE 'UL002' THEN cases := cases+1; END;
 IF (SELECT count(*) FROM media_bindings WHERE media_id = home_photo) <> 1 THEN RAISE EXCEPTION 'purge touched the binding'; END IF;
 cases := cases+1;

 -- ---- The release clock: NULL while deleted; an undo restarts it.
 IF usapp_media_released_at(m) IS NOT NULL THEN RAISE EXCEPTION 'released_at while deleted'; END IF;
 cases := cases+1;

 RAISE NOTICE 'ALL MEDIA DELETION LEDGER ASSERTIONS PASSED (% cases, role %)', cases, current_user;
END
$test$;
ROLLBACK;
