-- P3-1b: rollback-only guards on photo memories (0030). Run as usapp_api and as a superuser.
-- A revision's photos must arrive in its own transaction, identified by xmin. A plpgsql EXCEPTION block runs in a
-- subtransaction with its own xid, so every revision and proposal this script keeps is written at the top level.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 photos_a uuid[] := '{}'; photos_b uuid[] := '{}'; foreign_photo uuid; unready uuid; m uuid;
 mem uuid := gen_random_uuid(); mem2 uuid := gen_random_uuid(); other_mem uuid := gen_random_uuid();
 t1 uuid; t2 uuid; t3 uuid; t_other uuid; p1 uuid := gen_random_uuid(); p2 uuid := gen_random_uuid(); p3 uuid := gen_random_uuid();
 i integer; got text; stmt text; released timestamptz;
 app boolean := current_user = 'usapp_api'; cases integer := 0;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'p31ma-'||tag||'@example.com','p31ma-'||tag||'@example.com'),
  (b,'p31mb-'||tag||'@example.com','p31mb-'||tag||'@example.com'),
  (c,'p31mc-'||tag||'@example.com','p31mc-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p31m',a),(other_space,'p31m-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

 -- READY objects, all old enough that only their references decide whether cleanup may take them.
 FOR i IN 1..9 LOOP
  m := gen_random_uuid();
  INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
  VALUES (m,CASE WHEN i = 9 THEN other_space ELSE space END,CASE WHEN i = 9 THEN c WHEN i <= 4 THEN a ELSE b END,
          'staging/'||CASE WHEN i = 9 THEN other_space ELSE space END||'/'||m||'/upload','image/png',1000,clock_timestamp()-interval '9 days',tag||'-m'||i,repeat('a',64),clock_timestamp()-interval '9 days 00:10:00');
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
  VALUES (m,1,CASE WHEN i = 9 THEN other_space ELSE space END,'media/'||CASE WHEN i = 9 THEN other_space ELSE space END||'/'||m||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '9 days');
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
  VALUES (m,CASE WHEN i = 9 THEN other_space ELSE space END,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '8 days');
  IF i = 9 THEN foreign_photo := m; ELSIF i <= 4 THEN photos_a := photos_a || m; ELSE photos_b := photos_b || m; END IF;
 END LOOP;
 unready := gen_random_uuid();
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint)
 VALUES (unready,space,b,'staging/'||space||'/'||unready||'/upload','image/png',1000,clock_timestamp()+interval '15 minutes',tag||'-mu',repeat('a',64));

 -- ---- Text shape.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO memory_texts(couple_id,title) VALUES (%L,'')$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title) VALUES (%L,repeat('x',61))$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title) VALUES (%L,' x')$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title) VALUES (%L,E'a\nb')$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title,description) VALUES (%L,'x',repeat('y',2001))$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title,description) VALUES (%L,'x',E'y\n')$s$, space),
  format($s$INSERT INTO memory_texts(couple_id,title,description) VALUES (%L,'x',E'y\u0007z')$s$, space)
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad text accepted: %', left(stmt, 90);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,repeat('天',60),repeat('地',2000)) RETURNING id INTO t1;
 INSERT INTO memory_texts(couple_id,title,description) VALUES (space,'second',E'multi\nline') RETURNING id INTO t2;
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'third') RETURNING id INTO t3;
 INSERT INTO memory_texts(couple_id,title) VALUES (other_space,'other') RETURNING id INTO t_other;

 -- ---- A memory, then every way of mis-stating its first revision.
 INSERT INTO memories(id,couple_id,creator_id) VALUES (mem,space,a);
 BEGIN
  INSERT INTO memories(couple_id,creator_id) VALUES (space,c);
  RAISE EXCEPTION 'creator of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,1,b,'create',DATE '2026-01-01',t1,tag||'-rb',repeat('a',64));
  RAISE EXCEPTION 'partner wrote a revision';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,2,a,'edit',DATE '2026-01-01',t1,tag||'-rv',repeat('a',64));
  RAISE EXCEPTION 'version gap accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,1,a,'create',(clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + 2,t1,tag||'-rf',repeat('a',64));
  RAISE EXCEPTION 'future date accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_DATE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,1,a,'create',DATE '1899-12-31',t1,tag||'-ro',repeat('a',64));
  RAISE EXCEPTION 'old date accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,1,a,'create',DATE '2026-01-01',t_other,tag||'-rt',repeat('a',64));
  RAISE EXCEPTION 'text of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id)
  VALUES (mem,space,1,a,'create',DATE '2026-01-01',t1);
  RAISE EXCEPTION 'create without a key accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 -- Without a revision the memory cannot commit.
 BEGIN
  EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
  RAISE EXCEPTION 'memory without a revision accepted';
 EXCEPTION WHEN SQLSTATE 'UG006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_CREATE_REVISION' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;

 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (mem,space,1,a,'create',DATE '2026-01-01',t1,tag||'-r1',repeat('a',64));
 -- ---- Photos are bound before they are listed, and only by someone entitled.
 BEGIN
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,1,photos_a[1],space);
  RAISE EXCEPTION 'unbound photo listed';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_UNBOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'memory',gen_random_uuid(),a);
  RAISE EXCEPTION 'binding to no memory accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_TARGET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'memory',mem,b);
  RAISE EXCEPTION 'partner bound an unapproved photo';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (foreign_photo,other_space,'memory',mem,c);
  RAISE EXCEPTION 'cross-couple binding accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_TARGET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (foreign_photo,space,'memory',mem,a);
  RAISE EXCEPTION 'media of another couple accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' OR foreign_key_violation THEN cases := cases+1; END;
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'memory',mem,a);
 BEGIN
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,10,photos_a[1],space);
  RAISE EXCEPTION 'tenth position accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,1,photos_a[1],other_space);
  RAISE EXCEPTION 'photo row for another couple accepted';
 EXCEPTION WHEN SQLSTATE 'UG003' OR foreign_key_violation THEN cases := cases+1; END;
 -- No photo at all, or a gap: at commit.
 BEGIN
  EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
  RAISE EXCEPTION 'revision without photos accepted';
 EXCEPTION WHEN SQLSTATE 'UG006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_REVISION_PHOTOS' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,2,photos_a[1],space);
 BEGIN
  EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
  RAISE EXCEPTION 'position gap accepted';
 EXCEPTION WHEN SQLSTATE 'UG006' THEN cases := cases+1; END;
 BEGIN
  -- The same photo twice in one revision.
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,1,photos_a[1],space);
  RAISE EXCEPTION 'same photo twice accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[2],space,'memory',mem,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,1,photos_a[2],space);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 -- Bound: never cleanup, with or without a reason that would otherwise apply.
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_a[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'bound object planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_PLAN_BOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 IF usapp_media_released_at(photos_a[1]) IS NOT NULL THEN RAISE EXCEPTION 'bound object has a release time'; END IF;
 -- Unreferenced and old enough: its release is READY itself.
 IF usapp_media_released_at(photos_a[4]) IS NULL THEN RAISE EXCEPTION 'unreferenced object has no release time'; END IF;

 -- ---- Proposals: partner only, against current, one pending; the photos are reserved, not bound.
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,mem,a,1,DATE '2026-02-01',t2,tag||'-pa',repeat('a',64));
  RAISE EXCEPTION 'creator proposed';
 EXCEPTION WHEN SQLSTATE 'UG004' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,mem,b,2,DATE '2026-02-01',t2,tag||'-pb',repeat('a',64));
  RAISE EXCEPTION 'proposal against a version that does not exist';
 EXCEPTION WHEN SQLSTATE 'UG004' OR foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,mem,b,1,(clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date + 2,t2,tag||'-pf',repeat('a',64));
  RAISE EXCEPTION 'future proposal accepted';
 EXCEPTION WHEN SQLSTATE 'UG004' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,mem,c,1,DATE '2026-02-01',t2,tag||'-pc',repeat('a',64));
  RAISE EXCEPTION 'outsider proposed';
 EXCEPTION WHEN SQLSTATE 'UG004' OR foreign_key_violation THEN cases := cases+1; END;
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (p1,space,mem,b,1,DATE '2026-02-01',t2,tag||'-p1',repeat('a',64));
 -- Another member's unbound object, an object not READY, and an object of another couple.
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p1,1,photos_a[3],space);
  RAISE EXCEPTION 'proposal named the other member''s unbound photo';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_OWNER' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p1,1,unready,space);
  RAISE EXCEPTION 'proposal named an object not READY';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_UNAVAILABLE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p1,1,foreign_photo,space);
  RAISE EXCEPTION 'proposal named another couple''s photo';
 EXCEPTION WHEN SQLSTATE 'UG003' OR foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
  RAISE EXCEPTION 'proposal without photos accepted';
 EXCEPTION WHEN SQLSTATE 'UG006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PROPOSAL_PHOTOS' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p1,1,photos_a[1],space),(p1,2,photos_b[1],space);
 IF usapp_media_pending_reference(photos_b[1]) IS DISTINCT FROM p1 OR usapp_media_released_at(photos_b[1]) IS NOT NULL THEN RAISE EXCEPTION 'not reserved'; END IF;
 IF EXISTS (SELECT 1 FROM media_bindings WHERE media_id = photos_b[1]) THEN RAISE EXCEPTION 'a pending proposal bound its photo'; END IF;
 -- Reserved: cleanup refuses, and so does any use elsewhere.
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_b[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'reserved object planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_PLAN_RESERVED' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_proposals(couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (space,mem,b,1,DATE '2026-02-01',t2,tag||'-p2',repeat('a',64));
  RAISE EXCEPTION 'second pending proposal accepted';
 EXCEPTION WHEN SQLSTATE 'UG004' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PROPOSAL_PENDING' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
  VALUES (mem,space,2,a,'edit',DATE '2026-03-01',t3,tag||'-re',repeat('a',64));
  RAISE EXCEPTION 'edit while pending accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PROPOSAL_PENDING' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- A second memory, whose bound photo a proposal cannot borrow, and which cannot take the reserved one.
 INSERT INTO memories(id,couple_id,creator_id) VALUES (mem2,space,b);
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (mem2,space,1,b,'create',DATE '2026-01-05',t3,tag||'-m2',repeat('a',64));
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[1],space,'memory',mem2,b);
  RAISE EXCEPTION 'reserved object bound elsewhere';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_RESERVED' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[2],space,'memory',mem2,b);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem2,1,1,photos_b[2],space);

 -- ---- Decisions: who may say what; only one; approval needs its revision.
 BEGIN
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,b,'approved',tag||'-d1',repeat('a',64));
  RAISE EXCEPTION 'proposer approved';
 EXCEPTION WHEN SQLSTATE 'UG005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'withdrawn',tag||'-d2',repeat('a',64));
  RAISE EXCEPTION 'creator withdrew';
 EXCEPTION WHEN SQLSTATE 'UG005' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'maybe',tag||'-d3',repeat('a',64));
  RAISE EXCEPTION 'unknown outcome accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  -- Approval without its revision cannot commit.
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'approved',tag||'-d4',repeat('a',64));
  EXECUTE 'SET CONSTRAINTS ALL IMMEDIATE';
  RAISE EXCEPTION 'approval without a revision accepted';
 EXCEPTION WHEN SQLSTATE 'UG006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_APPROVAL_REVISION' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- Rejected: the reservation is gone, nothing was bound, and the unattached period restarts from this decision.
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'rejected',tag||'-d5',repeat('a',64));
 released := usapp_media_released_at(photos_b[1]);
 IF usapp_media_pending_reference(photos_b[1]) IS NOT NULL OR released IS NULL OR released < clock_timestamp() - interval '1 hour' THEN RAISE EXCEPTION 'not released by the decision'; END IF;
 IF EXISTS (SELECT 1 FROM media_bindings WHERE media_id = photos_b[1]) THEN RAISE EXCEPTION 'a rejection bound the photo'; END IF;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_b[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'just released object planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_PLAN_ELIGIBILITY' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p1,space,a,'rejected',tag||'-d6',repeat('a',64));
  RAISE EXCEPTION 'second decision accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id) VALUES (mem,space,2,a,'approve',p1,DATE '2026-02-01',t2);
  RAISE EXCEPTION 'revision from a rejected proposal accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_APPROVAL' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;

 -- ---- Released, the photo is usable again. Approved: revision equals the proposal; binding by the approver.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (p2,space,mem,b,1,DATE '2026-02-02',t2,tag||'-p3',repeat('a',64));
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p2,1,photos_b[1],space);
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p2,space,a,'approved',tag||'-d7',repeat('a',64));
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id) VALUES (mem,space,2,a,'approve',p2,DATE '2026-02-03',t2);
  RAISE EXCEPTION 'approval with another date accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN cases := cases+1; END;
 BEGIN
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id) VALUES (mem,space,2,a,'approve',p2,DATE '2026-02-02',t3);
  RAISE EXCEPTION 'approval with another text accepted';
 EXCEPTION WHEN SQLSTATE 'UG002' THEN cases := cases+1; END;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id) VALUES (mem,space,2,a,'approve',p2,DATE '2026-02-02',t2);
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[1],space,'memory',mem,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,2,1,photos_b[1],space);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 IF usapp_media_released_at(photos_b[1]) IS NOT NULL THEN RAISE EXCEPTION 'approved photo not held'; END IF;
 -- A photo of the other memory cannot be proposed here.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (p3,space,mem,b,2,DATE '2026-02-03',t3,tag||'-p4',repeat('a',64));
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p3,1,photos_b[2],space);
  RAISE EXCEPTION 'photo of another memory proposed';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_ELSEWHERE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- Withdrawn: only the proposer; the same release.
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (p3,1,photos_b[3],space);
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (p3,space,b,'withdrawn',tag||'-d8',repeat('a',64));
 IF usapp_media_pending_reference(photos_b[3]) IS NOT NULL OR usapp_media_released_at(photos_b[3]) IS NULL THEN RAISE EXCEPTION 'withdrawal did not release'; END IF;
 -- A later edit drops the approved photo: history keeps it, so it stays bound and out of cleanup.
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (mem,space,3,a,'edit',DATE '2026-02-03',t3,tag||'-r3',repeat('a',64));
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,3,1,photos_a[1],space);
 IF NOT EXISTS (SELECT 1 FROM memory_revision_photos WHERE memory_id = mem AND version = 2 AND media_id = photos_b[1]) THEN RAISE EXCEPTION 'history lost'; END IF;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_b[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'historical photo planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN cases := cases+1; END;

 -- ---- Append-only, for each table.
 FOREACH stmt IN ARRAY ARRAY[
  'UPDATE memories SET creator_id = creator_id', 'DELETE FROM memories',
  'UPDATE memory_texts SET title = title', 'DELETE FROM memory_texts',
  'UPDATE memory_revisions SET memory_date = memory_date', 'DELETE FROM memory_revisions',
  'UPDATE memory_revision_photos SET media_id = media_id', 'DELETE FROM memory_revision_photos',
  'UPDATE memory_edit_proposals SET memory_date = memory_date', 'DELETE FROM memory_edit_proposals',
  'UPDATE memory_proposal_photos SET media_id = media_id', 'DELETE FROM memory_proposal_photos',
  'UPDATE memory_edit_decisions SET outcome = outcome', 'DELETE FROM memory_edit_decisions'
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'mutation accepted: %', stmt;
  EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
   WHEN SQLSTATE 'UG001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;
 END LOOP;
 IF has_table_privilege('usapp_push', 'public.memories', 'SELECT') OR has_table_privilege('usapp_push', 'public.memory_texts', 'SELECT') THEN RAISE EXCEPTION 'usapp_push can read memories'; END IF;
 cases := cases+1;

 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'ALL MEMORY LEDGER ASSERTIONS PASSED: % cases as %', cases, current_user;
END
$test$;
ROLLBACK;
