-- Rollback-only controlled clock; maintenance on literal usapp_test, expected=baseline|fixed, mode=application|maintenance.
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('usapp.memory_clock_expected', :'expected', true);
SELECT set_config('usapp.memory_clock_role', :'mode', true);
SELECT set_config('usapp.memory_clock_base', pg_catalog.clock_timestamp()::text, true);
SELECT set_config('usapp.memory_clock_offset','0 seconds',true);
SELECT set_config('usapp.memory_clock_step','off',true);
DO $$ BEGIN
 IF current_database()<>'usapp_test' OR NOT (SELECT rolsuper FROM pg_roles WHERE rolname=current_user)
 THEN RAISE EXCEPTION 'maintenance on usapp_test required'; END IF;
 IF current_setting('usapp.memory_clock_expected') NOT IN ('baseline','fixed')
 OR current_setting('usapp.memory_clock_role') NOT IN ('application','maintenance') THEN RAISE EXCEPTION 'bad mode'; END IF;
 IF (SELECT atthasdef FROM pg_attribute WHERE attrelid='memory_edit_decisions'::regclass AND attname='decided_at')
 IS DISTINCT FROM (current_setting('usapp.memory_clock_expected')='baseline') THEN RAISE EXCEPTION 'unexpected default'; END IF;
END $$;
CREATE SCHEMA usapp_memory_clock;
GRANT USAGE ON SCHEMA usapp_memory_clock TO usapp_api;
CREATE FUNCTION usapp_memory_clock.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE AS
$$ SELECT current_setting('usapp.memory_clock_base')::timestamptz + current_setting('usapp.memory_clock_offset')::interval $$;
CREATE FUNCTION usapp_memory_clock.step() RETURNS trigger LANGUAGE plpgsql AS
$$ BEGIN
 IF current_setting('usapp.memory_clock_step')='on' THEN
  PERFORM set_config('usapp.memory_clock_offset','-1 day',true);
  PERFORM set_config('usapp.memory_clock_step','off',true);
 END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER a_memory_clock_step BEFORE INSERT ON memory_edit_decisions FOR EACH ROW EXECUTE FUNCTION usapp_memory_clock.step();
DO $$ BEGIN IF current_setting('usapp.memory_clock_expected')='baseline' THEN
 ALTER TABLE memory_edit_decisions ALTER COLUMN decided_at SET DEFAULT usapp_memory_clock.clock_timestamp();
END IF; END $$;
SET LOCAL search_path=usapp_memory_clock,pg_catalog,public;
DO $$ BEGIN IF current_setting('usapp.memory_clock_role')='application' THEN SET LOCAL ROLE usapp_api; END IF; END $$;
DO $test$
DECLARE
 a uuid:=gen_random_uuid(); b uuid:=gen_random_uuid(); c uuid:=gen_random_uuid();
 space uuid:=gen_random_uuid(); other_space uuid:=gen_random_uuid(); tag text:=replace(gen_random_uuid()::text,'-','');
 m uuid; media uuid; proposal uuid; txt uuid; base timestamptz:=current_setting('usapp.memory_clock_base')::timestamptz;
 source_time timestamptz; actual timestamptz; code text; msg text; stmt text; expected_message text;
 fixed boolean:=current_setting('usapp.memory_clock_expected')='fixed'; i integer; cases integer:=0;
