-- J3b: append-only recovery quarantine. Maintenance-only writes; application reads only.
-- No opening operation. Once an attempt commits, the media/memory domain stays closed.
-- J3c must still enforce startup and independently pinned latest checkpoints.
-- 33026: maintenance takes exclusive and never takes a domain lock; domain writers take shared.
-- UJ001 append-only, UJ002 domain closed, UJ003 invalid manifest.
BEGIN;

DO $preflight$
BEGIN
 IF NOT EXISTS (SELECT 1 FROM public.schema_migrations WHERE version = '0041_timestamp_guards') THEN
  RAISE EXCEPTION '0042_deletion_recovery requires 0041_timestamp_guards';
 END IF;
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='usapp_api' AND (rolsuper OR rolbypassrls)) THEN
  RAISE EXCEPTION '0042_deletion_recovery requires non-bypass application role';
 END IF;
END
$preflight$;

CREATE TABLE deletion_recovery_attempts (
 generation bigint PRIMARY KEY CHECK (generation > 0),
 database_id uuid NOT NULL,
 attempt_id uuid NOT NULL,
 journal_id uuid NOT NULL,
 closed_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE TABLE deletion_recovery_manifests (
 generation bigint PRIMARY KEY REFERENCES deletion_recovery_attempts(generation),
 payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object')
);
CREATE TABLE deletion_recovery_quarantine (
 generation bigint NOT NULL REFERENCES deletion_recovery_manifests(generation),
 target_kind text NOT NULL CHECK (target_kind IN ('memory', 'media')),
 target_id uuid NOT NULL,
 reason text NOT NULL CHECK (reason IN ('hidden', 'purge')),
 PRIMARY KEY (generation, target_kind, target_id, reason)
);
CREATE INDEX deletion_recovery_quarantine_target_idx ON deletion_recovery_quarantine(target_kind, target_id);
CREATE TABLE deletion_recovery_verifications (
 generation bigint PRIMARY KEY REFERENCES deletion_recovery_manifests(generation),
 head_seq bigint NOT NULL CHECK (head_seq >= 0),
 head_hash text NOT NULL CHECK (head_hash ~ '^[0-9a-f]{64}$'),
 verified_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE FUNCTION public.usapp_recovery_append_only() RETURNS trigger LANGUAGE plpgsql AS $reject$
BEGIN
 RAISE EXCEPTION USING ERRCODE = 'UJ001', MESSAGE = 'USAPP_RECOVERY_APPEND_ONLY';
END
$reject$;
CREATE FUNCTION public.usapp_recovery_owner_insert() RETURNS trigger LANGUAGE plpgsql AS $owner_insert$
BEGIN
 IF current_user IS DISTINCT FROM (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid=TG_RELID) THEN
  RAISE EXCEPTION USING ERRCODE='UJ001', MESSAGE='USAPP_RECOVERY_OWNER_ONLY';
 END IF;
 RETURN NEW;
END
$owner_insert$;
DO $triggers$
DECLARE t text;
BEGIN
 FOREACH t IN ARRAY ARRAY['deletion_recovery_attempts','deletion_recovery_manifests',
                          'deletion_recovery_quarantine','deletion_recovery_verifications'] LOOP
  EXECUTE format('CREATE TRIGGER recovery_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON public.%I FOR EACH STATEMENT EXECUTE FUNCTION public.usapp_recovery_append_only()', t);
  EXECUTE format('CREATE TRIGGER recovery_owner_insert BEFORE INSERT ON public.%I FOR EACH ROW EXECUTE FUNCTION public.usapp_recovery_owner_insert()', t);
 END LOOP;
END
$triggers$;

-- The only definer: a fixed boolean query, no input, no dynamic SQL, no writes.
-- Lets the existing eraser's invoker guards observe the hold without granting it more tables.
CREATE FUNCTION public.usapp_recovery_closed() RETURNS boolean
LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $closed$
 SELECT EXISTS (SELECT 1 FROM public.deletion_recovery_attempts)
$closed$;
REVOKE ALL ON FUNCTION public.usapp_recovery_closed() FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_recovery_closed() TO usapp_api, usapp_memory_eraser;

CREATE OR REPLACE FUNCTION usapp_media_deleted_now(p_media uuid) RETURNS boolean LANGUAGE sql STABLE AS $media$
 SELECT public.usapp_recovery_closed() OR public.usapp_media_active_deletion(p_media) IS NOT NULL
$media$;
CREATE OR REPLACE FUNCTION usapp_memory_deleted_now(p_memory uuid) RETURNS boolean LANGUAGE sql STABLE AS $memory$
 SELECT public.usapp_recovery_closed() OR public.usapp_memory_active_deletion(p_memory) IS NOT NULL
$memory$;

CREATE FUNCTION public.usapp_recovery_write_guard() RETURNS trigger LANGUAGE plpgsql AS $hold$
BEGIN
 IF current_setting('transaction_isolation') <> 'read committed' THEN
  RAISE EXCEPTION USING ERRCODE = 'UJ002', MESSAGE = 'USAPP_RECOVERY_WRITE_ISOLATION';
 END IF;
 PERFORM pg_advisory_xact_lock_shared(33026, 0);
 IF public.usapp_recovery_closed() THEN
  RAISE EXCEPTION USING ERRCODE = 'UJ002', MESSAGE = 'USAPP_RECOVERY_CLOSED';
 END IF;
 RETURN NULL;
END
$hold$;
DO $domain_holds$
DECLARE t text;
BEGIN
 FOREACH t IN ARRAY ARRAY[
  'media_objects','media_promotion_attempts','media_upload_outcomes','media_cleanup_plans','media_cleanup_passes',
  'media_bindings','media_late_objects','media_deletions','media_deletion_reversals',
  'memories','memory_texts','memory_revisions','memory_revision_photos','memory_edit_proposals','memory_proposal_photos',
  'memory_edit_decisions','memory_deletions','memory_deletion_reversals','memory_delete_requests','memory_delete_decisions',
  'memory_deletion_photo_cycles','memory_purges','albums','album_texts','album_revisions','album_photo_batches',
  'album_items','album_item_removals','album_cover_events','album_edit_proposals','album_edit_decisions',
  'shared_plan_memory_links'] LOOP
  EXECUTE format('CREATE TRIGGER recovery_hold BEFORE INSERT OR UPDATE OR DELETE OR TRUNCATE ON public.%I FOR EACH STATEMENT EXECUTE FUNCTION public.usapp_recovery_write_guard()', t);
  IF EXISTS(SELECT 1 FROM pg_class c JOIN pg_roles r ON r.oid=c.relowner WHERE c.oid=('public.'||t)::regclass AND r.rolname='usapp_api') THEN
   RAISE EXCEPTION '0042_deletion_recovery refuses application-owned domain tables';
  END IF;
  EXECUTE format('ALTER TABLE public.%I ENABLE ROW LEVEL SECURITY',t);
  -- Restrictive policy cannot be bypassed by another permissive policy. The scalar read is an init-plan, not per-row I/O.
  EXECUTE format('CREATE POLICY recovery_role_access ON public.%I AS PERMISSIVE TO usapp_api,usapp_memory_eraser USING (true) WITH CHECK (true)',t);
  EXECUTE format('CREATE POLICY recovery_visibility ON public.%I AS RESTRICTIVE TO usapp_api,usapp_memory_eraser USING (NOT (SELECT public.usapp_recovery_closed())) WITH CHECK (NOT (SELECT public.usapp_recovery_closed()))',t);
 END LOOP;
END
$domain_holds$;

CREATE FUNCTION public.usapp_recovery_fence(p_db uuid, p_attempt uuid, p_generation bigint) RETURNS boolean
LANGUAGE sql STABLE AS $fence$
 SELECT EXISTS (SELECT 1 FROM public.deletion_recovery_attempts
                 WHERE generation = p_generation AND database_id = p_db AND attempt_id = p_attempt
                   AND generation = (SELECT max(generation) FROM public.deletion_recovery_attempts))
$fence$;

CREATE FUNCTION public.usapp_recovery_lock() RETURNS void LANGUAGE plpgsql VOLATILE AS $lock$
BEGIN
 IF current_setting('transaction_isolation') <> 'read committed' THEN
  RAISE EXCEPTION USING ERRCODE='UJ002', MESSAGE='USAPP_RECOVERY_WRITE_ISOLATION';
 END IF;
 PERFORM pg_advisory_xact_lock(33026, 0);
END
$lock$;

CREATE FUNCTION public.usapp_recovery_close(p_db uuid, p_attempt uuid, p_journal uuid) RETURNS bigint
LANGUAGE plpgsql VOLATILE AS $close$
DECLARE g bigint;
BEGIN
 PERFORM public.usapp_recovery_lock();
 SELECT COALESCE(max(generation),0)+1 INTO g FROM public.deletion_recovery_attempts;
 INSERT INTO public.deletion_recovery_attempts(generation,database_id,attempt_id,journal_id)
 VALUES(g,p_db,p_attempt,p_journal);
 RETURN g;
END
$close$;

CREATE FUNCTION public.usapp_recovery_stage(p_db uuid, p_attempt uuid, p_generation bigint, p jsonb) RETURNS boolean
LANGUAGE plpgsql VOLATILE AS $stage$
DECLARE j uuid; old jsonb; a jsonb; v jsonb; seq_text text; h text;
BEGIN
 PERFORM public.usapp_recovery_lock();
 IF NOT public.usapp_recovery_fence(p_db,p_attempt,p_generation) THEN RETURN false; END IF;
 SELECT journal_id INTO j FROM public.deletion_recovery_attempts WHERE generation=p_generation;
 IF p IS NULL OR jsonb_typeof(p) <> 'object' OR
    (SELECT array_agg(k ORDER BY k) FROM jsonb_object_keys(p) k) IS DISTINCT FROM ARRAY['head','hidden','purge_barriers','unresolved']::text[] THEN
  RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_MANIFEST';
 END IF;
 IF jsonb_typeof(p->'head') IS DISTINCT FROM 'object' OR
    (SELECT array_agg(k ORDER BY k) FROM jsonb_object_keys(p->'head') k) IS DISTINCT FROM ARRAY['hash','journal_id','seq']::text[] OR
    jsonb_typeof(p#>'{head,seq}') IS DISTINCT FROM 'number' OR
    jsonb_typeof(p#>'{head,hash}') IS DISTINCT FROM 'string' OR
    jsonb_typeof(p#>'{head,journal_id}') IS DISTINCT FROM 'string' OR p#>>'{head,journal_id}' IS DISTINCT FROM j::text THEN
  RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_HEAD';
 END IF;
 seq_text := p#>>'{head,seq}'; h := p#>>'{head,hash}';
 IF COALESCE(seq_text ~ '^(0|[1-9][0-9]*)$',false) = false OR COALESCE(h ~ '^[0-9a-f]{64}$',false) = false THEN
  RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_HEAD';
 END IF;
 IF seq_text::numeric > 9223372036854775807 OR ((seq_text='0') IS DISTINCT FROM (h=repeat('0',64))) THEN
  RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_HEAD';
 END IF;
 FOREACH a IN ARRAY ARRAY[p->'hidden',p->'purge_barriers'] LOOP
  IF jsonb_typeof(a) IS DISTINCT FROM 'array' THEN RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_TARGETS'; END IF;
  FOR v IN SELECT value FROM jsonb_array_elements(a) LOOP
   IF jsonb_typeof(v) <> 'object' OR
      (SELECT array_agg(k ORDER BY k) FROM jsonb_object_keys(v) k) IS DISTINCT FROM ARRAY['id','kind']::text[] OR
      COALESCE(v->>'kind' IN ('memory','media'),false)=false OR
      COALESCE(v->>'id' ~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$',false)=false THEN
    RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_TARGET';
   END IF;
  END LOOP;
  IF (SELECT count(*) <> count(DISTINCT value) FROM jsonb_array_elements(a)) THEN
   RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_DUPLICATE';
  END IF;
 END LOOP;
 IF jsonb_typeof(p->'unresolved') IS DISTINCT FROM 'array' THEN RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_UNRESOLVED'; END IF;
 FOR v IN SELECT value FROM jsonb_array_elements(p->'unresolved') LOOP
  IF jsonb_typeof(v) <> 'string' OR COALESCE((v#>>'{}') ~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$',false)=false THEN
   RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_UNRESOLVED';
  END IF;
 END LOOP;
 IF (SELECT count(*) <> count(DISTINCT value) FROM jsonb_array_elements(p->'unresolved')) OR
    (seq_text='0' AND (jsonb_array_length(p->'hidden')+jsonb_array_length(p->'purge_barriers')+jsonb_array_length(p->'unresolved') <> 0)) THEN
  RAISE EXCEPTION USING ERRCODE='UJ003', MESSAGE='USAPP_RECOVERY_MANIFEST';
 END IF;
 SELECT payload INTO old FROM public.deletion_recovery_manifests WHERE generation=p_generation;
 IF FOUND THEN RETURN old=p; END IF;
 INSERT INTO public.deletion_recovery_manifests VALUES(p_generation,p);
 INSERT INTO public.deletion_recovery_quarantine
 SELECT p_generation,e.value->>'kind',(e.value->>'id')::uuid,'hidden' FROM jsonb_array_elements(p->'hidden') e(value)
 UNION ALL
 SELECT p_generation,e.value->>'kind',(e.value->>'id')::uuid,'purge' FROM jsonb_array_elements(p->'purge_barriers') e(value);
 RETURN true;
END
$stage$;

CREATE FUNCTION public.usapp_recovery_verify(p_db uuid, p_attempt uuid, p_generation bigint, p jsonb) RETURNS boolean
LANGUAGE plpgsql VOLATILE AS $verify$
BEGIN
 PERFORM public.usapp_recovery_lock();
 IF NOT public.usapp_recovery_fence(p_db,p_attempt,p_generation) OR NOT public.usapp_recovery_closed() THEN RETURN false; END IF;
 IF NOT EXISTS(SELECT 1 FROM public.deletion_recovery_manifests WHERE generation=p_generation AND payload=p) THEN RETURN false; END IF;
 -- Read the actual enforcement metadata back as well as the manifest. Names/count alone cannot prove a policy's expression.
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='usapp_api' AND (rolsuper OR rolbypassrls)) OR
    EXISTS(SELECT 1 FROM pg_auth_members m JOIN pg_roles r ON r.oid=m.member WHERE r.rolname='usapp_api') OR
    (SELECT count(*) FROM pg_trigger WHERE tgfoid='public.usapp_recovery_write_guard()'::regprocedure AND NOT tgisinternal) <> 32 OR
    EXISTS(
     SELECT 1 FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid
     LEFT JOIN pg_policy pol ON pol.polrelid=c.oid AND pol.polname='recovery_visibility'
     WHERE t.tgfoid='public.usapp_recovery_write_guard()'::regprocedure AND NOT t.tgisinternal AND
      (NOT c.relrowsecurity OR c.relowner=(SELECT oid FROM pg_roles WHERE rolname='usapp_api') OR t.tgenabled <> 'O' OR
       pol.oid IS NULL OR pol.polpermissive OR pol.polcmd <> '*' OR cardinality(pol.polroles) <> 2 OR
       NOT ((SELECT oid FROM pg_roles WHERE rolname='usapp_api')=ANY(pol.polroles)) OR
       NOT ((SELECT oid FROM pg_roles WHERE rolname='usapp_memory_eraser')=ANY(pol.polroles)) OR
       replace(regexp_replace(pg_get_expr(pol.polqual,pol.polrelid),'[[:space:]()]','','g'),'public.','') IS DISTINCT FROM
        'NOTSELECTusapp_recovery_closedASusapp_recovery_closed' OR
       replace(regexp_replace(pg_get_expr(pol.polwithcheck,pol.polrelid),'[[:space:]()]','','g'),'public.','') IS DISTINCT FROM
        'NOTSELECTusapp_recovery_closedASusapp_recovery_closed')
    ) THEN RETURN false; END IF;
 IF EXISTS (
  SELECT 1 FROM public.deletion_recovery_quarantine q WHERE q.generation=p_generation AND
    CASE q.target_kind WHEN 'memory' THEN NOT public.usapp_memory_deleted_now(q.target_id)
                       ELSE NOT public.usapp_media_deleted_now(q.target_id) END
 ) THEN RETURN false; END IF;
 RETURN NOT EXISTS (
  WITH expected AS (
   SELECT v->>'kind' kind,(v->>'id')::uuid id,'hidden'::text reason FROM jsonb_array_elements(p->'hidden') v
   UNION ALL SELECT v->>'kind',(v->>'id')::uuid,'purge' FROM jsonb_array_elements(p->'purge_barriers') v
  ), actual AS (SELECT target_kind kind,target_id id,reason FROM public.deletion_recovery_quarantine WHERE generation=p_generation)
  (SELECT * FROM expected EXCEPT SELECT * FROM actual) UNION ALL (SELECT * FROM actual EXCEPT SELECT * FROM expected)
 );
END
$verify$;

CREATE FUNCTION public.usapp_recovery_record(p_db uuid, p_attempt uuid, p_generation bigint, p_seq bigint, p_hash text) RETURNS boolean
LANGUAGE plpgsql VOLATILE AS $record$
DECLARE p jsonb;
BEGIN
 PERFORM public.usapp_recovery_lock();
 SELECT payload INTO p FROM public.deletion_recovery_manifests WHERE generation=p_generation;
 IF NOT FOUND OR NOT public.usapp_recovery_verify(p_db,p_attempt,p_generation,p) OR
    (p#>>'{head,seq}')::bigint IS DISTINCT FROM p_seq OR p#>>'{head,hash}' IS DISTINCT FROM p_hash THEN RETURN false; END IF;
 INSERT INTO public.deletion_recovery_verifications(generation,head_seq,head_hash) VALUES(p_generation,p_seq,p_hash) ON CONFLICT DO NOTHING;
 RETURN EXISTS(SELECT 1 FROM public.deletion_recovery_verifications WHERE generation=p_generation AND head_seq=p_seq AND head_hash=p_hash);
END
$record$;

REVOKE ALL ON public.deletion_recovery_attempts, public.deletion_recovery_manifests,
 public.deletion_recovery_quarantine, public.deletion_recovery_verifications FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON public.deletion_recovery_attempts, public.deletion_recovery_manifests,
 public.deletion_recovery_quarantine, public.deletion_recovery_verifications TO usapp_api;
REVOKE ALL ON FUNCTION public.usapp_recovery_lock(), public.usapp_recovery_close(uuid,uuid,uuid), public.usapp_recovery_fence(uuid,uuid,bigint),
 public.usapp_recovery_stage(uuid,uuid,bigint,jsonb), public.usapp_recovery_verify(uuid,uuid,bigint,jsonb),
 public.usapp_recovery_record(uuid,uuid,bigint,bigint,text) FROM PUBLIC, usapp_api, usapp_push, usapp_memory_eraser;
COMMIT;
