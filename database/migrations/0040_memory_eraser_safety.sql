-- =====================================================================
-- 0040_memory_eraser_safety: D6 M1d role hardening (docs/phase3-d6-memory-deletion-design.md Section 15). 0000-0039 stay frozen.
--
-- 0039 reuses a role named usapp_memory_eraser if one exists, and does not look at it. A role belongs to the whole cluster, so that one may be someone
-- else's. This never changes it. It refuses:
--
--   usapp_memory_eraser_violations()  every way the role or the function differs from what 0039 made, as fixed labels. Empty is safe
--   usapp_memory_erase                now refuses to run at all while that list is not empty
--   usapp_memory_purge_due()          the one definition of what memory.purge would erase, shared with the enablement check
--
-- This migration refuses to commit on a violation, and so do database/init-roles.sql and API startup. database/apply.ps1 checks the role's own attributes
-- before any pending migration, which is what stops 0039 granting anything to an unsafe role in the first place.
--
-- Replaces one function body: usapp_memory_erase (0039), keeping its owner, grants and search_path, with one more refusal first.
-- No new table and no new lock. SQLSTATE: UE008 role safety.
-- =====================================================================

BEGIN;

DO $memory_eraser_safety_preflight$
BEGIN
    IF to_regclass('public.memory_purges') IS NULL OR to_regprocedure('public.usapp_memory_erase(uuid)') IS NULL THEN
        RAISE EXCEPTION '0040_memory_eraser_safety requires 0039_memory_text_erasure';
    END IF;
END
$memory_eraser_safety_preflight$;

