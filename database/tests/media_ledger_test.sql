-- P3-1a: rollback-only guards on private media (0029). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 m uuid; m2 uuid; m3 uuid; aged uuid; key1 text; key2 text; got text; anchor uuid := gen_random_uuid(); anchor_text uuid;
 app boolean := current_user = 'usapp_api'; cases integer := 0;
 stmt text;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'p31a-'||tag||'@example.com','p31a-'||tag||'@example.com'),
  (b,'p31b-'||tag||'@example.com','p31b-'||tag||'@example.com'),
  (c,'p31c-'||tag||'@example.com','p31c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p31',a),(other_space,'p31-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

 -- ---- Grants: the shape of everything the store will be told.
 m := gen_random_uuid();
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,'media/x','image/png',1,clock_timestamp()+interval '1 minute',%L,repeat('a',64))$s$, m, space, a, tag||'-k1'),
  format($s$INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,'image/svg+xml',1,clock_timestamp()+interval '1 minute',%L,repeat('a',64))$s$, m, space, a, 'staging/'||space||'/'||m||'/upload', tag||'-k2'),
  format($s$INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,'image/png',20000001,clock_timestamp()+interval '1 minute',%L,repeat('a',64))$s$, m, space, a, 'staging/'||space||'/'||m||'/upload', tag||'-k3'),
  format($s$INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,'image/png',1,clock_timestamp()+interval '2 hours',%L,repeat('a',64))$s$, m, space, a, 'staging/'||space||'/'||m||'/upload', tag||'-k4')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad grant accepted: %', left(stmt, 80);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;
 -- An uploader of another couple: the composite FK.
 BEGIN
  INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint)
  VALUES (m,space,c,'staging/'||space||'/'||m||'/upload','image/png',1,clock_timestamp()+interval '1 minute',tag||'-k5',repeat('a',64));
  RAISE EXCEPTION 'foreign uploader accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;

 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint)
 VALUES (m,space,a,'staging/'||space||'/'||m||'/upload','image/png',1000,clock_timestamp()+interval '15 minutes',tag||'-m1',repeat('a',64));

 -- ---- Attempts: never a staging key, another object's path, or out of order.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key) VALUES (%L,1,%L,%L)$s$, m, space, 'staging/'||space||'/'||m||'/upload'),
  format($s$INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key) VALUES (%L,1,%L,%L)$s$, m, space, 'media/'||other_space||'/'||m||'/'||gen_random_uuid()||'/original')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad attempt accepted: %', left(stmt, 80);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;
 BEGIN
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key)
  VALUES (m,2,space,'media/'||space||'/'||m||'/'||gen_random_uuid()||'/original');
  RAISE EXCEPTION 'attempt out of order accepted';
 EXCEPTION WHEN SQLSTATE 'UF003' THEN cases := cases+1; END;
 key1 := 'media/'||space||'/'||m||'/'||gen_random_uuid()||'/original';
 key2 := 'media/'||space||'/'||m||'/'||gen_random_uuid()||'/original';
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key) VALUES (m,1,space,key1),(m,2,space,key2);
 BEGIN
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key) VALUES (m,3,space,key1);
  RAISE EXCEPTION 'a reused final key accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;

 -- ---- Dispositions before anything is decided: everything may still be running.
 IF usapp_media_key_disposition(key1) <> 'live' OR usapp_media_key_disposition('staging/'||space||'/'||m||'/upload') <> 'live'
    OR usapp_media_key_disposition('media/unknown') <> 'unknown' THEN RAISE EXCEPTION 'dispositions before a decision'; END IF;
 cases := cases+1;

 -- ---- Outcomes agree with the grant, and READY needs an attempt and all its facts.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,real_format,width,height,stored_size,etag) VALUES (%L,%L,'ready','image/png',4,4,1000,'e')$s$, m, space),
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag) VALUES (%L,%L,'ready',1,'image/png',30001,4,1000,'e')$s$, m, space),
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,reason) VALUES (%L,%L,'rejected','because')$s$, m, space)
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad outcome accepted: %', left(stmt, 80);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag) VALUES (%L,%L,'ready',1,'image/gif',4,4,1000,'e')$s$, m, space),
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag) VALUES (%L,%L,'ready',1,'image/jpeg',4,4,1000,'e')$s$, m, space),
  format($s$INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag) VALUES (%L,%L,'ready',1,'image/png',4,4,999,'e')$s$, m, space)
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'outcome differing from the grant accepted';
  EXCEPTION WHEN SQLSTATE 'UF004' THEN cases := cases+1; END;
 END LOOP;
 BEGIN
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag)
  VALUES (m,other_space,'ready',1,'image/png',4,4,1000,'e');
  RAISE EXCEPTION 'another couple accepted';
 EXCEPTION WHEN foreign_key_violation OR SQLSTATE 'UF004' THEN cases := cases+1; END;

 -- ---- Binding and plans before READY.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m,space,'memory',gen_random_uuid(),a);
  RAISE EXCEPTION 'binding before READY accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'everything','abandoned',clock_timestamp());
  RAISE EXCEPTION 'abandoned while the grant runs accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'leftovers','ready',clock_timestamp());
  RAISE EXCEPTION 'leftovers before READY accepted';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;

 -- ---- READY names attempt 2. Attempt 1 becomes a leftover; READY's key is kept from then on.
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,crc64)
 VALUES (m,space,'ready',2,'image/png',4,4,1000,'e','c');
 IF usapp_media_key_disposition(key2) <> 'keep' THEN RAISE EXCEPTION 'READY key not kept'; END IF;
 BEGIN
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,reason) VALUES (m,space,'rejected','not_image');
  RAISE EXCEPTION 'second outcome accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key) VALUES (m,3,space,'media/'||space||'/'||m||'/'||gen_random_uuid()||'/original');
  RAISE EXCEPTION 'attempt after an outcome accepted';
 EXCEPTION WHEN SQLSTATE 'UF003' THEN cases := cases+1; END;
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'leftovers','ready',clock_timestamp()+interval '1 hour');
 IF usapp_media_key_disposition(key1) <> 'pending_sweep' OR usapp_media_key_disposition(key2) <> 'keep'
    OR usapp_media_key_disposition('staging/'||space||'/'||m||'/upload') <> 'pending_sweep' THEN RAISE EXCEPTION 'dispositions after READY'; END IF;
 -- A sweep cannot be recorded before it is due, and only a dead key can be recorded as removed.
 BEGIN
  INSERT INTO media_cleanup_passes(media_id,scope,kind) VALUES (m,'leftovers','sweep');
  RAISE EXCEPTION 'early sweep accepted';
 EXCEPTION WHEN SQLSTATE 'UF006' THEN cases := cases+1; END;
 INSERT INTO media_cleanup_passes(media_id,scope,kind) VALUES (m,'leftovers','first');
 BEGIN
  INSERT INTO media_late_objects(object_key,disposition,source) VALUES (key2,'removed','reconcile');
  RAISE EXCEPTION 'READY key recorded as removed';
 EXCEPTION WHEN SQLSTATE 'UF008' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_late_objects(object_key,disposition,source) VALUES (key1,'report_only','reconcile');
  RAISE EXCEPTION 'a known key reported as unknown';
 EXCEPTION WHEN SQLSTATE 'UF008' THEN cases := cases+1; END;
 INSERT INTO media_late_objects(object_key,disposition,source) VALUES (key1,'removed','attempt_resume');
 INSERT INTO media_late_objects(object_key,disposition,source) VALUES ('media/unknown-'||tag,'report_only','reconcile');
 cases := cases+3;

 -- ---- Young and READY: not unattached. Bound: never everything.
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'young object planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;
 -- Since 0030 a binding names a real memory of this couple, and the uploader (or an approver) binds.
 INSERT INTO memories(id,couple_id,creator_id) VALUES (anchor,space,a);
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'anchor') RETURNING id INTO anchor_text;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (anchor,space,1,a,'create',DATE '2026-01-01',anchor_text,tag||'-anchor',repeat('a',64));
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m,space,'memory',anchor,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (anchor,1,1,m,space);
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m,'everything','rejected',clock_timestamp());
  RAISE EXCEPTION 'bound object planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m,space,'memory',anchor,a);
  RAISE EXCEPTION 'one home only: a second binding accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;

 -- ---- The fence: a plan stops every later attempt and outcome.
 aged := gen_random_uuid();
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 VALUES (aged,space,a,'staging/'||space||'/'||aged||'/upload','image/png',1000,clock_timestamp()-interval '1 hour',tag||'-aged',repeat('a',64),clock_timestamp()-interval '1 hour 10 minutes');
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 VALUES (aged,1,space,'media/'||space||'/'||aged||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '1 hour');
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (aged,'everything','abandoned',clock_timestamp()-interval '1 second');
 BEGIN
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag)
  VALUES (aged,space,'ready',1,'image/png',4,4,1000,'e');
  RAISE EXCEPTION 'READY after a plan accepted';
 EXCEPTION WHEN SQLSTATE 'UF004' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_OUTCOME_CLEANUP' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
  VALUES (aged,2,space,'media/'||space||'/'||aged||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '1 hour');
  RAISE EXCEPTION 'attempt after a plan accepted';
 EXCEPTION WHEN SQLSTATE 'UF003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_ATTEMPT_CLEANUP' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- Past the plan's time, the sweep may be recorded; a late object is then late.
 INSERT INTO media_cleanup_passes(media_id,scope,kind) VALUES (aged,'everything','sweep');
 IF usapp_media_key_disposition('staging/'||space||'/'||aged||'/upload') <> 'late' THEN RAISE EXCEPTION 'late staging not detected'; END IF;
 IF usapp_media_key_disposition((SELECT final_key FROM media_promotion_attempts WHERE media_id = aged)) <> 'late' THEN RAISE EXCEPTION 'late final key not detected'; END IF;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (aged,space,'memory',gen_random_uuid(),a);
  RAISE EXCEPTION 'binding of an unready planned object accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN cases := cases+1; END;
 cases := cases+2;

 -- ---- A READY object old enough: unattached may be planned, and removes READY's protection too.
 m2 := gen_random_uuid();
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 VALUES (m2,space,a,'staging/'||space||'/'||m2||'/upload','image/png',1000,clock_timestamp()-interval '9 days',tag||'-m2',repeat('a',64),clock_timestamp()-interval '9 days 00:10:00');
 key1 := 'media/'||space||'/'||m2||'/'||gen_random_uuid()||'/original';
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at) VALUES (m2,1,space,key1,clock_timestamp()-interval '9 days');
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 VALUES (m2,space,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '8 days');
 IF usapp_media_key_disposition(key1) <> 'keep' THEN RAISE EXCEPTION 'old READY not kept'; END IF;
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m2,'everything','unattached',clock_timestamp());
 IF usapp_media_key_disposition(key1) <> 'pending_sweep' THEN RAISE EXCEPTION 'planned READY still kept'; END IF;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (m2,space,'memory',gen_random_uuid(),a);
  RAISE EXCEPTION 'binding after a plan accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_CLEANUP' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;

 -- ---- A rejected object is planned in the same way, with no attempt needed.
 m3 := gen_random_uuid();
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint)
 VALUES (m3,space,a,'staging/'||space||'/'||m3||'/upload','image/png',1000,clock_timestamp()+interval '15 minutes',tag||'-m3',repeat('a',64));
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,reason) VALUES (m3,space,'rejected','attempts_exhausted');
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (m3,'everything','rejected',clock_timestamp());
 cases := cases+1;

 -- ---- Append-only, for each table.
 FOREACH stmt IN ARRAY ARRAY[
  'UPDATE media_objects SET declared_size = declared_size', 'DELETE FROM media_objects',
  'UPDATE media_promotion_attempts SET final_key = final_key', 'DELETE FROM media_promotion_attempts',
  'UPDATE media_upload_outcomes SET etag = etag', 'DELETE FROM media_upload_outcomes',
  'UPDATE media_cleanup_plans SET reason = reason', 'DELETE FROM media_cleanup_plans',
  'UPDATE media_cleanup_passes SET kind = kind', 'DELETE FROM media_cleanup_passes',
  'UPDATE media_bindings SET target_id = target_id', 'DELETE FROM media_bindings',
  'UPDATE media_late_objects SET source = source', 'DELETE FROM media_late_objects'
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'mutation accepted: %', stmt;
  EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
   WHEN SQLSTATE 'UF001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;
 END LOOP;
 IF has_table_privilege('usapp_push', 'public.media_objects', 'SELECT') THEN RAISE EXCEPTION 'usapp_push can read media'; END IF;
 cases := cases+1;

 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'ALL MEDIA LEDGER ASSERTIONS PASSED: % cases as %', cases, current_user;
END
$test$;
ROLLBACK;