BEGIN
 INSERT INTO users(id,email_canonical,email_display) VALUES
 (a,'mc-a-'||tag||'@example.com','mc-a-'||tag||'@example.com'),
 (b,'mc-b-'||tag||'@example.com','mc-b-'||tag||'@example.com'),
 (c,'mc-c-'||tag||'@example.com','mc-c-'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES(space,'clock',a),(other_space,'other-clock',c);
 INSERT INTO couple_members(couple_id,user_id,slot) VALUES(space,a,1),(space,b,2),(other_space,c,1);
 -- Each case gets a genuine complete memory/proposal; no disabled triggers or maintenance-only row edits.
 FOR i IN 1..13 LOOP
  PERFORM set_config('usapp.memory_clock_offset','0 seconds',true);
  PERFORM set_config('usapp.memory_clock_step','off',true);
  m:=gen_random_uuid(); media:=gen_random_uuid(); proposal:=gen_random_uuid();
  source_time:=CASE WHEN i IN (1,3,4,5) THEN base ELSE base-interval '2 days' END;
  INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,expires_at,idempotency_key,request_fingerprint,created_at)
  VALUES(media,space,a,'staging/'||space||'/'||media||'/upload','image/png',1000,base-interval '9 days',tag||'-media-'||i,repeat('a',64),base-interval '9 days 00:10:00');
  INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
  VALUES(media,1,space,'media/'||space||'/'||media||'/'||gen_random_uuid()||'/original',base-interval '9 days');
  INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,width,height,stored_size,etag,decided_at)
  VALUES(media,space,'ready',1,'image/png',4,4,1000,'e',base-interval '8 days');
  INSERT INTO memory_texts(couple_id,title) VALUES(space,'clock fixture') RETURNING id INTO txt;
  INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES(m,space,a,base-interval '3 days');
  INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
  VALUES(m,space,1,a,'create',DATE '1900-01-01',txt,tag||'-rev-'||i,repeat('b',64),base-interval '3 days');
  INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by,created_at) VALUES(media,space,'memory',m,a,base-interval '2 days');
  INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id) VALUES(m,1,1,media,space);
  INSERT INTO memory_edit_proposals(id,couple_id,memory_id,proposer_id,base_version,memory_date,text_id,idempotency_key,request_fingerprint,created_at)
  VALUES(proposal,space,m,b,1,DATE '1900-01-01',txt,tag||'-proposal-'||i,repeat('c',64),source_time);
  INSERT INTO memory_proposal_photos(proposal_id,position,media_id,couple_id) VALUES(proposal,1,media,space);
  SET CONSTRAINTS ALL IMMEDIATE;
  SET CONSTRAINTS ALL DEFERRED;
  -- 1: source dominates rewound clock; 2: clock rewinds after default evaluation.
  -- 3: DEFAULT; 4: explicit NULL; 5: supplied exact valid; 6/7: supplied before/future.
  -- 8/9: wrong actor (including NULL time); 10: wrong couple; 11/12: withdraw actor; 13: missing proposal.
  IF i IN (1,3,4) THEN PERFORM set_config('usapp.memory_clock_offset','-1 day',true); END IF;
  IF i=2 THEN PERFORM set_config('usapp.memory_clock_step','on',true); END IF;
  stmt:=format('INSERT INTO memory_edit_decisions(proposal_id,couple_id,decided_by,outcome,idempotency_key,request_fingerprint%s) VALUES(%L,%L,%L,%L,%L,%L%s) RETURNING decided_at',
   CASE WHEN i=1 OR i=2 THEN '' ELSE ',decided_at' END, CASE WHEN i=13 THEN gen_random_uuid() ELSE proposal END,CASE WHEN i=10 THEN other_space ELSE space END,
   CASE WHEN i IN (8,9,11) THEN b ELSE a END,CASE WHEN i IN (11,12) THEN 'withdrawn' ELSE 'rejected' END,tag||'-decision-'||i,repeat('d',64),
   CASE WHEN i IN (1,2) THEN '' WHEN i=3 THEN ',DEFAULT' WHEN i IN (4,9) THEN ',NULL'
   WHEN i=6 THEN ','||quote_literal(source_time-interval '1 microsecond')
   WHEN i=7 THEN ','||quote_literal(base+interval '1 microsecond') ELSE ','||quote_literal(base) END);
  code:=NULL;msg:=NULL;actual:=NULL;
  BEGIN EXECUTE stmt INTO actual;
  EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS code=RETURNED_SQLSTATE,msg=MESSAGE_TEXT; END;
  expected_message:=CASE WHEN i IN (10,13) THEN 'USAPP_MEMORY_DECISION_TARGET' WHEN i IN (8,9,12) THEN 'USAPP_MEMORY_DECISION_ACTOR' ELSE 'USAPP_MEMORY_DECISION_TIME' END;
  IF (i IN (1,2,3,4) AND fixed) OR i IN (5,11) THEN
   IF code IS NOT NULL OR actual IS DISTINCT FROM (CASE WHEN i=2 THEN base-interval '1 day' ELSE base END)
   THEN RAISE EXCEPTION 'assignment control % failed state=% message=%',i,code,msg; END IF;
  ELSIF NOT fixed AND i IN (4,9) THEN
   IF code IS DISTINCT FROM '23502' THEN RAISE EXCEPTION 'baseline null control % wrong state=%',i,code; END IF;
  ELSIF code IS DISTINCT FROM 'UG005' OR msg IS DISTINCT FROM expected_message THEN
   RAISE EXCEPTION 'refusal control % changed state=% message=%',i,code,msg;
  END IF;
  PERFORM set_config('usapp.memory_clock_offset','0 seconds',true);
  SET CONSTRAINTS ALL IMMEDIATE;
  SET CONSTRAINTS ALL DEFERRED;
  cases:=cases+1;
 END LOOP;
 RAISE NOTICE 'MEMORY_DECISION_CLOCK expected=% role=% cases=% passed',current_setting('usapp.memory_clock_expected'),current_setting('usapp.memory_clock_role'),cases;
END $test$;
ROLLBACK;