-- Catalog reads only, as whoever calls it. Labels hold names of objects and fixed words: nothing from a row.
CREATE FUNCTION public.usapp_memory_eraser_violations() RETURNS SETOF text
LANGUAGE sql STABLE SET search_path = pg_catalog, pg_temp AS $eraser_violations$
    WITH eraser AS (SELECT r.* FROM pg_roles r WHERE r.rolname = 'usapp_memory_eraser'),
    fn AS (SELECT p.* FROM pg_proc p WHERE p.oid = to_regprocedure('public.usapp_memory_erase(uuid)')),
    relations AS (
        SELECT c.oid, c.relname::text AS relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE n.nspname = 'public' AND c.relkind IN ('r', 'p', 'v', 'm', 'f')),
    wanted_tables(relname, privilege) AS (VALUES
        ('memory_deletions', 'SELECT'), ('memory_deletion_reversals', 'SELECT'), ('memory_purges', 'SELECT'), ('memory_purges', 'INSERT'),
        ('memory_texts', 'SELECT'), ('memory_revisions', 'SELECT'), ('memory_edit_proposals', 'SELECT')),
    wanted_updates(relname, attname) AS (VALUES
        ('memory_texts', 'title'), ('memory_texts', 'description'),
        ('memory_revisions', 'request_fingerprint'), ('memory_edit_proposals', 'request_fingerprint')),
    table_privileges(privilege) AS (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER'), ('MAINTAIN'))
    -- The role.
    SELECT 'role_missing' WHERE NOT EXISTS (SELECT 1 FROM eraser)
    UNION ALL SELECT 'superuser' FROM eraser WHERE rolsuper
    UNION ALL SELECT 'login' FROM eraser WHERE rolcanlogin
    UNION ALL SELECT 'createrole' FROM eraser WHERE rolcreaterole
    UNION ALL SELECT 'createdb' FROM eraser WHERE rolcreatedb
    UNION ALL SELECT 'replication' FROM eraser WHERE rolreplication
    UNION ALL SELECT 'bypassrls' FROM eraser WHERE rolbypassrls
    UNION ALL SELECT 'inherit' FROM eraser WHERE rolinherit
    UNION ALL SELECT 'role_settings' FROM eraser WHERE rolconfig IS NOT NULL
    UNION ALL SELECT 'has_members' FROM eraser e WHERE EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.roleid = e.oid)
    UNION ALL SELECT 'has_memberships' FROM eraser e WHERE EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.member = e.oid)
    UNION ALL SELECT 'default_privileges' FROM eraser e WHERE EXISTS (SELECT 1 FROM pg_default_acl d WHERE d.defaclrole = e.oid)
    -- What it owns: that one function, in this database.
    UNION ALL SELECT 'owns_relation:' || c.relname FROM pg_class c, eraser e WHERE c.relowner = e.oid AND c.relkind NOT IN ('i', 't', 'I')
    UNION ALL SELECT 'owns_function:' || p.proname FROM pg_proc p, eraser e WHERE p.proowner = e.oid AND p.oid IS DISTINCT FROM to_regprocedure('public.usapp_memory_erase(uuid)')
    UNION ALL SELECT 'owns_schema:' || n.nspname FROM pg_namespace n, eraser e WHERE n.nspowner = e.oid
    UNION ALL SELECT 'owns_database:' || d.datname FROM pg_database d, eraser e WHERE d.datdba = e.oid
    -- What it can reach beyond its grants.
    UNION ALL SELECT 'schema_create:' || n.nspname FROM pg_namespace n, eraser e
       WHERE n.nspname !~ '^pg_' AND n.nspname <> 'information_schema' AND has_schema_privilege(e.oid, n.oid, 'CREATE')
    UNION ALL SELECT 'schema_usage' FROM eraser e WHERE NOT has_schema_privilege(e.oid, 'public', 'USAGE')
    UNION ALL SELECT 'database_create' FROM eraser e WHERE has_database_privilege(e.oid, current_database(), 'CREATE')
    -- Its exact table grants, at table and column level, both ways.
    UNION ALL SELECT 'table_privilege:' || r.relname || ':' || t.privilege
      FROM relations r CROSS JOIN table_privileges t CROSS JOIN eraser e
     WHERE has_table_privilege(e.oid, r.oid, t.privilege)
           IS DISTINCT FROM EXISTS (SELECT 1 FROM wanted_tables w WHERE w.relname = r.relname AND w.privilege = t.privilege)
    UNION ALL SELECT 'column_update:' || r.relname || '.' || a.attname
      FROM relations r JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum > 0 AND NOT a.attisdropped CROSS JOIN eraser e
     WHERE has_column_privilege(e.oid, r.oid, a.attnum, 'UPDATE')
           IS DISTINCT FROM EXISTS (SELECT 1 FROM wanted_updates w WHERE w.relname = r.relname AND w.attname = a.attname)
    UNION ALL SELECT 'column_insert:' || r.relname || '.' || a.attname
      FROM relations r JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum > 0 AND NOT a.attisdropped CROSS JOIN eraser e
     WHERE r.relname <> 'memory_purges' AND has_column_privilege(e.oid, r.oid, a.attnum, 'INSERT')
    UNION ALL SELECT 'column_select:' || r.relname || '.' || a.attname
      FROM relations r JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum > 0 AND NOT a.attisdropped CROSS JOIN eraser e
     WHERE NOT EXISTS (SELECT 1 FROM wanted_tables w WHERE w.relname = r.relname AND w.privilege = 'SELECT') AND has_column_privilege(e.oid, r.oid, a.attnum, 'SELECT')
    -- The function: its owner, form and exact grants.
    UNION ALL SELECT 'function_missing' WHERE NOT EXISTS (SELECT 1 FROM fn)
    UNION ALL SELECT 'function_owner' FROM fn f WHERE f.proowner IS DISTINCT FROM (SELECT e.oid FROM eraser e)
    UNION ALL SELECT 'function_not_definer' FROM fn f WHERE NOT f.prosecdef
    UNION ALL SELECT 'function_search_path' FROM fn f WHERE f.proconfig IS DISTINCT FROM ARRAY['search_path=pg_catalog, pg_temp']
    UNION ALL SELECT 'function_not_volatile' FROM fn f WHERE f.provolatile <> 'v'
    UNION ALL SELECT 'function_no_acl' FROM fn f WHERE f.proacl IS NULL
    UNION ALL SELECT 'function_public_execute' FROM fn f WHERE EXISTS (SELECT 1 FROM aclexplode(f.proacl) a WHERE a.grantee = 0)
    UNION ALL SELECT 'function_grant:' || COALESCE(g.rolname, 'unknown') FROM fn f CROSS JOIN LATERAL aclexplode(f.proacl) a LEFT JOIN pg_roles g ON g.oid = a.grantee
     WHERE a.grantee <> 0 AND a.grantee <> f.proowner AND COALESCE(g.rolname, '') <> 'usapp_api'
    UNION ALL SELECT 'function_grant_option:' || COALESCE(g.rolname, 'unknown') FROM fn f CROSS JOIN LATERAL aclexplode(f.proacl) a LEFT JOIN pg_roles g ON g.oid = a.grantee
     WHERE a.is_grantable AND a.grantee <> f.proowner
    UNION ALL SELECT 'function_application_cannot_execute' FROM fn f WHERE NOT has_function_privilege('usapp_api', f.oid, 'EXECUTE')
$eraser_violations$;
REVOKE ALL ON FUNCTION public.usapp_memory_eraser_violations() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.usapp_memory_eraser_violations() TO usapp_api, usapp_memory_eraser;

-- The 0039 body plus one refusal, first of all. CREATE OR REPLACE keeps the owner and grants; the SET clause must be stated again.
CREATE OR REPLACE FUNCTION public.usapp_memory_erase(p_deletion uuid) RETURNS text
LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $memory_erase$
DECLARE
    d public.memory_deletions%ROWTYPE;
