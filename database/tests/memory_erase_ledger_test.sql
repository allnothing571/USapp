-- D6 M1d: rollback-only guards on text erasure (0039). Run as usapp_api and as a superuser. Nothing here calls a store.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid();
 space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 p1 uuid := gen_random_uuid(); p2 uuid := gen_random_uuid(); p3 uuid := gen_random_uuid();
 m uuid := gen_random_uuid(); open_memory uuid := gen_random_uuid(); undone uuid := gen_random_uuid();
 t1 uuid; t2 uuid; t3 uuid; t_open uuid; t_undone uuid; prop uuid := gen_random_uuid();
 d uuid; d_open uuid; d_undone uuid;
 fp text := repeat('a', 64); erased text := repeat('0', 64); cases integer := 0; t timestamptz; outcome text;
 is_app constant boolean := NOT (SELECT rolsuper FROM pg_roles WHERE rolname = current_user);
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'m1da-'||tag||'@example.com','m1da-'||tag||'@example.com'),
  (b,'m1db-'||tag||'@example.com','m1db-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'m1d',a);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2);
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
 SELECT x, space, a, 'staging/'||space||'/'||x||'/upload', 'image/png', 1000, clock_timestamp()-interval '30 days', tag||'-'||x, fp, clock_timestamp()-interval '30 days 00:10:00'
   FROM unnest(ARRAY[p1,p2,p3]) x;
 INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
 SELECT x, 1, space, 'media/'||space||'/'||x||'/'||gen_random_uuid()||'/original', clock_timestamp()-interval '30 days' FROM unnest(ARRAY[p1,p2,p3]) x;
 INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
 SELECT x, space, 'ready', 1, 'image/png', 4, 4, 1000, 'e', clock_timestamp()-interval '29 days' FROM unnest(ARRAY[p1,p2,p3]) x;
 INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES (m,space,a,clock_timestamp()-interval '20 days'),(open_memory,space,a,clock_timestamp()-interval '20 days'),(undone,space,a,clock_timestamp()-interval '20 days');
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'m1d-first','m1d-first words') RETURNING id INTO t1;
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'m1d-second','m1d-second words') RETURNING id INTO t2;
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'m1d-asked','m1d-asked words') RETURNING id INTO t3;
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'m1d-open') RETURNING id INTO t_open;
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'m1d-undone') RETURNING id INTO t_undone;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint,created_at) VALUES
  (m,space,1,a,'create',DATE '2026-01-01',t1,tag||'-r1',fp,clock_timestamp()-interval '19 days'),
  (m,space,2,a,'edit',DATE '2026-01-02',t2,tag||'-r2',fp,clock_timestamp()-interval '18 days'),
  (open_memory,space,1,a,'create',DATE '2026-01-03',t_open,tag||'-r3',fp,clock_timestamp()-interval '19 days'),
  (undone,space,1,a,'create',DATE '2026-01-04',t_undone,tag||'-r4',fp,clock_timestamp()-interval '19 days');
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (p1,space,'memory',m,a),(p2,space,'memory',open_memory,a),(p3,space,'memory',undone,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (m,1,1,p1,space),(m,2,1,p1,space),(open_memory,1,1,p2,space),(undone,1,1,p3,space);
 -- A rejected request's text is the memory's too.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
 VALUES (prop,space,m,b,2,DATE '2026-01-05',t3,tag||'-e1',fp,clock_timestamp()-interval '17 days');
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,p1,space);
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint,decided_at)
 VALUES (prop,space,a,'rejected',tag||'-e2',repeat('b', 64),clock_timestamp()-interval '16 days');

 -- ---- Before any cycle.
 BEGIN PERFORM usapp_memory_erase(gen_random_uuid()); RAISE EXCEPTION 'an absent cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_TARGET' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- A cycle inside its window.
 t := clock_timestamp()-interval '167 hours';
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (open_memory,space,a,1,tag||'-d1',fp,t,t+interval '168 hours') RETURNING id INTO d_open;
 PERFORM usapp_memory_cascade_photos(d_open);
 BEGIN PERFORM usapp_memory_erase(d_open); RAISE EXCEPTION 'an open cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_WINDOW' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- A reversed cycle, though its deadline would not matter.
 t := clock_timestamp()-interval '167 hours';
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (undone,space,a,1,tag||'-d2',fp,t,t+interval '168 hours') RETURNING id INTO d_undone;
 PERFORM usapp_memory_cascade_photos(d_undone);
 INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint) VALUES (d_undone,undone,space,a,tag||'-u1',fp);
 PERFORM usapp_memory_cascade_undo(d_undone);
 BEGIN PERFORM usapp_memory_erase(d_undone); RAISE EXCEPTION 'a reversed cycle accepted';
 EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_REVERSED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- An expired cycle: every text and text-derived fingerprint, and nothing else.
 t := clock_timestamp()-interval '168 hours 1 second';
 INSERT INTO memory_deletions(memory_id,couple_id,deleted_by,cycle_no,idempotency_key,request_fingerprint,deleted_at,purge_after)
 VALUES (m,space,a,1,tag||'-d3',fp,t,t+interval '168 hours') RETURNING id INTO d;
 PERFORM usapp_memory_cascade_photos(d);
 -- Before the record exists the narrow path is closed, to whoever asks. The role only a superuser can take.
 IF NOT is_app THEN
  SET LOCAL ROLE usapp_memory_eraser;
  BEGIN UPDATE memory_texts SET title = '[erased]', description = '' WHERE id = t1; RAISE EXCEPTION 'an erasure with no record accepted';
  EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_UNRECORDED' THEN RAISE; END IF; cases := cases+1; END;
  BEGIN UPDATE memory_revisions SET request_fingerprint = erased WHERE memory_id = m; RAISE EXCEPTION 'a fingerprint with no record accepted';
  EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_UNRECORDED' THEN RAISE; END IF; cases := cases+1; END;
  RESET ROLE;
 END IF;
 outcome := usapp_memory_erase(d);
 IF outcome <> 'erased' THEN RAISE EXCEPTION 'outcome: %', outcome; END IF;
 IF (SELECT count(*) FROM memory_texts WHERE id IN (t1,t2,t3) AND title = '[erased]' AND description = '') <> 3 THEN RAISE EXCEPTION 'a text survived'; END IF;
 IF (SELECT count(*) FROM memory_revisions WHERE memory_id = m AND request_fingerprint = erased) <> 2 THEN RAISE EXCEPTION 'a revision fingerprint survived'; END IF;
 IF (SELECT request_fingerprint FROM memory_edit_proposals WHERE id = prop) <> erased THEN RAISE EXCEPTION 'a proposal fingerprint survived'; END IF;
 -- Ids only: a decision's fingerprint holds no text, and stays. So do keys, dates, the text links and the row counts.
 IF (SELECT request_fingerprint FROM memory_edit_decisions WHERE proposal_id = prop) <> repeat('b', 64) THEN RAISE EXCEPTION 'a decision fingerprint changed'; END IF;
 IF (SELECT array_agg(text_id ORDER BY version) FROM memory_revisions WHERE memory_id = m) IS DISTINCT FROM ARRAY[t1,t2] THEN RAISE EXCEPTION 'a text link changed'; END IF;
 IF (SELECT array_agg(memory_date ORDER BY version) FROM memory_revisions WHERE memory_id = m) IS DISTINCT FROM ARRAY[DATE '2026-01-01', DATE '2026-01-02'] THEN RAISE EXCEPTION 'a date changed'; END IF;
 IF (SELECT array_agg(idempotency_key ORDER BY version) FROM memory_revisions WHERE memory_id = m) IS DISTINCT FROM ARRAY[tag||'-r1', tag||'-r2'] THEN RAISE EXCEPTION 'a key changed'; END IF;
 IF NOT EXISTS (SELECT 1 FROM memory_purges WHERE deletion_id = d AND memory_id = m AND couple_id = space) THEN RAISE EXCEPTION 'no record'; END IF;
 -- Others untouched.
 IF (SELECT title FROM memory_texts WHERE id = t_open) <> 'm1d-open' OR (SELECT title FROM memory_texts WHERE id = t_undone) <> 'm1d-undone' THEN RAISE EXCEPTION 'another memory changed'; END IF;
 cases := cases+1;

 -- ---- Again: nothing more, and the same answer.
 IF usapp_memory_erase(d) <> 'already' THEN RAISE EXCEPTION 'repeat'; END IF;
 IF (SELECT count(*) FROM memory_purges WHERE memory_id = m) <> 1 THEN RAISE EXCEPTION 'two records'; END IF;
 cases := cases+1;

 -- ---- It stays gone: the reversal guard answers before the clock.
 BEGIN
  INSERT INTO memory_deletion_reversals(deletion_id,memory_id,couple_id,reversed_by,idempotency_key,request_fingerprint) VALUES (d,m,space,a,tag||'-u2',fp);
  RAISE EXCEPTION 'an erased cycle reversed';
 EXCEPTION WHEN SQLSTATE 'UE003' THEN IF SQLERRM <> 'USAPP_MEMORY_REVERSAL_PURGED' THEN RAISE; END IF; cases := cases+1; END;

 -- ---- The narrow path, for those outside it.
 BEGIN UPDATE memory_texts SET title = 'recovered' WHERE id = t1; RAISE EXCEPTION 'an update accepted';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT is_app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UG001' THEN IF is_app THEN RAISE; END IF; cases := cases+1; END;
 BEGIN DELETE FROM memory_texts WHERE false; RAISE EXCEPTION 'a delete accepted';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT is_app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UG001' THEN IF is_app THEN RAISE; END IF; cases := cases+1; END;
 BEGIN INSERT INTO memory_purges(deletion_id,memory_id,couple_id) VALUES (d_open,open_memory,space); RAISE EXCEPTION 'a direct record accepted';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT is_app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UE006' THEN IF is_app OR SQLERRM <> 'USAPP_MEMORY_ERASE_WINDOW' THEN RAISE; END IF; cases := cases+1; END;
 BEGIN UPDATE memory_purges SET couple_id = couple_id WHERE false; RAISE EXCEPTION 'a record changed';
 EXCEPTION WHEN insufficient_privilege THEN IF NOT is_app THEN RAISE; END IF; cases := cases+1;
  WHEN SQLSTATE 'UE001' THEN IF is_app THEN RAISE; END IF; cases := cases+1; END;
 -- An application login cannot take the eraser's role. SET ROLE is judged by the session user, so prevalidation, which only pretends to be the
 -- application inside a superuser session, cannot ask this; the standalone run and the C++ boundary case do, on real logins.
 IF is_app AND session_user = 'usapp_api' THEN
  BEGIN SET LOCAL ROLE usapp_memory_eraser; RAISE EXCEPTION 'the application took the eraser role';
  EXCEPTION WHEN insufficient_privilege THEN cases := cases+1; END;
 ELSIF NOT is_app THEN
  -- Even the eraser's own role writes only the fixed values, in only those columns.
  SET LOCAL ROLE usapp_memory_eraser;
  BEGIN UPDATE memory_texts SET title = 'recovered' WHERE id = t1; RAISE EXCEPTION 'another value accepted';
  EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_VALUE' THEN RAISE; END IF; cases := cases+1; END;
  BEGIN UPDATE memory_revisions SET request_fingerprint = fp WHERE memory_id = m; RAISE EXCEPTION 'another fingerprint accepted';
  EXCEPTION WHEN SQLSTATE 'UE006' THEN IF SQLERRM <> 'USAPP_MEMORY_ERASE_VALUE' THEN RAISE; END IF; cases := cases+1; END;
  BEGIN UPDATE memory_revisions SET memory_date = DATE '2026-02-02' WHERE memory_id = m; RAISE EXCEPTION 'another column accepted';
  EXCEPTION WHEN insufficient_privilege THEN cases := cases+1; END;
  RESET ROLE;
 END IF;

 RAISE NOTICE 'ALL MEMORY ERASE LEDGER ASSERTIONS PASSED: % cases', cases;
END
$test$;
ROLLBACK;
