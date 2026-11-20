\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 db uuid := gen_random_uuid(); a uuid := gen_random_uuid(); j uuid := gen_random_uuid();
 g bigint; next_g bigint; p jsonb; bad jsonb; n integer := 0; t text;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
 FOREACH t IN ARRAY ARRAY['deletion_recovery_attempts','deletion_recovery_manifests','deletion_recovery_quarantine','deletion_recovery_verifications'] LOOP
  IF NOT has_table_privilege('usapp_api', 'public.'||t,'SELECT') OR has_table_privilege('usapp_api','public.'||t,'INSERT,UPDATE,DELETE,TRUNCATE') THEN
   RAISE EXCEPTION 'recovery table privilege';
  END IF;
  n:=n+1;
 END LOOP;
 IF has_function_privilege('usapp_api','public.usapp_recovery_close(uuid,uuid,uuid)','EXECUTE') OR
    has_function_privilege('usapp_api','public.usapp_recovery_stage(uuid,uuid,bigint,jsonb)','EXECUTE') OR
    has_function_privilege('usapp_api','public.usapp_recovery_record(uuid,uuid,bigint,bigint,text)','EXECUTE') THEN
  RAISE EXCEPTION 'recovery mutation executable by API';
 END IF;
 n:=n+1;
 IF current_user = 'usapp_api' THEN
  BEGIN PERFORM public.usapp_recovery_close(db,a,j); RAISE EXCEPTION 'API closed recovery';
  EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
  BEGIN INSERT INTO public.deletion_recovery_attempts VALUES(1,db,a,j,clock_timestamp()); RAISE EXCEPTION 'API wrote recovery';
  EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
 ELSE
  g:=public.usapp_recovery_close(db,a,j); n:=n+1;
  IF NOT public.usapp_recovery_closed() OR NOT public.usapp_memory_deleted_now(gen_random_uuid()) OR
     NOT public.usapp_media_deleted_now(gen_random_uuid()) THEN RAISE EXCEPTION 'hold absent'; END IF; n:=n+1;
  p:=jsonb_build_object('head',jsonb_build_object('journal_id',j,'seq',3,'hash',repeat('a',64)),
    'hidden',jsonb_build_array(jsonb_build_object('kind','memory','id',gen_random_uuid())),
    'purge_barriers',jsonb_build_array(jsonb_build_object('kind','media','id',gen_random_uuid())),
    'unresolved',jsonb_build_array(gen_random_uuid()));
  IF NOT public.usapp_recovery_stage(db,a,g,p) OR NOT public.usapp_recovery_stage(db,a,g,p) OR
     NOT public.usapp_recovery_verify(db,a,g,p) THEN RAISE EXCEPTION 'stage/verify'; END IF; n:=n+1;
  ALTER TABLE public.memory_texts DISABLE ROW LEVEL SECURITY;
  IF public.usapp_recovery_verify(db,a,g,p) THEN RAISE EXCEPTION 'disabled row security accepted'; END IF; n:=n+1;
  ALTER TABLE public.memory_texts ENABLE ROW LEVEL SECURITY;
  ALTER POLICY recovery_visibility ON public.memory_texts USING(true) WITH CHECK(true);
  IF public.usapp_recovery_verify(db,a,g,p) THEN RAISE EXCEPTION 'weakened policy accepted'; END IF; n:=n+1;
  ALTER POLICY recovery_visibility ON public.memory_texts USING(NOT (SELECT public.usapp_recovery_closed())) WITH CHECK(NOT (SELECT public.usapp_recovery_closed()));
  IF NOT public.usapp_recovery_verify(db,a,g,p) THEN RAISE EXCEPTION 'restored policy refused'; END IF; n:=n+1;
  IF public.usapp_recovery_stage(db,a,g,jsonb_set(p,'{head,hash}',to_jsonb(repeat('b',64)))) THEN RAISE EXCEPTION 'manifest replaced'; END IF; n:=n+1;
  IF public.usapp_recovery_record(db,a,g,4,repeat('a',64)) THEN RAISE EXCEPTION 'wrong head recorded'; END IF; n:=n+1;
  IF NOT public.usapp_recovery_record(db,a,g,3,repeat('a',64)) OR NOT public.usapp_recovery_closed() THEN RAISE EXCEPTION 'verification opened serving'; END IF; n:=n+1;
  BEGIN UPDATE public.deletion_recovery_manifests SET payload=p WHERE generation=g; RAISE EXCEPTION 'manifest mutated';
  EXCEPTION WHEN SQLSTATE 'UJ001' THEN n:=n+1; END;
  BEGIN DELETE FROM public.deletion_recovery_verifications WHERE generation=g; RAISE EXCEPTION 'verification removed';
  EXCEPTION WHEN SQLSTATE 'UJ001' THEN n:=n+1; END;
  BEGIN TRUNCATE public.deletion_recovery_quarantine; RAISE EXCEPTION 'quarantine truncated';
  EXCEPTION WHEN SQLSTATE 'UJ001' THEN n:=n+1; END;
  BEGIN INSERT INTO public.memory_texts SELECT * FROM public.memory_texts WHERE false; RAISE EXCEPTION 'domain write escaped';
  EXCEPTION WHEN SQLSTATE 'UJ002' THEN n:=n+1; END;
  -- Even the broad grant at the start of repeated InitRoles cannot let API forge recovery evidence.
  GRANT INSERT ON public.deletion_recovery_attempts TO usapp_api;
  SET LOCAL ROLE usapp_api;
  BEGIN INSERT INTO public.deletion_recovery_attempts(generation,database_id,attempt_id,journal_id) VALUES(999,db,a,j);
   RAISE EXCEPTION 'widened API forged recovery';
  EXCEPTION WHEN SQLSTATE 'UJ001' THEN n:=n+1; END;
  RESET ROLE;
  REVOKE INSERT ON public.deletion_recovery_attempts FROM usapp_api;
  next_g:=public.usapp_recovery_close(db,gen_random_uuid(),j); n:=n+1;
  IF public.usapp_recovery_stage(db,a,g,p) OR public.usapp_recovery_verify(db,a,g,p) OR
     public.usapp_recovery_record(db,a,g,3,repeat('a',64)) THEN RAISE EXCEPTION 'old fence accepted'; END IF; n:=n+1;
  SELECT attempt_id INTO a FROM public.deletion_recovery_attempts WHERE generation=next_g;
  bad:=jsonb_set(p,'{hidden}',jsonb_build_array(p->'hidden'->0,p->'hidden'->0));
  BEGIN PERFORM public.usapp_recovery_stage(db,a,next_g,bad); RAISE EXCEPTION 'duplicate accepted';
  EXCEPTION WHEN SQLSTATE 'UJ003' THEN n:=n+1; END;
  bad:=jsonb_set(p,'{head,hash}','123'::jsonb);
  BEGIN PERFORM public.usapp_recovery_stage(db,a,next_g,bad); RAISE EXCEPTION 'numeric hash accepted';
  EXCEPTION WHEN SQLSTATE 'UJ003' THEN n:=n+1; END;
  bad:=p||' {"unknown":true}'::jsonb;
  BEGIN PERFORM public.usapp_recovery_stage(db,a,next_g,bad); RAISE EXCEPTION 'extra field accepted';
  EXCEPTION WHEN SQLSTATE 'UJ003' THEN n:=n+1; END;
  IF NOT public.usapp_recovery_stage(db,a,next_g,p) THEN RAISE EXCEPTION 'second stage'; END IF;
  INSERT INTO public.deletion_recovery_quarantine VALUES(next_g,'media',gen_random_uuid(),'hidden');
  IF public.usapp_recovery_verify(db,a,next_g,p) OR public.usapp_recovery_record(db,a,next_g,3,repeat('a',64)) THEN
   RAISE EXCEPTION 'extra quarantine accepted';
  END IF; n:=n+1;
 END IF;
 RAISE NOTICE 'recovery ledger cases=%',n;
END
$test$;
ROLLBACK;