BEGIN
    IF EXISTS (SELECT 1 FROM public.usapp_memory_eraser_violations()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE';
    END IF;
    SELECT * INTO d FROM public.memory_deletions WHERE id = p_deletion;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_TARGET';
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(d.memory_id::text));
    IF EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.deletion_id = p_deletion) THEN
        RETURN 'already';
    END IF;
    IF EXISTS (SELECT 1 FROM public.memory_deletion_reversals r WHERE r.deletion_id = p_deletion) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_REVERSED';
    END IF;
    IF public.usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_WINDOW';
    END IF;
    -- A text this memory uses, used too by a memory that has not been erased.
    IF EXISTS (
        WITH mine AS (
            SELECT r.text_id FROM public.memory_revisions r WHERE r.memory_id = d.memory_id
            UNION SELECT q.text_id FROM public.memory_edit_proposals q WHERE q.memory_id = d.memory_id),
        users AS (
            SELECT r.memory_id FROM public.memory_revisions r JOIN mine USING (text_id)
            UNION SELECT q.memory_id FROM public.memory_edit_proposals q JOIN mine USING (text_id))
        SELECT 1 FROM users u
         WHERE u.memory_id <> d.memory_id
           AND NOT EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.memory_id = u.memory_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_SHARED_TEXT';
    END IF;
    INSERT INTO public.memory_purges (deletion_id, memory_id, couple_id) VALUES (d.id, d.memory_id, d.couple_id);
    UPDATE public.memory_texts x SET title = '[erased]', description = ''
     WHERE x.id IN (SELECT r.text_id FROM public.memory_revisions r WHERE r.memory_id = d.memory_id
                    UNION SELECT q.text_id FROM public.memory_edit_proposals q WHERE q.memory_id = d.memory_id)
       AND (x.title <> '[erased]' OR x.description <> '');
    UPDATE public.memory_revisions r SET request_fingerprint = repeat('0', 64)
     WHERE r.memory_id = d.memory_id AND r.request_fingerprint IS NOT NULL;
    UPDATE public.memory_edit_proposals q SET request_fingerprint = repeat('0', 64)
     WHERE q.memory_id = d.memory_id;
    RETURN 'erased';
END
$memory_erase$;

-- What memory.purge would erase now, oldest first: ids, times and counts only. linked_photos = 0 marks a cycle with no cascade, as from before 0038.
CREATE FUNCTION public.usapp_memory_purge_due()
RETURNS TABLE (deletion_id uuid, memory_id uuid, deleted_at timestamptz, purge_after timestamptz, linked_photos bigint, blocked_shared_text boolean)
LANGUAGE sql STABLE SET search_path = pg_catalog, pg_temp AS $purge_due$
    SELECT d.id, d.memory_id, d.deleted_at, d.purge_after,
           (SELECT count(*) FROM public.memory_deletion_photo_cycles l WHERE l.memory_deletion_id = d.id),
           EXISTS (WITH mine AS (SELECT r.text_id FROM public.memory_revisions r WHERE r.memory_id = d.memory_id
                                 UNION SELECT q.text_id FROM public.memory_edit_proposals q WHERE q.memory_id = d.memory_id),
                        users AS (SELECT r.memory_id FROM public.memory_revisions r JOIN mine USING (text_id)
                                  UNION SELECT q.memory_id FROM public.memory_edit_proposals q JOIN mine USING (text_id))
                   SELECT 1 FROM users u WHERE u.memory_id <> d.memory_id
                      AND NOT EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.memory_id = u.memory_id))
      FROM public.memory_deletions d
     WHERE NOT public.usapp_media_undo_open(d.purge_after, clock_timestamp())
       AND NOT EXISTS (SELECT 1 FROM public.memory_deletion_reversals r WHERE r.deletion_id = d.id)
       AND NOT EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.deletion_id = d.id)
     ORDER BY d.purge_after, d.id
$purge_due$;
REVOKE ALL ON FUNCTION public.usapp_memory_purge_due() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.usapp_memory_purge_due() TO usapp_api;

-- Fail closed: nothing above is committed over an unsafe role, and the role is not changed to make this pass.
DO $memory_eraser_safety_check$
DECLARE
    found text;
BEGIN
    SELECT string_agg(v, ', ' ORDER BY v) INTO found FROM public.usapp_memory_eraser_violations() v;
    IF found IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE', DETAIL = found,
            HINT = 'A role named usapp_memory_eraser exists but is not the one this needs. Review it and resolve it by hand; nothing has changed it.';
    END IF;
END
$memory_eraser_safety_check$;

COMMIT;
