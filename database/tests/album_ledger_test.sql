-- P3-2a: rollback-only guards on shared albums (0031). Run as usapp_api and as a superuser.
-- An item must arrive in its batch's transaction, identified by xmin. A plpgsql EXCEPTION block runs in a subtransaction with its
-- own xid, so every batch this script keeps is written at the top level.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 a uuid := gen_random_uuid(); b uuid := gen_random_uuid(); c uuid := gen_random_uuid();
 space uuid := gen_random_uuid(); other_space uuid := gen_random_uuid();
 tag text := replace(gen_random_uuid()::text, '-', '');
 photos_a uuid[] := '{}'; photos_b uuid[] := '{}'; foreign_photo uuid; unready uuid; m uuid;
 alb uuid := gen_random_uuid(); alb2 uuid := gen_random_uuid(); other_alb uuid := gen_random_uuid();
 mem uuid := gen_random_uuid(); mem_text uuid; mem_text2 uuid; prop uuid := gen_random_uuid(); prop2 uuid := gen_random_uuid();
 t1 uuid; t2 uuid; t3 uuid; t_other uuid; batch1 uuid := gen_random_uuid(); batch2 uuid := gen_random_uuid(); batch3 uuid := gen_random_uuid();
 i integer; got text; stmt text; def text; before_row text;
 app boolean := current_user = 'usapp_api'; cases integer := 0;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES
  (a,'p32a-'||tag||'@example.com','p32a-'||tag||'@example.com'),
  (b,'p32b-'||tag||'@example.com','p32b-'||tag||'@example.com'),
  (c,'p32c-'||tag||'@example.com','p32c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p32',a),(other_space,'p32-other',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

 -- READY objects, all old enough that only their references decide whether cleanup may take them.
 FOR i IN 1..13 LOOP
  m := gen_random_uuid();
  INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
  VALUES (m,CASE WHEN i = 13 THEN other_space ELSE space END,CASE WHEN i = 13 THEN c WHEN i <= 8 THEN a ELSE b END,
          'staging/'||CASE WHEN i = 13 THEN other_space ELSE space END||'/'||m||'/upload','image/png',1000,clock_timestamp()-interval '9 days',tag||'-m'||i,repeat('a',64),clock_timestamp()-interval '9 days 00:10:00');
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
  VALUES (m,1,CASE WHEN i = 13 THEN other_space ELSE space END,'media/'||CASE WHEN i = 13 THEN other_space ELSE space END||'/'||m||'/'||gen_random_uuid()||'/original',clock_timestamp()-interval '9 days');
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
  VALUES (m,CASE WHEN i = 13 THEN other_space ELSE space END,'ready',1,'image/png',4,4,1000,'e',clock_timestamp()-interval '8 days');
  IF i = 13 THEN foreign_photo := m; ELSIF i <= 8 THEN photos_a := photos_a || m; ELSE photos_b := photos_b || m; END IF;
 END LOOP;
 unready := gen_random_uuid();
 INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint)
 VALUES (unready,space,a,'staging/'||space||'/'||unready||'/upload','image/png',1000,clock_timestamp()+interval '15 minutes',tag||'-mu',repeat('a',64));

 -- ---- Name shape.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,'')$s$, space),
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,repeat('x',41))$s$, space),
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,' x')$s$, space),
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,E'a\nb')$s$, space),
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,E'a b')$s$, space),
  format($s$INSERT INTO album_texts(couple_id,name) VALUES (%L,E'a\u0085b')$s$, space)
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad name accepted: %', left(stmt, 90);
  EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 END LOOP;
 INSERT INTO album_texts(couple_id,name) VALUES (space,repeat('天',40)) RETURNING id INTO t1;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'second') RETURNING id INTO t2;
 INSERT INTO album_texts(couple_id,name) VALUES (space,'third') RETURNING id INTO t3;
 INSERT INTO album_texts(couple_id,name) VALUES (other_space,'other') RETURNING id INTO t_other;

 -- ---- An album, then every way of mis-stating its first revision.
 INSERT INTO albums(id,couple_id,creator_id) VALUES (alb,space,a),(alb2,space,b),(other_alb,other_space,c);
 BEGIN
  INSERT INTO albums(couple_id,creator_id) VALUES (space,c);
  RAISE EXCEPTION 'creator of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,1,b,'create',t1,tag||'-rb',repeat('a',64));
  RAISE EXCEPTION 'partner wrote a revision';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,2,a,'edit',t1,tag||'-rv',repeat('a',64));
  RAISE EXCEPTION 'version gap accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_VERSION' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,1,a,'edit',t1,tag||'-re',repeat('a',64));
  RAISE EXCEPTION 'edit as first revision accepted';
 EXCEPTION WHEN check_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,1,a,'create',t_other,tag||'-rt',repeat('a',64));
  RAISE EXCEPTION 'text of another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,other_space,1,c,'create',t_other,tag||'-rc',repeat('a',64));
  RAISE EXCEPTION 'revision across couples accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN cases := cases+1; END;
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,1,a,'create',t1,tag||'-r1',repeat('a',64));
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb2,space,1,b,'create',t2,tag||'-r2',repeat('a',64));
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (other_alb,other_space,1,c,'create',t_other,tag||'-r3',repeat('a',64));
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,2,a,'edit',t2,tag||'-r1',repeat('a',64));
  RAISE EXCEPTION 'key reused';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 -- The edit rule ships in this migration, its service path is P3-2b: the creator only, contiguous.
 BEGIN
  INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,2,b,'edit',t2,tag||'-rp',repeat('a',64));
  RAISE EXCEPTION 'partner edit accepted';
 EXCEPTION WHEN SQLSTATE 'UH002' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO album_revisions(album_id,couple_id,version,actor_id,action,text_id,idempotency_key,request_fingerprint) VALUES (alb,space,2,a,'edit',t3,tag||'-r4',repeat('a',64));

 -- A memory and two proposals for the binding guard's memory branch and cross-kind cases.
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'mem') RETURNING id INTO mem_text;
 INSERT INTO memory_texts(couple_id,title) VALUES (space,'proposed') RETURNING id INTO mem_text2;
 INSERT INTO memories(id,couple_id,creator_id) VALUES (mem,space,a);
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (mem,space,1,a,'create',DATE '2026-01-01',mem_text,tag||'-mr1',repeat('a',64));
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[8],space,'memory',mem,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,1,1,photos_a[8],space);
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (prop,space,mem,b,1,DATE '2026-01-02',mem_text2,tag||'-mp1',repeat('a',64));
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop,1,photos_b[1],space);

 -- A memory binding with no album item is never queued for the album check.
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 cases := cases+1;

 -- ---- The binding guard, in its order, for an album target.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (unready,space,'album',alb,a);
  RAISE EXCEPTION 'unready bound';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_NOT_READY' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- NOT_READY comes before the target check, for a wrong target too.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (unready,space,'album',gen_random_uuid(),b);
  RAISE EXCEPTION 'unready bound';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_NOT_READY' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_a[7],'everything','unattached',clock_timestamp());
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[7],space,'album',gen_random_uuid(),b);
  RAISE EXCEPTION 'planned bound';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_CLEANUP' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by,created_at) VALUES (photos_a[1],space,'album',gen_random_uuid(),b,clock_timestamp()+interval '1 day');
  RAISE EXCEPTION 'future binding accepted';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_TIME' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'album',%L,%L)$s$, photos_a[1], space, gen_random_uuid(), a),
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'album',%L,%L)$s$, photos_a[1], space, other_alb, a),
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'album',%L,%L)$s$, photos_a[1], space, mem, a),
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'memory',%L,%L)$s$, photos_a[1], space, alb, a),
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'task',%L,%L)$s$, photos_a[1], space, alb, a),
  format($s$INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (%L,%L,'album',%L,%L)$s$, foreign_photo, other_space, alb, c)
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad target accepted: %', left(stmt, 120);
  EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_MEDIA_BINDING_TARGET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 END LOOP;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[2],space,'album',alb,a);
  RAISE EXCEPTION 'partner bound another member''s photo';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (foreign_photo,space,'album',alb,a);
  RAISE EXCEPTION 'foreign photo bound';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN cases := cases+1; END;
 -- Reserved: refused for the album, even by its uploader.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[1],space,'album',alb2,b);
  RAISE EXCEPTION 'reserved photo bound';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_RESERVED' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- An approved proposal lets its approver bind to the memory, never to an album. The decision comes first, as in the service.
 INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint) VALUES (prop,space,a,'approved',tag||'-md1',repeat('a',64));
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[1],space,'album',alb,a);
  RAISE EXCEPTION 'approver bound to an album';
 EXCEPTION WHEN SQLSTATE 'UF007' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_BINDING_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,proposal_id,memory_date,text_id) VALUES (mem,space,2,a,'approve',prop,DATE '2026-01-02',mem_text2);
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[1],space,'memory',mem,a);
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,2,1,photos_b[1],space);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 cases := cases+1;

 -- ---- A batch, its bindings and items.
 FOREACH stmt IN ARRAY ARRAY[
  format($s$INSERT INTO album_photo_batches(album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,repeat('a',64))$s$, other_alb, space, a, tag||'-bx1'),
  format($s$INSERT INTO album_photo_batches(album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,repeat('a',64))$s$, gen_random_uuid(), space, a, tag||'-bx2'),
  format($s$INSERT INTO album_photo_batches(album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (%L,%L,%L,%L,repeat('a',64))$s$, alb, space, c, tag||'-bx3')
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'bad batch accepted: %', left(stmt, 120);
  EXCEPTION WHEN SQLSTATE 'UH003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
   IF got <> 'USAPP_ALBUM_BATCH_TARGET' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 END LOOP;
 IF usapp_album_batch_valid(0) OR usapp_album_batch_valid(21) OR NOT usapp_album_batch_valid(1) OR NOT usapp_album_batch_valid(20) THEN RAISE EXCEPTION 'batch limits'; END IF;
 cases := cases+1;
 INSERT INTO album_photo_batches(id,album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (batch1,alb,space,a,tag||'-b1',repeat('b',64));
 BEGIN
  INSERT INTO album_photo_batches(album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (alb2,space,a,tag||'-b1',repeat('b',64));
  RAISE EXCEPTION 'batch key reused';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 -- No binding yet.
 BEGIN
  INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb,1,space,photos_a[1],batch1,a);
  RAISE EXCEPTION 'unbound item accepted';
 EXCEPTION WHEN SQLSTATE 'UH003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ITEM_UNBOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'album',alb,a),(photos_a[2],space,'album',alb,a),(photos_a[3],space,'album',alb,a);
 -- One home: the binding's key refuses a second one of any kind, and the item's refuses a second album.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'album',alb2,a);
  RAISE EXCEPTION 'second binding accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'memory',mem,a);
  RAISE EXCEPTION 'album photo bound to a memory';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (mem,space,3,a,'edit',DATE '2026-01-03',mem_text,tag||'-mr3',repeat('a',64));
 BEGIN
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,3,1,photos_a[1],space);
  RAISE EXCEPTION 'album photo in a memory revision';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_UNBOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES (mem,3,1,photos_a[8],space);
 -- Seq: gaps refused; another batch, another actor refused. A batch written in a savepoint fails closed.
 BEGIN
  INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb,2,space,photos_a[1],batch1,a);
  RAISE EXCEPTION 'seq gap accepted';
 EXCEPTION WHEN SQLSTATE 'UH003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ITEM_SEQ' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb,1,space,photos_a[1],batch1,b);
  RAISE EXCEPTION 'item by another actor accepted';
 EXCEPTION WHEN SQLSTATE 'UH003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ITEM_BATCH' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb2,1,space,photos_a[1],batch1,a);
  RAISE EXCEPTION 'item with a batch of another album accepted';
 EXCEPTION WHEN SQLSTATE 'UH003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_ITEM_BATCH' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_photo_batches(id,album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (batch3,alb,space,a,tag||'-b3',repeat('b',64));
  BEGIN
   INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb,1,space,photos_a[1],batch3,a);
   RAISE EXCEPTION 'batch in a savepoint accepted';
  EXCEPTION WHEN SQLSTATE 'UH003' THEN cases := cases+1; END;
  RAISE EXCEPTION USING ERRCODE = 'UZ999', MESSAGE = 'undo';
 EXCEPTION WHEN SQLSTATE 'UZ999' THEN NULL; END;
 -- Several rows in one statement: each guard sees the rows before it.
 INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by)
 SELECT alb, o.n, space, o.v, batch1, a FROM unnest(ARRAY[photos_a[3],photos_a[2],photos_a[1]]) WITH ORDINALITY o(v,n) ORDER BY o.n;
 IF (SELECT array_agg(media_id ORDER BY seq DESC) FROM album_items WHERE album_id = alb) <> ARRAY[photos_a[1],photos_a[2],photos_a[3]] THEN RAISE EXCEPTION 'order'; END IF;
 cases := cases+1;
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 -- The other member's own photo, in the same album.
 INSERT INTO album_photo_batches(id,album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (batch2,alb,space,b,tag||'-b2',repeat('b',64));
 INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_b[2],space,'album',alb,b);
 INSERT INTO album_items(album_id,seq,couple_id,media_id,batch_id,added_by) VALUES (alb,4,space,photos_b[2],batch2,b);
 SET CONSTRAINTS ALL IMMEDIATE;
 SET CONSTRAINTS ALL DEFERRED;
 cases := cases+1;

 -- ---- The read rule and the cleanup rule on an active item.
 IF NOT usapp_media_shared_now(photos_a[1]) OR NOT usapp_media_shared_now(photos_a[8]) OR NOT usapp_media_shared_now(photos_b[1])
    OR usapp_media_shared_now(photos_a[4]) OR usapp_media_shared_now(unready) THEN RAISE EXCEPTION 'shared_now active'; END IF;
 IF usapp_media_released_at(photos_a[1]) IS NOT NULL THEN RAISE EXCEPTION 'album photo released'; END IF;
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_a[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'active album photo planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_PLAN_BOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 -- An album photo cannot be proposed for a memory.
 INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint)
 VALUES (prop2,space,mem,b,3,DATE '2026-01-04',mem_text,tag||'-mp2',repeat('a',64));
 BEGIN
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop2,1,photos_b[2],space);
  RAISE EXCEPTION 'album photo proposed';
 EXCEPTION WHEN SQLSTATE 'UG003' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEMORY_PHOTO_ELSEWHERE' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES (prop2,1,photos_b[3],space);

 -- ---- Cover.
 BEGIN
  INSERT INTO album_cover_events(album_id,event_no,couple_id,media_id,actor_id,idempotency_key,request_fingerprint) VALUES (alb,2,space,photos_a[1],a,tag||'-c0',repeat('c',64));
  RAISE EXCEPTION 'event gap accepted';
 EXCEPTION WHEN SQLSTATE 'UH005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_COVER_EVENT' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_cover_events(album_id,event_no,couple_id,media_id,actor_id,idempotency_key,request_fingerprint) VALUES (alb,1,space,photos_a[4],a,tag||'-c1',repeat('c',64));
  RAISE EXCEPTION 'cover outside the album accepted';
 EXCEPTION WHEN SQLSTATE 'UH005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_COVER_ITEM' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_cover_events(album_id,event_no,couple_id,media_id,actor_id,idempotency_key,request_fingerprint) VALUES (alb,1,space,photos_a[1],c,tag||'-c2',repeat('c',64));
  RAISE EXCEPTION 'cover by another couple accepted';
 EXCEPTION WHEN foreign_key_violation THEN cases := cases+1; END;
 -- Either member.
 INSERT INTO album_cover_events(album_id,event_no,couple_id,media_id,actor_id,idempotency_key,request_fingerprint) VALUES (alb,1,space,photos_a[1],b,tag||'-c3',repeat('c',64));
 cases := cases+1;

 -- ---- Removal: the uploader only; the binding stays, byte for byte.
 BEGIN
  INSERT INTO album_item_removals(album_id,media_id,couple_id,removed_by,idempotency_key,request_fingerprint) VALUES (alb,photos_a[1],space,b,tag||'-x1',repeat('d',64));
  RAISE EXCEPTION 'partner removed a photo';
 EXCEPTION WHEN SQLSTATE 'UH004' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_REMOVAL_ACTOR' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_item_removals(album_id,media_id,couple_id,removed_by,idempotency_key,request_fingerprint) VALUES (alb,photos_a[4],space,a,tag||'-x2',repeat('d',64));
  RAISE EXCEPTION 'absent item removed';
 EXCEPTION WHEN SQLSTATE 'UH004' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_REMOVAL_ITEM' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 SELECT row_to_json(x)::text INTO before_row FROM media_bindings x WHERE media_id = photos_a[1];
 INSERT INTO album_item_removals(album_id,media_id,couple_id,removed_by,idempotency_key,request_fingerprint) VALUES (alb,photos_a[1],space,a,tag||'-x3',repeat('d',64));
 IF (SELECT row_to_json(x)::text FROM media_bindings x WHERE media_id = photos_a[1]) <> before_row THEN RAISE EXCEPTION 'binding changed'; END IF;
 BEGIN
  INSERT INTO album_item_removals(album_id,media_id,couple_id,removed_by,idempotency_key,request_fingerprint) VALUES (alb,photos_a[1],space,a,tag||'-x4',repeat('d',64));
  RAISE EXCEPTION 'second removal accepted';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;
 IF usapp_media_shared_now(photos_a[1]) THEN RAISE EXCEPTION 'removed photo still shared'; END IF;
 IF NOT usapp_media_shared_now(photos_a[2]) OR NOT usapp_media_shared_now(photos_a[8]) THEN RAISE EXCEPTION 'removal touched another photo'; END IF;
 IF usapp_media_released_at(photos_a[1]) IS NOT NULL THEN RAISE EXCEPTION 'removed photo released'; END IF;
 -- A removed photo: still out of cleanup; refused as a cover; refused in any album.
 BEGIN
  INSERT INTO media_cleanup_plans(media_id,scope,reason,sweep_not_before) VALUES (photos_a[1],'everything','unattached',clock_timestamp());
  RAISE EXCEPTION 'removed album photo planned';
 EXCEPTION WHEN SQLSTATE 'UF005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_MEDIA_PLAN_BOUND' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_cover_events(album_id,event_no,couple_id,media_id,actor_id,idempotency_key,request_fingerprint) VALUES (alb,2,space,photos_a[1],a,tag||'-c4',repeat('c',64));
  RAISE EXCEPTION 'removed photo as cover';
 EXCEPTION WHEN SQLSTATE 'UH005' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_COVER_ITEM' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[1],space,'album',alb2,a);
  RAISE EXCEPTION 'removed photo rebound';
 EXCEPTION WHEN unique_violation THEN cases := cases+1; END;

 -- ---- Deferred links, each alone. The binding of an item: an unmatched album binding fails at the check.
 BEGIN
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by) VALUES (photos_a[5],space,'album',alb,a);
  SET CONSTRAINTS ALL IMMEDIATE;
  RAISE EXCEPTION 'binding without an item accepted';
 EXCEPTION WHEN SQLSTATE 'UH006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_BINDING_ITEM' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO albums(couple_id,creator_id) VALUES (space,a);
  SET CONSTRAINTS ALL IMMEDIATE;
  RAISE EXCEPTION 'album without a revision accepted';
 EXCEPTION WHEN SQLSTATE 'UH006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_CREATE_REVISION' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 BEGIN
  INSERT INTO album_photo_batches(album_id,couple_id,added_by,idempotency_key,request_fingerprint) VALUES (alb,space,a,tag||'-b9',repeat('b',64));
  SET CONSTRAINTS ALL IMMEDIATE;
  RAISE EXCEPTION 'batch without an item accepted';
 EXCEPTION WHEN SQLSTATE 'UH006' THEN GET STACKED DIAGNOSTICS got = MESSAGE_TEXT;
  IF got <> 'USAPP_ALBUM_BATCH_ITEMS' THEN RAISE EXCEPTION 'wrong refusal: %', got; END IF; cases := cases+1; END;
 SET CONSTRAINTS ALL DEFERRED;

 -- The definition: deferrable, initially deferred, insert only, album only.
 SELECT pg_get_triggerdef(t.oid) INTO def FROM pg_trigger t WHERE t.tgrelid = 'public.media_bindings'::regclass AND t.tgname = 'media_bindings_album_item'
    AND t.tgdeferrable AND t.tginitdeferred AND t.tgconstraint <> 0;
 IF def IS NULL OR def NOT LIKE '%AFTER INSERT ON public.media_bindings%' OR def NOT LIKE '%''album''%' OR def LIKE '%UPDATE%' OR def LIKE '%DELETE%' THEN RAISE EXCEPTION 'trigger definition: %', def; END IF;
 cases := cases+1;

 -- ---- Append-only, for each table.
 FOREACH stmt IN ARRAY ARRAY[
  'UPDATE albums SET creator_id = creator_id', 'DELETE FROM albums',
  'UPDATE album_texts SET name = name', 'DELETE FROM album_texts',
  'UPDATE album_revisions SET action = action', 'DELETE FROM album_revisions',
  'UPDATE album_photo_batches SET added_by = added_by', 'DELETE FROM album_photo_batches',
  'UPDATE album_items SET seq = seq', 'DELETE FROM album_items',
  'UPDATE album_item_removals SET removed_by = removed_by', 'DELETE FROM album_item_removals',
  'UPDATE album_cover_events SET media_id = media_id', 'DELETE FROM album_cover_events'
 ] LOOP
  BEGIN EXECUTE stmt; RAISE EXCEPTION 'mutation accepted: %', stmt;
  EXCEPTION WHEN insufficient_privilege THEN IF NOT app THEN RAISE; END IF; cases := cases+1;
   WHEN SQLSTATE 'UH001' THEN IF app THEN RAISE; END IF; cases := cases+1; END;
 END LOOP;
 IF has_table_privilege('usapp_push', 'public.albums', 'SELECT') OR has_table_privilege('usapp_push', 'public.album_items', 'SELECT') THEN RAISE EXCEPTION 'usapp_push can read albums'; END IF;
 cases := cases+1;

 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'ALL ALBUM LEDGER ASSERTIONS PASSED: % cases as %', cases, current_user;
END
$test$;
ROLLBACK;
