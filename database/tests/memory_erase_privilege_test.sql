-- D6 M1d: the privilege boundary of usapp_memory_erase (0039). It runs first, in both roles, before any purge case. Rollback only.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
 f pg_proc%ROWTYPE;
 eraser pg_roles%ROWTYPE;
 sig constant regprocedure := 'public.usapp_memory_erase(uuid)'::regprocedure;
 trig record;
 col record;
 cases integer := 0;
BEGIN
 IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;

 -- ---- The owner: a role with no power of its own, and no way into it.
 SELECT * INTO eraser FROM pg_roles WHERE rolname = 'usapp_memory_eraser';
 IF NOT FOUND THEN RAISE EXCEPTION 'no eraser role'; END IF;
 IF eraser.rolsuper OR eraser.rolcanlogin OR eraser.rolcreaterole OR eraser.rolcreatedb OR eraser.rolreplication OR eraser.rolbypassrls THEN
  RAISE EXCEPTION 'the eraser has a role attribute it must not have';
 END IF;
 IF EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.roleid = eraser.oid) THEN RAISE EXCEPTION 'someone is a member of the eraser'; END IF;
 IF EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.member = eraser.oid) THEN RAISE EXCEPTION 'the eraser is a member of a role'; END IF;
 cases := cases+1;

 -- ---- The validator agrees with every check below, and with 0040's refusal.
 IF EXISTS (SELECT 1 FROM public.usapp_memory_eraser_violations()) THEN
  RAISE EXCEPTION 'violations: %', (SELECT string_agg(v, ', ') FROM public.usapp_memory_eraser_violations() v);
 END IF;
 IF has_function_privilege('public', 'public.usapp_memory_eraser_violations()'::regprocedure, 'EXECUTE') OR has_function_privilege('public', 'public.usapp_memory_purge_due()'::regprocedure, 'EXECUTE') THEN
  RAISE EXCEPTION 'PUBLIC can execute a helper';
 END IF;
 IF (SELECT prosecdef FROM pg_proc WHERE oid = 'public.usapp_memory_eraser_violations()'::regprocedure) OR (SELECT prosecdef FROM pg_proc WHERE oid = 'public.usapp_memory_purge_due()'::regprocedure) THEN
  RAISE EXCEPTION 'a helper is a definer function';
 END IF;
 IF eraser.rolinherit THEN RAISE EXCEPTION 'the eraser inherits'; END IF;
 cases := cases+1;

 -- ---- The function: definer, that owner, a fixed path, no PUBLIC.
 SELECT * INTO f FROM pg_proc WHERE oid = sig;
 IF NOT f.prosecdef THEN RAISE EXCEPTION 'not SECURITY DEFINER'; END IF;
 IF f.proowner IS DISTINCT FROM eraser.oid THEN RAISE EXCEPTION 'the wrong owner'; END IF;
 IF f.proconfig IS DISTINCT FROM ARRAY['search_path=pg_catalog, pg_temp'] THEN RAISE EXCEPTION 'search_path: %', f.proconfig; END IF;
 IF f.provolatile <> 'v' THEN RAISE EXCEPTION 'not volatile'; END IF;
 IF has_function_privilege('public', sig, 'EXECUTE') THEN RAISE EXCEPTION 'PUBLIC can execute'; END IF;
 IF EXISTS (SELECT 1 FROM aclexplode(f.proacl) a WHERE a.grantee = 0) THEN RAISE EXCEPTION 'an ACL entry for PUBLIC'; END IF;
 IF NOT has_function_privilege('usapp_api', sig, 'EXECUTE') THEN RAISE EXCEPTION 'the application cannot execute'; END IF;
 IF has_function_privilege('usapp_push', sig, 'EXECUTE') THEN RAISE EXCEPTION 'usapp_push can execute'; END IF;
 cases := cases+1;
 -- Only one definer function exists, and it is this one.
 IF (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'public' AND p.prosecdef) <> 1 THEN RAISE EXCEPTION 'another definer function exists'; END IF;
 cases := cases+1;

 -- ---- What the eraser may write: exactly four columns and one record.
 FOR col IN
  SELECT c.relname::text AS relname, a.attname::text AS attname, has_column_privilege('usapp_memory_eraser', c.oid, a.attnum, 'UPDATE') AS can
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
   WHERE n.nspname = 'public' AND c.relkind IN ('r', 'p')
 LOOP
  IF col.can IS DISTINCT FROM (col.relname || '.' || col.attname IN ('memory_texts.title', 'memory_texts.description', 'memory_revisions.request_fingerprint', 'memory_edit_proposals.request_fingerprint')) THEN
   RAISE EXCEPTION 'eraser UPDATE on %.%: %', col.relname, col.attname, col.can;
  END IF;
 END LOOP;
 cases := cases+1;
 FOR col IN
  SELECT c.relname::text AS relname, has_table_privilege('usapp_memory_eraser', c.oid, 'INSERT') AS ins, has_table_privilege('usapp_memory_eraser', c.oid, 'DELETE') AS del,
         has_table_privilege('usapp_memory_eraser', c.oid, 'TRUNCATE') AS trunc, has_table_privilege('usapp_memory_eraser', c.oid, 'SELECT') AS sel
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind IN ('r', 'p')
 LOOP
  IF col.del OR col.trunc THEN RAISE EXCEPTION 'eraser can remove from %', col.relname; END IF;
  IF col.ins IS DISTINCT FROM (col.relname = 'memory_purges') THEN RAISE EXCEPTION 'eraser INSERT on %', col.relname; END IF;
  IF col.sel IS DISTINCT FROM (col.relname IN ('memory_deletions', 'memory_deletion_reversals', 'memory_purges', 'memory_texts', 'memory_revisions', 'memory_edit_proposals')) THEN
   RAISE EXCEPTION 'eraser SELECT on %', col.relname;
  END IF;
 END LOOP;
 cases := cases+1;
 -- The plan's own text and the audit log are out of its reach.
 IF has_table_privilege('usapp_memory_eraser', 'public.shared_plan_events', 'SELECT') OR has_any_column_privilege('usapp_memory_eraser', 'public.shared_plan_events', 'UPDATE')
    OR has_any_column_privilege('usapp_memory_eraser', 'public.audit_logs', 'UPDATE') OR has_table_privilege('usapp_memory_eraser', 'public.audit_logs', 'INSERT') THEN
  RAISE EXCEPTION 'the eraser reaches plan text or the audit log';
 END IF;
 cases := cases+1;

 -- ---- The application: no column of these, directly.
 FOR col IN SELECT unnest(ARRAY['memory_texts', 'memory_revisions', 'memory_edit_proposals', 'memory_purges']) AS t LOOP
  IF has_any_column_privilege('usapp_api', ('public.' || col.t)::regclass, 'UPDATE') OR has_table_privilege('usapp_api', ('public.' || col.t)::regclass, 'DELETE') THEN
   RAISE EXCEPTION 'usapp_api can change %', col.t;
  END IF;
 END LOOP;
 IF has_any_column_privilege('usapp_api', 'public.memory_purges', 'INSERT') THEN RAISE EXCEPTION 'usapp_api can write a purge record'; END IF;
 cases := cases+1;

 -- ---- The triggers are on, and the old refusals remain.
 FOR trig IN
  SELECT * FROM (VALUES
   ('memory_texts', 'memory_texts_append_only'), ('memory_texts', 'memory_texts_update_statement'), ('memory_texts', 'memory_texts_update_row'),
   ('memory_revisions', 'memory_revisions_append_only'), ('memory_revisions', 'memory_revisions_update_statement'), ('memory_revisions', 'memory_revisions_update_row'),
   ('memory_edit_proposals', 'memory_edit_proposals_append_only'), ('memory_edit_proposals', 'memory_edit_proposals_update_statement'), ('memory_edit_proposals', 'memory_edit_proposals_update_row'),
   ('memory_purges', 'memory_purges_append_only'), ('memory_purges', 'memory_purges_guard')) v(tbl, trigger_name)
 LOOP
  IF NOT EXISTS (SELECT 1 FROM pg_trigger t WHERE t.tgrelid = ('public.' || trig.tbl)::regclass AND t.tgname = trig.trigger_name AND t.tgenabled = 'O') THEN
   RAISE EXCEPTION 'trigger % is missing or off', trig.trigger_name;
  END IF;
 END LOOP;
 cases := cases+1;

 RAISE NOTICE 'ALL MEMORY ERASE PRIVILEGE ASSERTIONS PASSED: % cases', cases;
END
$test$;
ROLLBACK;
